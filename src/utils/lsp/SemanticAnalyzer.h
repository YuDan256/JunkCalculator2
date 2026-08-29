#ifndef JC2_LSP_SEMANTIC_ANALYZER_H
#define JC2_LSP_SEMANTIC_ANALYZER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "LspProtocol.h"
#include "Workspace.h"
#include "../../frontend/Expr.h"
#include "../../frontend/Token.h"

namespace jc {
namespace lsp {

    enum class SymbolKind {
        Variable,
        Function,
        Class,
        Parameter,
        Property,
        Namespace
    };

    struct Symbol {
        std::string name;
        SymbolKind kind;
        Range definitionRange;
        std::string typeHint;
        std::string docstring;
    };

    enum class ScopeKind { Global, Function, Block, Class, Namespace };

    struct Scope {
        ScopeKind kind = ScopeKind::Block;
        Scope* parent = nullptr;
        std::vector<std::unique_ptr<Scope>> children;
        std::unordered_map<std::string, std::shared_ptr<Symbol>> symbols;
        Range scopeRange;
    };

    class SemanticAnalyzer : public ExprVisitor {
    public:
        SemanticAnalyzer(Document* doc, const std::vector<Token>& tokens);
        
        void analyze(Expr* root);

        // 查询给定位置的符号定义（用于 Hover 和 Go to Definition）
        std::shared_ptr<Symbol> getSymbolAt(const Position& pos);
        
        // 获取给定位置所有可见的符号（用于自动补全）
        std::vector<std::shared_ptr<Symbol>> getVisibleSymbolsAt(const Position& pos);

        // 获取文档符号树（用于大纲视图）
        std::vector<DocumentSymbol> getDocumentSymbols();

    private:
        void buildDocumentSymbols(Scope* scope, std::vector<DocumentSymbol>& outSymbols);
        Scope* getScopeAt(Scope* scope, const Position& pos);

        struct Reference {
            Range range;
            std::shared_ptr<Symbol> symbol;
        };
        std::vector<Reference> references;

        Document* doc;
        const std::vector<Token>& tokens;
        
        std::unique_ptr<Scope> globalScope;
        Scope* currentScope = nullptr;

        // 作用域管理
        void enterScope(const Range& range, ScopeKind kind = ScopeKind::Block);
        void leaveScope();
        void declareSymbol(const std::string& name, SymbolKind kind, int startPos, int endPos, const std::string& typeHint = "", bool isLocal = true);
        std::shared_ptr<Symbol> resolveSymbol(const std::string& name);
        void declarePattern(Pattern* pat, bool isLocal, bool isConst);
        void hoistBlock(Block* expr);

        // 提取文档注释
        std::string extractDocstring(int nodeStartPos);

        // ExprVisitor 接口实现 (部分核心节点)
        void visitBlock(Block* expr) override;
        void visitAssign(Assign* expr) override;
        void visitVariable(Variable* expr) override;
        void visitLambdaExpr(LambdaExpr* expr) override;
        void visitClassDefExpr(ClassDefExpr* expr) override;
        
        // 其他节点暂时提供空实现或简单遍历
        void visitBinary(Binary* expr) override;
        void visitUnary(Unary* expr) override;
        void visitLiteral(Literal* /*expr*/) override {}
        void visitCall(Call* expr) override;
        void visitMatrixNode(MatrixNode* expr) override;
        void visitListNode(ListNode* expr) override;
        void visitIfExpr(IfExpr* expr) override;
        void visitWhileExpr(WhileExpr* expr) override;
        void visitForExpr(ForExpr* expr) override;
        void visitBreakExpr(BreakExpr* /*expr*/) override {}
        void visitContinueExpr(ContinueExpr* /*expr*/) override {}
        void visitReturnExpr(ReturnExpr* expr) override;
        void visitIndexAccess(IndexAccess* expr) override;
        void visitIndexAssign(IndexAssign* expr) override;
        void visitLocalDecl(LocalDecl* expr) override;
        void visitRefDecl(RefDecl* expr) override;
        void visitStateDecl(StateDecl* expr) override;
        void visitConstDecl(ConstDecl* expr) override;
        void visitDeleteExpr(DeleteExpr* /*expr*/) override {}
        void visitCompoundAssign(CompoundAssign* expr) override;
        void visitInvokeExpr(InvokeExpr* expr) override;
        void visitForInExpr(ForInExpr* expr) override;
        void visitThrowExpr(ThrowExpr* expr) override;
        void visitTryCatchExpr(TryCatchExpr* expr) override;
        void visitImportExpr(ImportExpr* /*expr*/) override {}
        void visitSwitchExpr(SwitchExpr* expr) override;
        void visitNamespaceDecl(NamespaceDecl* expr) override;
        void visitEnumDefExpr(EnumDefExpr* /*expr*/) override {}
        void visitDotAccess(DotAccess* expr) override;
        void visitDotAssign(DotAssign* expr) override;
        void visitMethodCallExpr(MethodCallExpr* expr) override;
        void visitSuperExpr(SuperExpr* /*expr*/) override {}
        void visitSelfExpr(SelfExpr* /*expr*/) override {}
        void visitContextKeywordExpr(ContextKeywordExpr* /*expr*/) override {}
        void visitDestructAssign(DestructAssign* expr) override;
        void visitFStringExpr(FStringExpr* expr) override;
        void visitMatrixCompExpr(MatrixCompExpr* expr) override;
        void visitListCompExpr(ListCompExpr* expr) override;
        void visitSetCompExpr(SetCompExpr* expr) override;
        void visitDictCompExpr(DictCompExpr* expr) override;
        void visitDictLiteral(DictLiteral* expr) override;
        void visitSetLiteral(SetLiteral* expr) override;
        void visitSliceExpr(SliceExpr* expr) override;
        void visitSequenceExpr(SequenceExpr* expr) override;
        void visitMatchExpr(MatchExpr* expr) override;
        void visitGroupingExpr(GroupingExpr* expr) override;
        void visitMacroDefExpr(MacroDefExpr* /*expr*/) override {}
        void visitMacroCallExpr(MacroCallExpr* /*expr*/) override {}
        void visitQuoteExpr(QuoteExpr* /*expr*/) override {}
        void visitUnquoteExpr(UnquoteExpr* /*expr*/) override {}
        void visitExprAssign(ExprAssign* expr) override;
        void visitDeferExpr(DeferExpr* expr) override;
        void visitKeywordArgExpr(KeywordArgExpr* expr) override;
        void visitSpreadExpr(SpreadExpr* expr) override;
        void visitTypeAssertExpr(TypeAssertExpr* expr) override;
    };

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_SEMANTIC_ANALYZER_H
