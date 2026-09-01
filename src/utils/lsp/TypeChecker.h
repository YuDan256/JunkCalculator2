#ifndef JC2_LSP_TYPE_CHECKER_H
#define JC2_LSP_TYPE_CHECKER_H

#include <string>
#include <vector>
#include "NameResolver.h"
#include "BuiltinIndex.h"
#include "TypeInferrer.h"
#include "Workspace.h"
#include "../../frontend/Expr.h"

namespace jc {
namespace lsp {

    struct Diagnostic {
        std::string message;
        int startPos = 0;
        int endPos = 0;
        int severity = 2; // 1=Error, 2=Warning, 3=Info, 4=Hint
    };

    class TypeChecker : public ExprVisitor {
    public:
        TypeChecker(Document* doc, NameResolver& resolver, BuiltinIndex& index, TypeInferrer& inferrer);
        void check(Expr* root);
        std::vector<Diagnostic> diagnostics;

    private:
        Document* doc;
        NameResolver& resolver;
        BuiltinIndex& index;
        TypeInferrer& inferrer;

        void addDiag(const std::string& msg, int startPos, int endPos, int severity = 2);
        std::string didYouMean(const std::string& name);
        void checkCallArity(const NameRes& res, int argCount, const Token& callee);
        void checkMemberArity(const BuiltinSymbol& sym, int argCount, const Token& member);

        // ExprVisitor
        void visitBinary(Binary* e) override;
        void visitUnary(Unary* e) override;
        void visitLiteral(Literal*) override {}
        void visitVariable(Variable* e) override;
        void visitAssign(Assign* e) override;
        void visitCall(Call* e) override;
        void visitMatrixNode(MatrixNode* e) override;
        void visitListNode(ListNode* e) override;
        void visitBlock(Block* e) override;
        void visitIfExpr(IfExpr* e) override;
        void visitWhileExpr(WhileExpr* e) override;
        void visitForExpr(ForExpr* e) override;
        void visitBreakExpr(BreakExpr*) override {}
        void visitContinueExpr(ContinueExpr*) override {}
        void visitReturnExpr(ReturnExpr* e) override;
        void visitIndexAccess(IndexAccess* e) override;
        void visitIndexAssign(IndexAssign* e) override;
        void visitLocalDecl(LocalDecl* e) override;
        void visitRefDecl(RefDecl* e) override;
        void visitStateDecl(StateDecl* e) override;
        void visitConstDecl(ConstDecl* e) override;
        void visitDeleteExpr(DeleteExpr*) override {}
        void visitCompoundAssign(CompoundAssign* e) override;
        void visitLambdaExpr(LambdaExpr* e) override;
        void visitInvokeExpr(InvokeExpr* e) override;
        void visitForInExpr(ForInExpr* e) override;
        void visitThrowExpr(ThrowExpr* e) override;
        void visitTryCatchExpr(TryCatchExpr* e) override;
        void visitImportExpr(ImportExpr*) override {}
        void visitSwitchExpr(SwitchExpr* e) override;
        void visitClassDefExpr(ClassDefExpr* e) override;
        void visitNamespaceDecl(NamespaceDecl* e) override;
        void visitEnumDefExpr(EnumDefExpr* e) override;
        void visitDotAccess(DotAccess* e) override;
        void visitDotAssign(DotAssign* e) override;
        void visitMethodCallExpr(MethodCallExpr* e) override;
        void visitSuperExpr(SuperExpr*) override {}
        void visitSelfExpr(SelfExpr*) override {}
        void visitContextKeywordExpr(ContextKeywordExpr*) override {}
        void visitDestructAssign(DestructAssign* e) override;
        void visitFStringExpr(FStringExpr* e) override;
        void visitMatrixCompExpr(MatrixCompExpr* e) override;
        void visitListCompExpr(ListCompExpr* e) override;
        void visitSetCompExpr(SetCompExpr* e) override;
        void visitDictCompExpr(DictCompExpr* e) override;
        void visitDictLiteral(DictLiteral* e) override;
        void visitSetLiteral(SetLiteral* e) override;
        void visitSliceExpr(SliceExpr* e) override;
        void visitSequenceExpr(SequenceExpr* e) override;
        void visitMatchExpr(MatchExpr* e) override;
        void visitGroupingExpr(GroupingExpr* e) override;
        void visitMacroDefExpr(MacroDefExpr* e) override;
        void visitMacroCallExpr(MacroCallExpr*) override {}
        void visitQuoteExpr(QuoteExpr*) override {}
        void visitUnquoteExpr(UnquoteExpr*) override {}
        void visitExprAssign(ExprAssign* e) override;
        void visitDeferExpr(DeferExpr* e) override;
        void visitKeywordArgExpr(KeywordArgExpr* e) override;
        void visitSpreadExpr(SpreadExpr* e) override;
        void visitTypeAssertExpr(TypeAssertExpr* e) override;
    };

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_TYPE_CHECKER_H
