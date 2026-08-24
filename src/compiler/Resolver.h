#ifndef JC2_COMPILER_RESOLVER_H
#define JC2_COMPILER_RESOLVER_H

#include "../frontend/Expr.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <stdexcept>

namespace jc {

// 变量的作用域类型
enum class VarScope {
    Global,         // 全局变量
    Local,          // 局部变量 (Auto-local 或 显式 local)
    Upvalue,        // 闭包捕获的上值
    CapturedState,  // 闭包捕获的外部 state 变量
    State,          // 闭包私有状态 (state)
    RefParam        // 引用参数 (ref)
};

// 解析后的符号信息
struct ResolvedSym {
    VarScope scope = VarScope::Global;
    int index = -1;      // 局部变量槽位、上值索引等
    int depth = -1;      // 作用域深度
    bool isConst = false;
};

class Resolver : public ExprVisitor {
public:
    // 旁侧表 (Side Tables)：记录 AST 节点对应的符号解析结果
    std::unordered_map<Expr*, ResolvedSym> exprSymbols;
    std::unordered_map<Pattern*, ResolvedSym> patternSymbols;

    void resolve(Expr* expr);

    // --- ExprVisitor 接口 ---
    void visitBinary(Binary* expr) override;
    void visitUnary(Unary* expr) override;
    void visitLiteral(Literal* expr) override;
    void visitVariable(Variable* expr) override;
    void visitAssign(Assign* expr) override;
    void visitCall(Call* expr) override;
    void visitMatrixNode(MatrixNode* expr) override;
    void visitBlock(Block* expr) override;
    void visitIfExpr(IfExpr* expr) override;
    void visitWhileExpr(WhileExpr* expr) override;
    void visitForExpr(ForExpr* expr) override;
    void visitBreakExpr(BreakExpr* expr) override;
    void visitContinueExpr(ContinueExpr* expr) override;
    void visitReturnExpr(ReturnExpr* expr) override;
    void visitIndexAccess(IndexAccess* expr) override;
    void visitIndexAssign(IndexAssign* expr) override;
    void visitLocalDecl(LocalDecl* expr) override;
    void visitRefDecl(RefDecl* expr) override;
    void visitStateDecl(StateDecl* expr) override;
    void visitConstDecl(ConstDecl* expr) override;
    void visitDeleteExpr(DeleteExpr* expr) override;
    void visitCompoundAssign(CompoundAssign* expr) override;
    void visitLambdaExpr(LambdaExpr* expr) override;
    void visitInvokeExpr(InvokeExpr* expr) override;
    void visitForInExpr(ForInExpr* expr) override;
    void visitThrowExpr(ThrowExpr* expr) override;
    void visitTryCatchExpr(TryCatchExpr* expr) override;
    void visitImportExpr(ImportExpr* expr) override;
    void visitSwitchExpr(SwitchExpr* expr) override;
    void visitClassDefExpr(ClassDefExpr* expr) override;
    void visitNamespaceDecl(NamespaceDecl* expr) override;
    void visitEnumDefExpr(EnumDefExpr* expr) override;
    void visitDotAccess(DotAccess* expr) override;
    void visitDotAssign(DotAssign* expr) override;
    void visitMethodCallExpr(MethodCallExpr* expr) override;
    void visitSuperExpr(SuperExpr* expr) override;
    void visitSelfExpr(SelfExpr* expr) override;
    void visitContextKeywordExpr(ContextKeywordExpr* expr) override;
    void visitDestructAssign(DestructAssign* expr) override;
    void visitFStringExpr(FStringExpr* expr) override;
    void visitListCompExpr(ListCompExpr* expr) override;
    void visitSetCompExpr(SetCompExpr* expr) override;
    void visitDictCompExpr(DictCompExpr* expr) override;
    void visitDictLiteral(DictLiteral* expr) override;
    void visitSetLiteral(SetLiteral* expr) override;
    void visitSliceExpr(SliceExpr* expr) override;
    void visitSequenceExpr(SequenceExpr* expr) override;
    void visitMatchExpr(MatchExpr* expr) override;
    void visitGroupingExpr(GroupingExpr* expr) override;
    void visitMacroDefExpr(MacroDefExpr* expr) override;
    void visitMacroCallExpr(MacroCallExpr* expr) override;
    void visitQuoteExpr(QuoteExpr* expr) override;
    void visitUnquoteExpr(UnquoteExpr* expr) override;
    void visitExprAssign(ExprAssign* expr) override;
    void visitDeferExpr(DeferExpr* expr) override;
    void visitKeywordArgExpr(KeywordArgExpr* expr) override;
    void visitTypeAssertExpr(TypeAssertExpr* expr) override;

private:
    struct Scope {
        std::unordered_map<std::string, ResolvedSym> symbols;
        std::unordered_set<std::string> lexicalDecls;
        bool isFunctionScope = false;
        bool isNamespaceScope = false;
    };
    std::vector<Scope> scopes;
    std::unordered_set<void*> checkedDecls;

    void checkExplicitDecl(void* node, const std::string& name);
    void beginScope(bool isFunc = false, bool isNamespace = false);
    void endScope();
    void declareVariable(const std::string& name, VarScope scopeType, bool isConst, bool isExplicitLocal = false);
    ResolvedSym resolveName(const std::string& name);
    
    void hoistBlock(Block* block);
    void resolvePattern(Pattern* pat, bool isAssignment, ScopeModifier globalMod = ScopeModifier::None, bool globalConst = false, bool skipRedecl = false);
};

} // namespace jc

#endif // JC2_COMPILER_RESOLVER_H
