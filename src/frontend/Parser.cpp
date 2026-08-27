#include "Lexer.h"  // ★ f-string 子解析需要
#include "Parser.h"
#include <filesystem>
#include "../compiler/Resolver.h"
#include "../compiler/IRBuilder.h"
#include "../compiler/IROptimizer.h"
#include "../compiler/RegisterAllocator.h"
#include "../compiler/Emitter.h"
#include "../vm/VM.h"
#include "ASTConverter.h"
#include <functional>

namespace jc {

    struct MacroScopeGuard {
        Parser* p;
        MacroScopeGuard(Parser* p) : p(p) { p->pushMacroScope(); }
        ~MacroScopeGuard() { p->popMacroScope(); }
    };

    void Parser::pushMacroScope() {
        macroEnvStack.push_back({});
    }

    void Parser::popMacroScope() {
        if (macroEnvStack.size() > 1) {
            macroEnvStack.pop_back();
        }
    }

    void Parser::defineMacro(const std::string& name, Value closure) {
        if (macroEnvStack.size() == 1) {
            VM::activeVM->setGlobal("<macro_" + name + ">", closure);
        } else {
            macroEnvStack.back()[name] = closure;
        }
    }

    Value Parser::resolveMacro(const std::string& name) {
        for (auto it = macroEnvStack.rbegin(); it != macroEnvStack.rend() - 1; ++it) {
            if (it->count(name)) return it->at(name);
        }
        auto globals = VM::activeVM->getGlobals();
        std::string internalName = "<macro_" + name + ">";
        if (globals.count(internalName)) return globals.at(internalName);
        return Value::none();
    }

    bool Parser::deleteMacro(const std::string& name) {
        for (auto it = macroEnvStack.rbegin(); it != macroEnvStack.rend() - 1; ++it) {
            if (it->count(name)) {
                it->erase(name);
                return true;
            }
        }
        auto globals = VM::activeVM->getGlobals();
        std::string internalName = "<macro_" + name + ">";
        if (globals.count(internalName)) {
            VM::activeVM->removeGlobal(internalName);
            return true;
        }
        return false;
    }

    std::unique_ptr<Expr> Parser::expression() {
        auto expr = assignment();
        if (match({ TokenType::COMMA })) {
            std::vector<std::unique_ptr<Expr>> exprs;
            exprs.push_back(std::move(expr));
            do {
                while (match({ TokenType::NEWLINE })) {}
                exprs.push_back(assignment());
            } while (match({ TokenType::COMMA }));
            return std::make_unique<SequenceExpr>(std::move(exprs));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::pipe() {
        auto expr = logicalOr();
        while (match({ TokenType::PIPE })) {
            Token op = previous();
            while (match({ TokenType::NEWLINE })) {}
            
            if (match({ TokenType::DOT })) {
                Token field(TokenType::ERROR, "");
                if (match({ TokenType::DOLLAR })) {
                    Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                    field = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                } else {
                    Token t = peek();
                    if (t.type == TokenType::IDENTIFIER || (t.type >= TokenType::IF && t.type <= TokenType::NONE_KW)) {
                        field = advance();
                        field.type = TokenType::IDENTIFIER;
                    } else {
                        throw std::runtime_error("Parser Error: Expect method name after '|> .'.");
                    }
                }

                if (match({ TokenType::LPAREN })) {
                    std::vector<std::unique_ptr<Expr>> args;
                    while (match({ TokenType::NEWLINE })) {}
                    if (!check(TokenType::RPAREN)) {
                        bool hasKwArg = false;
                        do {
                            while (match({ TokenType::NEWLINE })) {}
                            if (check(TokenType::IDENTIFIER) && current + 1 < static_cast<int>(tokens.size()) && tokens[current + 1].type == TokenType::ASSIGN) {
                                Token kwName = advance();
                                advance(); // consume '='
                                auto val = assignment();
                                args.push_back(std::make_unique<KeywordArgExpr>(kwName, std::move(val)));
                                hasKwArg = true;
                            } else {
                                if (hasKwArg) throw std::runtime_error("Parser Error: Positional argument cannot follow keyword argument.");
                                args.push_back(assignment());
                            }
                            while (match({ TokenType::NEWLINE })) {}
                        } while (match({ TokenType::COMMA }));
                    }
                    while (match({ TokenType::NEWLINE })) {}
                    if (check(TokenType::SEMICOLON)) {
                        throw std::runtime_error("Parser Error: ';' (keyword-only separator) is only allowed in function definitions, not in calls.");
                    }
                    consume(TokenType::RPAREN, "Parser Error: Expect ')' after method arguments.");

                    bool isPartial = false;
                    std::vector<Token> phParams;
                    std::vector<std::shared_ptr<Expr>> phDefaults;
                    int phCount = 0;
                    for (auto& arg : args) {
                        if (auto* var = dynamic_cast<Variable*>(arg.get())) {
                            if (var->name.lexeme == "_") {
                                isPartial = true;
                                Token phTok(TokenType::IDENTIFIER, "__ph_" + std::to_string(phCount++), var->name.line);
                                phParams.push_back(phTok);
                                phDefaults.push_back(nullptr);
                                arg = std::make_unique<Variable>(phTok);
                            }
                        }
                    }
                    std::unique_ptr<Expr> methodNode = std::make_unique<MethodCallExpr>(std::move(expr), field, std::move(args));

                    if (isPartial) {
                        std::vector<bool> phIsRef(phParams.size(), false);
                        std::vector<bool> phIsConst(phParams.size(), false);
                        expr = std::make_unique<LambdaExpr>(
                            "<partial_method>", std::move(phParams), std::move(phIsRef), std::move(phIsConst), std::move(phDefaults), "",
                            std::vector<std::shared_ptr<Expr>>(phParams.size(), nullptr), nullptr,
                            "<partial_method>", std::shared_ptr<Expr>(methodNode.release())
                        );
                    } else {
                        expr = std::move(methodNode);
                    }
                } else {
                    expr = std::make_unique<DotAccess>(std::move(expr), field);
                }
            } else {
                auto right = logicalOr();
                expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::logicalOr() {
        auto expr = logicalAnd();
        while (match({ TokenType::OR_OR })) {
            Token op = previous();
            auto right = logicalAnd();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::logicalAnd() {
        auto expr = comparison();
        while (match({ TokenType::AND_AND })) {
            Token op = previous();
            auto right = comparison();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    // ★ 新增层级 1：位或 (并集)
    std::unique_ptr<Expr> Parser::bitwiseOr() {
        auto expr = bitwiseXor();
        while (match({ TokenType::BIT_OR })) {
            Token op = previous();
            auto right = bitwiseXor();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    // ★ 新增层级 1.5：位异或
    std::unique_ptr<Expr> Parser::bitwiseXor() {
        auto expr = bitwiseAnd();
        while (match({ TokenType::BIT_XOR })) {
            Token op = previous();
            auto right = bitwiseAnd();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    // ★ 新增层级 2：位与 (交集)
    std::unique_ptr<Expr> Parser::bitwiseAnd() {
        auto expr = shift();
        while (match({ TokenType::BIT_AND })) {
            Token op = previous();
            auto right = shift();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parse() {
        if (VM::activeVM) VM::activeVM->parsingDepth++;
        try {
            std::vector<std::unique_ptr<Expr>> stmts;
            while (!isAtEnd()) {
                while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
                if (isAtEnd()) break;
                
                auto expr = expression();
                if (auto* seq = dynamic_cast<SequenceExpr*>(expr.get())) {
                    for (auto& e : seq->expressions) stmts.push_back(std::move(e));
                } else {
                    stmts.push_back(std::move(expr));
                }
                
                if (!isAtEnd() && check(TokenType::ERROR)) {
                    advance();  // 触发 "Lexer Error"（如 Unexpected character）
                }
                if (!isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                    throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
                }
                while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
            }
            if (VM::activeVM) {
                VM::activeVM->parsingDepth--;
                if (VM::activeVM->parsingDepth == 0) {
                    VM::activeVM->cleanupComptimeGlobals(0);
                }
            }
            if (stmts.empty()) return std::make_unique<Literal>("none", false, false, true);
            return std::make_unique<Block>(std::move(stmts));
        }
        catch (const std::exception& e) {
            if (VM::activeVM) {
                VM::activeVM->parsingDepth--;
                if (VM::activeVM->parsingDepth == 0) {
                    VM::activeVM->cleanupComptimeGlobals(0);
                }
            }
            std::string msg = e.what();
            if (msg.find("[") != 0) { 
                int errLine = 0;
                if (peek().type == TokenType::END_OF_FILE || peek().type == TokenType::NEWLINE) {
                    errLine = previous().line;
                }
                else {
                    errLine = peek().line > 0 ? peek().line : previous().line;
                }

                std::string fn = "Script";
                try { fn = std::filesystem::path(sourceFile).filename().string(); }
                catch (...) {}
                if (fn.empty()) fn = "Script";
                msg = "[" + fn + " : " + std::to_string(errLine) + "] " + msg;
            }
            throw std::runtime_error(msg);
        }
    }

    std::unique_ptr<Expr> Parser::ternary() {
        auto expr = pipe();  // ★

        if (match({ TokenType::QUESTION })) {
            // cond ? thenExpr : elseExpr
            // 右结合：a ? b : c ? d : e  →  a ? b : (c ? d : e)
            auto thenBranch = ternary();    // 允许嵌套 ternary
            consume(TokenType::COLON, "Parser Error: Expect ':' in ternary expression.");
            auto elseBranch = ternary();    // 右结合递归

            // ★ 直接复用 IfExpr，零 Evaluator 改动
            expr = std::make_unique<IfExpr>(
                std::move(expr),
                std::move(thenBranch),
                std::move(elseBranch));
        }

        return expr;
    }

    std::unique_ptr<Expr> Parser::assignment() {
        bool isLocal = false, isRef = false, isState = false, isConst = false;
        while (true) {
            if (match({ TokenType::LOCAL })) {
                if (isLocal) throw std::runtime_error("Parser Error: Duplicate 'local' modifier.");
                isLocal = true;
            } else if (match({ TokenType::REF })) {
                if (isRef) throw std::runtime_error("Parser Error: Duplicate 'ref' modifier.");
                isRef = true;
            } else if (match({ TokenType::STATE })) {
                if (isState) throw std::runtime_error("Parser Error: Duplicate 'state' modifier.");
                isState = true;
            } else if (match({ TokenType::CONST })) {
                if (isConst) throw std::runtime_error("Parser Error: Duplicate 'const' modifier.");
                isConst = true;
            } else {
                break;
            }
        }
        if (isLocal && (isRef || isState)) throw std::runtime_error("Parser Error: Cannot combine 'local' with 'ref' or 'state'.");
        if (isRef && isState) throw std::runtime_error("Parser Error: Cannot combine 'ref' and 'state'.");

        // ★ 特权推测解析：精准捕获带类型注解的函数定义 f(x: int) -> int = ...
        bool isFuncDef = false;
        int peekPos = current;
        if (check(TokenType::IDENTIFIER) && current + 1 < static_cast<int>(tokens.size()) && tokens[current + 1].type == TokenType::LPAREN) {
            isFuncDef = true;
            peekPos = current + 2;
        } else if (check(TokenType::DOLLAR) && current + 1 < static_cast<int>(tokens.size()) && tokens[current + 1].type == TokenType::IDENTIFIER &&
                   current + 2 < static_cast<int>(tokens.size()) && tokens[current + 2].type == TokenType::LPAREN) {
            isFuncDef = true;
            peekPos = current + 3;
        }

        if (isFuncDef) {
            int depth = 1;
            // 快速前扫，跨过括号
            while (peekPos < static_cast<int>(tokens.size()) && depth > 0) {
                if (tokens[peekPos].type == TokenType::LPAREN) depth++;
                else if (tokens[peekPos].type == TokenType::RPAREN) depth--;
                else if (tokens[peekPos].type == TokenType::LBRACE) depth++;
                else if (tokens[peekPos].type == TokenType::RBRACE) depth--;
                else if (tokens[peekPos].type == TokenType::LBRACKET) depth++;
                else if (tokens[peekPos].type == TokenType::RBRACKET) depth--;
                peekPos++;
            }

            if (depth == 0) {
                while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) peekPos++;
                // 试探是否带有 -> 返回类型
                if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::RIGHT_ARROW) {
                    peekPos++;
                    int arrowDepth = 0;
                    while (peekPos < static_cast<int>(tokens.size())) {
                        TokenType pt = tokens[peekPos].type;
                        if (pt == TokenType::LPAREN || pt == TokenType::LBRACKET || pt == TokenType::LBRACE) arrowDepth++;
                        else if (pt == TokenType::RPAREN || pt == TokenType::RBRACKET || pt == TokenType::RBRACE) arrowDepth--;
                        else if (arrowDepth == 0 && (pt == TokenType::ASSIGN || pt == TokenType::NEWLINE)) break;
                        peekPos++;
                    }
                }
                while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) peekPos++;

                // 如果结尾是等号，那它 100% 就是一个正经的函数定义！
                if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::ASSIGN) {
                    Token funcName(TokenType::ERROR, "");
                    if (match({ TokenType::DOLLAR })) {
                        Token idTok = consume(TokenType::IDENTIFIER, "");
                        funcName = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                    } else {
                        funcName = advance(); // 吞掉函数名
                    }
                    consume(TokenType::LPAREN, "");

                    std::vector<Token> params;
                    std::vector<bool> paramIsRef;
                    std::vector<bool> paramIsConst;
                    std::vector<std::shared_ptr<Expr>> defaultExprs;
                    std::vector<std::shared_ptr<Expr>> paramTypes;  // ★
                    std::string restName = "";
                    std::vector<Token> kwargParams;
                    std::vector<bool> kwargIsRef;
                    std::vector<bool> kwargIsConst;
                    std::vector<std::shared_ptr<Expr>> kwargDefaultExprs;
                    std::vector<std::shared_ptr<Expr>> kwargTypes;
                    std::string kwargsName = "";
                    bool inKwOnly = false;

                    std::vector<std::unique_ptr<Expr>> destructStmts;
                    int destructCounter = 0;

                    if (!check(TokenType::RPAREN)) {
                        while (true) {
                            while (match({ TokenType::NEWLINE })) {}

                            // ★ 分号在参数列表开头：全仅关键字（f(; a, b)）
                            if (match({ TokenType::SEMICOLON })) {
                                if (inKwOnly) throw std::runtime_error("Parser Error: Only one ';' allowed in parameter list.");
                                inKwOnly = true;
                                continue;
                            }
                            // ★ rest 后不能再有位置参数；kwargs 后不能再有任何参数
                            if (!restName.empty() && !inKwOnly) throw std::runtime_error("Parser Error: Rest parameter must be last.");
                            if (!kwargsName.empty()) throw std::runtime_error("Parser Error: kwargs parameter must be last.");

                            bool isParamRef = false;
                            bool isParamConst = false;
                            while (true) {
                                if (match({ TokenType::REF })) {
                                    if (isParamRef) throw std::runtime_error("Parser Error: Duplicate 'ref' modifier.");
                                    isParamRef = true;
                                } else if (match({ TokenType::CONST })) {
                                    if (isParamConst) throw std::runtime_error("Parser Error: Duplicate 'const' modifier.");
                                    isParamConst = true;
                                } else {
                                    break;
                                }
                            }

                            Token paramTok(TokenType::IDENTIFIER, "", 0, 0);
                            bool isRest = false;      // ...rest（分号前）或 ...kw（分号后）
                            bool isDestruct = false;
                            std::unique_ptr<Pattern> patNode = nullptr;

                            if (match({ TokenType::ELLIPSIS })) {
                                if (isParamRef) throw std::runtime_error("Parser Error: Rest/kwargs parameter cannot be ref.");
                                if (match({ TokenType::DOLLAR })) {
                                    Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                                    paramTok = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                                } else {
                                    paramTok = consume(TokenType::IDENTIFIER, "Expect parameter name.");
                                }
                                isRest = true;
                                if (inKwOnly) {
                                    if (!kwargsName.empty()) throw std::runtime_error("Parser Error: Duplicate kwargs parameter.");
                                    kwargsName = paramTok.lexeme;
                                } else {
                                    if (!restName.empty()) throw std::runtime_error("Parser Error: Duplicate rest parameter.");
                                    restName = paramTok.lexeme;
                                }
                            } else if (check(TokenType::LBRACE) || check(TokenType::LBRACKET)) {
                                if (inKwOnly) throw std::runtime_error("Parser Error: Destructured parameter cannot be keyword-only.");
                                if (isParamRef) throw std::runtime_error("Destructured parameter cannot be ref.");
                                patNode = parsePrimaryPattern();
                                std::string phName = "<param_destruct>_" + std::to_string(destructCounter++);
                                paramTok = Token(TokenType::IDENTIFIER, phName, funcName.line);
                                isDestruct = true;
                            } else {
                                if (match({ TokenType::DOLLAR })) {
                                    Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                                    paramTok = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                                } else {
                                    paramTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name.");
                                }
                            }

                            if (!isRest) {
                                if (inKwOnly) {
                                    kwargParams.push_back(paramTok);
                                    kwargIsRef.push_back(isParamRef);
                                    kwargIsConst.push_back(isParamConst);
                                } else {
                                    params.push_back(paramTok);
                                    paramIsRef.push_back(isParamRef);
                                    paramIsConst.push_back(isParamConst);
                                }
                            }

                            std::shared_ptr<Expr> pType = nullptr;
                            if (match({ TokenType::COLON })) {
                                pType = std::shared_ptr<Expr>(ternary().release());
                            }
                            if (!isRest) {
                                if (inKwOnly) kwargTypes.push_back(std::move(pType));
                                else paramTypes.push_back(std::move(pType));
                            }

                            if (match({ TokenType::ASSIGN })) {
                                if (isRest) throw std::runtime_error("Parser Error: Rest/kwargs parameter cannot have a default value.");
                                if (inKwOnly) kwargDefaultExprs.push_back(std::shared_ptr<Expr>(ternary().release()));
                                else defaultExprs.push_back(std::shared_ptr<Expr>(ternary().release()));
                            } else {
                                if (!isRest) {
                                    if (inKwOnly) kwargDefaultExprs.push_back(nullptr);
                                    else defaultExprs.push_back(nullptr);
                                }
                            }

                            if (isDestruct) {
                                auto rhs = std::make_unique<Variable>(paramTok);
                                destructStmts.push_back(std::make_unique<DestructAssign>(std::move(patNode), std::move(rhs), false, false, false, isParamConst));
                            }
                            // , 或 ; 分隔（; 进入仅关键字区，只允许一次）
                            if (match({ TokenType::COMMA })) continue;
                            if (match({ TokenType::SEMICOLON })) {
                                if (inKwOnly) throw std::runtime_error("Parser Error: Only one ';' allowed in parameter list.");
                                inKwOnly = true;
                                if (check(TokenType::RPAREN)) break;
                                continue;
                            }
                            break;
                        }
                    }
                    consume(TokenType::RPAREN, "Parser Error: Expect ')' after parameters.");

                    // ★ 解析返回类型 -> int
                    std::shared_ptr<Expr> retType = nullptr;
                    while (match({ TokenType::NEWLINE })) {}
                    if (match({ TokenType::RIGHT_ARROW })) {
                        retType = std::shared_ptr<Expr>(ternary().release());
                    }

                    while (match({ TokenType::NEWLINE })) {}
                    consume(TokenType::ASSIGN, "Parser Error: Expect '=' after function signature.");

                    // 解析函数体
                    int bodyStart = current;
                    auto rawB = check(TokenType::LBRACE) ? parseBlock() : assignment();
                    int bodyEnd = current;

                    std::string rawBodyStr = "";
                    for (int i = bodyStart; i < bodyEnd; ++i) {
                        if (tokens[i].type == TokenType::STRING) rawBodyStr += "\"" + tokens[i].lexeme + "\"";
                        else rawBodyStr += tokens[i].lexeme;
                        if (i < bodyEnd - 1) rawBodyStr += " ";
                    }

                    std::shared_ptr<Expr> finalBody;
                    if (!destructStmts.empty()) {
                        destructStmts.push_back(std::move(rawB));
                        finalBody = std::make_shared<Block>(std::move(destructStmts));
                    }
                    else {
                        finalBody = std::shared_ptr<Expr>(rawB.release());
                    }

                    auto lambda = std::make_unique<LambdaExpr>(
                        funcName.lexeme, params, paramIsRef, paramIsConst, defaultExprs, restName,
                        paramTypes, retType, rawBodyStr, std::move(finalBody),
                        kwargParams, kwargIsRef, kwargIsConst, kwargDefaultExprs, kwargTypes, kwargsName
                    );

                    return std::make_unique<Assign>(funcName, std::move(lambda), isRef, isState, isLocal, isConst);
                }
            }
        }

        // ★ 新增：直接拦截解构赋值！彻底分离 Pattern 和 Expr 的解析！
        if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
            int destructPeekPos = current;
            int depth = 0;
            while (destructPeekPos < static_cast<int>(tokens.size())) {
                TokenType t = tokens[destructPeekPos].type;
                if (t == TokenType::LBRACKET || t == TokenType::LBRACE || t == TokenType::LPAREN) depth++;
                else if (t == TokenType::RBRACKET || t == TokenType::RBRACE || t == TokenType::RPAREN) depth--;
                destructPeekPos++;
                if (depth == 0) break;
            }
            while (destructPeekPos < static_cast<int>(tokens.size()) && tokens[destructPeekPos].type == TokenType::NEWLINE) destructPeekPos++;
            if (destructPeekPos < static_cast<int>(tokens.size()) && tokens[destructPeekPos].type == TokenType::ASSIGN) {
                // 确认是解构赋值，直接解析为纯正的 Pattern！
                auto pat = parsePrimaryPattern();
                consume(TokenType::ASSIGN, "Parser Error: Expect '=' after destructuring pattern.");
                auto value = assignment();
                return std::make_unique<DestructAssign>(std::move(pat), std::move(value), isRef, isState, isLocal, isConst);
            }
        }

        auto expr = ternary();

        // ★ 类型断言：x: int = 10、d.a: string = 1、A[i]: int = 5（对任意赋值目标）
        std::shared_ptr<Expr> typeHint = nullptr;
        if ((dynamic_cast<Variable*>(expr.get()) || dynamic_cast<DotAccess*>(expr.get()) || dynamic_cast<IndexAccess*>(expr.get())) && match({ TokenType::COLON })) {
            typeHint = std::shared_ptr<Expr>(ternary().release());
        }

        // 处理复合赋值 +=, -= 等...
        if (match({ TokenType::PLUS_ASSIGN, TokenType::MINUS_ASSIGN,
                    TokenType::STAR_ASSIGN, TokenType::SLASH_ASSIGN, TokenType::TILDE_SLASH_ASSIGN,
                    TokenType::PERCENT_ASSIGN, TokenType::CARET_ASSIGN,
                    TokenType::BACKSLASH_ASSIGN,
                    TokenType::BIT_AND_ASSIGN, TokenType::BIT_OR_ASSIGN, TokenType::BIT_XOR_ASSIGN,
                    TokenType::SHIFT_LEFT_ASSIGN, TokenType::SHIFT_RIGHT_ASSIGN })) {
            if (isConst && isRef) {
                throw std::runtime_error("Parser Error: 'const ref' declaration cannot be initialized with compound assignment.");
            }
            if (isConst) throw std::runtime_error("Parser Error: 'const' cannot be applied to compound assignment.");
            Token compOp = previous();

            TokenType baseOp;
            switch (compOp.type) {
            case TokenType::PLUS_ASSIGN:    baseOp = TokenType::PLUS; break;
            case TokenType::MINUS_ASSIGN:   baseOp = TokenType::MINUS; break;
            case TokenType::STAR_ASSIGN:    baseOp = TokenType::STAR; break;
            case TokenType::SLASH_ASSIGN:   baseOp = TokenType::SLASH; break;
            case TokenType::TILDE_SLASH_ASSIGN: baseOp = TokenType::TILDE_SLASH; break;
            case TokenType::PERCENT_ASSIGN: baseOp = TokenType::PERCENT; break;
            case TokenType::CARET_ASSIGN:   baseOp = TokenType::CARET; break;
            case TokenType::BACKSLASH_ASSIGN: baseOp = TokenType::BACKSLASH; break;
            case TokenType::BIT_AND_ASSIGN: baseOp = TokenType::BIT_AND; break;
            case TokenType::BIT_OR_ASSIGN:  baseOp = TokenType::BIT_OR; break;
            case TokenType::BIT_XOR_ASSIGN: baseOp = TokenType::BIT_XOR; break;
            case TokenType::SHIFT_LEFT_ASSIGN: baseOp = TokenType::SHIFT_LEFT; break;
            case TokenType::SHIFT_RIGHT_ASSIGN: baseOp = TokenType::SHIFT_RIGHT; break;
            default: baseOp = TokenType::PLUS; break;
            }

            if (typeHint) throw std::runtime_error("Parser Error: Type assertion cannot be combined with compound assignment.");

            auto value = assignment();

            if (!dynamic_cast<Variable*>(expr.get()) && (isLocal || isRef || isState)) {
                throw std::runtime_error("Parser Error: 'local', 'ref' or 'state' can only be applied to variables.");
            }

            return std::make_unique<CompoundAssign>(std::move(expr), baseOp, std::move(value), isRef, isState, isLocal);
        }

        // ── 处理标准赋值 (=) ──
        if (match({ TokenType::ASSIGN })) {
            if (isConst && isRef) {
                throw std::runtime_error("Parser Error: 'const ref' declaration cannot be initialized with '='.");
            }
            Token equals = previous();
            auto value = assignment();  // ★ 直接读取右值即可，把上下两行记录 index 的删掉

            if (auto* dotExpr = dynamic_cast<DotAccess*>(expr.get())) {
                if (isLocal || isRef || isState || isConst) throw std::runtime_error("Parser Error: 'local', 'ref', 'state', or 'const' cannot be applied to object properties.");
                return std::make_unique<DotAssign>(std::move(dotExpr->object), std::move(dotExpr->field), std::move(value), std::move(typeHint));
            }

            if (auto* indexExpr = dynamic_cast<IndexAccess*>(expr.get())) {
                if (isLocal || isRef || isState || isConst) throw std::runtime_error("Parser Error: 'local', 'ref', 'state', or 'const' cannot be applied to array elements.");
                std::vector<std::vector<std::unique_ptr<Expr>>> chain;
                IndexAccess* currentIA = indexExpr;
                chain.push_back(std::move(currentIA->indices));
                while (auto* inner = dynamic_cast<IndexAccess*>(currentIA->object.get())) {
                    chain.push_back(std::move(inner->indices));
                    currentIA = inner;
                }
                std::reverse(chain.begin(), chain.end());
                auto* varExpr = dynamic_cast<Variable*>(currentIA->object.get());
                if (varExpr) {
                    return std::make_unique<IndexAssign>(varExpr->name, std::move(chain), std::move(value), std::move(typeHint));
                }
                else {
                    return std::make_unique<IndexAssign>(std::move(currentIA->object), std::move(chain), std::move(value), std::move(typeHint));
                }
            }

            if (auto* varExpr = dynamic_cast<Variable*>(expr.get())) {
                return std::make_unique<Assign>(varExpr->name, std::move(value), isRef, isState, isLocal, isConst, std::move(typeHint));
            }

            if (dynamic_cast<UnquoteExpr*>(expr.get())) {
                return std::make_unique<ExprAssign>(std::move(expr), std::move(value), isRef, isState, isLocal, isConst);
            }

            // ★ （旧的 Call 拦截已经被上面顶端安全取代，这里删去原来的 Call if 分支即可！）

            throw std::runtime_error("Parser Error: Invalid assignment target at '" + equals.lexeme + "'.");
        }

        if (isLocal || isRef || isState || isConst) {
            if (auto* var = dynamic_cast<Variable*>(expr.get())) {
                if (isLocal) expr = std::make_unique<LocalDecl>(var->name, isConst, std::move(typeHint));
                else if (isRef) expr = std::make_unique<RefDecl>(var->name, isConst, std::move(typeHint));
                else if (isState) expr = std::make_unique<StateDecl>(var->name, isConst, std::move(typeHint));
                else if (isConst) expr = std::make_unique<ConstDecl>(var->name);
            } else if (auto* assign = dynamic_cast<Assign*>(expr.get())) {
                assign->isLocal = isLocal;
                assign->isRef = isRef;
                assign->isState = isState;
                assign->isConst = isConst;
            } else if (auto* exprAssign = dynamic_cast<ExprAssign*>(expr.get())) {
                exprAssign->isLocal = isLocal;
                exprAssign->isRef = isRef;
                exprAssign->isState = isState;
                exprAssign->isConst = isConst;
            } else if (auto* un = dynamic_cast<Unary*>(expr.get())) {
                if (un->op.type == TokenType::ELLIPSIS) {
                    if (auto* restVar = dynamic_cast<Variable*>(un->right.get())) {
                        if (isLocal) un->right = std::make_unique<LocalDecl>(restVar->name, isConst);
                        else if (isRef) un->right = std::make_unique<RefDecl>(restVar->name, isConst);
                        else if (isState) un->right = std::make_unique<StateDecl>(restVar->name, isConst);
                        else if (isConst) un->right = std::make_unique<ConstDecl>(restVar->name);
                    } else {
                        throw std::runtime_error("Parser Error: 'local', 'ref', 'state', or 'const' must be followed by a variable or assignment.");
                    }
                } else {
                    throw std::runtime_error("Parser Error: 'local', 'ref', 'state', or 'const' must be followed by a variable or assignment.");
                }
            } else {
                throw std::runtime_error("Parser Error: 'local', 'ref', 'state', or 'const' must be followed by a variable or assignment.");
            }
        }

        // ★ 裸类型注解（无 '='/声明）已无意义，报错；求值断言用 'as'
        if (typeHint) {
            throw std::runtime_error("Parser Error: Type annotation must be followed by '=' or a declaration. Use 'as' for a value assertion (e.g. 'x as int').");
        }

        return expr;
    }

    std::unique_ptr<Expr> Parser::comparison() {
        auto expr = bitwiseOr();
        if (match({ TokenType::EQUAL, TokenType::BANG_EQUAL,
                    TokenType::LESS, TokenType::LESS_EQUAL,
                    TokenType::GREATER, TokenType::GREATER_EQUAL,
                    TokenType::IN, TokenType::IS, TokenType::SUBSET })) {
            Token op = previous();
            auto right = bitwiseOr();
            
            if (check(TokenType::EQUAL) || check(TokenType::BANG_EQUAL) ||
                check(TokenType::LESS) || check(TokenType::LESS_EQUAL) ||
                check(TokenType::GREATER) || check(TokenType::GREATER_EQUAL) ||
                check(TokenType::IN) || check(TokenType::IS) || check(TokenType::SUBSET)) {
                
                // 连续比较，为每个中间节点生成独立的临时变量
                int chainIdx = 0;
                std::string tmpName = "<chain>_" + std::to_string(current) + "_" + std::to_string(chainIdx++);
                Token tmpTok(TokenType::IDENTIFIER, tmpName, op.position, op.line);
                
                auto assign = std::make_unique<Assign>(tmpTok, std::move(right));
                auto comp = std::make_unique<Binary>(std::move(expr), op, std::move(assign));
                
                Token prevTmpTok = tmpTok;

                while (match({ TokenType::EQUAL, TokenType::BANG_EQUAL,
                               TokenType::LESS, TokenType::LESS_EQUAL,
                               TokenType::GREATER, TokenType::GREATER_EQUAL,
                               TokenType::IN, TokenType::IS, TokenType::SUBSET })) {
                    Token nextOp = previous();
                    auto nextRight = bitwiseOr();
                    
                    if (check(TokenType::EQUAL) || check(TokenType::BANG_EQUAL) ||
                        check(TokenType::LESS) || check(TokenType::LESS_EQUAL) ||
                        check(TokenType::GREATER) || check(TokenType::GREATER_EQUAL) ||
                        check(TokenType::IN) || check(TokenType::IS) || check(TokenType::SUBSET)) {
                        
                        std::string nextTmpName = "<chain>_" + std::to_string(current) + "_" + std::to_string(chainIdx++);
                        Token nextTmpTok(TokenType::IDENTIFIER, nextTmpName, nextOp.position, nextOp.line);

                        auto nextAssign = std::make_unique<Assign>(nextTmpTok, std::move(nextRight));
                        auto leftVar = std::make_unique<Variable>(prevTmpTok);
                        auto nextComp = std::make_unique<Binary>(std::move(leftVar), nextOp, std::move(nextAssign));
                        
                        Token andOp(TokenType::AND_AND, "&&", nextOp.position, nextOp.line);
                        comp = std::make_unique<Binary>(std::move(comp), andOp, std::move(nextComp));

                        prevTmpTok = nextTmpTok;
                    } else {
                        auto leftVar = std::make_unique<Variable>(prevTmpTok);
                        auto nextComp = std::make_unique<Binary>(std::move(leftVar), nextOp, std::move(nextRight));
                        
                        Token andOp(TokenType::AND_AND, "&&", nextOp.position, nextOp.line);
                        comp = std::make_unique<Binary>(std::move(comp), andOp, std::move(nextComp));
                    }
                }
                return comp;
            } else {
                return std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::shift() {
        auto expr = addition();
        while (match({ TokenType::SHIFT_LEFT, TokenType::SHIFT_RIGHT })) {
            Token op = previous();
            auto right = addition();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::addition() {
        auto expr = multiplication();
        while (match({ TokenType::PLUS, TokenType::MINUS })) {
            Token op = previous();
            auto right = multiplication();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::multiplication() {
        auto expr = unary();
        while (match({ TokenType::STAR, TokenType::SLASH, TokenType::TILDE_SLASH, TokenType::PERCENT, TokenType::BACKSLASH })) {
            Token op = previous();
            auto right = unary();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::unary() {
        if (match({ TokenType::PLUS, TokenType::MINUS, TokenType::BANG , TokenType::TILDE, TokenType::ELLIPSIS })) {
            Token op = previous();
            auto right = unary();
            return std::make_unique<Unary>(op, std::move(right));
        }
        return power();
    }

    std::unique_ptr<Expr> Parser::as() {
        auto expr = call();
        while (match({ TokenType::AS })) {
            Token asTok = previous();
            auto typeHint = std::shared_ptr<Expr>(call().release());
            std::string nameStr = "";
            if (auto* var = dynamic_cast<Variable*>(expr.get())) nameStr = var->name.lexeme;
            Token nameTok(TokenType::IDENTIFIER, nameStr, asTok.position, asTok.line);
            expr = std::make_unique<TypeAssertExpr>(std::move(nameTok), std::move(expr), std::move(typeHint));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::power() {
        auto expr = as();
        if (match({ TokenType::CARET })) {
            Token op = previous();
            auto right = unary();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::call() {
        auto expr = primary();
        while (true) {
            // 1. 点属性/方法调用访问 (必须与对象在同一行，或被 () 整体包裹)
            if (match({ TokenType::DOT })) {
                Token field(TokenType::ERROR, "");
                if (match({ TokenType::DOLLAR })) {
                    Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                    field = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                } else {
                    Token t = peek();
                    // 允许真正的标识符，或者任何关键字 (IF 到 NONE_KW) 作为属性/方法名
                    if (t.type == TokenType::IDENTIFIER || (t.type >= TokenType::IF && t.type <= TokenType::NONE_KW)) {
                        field = advance();
                        field.type = TokenType::IDENTIFIER; // 统一降级视为标识符
                    } else {
                        throw std::runtime_error("Parser Error: Expect field/method name after '.'.");
                    }
                }

                if (match({ TokenType::LPAREN })) {
                    std::vector<std::unique_ptr<Expr>> args;
                    while (match({ TokenType::NEWLINE })) {}
                    if (!check(TokenType::RPAREN)) {
                        bool hasKwArg = false;
                        do {
                            while (match({ TokenType::NEWLINE })) {}
                            if (check(TokenType::IDENTIFIER) && current + 1 < static_cast<int>(tokens.size()) && tokens[current + 1].type == TokenType::ASSIGN) {
                                Token kwName = advance();
                                advance(); // consume '='
                                auto val = assignment();
                                args.push_back(std::make_unique<KeywordArgExpr>(kwName, std::move(val)));
                                hasKwArg = true;
                            } else {
                                if (hasKwArg) throw std::runtime_error("Parser Error: Positional argument cannot follow keyword argument.");
                                args.push_back(assignment()); // ★ 降级调用，保护函数参数的逗号
                            }
                            while (match({ TokenType::NEWLINE })) {}
                        } while (match({ TokenType::COMMA }));
                    }
                    while (match({ TokenType::NEWLINE })) {}
                    if (check(TokenType::SEMICOLON)) {
                        throw std::runtime_error("Parser Error: ';' (keyword-only separator) is only allowed in function definitions, not in calls.");
                    }
                    consume(TokenType::RPAREN, "Parser Error: Expect ')' after method arguments.");

                    // ★ 魔法糖 2：对象方法的自动柯里化
                    bool isPartial = false;
                    std::vector<Token> phParams;
                    std::vector<std::shared_ptr<Expr>> phDefaults;
                    int phCount = 0;
                    for (auto& arg : args) {
                        if (auto* var = dynamic_cast<Variable*>(arg.get())) {
                            if (var->name.lexeme == "_") {
                                isPartial = true;
                                Token phTok(TokenType::IDENTIFIER, "__ph_" + std::to_string(phCount++), var->name.line);
                                phParams.push_back(phTok);
                                phDefaults.push_back(nullptr);
                                arg = std::make_unique<Variable>(phTok);
                            }
                        }
                    }
                    std::unique_ptr<Expr> methodNode = std::make_unique<MethodCallExpr>(std::move(expr), field, std::move(args));

                    if (isPartial) {
                        std::vector<bool> phIsRef(phParams.size(), false);
                        std::vector<bool> phIsConst(phParams.size(), false);
                        expr = std::make_unique<LambdaExpr>(
                            "<partial_method>", std::move(phParams), std::move(phIsRef), std::move(phIsConst), std::move(phDefaults), "",
                            std::vector<std::shared_ptr<Expr>>(phParams.size(), nullptr), nullptr,  // ★★★ 补上: 空参数类型数组，空返回类型
                            "<partial_method>", std::shared_ptr<Expr>(methodNode.release())
                        );
                    }
                    else {
                        expr = std::move(methodNode);
                    }
                }
                else {
                    expr = std::make_unique<DotAccess>(std::move(expr), field);
                }
            }
            // 2. 普通函数调用
            else if (match({ TokenType::LPAREN })) {
                std::vector<std::unique_ptr<Expr>> args;
                while (match({ TokenType::NEWLINE })) {}
                if (!check(TokenType::RPAREN)) {
                    bool hasKwArg = false;
                    do {
                        while (match({ TokenType::NEWLINE })) {}
                        if (check(TokenType::IDENTIFIER) && current + 1 < static_cast<int>(tokens.size()) && tokens[current + 1].type == TokenType::ASSIGN) {
                            Token kwName = advance();
                            advance(); // consume '='
                            auto val = assignment();
                            args.push_back(std::make_unique<KeywordArgExpr>(kwName, std::move(val)));
                            hasKwArg = true;
                        } else {
                            if (hasKwArg) throw std::runtime_error("Parser Error: Positional argument cannot follow keyword argument.");
                            args.push_back(assignment());
                        }
                        while (match({ TokenType::NEWLINE })) {}
                    } while (match({ TokenType::COMMA }));
                }
                while (match({ TokenType::NEWLINE })) {}
                if (check(TokenType::SEMICOLON)) {
                    throw std::runtime_error("Parser Error: ';' (keyword-only separator) is only allowed in function definitions, not in calls.");
                }
                consume(TokenType::RPAREN, "Parser Error: Expect ')' after arguments.");

                // ★ 魔法糖 1：普通函数的自动柯里化
                bool isPartial = false;
                std::vector<Token> phParams;
                std::vector<std::shared_ptr<Expr>> phDefaults;
                int phCount = 0;
                for (auto& arg : args) {
                    if (auto* var = dynamic_cast<Variable*>(arg.get())) {
                        if (var->name.lexeme == "_") {
                            isPartial = true;
                            Token phTok(TokenType::IDENTIFIER, "__ph_" + std::to_string(phCount++), var->name.line);
                            phParams.push_back(phTok);
                            phDefaults.push_back(nullptr);
                            arg = std::make_unique<Variable>(phTok);
                        }
                    }
                }
                std::unique_ptr<Expr> callNode;
                if (auto* varExpr = dynamic_cast<Variable*>(expr.get())) {
                    callNode = std::make_unique<Call>(varExpr->name, std::move(args));
                }
                else {
                    callNode = std::make_unique<InvokeExpr>(std::move(expr), std::move(args));
                }
                if (isPartial) {
                    std::vector<bool> phIsRef(phParams.size(), false);
                    std::vector<bool> phIsConst(phParams.size(), false);
                    expr = std::make_unique<LambdaExpr>(
                        "<partial_apply>", std::move(phParams), std::move(phIsRef), std::move(phIsConst), std::move(phDefaults), "",
                        std::vector<std::shared_ptr<Expr>>(phParams.size(), nullptr), nullptr, // ★★★ 补上: 空参数类型数组，空返回类型
                        "<partial_apply>", std::shared_ptr<Expr>(callNode.release())
                    );
                }
                else {
                    expr = std::move(callNode);
                }
            }
            // 3. 数组/矩阵索引访问
            else if (match({ TokenType::LBRACKET })) {
                std::vector<std::unique_ptr<Expr>> indices;
                auto parseSliceArg = [this]() -> std::unique_ptr<Expr> {
                    if (check(TokenType::COMMA) || check(TokenType::RBRACKET)) {
                        throw std::runtime_error("Syntax Error: Missing index expression.");
                    }
                    std::unique_ptr<Expr> st, en, sp;
                    bool isSl = false;
                    while (match({ TokenType::NEWLINE })) {}

                    if (!check(TokenType::COLON) && !check(TokenType::COMMA) && !check(TokenType::RBRACKET)) {
                        st = assignment();
                    }
                    if (match({ TokenType::COLON })) {
                        isSl = true;
                        while (match({ TokenType::NEWLINE })) {}
                        if (!check(TokenType::COLON) && !check(TokenType::COMMA) && !check(TokenType::RBRACKET)) {
                            en = assignment();
                        }
                        if (match({ TokenType::COLON })) {
                            while (match({ TokenType::NEWLINE })) {}
                            if (!check(TokenType::COMMA) && !check(TokenType::RBRACKET)) {
                                sp = assignment();
                            }
                        }
                    }
                    if (isSl) return std::make_unique<SliceExpr>(std::move(st), std::move(en), std::move(sp));
                    return st;
                    };

                if (check(TokenType::COLON)) indices.push_back(parseSliceArg());
                else indices.push_back(parseSliceArg());

                while (match({ TokenType::COMMA })) {
                    while (match({ TokenType::NEWLINE })) {}
                    if (check(TokenType::RBRACKET)) throw std::runtime_error("Syntax Error: Missing index expression after comma.");
                    indices.push_back(parseSliceArg());
                }
                while (match({ TokenType::NEWLINE })) {}
                consume(TokenType::RBRACKET, "Parser Error: Expect ']' after index.");
                expr = std::make_unique<IndexAccess>(std::move(expr), std::move(indices));
            }
            else {
                break; // 如果既不是 . 也不是 ( 也不是 [，说明后缀访问结束，跳出循环
            }
        }
        return expr;
    }

    // =================================================================
    // ★ 新增：块 { stmt1; stmt2; ... }
    // =================================================================
    std::unique_ptr<Expr> Parser::parseBlock() {
        while (match({ TokenType::NEWLINE })) {}  // ★ 跳过 { 前的换行
        consume(TokenType::LBRACE, "Parser Error: Expect '{'.");
        MacroScopeGuard guard(this);
        std::vector<std::unique_ptr<Expr>> stmts;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
            if (check(TokenType::RBRACE)) break;
            
            auto expr = expression();
            if (auto* seq = dynamic_cast<SequenceExpr*>(expr.get())) {
                for (auto& e : seq->expressions) stmts.push_back(std::move(e));
            } else {
                stmts.push_back(std::move(expr));
            }
            
            if (!check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
            }
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after block.");
        return std::make_unique<Block>(std::move(stmts));
    }

    bool Parser::isDictLiteralLookahead(int startPos) {
        int peekPos = startPos;
        while (peekPos < static_cast<int>(tokens.size()) &&
            tokens[peekPos].type == TokenType::NEWLINE) {
            peekPos++;
        }

        bool isDict = false;
        int depth = 0;
        int ternaryDepth = 0;
        int scanPos = peekPos;
        bool foundColon = false;
        bool foundSemicolon = false;
        bool foundComma = false;
        bool foundNewline = false;
        bool foundAssign = false;
        bool foundTernary = false;
        bool isPureShorthand = true;

        TokenType lastMeaningfulToken = TokenType::END_OF_FILE;
        bool newlineSinceLastMeaningful = false;
        bool missingComma = false;

        while (scanPos < static_cast<int>(tokens.size())) {
            TokenType t = tokens[scanPos].type;
            if (t == TokenType::NEWLINE) {
                foundNewline = true;
                newlineSinceLastMeaningful = true;
            } else {
                if (depth == 0) {
                    if (newlineSinceLastMeaningful && lastMeaningfulToken == TokenType::IDENTIFIER) {
                        if (t == TokenType::IDENTIFIER || t == TokenType::LOCAL || 
                            t == TokenType::REF || t == TokenType::STATE || 
                            t == TokenType::CONST || t == TokenType::ELLIPSIS) {
                            missingComma = true;
                        }
                    }
                }

                if (t == TokenType::LBRACE || t == TokenType::LBRACKET || t == TokenType::LPAREN) {
                    depth++;
                    isPureShorthand = false;
                } else if (t == TokenType::RBRACE || t == TokenType::RBRACKET || t == TokenType::RPAREN) {
                    if (depth == 0) break;
                    depth--;
                    isPureShorthand = false;
                } else if (depth == 0) {
                    if (t == TokenType::QUESTION) {
                        ternaryDepth++;
                        foundTernary = true;
                        isPureShorthand = false;
                    } else if (t == TokenType::COLON) {
                        if (ternaryDepth > 0) {
                            ternaryDepth--;
                        } else {
                            foundColon = true;
                        }
                        isPureShorthand = false;
                    } else if (t == TokenType::SEMICOLON) {
                        foundSemicolon = true;
                        isPureShorthand = false;
                    } else if (t == TokenType::COMMA) {
                        foundComma = true;
                    } else if (t == TokenType::ASSIGN || t == TokenType::PLUS_ASSIGN || t == TokenType::MINUS_ASSIGN ||
                               t == TokenType::STAR_ASSIGN || t == TokenType::SLASH_ASSIGN || t == TokenType::PERCENT_ASSIGN ||
                               t == TokenType::CARET_ASSIGN || t == TokenType::BACKSLASH_ASSIGN ||
                               t == TokenType::BIT_AND_ASSIGN || t == TokenType::BIT_OR_ASSIGN || t == TokenType::BIT_XOR_ASSIGN ||
                               t == TokenType::SHIFT_LEFT_ASSIGN || t == TokenType::SHIFT_RIGHT_ASSIGN) {
                        foundAssign = true;
                        isPureShorthand = false;
                    } else if (t != TokenType::IDENTIFIER && t != TokenType::ELLIPSIS && 
                               t != TokenType::LOCAL && t != TokenType::REF && 
                               t != TokenType::STATE && t != TokenType::CONST) {
                        // 如果出现了除了标识符、展开符、修饰符、逗号、换行之外的任何 Token，它就不是纯简写字典
                        isPureShorthand = false;
                    }
                }
                
                if (depth == 0) {
                    lastMeaningfulToken = t;
                }
                newlineSinceLastMeaningful = false;
            }
            scanPos++;
        }

        if (foundColon) {
            isDict = true;
        } else if (foundSemicolon) {
            isDict = false;
        } else if (foundAssign) {
            isDict = false;
        } else if (foundTernary) {
            isDict = false;
        } else if (isPureShorthand) {
            if (missingComma) {
                isDict = false;
            } else {
                isDict = true; // 空字典 {} 或 纯简写字典 {a, b} 或 {local a}
            }
        } else {
            isDict = false;
        }
        return isDict;
    }

    // =================================================================
// ★ 升级版：支持单行语句并智能避开字典字面量陷阱
// =================================================================
    std::unique_ptr<Expr> Parser::parseStatementOrBlock() {
        while (match({ TokenType::NEWLINE })) {}

        if (check(TokenType::LBRACE)) {
            // ★ 在控制流语句（if/while/for/try/catch）后，'{' 永远被视为代码块！
            // 不再进行字典字面量探测，避免 catch(e) { e } 被误解析为返回字典 {"e": e}
            return parseBlock();
        }

        // 走到这里说明它没有大括号，是单行语句
        // 统统包装为安全的单句 Block 以封锁词法作用域
        std::vector<std::unique_ptr<Expr>> stmts;
        {
            MacroScopeGuard guard(this);
            stmts.push_back(assignment());
        }
        return std::make_unique<Block>(std::move(stmts));
    }

    std::unique_ptr<Expr> Parser::ifExpr() {
        consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'if'.");
        auto condition = expression();
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after if condition.");

        auto thenBranch = parseStatementOrBlock();
        
        // ★ 核心修复：安全地向前探测 else，如果不是 else 则回退，避免误吞换行符导致表达式被意外拼接
        int savedPos = current;
        while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
        
        std::unique_ptr<Expr> elseBranch = nullptr;
        if (match({ TokenType::ELSE })) {
            while (match({ TokenType::NEWLINE })) {}
            if (check(TokenType::IF)) {
                advance();
                elseBranch = ifExpr();
            }
            else {
                elseBranch = parseStatementOrBlock();
            }
        } else {
            current = savedPos; // 回退，把换行符还给外部上下文
        }
        
        return std::make_unique<IfExpr>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
    }

    std::unique_ptr<Expr> Parser::whileExpr() {
        consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'while'.");
        auto condition = expression();
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after while condition.");

        auto body = parseStatementOrBlock(); // ★ 修改处

        return std::make_unique<WhileExpr>(std::move(condition), std::move(body));
    }

    // =================================================================
    // ★ 新增：for (init; cond; update) { ... }
    // =================================================================
    std::unique_ptr<Expr> Parser::forExpr() {
        consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'for'.");

        int savedPos = current;
        bool isLocal = false, isConst = false;
        while (true) {
            if (match({ TokenType::LOCAL })) {
                if (isLocal) throw std::runtime_error("Parser Error: Duplicate 'local' modifier.");
                isLocal = true;
            } else if (match({ TokenType::CONST })) {
                if (isConst) throw std::runtime_error("Parser Error: Duplicate 'const' modifier.");
                isConst = true;
            } else {
                break;
            }
        }

        // ★ 统一尝试解析 for-in 模式
        try {
            auto pat = parsePrimaryPattern();
            if (check(TokenType::IN)) {
                advance(); // consume 'in'
                auto iterable = expression();
                consume(TokenType::RPAREN, "Parser Error: Expect ')' after for-in iterable.");
                auto body = parseStatementOrBlock();
                return std::make_unique<ForInExpr>(std::move(pat), std::move(iterable), std::move(body), isLocal, isConst);
            }
        } catch (...) {
            // Fall through to normal for loop parsing
        }
        
        // 不是 for-in，回退到 '(' 之后，让 expression() 正常解析 init
        current = savedPos;

        // 传统三段式 for
        auto init = expression();
        consume(TokenType::SEMICOLON, "Parser Error: Expect ';' after for-initializer.");
        auto cond = expression();
        consume(TokenType::SEMICOLON, "Parser Error: Expect ';' after for-condition.");
        auto update = expression();
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after for-clauses.");
        auto body = parseStatementOrBlock();
        return std::make_unique<ForExpr>(std::move(init), std::move(cond), std::move(update), std::move(body));
    }

    // =================================================================
    // primary — ★ 新增 if / while / for / block / break / continue
    // =================================================================
    std::unique_ptr<Expr> Parser::primary() {
        if (match({ TokenType::ERROR })) {
            throw std::runtime_error("Lexer Error: " + previous().lexeme);
        }

        // ★ 统一拦截：任何关键字后面紧跟 = 都是误用
        auto isKeyword = [](TokenType t) {
            return t == TokenType::IF || t == TokenType::ELSE ||
                t == TokenType::WHILE || t == TokenType::FOR ||
                t == TokenType::BREAK || t == TokenType::CONTINUE ||
                t == TokenType::RETURN || t == TokenType::LOCAL ||
                t == TokenType::CONST || t == TokenType::DELETE ||
                t == TokenType::IN || t == TokenType::IS ||
                t == TokenType::THROW || t == TokenType::TRY ||  // ★
                t == TokenType::CATCH || t == TokenType::REF ||   // ★
                t == TokenType::STATE || t == TokenType::STATIC || // ★
                t == TokenType::IMPORT || t == TokenType::SWITCH ||  // ★
                t == TokenType::CASE || t == TokenType::DEFAULT ||
                t == TokenType::MATCH || t == TokenType::MACRO || t == TokenType::QUOTE ||
                t == TokenType::SUPER || t == TokenType::CLASS || t == TokenType::SELF ||
                t == TokenType::TRUE_KW || t == TokenType::FALSE_KW || t == TokenType::NONE_KW ||
                t == TokenType::NAMESPACE || t == TokenType::ENUM; // ★ 新增
            };
        if (isKeyword(peek().type) && current + 1 < static_cast<int>(tokens.size())
            && tokens[current + 1].type == TokenType::ASSIGN) {
            throw std::runtime_error("Syntax Error: '" + peek().lexeme +
                "' is a reserved keyword and cannot be used as a variable name.");
        }

        if (match({ TokenType::DOLLAR })) {
            auto right = primary();
            return std::make_unique<UnquoteExpr>(std::move(right));
        }

        if (match({ TokenType::NUMBER }))     return std::make_unique<Literal>(previous().lexeme);
        if (match({ TokenType::IMAGINARY })) {  // ★
            std::string numStr = previous().lexeme;
            numStr.pop_back();  // 去掉尾部 'i'
            return std::make_unique<Literal>(numStr, false, true);
        }
        if (match({ TokenType::FSTRING }))    return parseFString(previous().lexeme);  // ★
        if (match({ TokenType::RSTRING })) return std::make_unique<Literal>(previous().lexeme, true);  // ★
        if (match({ TokenType::STRING }))     return std::make_unique<Literal>(previous().lexeme, true);
        if (match({ TokenType::IDENTIFIER })) return std::make_unique<Variable>(previous());
        // ★ 控制流关键字
        if (match({ TokenType::TRUE_KW }))  return std::make_unique<Literal>("true", false, false, true);
        if (match({ TokenType::FALSE_KW })) return std::make_unique<Literal>("false", false, false, true);
        if (match({ TokenType::NONE_KW }))  return std::make_unique<Literal>("none", false, false, true);
        if (match({ TokenType::SUPER }))    return std::make_unique<SuperExpr>();
        if (match({ TokenType::SELF }))     return std::make_unique<SelfExpr>();
        if (match({ TokenType::CLASS })) {
            if (check(TokenType::LBRACE) || check(TokenType::IDENTIFIER) || check(TokenType::DOLLAR)) {
                return classDefExpr();
            }
            return std::make_unique<ContextKeywordExpr>(ContextKeywordExpr::Kind::Class, previous());
        }
        if (match({ TokenType::NAMESPACE })) {
            if (check(TokenType::LBRACE) || check(TokenType::IDENTIFIER) || check(TokenType::DOLLAR)) {
                return namespaceExpr();
            }
            return std::make_unique<ContextKeywordExpr>(ContextKeywordExpr::Kind::Namespace, previous());
        }
        if (match({ TokenType::ENUM })) {
            if (check(TokenType::LBRACE) || check(TokenType::IDENTIFIER) || check(TokenType::DOLLAR)) {
                return enumExpr();
            }
            throw std::runtime_error("Syntax Error: 'enum' cannot be self-referenced; enum members are compile-time constants.");
        }
        if (match({ TokenType::IF }))       return ifExpr();
        if (match({ TokenType::WHILE }))    return whileExpr();
        if (match({ TokenType::FOR }))      return forExpr();
        if (match({ TokenType::BREAK }))    return std::make_unique<BreakExpr>(previous());
        if (match({ TokenType::CONTINUE })) return std::make_unique<ContinueExpr>(previous());
        if (match({ TokenType::RETURN })) {
            Token retTok = previous();
            std::unique_ptr<Expr> value = nullptr;
            if (!check(TokenType::RBRACE) &&
                !check(TokenType::SEMICOLON) &&
                !check(TokenType::NEWLINE) &&      // ★ 新增
                !check(TokenType::END_OF_FILE)) {
                value = assignment();  // ★ 降级：防止逗号被误吞
            }
            return std::make_unique<ReturnExpr>(retTok, std::move(value));
        }
        if (match({ TokenType::THROW })) {
            Token throwTok = previous();
            auto value = assignment();  // ★ 降级：防止逗号被误吞
            return std::make_unique<ThrowExpr>(throwTok, std::move(value));
        }
        if (match({ TokenType::TRY })) {
            auto tryBody = parseStatementOrBlock();
            while (match({ TokenType::NEWLINE })) {}  // ★ 跳过 } 和 catch 之间的换行
            consume(TokenType::CATCH, "Parser Error: Expect 'catch' after try block.");
            consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'catch'.");
            auto catchPattern = parsePrimaryPattern();
            consume(TokenType::RPAREN, "Parser Error: Expect ')' after catch pattern.");
            auto catchBody = parseStatementOrBlock();
            return std::make_unique<TryCatchExpr>(std::move(tryBody), std::move(catchPattern), std::move(catchBody));
        }
        if (match({ TokenType::IMPORT })) {
            bool isComptime = match({ TokenType::AT });
            
            if (check(TokenType::IDENTIFIER)) {
                Token nameTok = advance();
                if (isComptime) {
                    VM::activeVM->execCompileTimeImport(nameTok.lexeme);
                }
                auto pathExpr = std::make_unique<Literal>(nameTok.lexeme, true);
                auto importExpr = std::make_unique<ImportExpr>(std::move(pathExpr));
                return std::make_unique<Assign>(nameTok, std::move(importExpr));
            } else {
                auto path = assignment();  // ★ 降级：防止逗号被误吞
                if (isComptime) {
                    if (auto* lit = dynamic_cast<Literal*>(path.get())) {
                        VM::activeVM->execCompileTimeImport(lit->value);
                    } else {
                        throw std::runtime_error("Parser Error: Compile-time import (@) requires a static string or identifier.");
                    }
                }
                return std::make_unique<ImportExpr>(std::move(path));
            }
        }
        if (match({ TokenType::SWITCH })) return switchExpr();
        if (match({ TokenType::MATCH })) return matchExpr();
        if (match({ TokenType::MACRO })) return macroDefExpr(false);
        if (match({ TokenType::SYNTAX })) return macroDefExpr(true);
        if (match({ TokenType::QUOTE })) return quoteExpr();
        if (match({ TokenType::DEFER })) return deferExpr();
        if (match({ TokenType::DELETE })) {
            if (match({ TokenType::AT })) {
                Token macroName = consume(TokenType::IDENTIFIER, "Parser Error: Expect macro name after '@'.");
                if (!deleteMacro(macroName.lexeme)) {
                    throw std::runtime_error("Parser Error: Macro '" + macroName.lexeme + "' not found.");
                }
                return std::make_unique<Literal>("none", false, false, true);
            }
            std::vector<Token> names;
            auto parseDelName = [&]() {
                if (match({ TokenType::DOLLAR })) {
                    Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                    return Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                }
                return consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name after 'delete' or ','.");
            };
            names.push_back(parseDelName());
            while (check(TokenType::COMMA)) {
                int peekPos = current + 1;
                while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) peekPos++;
                if (peekPos < static_cast<int>(tokens.size()) && (tokens[peekPos].type == TokenType::IDENTIFIER || tokens[peekPos].type == TokenType::DOLLAR)) {
                    advance(); // 吞掉逗号
                    while (match({ TokenType::NEWLINE })) {}
                    names.push_back(parseDelName());
                } else {
                    break; // 智能探测：后面不是变量名，把逗号留给外层
                }
            }
            return std::make_unique<DeleteExpr>(std::move(names));
        }

        // ★ 裸块 { ... } 或字典字面量 { key: value, ... }
        if (check(TokenType::LBRACE)) {
            if (isDictLiteralLookahead(current + 1)) {
                return parseDictLiteral();
            }
            return parseBlock();
        }

        if (match({ TokenType::LPAREN })) {
            // ★ 推测性 lambda 解析：(params) => body  [兼容了类型签名侦测]
            int savedPos = current;
            bool isLambda = false;

            int peekPos = current;
            int depth = 1;
            while (peekPos < static_cast<int>(tokens.size()) && depth > 0) {
                TokenType tt = tokens[peekPos].type;
                if (tt == TokenType::LPAREN || tt == TokenType::LBRACKET || tt == TokenType::LBRACE) depth++;
                else if (tt == TokenType::RPAREN || tt == TokenType::RBRACKET || tt == TokenType::RBRACE) depth--;
                peekPos++;
            }

            if (depth == 0) {
                while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) peekPos++;

                // ★ 嗅探返回类型 ->
                if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::RIGHT_ARROW) {
                    peekPos++;
                    int arrowDepth = 0;
                    while (peekPos < static_cast<int>(tokens.size())) {
                        TokenType pt = tokens[peekPos].type;
                        if (pt == TokenType::LPAREN || pt == TokenType::LBRACKET || pt == TokenType::LBRACE) arrowDepth++;
                        else if (pt == TokenType::RPAREN || pt == TokenType::RBRACKET || pt == TokenType::RBRACE) arrowDepth--;
                        else if (arrowDepth == 0 && (pt == TokenType::ARROW || pt == TokenType::NEWLINE)) break;
                        peekPos++;
                    }
                }
                while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) peekPos++;

                if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::ARROW) {
                    isLambda = true;
                }
            }

            if (isLambda) {
                current = savedPos; // 回退，开启真正无坚不摧的 Lambda 解析！
                std::vector<Token> lambdaParams;
                std::vector<bool> lambdaParamIsRef;
                std::vector<bool> lambdaParamIsConst;
                std::vector<std::shared_ptr<Expr>> lambdaDefaults;
                std::vector<std::shared_ptr<Expr>> paramTypes; // ★
                std::string restName = "";
                std::vector<Token> kwargParams;
                std::vector<bool> kwargIsRef;
                std::vector<bool> kwargIsConst;
                std::vector<std::shared_ptr<Expr>> kwargDefaultExprs;
                std::vector<std::shared_ptr<Expr>> kwargTypes;
                std::string kwargsName = "";
                bool inKwOnly = false;

                std::vector<std::unique_ptr<Expr>> destructStmts;
                int destructCounter = 0;

                if (!check(TokenType::RPAREN)) {
                    while (true) {
                        // ★ 分号在参数列表开头：全仅关键字（f(; a, b)）
                        if (match({ TokenType::SEMICOLON })) {
                            if (inKwOnly) throw std::runtime_error("Parser Error: Only one ';' allowed in parameter list.");
                            inKwOnly = true;
                            continue;
                        }
                        // ★ rest 后不能再有位置参数；kwargs 后不能再有任何参数
                        if (!restName.empty() && !inKwOnly) throw std::runtime_error("Parser Error: Rest parameter must be last.");
                        if (!kwargsName.empty()) throw std::runtime_error("Parser Error: kwargs parameter must be last.");
                        bool isRef = false;
                        bool isConst = false;
                        while (true) {
                            if (match({ TokenType::REF })) {
                                if (isRef) throw std::runtime_error("Parser Error: Duplicate 'ref' modifier.");
                                isRef = true;
                            } else if (match({ TokenType::CONST })) {
                                if (isConst) throw std::runtime_error("Parser Error: Duplicate 'const' modifier.");
                                isConst = true;
                            } else {
                                break;
                            }
                        }
                        
                        Token paramTok(TokenType::IDENTIFIER, "", 0, 0);
                        bool isRest = false;
                        bool isDestruct = false;
                        std::unique_ptr<Pattern> patNode = nullptr;

                        if (match({ TokenType::ELLIPSIS })) {
                            if (isRef) throw std::runtime_error("Parser Error: Rest/kwargs parameter cannot be ref.");
                            if (match({ TokenType::DOLLAR })) {
                                Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                                paramTok = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                            } else {
                                paramTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name after '...'.");
                            }
                            isRest = true;
                            if (inKwOnly) {
                                if (!kwargsName.empty()) throw std::runtime_error("Parser Error: Duplicate kwargs parameter.");
                                kwargsName = paramTok.lexeme;
                            } else {
                                if (!restName.empty()) throw std::runtime_error("Parser Error: Duplicate rest parameter.");
                                restName = paramTok.lexeme;
                            }
                        } else if (check(TokenType::LBRACE) || check(TokenType::LBRACKET)) {
                            if (inKwOnly) throw std::runtime_error("Parser Error: Destructured parameter cannot be keyword-only.");
                            if (isRef) throw std::runtime_error("Destructured parameter cannot be ref.");
                            patNode = parsePrimaryPattern();
                            std::string phName = "<param_destruct>_" + std::to_string(destructCounter++);
                            paramTok = Token(TokenType::IDENTIFIER, phName, previous().line);
                            isDestruct = true;
                        } else {
                            if (match({ TokenType::DOLLAR })) {
                                Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                                paramTok = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
                            } else {
                                paramTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name.");
                            }
                        }

                        if (!isRest) {
                            if (inKwOnly) {
                                kwargParams.push_back(paramTok);
                                kwargIsRef.push_back(isRef);
                                kwargIsConst.push_back(isConst);
                            } else {
                                lambdaParams.push_back(paramTok);
                                lambdaParamIsRef.push_back(isRef);
                                lambdaParamIsConst.push_back(isConst);
                            }
                        }

                        std::shared_ptr<Expr> pType = nullptr;
                        if (match({ TokenType::COLON })) {
                            pType = std::shared_ptr<Expr>(ternary().release());
                        }
                        if (!isRest) {
                            if (inKwOnly) kwargTypes.push_back(std::move(pType));
                            else paramTypes.push_back(std::move(pType));
                        }

                        if (match({ TokenType::ASSIGN })) {
                            if (isRest) throw std::runtime_error("Parser Error: Rest/kwargs parameter cannot have a default value.");
                            auto defExpr = ternary();
                            if (inKwOnly) kwargDefaultExprs.push_back(std::shared_ptr<Expr>(defExpr.release()));
                            else lambdaDefaults.push_back(std::shared_ptr<Expr>(defExpr.release()));
                        } else {
                            if (!isRest) {
                                if (inKwOnly) kwargDefaultExprs.push_back(nullptr);
                                else lambdaDefaults.push_back(nullptr);
                            }
                        }

                        if (isDestruct) {
                            auto rhs = std::make_unique<Variable>(paramTok);
                            destructStmts.push_back(std::make_unique<DestructAssign>(std::move(patNode), std::move(rhs), false, false, false, isConst));
                        }
                        // , 或 ; 分隔（; 进入仅关键字区，只允许一次）
                        if (match({ TokenType::COMMA })) continue;
                        if (match({ TokenType::SEMICOLON })) {
                            if (inKwOnly) throw std::runtime_error("Parser Error: Only one ';' allowed in parameter list.");
                            inKwOnly = true;
                            if (check(TokenType::RPAREN)) break;
                            continue;
                        }
                        break;
                    }
                }

                consume(TokenType::RPAREN, "Parser Error: Expect ')' after lambda parameters.");

                // ★ 捕获返回值
                std::shared_ptr<Expr> retType = nullptr;
                while (match({ TokenType::NEWLINE })) {}
                if (match({ TokenType::RIGHT_ARROW })) {
                    retType = std::shared_ptr<Expr>(ternary().release());
                }
                while (match({ TokenType::NEWLINE })) {}

                consume(TokenType::ARROW, "Parser Error: Expect '=>' for lambda.");

                int bodyStart = current;
                auto body = check(TokenType::LBRACE) ? parseBlock() : assignment();
                int bodyEnd = current;

                std::string rawBody;
                for (int ii = bodyStart; ii < bodyEnd; ++ii) {
                    if (tokens[ii].type == TokenType::STRING) rawBody += "\"" + tokens[ii].lexeme + "\"";
                    else rawBody += tokens[ii].lexeme;
                    if (ii < bodyEnd - 1) rawBody += " ";
                }

                std::shared_ptr<Expr> finalBody;
                if (!destructStmts.empty()) {
                    destructStmts.push_back(std::move(body));
                    finalBody = std::make_shared<Block>(std::move(destructStmts));
                } else {
                    finalBody = std::shared_ptr<Expr>(body.release());
                }

                return std::make_unique<LambdaExpr>(
                    "<lambda>",
                    std::move(lambdaParams),
                    std::move(lambdaParamIsRef),
                    std::move(lambdaParamIsConst),
                    std::move(lambdaDefaults),
                    restName,
                    paramTypes, retType,  // ★
                    rawBody,
                    std::move(finalBody),
                    kwargParams, kwargIsRef, kwargIsConst, kwargDefaultExprs, kwargTypes, kwargsName);
            }
            else {
                current = savedPos;
                while (match({ TokenType::NEWLINE })) {}
                auto expr = expression();
                while (match({ TokenType::NEWLINE })) {}
                consume(TokenType::RPAREN, "Parser Error: Expect ')' after expression.");
                return std::make_unique<GroupingExpr>(std::move(expr));
            }
        }

        // ★ 矩阵 [...] 或列表推导式 [expr for x in ...]
        bool forceList = false;
        if (match({ TokenType::AT })) {
            if (check(TokenType::LBRACE)) {
                return parseSetLiteral();
            }
            if (check(TokenType::IDENTIFIER)) {
                Token macroName = advance();
            
                Value macroVal = resolveMacro(macroName.lexeme);
                if (macroVal.isNone() || !macroVal.isFunctionClosure()) {
                    throw std::runtime_error("Parser Error: Macro '" + macroName.lexeme + "' is not defined or not a function.");
                }
            
                ObjClosure* macroFn = macroVal.asFunction();
                
                if (macroFn->isTokenMacro) {
                    consume(TokenType::LBRACE, "Parser Error: Expect '{' after token macro name.");
                    std::vector<Token> macroTokens;
                    int depth = 1;
                    while (!isAtEnd() && depth > 0) {
                        Token t = tokens[current++];
                        if (t.type == TokenType::LBRACE) depth++;
                        else if (t.type == TokenType::RBRACE) depth--;
                        
                        if (depth > 0) {
                            macroTokens.push_back(t);
                        }
                    }
                    if (depth != 0) throw std::runtime_error("Parser Error: Unterminated '{' in token macro.");
                    
                    std::vector<Value> callArgs;
                    ObjList* tokenList = GcHeap::get().allocate<ObjList>();
                    GcObjGuard tlGuard(tokenList);
                    for (const auto& t : macroTokens) {
                        tokenList->vec.push_back(VM::activeVM->makeTokenInstance(t));
                    }
                    
                    Value streamClassVal = VM::activeVM->getBuiltinValue("TokenStream");
                    ObjInstance* streamInst = GcHeap::get().allocate<ObjInstance>();
                    streamInst->classDef = static_cast<ObjClass*>(streamClassVal.asObj());
                    streamInst->properties["_tokens"] = {Value(tokenList), false, false};
                    streamInst->properties["cursor"] = {Value::fromInt32(0), false, false};

                    callArgs.push_back(Value(streamInst));
                    
                    Value resultVal;
                    try {
                        resultVal = VM::activeVM->callVMFunction(macroFn->compiledFnIndex, callArgs, macroFn);
                    } catch (const ValueException& ex) {
                        std::string errStr = ex.val.isString() ? ex.val.asString() : ex.val.toRepr();
                        if (ex.val.isInstance() && ex.val.asInstance()->classDef->name == "Exception") {
                            auto inst = ex.val.asInstance();
                            auto itMsg = inst->properties.find("message");
                            if (itMsg != inst->properties.end()) {
                                Value mVal = itMsg->second.val;
                                errStr = mVal.isString() ? mVal.asString() : mVal.toRepr();
                            }
                        }
                        throw std::runtime_error("Token Macro Execution Error: " + errStr);
                    }
                    GcValueGuard resultGuard(resultVal);
                    
                    auto expander = [this](const std::string& mName, std::vector<std::unique_ptr<Expr>>& mArgs) -> std::unique_ptr<Expr> {
                        return this->expandMacro(mName, mArgs);
                    };
                    
                    auto expandedAst = JC2_to_AST(resultVal, expander, 0);
                    if (!expandedAst) throw std::runtime_error("Parser Error: Token Macro '" + macroName.lexeme + "' did not return a valid ASTNode.");
                    
                    return expandedAst;
                }
                
                std::vector<std::unique_ptr<Expr>> args;
                
                if (match({ TokenType::LPAREN })) {
                    if (!check(TokenType::RPAREN)) {
                        bool hasKwArg = false;
                        do {
                            if (check(TokenType::IDENTIFIER) && current + 1 < static_cast<int>(tokens.size()) && tokens[current + 1].type == TokenType::ASSIGN) {
                                Token kwName = advance();
                                advance(); // consume '='
                                auto val = assignment();
                                args.push_back(std::make_unique<KeywordArgExpr>(kwName, std::move(val)));
                                hasKwArg = true;
                            } else {
                                if (hasKwArg) throw std::runtime_error("Parser Error: Positional argument cannot follow keyword argument.");
                                args.push_back(assignment());
                            }
                        } while (match({ TokenType::COMMA }));
                    }
                    if (check(TokenType::SEMICOLON)) {
                        throw std::runtime_error("Parser Error: ';' (keyword-only separator) is only allowed in function definitions, not in calls.");
                    }
                    consume(TokenType::RPAREN, "Parser Error: Expect ')' after macro arguments.");
                }
                
                bool shouldProbe = false;
                if (!macroFn->restName.empty()) {
                    shouldProbe = true;
                } else {
                    int nArgs = macroFn->maxArgs();
                    int provided = static_cast<int>(args.size());
                    if (provided == nArgs - 1) {
                        shouldProbe = true;
                    } else if (provided == nArgs) {
                        shouldProbe = false;
                    } else if (provided < nArgs - 1) {
                        throw std::runtime_error("Parser Error: Macro '" + macroName.lexeme + "' expects " + std::to_string(nArgs) + " arguments, but only " + std::to_string(provided) + " were provided.");
                    } else {
                        shouldProbe = false;
                    }
                }
                
                if (shouldProbe) {
                    int peekPos = current;
                    while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) {
                        peekPos++;
                    }
                    
                    if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::LBRACE) {
                        args.push_back(parseBlock());
                    }
                }
                
                if (static_cast<int>(args.size()) < macroFn->minArgs() || (macroFn->restName.empty() && static_cast<int>(args.size()) > macroFn->maxArgs())) {
                    throw std::runtime_error("Parser Error: Macro '" + macroName.lexeme + "' expects " + std::to_string(macroFn->minArgs()) + (!macroFn->restName.empty() ? " or more" : (macroFn->minArgs() == macroFn->maxArgs() ? "" : " to " + std::to_string(macroFn->maxArgs()))) + " arguments, got " + std::to_string(args.size()) + ".");
                }
                
                if (quoteDepth > 0) {
                    return std::make_unique<MacroCallExpr>(macroName, std::move(args));
                }
                
                return expandMacro(macroName.lexeme, args);
            }
            if (!check(TokenType::LBRACKET)) {
                throw std::runtime_error("Parser Error: Expect '[', '{', or identifier after '@'.");
            }
            forceList = true;
        }

        if (match({ TokenType::LBRACKET })) {
            std::vector<std::vector<std::unique_ptr<Expr>>> matrixElements;
            std::vector<std::unique_ptr<Expr>> currentRow;

            if (!check(TokenType::RBRACKET)) {
                // ★ 先解析第一个元素
                currentRow.push_back(assignment());

                // ★ 检测推导式：[expr for x in ...] 或 @[expr for x in ...]
                if (check(TokenType::FOR)) {
                    auto valueExpr = std::move(currentRow[0]);
                    return parseComp(std::move(valueExpr), forceList);
                }

                // ★ 非推导式 → 继续解析矩阵
                if (match({ TokenType::COMMA })) {
                    do {
                        if (check(TokenType::SEMICOLON) || check(TokenType::RBRACKET)) break;
                        currentRow.push_back(assignment());
                    } while (match({ TokenType::COMMA }));
                }

                if (match({ TokenType::SEMICOLON })) {
                    matrixElements.push_back(std::move(currentRow));
                    currentRow.clear();
                    while (!check(TokenType::RBRACKET) && !isAtEnd()) {
                        if (check(TokenType::SEMICOLON)) { advance(); continue; }
                        currentRow.push_back(assignment());
                        if (match({ TokenType::COMMA })) continue;
                        else if (match({ TokenType::SEMICOLON })) {
                            matrixElements.push_back(std::move(currentRow));
                            currentRow.clear();
                        }
                        else if (!check(TokenType::RBRACKET)) {
                            throw std::runtime_error("Parser Error: Expect ',' or ';' or ']' inside matrix.");
                        }
                    }
                }

                if (!currentRow.empty()) matrixElements.push_back(std::move(currentRow));
            }

            while (match({ TokenType::NEWLINE })) {}
            consume(TokenType::RBRACKET, "Parser Error: Expect ']' after matrix structure.");
            if (forceList) {
                return std::make_unique<ListNode>(std::move(matrixElements));
            }
            return std::make_unique<MatrixNode>(std::move(matrixElements));
        }
        throw std::runtime_error("Parser Error: Expect expression at '" + peek().lexeme + "'.");
    }

    std::unique_ptr<Expr> Parser::expandMacro(const std::string& name, std::vector<std::unique_ptr<Expr>>& args) {
        Value macroVal = resolveMacro(name);
        if (macroVal.isNone() || !macroVal.isFunctionClosure()) {
            throw std::runtime_error("Parser Error: Macro '" + name + "' is not defined or not a function.");
        }
        ObjClosure* macroFn = macroVal.asFunction();

        std::vector<Value> callArgs;
        std::vector<std::unique_ptr<GcValueGuard>> guards;
        for (auto& a : args) {
            callArgs.push_back(AST_to_JC2(a.get()));
            guards.push_back(std::make_unique<GcValueGuard>(callArgs.back()));
        }
        
        Value resultVal;
        try {
            resultVal = VM::activeVM->callVMFunction(macroFn->compiledFnIndex, callArgs, macroFn);
        } catch (const ValueException& ex) {
            std::string errStr = ex.val.isString() ? ex.val.asString() : ex.val.toRepr();
            if (ex.val.isInstance() && ex.val.asInstance()->classDef->name == "Exception") {
                auto inst = ex.val.asInstance();
                auto itMsg = inst->properties.find("message");
                if (itMsg != inst->properties.end()) {
                    Value mVal = itMsg->second.val;
                    errStr = mVal.isString() ? mVal.asString() : mVal.toRepr();
                }
            }
            throw std::runtime_error("Macro Execution Error: " + errStr);
        }
        GcValueGuard resultGuard(resultVal);
        
        auto expander = [this](const std::string& mName, std::vector<std::unique_ptr<Expr>>& mArgs) -> std::unique_ptr<Expr> {
            return this->expandMacro(mName, mArgs);
        };
        
        auto expandedAst = JC2_to_AST(resultVal, expander, 0);
        if (!expandedAst) throw std::runtime_error("Parser Error: Macro '" + name + "' did not return a valid ASTNode.");
        
        return expandedAst;
    }

    std::unique_ptr<Expr> Parser::macroDefExpr(bool isTokenMacro) {
        Token name(TokenType::ERROR, "");
        if (match({ TokenType::DOLLAR })) {
            Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
            name = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
        } else {
            name = consume(TokenType::IDENTIFIER, isTokenMacro ? "Parser Error: Expect syntax macro name." : "Parser Error: Expect macro name.");
        }
        consume(TokenType::LPAREN, "Parser Error: Expect '(' after macro name.");
        
        std::vector<Token> params;
        std::string restName = "";
        
        if (!check(TokenType::RPAREN)) {
            do {
                if (!restName.empty()) throw std::runtime_error("Parser Error: Rest parameter must be last.");
                if (match({ TokenType::ELLIPSIS })) {
                    if (isTokenMacro) throw std::runtime_error("Parser Error: Token macro cannot have rest parameters.");
                    if (match({ TokenType::DOLLAR })) {
                        Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                        restName = "$" + idTok.lexeme;
                    } else {
                        restName = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name after '...'.").lexeme;
                    }
                } else {
                    if (match({ TokenType::DOLLAR })) {
                        Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                        params.push_back(Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line));
                    } else {
                        params.push_back(consume(TokenType::IDENTIFIER, "Parser Error: Expect macro parameter name."));
                    }
                }
            } while (match({ TokenType::COMMA }));
        }
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after macro parameters.");
        
        if (isTokenMacro && params.size() != 1) {
            throw std::runtime_error("Parser Error: Token macro must have exactly one parameter.");
        }

        consume(TokenType::ASSIGN, "Parser Error: Expect '=' after macro signature.");
        
        // ★ 注册占位符宏，允许递归调用
        ObjClosure* dummyMacro = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{}, std::vector<bool>{}, name.lexeme, nullptr
        );
        dummyMacro->isTokenMacro = isTokenMacro;
        dummyMacro->restName = restName;
        for (size_t i = 0; i < params.size(); ++i) {
            dummyMacro->paramNames.push_back(params[i].lexeme);
            dummyMacro->isRef.push_back(false);
            dummyMacro->defaultValues.push_back(Value::uninit());
        }
        defineMacro(name.lexeme, Value(dummyMacro));

        auto body = parseStatementOrBlock();
        
        std::vector<bool> paramIsRef(params.size(), false);
        std::vector<bool> paramIsConst(params.size(), false);
        std::vector<std::shared_ptr<Expr>> defaultExprs(params.size(), nullptr);
        std::vector<std::shared_ptr<Expr>> paramTypes(params.size(), nullptr);

        auto lambda = std::make_unique<LambdaExpr>(
            name.lexeme, params, std::move(paramIsRef), std::move(paramIsConst),
            std::move(defaultExprs), restName,
            std::move(paramTypes), nullptr,
            "<macro_body>", std::shared_ptr<Expr>(body.release())
        );
        Token internalName = name;
        internalName.lexeme = "<macro_temp_" + name.lexeme + ">";
        auto assign = std::make_unique<Assign>(internalName, std::move(lambda), false, false, false, false);

        Resolver resolver;
        resolver.resolve(assign.get());

        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, &VM::activeVM->getCompiledFunctions(), nullptr, nullptr, &resolver.exprSymbols, &resolver.patternSymbols);
        fnBuilder.allowInternalNames = true; // 宏定义内部编译使用 <macro_temp_xxx> 等内部名，跳过保留名检查
        fnBuilder.build(assign.get());

        IROptimizer::optimize(&fnGraph);
        RegisterAllocator::allocate(&fnGraph);
        
        Chunk chunk;
        int localCount = Emitter::emit(&fnGraph, chunk);

        VM::activeVM->execute(chunk, localCount);

        Value macroVal = VM::activeVM->getGlobal(internalName.lexeme);
        VM::activeVM->removeGlobal(internalName.lexeme);

        if (macroVal.isFunctionClosure()) {
            macroVal.asFunction()->isTokenMacro = isTokenMacro;
        }

        defineMacro(name.lexeme, macroVal);

        return std::make_unique<Literal>("none", false, false, true);
    }

    std::unique_ptr<Expr> Parser::transformQuote(Expr* expr) {
        if (!expr) return std::make_unique<Literal>("none", false, false, true);
        
        if (auto* unquote = dynamic_cast<UnquoteExpr*>(expr)) {
            return std::move(unquote->expr);
        }
        
        auto makeGetNameExpr = [&](const std::string& varName, int line) -> std::unique_ptr<Expr> {
            auto varExpr1 = std::make_unique<Variable>(Token(TokenType::IDENTIFIER, varName, line));
            auto varExpr2 = std::make_unique<Variable>(Token(TokenType::IDENTIFIER, varName, line));
            auto varExpr3 = std::make_unique<Variable>(Token(TokenType::IDENTIFIER, varName, line));
            
            auto strLit = std::make_unique<Variable>(Token(TokenType::IDENTIFIER, "string", line));
            auto cond = std::make_unique<Binary>(std::move(varExpr1), Token(TokenType::IN, "in", line), std::move(strLit));
            
            auto nameAccess = std::make_unique<DotAccess>(std::move(varExpr3), Token(TokenType::IDENTIFIER, "name", line));
            
            return std::make_unique<IfExpr>(std::move(cond), std::move(varExpr2), std::move(nameAccess));
        };

        auto makeASTNodeCall = [&](const std::string& type, int line, std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props) {
            std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> dictEntries;
            for (auto& p : props) {
                dictEntries.push_back({
                    std::make_unique<Literal>(p.first, true),
                    std::move(p.second)
                });
            }
            auto dictExpr = std::make_unique<DictLiteral>(std::move(dictEntries));
            
            std::vector<std::unique_ptr<Expr>> args;
            args.push_back(std::make_unique<Literal>(type, true));
            args.push_back(std::make_unique<Literal>(std::to_string(line)));
            args.push_back(std::move(dictExpr));
            
            return std::make_unique<Call>(Token(TokenType::IDENTIFIER, "ASTNode", 0, line), std::move(args));
        };

        if (auto* bin = dynamic_cast<Binary*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"op", std::make_unique<Literal>(bin->op.lexeme, true)});
            props.push_back({"left", transformQuote(bin->left.get())});
            props.push_back({"right", transformQuote(bin->right.get())});
            return makeASTNodeCall("Binary", bin->op.line, std::move(props));
        }
        if (auto* un = dynamic_cast<Unary*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"op", std::make_unique<Literal>(un->op.lexeme, true)});
            props.push_back({"right", transformQuote(un->right.get())});
            return makeASTNodeCall("Unary", un->op.line, std::move(props));
        }
        if (auto* lit = dynamic_cast<Literal*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"value", std::make_unique<Literal>(lit->value, true)});
            props.push_back({"isString", std::make_unique<Literal>(lit->isString ? "true" : "false", false, false, true)});
            props.push_back({"isImaginary", std::make_unique<Literal>(lit->isImaginary ? "true" : "false", false, false, true)});
            props.push_back({"isKeyword", std::make_unique<Literal>(lit->isKeyword ? "true" : "false", false, false, true)});
            return makeASTNodeCall("Literal", 0, std::move(props));
        }
        if (auto* var = dynamic_cast<Variable*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"name", std::make_unique<Literal>(var->name.lexeme, true)});
            return makeASTNodeCall("Variable", var->name.line, std::move(props));
        }
        if (auto* ta = dynamic_cast<TypeAssertExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"name", std::make_unique<Literal>(ta->name.lexeme, true)});
            props.push_back({"value", transformQuote(ta->value.get())});
            props.push_back({"typeHint", transformQuote(ta->typeHint.get())});
            return makeASTNodeCall("TypeAssertExpr", ta->name.line, std::move(props));
        }
        if (auto* assign = dynamic_cast<Assign*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!assign->name.lexeme.empty() && assign->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(assign->name.lexeme.substr(1), assign->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(assign->name.lexeme, true)});
            }
            props.push_back({"value", transformQuote(assign->value.get())});
            props.push_back({"isRef", std::make_unique<Literal>(assign->isRef ? "true" : "false", false, false, true)});
            props.push_back({"isState", std::make_unique<Literal>(assign->isState ? "true" : "false", false, false, true)});
            props.push_back({"isLocal", std::make_unique<Literal>(assign->isLocal ? "true" : "false", false, false, true)});
            props.push_back({"isConst", std::make_unique<Literal>(assign->isConst ? "true" : "false", false, false, true)});
            props.push_back({"typeHint", assign->typeHint ? transformQuote(assign->typeHint.get()) : std::make_unique<Literal>("none", false, false, true)});
            return makeASTNodeCall("Assign", assign->name.line, std::move(props));
        }
        if (auto* exprAssign = dynamic_cast<ExprAssign*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            
            auto tmpParam = Token(TokenType::IDENTIFIER, "<tmp_name>", 0);
            auto varExpr1 = std::make_unique<Variable>(tmpParam);
            auto varExpr2 = std::make_unique<Variable>(tmpParam);
            auto varExpr3 = std::make_unique<Variable>(tmpParam);
            
            auto strLit = std::make_unique<Variable>(Token(TokenType::IDENTIFIER, "string", 0));
            auto cond = std::make_unique<Binary>(std::move(varExpr1), Token(TokenType::IN, "in", 0), std::move(strLit));
            auto nameAccess = std::make_unique<DotAccess>(std::move(varExpr3), Token(TokenType::IDENTIFIER, "name", 0));
            auto ternary = std::make_unique<IfExpr>(std::move(cond), std::move(varExpr2), std::move(nameAccess));
            
            std::vector<Token> params = { tmpParam };
            std::vector<bool> paramIsRef = { false };
            std::vector<bool> paramIsConst = { false };
            std::vector<std::shared_ptr<Expr>> defaultExprs = { nullptr };
            std::vector<std::shared_ptr<Expr>> paramTypes = { nullptr };
            
            auto lambda = std::make_unique<LambdaExpr>(
                "<name_resolver>", params, paramIsRef, paramIsConst, defaultExprs, "",
                paramTypes, nullptr, "", std::move(ternary)
            );
            
            std::vector<std::unique_ptr<Expr>> callArgs;
            callArgs.push_back(transformQuote(exprAssign->target.get()));
            auto iife = std::make_unique<InvokeExpr>(std::move(lambda), std::move(callArgs));
            
            props.push_back({"name", std::move(iife)});
            props.push_back({"value", transformQuote(exprAssign->value.get())});
            props.push_back({"isRef", std::make_unique<Literal>(exprAssign->isRef ? "true" : "false", false, false, true)});
            props.push_back({"isState", std::make_unique<Literal>(exprAssign->isState ? "true" : "false", false, false, true)});
            props.push_back({"isLocal", std::make_unique<Literal>(exprAssign->isLocal ? "true" : "false", false, false, true)});
            props.push_back({"isConst", std::make_unique<Literal>(exprAssign->isConst ? "true" : "false", false, false, true)});
            return makeASTNodeCall("Assign", 0, std::move(props));
        }
        auto makeExprList = [&](const std::vector<std::unique_ptr<Expr>>& exprs) {
            std::vector<std::unique_ptr<Expr>> listArgs;
            for (const auto& e : exprs) listArgs.push_back(transformQuote(e.get()));
            return std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(listArgs));
        };

        std::function<std::unique_ptr<Expr>(Pattern*)> transformPattern = [&](Pattern* pat) -> std::unique_ptr<Expr> {
            if (!pat) return std::make_unique<Literal>("none", false, false, true);
            if (auto* lp = dynamic_cast<LiteralPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                props.push_back({"literal", transformQuote(lp->literal.get())});
                return makeASTNodeCall("LiteralPattern", 0, std::move(props));
            }
            if (auto* ep = dynamic_cast<ExprPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                props.push_back({"expr", transformQuote(ep->expr.get())});
                return makeASTNodeCall("ExprPattern", 0, std::move(props));
            }
            if (auto* dp = dynamic_cast<DynamicAssertPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                props.push_back({"expr", transformQuote(dp->expr.get())});
                return makeASTNodeCall("DynamicAssertPattern", 0, std::move(props));
            }
            if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                if (!vp->name.lexeme.empty() && vp->name.lexeme[0] == '$') {
                    props.push_back({"name", makeGetNameExpr(vp->name.lexeme.substr(1), vp->name.line)});
                } else {
                    props.push_back({"name", std::make_unique<Literal>(vp->name.lexeme, true)});
                }
                props.push_back({"modifier", std::make_unique<Literal>(std::to_string(static_cast<int>(vp->modifier)))});
                props.push_back({"isConst", std::make_unique<Literal>(vp->isConst ? "true" : "false", false, false, true)});
                props.push_back({"typeHint", vp->typeHint ? transformQuote(vp->typeHint.get()) : std::make_unique<Literal>("none", false, false, true)});
                return makeASTNodeCall("VariablePattern", vp->name.line, std::move(props));
            }
            if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                if (!rp->name.lexeme.empty() && rp->name.lexeme[0] == '$') {
                    props.push_back({"name", makeGetNameExpr(rp->name.lexeme.substr(1), rp->name.line)});
                } else {
                    props.push_back({"name", std::make_unique<Literal>(rp->name.lexeme, true)});
                }
                props.push_back({"modifier", std::make_unique<Literal>(std::to_string(static_cast<int>(rp->modifier)))});
                props.push_back({"isConst", std::make_unique<Literal>(rp->isConst ? "true" : "false", false, false, true)});
                props.push_back({"typeHint", rp->typeHint ? transformQuote(rp->typeHint.get()) : std::make_unique<Literal>("none", false, false, true)});
                return makeASTNodeCall("RestPattern", rp->name.line, std::move(props));
            }
            if (auto* listp = dynamic_cast<ListPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                std::vector<std::unique_ptr<Expr>> elemsArgs;
                for (auto& e : listp->elements) elemsArgs.push_back(transformPattern(e.get()));
                props.push_back({"elements", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(elemsArgs))});
                props.push_back({"rest", transformPattern(listp->rest.get())});
                return makeASTNodeCall("ListPattern", 0, std::move(props));
            }
            if (auto* matp = dynamic_cast<MatrixPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                std::vector<std::unique_ptr<Expr>> rowsArgs;
                for (auto& row : matp->rows) {
                    std::vector<std::unique_ptr<Expr>> rowArgs;
                    for (auto& e : row) rowArgs.push_back(transformPattern(e.get()));
                    rowsArgs.push_back(std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(rowArgs)));
                }
                props.push_back({"rows", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(rowsArgs))});
                props.push_back({"restRow", transformPattern(matp->restRow.get())});
                return makeASTNodeCall("MatrixPattern", 0, std::move(props));
            }
            if (auto* dictp = dynamic_cast<DictPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                std::vector<std::unique_ptr<Expr>> entriesArgs;
                for (auto& e : dictp->entries) {
                    std::vector<std::unique_ptr<Expr>> kvArgs;
                    kvArgs.push_back(std::make_unique<Literal>(e.first, true));
                    kvArgs.push_back(transformPattern(e.second.get()));
                    entriesArgs.push_back(std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(kvArgs)));
                }
                props.push_back({"entries", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(entriesArgs))});
                props.push_back({"rest", transformPattern(dictp->rest.get())});
                return makeASTNodeCall("DictPattern", 0, std::move(props));
            }
            if (auto* defp = dynamic_cast<DefaultPattern*>(pat)) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
                props.push_back({"inner", transformPattern(defp->inner.get())});
                props.push_back({"defaultExpr", transformQuote(defp->defaultExpr.get())});
                return makeASTNodeCall("DefaultPattern", 0, std::move(props));
            }
            throw std::runtime_error("Parser Error: Unsupported pattern type in quote block.");
        };

        if (auto* block = dynamic_cast<Block*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"statements", makeExprList(block->statements)});
            return makeASTNodeCall("Block", 0, std::move(props));
        }
        if (auto* call = dynamic_cast<Call*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"callee", std::make_unique<Literal>(call->callee.lexeme, true)});
            props.push_back({"arguments", makeExprList(call->arguments)});
            return makeASTNodeCall("Call", call->callee.line, std::move(props));
        }
        if (auto* ifExpr = dynamic_cast<IfExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"condition", transformQuote(ifExpr->condition.get())});
            props.push_back({"thenBranch", transformQuote(ifExpr->thenBranch.get())});
            props.push_back({"elseBranch", transformQuote(ifExpr->elseBranch.get())});
            return makeASTNodeCall("IfExpr", 0, std::move(props));
        }
        if (auto* whileExpr = dynamic_cast<WhileExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"condition", transformQuote(whileExpr->condition.get())});
            props.push_back({"body", transformQuote(whileExpr->body.get())});
            return makeASTNodeCall("WhileExpr", 0, std::move(props));
        }
        if (auto* forExpr = dynamic_cast<ForExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"initializer", transformQuote(forExpr->initializer.get())});
            props.push_back({"condition", transformQuote(forExpr->condition.get())});
            props.push_back({"update", transformQuote(forExpr->update.get())});
            props.push_back({"body", transformQuote(forExpr->body.get())});
            return makeASTNodeCall("ForExpr", 0, std::move(props));
        }
        if (auto* retExpr = dynamic_cast<ReturnExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"value", transformQuote(retExpr->value.get())});
            return makeASTNodeCall("ReturnExpr", retExpr->keyword.line, std::move(props));
        }
        if (auto* dot = dynamic_cast<DotAccess*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"object", transformQuote(dot->object.get())});
            if (!dot->field.lexeme.empty() && dot->field.lexeme[0] == '$') {
                props.push_back({"field", makeGetNameExpr(dot->field.lexeme.substr(1), dot->field.line)});
            } else {
                props.push_back({"field", std::make_unique<Literal>(dot->field.lexeme, true)});
            }
            return makeASTNodeCall("DotAccess", dot->field.line, std::move(props));
        }
        if (auto* dotAssign = dynamic_cast<DotAssign*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"object", transformQuote(dotAssign->object.get())});
            if (!dotAssign->field.lexeme.empty() && dotAssign->field.lexeme[0] == '$') {
                props.push_back({"field", makeGetNameExpr(dotAssign->field.lexeme.substr(1), dotAssign->field.line)});
            } else {
                props.push_back({"field", std::make_unique<Literal>(dotAssign->field.lexeme, true)});
            }
            props.push_back({"value", transformQuote(dotAssign->value.get())});
            props.push_back({"typeHint", dotAssign->typeHint ? transformQuote(dotAssign->typeHint.get()) : std::make_unique<Literal>("none", false, false, true)});
            return makeASTNodeCall("DotAssign", dotAssign->field.line, std::move(props));
        }
        if (auto* mcall = dynamic_cast<MethodCallExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"object", transformQuote(mcall->object.get())});
            if (!mcall->method.lexeme.empty() && mcall->method.lexeme[0] == '$') {
                props.push_back({"method", makeGetNameExpr(mcall->method.lexeme.substr(1), mcall->method.line)});
            } else {
                props.push_back({"method", std::make_unique<Literal>(mcall->method.lexeme, true)});
            }
            props.push_back({"arguments", makeExprList(mcall->arguments)});
            return makeASTNodeCall("MethodCallExpr", mcall->method.line, std::move(props));
        }
        if (auto* idx = dynamic_cast<IndexAccess*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"object", transformQuote(idx->object.get())});
            props.push_back({"indices", makeExprList(idx->indices)});
            return makeASTNodeCall("IndexAccess", 0, std::move(props));
        }
        if (auto* idxAssign = dynamic_cast<IndexAssign*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"name", std::make_unique<Literal>(idxAssign->name.lexeme, true)});
            props.push_back({"objectExpr", transformQuote(idxAssign->objectExpr.get())});
            
            std::vector<std::unique_ptr<Expr>> chainListArgs;
            for (const auto& level : idxAssign->indexChain) {
                chainListArgs.push_back(makeExprList(level));
            }
            auto chainList = std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(chainListArgs));
            props.push_back({"indexChain", std::move(chainList)});
            
            props.push_back({"value", transformQuote(idxAssign->value.get())});
            props.push_back({"typeHint", idxAssign->typeHint ? transformQuote(idxAssign->typeHint.get()) : std::make_unique<Literal>("none", false, false, true)});
            return makeASTNodeCall("IndexAssign", idxAssign->name.line, std::move(props));
        }
        if (auto* mat = dynamic_cast<MatrixNode*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            std::vector<std::unique_ptr<Expr>> rowsArgs;
            for (const auto& row : mat->elements) {
                rowsArgs.push_back(makeExprList(row));
            }
            auto rowsList = std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(rowsArgs));
            props.push_back({"elements", std::move(rowsList)});
            return makeASTNodeCall("MatrixNode", 0, std::move(props));
        }
        if (auto* lst = dynamic_cast<ListNode*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            std::vector<std::unique_ptr<Expr>> rowsArgs;
            for (const auto& row : lst->elements) {
                rowsArgs.push_back(makeExprList(row));
            }
            auto rowsList = std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(rowsArgs));
            props.push_back({"elements", std::move(rowsList)});
            return makeASTNodeCall("ListNode", 0, std::move(props));
        }
        if (auto* dict = dynamic_cast<DictLiteral*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            std::vector<std::unique_ptr<Expr>> entriesArgs;
            for (const auto& pair : dict->entries) {
                std::vector<std::unique_ptr<Expr>> kvArgs;
                kvArgs.push_back(transformQuote(pair.first.get()));
                kvArgs.push_back(transformQuote(pair.second.get()));
                entriesArgs.push_back(std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(kvArgs)));
            }
            auto entriesList = std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(entriesArgs));
            props.push_back({"entries", std::move(entriesList)});
            return makeASTNodeCall("DictLiteral", 0, std::move(props));
        }
        if (auto* set = dynamic_cast<SetLiteral*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"elements", makeExprList(set->elements)});
            return makeASTNodeCall("SetLiteral", 0, std::move(props));
        }
        if (auto* seq = dynamic_cast<SequenceExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"expressions", makeExprList(seq->expressions)});
            return makeASTNodeCall("SequenceExpr", 0, std::move(props));
        }
        if (auto* grp = dynamic_cast<GroupingExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"expression", transformQuote(grp->expression.get())});
            return makeASTNodeCall("GroupingExpr", 0, std::move(props));
        }
        if (auto* slc = dynamic_cast<SliceExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"start", transformQuote(slc->start.get())});
            props.push_back({"end", transformQuote(slc->end.get())});
            props.push_back({"step", transformQuote(slc->step.get())});
            return makeASTNodeCall("SliceExpr", 0, std::move(props));
        }
        if (auto* comp = dynamic_cast<CompoundAssign*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"target", transformQuote(comp->target.get())});
            props.push_back({"op", std::make_unique<Literal>(std::to_string(static_cast<int>(comp->op)))});
            props.push_back({"value", transformQuote(comp->value.get())});
            props.push_back({"isRef", std::make_unique<Literal>(comp->isRef ? "true" : "false", false, false, true)});
            props.push_back({"isState", std::make_unique<Literal>(comp->isState ? "true" : "false", false, false, true)});
            props.push_back({"isLocal", std::make_unique<Literal>(comp->isLocal ? "true" : "false", false, false, true)});
            return makeASTNodeCall("CompoundAssign", 0, std::move(props));
        }
        if (auto* brk = dynamic_cast<BreakExpr*>(expr)) return makeASTNodeCall("BreakExpr", brk->keyword.line, {});
        if (auto* cnt = dynamic_cast<ContinueExpr*>(expr)) return makeASTNodeCall("ContinueExpr", cnt->keyword.line, {});
        if (dynamic_cast<SuperExpr*>(expr)) return makeASTNodeCall("SuperExpr", 0, {});
        if (dynamic_cast<SelfExpr*>(expr)) return makeASTNodeCall("SelfExpr", 0, {});
        if (auto* decl = dynamic_cast<LocalDecl*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!decl->name.lexeme.empty() && decl->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(decl->name.lexeme.substr(1), decl->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(decl->name.lexeme, true)});
            }
            props.push_back({"isConst", std::make_unique<Literal>(decl->isConst ? "true" : "false", false, false, true)});
            return makeASTNodeCall("LocalDecl", decl->name.line, std::move(props));
        }
        if (auto* decl = dynamic_cast<RefDecl*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!decl->name.lexeme.empty() && decl->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(decl->name.lexeme.substr(1), decl->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(decl->name.lexeme, true)});
            }
            props.push_back({"isConst", std::make_unique<Literal>(decl->isConst ? "true" : "false", false, false, true)});
            return makeASTNodeCall("RefDecl", decl->name.line, std::move(props));
        }
        if (auto* decl = dynamic_cast<StateDecl*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!decl->name.lexeme.empty() && decl->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(decl->name.lexeme.substr(1), decl->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(decl->name.lexeme, true)});
            }
            props.push_back({"isConst", std::make_unique<Literal>(decl->isConst ? "true" : "false", false, false, true)});
            return makeASTNodeCall("StateDecl", decl->name.line, std::move(props));
        }
        if (auto* decl = dynamic_cast<ConstDecl*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!decl->name.lexeme.empty() && decl->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(decl->name.lexeme.substr(1), decl->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(decl->name.lexeme, true)});
            }
            return makeASTNodeCall("ConstDecl", decl->name.line, std::move(props));
        }
        if (auto* del = dynamic_cast<DeleteExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            std::vector<std::unique_ptr<Expr>> namesArgs;
            for (const auto& n : del->names) {
                if (!n.lexeme.empty() && n.lexeme[0] == '$') {
                    namesArgs.push_back(makeGetNameExpr(n.lexeme.substr(1), n.line));
                } else {
                    namesArgs.push_back(std::make_unique<Literal>(n.lexeme, true));
                }
            }
            props.push_back({"names", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(namesArgs))});
            return makeASTNodeCall("DeleteExpr", 0, std::move(props));
        }
        if (auto* thr = dynamic_cast<ThrowExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"value", transformQuote(thr->value.get())});
            return makeASTNodeCall("ThrowExpr", thr->keyword.line, std::move(props));
        }
        if (auto* imp = dynamic_cast<ImportExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"path", transformQuote(imp->path.get())});
            return makeASTNodeCall("ImportExpr", 0, std::move(props));
        }
        if (auto* inv = dynamic_cast<InvokeExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"callee", transformQuote(inv->callee.get())});
            props.push_back({"arguments", makeExprList(inv->arguments)});
            return makeASTNodeCall("InvokeExpr", 0, std::move(props));
        }
        if (auto* lam = dynamic_cast<LambdaExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!lam->name.empty() && lam->name[0] == '$') {
                props.push_back({"name", makeGetNameExpr(lam->name.substr(1), 0)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(lam->name, true)});
            }
            std::vector<std::unique_ptr<Expr>> paramsArgs;
            for (const auto& p : lam->params) {
                if (!p.lexeme.empty() && p.lexeme[0] == '$') {
                    paramsArgs.push_back(makeGetNameExpr(p.lexeme.substr(1), p.line));
                } else {
                    paramsArgs.push_back(std::make_unique<Literal>(p.lexeme, true));
                }
            }
            props.push_back({"params", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(paramsArgs))});
            std::vector<std::unique_ptr<Expr>> refArgs;
            for (bool b : lam->paramIsRef) refArgs.push_back(std::make_unique<Literal>(b ? "true" : "false", false, false, true));
            props.push_back({"paramIsRef", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(refArgs))});
            std::vector<std::unique_ptr<Expr>> constArgs;
            for (bool b : lam->paramIsConst) constArgs.push_back(std::make_unique<Literal>(b ? "true" : "false", false, false, true));
            props.push_back({"paramIsConst", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(constArgs))});
            std::vector<std::unique_ptr<Expr>> defArgs;
            for (const auto& d : lam->defaultExprs) defArgs.push_back(transformQuote(d.get()));
            props.push_back({"defaultExprs", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(defArgs))});
            props.push_back({"restName", std::make_unique<Literal>(lam->restName, true)});
            props.push_back({"kwargsName", std::make_unique<Literal>(lam->kwargsName, true)});
            std::vector<std::unique_ptr<Expr>> kwargParamsArgs;
            for (const auto& p : lam->kwargParams) kwargParamsArgs.push_back(std::make_unique<Literal>(p.lexeme, true));
            props.push_back({"kwargParams", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(kwargParamsArgs))});
            std::vector<std::unique_ptr<Expr>> typeArgs;
            for (const auto& t : lam->paramTypes) typeArgs.push_back(transformQuote(t.get()));
            props.push_back({"paramTypes", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(typeArgs))});
            props.push_back({"returnType", transformQuote(lam->returnType.get())});
            props.push_back({"rawBody", std::make_unique<Literal>(lam->rawBody, true)});
            props.push_back({"body", transformQuote(lam->body.get())});
            return makeASTNodeCall("LambdaExpr", 0, std::move(props));
        }
        if (auto* fstr = dynamic_cast<FStringExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            std::vector<std::unique_ptr<Expr>> litArgs;
            for (const auto& l : fstr->literals) litArgs.push_back(std::make_unique<Literal>(l, true));
            props.push_back({"literals", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(litArgs))});
            props.push_back({"exprs", makeExprList(fstr->exprs)});
            std::vector<std::unique_ptr<Expr>> specArgs;
            for (const auto& s : fstr->formatSpecs) specArgs.push_back(std::make_unique<Literal>(s, true));
            props.push_back({"formatSpecs", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(specArgs))});
            return makeASTNodeCall("FStringExpr", 0, std::move(props));
        }
        if (auto* forIn = dynamic_cast<ForInExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"pattern", transformPattern(forIn->pattern.get())});
            props.push_back({"iterable", transformQuote(forIn->iterable.get())});
            props.push_back({"body", transformQuote(forIn->body.get())});
            props.push_back({"isLocal", std::make_unique<Literal>(forIn->isLocal ? "true" : "false", false, false, true)});
            props.push_back({"isConst", std::make_unique<Literal>(forIn->isConst ? "true" : "false", false, false, true)});
            return makeASTNodeCall("ForInExpr", 0, std::move(props));
        }
        if (auto* tryCatch = dynamic_cast<TryCatchExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"tryBody", transformQuote(tryCatch->tryBody.get())});
            props.push_back({"catchPattern", transformPattern(tryCatch->catchPattern.get())});
            props.push_back({"catchBody", transformQuote(tryCatch->catchBody.get())});
            return makeASTNodeCall("TryCatchExpr", 0, std::move(props));
        }
        if (auto* sw = dynamic_cast<SwitchExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"subject", transformQuote(sw->subject.get())});
            std::vector<std::unique_ptr<Expr>> casesArgs;
            for (const auto& c : sw->cases) {
                std::vector<std::unique_ptr<Expr>> casePairArgs;
                casePairArgs.push_back(makeExprList(c.first));
                casePairArgs.push_back(transformQuote(c.second.get()));
                casesArgs.push_back(std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(casePairArgs)));
            }
            props.push_back({"cases", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(casesArgs))});
            props.push_back({"defaultBody", transformQuote(sw->defaultBody.get())});
            return makeASTNodeCall("SwitchExpr", 0, std::move(props));
        }
        if (auto* cls = dynamic_cast<ClassDefExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!cls->name.lexeme.empty() && cls->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(cls->name.lexeme.substr(1), cls->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(cls->name.lexeme, true)});
            }
            props.push_back({"superClassExpr", transformQuote(cls->superClassExpr.get())});
            
            auto serializeProps = [&](const std::vector<ClassDefExpr::PropertyDef>& properties) {
                std::vector<std::unique_ptr<Expr>> args;
                for (const auto& p : properties) {
                    std::vector<std::unique_ptr<Expr>> pairArgs;
                    if (!p.name.lexeme.empty() && p.name.lexeme[0] == '$') {
                        pairArgs.push_back(makeGetNameExpr(p.name.lexeme.substr(1), p.name.line));
                    } else {
                        pairArgs.push_back(std::make_unique<Literal>(p.name.lexeme, true));
                    }
                    pairArgs.push_back(transformQuote(p.value.get()));
                    pairArgs.push_back(std::make_unique<Literal>(p.isLocal ? "true" : "false", false, false, true));
                    pairArgs.push_back(std::make_unique<Literal>(p.isConst ? "true" : "false", false, false, true));
                    args.push_back(std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(pairArgs)));
                }
                return std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(args));
            };
            
            props.push_back({"staticProperties", serializeProps(cls->staticProperties)});
            props.push_back({"instanceProperties", serializeProps(cls->instanceProperties)});
            
            return makeASTNodeCall("ClassDefExpr", cls->name.line, std::move(props));
        }
        if (auto* ns = dynamic_cast<NamespaceDecl*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!ns->name.lexeme.empty() && ns->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(ns->name.lexeme.substr(1), ns->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(ns->name.lexeme, true)});
            }
            props.push_back({"body", transformQuote(ns->body.get())});
            return makeASTNodeCall("NamespaceDecl", ns->name.line, std::move(props));
        }
        if (auto* enm = dynamic_cast<EnumDefExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!enm->name.lexeme.empty() && enm->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(enm->name.lexeme.substr(1), enm->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(enm->name.lexeme, true)});
            }
            std::vector<std::unique_ptr<Expr>> membersArgs;
            for (const auto& m : enm->members) {
                std::vector<std::unique_ptr<Expr>> pairArgs;
                if (!m.first.lexeme.empty() && m.first.lexeme[0] == '$') {
                    pairArgs.push_back(makeGetNameExpr(m.first.lexeme.substr(1), m.first.line));
                } else {
                    pairArgs.push_back(std::make_unique<Literal>(m.first.lexeme, true));
                }
                if (m.second) {
                    pairArgs.push_back(transformQuote(m.second.get()));
                } else {
                    pairArgs.push_back(std::make_unique<Literal>("none", false, false, true));
                }
                membersArgs.push_back(std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(pairArgs)));
            }
            props.push_back({"members", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(membersArgs))});
            return makeASTNodeCall("EnumDefExpr", enm->name.line, std::move(props));
        }
        if (auto* dest = dynamic_cast<DestructAssign*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"pattern", transformPattern(dest->pattern.get())});
            props.push_back({"value", transformQuote(dest->value.get())});
            props.push_back({"isRef", std::make_unique<Literal>(dest->isRef ? "true" : "false", false, false, true)});
            props.push_back({"isState", std::make_unique<Literal>(dest->isState ? "true" : "false", false, false, true)});
            props.push_back({"isLocal", std::make_unique<Literal>(dest->isLocal ? "true" : "false", false, false, true)});
            props.push_back({"isConst", std::make_unique<Literal>(dest->isConst ? "true" : "false", false, false, true)});
            return makeASTNodeCall("DestructAssign", 0, std::move(props));
        }
        if (auto* mcomp = dynamic_cast<MatrixCompExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"valueExpr", transformQuote(mcomp->valueExpr.get())});
            std::vector<std::unique_ptr<Expr>> clausesArgs;
            for (const auto& c : mcomp->clauses) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> cProps;
                cProps.push_back({"pattern", transformPattern(c.pattern.get())});
                cProps.push_back({"iterable", transformQuote(c.iterable.get())});
                std::vector<std::unique_ptr<Expr>> condsArgs;
                for (const auto& cond : c.conditions) condsArgs.push_back(transformQuote(cond.get()));
                cProps.push_back({"conditions", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(condsArgs))});
                clausesArgs.push_back(makeASTNodeCall("CompClause", 0, std::move(cProps)));
            }
            props.push_back({"clauses", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(clausesArgs))});
            return makeASTNodeCall("MatrixCompExpr", 0, std::move(props));
        }
        if (auto* lcomp = dynamic_cast<ListCompExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"valueExpr", transformQuote(lcomp->valueExpr.get())});
            std::vector<std::unique_ptr<Expr>> clausesArgs;
            for (const auto& c : lcomp->clauses) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> cProps;
                cProps.push_back({"pattern", transformPattern(c.pattern.get())});
                cProps.push_back({"iterable", transformQuote(c.iterable.get())});
                std::vector<std::unique_ptr<Expr>> condsArgs;
                for (const auto& cond : c.conditions) condsArgs.push_back(transformQuote(cond.get()));
                cProps.push_back({"conditions", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(condsArgs))});
                clausesArgs.push_back(makeASTNodeCall("CompClause", 0, std::move(cProps)));
            }
            props.push_back({"clauses", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(clausesArgs))});
            return makeASTNodeCall("ListCompExpr", 0, std::move(props));
        }
        if (auto* scomp = dynamic_cast<SetCompExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"valueExpr", transformQuote(scomp->valueExpr.get())});
            std::vector<std::unique_ptr<Expr>> clausesArgs;
            for (const auto& c : scomp->clauses) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> cProps;
                cProps.push_back({"pattern", transformPattern(c.pattern.get())});
                cProps.push_back({"iterable", transformQuote(c.iterable.get())});
                std::vector<std::unique_ptr<Expr>> condsArgs;
                for (const auto& cond : c.conditions) condsArgs.push_back(transformQuote(cond.get()));
                cProps.push_back({"conditions", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(condsArgs))});
                clausesArgs.push_back(makeASTNodeCall("CompClause", 0, std::move(cProps)));
            }
            props.push_back({"clauses", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(clausesArgs))});
            return makeASTNodeCall("SetCompExpr", 0, std::move(props));
        }
        if (auto* dcomp = dynamic_cast<DictCompExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"keyExpr", transformQuote(dcomp->keyExpr.get())});
            props.push_back({"valueExpr", transformQuote(dcomp->valueExpr.get())});
            std::vector<std::unique_ptr<Expr>> clausesArgs;
            for (const auto& c : dcomp->clauses) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> cProps;
                cProps.push_back({"pattern", transformPattern(c.pattern.get())});
                cProps.push_back({"iterable", transformQuote(c.iterable.get())});
                std::vector<std::unique_ptr<Expr>> condsArgs;
                for (const auto& cond : c.conditions) condsArgs.push_back(transformQuote(cond.get()));
                cProps.push_back({"conditions", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(condsArgs))});
                clausesArgs.push_back(makeASTNodeCall("CompClause", 0, std::move(cProps)));
            }
            props.push_back({"clauses", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(clausesArgs))});
            return makeASTNodeCall("DictCompExpr", 0, std::move(props));
        }
        if (auto* matchExpr = dynamic_cast<MatchExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"subject", transformQuote(matchExpr->subject.get())});
            std::vector<std::unique_ptr<Expr>> branchesArgs;
            for (const auto& b : matchExpr->branches) {
                std::vector<std::pair<std::string, std::unique_ptr<Expr>>> bProps;
                std::vector<std::unique_ptr<Expr>> patsArgs;
                for (const auto& p : b.patterns) patsArgs.push_back(transformPattern(p.get()));
                bProps.push_back({"patterns", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(patsArgs))});
                bProps.push_back({"guard", transformQuote(b.guard.get())});
                bProps.push_back({"body", transformQuote(b.body.get())});
                branchesArgs.push_back(makeASTNodeCall("MatchBranch", 0, std::move(bProps)));
            }
            props.push_back({"branches", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(branchesArgs))});
            return makeASTNodeCall("MatchExpr", 0, std::move(props));
        }
        if (auto* mdef = dynamic_cast<MacroDefExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!mdef->name.lexeme.empty() && mdef->name.lexeme[0] == '$') {
                props.push_back({"name", makeGetNameExpr(mdef->name.lexeme.substr(1), mdef->name.line)});
            } else {
                props.push_back({"name", std::make_unique<Literal>(mdef->name.lexeme, true)});
            }
            std::vector<std::unique_ptr<Expr>> paramsArgs;
            for (const auto& p : mdef->params) {
                if (!p.lexeme.empty() && p.lexeme[0] == '$') {
                    paramsArgs.push_back(makeGetNameExpr(p.lexeme.substr(1), p.line));
                } else {
                    paramsArgs.push_back(std::make_unique<Literal>(p.lexeme, true));
                }
            }
            props.push_back({"params", std::make_unique<Call>(Token(TokenType::IDENTIFIER, "list", 0, 0), std::move(paramsArgs))});
            props.push_back({"restName", std::make_unique<Literal>(mdef->restName, true)});
            props.push_back({"isTokenMacro", std::make_unique<Literal>(mdef->isTokenMacro ? "true" : "false", false, false, true)});
            props.push_back({"body", transformQuote(mdef->body.get())});
            return makeASTNodeCall("MacroDefExpr", mdef->name.line, std::move(props));
        }
        if (auto* mcall = dynamic_cast<MacroCallExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"macroName", std::make_unique<Literal>(mcall->macroName.lexeme, true)});
            props.push_back({"arguments", makeExprList(mcall->arguments)});
            return makeASTNodeCall("MacroCallExpr", mcall->macroName.line, std::move(props));
        }
        if (auto* qe = dynamic_cast<QuoteExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"body", transformQuote(qe->body.get())});
            return makeASTNodeCall("QuoteExpr", 0, std::move(props));
        }
        if (auto* def = dynamic_cast<DeferExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"body", transformQuote(def->body.get())});
            return makeASTNodeCall("DeferExpr", 0, std::move(props));
        }
        if (auto* kw = dynamic_cast<KeywordArgExpr*>(expr)) {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            props.push_back({"name", std::make_unique<Literal>(kw->name.lexeme, true)});
            props.push_back({"value", transformQuote(kw->value.get())});
            return makeASTNodeCall("KeywordArgExpr", kw->name.line, std::move(props));
        }
        
        throw std::runtime_error("Parser Error: Unsupported AST node in quote block.");
    }

    std::unique_ptr<Expr> Parser::quoteExpr() {
        while (match({ TokenType::NEWLINE })) {}
        quoteDepth++;
        auto body = assignment();
        quoteDepth--;
        return transformQuote(body.get());
    }

    std::unique_ptr<Expr> Parser::deferExpr() {
        while (match({ TokenType::NEWLINE })) {}
        auto body = assignment();
        return std::make_unique<DeferExpr>(std::move(body));
    }

    std::unique_ptr<Expr> Parser::switchExpr() {
        consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'switch'.");
        auto subject = expression();
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after switch expression.");
        while (match({ TokenType::NEWLINE })) {}
        
        consume(TokenType::LBRACE, "Parser Error: Expect '{' to open switch body.");

        MacroScopeGuard guard(this);
        std::vector<std::pair<std::vector<std::unique_ptr<Expr>>, std::unique_ptr<Expr>>> cases;
        std::unique_ptr<Expr> defaultBody = nullptr;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break;
            
            if (match({ TokenType::CASE })) {
                std::vector<std::unique_ptr<Expr>> values;
                values.push_back(assignment()); 
                while (match({ TokenType::COMMA })) {
                    values.push_back(assignment()); 
                }
                consume(TokenType::COLON, "Parser Error: Expect ':' after case value(s).");
                
                // 持续吸收语句，直到遇到下一个 case, default 或 }
                std::vector<std::unique_ptr<Expr>> stmts;
                while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd()) {
                    while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
                    if (check(TokenType::CASE) || check(TokenType::DEFAULT) || check(TokenType::RBRACE)) break;
                    
                    auto expr = expression();
                    if (auto* seq = dynamic_cast<SequenceExpr*>(expr.get())) {
                        for (auto& e : seq->expressions) stmts.push_back(std::move(e));
                    } else {
                        stmts.push_back(std::move(expr));
                    }
                    
                    if (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                        throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
                    }
                    while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
                }
                cases.push_back({ std::move(values), std::make_unique<Block>(std::move(stmts)) });
            }
            else if (match({ TokenType::DEFAULT })) {
                consume(TokenType::COLON, "Parser Error: Expect ':' after 'default'.");
                std::vector<std::unique_ptr<Expr>> stmts;
                while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd()) {
                    while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
                    if (check(TokenType::CASE) || check(TokenType::DEFAULT) || check(TokenType::RBRACE)) break;
                    
                    auto expr = expression();
                    if (auto* seq = dynamic_cast<SequenceExpr*>(expr.get())) {
                        for (auto& e : seq->expressions) stmts.push_back(std::move(e));
                    } else {
                        stmts.push_back(std::move(expr));
                    }
                    
                    if (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                        throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
                    }
                    while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
                }
                defaultBody = std::make_unique<Block>(std::move(stmts));
            }
            else {
                throw std::runtime_error("Parser Error: Expect 'case' or 'default' inside switch.");
            }
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' to close switch body.");

        return std::make_unique<SwitchExpr>(std::move(subject), std::move(cases), std::move(defaultBody));
    }

    std::unique_ptr<Pattern> Parser::parsePrimaryPattern() {
        ScopeModifier mod = ScopeModifier::None;
        bool hasMod = false;
        bool isConst = false;
        while (true) {
            if (match({TokenType::LOCAL})) { mod = ScopeModifier::Local; hasMod = true; }
            else if (match({TokenType::REF})) { mod = ScopeModifier::Ref; hasMod = true; }
            else if (match({TokenType::STATE})) { mod = ScopeModifier::State; hasMod = true; }
            else if (match({TokenType::CONST})) { isConst = true; hasMod = true; }
            else break;
        }

        if (match({TokenType::ELLIPSIS})) {
            Token name(TokenType::ERROR, "");
            if (match({TokenType::DOLLAR})) {
                Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                name = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
            } else {
                name = consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name or '_' after '...'.");
            }
            auto typeHint = parseOptionalTypeHint();
            return std::make_unique<RestPattern>(name, mod, isConst, std::move(typeHint));
        }
        
        if (hasMod) {
            Token name(TokenType::ERROR, "");
            if (match({TokenType::DOLLAR})) {
                Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                name = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
            } else {
                name = consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name after modifier.");
            }
            auto typeHint = parseOptionalTypeHint();
            return std::make_unique<VariablePattern>(name, mod, isConst, std::move(typeHint));
        }
        if (match({TokenType::LBRACKET})) {
            std::vector<std::vector<std::unique_ptr<Pattern>>> rows;
            std::vector<std::unique_ptr<Pattern>> currentRow;
            std::unique_ptr<RestPattern> restCol = nullptr;
            std::unique_ptr<RestPattern> restRow = nullptr;
            bool isMatrix = false;

            while (!check(TokenType::RBRACKET) && !isAtEnd()) {
                while (match({TokenType::NEWLINE})) {}
                if (check(TokenType::RBRACKET)) break;

                if (match({TokenType::SEMICOLON})) {
                    isMatrix = true;
                    if (restCol) {
                        currentRow.push_back(std::move(restCol));
                        restCol = nullptr;
                    }
                    rows.push_back(std::move(currentRow));
                    currentRow.clear();
                    continue;
                }
                if (check(TokenType::ELLIPSIS) || check(TokenType::LOCAL) || check(TokenType::REF) || check(TokenType::STATE) || check(TokenType::CONST)) {
                    int savedPos = current;
                    ScopeModifier elemMod = ScopeModifier::None;
                    bool elemConst = false;
                    while (true) {
                        if (match({TokenType::LOCAL})) elemMod = ScopeModifier::Local;
                        else if (match({TokenType::REF})) elemMod = ScopeModifier::Ref;
                        else if (match({TokenType::STATE})) elemMod = ScopeModifier::State;
                        else if (match({TokenType::CONST})) elemConst = true;
                        else break;
                    }
                    
                    if (match({TokenType::ELLIPSIS})) {
                        Token name = consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name or '_' after '...'.");
                        auto typeHint = parseOptionalTypeHint();
                        
                        bool hasRest = false;
                        for (const auto& pat : currentRow) {
                            if (dynamic_cast<RestPattern*>(pat.get())) {
                                hasRest = true;
                                break;
                            }
                        }
                        if (hasRest) {
                            throw std::runtime_error("Parser Error: Multiple rest patterns ('...') are not allowed in a single row.");
                        }

                        if (check(TokenType::SEMICOLON) || check(TokenType::RBRACKET)) {
                            if (check(TokenType::RBRACKET) && currentRow.empty() && !rows.empty()) {
                                restRow = std::make_unique<RestPattern>(name, elemMod, elemConst, typeHint);
                                isMatrix = true;
                            } else {
                                restCol = std::make_unique<RestPattern>(name, elemMod, elemConst, typeHint);
                            }
                            continue;
                        } else {
                            currentRow.push_back(std::make_unique<RestPattern>(name, elemMod, elemConst, typeHint));
                            if (!match({TokenType::COMMA})) {
                                throw std::runtime_error("Parser Error: Expect ',' after pattern.");
                            }
                            continue;
                        }
                    } else {
                        current = savedPos;
                    }
                }
                
                currentRow.push_back(parsePattern());
                
                if (!match({TokenType::COMMA})) {
                    if (check(TokenType::SEMICOLON) || check(TokenType::RBRACKET)) {
                        // fine
                    } else {
                        throw std::runtime_error("Parser Error: Expect ',' or ';' or ']' in pattern.");
                    }
                }
            }
            if (!currentRow.empty() || restCol) {
                if (isMatrix && restCol) {
                    currentRow.push_back(std::move(restCol));
                    restCol = nullptr;
                }
                rows.push_back(std::move(currentRow));
            }
            consume(TokenType::RBRACKET, "Parser Error: Expect ']' after pattern.");

            if (!isMatrix && rows.size() <= 1) {
                auto elements = rows.empty() ? std::vector<std::unique_ptr<Pattern>>() : std::move(rows[0]);
                return std::make_unique<ListPattern>(std::move(elements), std::move(restCol));
            } else {
                return std::make_unique<MatrixPattern>(std::move(rows), std::move(restRow));
            }
        }
        if (match({TokenType::LBRACE})) {
            std::vector<std::pair<std::string, std::unique_ptr<Pattern>>> entries;
            std::unique_ptr<RestPattern> rest = nullptr;
            
            while (!check(TokenType::RBRACE) && !isAtEnd()) {
                while (match({TokenType::NEWLINE})) {}
                if (check(TokenType::RBRACE)) break;

                ScopeModifier elemMod = ScopeModifier::None;
                bool elemConst = false;
                bool elemHasMod = false;
                while (true) {
                    if (match({TokenType::LOCAL})) { elemMod = ScopeModifier::Local; elemHasMod = true; }
                    else if (match({TokenType::REF})) { elemMod = ScopeModifier::Ref; elemHasMod = true; }
                    else if (match({TokenType::STATE})) { elemMod = ScopeModifier::State; elemHasMod = true; }
                    else if (match({TokenType::CONST})) { elemConst = true; elemHasMod = true; }
                    else break;
                }
                
                if (match({TokenType::ELLIPSIS})) {
                    Token name = consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name or '_' after '...'.");
                    auto typeHint = parseOptionalTypeHint();
                    rest = std::make_unique<RestPattern>(name, elemMod, elemConst, std::move(typeHint));
                    break; // Rest must be last
                }
                
                std::string keyStr;
                Token keyTok = peek();
                if (match({TokenType::IDENTIFIER})) {
                    keyStr = previous().lexeme;
                } else if (!elemHasMod && match({TokenType::STRING})) {
                    keyStr = previous().lexeme;
                } else {
                    throw std::runtime_error("Parser Error: Expect identifier or string as dict pattern key.");
                }
                
                if (!elemHasMod && match({TokenType::COLON})) {
                    entries.push_back({keyStr, parsePattern()});
                } else {
                    if (match({TokenType::ASSIGN})) {
                        auto defExpr = ternary();
                        entries.push_back({keyStr, std::make_unique<DefaultPattern>(std::make_unique<VariablePattern>(keyTok, elemMod, elemConst), std::move(defExpr))});
                    } else {
                        entries.push_back({keyStr, std::make_unique<VariablePattern>(keyTok, elemMod, elemConst)});
                    }
                }

                if (!match({TokenType::COMMA})) {
                    while (match({TokenType::NEWLINE})) {}
                    break;
                }
            }
            while (match({TokenType::NEWLINE})) {}
            consume(TokenType::RBRACE, "Parser Error: Expect '}' after dict pattern.");
            return std::make_unique<DictPattern>(std::move(entries), std::move(rest));
        }
        
        auto expr = call();
        if (auto* var = dynamic_cast<Variable*>(expr.get())) {
            auto typeHint = parseOptionalTypeHint();
            return std::make_unique<VariablePattern>(var->name, mod, isConst, std::move(typeHint));
        }
        if (auto* unq = dynamic_cast<UnquoteExpr*>(expr.get())) {
            if (auto* var = dynamic_cast<Variable*>(unq->expr.get())) {
                Token nameTok(TokenType::IDENTIFIER, "$" + var->name.lexeme, var->name.position, var->name.line);
                auto typeHint = parseOptionalTypeHint();
                return std::make_unique<VariablePattern>(nameTok, mod, isConst, std::move(typeHint));
            }
        }
        if (dynamic_cast<IndexAccess*>(expr.get()) || dynamic_cast<DotAccess*>(expr.get())) {
            return std::make_unique<ExprPattern>(std::move(expr));
        }
        if (auto* group = dynamic_cast<GroupingExpr*>(expr.get())) {
            return std::make_unique<DynamicAssertPattern>(std::move(group->expression));
        }
        if (dynamic_cast<SetLiteral*>(expr.get())) {
            return std::make_unique<DynamicAssertPattern>(std::move(expr));
        }
        return std::make_unique<LiteralPattern>(std::move(expr));
    }

    std::shared_ptr<Expr> Parser::parseOptionalTypeHint() {
        if (match({TokenType::COLON})) {
            return std::shared_ptr<Expr>(ternary().release());
        }
        return nullptr;
    }

    std::unique_ptr<Pattern> Parser::parsePattern() {
        auto pat = parsePrimaryPattern();
        if (match({TokenType::ASSIGN})) {
            if (dynamic_cast<RestPattern*>(pat.get())) {
                throw std::runtime_error("Parser Error: Rest pattern '...' cannot have a default value.");
            }
            auto defExpr = ternary();
            return std::make_unique<DefaultPattern>(std::move(pat), std::move(defExpr));
        }
        return pat;
    }

    std::unique_ptr<Expr> Parser::parseMatchBody() {
        while (match({TokenType::NEWLINE})) {}
        if (check(TokenType::LBRACE)) {
            if (!isDictLiteralLookahead(current + 1)) {
                return parseBlock();
            }
        }
        auto expr = assignment();
        std::vector<std::unique_ptr<Expr>> stmts;
        stmts.push_back(std::move(expr));
        return std::make_unique<Block>(std::move(stmts));
    }

    std::unique_ptr<Expr> Parser::matchExpr() {
        consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'match'.");
        auto subject = expression();
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after match subject.");
        while (match({TokenType::NEWLINE})) {}
        consume(TokenType::LBRACE, "Parser Error: Expect '{' to open match body.");

        MacroScopeGuard guard(this);
        std::vector<MatchBranch> branches;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({TokenType::NEWLINE, TokenType::SEMICOLON})) {}
            if (check(TokenType::RBRACE)) break;

            MatchBranch branch;
            
            do {
                branch.patterns.push_back(parsePattern());
            } while (match({TokenType::COMMA}));

            if (match({TokenType::IF})) {
                consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'if' guard.");
                branch.guard = expression();
                consume(TokenType::RPAREN, "Parser Error: Expect ')' after 'if' guard.");
            }

            consume(TokenType::ARROW, "Parser Error: Expect '=>' after match pattern.");

            branch.body = parseMatchBody();
            
            branches.push_back(std::move(branch));

            match({TokenType::COMMA});
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' to close match body.");

        return std::make_unique<MatchExpr>(std::move(subject), std::move(branches));
    }

    std::unique_ptr<Expr> Parser::namespaceExpr() {
        Token name(TokenType::IDENTIFIER, "", previous().position, previous().line);
        bool isNamed = false;

        if (match({ TokenType::DOLLAR })) {
            Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
            name = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
            isNamed = true;
        } else if (check(TokenType::IDENTIFIER)) {
            name = advance();
            isNamed = true;
        }

        if (isNamed) {
            while (match({ TokenType::NEWLINE })) {}
        }
        consume(TokenType::LBRACE, "Parser Error: Expect '{' after namespace definition.");
        MacroScopeGuard guard(this);
        std::vector<std::unique_ptr<Expr>> stmts;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break;
            
            auto expr = expression();
            if (auto* seq = dynamic_cast<SequenceExpr*>(expr.get())) {
                for (auto& e : seq->expressions) stmts.push_back(std::move(e));
            } else {
                stmts.push_back(std::move(expr));
            }
            
            if (!check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
            }
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after namespace body.");
        auto nsExpr = std::make_unique<NamespaceDecl>(name, std::make_unique<Block>(std::move(stmts)));
        if (isNamed) {
            return std::make_unique<Assign>(name, std::move(nsExpr), false, false, false, false);
        }
        return nsExpr;
    }

    std::unique_ptr<Expr> Parser::enumExpr() {
        Token name(TokenType::IDENTIFIER, "", previous().position, previous().line);
        bool isNamed = false;

        if (match({ TokenType::DOLLAR })) {
            Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
            name = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
            isNamed = true;
        } else if (check(TokenType::IDENTIFIER)) {
            name = advance();
            isNamed = true;
        }

        if (isNamed) {
            while (match({ TokenType::NEWLINE })) {}
        }
        consume(TokenType::LBRACE, "Parser Error: Expect '{' after enum definition.");

        std::vector<std::pair<Token, std::unique_ptr<Expr>>> members;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break;

            Token memberName(TokenType::ERROR, "");
            if (match({ TokenType::DOLLAR })) {
                Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                memberName = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
            } else {
                Token t = peek();
                if (t.type == TokenType::IDENTIFIER || (t.type >= TokenType::IF && t.type <= TokenType::NONE_KW)) {
                    memberName = advance();
                    memberName.type = TokenType::IDENTIFIER;
                } else {
                    throw std::runtime_error("Parser Error: Expect enum member name.");
                }
            }

            std::unique_ptr<Expr> value = nullptr;
            if (match({ TokenType::ASSIGN })) {
                value = ternary();
            }

            members.push_back({ memberName, std::move(value) });

            if (!match({ TokenType::COMMA })) {
                while (match({ TokenType::NEWLINE })) {}
                break;
            }
        }

        while (match({ TokenType::NEWLINE })) {}
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after enum body.");

        auto enumExpr = std::make_unique<EnumDefExpr>(name, std::move(members));
        if (isNamed) {
            return std::make_unique<Assign>(name, std::move(enumExpr), false, false, false, false);
        }
        return enumExpr;
    }

    std::unique_ptr<Expr> Parser::classDefExpr() {
        Token name(TokenType::IDENTIFIER, "", previous().position, previous().line);
        bool isNamed = false;

        if (match({ TokenType::DOLLAR })) {
            Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
            name = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
            isNamed = true;
        } else if (check(TokenType::IDENTIFIER) && peek().lexeme != "extends") {
            name = advance();
            isNamed = true;
        }

        if (isNamed) {
            while (match({ TokenType::NEWLINE })) {}
        }

        std::unique_ptr<Expr> superClassExpr = nullptr;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "extends") {
            advance();
            superClassExpr = expression();
        }

        if (isNamed || superClassExpr) {
            while (match({ TokenType::NEWLINE })) {}
        }
        consume(TokenType::LBRACE, "Parser Error: Expect '{' after class definition.");

        MacroScopeGuard guard(this);
        std::vector<ClassDefExpr::PropertyDef> staticProperties;
        std::vector<ClassDefExpr::PropertyDef> instanceProperties;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break;

            bool isStatic = false, isLocal = false, isConst = false;
            while (true) {
                if (match({ TokenType::STATIC })) {
                    if (isStatic) throw std::runtime_error("Parser Error: Duplicate 'static' modifier.");
                    isStatic = true;
                } else if (match({ TokenType::LOCAL })) {
                    if (isLocal) throw std::runtime_error("Parser Error: Duplicate 'local' modifier.");
                    isLocal = true;
                } else if (match({ TokenType::CONST })) {
                    if (isConst) throw std::runtime_error("Parser Error: Duplicate 'const' modifier.");
                    isConst = true;
                } else {
                    break;
                }
            }

            Token memberName(TokenType::ERROR, "");
            if (match({ TokenType::DOLLAR })) {
                Token idTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect identifier after '$'.");
                memberName = Token(TokenType::IDENTIFIER, "$" + idTok.lexeme, idTok.position, idTok.line);
            } else {
                Token t = peek();
                if (t.type == TokenType::IDENTIFIER || (t.type >= TokenType::IF && t.type <= TokenType::NONE_KW)) {
                    memberName = advance();
                    memberName.type = TokenType::IDENTIFIER;
                } else {
                    throw std::runtime_error("Parser Error: Expect method or field name.");
                }
            }

            std::unique_ptr<Expr> value;
            if (match({ TokenType::ASSIGN })) {
                value = ternary();
            } else {
                consume(TokenType::LPAREN, "Parser Error: Expect '(' after method name.");

                std::vector<Token> params;
                std::vector<bool> paramIsRef;
                std::vector<bool> paramIsConst;
                std::vector<std::shared_ptr<Expr>> defaultExprs;
                std::vector<std::shared_ptr<Expr>> paramTypes;
                std::string restName = "";
                std::vector<Token> kwargParams;
                std::vector<bool> kwargIsRef;
                std::vector<bool> kwargIsConst;
                std::vector<std::shared_ptr<Expr>> kwargDefaultExprs;
                std::vector<std::shared_ptr<Expr>> kwargTypes;
                std::string kwargsName = "";
                bool inKwOnly = false;

                std::vector<std::unique_ptr<Expr>> destructStmts;
                int destructCounter = 0;

                if (!check(TokenType::RPAREN)) {
                    while (true) {
                        // ★ 分号在参数列表开头：全仅关键字（f(; a, b)）
                        if (match({ TokenType::SEMICOLON })) {
                            if (inKwOnly) throw std::runtime_error("Parser Error: Only one ';' allowed in parameter list.");
                            inKwOnly = true;
                            continue;
                        }
                        // ★ rest 后不能再有位置参数；kwargs 后不能再有任何参数
                        if (!restName.empty() && !inKwOnly) throw std::runtime_error("Parser Error: Rest parameter must be last.");
                        if (!kwargsName.empty()) throw std::runtime_error("Parser Error: kwargs parameter must be last.");
                        bool isParamRef = false;
                        bool isParamConst = false;
                        while (true) {
                            if (match({ TokenType::REF })) {
                                if (isParamRef) throw std::runtime_error("Parser Error: Duplicate 'ref' modifier.");
                                isParamRef = true;
                            } else if (match({ TokenType::CONST })) {
                                if (isParamConst) throw std::runtime_error("Parser Error: Duplicate 'const' modifier.");
                                isParamConst = true;
                            } else {
                                break;
                            }
                        }

                        Token paramTok(TokenType::IDENTIFIER, "", 0, 0);
                        bool isRest = false;
                        bool isDestruct = false;
                        std::unique_ptr<Pattern> patNode = nullptr;

                        if (match({ TokenType::ELLIPSIS })) {
                            if (isParamRef) throw std::runtime_error("Parser Error: Rest/kwargs parameter cannot be passed by ref.");
                            paramTok = consume(TokenType::IDENTIFIER, "Expect parameter name.");
                            isRest = true;
                            if (inKwOnly) {
                                if (!kwargsName.empty()) throw std::runtime_error("Parser Error: Duplicate kwargs parameter.");
                                kwargsName = paramTok.lexeme;
                            } else {
                                if (!restName.empty()) throw std::runtime_error("Parser Error: Duplicate rest parameter.");
                                restName = paramTok.lexeme;
                            }
                        } else if (check(TokenType::LBRACE) || check(TokenType::LBRACKET)) {
                            if (inKwOnly) throw std::runtime_error("Parser Error: Destructured parameter cannot be keyword-only.");
                            if (isParamRef) throw std::runtime_error("Destructured parameter cannot be ref.");
                            patNode = parsePrimaryPattern();
                            std::string phName = "<param_destruct>_" + std::to_string(destructCounter++);
                            paramTok = Token(TokenType::IDENTIFIER, phName, memberName.line);
                            isDestruct = true;
                        } else {
                            paramTok = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name.");
                        }

                        if (!isRest) {
                            if (inKwOnly) {
                                kwargParams.push_back(paramTok);
                                kwargIsRef.push_back(isParamRef);
                                kwargIsConst.push_back(isParamConst);
                            } else {
                                params.push_back(paramTok);
                                paramIsRef.push_back(isParamRef);
                                paramIsConst.push_back(isParamConst);
                            }
                        }

                        std::shared_ptr<Expr> pType = nullptr;
                        if (match({ TokenType::COLON })) {
                            pType = std::shared_ptr<Expr>(ternary().release());
                        }
                        if (!isRest) {
                            if (inKwOnly) kwargTypes.push_back(std::move(pType));
                            else paramTypes.push_back(std::move(pType));
                        }

                        if (match({ TokenType::ASSIGN })) {
                            if (isRest) throw std::runtime_error("Parser Error: Rest/kwargs parameter cannot have a default value.");
                            auto defExpr = ternary();
                            if (inKwOnly) kwargDefaultExprs.push_back(std::shared_ptr<Expr>(defExpr.release()));
                            else defaultExprs.push_back(std::shared_ptr<Expr>(defExpr.release()));
                        } else {
                            if (!isRest) {
                                if (inKwOnly) kwargDefaultExprs.push_back(nullptr);
                                else defaultExprs.push_back(nullptr);
                            }
                        }

                        if (isDestruct) {
                            auto rhs = std::make_unique<Variable>(paramTok);
                            destructStmts.push_back(std::make_unique<DestructAssign>(std::move(patNode), std::move(rhs), false, false, false, isParamConst));
                        }
                        // , 或 ; 分隔（; 进入仅关键字区，只允许一次）
                        if (match({ TokenType::COMMA })) continue;
                        if (match({ TokenType::SEMICOLON })) {
                            if (inKwOnly) throw std::runtime_error("Parser Error: Only one ';' allowed in parameter list.");
                            inKwOnly = true;
                            if (check(TokenType::RPAREN)) break;
                            continue;
                        }
                        break;
                    }
                }
                consume(TokenType::RPAREN, "Parser Error: Expect ')' after method parameters.");

                std::shared_ptr<Expr> retType = nullptr;
                while (match({ TokenType::NEWLINE })) {}
                if (match({ TokenType::RIGHT_ARROW })) {
                    retType = std::shared_ptr<Expr>(ternary().release());
                }
                while (match({ TokenType::NEWLINE })) {}

                consume(TokenType::ASSIGN, "Parser Error: Expect '=' after method signature.");

                int bodyStart = current;
                auto body = check(TokenType::LBRACE) ? parseBlock() : assignment();
                int bodyEnd = current;

                std::string rawBody;
                for (int i = bodyStart; i < bodyEnd; ++i) {
                    if (tokens[i].type == TokenType::NEWLINE) continue;
                    if (tokens[i].type == TokenType::STRING) rawBody += "\"" + tokens[i].lexeme + "\"";
                    else rawBody += tokens[i].lexeme;
                    if (i < bodyEnd - 1 && tokens[i + 1].type != TokenType::NEWLINE) rawBody += " ";
                }

                std::shared_ptr<Expr> finalBody;
                if (!destructStmts.empty()) {
                    destructStmts.push_back(std::move(body));
                    finalBody = std::make_shared<Block>(std::move(destructStmts));
                }
                else {
                    finalBody = std::shared_ptr<Expr>(body.release());
                }

                value = std::make_unique<LambdaExpr>(
                    memberName.lexeme,
                    std::move(params),
                    std::move(paramIsRef),
                    std::move(paramIsConst),
                    std::move(defaultExprs),
                    restName,
                    std::move(paramTypes),
                    std::move(retType),
                    std::move(rawBody),
                    std::move(finalBody),
                    kwargParams, kwargIsRef, kwargIsConst, kwargDefaultExprs, kwargTypes, kwargsName
                );
            }

            if (isStatic) {
                staticProperties.push_back({ memberName, std::move(value), isLocal, isConst });
            } else {
                instanceProperties.push_back({ memberName, std::move(value), isLocal, isConst });
            }

            if (!check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                throw std::runtime_error("Parser Error: Expect newline or ';' after class member definition.");
            }
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after class body.");
        auto classExpr = std::make_unique<ClassDefExpr>(name, std::move(superClassExpr), std::move(staticProperties), std::move(instanceProperties));
        if (isNamed) {
            return std::make_unique<Assign>(name, std::move(classExpr), false, false, false, false);
        }
        return classExpr;
    }

    std::unique_ptr<Expr> Parser::parseFString(const std::string& raw) {
        std::vector<std::string> literals;
        std::vector<std::unique_ptr<Expr>> exprs;
        std::vector<std::string> specs;

        std::string currentLit;
        size_t i = 0;

        while (i < raw.size()) {
            if (raw[i] == '{') {
                i++; // skip opening {

                // ★ 提取到匹配的 } 之间的原始内容
                std::string exprStr;
                int depth = 1;
                bool inStr = false;
                while (i < raw.size() && depth > 0) {
                    char c = raw[i];
                    if (inStr) {
                        if (c == '\\' && i + 1 < raw.size()) { exprStr += c; i++; c = raw[i]; }
                        else if (c == '"') inStr = false;
                        exprStr += c; i++;
                    }
                    else {
                        if (c == '"') { inStr = true; exprStr += c; i++; }
                        else if (c == '{') { depth++; exprStr += c; i++; }
                        else if (c == '}') { depth--; if (depth > 0) exprStr += c; i++; }
                        else { exprStr += c; i++; }
                    }
                }

                // ★ 分离格式说明符：查找顶层 '::'（双冒号，彻底无歧义）
                std::string spec;
                {
                    int pd = 0, bd = 0, brd = 0;
                    bool s = false;
                    int sepPos = -1;
                    for (int j = 0; j < static_cast<int>(exprStr.size()) - 1; j++) {
                        char c = exprStr[j];
                        if (s) {
                            if (c == '\\' && j + 1 < static_cast<int>(exprStr.size())) j++;
                            else if (c == '"') s = false;
                        }
                        else {
                            if (c == '"')      s = true;
                            else if (c == '(') pd++;
                            else if (c == ')') pd--;
                            else if (c == '[') bd++;
                            else if (c == ']') bd--;
                            else if (c == '{') brd++;
                            else if (c == '}') brd--;
                            else if (c == ':' && exprStr[j + 1] == ':' &&
                                pd == 0 && bd == 0 && brd == 0) {
                                sepPos = j;
                                break;  // 找到第一个顶层 :: 即停止
                            }
                        }
                    }
                    if (sepPos >= 0) {
                        spec = exprStr.substr(sepPos + 2);  // 跳过 ::
                        exprStr = exprStr.substr(0, sepPos);
                    }
                }

                // ★ 保存前置文本段
                literals.push_back(currentLit);
                currentLit.clear();

                // ★ 子词法分析 + 子语法分析
                Lexer subLexer(exprStr, sourceFile);
                auto subTokens = subLexer.tokenize();
                Parser subParser(subTokens, sourceFile);
                auto exprAst = subParser.parse();

                exprs.push_back(std::move(exprAst));
                specs.push_back(spec);
            }
            else {
                currentLit += raw[i++];
            }
        }
        // ★ 尾部文本段
        literals.push_back(currentLit);

        return std::make_unique<FStringExpr>(
            std::move(literals), std::move(exprs), std::move(specs));
    }

    std::vector<CompClause> Parser::parseCompClauses() {
        std::vector<CompClause> clauses;
        while (match({ TokenType::FOR })) {
            consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'for' in comprehension.");
            match({ TokenType::LOCAL }); // ★ 允许并忽略可选的 local 关键字

            auto pat = parsePrimaryPattern();
            consume(TokenType::IN, "Parser Error: Expect 'in' after pattern in comprehension.");
            auto iterable = expression();
            clauses.emplace_back(std::move(pat), std::shared_ptr<Expr>(iterable.release()));
            consume(TokenType::RPAREN, "Parser Error: Expect ')' after for-in iterable in comprehension.");

            // ★ 可选的 if 过滤条件 (可以有多个)
            while (match({ TokenType::IF })) {
                consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'if' in comprehension.");
                auto cond = expression();
                consume(TokenType::RPAREN, "Parser Error: Expect ')' after if condition in comprehension.");
                clauses.back().conditions.push_back(std::shared_ptr<Expr>(cond.release()));
            }
        }
        return clauses;
    }

    std::unique_ptr<Expr> Parser::parseComp(std::unique_ptr<Expr> valueExpr, bool forceList) {
        auto clauses = parseCompClauses();
        consume(TokenType::RBRACKET, "Parser Error: Expect ']' after comprehension.");
        if (forceList) {
            return std::make_unique<ListCompExpr>(std::move(valueExpr), std::move(clauses));
        }
        return std::make_unique<MatrixCompExpr>(std::move(valueExpr), std::move(clauses));
    }

    std::unique_ptr<Expr> Parser::parseSetLiteral() {
        consume(TokenType::LBRACE, "Parser Error: Expect '{' after '@'.");

        std::vector<std::unique_ptr<Expr>> elements;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::NEWLINE })) {}  // ★ 跳过前导换行
            if (check(TokenType::RBRACE)) break;

            auto expr = assignment();

            // ★ 检测集合推导式：@{expr for x in ...}
            if (elements.empty() && check(TokenType::FOR)) {
                auto clauses = parseCompClauses();
                consume(TokenType::RBRACE, "Parser Error: Expect '}' after set comprehension.");
                return std::make_unique<SetCompExpr>(std::move(expr), std::move(clauses));
            }

            elements.push_back(std::move(expr));

            if (!match({ TokenType::COMMA })) {
                while (match({ TokenType::NEWLINE })) {}
                break;
            }
            while (match({ TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break; // 允许尾随逗号
        }

        while (match({ TokenType::NEWLINE })) {}
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after set literal.");
        return std::make_unique<SetLiteral>(std::move(elements));
    }

    std::unique_ptr<Expr> Parser::parseDictLiteral() {
        consume(TokenType::LBRACE, "Parser Error: Expect '{'.");

        std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> entries;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::NEWLINE })) {}  // ★ 跳过前导换行
            if (check(TokenType::RBRACE)) break;

            std::unique_ptr<Expr> key, value;

            // 1. 尝试将第一个元素当成可能的标识符或常数提取出来
            int savedPos = current;
            
            bool isSimpleId = check(TokenType::IDENTIFIER);
            Token maybeIdTok = peek(); // 暂存这个可能的名字
            
            bool isRest = false;
            if (match({TokenType::ELLIPSIS})) {
                isRest = true;
                key = nullptr;
                Token opTok = previous();
                value = std::make_unique<Unary>(opTok, assignment());
            } else {
                if (isSimpleId) {
                    advance(); // 吞掉这个标识符
                    int tempPos = current;
                    while (tempPos < static_cast<int>(tokens.size()) && tokens[tempPos].type == TokenType::NEWLINE) tempPos++;
                    if (tempPos < static_cast<int>(tokens.size()) && 
                        (tokens[tempPos].type == TokenType::COLON || tokens[tempPos].type == TokenType::COMMA || tokens[tempPos].type == TokenType::RBRACE)) {
                        // 确实是简写或普通标识符 key
                    } else {
                        isSimpleId = false;
                        current = savedPos; // 回退
                    }
                }
                
                if (!isSimpleId) {
                    key = ternary(); // 它不是简单的标识符，按常规表达式读取
                }
            }

            // 2. 核心分发：看看接下来是不是冒号
            while (match({ TokenType::NEWLINE })) {} // 跳过中间可能的换行

            bool hasColon = false;
            if (isRest) {
                // 已经处理好了
            }
            else if (match({ TokenType::COLON })) {
                hasColon = true;
                // ★ 它是标准的 "key: value" 模式
                if (isSimpleId) {
                    // 把刚才吞掉的标识符转为字符串常数作为 key
                    key = std::make_unique<Literal>(maybeIdTok.lexeme, true);
                }
                value = assignment();
            }
            else if (isSimpleId) {
                // ★ 它是简写的 "{ name }" 模式（没遇到冒号！）
                // 1. 把它名字作为字符串当 Key
                // 2. 把它作为一个对同名局域变量的读取当 Value
                key = std::make_unique<Literal>(maybeIdTok.lexeme, true);
                value = std::make_unique<Variable>(maybeIdTok);
            }
            else {
                throw std::runtime_error("Parser Error: Expect ':' after dict key.");
            }

            // ★ 检测字典推导式：{k: v for x in ...}
            if (entries.empty() && !isRest && check(TokenType::FOR)) {
                if (!hasColon) {
                    throw std::runtime_error("Parser Error: Dict comprehension requires 'key: value' format.");
                }
                if (isSimpleId) {
                    // 在推导式中，标识符键应作为变量表达式求值，而不是字符串字面量
                    key = std::make_unique<Variable>(maybeIdTok);
                }
                auto clauses = parseCompClauses();
                consume(TokenType::RBRACE, "Parser Error: Expect '}' after dict comprehension.");
                return std::make_unique<DictCompExpr>(std::move(key), std::move(value), std::move(clauses));
            }

            // 保存这一对 entry
            entries.push_back({ std::move(key), std::move(value) });

            // 3. 处理分隔符（逗号或换行）
            if (!match({ TokenType::COMMA })) {
                while (match({ TokenType::NEWLINE })) {}
                break; // 如果既不是逗号也不是换行（比如碰到了 }），准备退出
            }
            while (match({ TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break; // 允许尾随逗号 {a, b,}
        }

        while (match({ TokenType::NEWLINE })) {}  // ★ 最终 } 前的换行
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after dict literal.");
        return std::make_unique<DictLiteral>(std::move(entries));
    }

    // ---- 辅助函数 (不变) ----
    Token Parser::consume(TokenType type, const std::string& message) { if (check(type)) return advance(); throw std::runtime_error(message); }

} // namespace jc
