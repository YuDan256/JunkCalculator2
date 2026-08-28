#include "Formatter.h"
#include "../frontend/Lexer.h"
#include "../frontend/Parser.h"
#include "../frontend/Expr.h"
#include "../frontend/Token.h"
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>

namespace jc {

namespace {

// ============================================================================
// Token 分类辅助
// ============================================================================

bool isBinaryOp(TokenType t) {
    switch (t) {
    case TokenType::PLUS: case TokenType::MINUS: case TokenType::STAR:
    case TokenType::SLASH: case TokenType::CARET: case TokenType::BACKSLASH:
    case TokenType::PERCENT: case TokenType::TILDE_SLASH:
    case TokenType::SHIFT_LEFT: case TokenType::SHIFT_RIGHT:
    case TokenType::BIT_AND: case TokenType::BIT_OR: case TokenType::BIT_XOR:
        return true;
    default: return false;
    }
}

bool isAssignOp(TokenType t) {
    switch (t) {
    case TokenType::ASSIGN:
    case TokenType::PLUS_ASSIGN: case TokenType::MINUS_ASSIGN:
    case TokenType::STAR_ASSIGN: case TokenType::SLASH_ASSIGN:
    case TokenType::TILDE_SLASH_ASSIGN: case TokenType::PERCENT_ASSIGN:
    case TokenType::CARET_ASSIGN: case TokenType::BACKSLASH_ASSIGN:
    case TokenType::BIT_AND_ASSIGN: case TokenType::BIT_OR_ASSIGN:
    case TokenType::BIT_XOR_ASSIGN: case TokenType::SHIFT_LEFT_ASSIGN:
    case TokenType::SHIFT_RIGHT_ASSIGN:
        return true;
    default: return false;
    }
}

bool isCompareOp(TokenType t) {
    switch (t) {
    case TokenType::EQUAL: case TokenType::BANG_EQUAL:
    case TokenType::LESS: case TokenType::LESS_EQUAL:
    case TokenType::GREATER: case TokenType::GREATER_EQUAL:
    case TokenType::SUBSET:
        return true;
    default: return false;
    }
}

bool isLogicOp(TokenType t) {
    return t == TokenType::AND_AND || t == TokenType::OR_OR;
}

bool isOperator(TokenType t) {
    return isBinaryOp(t) || isAssignOp(t) || isCompareOp(t) || isLogicOp(t) ||
           t == TokenType::PIPE || t == TokenType::RIGHT_ARROW || t == TokenType::ARROW ||
           t == TokenType::IN || t == TokenType::IS || t == TokenType::QUESTION;
}

bool isKeywordLike(TokenType t) {
    switch (t) {
    case TokenType::IF: case TokenType::WHILE: case TokenType::FOR:
    case TokenType::RETURN: case TokenType::THROW: case TokenType::IMPORT:
    case TokenType::CONST: case TokenType::LOCAL: case TokenType::REF:
    case TokenType::STATE: case TokenType::STATIC: case TokenType::DELETE:
    case TokenType::IN: case TokenType::IS: case TokenType::AS:
    case TokenType::MATCH: case TokenType::SWITCH: case TokenType::CASE:
    case TokenType::DEFAULT: case TokenType::TRY: case TokenType::CATCH:
    case TokenType::ELSE: case TokenType::CLASS: case TokenType::EXTENDS:
    case TokenType::NAMESPACE: case TokenType::ENUM: case TokenType::DEFER:
    case TokenType::MACRO: case TokenType::SYNTAX: case TokenType::QUOTE:
        return true;
    default: return false;
    }
}

bool isWordLike(TokenType t) {
    return t == TokenType::IDENTIFIER || t == TokenType::NUMBER ||
           t == TokenType::STRING || t == TokenType::FSTRING ||
           t == TokenType::RSTRING || t == TokenType::IMAGINARY ||
           t == TokenType::TRUE_KW || t == TokenType::FALSE_KW ||
           t == TokenType::NONE_KW || isKeywordLike(t);
}

// 修饰符（文档要求排序：static -> const -> local/ref/state）
bool isModifierToken(TokenType t) {
    return t == TokenType::STATIC || t == TokenType::CONST ||
           t == TokenType::LOCAL || t == TokenType::REF || t == TokenType::STATE;
}
int modifierRank(TokenType t) {
    if (t == TokenType::STATIC) return 0;
    if (t == TokenType::CONST) return 1;
    return 2;
}

// cur 前是否需要空格（prevPos / unaryOpPos 由 AST Unary 节点提供一元运算符的权威信息）
bool needSpaceBefore(TokenType cur, TokenType prev, int prevPos, const std::set<int>& unaryOpPos) {
    // 一元前缀运算符后无空格：!x ~x -x $x @x ...x
    if (unaryOpPos.count(prevPos)) return false;
    // 紧贴型：闭括号、逗号、分号、点、冒号前无空格
    if (cur == TokenType::RPAREN || cur == TokenType::RBRACKET ||
        cur == TokenType::RBRACE || cur == TokenType::COMMA ||
        cur == TokenType::SEMICOLON || cur == TokenType::DOT ||
        cur == TokenType::COLON)
        return false;
    // 开括号（：关键字后空格（if/while/for/catch/return/throw/import 等），
    // 但 match/switch 是函数风格无空格（match(），函数名也无空格（f(）
    if (cur == TokenType::LPAREN) {
        if (prev == TokenType::MATCH || prev == TokenType::SWITCH) return false;
        return isKeywordLike(prev);
    }
    // 开括号 [：= / 运算符 / 关键字 后空格（= [），@ 后无空格（@[），标识符后无空格（a[）
    if (cur == TokenType::LBRACKET)
        return prev == TokenType::ASSIGN || isOperator(prev) || isKeywordLike(prev);
    // 花括号：前空格（block 或 dict），但 @{ 无空格
    if (cur == TokenType::LBRACE)
        return prev != TokenType::AT && prev != TokenType::DOLLAR;
    // 运算符：前空格
    if (isOperator(cur)) return true;
    // 单词类：前空格（prev 是单词类 / 闭括号 / 逗号 / 分号 / 冒号 / 运算符）
    return isWordLike(prev) || prev == TokenType::RPAREN ||
           prev == TokenType::RBRACKET || prev == TokenType::RBRACE ||
           prev == TokenType::COMMA || prev == TokenType::SEMICOLON ||
           prev == TokenType::COLON || isOperator(prev);
}

// ============================================================================
// BlockBraceCollector：遍历 AST，收集 block 花括号的 position
// ============================================================================
class BlockBraceCollector : public ExprVisitor {
public:
    std::set<int> bracePos;
    std::set<int> unaryOpPos;       // 一元运算符 position（后无空格，AST Unary 节点提供）
    std::set<int> caseColonPos;     // switch case 的 :（后换行）
    std::set<int> matchCommaPos;    // match 分支之间的 ,（后换行）
    std::set<int> caseKeywordPos;   // switch 内 case/default 关键字（前换行）
    std::vector<std::pair<int, int>> preserveRanges;  // 语法宏 body 原样保留范围
    std::vector<std::pair<int, int>> forceBraceRanges;  // if/else 链需强制加花括号的分支范围
    const std::vector<Token>& tokens;

    explicit BlockBraceCollector(const std::vector<Token>& t) : tokens(t) {}

    // 收集「无 Block 字段但仍是块体」的节点的外层花括号（class/enum/switch/match）
    void collectOuterBraces(int startPos, int endPos) {
        int open = -1, close = -1;
        for (const auto& tok : tokens) {
            if (tok.position < startPos) continue;
            if (tok.position >= endPos) break;
            if (tok.type == TokenType::LBRACE && open == -1) open = tok.position;
            if (tok.type == TokenType::RBRACE) close = tok.position;
        }
        if (open != -1 && close != -1) {
            bracePos.insert(open);
            bracePos.insert(close);
        }
    }

    void visitPattern(Pattern* p) {
        if (!p) return;
        if (auto* lp = dynamic_cast<LiteralPattern*>(p)) {
            if (lp->literal) lp->literal->accept(*this);
        } else if (auto* ep = dynamic_cast<ExprPattern*>(p)) {
            if (ep->expr) ep->expr->accept(*this);
        } else if (auto* dp = dynamic_cast<DynamicAssertPattern*>(p)) {
            if (dp->expr) dp->expr->accept(*this);
        } else if (auto* vp = dynamic_cast<VariablePattern*>(p)) {
            if (vp->typeHint) vp->typeHint->accept(*this);
        } else if (auto* rp = dynamic_cast<RestPattern*>(p)) {
            if (rp->typeHint) rp->typeHint->accept(*this);
        } else if (auto* lsp = dynamic_cast<ListPattern*>(p)) {
            for (auto& e : lsp->elements) visitPattern(e.get());
            if (lsp->rest) visitPattern(lsp->rest.get());
        } else if (auto* mp = dynamic_cast<MatrixPattern*>(p)) {
            for (auto& row : mp->rows) for (auto& e : row) visitPattern(e.get());
            if (mp->restRow) visitPattern(mp->restRow.get());
        } else if (auto* dpp = dynamic_cast<DictPattern*>(p)) {
            for (auto& e : dpp->entries) visitPattern(e.second.get());
            if (dpp->rest) visitPattern(dpp->rest.get());
        } else if (auto* dfp = dynamic_cast<DefaultPattern*>(p)) {
            visitPattern(dfp->inner.get());
            if (dfp->defaultExpr) dfp->defaultExpr->accept(*this);
        }
    }

    void visitClause(const CompClause& c) {
        visitPattern(c.pattern.get());
        if (c.iterable) c.iterable->accept(*this);
        for (auto& cond : c.conditions) if (cond) cond->accept(*this);
    }

    void visitBinary(Binary* e) override { if (e->left) e->left->accept(*this); if (e->right) e->right->accept(*this); }
    void visitUnary(Unary* e) override { unaryOpPos.insert(e->op.position); if (e->right) e->right->accept(*this); }
    void visitLiteral(Literal*) override {}
    void visitVariable(Variable*) override {}
    void visitAssign(Assign* e) override { if (e->value) e->value->accept(*this); if (e->typeHint) e->typeHint->accept(*this); }
    void visitCall(Call* e) override { for (auto& a : e->arguments) if (a) a->accept(*this); }
    void visitMatrixNode(MatrixNode* e) override { for (auto& row : e->elements) for (auto& el : row) if (el) el->accept(*this); }
    void visitListNode(ListNode* e) override { for (auto& row : e->elements) for (auto& el : row) if (el) el->accept(*this); }
    void visitBlock(Block* e) override {
        for (const auto& tok : tokens) {
            if (tok.position == e->startPos && tok.type == TokenType::LBRACE) {
                bracePos.insert(e->startPos);
                bracePos.insert(e->endPos - 1);
                break;
            }
        }
        for (auto& s : e->statements) if (s) s->accept(*this);
    }
    bool isBlockWithBraces(Expr* b) {
        if (auto* blk = dynamic_cast<Block*>(b)) {
            for (const auto& tok : tokens) {
                if (tok.position == blk->startPos && tok.type == TokenType::LBRACE) return true;
            }
        }
        return false;
    }

    void visitIfExpr(IfExpr* e) override {
        // 收集 if/else 链所有分支，任一用花括号则全链无花括号分支强制加花括号
        std::vector<Expr*> branches;
        IfExpr* cur = e;
        while (cur) {
            branches.push_back(cur->thenBranch.get());
            if (auto* elif = dynamic_cast<IfExpr*>(cur->elseBranch.get())) {
                cur = elif;
            } else {
                branches.push_back(cur->elseBranch.get());
                cur = nullptr;
            }
        }
        bool anyBraces = false;
        for (auto* b : branches) if (b && isBlockWithBraces(b)) { anyBraces = true; break; }
        if (anyBraces) {
            for (auto* b : branches) {
                if (b && b->startPos > 0 && b->endPos > b->startPos && !isBlockWithBraces(b)) {
                    forceBraceRanges.push_back({ b->startPos, b->endPos });
                }
            }
        }
        if (e->condition) e->condition->accept(*this);
        if (e->thenBranch) e->thenBranch->accept(*this);
        if (e->elseBranch) e->elseBranch->accept(*this);
    }
    void visitWhileExpr(WhileExpr* e) override { if (e->condition) e->condition->accept(*this); if (e->body) e->body->accept(*this); }
    void visitForExpr(ForExpr* e) override { if (e->initializer) e->initializer->accept(*this); if (e->condition) e->condition->accept(*this); if (e->update) e->update->accept(*this); if (e->body) e->body->accept(*this); }
    void visitBreakExpr(BreakExpr*) override {}
    void visitContinueExpr(ContinueExpr*) override {}
    void visitReturnExpr(ReturnExpr* e) override { if (e->value) e->value->accept(*this); }
    void visitIndexAccess(IndexAccess* e) override { if (e->object) e->object->accept(*this); for (auto& idx : e->indices) if (idx) idx->accept(*this); }
    void visitIndexAssign(IndexAssign* e) override { if (e->objectExpr) e->objectExpr->accept(*this); for (auto& row : e->indexChain) for (auto& idx : row) if (idx) idx->accept(*this); if (e->value) e->value->accept(*this); if (e->typeHint) e->typeHint->accept(*this); }
    void visitLocalDecl(LocalDecl* e) override { if (e->typeHint) e->typeHint->accept(*this); }
    void visitRefDecl(RefDecl* e) override { if (e->typeHint) e->typeHint->accept(*this); }
    void visitStateDecl(StateDecl* e) override { if (e->typeHint) e->typeHint->accept(*this); }
    void visitConstDecl(ConstDecl*) override {}
    void visitDeleteExpr(DeleteExpr*) override {}
    void visitCompoundAssign(CompoundAssign* e) override { if (e->target) e->target->accept(*this); if (e->value) e->value->accept(*this); }
    void visitLambdaExpr(LambdaExpr* e) override {
        for (auto& d : e->defaultExprs) if (d) d->accept(*this);
        for (auto& pt : e->paramTypes) if (pt) pt->accept(*this);
        if (e->returnType) e->returnType->accept(*this);
        for (auto& kd : e->kwargDefaultExprs) if (kd) kd->accept(*this);
        for (auto& kt : e->kwargTypes) if (kt) kt->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void visitInvokeExpr(InvokeExpr* e) override { if (e->callee) e->callee->accept(*this); for (auto& a : e->arguments) if (a) a->accept(*this); }
    void visitForInExpr(ForInExpr* e) override { visitPattern(e->pattern.get()); if (e->iterable) e->iterable->accept(*this); if (e->body) e->body->accept(*this); }
    void visitThrowExpr(ThrowExpr* e) override { if (e->value) e->value->accept(*this); }
    void visitTryCatchExpr(TryCatchExpr* e) override { if (e->tryBody) e->tryBody->accept(*this); visitPattern(e->catchPattern.get()); if (e->catchBody) e->catchBody->accept(*this); }
    void visitImportExpr(ImportExpr* e) override { if (e->path) e->path->accept(*this); }
    void visitSwitchExpr(SwitchExpr* e) override {
        collectOuterBraces(e->startPos, e->endPos);
        if (e->subject) e->subject->accept(*this);
        // 收集 switch 内所有 case/default 关键字位置
        int open = -1, close = -1;
        for (const auto& tok : tokens) {
            if (tok.position >= e->startPos && tok.position < e->endPos) {
                if (tok.type == TokenType::LBRACE && open == -1) open = tok.position;
                if (tok.type == TokenType::RBRACE) close = tok.position;
            }
        }
        for (const auto& tok : tokens) {
            if (tok.position > open && tok.position < close &&
                (tok.type == TokenType::CASE || tok.type == TokenType::DEFAULT)) {
                caseKeywordPos.insert(tok.position);
            }
        }
        for (auto& c : e->cases) {
            for (auto& v : c.first) if (v) v->accept(*this);
            if (c.second) {
                int bodyStart = c.second->startPos;
                int lastEnd = -1;
                for (auto& v : c.first) if (v) lastEnd = std::max(lastEnd, v->endPos);
                for (const auto& tok : tokens) {
                    if (tok.position >= lastEnd && tok.position < bodyStart && tok.type == TokenType::COLON) {
                        caseColonPos.insert(tok.position);
                        break;
                    }
                }
                c.second->accept(*this);
            }
        }
        if (e->defaultBody) {
            for (int j = static_cast<int>(tokens.size()) - 1; j >= 0; --j) {
                if (tokens[j].position < e->defaultBody->startPos && tokens[j].type == TokenType::COLON) {
                    caseColonPos.insert(tokens[j].position);
                    break;
                }
            }
            e->defaultBody->accept(*this);
        }
    }
    void visitClassDefExpr(ClassDefExpr* e) override {
        collectOuterBraces(e->startPos, e->endPos);
        if (e->superClassExpr) e->superClassExpr->accept(*this);
        for (auto& p : e->staticProperties) if (p.value) p.value->accept(*this);
        for (auto& p : e->instanceProperties) if (p.value) p.value->accept(*this);
    }
    void visitNamespaceDecl(NamespaceDecl* e) override { if (e->body) e->body->accept(*this); }
    void visitEnumDefExpr(EnumDefExpr* e) override { collectOuterBraces(e->startPos, e->endPos); for (auto& m : e->members) if (m.second) m.second->accept(*this); }
    void visitDotAccess(DotAccess* e) override { if (e->object) e->object->accept(*this); }
    void visitDotAssign(DotAssign* e) override { if (e->object) e->object->accept(*this); if (e->value) e->value->accept(*this); if (e->typeHint) e->typeHint->accept(*this); }
    void visitMethodCallExpr(MethodCallExpr* e) override { if (e->object) e->object->accept(*this); for (auto& a : e->arguments) if (a) a->accept(*this); }
    void visitSuperExpr(SuperExpr*) override {}
    void visitSelfExpr(SelfExpr*) override {}
    void visitContextKeywordExpr(ContextKeywordExpr*) override {}
    void visitDestructAssign(DestructAssign* e) override { visitPattern(e->pattern.get()); if (e->value) e->value->accept(*this); }
    void visitFStringExpr(FStringExpr* e) override { for (auto& x : e->exprs) if (x) x->accept(*this); }
    void visitMatrixCompExpr(MatrixCompExpr* e) override { if (e->valueExpr) e->valueExpr->accept(*this); for (auto& c : e->clauses) visitClause(c); }
    void visitListCompExpr(ListCompExpr* e) override { if (e->valueExpr) e->valueExpr->accept(*this); for (auto& c : e->clauses) visitClause(c); }
    void visitSetCompExpr(SetCompExpr* e) override { if (e->valueExpr) e->valueExpr->accept(*this); for (auto& c : e->clauses) visitClause(c); }
    void visitDictCompExpr(DictCompExpr* e) override { if (e->keyExpr) e->keyExpr->accept(*this); if (e->valueExpr) e->valueExpr->accept(*this); for (auto& c : e->clauses) visitClause(c); }
    void visitDictLiteral(DictLiteral* e) override { for (auto& en : e->entries) { if (en.first) en.first->accept(*this); if (en.second) en.second->accept(*this); } }
    void visitSetLiteral(SetLiteral* e) override { for (auto& el : e->elements) if (el) el->accept(*this); }
    void visitSliceExpr(SliceExpr* e) override { if (e->start) e->start->accept(*this); if (e->end) e->end->accept(*this); if (e->step) e->step->accept(*this); }
    void visitSequenceExpr(SequenceExpr* e) override { for (auto& x : e->expressions) if (x) x->accept(*this); }
    void visitMatchExpr(MatchExpr* e) override {
        collectOuterBraces(e->startPos, e->endPos);
        if (e->subject) e->subject->accept(*this);
        for (size_t bi = 0; bi < e->branches.size(); ++bi) {
            auto& b = e->branches[bi];
            for (auto& p : b.patterns) visitPattern(p.get());
            if (b.guard) b.guard->accept(*this);
            if (b.body) b.body->accept(*this);
            // 收集分支之间的 ,
            if (bi + 1 < e->branches.size()) {
                int thisEnd = b.body ? b.body->endPos : -1;
                int nextStart = -1;
                if (!e->branches[bi + 1].patterns.empty() && e->branches[bi + 1].patterns[0]) {
                    nextStart = e->branches[bi + 1].patterns[0]->startPos;
                }
                if (thisEnd != -1 && nextStart != -1) {
                    for (const auto& tok : tokens) {
                        if (tok.position >= thisEnd && tok.position < nextStart && tok.type == TokenType::COMMA) {
                            matchCommaPos.insert(tok.position);
                            break;
                        }
                    }
                }
            }
        }
    }
    void visitGroupingExpr(GroupingExpr* e) override { if (e->expression) e->expression->accept(*this); }
    void visitMacroDefExpr(MacroDefExpr* e) override { if (e->body) e->body->accept(*this); }
    void visitMacroCallExpr(MacroCallExpr* e) override {
        // 宏调用的 { ... } 代码块参数是 DSL 代码，原样保留
        for (const auto& tok : tokens) {
            if (tok.position >= e->startPos && tok.position < e->endPos && tok.type == TokenType::LBRACE) {
                preserveRanges.push_back({ tok.position, e->endPos });
                break;
            }
        }
        for (auto& a : e->arguments) if (a) a->accept(*this);
    }
    void visitQuoteExpr(QuoteExpr* e) override { if (e->body) e->body->accept(*this); }
    void visitUnquoteExpr(UnquoteExpr* e) override { if (e->expr) e->expr->accept(*this); }
    void visitExprAssign(ExprAssign* e) override { if (e->target) e->target->accept(*this); if (e->value) e->value->accept(*this); }
    void visitDeferExpr(DeferExpr* e) override { if (e->body) e->body->accept(*this); }
    void visitKeywordArgExpr(KeywordArgExpr* e) override { if (e->value) e->value->accept(*this); }
    void visitSpreadExpr(SpreadExpr* e) override { if (e->value) e->value->accept(*this); }
    void visitTypeAssertExpr(TypeAssertExpr* e) override { if (e->value) e->value->accept(*this); if (e->typeHint) e->typeHint->accept(*this); }
};

// ============================================================================
// TokenPrinter：遍历 token 流，排版
// ============================================================================
std::string printTokens(const std::string& source, const std::vector<Token>& tokens, const std::set<int>& bracePos,
                        const std::set<int>& unaryOpPos, const std::set<int>& caseColonPos,
                        const std::set<int>& matchCommaPos, const std::set<int>& caseKeywordPos,
                        const std::vector<std::pair<int, int>>& preserveRanges,
                        const std::vector<std::pair<int, int>>& forceBraceRanges) {
    std::string out;
    int indent = 0;
    int pendingNL = 0;          // 0 无换行 / 1 单换行 / 2 空一行
    bool lineHasContent = false;
    bool inCaseBody = false;
    int forceBraceEnd = -1;
    std::vector<int> indentStack;  // 多行字面量缩进栈
    TokenType prev = TokenType::NEWLINE;
    int prevPos = -1;
    auto setPrev = [&](TokenType t, int p) { prev = t; prevPos = p; };
    // token 之后、下一个非空白字符之前是否有换行符（手动换行）
    auto newlineAfter = [&](const Token& tok) {
        int end = tok.position + static_cast<int>(tok.lexeme.length());
        for (int p = end; p < static_cast<int>(source.length()); ++p) {
            if (source[p] == '\n') return true;
            if (source[p] != ' ' && source[p] != '\t' && source[p] != '\r') return false;
        }
        return false;
    };
    auto currentLineLength = [&]() {
        size_t lastNL = out.find_last_of('\n');
        return lastNL == std::string::npos ? out.size() : out.size() - lastNL - 1;
    };
    // 引号包裹的 token（STRING/RSTRING/FSTRING）lexeme 不含引号，从 source 恢复原文
    auto rawText = [&](const Token& tok) -> std::string {
        // 反引号标识符 `...`：position 是内容开始（` 在 position-1），从 source 恢复含反引号的原文
        if (tok.type == TokenType::IDENTIFIER && tok.position > 0 && source[tok.position - 1] == '`') {
            int start = tok.position - 1;
            int q = tok.position;
            while (q < static_cast<int>(source.length()) && source[q] != '`' && source[q] != '\n') q++;
            if (q < static_cast<int>(source.length()) && source[q] == '`') q++;
            return source.substr(start, q - start);
        }
        if (tok.type != TokenType::STRING && tok.type != TokenType::FSTRING && tok.type != TokenType::RSTRING) {
            return tok.lexeme;
        }
        int p = tok.position;
        if (tok.type == TokenType::FSTRING || tok.type == TokenType::RSTRING) {
            if (p < static_cast<int>(source.length()) && (source[p] == 'f' || source[p] == 'r')) p++;
        }
        if (p < static_cast<int>(source.length()) && (source[p] == '"' || source[p] == '\'')) {
            char quote = source[p];
            int q = p + 1;
            // 自定义分隔符 raw string：r"TAG(content)TAG"
            if (tok.type == TokenType::RSTRING) {
                int tagEnd = q;
                while (tagEnd < static_cast<int>(source.length()) && std::isalnum(static_cast<unsigned char>(source[tagEnd]))) tagEnd++;
                if (tagEnd < static_cast<int>(source.length()) && source[tagEnd] == '(') {
                    std::string delimiter = source.substr(q, tagEnd - q);
                    std::string endMarker = ")" + delimiter + quote;
                    int m = tagEnd + 1;
                    while (m < static_cast<int>(source.length())) {
                        if (source[m] == ')' && m + static_cast<int>(endMarker.length()) <= static_cast<int>(source.length()) &&
                            source.substr(m, endMarker.length()) == endMarker) {
                            m += static_cast<int>(endMarker.length());
                            break;
                        }
                        m++;
                    }
                    return source.substr(tok.position, m - tok.position);
                }
            }
            bool multi = (q + 1 < static_cast<int>(source.length()) && source[q] == quote && source[q + 1] == quote);
            while (q < static_cast<int>(source.length())) {
                if (tok.type != TokenType::RSTRING && source[q] == '\\') { q += 2; continue; }
                if (multi) {
                    if (q + 2 < static_cast<int>(source.length()) && source[q] == quote && source[q + 1] == quote && source[q + 2] == quote) { q += 3; break; }
                } else {
                    if (source[q] == quote) { q++; break; }
                }
                q++;
            }
            return source.substr(tok.position, q - tok.position);
        }
        return tok.lexeme;
    };

    auto emitNewlines = [&](int count) {
        while (!out.empty() && out.back() == ' ') out.pop_back();
        for (int k = 0; k < count; ++k) out += '\n';
        for (int k = 0; k < indent; ++k) out += "    ";
        lineHasContent = false;
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];

        // ★ if/else 链强制花括号：退出上一个强制分支
        if (forceBraceEnd > 0 && t.position >= forceBraceEnd) {
            indent = std::max(indent - 1, 0);
            pendingNL = std::max(pendingNL, 1);
            emitNewlines(pendingNL);
            pendingNL = 0;
            out += '}';
            lineHasContent = true;
            setPrev(TokenType::RBRACE, t.position);
            forceBraceEnd = -1;
        }

        // ★ if/else 链强制花括号：进入一个无花括号分支
        if (forceBraceEnd <= 0) {
            for (const auto& r : forceBraceRanges) {
                if (t.position == r.first) {
                    if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                    else if (lineHasContent) out += ' ';
                    out += '{';
                    lineHasContent = true;
                    setPrev(TokenType::LBRACE, t.position);
                    pendingNL = std::max(pendingNL, 1);
                    indent++;
                    forceBraceEnd = r.second;
                    break;
                }
            }
        }

        // ★ 语法宏 body 原样保留（不格式化）
        {
            bool inPreserve = false;
            int preserveEnd = -1;
            for (const auto& r : preserveRanges) {
                if (t.position >= r.first && t.position < r.second) {
                    inPreserve = true;
                    preserveEnd = r.second;
                    break;
                }
            }
            if (inPreserve) {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                else if (lineHasContent && needSpaceBefore(TokenType::LBRACE, prev, prevPos, unaryOpPos)) out += ' ';
                out += source.substr(t.position, preserveEnd - t.position);
                lineHasContent = true;
                while (i + 1 < tokens.size() && tokens[i + 1].position < preserveEnd) ++i;
                continue;
            }
        }

        // ★ 修饰符排序：static -> const -> local/ref/state
        if (isModifierToken(t.type)) {
            std::vector<int> ranks;
            std::vector<std::string> lexemes;
            std::vector<TokenType> types;
            size_t j = i;
            while (j < tokens.size() && isModifierToken(tokens[j].type)) {
                ranks.push_back(modifierRank(tokens[j].type));
                lexemes.push_back(tokens[j].lexeme);
                types.push_back(tokens[j].type);
                j++;
            }
            std::vector<size_t> order(ranks.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ranks[a] < ranks[b]; });
            for (size_t k = 0; k < order.size(); ++k) {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                else if (lineHasContent && (k > 0 || needSpaceBefore(types[order[k]], prev, prevPos, unaryOpPos))) out += ' ';
                out += lexemes[order[k]];
                lineHasContent = true;
            }
            setPrev(types[order.back()], t.position);
            i = j - 1;
            continue;
        }

        switch (t.type) {
        case TokenType::NEWLINE:
            pendingNL = std::min(pendingNL + 1, 2);
            break;

        case TokenType::COMMENT: {
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            else if (lineHasContent) out += ' ';
            out += t.lexeme;
            lineHasContent = true;
            setPrev(TokenType::COMMENT, t.position);
            if (newlineAfter(t)) pendingNL = std::max(pendingNL, 1);
            break;
        }

        case TokenType::LBRACE: {
            if (bracePos.count(t.position)) {
                // block {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                else if (lineHasContent && needSpaceBefore(TokenType::LBRACE, prev, prevPos, unaryOpPos)) out += ' ';
                out += '{';
                lineHasContent = true;
                setPrev(TokenType::LBRACE, t.position);
                // 空 block：下一个显著 token 是 }
                bool emptyBlock = false;
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (tokens[j].type == TokenType::COMMENT || tokens[j].type == TokenType::NEWLINE) continue;
                    if (tokens[j].type == TokenType::RBRACE && bracePos.count(tokens[j].position)) emptyBlock = true;
                    break;
                }
                if (!emptyBlock) { pendingNL = std::max(pendingNL, 1); indent++; }
                else { out += ' '; }
            } else {
                // dict/set {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                else if (lineHasContent && needSpaceBefore(TokenType::LBRACE, prev, prevPos, unaryOpPos)) out += ' ';
                out += '{';
                lineHasContent = true;
                setPrev(TokenType::LBRACE, t.position);
                // 多行字面量：{ 后换行则缩进
                if (newlineAfter(t)) { pendingNL = std::max(pendingNL, 1); indent++; indentStack.push_back(1); }
            }
            break;
        }

        case TokenType::RBRACE: {
            if (bracePos.count(t.position)) {
                // block }
                if (inCaseBody) { indent = std::max(indent - 1, 0); inCaseBody = false; }
                indent = std::max(indent - 1, 0);
                pendingNL = std::max(pendingNL, 1);
                emitNewlines(pendingNL);
                pendingNL = 0;
                out += '}';
                lineHasContent = true;
                setPrev(TokenType::RBRACE, t.position);
            } else {
                // dict/set }
                if (!indentStack.empty()) { indent -= indentStack.back(); indentStack.pop_back(); pendingNL = std::max(pendingNL, 1); }
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                out += '}';
                lineHasContent = true;
                setPrev(TokenType::RBRACE, t.position);
            }
            break;
        }

        case TokenType::COLON:
            if (caseColonPos.count(t.position)) {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                out += ':';
                lineHasContent = true;
                setPrev(TokenType::COLON, t.position);
                pendingNL = std::max(pendingNL, 1);
                indent++;
                inCaseBody = true;
            } else {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                out += ':';
                lineHasContent = true;
                setPrev(TokenType::COLON, t.position);
            }
            break;

        case TokenType::COMMA:
            if (matchCommaPos.count(t.position)) {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                out += ',';
                lineHasContent = true;
                setPrev(TokenType::COMMA, t.position);
                pendingNL = std::max(pendingNL, 1);
            } else {
                if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
                out += ',';
                lineHasContent = true;
                setPrev(TokenType::COMMA, t.position);
                if (newlineAfter(t)) pendingNL = std::max(pendingNL, 1);
            }
            break;

        case TokenType::SEMICOLON:
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            out += ';';
            lineHasContent = true;
            setPrev(TokenType::SEMICOLON, t.position);
            if (newlineAfter(t)) pendingNL = std::max(pendingNL, 1);
            break;

        case TokenType::LBRACKET:
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            else if (lineHasContent && needSpaceBefore(TokenType::LBRACKET, prev, prevPos, unaryOpPos)) out += ' ';
            out += '[';
            lineHasContent = true;
            setPrev(TokenType::LBRACKET, t.position);
            if (newlineAfter(t)) { pendingNL = std::max(pendingNL, 1); indent++; indentStack.push_back(1); }
            break;

        case TokenType::RBRACKET:
            if (!indentStack.empty()) { indent -= indentStack.back(); indentStack.pop_back(); pendingNL = std::max(pendingNL, 1); }
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            out += ']';
            lineHasContent = true;
            setPrev(TokenType::RBRACKET, t.position);
            break;

        case TokenType::AND_AND:
        case TokenType::OR_OR:
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            else if (lineHasContent) out += ' ';
            out += t.lexeme;
            lineHasContent = true;
            setPrev(t.type, t.position);
            if (currentLineLength() >= 80) pendingNL = std::max(pendingNL, 1);
            break;

        case TokenType::PIPE:
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            else if (lineHasContent) out += ' ';
            out += "|>";
            lineHasContent = true;
            setPrev(TokenType::PIPE, t.position);
            if (newlineAfter(t) || currentLineLength() >= 80) pendingNL = std::max(pendingNL, 1);
            break;

        case TokenType::CASE:
        case TokenType::DEFAULT:
            if (caseKeywordPos.count(t.position)) {
                if (inCaseBody) { indent = std::max(indent - 1, 0); inCaseBody = false; }
                pendingNL = std::max(pendingNL, 1);
            }
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            else if (lineHasContent && needSpaceBefore(t.type, prev, prevPos, unaryOpPos)) out += ' ';
            out += t.lexeme;
            lineHasContent = true;
            setPrev(t.type, t.position);
            break;

        default: {
            if (pendingNL > 0) { emitNewlines(pendingNL); pendingNL = 0; }
            else if (lineHasContent && needSpaceBefore(t.type, prev, prevPos, unaryOpPos)) out += ' ';
            out += rawText(t);
            lineHasContent = true;
            setPrev(t.type, t.position);
            break;
        }
        }
    }

    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

} // namespace

std::string Formatter::format(const std::string& source) {
    Lexer lexer(source);
    lexer.keepComments = true;
    auto tokens = lexer.tokenize();

    try {
        Lexer parserLexer(source);
        auto parserTokens = parserLexer.tokenize();
        Parser parser(parserTokens);
        parser.disableMacroExpansion = true;
        auto ast = parser.parse();

        BlockBraceCollector collector(tokens);
        ast->accept(collector);

        std::string out = printTokens(source, tokens, collector.bracePos, collector.unaryOpPos, collector.caseColonPos, collector.matchCommaPos, collector.caseKeywordPos, collector.preserveRanges, collector.forceBraceRanges);
        if (!out.empty() && out.back() != '\n') out += '\n';

        // ★ 保留 # 指令：shebang 固定在最上行，其余 directive 随后
        std::string shebangText;
        std::string directivesText;
        for (const auto& d : lexer.directives) {
            if (d.name == "!") { if (shebangText.empty()) shebangText = "#!" + d.args + "\n"; }
            else directivesText += "#" + d.name + d.args + "\n";
        }
        return shebangText + directivesText + out;
    } catch (...) {
        return source;
    }
}

} // namespace jc
