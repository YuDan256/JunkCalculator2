#ifndef JC2_LSP_TYPE_INFERRER_H
#define JC2_LSP_TYPE_INFERRER_H

#include <string>
#include <vector>
#include <unordered_map>
#include "Type.h"
#include "NameResolver.h"
#include "BuiltinIndex.h"
#include "Workspace.h"
#include "../../frontend/Expr.h"

namespace jc {
namespace lsp {

    // 类型推导引擎：fixpoint 值流 + 类型别名表，对齐运行时 ObjTypeDef 模型
    class TypeInferrer : public ExprVisitor {
    public:
        TypeInferrer(Document* doc, NameResolver& resolver, BuiltinIndex& index);

        void infer(Expr* root);

        // 查询节点推导出的类型
        Type typeOf(Expr* node) const;

        // 类型别名表（诊断/补全用）
        const std::unordered_map<std::string, Type>& aliases() const { return typeAliases; }

    private:
        Document* doc;
        NameResolver& resolver;
        BuiltinIndex& index;

        std::unordered_map<Expr*, Type> exprTypes;               // 节点 → 类型
        std::unordered_map<std::string, Type> typeAliases;       // 类型别名 MyType → Type
        std::unordered_map<Expr*, UserSymbol*> declTargets;      // 声明节点 → 变量符号（fixpoint 回填）

        // 值推导
        Type inferExpr(Expr* e);
        // 类型对象推导（类型上下文：注解/断言/类型定义右边）
        Type inferTypeObject(Expr* e);
        // 内置函数返回类型（TypeSig → Type，处理 sameAsParam）
        Type returnTypeOf(const BuiltinSymbol& sym, const std::vector<Type>& argTypes);

        // fixpoint 迭代
        void runFixpoint();
        // 收集声明（变量 = 表达式）
        void collectDecls(Expr* root);

        // 字面量类型
        Type literalType(const Literal* lit);

        // ExprVisitor（收集声明 + 推导）
        void visitAssign(Assign* e) override;
        void visitLocalDecl(LocalDecl* e) override;
        void visitRefDecl(RefDecl* e) override;
        void visitStateDecl(StateDecl* e) override;
        void visitConstDecl(ConstDecl* e) override;
        void visitBlock(Block* e) override;
        void visitLambdaExpr(LambdaExpr* e) override;
        void visitClassDefExpr(ClassDefExpr* e) override;
        void visitNamespaceDecl(NamespaceDecl* e) override;
        void visitBinary(Binary* e) override;
        void visitUnary(Unary* e) override;
        void visitLiteral(Literal*) override {}
        void visitVariable(Variable* e) override;
        void visitCall(Call* e) override;
        void visitMatrixNode(MatrixNode* e) override;
        void visitListNode(ListNode* e) override;
        void visitIfExpr(IfExpr* e) override;
        void visitWhileExpr(WhileExpr* e) override;
        void visitForExpr(ForExpr* e) override;
        void visitBreakExpr(BreakExpr*) override {}
        void visitContinueExpr(ContinueExpr*) override {}
        void visitReturnExpr(ReturnExpr* e) override;
        void visitIndexAccess(IndexAccess* e) override;
        void visitIndexAssign(IndexAssign* e) override;
        void visitDeleteExpr(DeleteExpr*) override {}
        void visitCompoundAssign(CompoundAssign* e) override;
        void visitInvokeExpr(InvokeExpr* e) override;
        void visitForInExpr(ForInExpr* e) override;
        void visitThrowExpr(ThrowExpr* e) override;
        void visitTryCatchExpr(TryCatchExpr* e) override;
        void visitImportExpr(ImportExpr* e) override;
        void visitSwitchExpr(SwitchExpr* e) override;
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

#endif // JC2_LSP_TYPE_INFERRER_H
