#include "Lexer.h"  // ★ f-string 子解析需要
#include "Parser.h"
#include <filesystem>

namespace jc {

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
            auto right = logicalOr();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
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
        auto expr = bitwiseOr();  // ★ 从 comparison 改为 bitwiseOr
        while (match({ TokenType::AND_AND })) {
            Token op = previous();
            auto right = bitwiseOr(); // ★
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
        auto expr = comparison(); // ★ 衔接原来的 comparison
        while (match({ TokenType::BIT_AND })) {
            Token op = previous();
            auto right = comparison();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parse() {
        try {
            std::vector<std::unique_ptr<Expr>> stmts;
            while (!isAtEnd()) {
                while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
                if (isAtEnd()) break;
                stmts.push_back(expression());
                if (!isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                    throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
                }
                while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
            }
            if (stmts.empty()) return std::make_unique<Literal>("0");
            if (stmts.size() == 1) return std::move(stmts[0]);
            return std::make_unique<Block>(std::move(stmts));
        }
        catch (const std::exception& e) {
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
        if (check(TokenType::IDENTIFIER) &&
            current + 1 < static_cast<int>(tokens.size()) &&
            tokens[current + 1].type == TokenType::LPAREN) {

            int peekPos = current + 2;
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
                    if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::IDENTIFIER) peekPos++;
                }
                while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) peekPos++;

                // 如果结尾是等号，那它 100% 就是一个正经的函数定义！
                if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::ASSIGN) {
                    Token funcName = advance(); // 吞掉函数名
                    consume(TokenType::LPAREN, "");

                    std::vector<Token> params;
                    std::vector<bool> paramIsRef;
                    std::vector<std::shared_ptr<Expr>> defaultExprs;
                    std::vector<std::string> paramTypes;  // ★
                    bool hasRestParam = false;

                    std::vector<std::unique_ptr<Expr>> destructStmts;
                    int destructCounter = 0;

                    if (!check(TokenType::RPAREN)) {
                        do {
                            while (match({ TokenType::NEWLINE })) {}
                            if (hasRestParam) throw std::runtime_error("Parser Error: Rest parameter must be last.");

                            bool isParamRef = match({ TokenType::REF });

                            // 1. 变长参数 ...args
                            if (match({ TokenType::ELLIPSIS })) {
                                if (isParamRef) throw std::runtime_error("Parser Error: Rest parameter cannot be ref.");
                                params.push_back(consume(TokenType::IDENTIFIER, "Expect parameter name."));
                                paramIsRef.push_back(false);
                                paramTypes.push_back(""); // 变长暂不强校验类型
                                defaultExprs.push_back(nullptr);
                                hasRestParam = true;
                                continue;
                            }

                            // 2. 解构参数 {a, b} 或 [a, b]
                            if (check(TokenType::LBRACE) || check(TokenType::LBRACKET)) {
                                if (isParamRef) throw std::runtime_error("Destructured parameter cannot be ref.");
                                bool isDict = check(TokenType::LBRACE);
                                auto patNode = parsePrimaryPattern();

                                std::string phName = "<param_destruct>_" + std::to_string(destructCounter++);
                                Token phTok(TokenType::IDENTIFIER, phName, funcName.line);
                                params.push_back(phTok);
                                paramIsRef.push_back(false);
                                paramTypes.push_back(isDict ? "dict" : "list"); // ★ 自动加上硬性类型约束！
                                
                                if (match({ TokenType::ASSIGN })) {
                                    defaultExprs.push_back(std::shared_ptr<Expr>(ternary().release()));
                                } else {
                                    defaultExprs.push_back(nullptr);
                                }

                                auto rhs = std::make_unique<Variable>(phTok);
                                destructStmts.push_back(std::make_unique<DestructAssign>(std::move(patNode), std::move(rhs)));
                                continue;
                            }

                            // 3. 通规变量参数 x : int = 10
                            Token paramName = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name.");
                            params.push_back(paramName);
                            paramIsRef.push_back(isParamRef);

                            std::string pType = "";
                            if (match({ TokenType::COLON })) {
                                pType = consume(TokenType::IDENTIFIER, "Parser Error: Expect type after ':'.").lexeme;
                            }
                            paramTypes.push_back(pType); // ★ 存入参数的类型

                            if (match({ TokenType::ASSIGN })) {
                                defaultExprs.push_back(std::shared_ptr<Expr>(ternary().release()));
                            }
                            else {
                                defaultExprs.push_back(nullptr);
                            }
                        } while (match({ TokenType::COMMA }));
                    }
                    consume(TokenType::RPAREN, "Parser Error: Expect ')' after parameters.");

                    // ★ 解析返回类型 -> int
                    std::string retType = "";
                    while (match({ TokenType::NEWLINE })) {}
                    if (match({ TokenType::RIGHT_ARROW })) {
                        retType = consume(TokenType::IDENTIFIER, "Parser Error: Expect return type after '->'.").lexeme;
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
                        funcName.lexeme, params, paramIsRef, defaultExprs, hasRestParam,
                        paramTypes, retType, rawBodyStr, std::move(finalBody)
                    );

                    return std::make_unique<Assign>(funcName, std::move(lambda), isRef, isState, isLocal, isConst);
                }
            }
        }

        // ★ 新增：直接拦截解构赋值！彻底分离 Pattern 和 Expr 的解析！
        if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
            int peekPos = current;
            int depth = 0;
            while (peekPos < static_cast<int>(tokens.size())) {
                TokenType t = tokens[peekPos].type;
                if (t == TokenType::LBRACKET || t == TokenType::LBRACE || t == TokenType::LPAREN) depth++;
                else if (t == TokenType::RBRACKET || t == TokenType::RBRACE || t == TokenType::RPAREN) depth--;
                peekPos++;
                if (depth == 0) break;
            }
            while (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::NEWLINE) peekPos++;
            if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::ASSIGN) {
                // 确认是解构赋值，直接解析为纯正的 Pattern！
                auto pat = parsePrimaryPattern();
                consume(TokenType::ASSIGN, "Parser Error: Expect '=' after destructuring pattern.");
                auto value = assignment();
                return std::make_unique<DestructAssign>(std::move(pat), std::move(value), isRef, isState, isLocal, isConst);
            }
        }

        auto expr = ternary();

        // 处理复合赋值 +=, -= 等...
        if (match({ TokenType::PLUS_ASSIGN, TokenType::MINUS_ASSIGN,
                    TokenType::STAR_ASSIGN, TokenType::SLASH_ASSIGN,
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
                return std::make_unique<DotAssign>(std::move(dotExpr->object), std::move(dotExpr->field), std::move(value));
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
                    return std::make_unique<IndexAssign>(varExpr->name, std::move(chain), std::move(value));
                }
                else {
                    return std::make_unique<IndexAssign>(std::move(currentIA->object), std::move(chain), std::move(value));
                }
            }

            if (auto* varExpr = dynamic_cast<Variable*>(expr.get())) {
                return std::make_unique<Assign>(varExpr->name, std::move(value), isRef, isState, isLocal, isConst);
            }

            // ★ （旧的 Call 拦截已经被上面顶端安全取代，这里删去原来的 Call if 分支即可！）

            throw std::runtime_error("Parser Error: Invalid assignment target at '" + equals.lexeme + "'.");
        }

        if (isLocal || isRef || isState || isConst) {
            if (auto* var = dynamic_cast<Variable*>(expr.get())) {
                if (isLocal) expr = std::make_unique<LocalDecl>(var->name, isConst);
                else if (isRef) expr = std::make_unique<RefDecl>(var->name, isConst);
                else if (isState) expr = std::make_unique<StateDecl>(var->name, isConst);
                else if (isConst) expr = std::make_unique<ConstDecl>(var->name);
            } else if (auto* assign = dynamic_cast<Assign*>(expr.get())) {
                assign->isLocal = isLocal;
                assign->isRef = isRef;
                assign->isState = isState;
                assign->isConst = isConst;
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

        return expr;
    }

    std::unique_ptr<Expr> Parser::comparison() {
        auto expr = shift();
        if (match({ TokenType::EQUAL, TokenType::BANG_EQUAL,
                    TokenType::LESS, TokenType::LESS_EQUAL,
                    TokenType::GREATER, TokenType::GREATER_EQUAL,
                    TokenType::IN })) {
            Token op = previous();
            auto right = shift();
            
            if (check(TokenType::EQUAL) || check(TokenType::BANG_EQUAL) ||
                check(TokenType::LESS) || check(TokenType::LESS_EQUAL) ||
                check(TokenType::GREATER) || check(TokenType::GREATER_EQUAL) ||
                check(TokenType::IN)) {
                
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
                               TokenType::IN })) {
                    Token nextOp = previous();
                    auto nextRight = shift();
                    
                    if (check(TokenType::EQUAL) || check(TokenType::BANG_EQUAL) ||
                        check(TokenType::LESS) || check(TokenType::LESS_EQUAL) ||
                        check(TokenType::GREATER) || check(TokenType::GREATER_EQUAL) ||
                        check(TokenType::IN)) {
                        
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
        while (match({ TokenType::STAR, TokenType::SLASH, TokenType::PERCENT, TokenType::BACKSLASH })) {
            Token op = previous();
            auto right = unary();
            expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::unary() {
        if (match({ TokenType::PLUS, TokenType::MINUS, TokenType::BANG , TokenType::TILDE, TokenType::ELLIPSIS })) {  // ★ 加 BANG, TILDE
            Token op = previous();
            auto right = unary();
            return std::make_unique<Unary>(op, std::move(right));
        }
        return power();
    }

    std::unique_ptr<Expr> Parser::power() {
        auto expr = call();
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
                Token field = consume(TokenType::IDENTIFIER,
                    "Parser Error: Expect field/method name after '.'.");

                if (match({ TokenType::LPAREN })) {
                    std::vector<std::unique_ptr<Expr>> args;
                    while (match({ TokenType::NEWLINE })) {}
                    if (!check(TokenType::RPAREN)) {
                        do {
                            while (match({ TokenType::NEWLINE })) {}
                            args.push_back(assignment()); // ★ 降级调用，保护函数参数的逗号
                            while (match({ TokenType::NEWLINE })) {}
                        } while (match({ TokenType::COMMA }));
                    }
                    while (match({ TokenType::NEWLINE })) {}
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
                                Token phTok(TokenType::IDENTIFIER, "<ph>_" + std::to_string(phCount++), var->name.line);
                                phParams.push_back(phTok);
                                phDefaults.push_back(nullptr);
                                arg = std::make_unique<Variable>(phTok);
                            }
                        }
                    }
                    std::unique_ptr<Expr> methodNode = std::make_unique<MethodCallExpr>(std::move(expr), field, std::move(args));

                    if (isPartial) {
                        std::vector<bool> phIsRef(phParams.size(), false);
                        expr = std::make_unique<LambdaExpr>(
                            "<partial_method>", std::move(phParams), std::move(phIsRef), std::move(phDefaults), false,
                            std::vector<std::string>(phParams.size(), ""), "",  // ★★★ 补上: 空参数类型数组，空返回类型
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
                    do {
                        while (match({ TokenType::NEWLINE })) {}
                        args.push_back(assignment());
                        while (match({ TokenType::NEWLINE })) {}
                    } while (match({ TokenType::COMMA }));
                }
                while (match({ TokenType::NEWLINE })) {}
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
                            Token phTok(TokenType::IDENTIFIER, "<ph>_" + std::to_string(phCount++), var->name.line);
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
                    expr = std::make_unique<LambdaExpr>(
                        "<partial_apply>", std::move(phParams), std::move(phIsRef), std::move(phDefaults), false,
                        std::vector<std::string>(phParams.size(), ""), "", // ★★★ 补上: 空参数类型数组，空返回类型
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

                if (match({ TokenType::COMMA })) {
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
        std::vector<std::unique_ptr<Expr>> stmts;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
            if (check(TokenType::RBRACE)) break;
            stmts.push_back(expression());
            if (!check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
            }
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}  // ★
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after block.");
        return std::make_unique<Block>(std::move(stmts));
    }

    // =================================================================
// ★ 升级版：支持单行语句并智能避开字典字面量陷阱
// =================================================================
    std::unique_ptr<Expr> Parser::parseStatementOrBlock() {
        while (match({ TokenType::NEWLINE })) {}

        if (check(TokenType::LBRACE)) {
            // ★ 我们必须在这里进行智能探测！
            int peekPos = current + 1;
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
            bool foundAssign = false; // ★ 新增

            // 深度扫描，跳过嵌套的 [], {}, ()
            while (scanPos < static_cast<int>(tokens.size())) {
                TokenType t = tokens[scanPos].type;
                if (t == TokenType::LBRACE || t == TokenType::LBRACKET || t == TokenType::LPAREN) {
                    depth++;
                } else if (t == TokenType::RBRACE || t == TokenType::RBRACKET || t == TokenType::RPAREN) {
                    if (depth == 0) break;
                    depth--;
                } else if (depth == 0) {
                    if (t == TokenType::QUESTION) {
                        ternaryDepth++;
                    } else if (t == TokenType::COLON) {
                        if (ternaryDepth > 0) {
                            ternaryDepth--;
                        } else {
                            foundColon = true;
                        }
                    } else if (t == TokenType::SEMICOLON) {
                        foundSemicolon = true;
                    } else if (t == TokenType::COMMA) {
                        foundComma = true;
                    } else if (t == TokenType::NEWLINE) {
                        foundNewline = true;
                    } else if (t == TokenType::ASSIGN) { // ★ 嗅探 =
                        foundAssign = true;
                    }
                }
                scanPos++;
            }

            // ★ 移除 isDestructLValue 的逻辑，因为解构赋值在 assignment() 中处理
            if (foundColon) {
                isDict = true;
            } else if (foundSemicolon) {
                isDict = false;
            } else if (foundComma) {
                isDict = true;
            } else if (foundNewline) {
                isDict = false;
            } else if (foundAssign) { // ★ {a = 10} 作为 Block
                isDict = false;
            } else { // 单行无逗号无冒号无等号 （如 {a}），优先作为字典简写
                isDict = true;
            }

            // 如果它是字典，必须让 expression() 层级去调用 primary() 将其当做右值解析！
            if (!isDict) {
                return parseBlock(); // 确定是普通代码块，安全进入！
            }
        }

        // 走到这里说明它没有大括号，或者它是被识别为单行字典字面量的大括号
        // 统统包装为安全的单句 Block 以封锁词法作用域
        std::vector<std::unique_ptr<Expr>> stmts;
        stmts.push_back(expression());
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

        // ★ 解构 for-in: for ([a, b, ...] in iterable) or for ({a, b} in iterable)
        if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
            int savedPos2 = current;
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
            current = savedPos2;
        }

        // ★ 推测性检查：for (IDENTIFIER in ...) 还是 for (init; cond; update)
        if (check(TokenType::IDENTIFIER) &&
            current + 1 < static_cast<int>(tokens.size()) &&
            tokens[current + 1].type == TokenType::IN) {
            Token varName = advance(); // 消费标识符
            advance();                  // 消费 'in'
            auto iterable = expression();
            consume(TokenType::RPAREN, "Parser Error: Expect ')' after for-in iterable.");
            auto body = parseStatementOrBlock();
            return std::make_unique<ForInExpr>(std::move(varName), std::move(iterable), std::move(body), isLocal, isConst);
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

    std::unique_ptr<Expr> Parser::forInExpr(Token varName) {
        auto iterable = expression();
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after for-in iterable.");
        auto body = parseStatementOrBlock();
        return std::make_unique<ForInExpr>(std::move(varName), std::move(iterable), std::move(body));
    }

    // =================================================================
    // primary — ★ 新增 if / while / for / block / break / continue
    // =================================================================
    std::unique_ptr<Expr> Parser::primary() {
        // ★ 统一拦截：任何关键字后面紧跟 = 都是误用
        auto isKeyword = [](TokenType t) {
            return t == TokenType::IF || t == TokenType::ELSE ||
                t == TokenType::WHILE || t == TokenType::FOR ||
                t == TokenType::BREAK || t == TokenType::CONTINUE ||
                t == TokenType::RETURN || t == TokenType::LOCAL ||
                t == TokenType::CONST || t == TokenType::DELETE ||
                t == TokenType::IN ||
                t == TokenType::THROW || t == TokenType::TRY ||  // ★
                t == TokenType::CATCH || t == TokenType::REF ||   // ★
                t == TokenType::STATE ||                          // ★
                t == TokenType::IMPORT || t == TokenType::SWITCH ||  // ★
                t == TokenType::CASE || t == TokenType::DEFAULT ||
                t == TokenType::MATCH ||
                t == TokenType::SUPER || t == TokenType::CLASS || t == TokenType::SELF ||
                t == TokenType::TRUE_KW || t == TokenType::FALSE_KW || t == TokenType::NONE_KW ||
                t == TokenType::NAMESPACE; // ★ 新增
            };
        if (isKeyword(peek().type) && current + 1 < static_cast<int>(tokens.size())
            && tokens[current + 1].type == TokenType::ASSIGN) {
            throw std::runtime_error("Syntax Error: '" + peek().lexeme +
                "' is a reserved keyword and cannot be used as a variable name.");
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
        if (match({ TokenType::CLASS }))    return classDefExpr();
        if (match({ TokenType::NAMESPACE })) return namespaceExpr();
        if (match({ TokenType::IF }))       return ifExpr();
        if (match({ TokenType::WHILE }))    return whileExpr();
        if (match({ TokenType::FOR }))      return forExpr();
        if (match({ TokenType::BREAK }))    return std::make_unique<BreakExpr>();
        if (match({ TokenType::CONTINUE })) return std::make_unique<ContinueExpr>();
        if (match({ TokenType::RETURN })) {
            std::unique_ptr<Expr> value = nullptr;
            if (!check(TokenType::RBRACE) &&
                !check(TokenType::SEMICOLON) &&
                !check(TokenType::NEWLINE) &&      // ★ 新增
                !check(TokenType::END_OF_FILE)) {
                value = assignment();  // ★ 降级：防止逗号被误吞
            }
            return std::make_unique<ReturnExpr>(std::move(value));
        }
        if (match({ TokenType::THROW })) {
            auto value = assignment();  // ★ 降级：防止逗号被误吞
            return std::make_unique<ThrowExpr>(std::move(value));
        }
        if (match({ TokenType::TRY })) {
            auto tryBody = parseStatementOrBlock();
            while (match({ TokenType::NEWLINE })) {}  // ★ 跳过 } 和 catch 之间的换行
            consume(TokenType::CATCH, "Parser Error: Expect 'catch' after try block.");
            consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'catch'.");
            Token catchName = consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name in catch.");
            consume(TokenType::RPAREN, "Parser Error: Expect ')' after catch variable.");
            auto catchBody = parseStatementOrBlock();
            return std::make_unique<TryCatchExpr>(std::move(tryBody), catchName, std::move(catchBody));
        }
        if (match({ TokenType::IMPORT })) {
            if (check(TokenType::IDENTIFIER)) {
                Token nameTok = advance();
                auto pathExpr = std::make_unique<Literal>(nameTok.lexeme, true);
                auto importExpr = std::make_unique<ImportExpr>(std::move(pathExpr));
                return std::make_unique<Assign>(nameTok, std::move(importExpr));
            } else {
                auto path = assignment();  // ★ 降级：防止逗号被误吞
                return std::make_unique<ImportExpr>(std::move(path));
            }
        }
        if (match({ TokenType::SWITCH })) return switchExpr();
        if (match({ TokenType::MATCH })) return matchExpr();
        if (match({ TokenType::DELETE })) {
            std::vector<Token> names;
            names.push_back(consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name after 'delete'."));
            while (match({ TokenType::COMMA })) {
                names.push_back(consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name after ','."));
            }
            return std::make_unique<DeleteExpr>(std::move(names));
        }

        // ★ 裸块 { ... } 或字典字面量 { key: value, ... }
        if (check(TokenType::LBRACE)) {
            // ★ 修改 lookahead：跳过 { 后的 NEWLINE 再检查是否为 dict
            int peekPos = current + 1;
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
            bool foundAssign = false; // ★ 新增

            // 深度扫描，跳过嵌套的 [], {}, ()
            while (scanPos < static_cast<int>(tokens.size())) {
                TokenType t = tokens[scanPos].type;
                if (t == TokenType::LBRACE || t == TokenType::LBRACKET || t == TokenType::LPAREN) {
                    depth++;
                } else if (t == TokenType::RBRACE || t == TokenType::RBRACKET || t == TokenType::RPAREN) {
                    if (depth == 0) break;
                    depth--;
                } else if (depth == 0) {
                    if (t == TokenType::QUESTION) {
                        ternaryDepth++;
                    } else if (t == TokenType::COLON) {
                        if (ternaryDepth > 0) {
                            ternaryDepth--;
                        } else {
                            foundColon = true;
                        }
                    } else if (t == TokenType::SEMICOLON) {
                        foundSemicolon = true;
                    } else if (t == TokenType::COMMA) {
                        foundComma = true;
                    } else if (t == TokenType::NEWLINE) {
                        foundNewline = true;
                    } else if (t == TokenType::ASSIGN) { // ★ 嗅探 =
                        foundAssign = true;
                    }
                }
                scanPos++;
            }

            // ★ 移除 isDestructLValue 的逻辑，因为解构赋值在 assignment() 中处理
            if (foundColon) {
                isDict = true;
            } else if (foundSemicolon) {
                isDict = false;
            } else if (foundComma) {
                isDict = true;
            } else if (foundNewline) {
                isDict = false;
            } else if (foundAssign) { // ★ {a = 10} 作为 Block
                isDict = false;
            } else { // 单行无逗号无冒号无等号 （如 {a}），优先作为字典简写
                isDict = true;
            }

            if (isDict) {
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
                    if (peekPos < static_cast<int>(tokens.size()) && tokens[peekPos].type == TokenType::IDENTIFIER) peekPos++;
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
                std::vector<std::shared_ptr<Expr>> lambdaDefaults;
                std::vector<std::string> paramTypes; // ★
                bool hasRestParam = false;

                if (!check(TokenType::RPAREN)) {
                    do {
                        if (hasRestParam) throw std::runtime_error("Parser Error: Rest parameter must be last.");

                        bool isRef = match({ TokenType::REF });

                        if (match({ TokenType::ELLIPSIS })) {
                            if (isRef) throw std::runtime_error("Parser Error: Rest parameter cannot be ref.");
                            Token param = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name after '...'.");
                            lambdaParams.push_back(param);
                            lambdaParamIsRef.push_back(false);
                            paramTypes.push_back("");
                            lambdaDefaults.push_back(nullptr);
                            hasRestParam = true;
                            continue;
                        }

                        Token param = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name.");
                        lambdaParams.push_back(param);
                        lambdaParamIsRef.push_back(isRef);

                        std::string pType = "";
                        if (match({ TokenType::COLON })) pType = consume(TokenType::IDENTIFIER, "Expect type after ':'.").lexeme;
                        paramTypes.push_back(pType);

                        if (match({ TokenType::ASSIGN })) {
                            auto defExpr = ternary();
                            lambdaDefaults.push_back(std::shared_ptr<Expr>(defExpr.release()));
                        }
                        else lambdaDefaults.push_back(nullptr);
                    } while (match({ TokenType::COMMA }));
                }

                consume(TokenType::RPAREN, "Parser Error: Expect ')' after lambda parameters.");

                // ★ 捕获返回值
                std::string retType = "";
                while (match({ TokenType::NEWLINE })) {}
                if (match({ TokenType::RIGHT_ARROW })) retType = consume(TokenType::IDENTIFIER, "Parser Error: Expect return type.").lexeme;
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

                return std::make_unique<LambdaExpr>(
                    "<lambda>",
                    std::move(lambdaParams),
                    std::move(lambdaParamIsRef),
                    std::move(lambdaDefaults),
                    hasRestParam,
                    paramTypes, retType,  // ★
                    rawBody,
                    std::shared_ptr<Expr>(body.release()));
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
            if (!check(TokenType::LBRACKET)) {
                throw std::runtime_error("Parser Error: Expect '[' or '{' after '@'.");
            }
            forceList = true;
        }

        if (match({ TokenType::LBRACKET })) {
            std::vector<std::vector<std::unique_ptr<Expr>>> matrixElements;
            std::vector<std::unique_ptr<Expr>> currentRow;

            if (!check(TokenType::RBRACKET)) {
                // ★ 先解析第一个元素
                currentRow.push_back(assignment());

                // ★ 检测列表推导式：[expr for x in ...]
                if (check(TokenType::FOR)) {
                    auto valueExpr = std::move(currentRow[0]);
                    auto comp = parseListComp(std::move(valueExpr));
                    static_cast<ListCompExpr*>(comp.get())->forceList = forceList;
                    return comp;
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
            return std::make_unique<MatrixNode>(std::move(matrixElements), forceList);
        }
        throw std::runtime_error("Parser Error: Expect expression at '" + peek().lexeme + "'.");
    }

    std::unique_ptr<Expr> Parser::switchExpr() {
        consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'switch'.");
        auto subject = expression();
        consume(TokenType::RPAREN, "Parser Error: Expect ')' after switch expression.");
        while (match({ TokenType::NEWLINE })) {}
        
        consume(TokenType::LBRACE, "Parser Error: Expect '{' to open switch body.");

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
                    stmts.push_back(expression());
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
                    stmts.push_back(expression());
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
            Token name = consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name or '_' after '...'.");
            return std::make_unique<RestPattern>(name, mod, isConst);
        }
        
        if (hasMod) {
            Token name = consume(TokenType::IDENTIFIER, "Parser Error: Expect variable name after modifier.");
            return std::make_unique<VariablePattern>(name, mod, isConst);
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
                                restRow = std::make_unique<RestPattern>(name, elemMod, elemConst);
                                isMatrix = true;
                            } else {
                                restCol = std::make_unique<RestPattern>(name, elemMod, elemConst);
                            }
                            continue;
                        } else {
                            currentRow.push_back(std::make_unique<RestPattern>(name, elemMod, elemConst));
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
                    rest = std::make_unique<RestPattern>(name, elemMod, elemConst);
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
            return std::make_unique<VariablePattern>(var->name, mod, isConst);
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
            int peekPos = current + 1;
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
            bool foundAssign = false; // ★ 新增

            while (scanPos < static_cast<int>(tokens.size())) {
                TokenType t = tokens[scanPos].type;
                if (t == TokenType::LBRACE || t == TokenType::LBRACKET || t == TokenType::LPAREN) {
                    depth++;
                } else if (t == TokenType::RBRACE || t == TokenType::RBRACKET || t == TokenType::RPAREN) {
                    if (depth == 0) break;
                    depth--;
                } else if (depth == 0) {
                    if (t == TokenType::QUESTION) {
                        ternaryDepth++;
                    } else if (t == TokenType::COLON) {
                        if (ternaryDepth > 0) {
                            ternaryDepth--;
                        } else {
                            foundColon = true;
                        }
                    } else if (t == TokenType::SEMICOLON) {
                        foundSemicolon = true;
                    } else if (t == TokenType::COMMA) {
                        foundComma = true;
                    } else if (t == TokenType::NEWLINE) {
                        foundNewline = true;
                    } else if (t == TokenType::ASSIGN) { // ★ 嗅探 =
                        foundAssign = true;
                    }
                }
                scanPos++;
            }

            // ★ 移除 isDestructLValue 的逻辑，因为解构赋值在 assignment() 中处理
            if (foundColon) {
                isDict = true;
            } else if (foundSemicolon) {
                isDict = false;
            } else if (foundComma) {
                isDict = true;
            } else if (foundNewline) {
                isDict = false;
            } else if (foundAssign) { // ★ {a = 10} 作为 Block
                isDict = false;
            } else { // 单行无逗号无冒号无等号 （如 {a}），优先作为字典简写
                isDict = true;
            }

            if (!isDict) {
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
        Token name = consume(TokenType::IDENTIFIER, "Parser Error: Expect namespace name.");
        consume(TokenType::LBRACE, "Parser Error: Expect '{' after namespace name.");
        std::vector<std::unique_ptr<Expr>> stmts;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break;
            stmts.push_back(expression());
            if (!check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                throw std::runtime_error("Parser Error: Expect newline or ';' after statement.");
            }
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after namespace body.");
        return std::make_unique<NamespaceDecl>(name, std::make_unique<Block>(std::move(stmts)));
    }

    std::unique_ptr<Expr> Parser::classDefExpr() {
        Token name = consume(TokenType::IDENTIFIER, "Parser Error: Expect class name after 'class'.");

        while (match({ TokenType::NEWLINE })) {}

        std::string superClassName;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "extends") {
            advance();
            Token superToken = consume(TokenType::IDENTIFIER, "Parser Error: Expect parent class name after 'extends'.");
            superClassName = superToken.lexeme;
        }

        while (match({ TokenType::NEWLINE })) {}
        consume(TokenType::LBRACE, "Parser Error: Expect '{' after class name.");

        std::vector<ClassDefExpr::MethodDef> methods;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
            if (check(TokenType::RBRACE)) break;

            Token methodName = consume(TokenType::IDENTIFIER, "Parser Error: Expect method name.");
            consume(TokenType::LPAREN, "Parser Error: Expect '(' after method name.");

            std::vector<Token> params;
            std::vector<bool> paramIsRef;
            std::vector<std::shared_ptr<Expr>> defaultExprs;
            std::vector<std::string> paramTypes; // ★
            bool hasRestParam = false;

            if (!check(TokenType::RPAREN)) {
                do {
                    if (hasRestParam) throw std::runtime_error("Parser Error: Rest parameter must be last.");
                    bool isRef = false;
                    if (match({ TokenType::REF })) isRef = true;

                    if (match({ TokenType::ELLIPSIS })) {
                        if (isRef) throw std::runtime_error("Parser Error: Rest parameter cannot be passed by ref.");
                        params.push_back(consume(TokenType::IDENTIFIER, "Expect parameter name."));
                        paramIsRef.push_back(false);
                        paramTypes.push_back("");
                        defaultExprs.push_back(nullptr);
                        hasRestParam = true;
                        continue;
                    }

                    Token param = consume(TokenType::IDENTIFIER, "Parser Error: Expect parameter name.");
                    params.push_back(param);
                    paramIsRef.push_back(isRef);

                    std::string pType = "";
                    if (match({ TokenType::COLON })) pType = consume(TokenType::IDENTIFIER, "Expect type.").lexeme;
                    paramTypes.push_back(pType);

                    if (match({ TokenType::ASSIGN })) {
                        auto defExpr = ternary();
                        defaultExprs.push_back(std::shared_ptr<Expr>(defExpr.release()));
                    }
                    else defaultExprs.push_back(nullptr);
                } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RPAREN, "Parser Error: Expect ')' after method parameters.");

            // ★ 解析方法返回类型
            std::string retType = "";
            while (match({ TokenType::NEWLINE })) {}
            if (match({ TokenType::RIGHT_ARROW })) {
                retType = consume(TokenType::IDENTIFIER, "Parser Error: Expect return type after '->'.").lexeme;
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

            methods.push_back(ClassDefExpr::MethodDef{
                methodName,
                std::move(params),
                std::move(paramIsRef),
                std::move(defaultExprs),
                hasRestParam,
                paramTypes, retType, // ★ 加载进入结构体
                std::move(rawBody),
                std::shared_ptr<Expr>(body.release())
                });

            if (!check(TokenType::RBRACE) && !isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                throw std::runtime_error("Parser Error: Expect newline or ';' after method definition.");
            }
            while (match({ TokenType::SEMICOLON, TokenType::NEWLINE })) {}
        }
        consume(TokenType::RBRACE, "Parser Error: Expect '}' after class body.");
        return std::make_unique<ClassDefExpr>(name, std::move(superClassName), std::move(methods));
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

    std::unique_ptr<Expr> Parser::parseListComp(std::unique_ptr<Expr> valueExpr) {
        // 进入时：当前 token 指向 FOR，valueExpr 已被提取
        std::vector<ListCompExpr::CompClause> clauses;

        while (match({ TokenType::FOR })) {
            consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'for' in list comprehension.");
            match({ TokenType::LOCAL }); // ★ 允许并忽略可选的 local 关键字（推导式变量默认就是 local 的）

            // ★ 解构模式：for ([a, b] in ...) or for ({a, b} in ...)
            if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
                auto pat = parsePrimaryPattern();
                consume(TokenType::IN, "Parser Error: Expect 'in' after pattern in list comprehension.");
                auto iterable = expression();
                clauses.emplace_back(std::move(pat), std::shared_ptr<Expr>(iterable.release()));
            }
            // ★ 单变量模式：for (x in ...)
            else {
                Token varName = consume(TokenType::IDENTIFIER,
                    "Parser Error: Expect variable name after 'for' in list comprehension.");
                consume(TokenType::IN,
                    "Parser Error: Expect 'in' after variable in list comprehension.");
                auto iterable = expression();
                clauses.emplace_back(varName,
                    std::shared_ptr<Expr>(iterable.release()));
            }
            consume(TokenType::RPAREN, "Parser Error: Expect ')' after for-in iterable in list comprehension.");

            // ★ 可选的 if 过滤条件 (可以有多个)
            while (match({ TokenType::IF })) {
                consume(TokenType::LPAREN, "Parser Error: Expect '(' after 'if' in list comprehension.");
                auto cond = expression();
                consume(TokenType::RPAREN, "Parser Error: Expect ')' after if condition in list comprehension.");
                clauses.back().conditions.push_back(std::shared_ptr<Expr>(cond.release()));
            }
        }

        consume(TokenType::RBRACKET,
            "Parser Error: Expect ']' after list comprehension.");

        return std::make_unique<ListCompExpr>(
            std::move(valueExpr), std::move(clauses));
    }

    std::unique_ptr<Expr> Parser::parseSetLiteral() {
        consume(TokenType::LBRACE, "Parser Error: Expect '{' after '@'.");

        std::vector<std::unique_ptr<Expr>> elements;

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            while (match({ TokenType::NEWLINE })) {}  // ★ 跳过前导换行
            if (check(TokenType::RBRACE)) break;

            elements.push_back(assignment());

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

            if (isRest) {
                // 已经处理好了
            }
            else if (match({ TokenType::COLON })) {
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
