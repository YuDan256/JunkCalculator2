#ifndef JC2_LSP_NAME_RESOLVER_H
#define JC2_LSP_NAME_RESOLVER_H

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "BuiltinIndex.h"
#include "Workspace.h"
#include "../../frontend/Expr.h"
#include "../../frontend/Token.h"

namespace jc {
namespace lsp {

    // 用户声明的符号
    struct UserSymbol {
        std::string name;
        enum Kind { Variable, Function, Class, Parameter, Property, Namespace } kind = Variable;
        Position defPos;
        Position defEndPos;
        std::string typeHint;
        std::string inferredType;   // 类型推导（TypeChecker 填）
        std::string docstring;
    };

    // 名字解析结果（某个 identifier 节点解析到谁）
    struct NameRes {
        enum Origin { User, Builtin, Imported, Undefined } origin = Undefined;
        UserSymbol* user = nullptr;                 // User 时
        const BuiltinSymbol* builtin = nullptr;     // Builtin 时
        std::string moduleName;                     // Imported 时：模块名
        std::string memberName;                     // Imported 时：成员名
        const BuiltinSymbol* member = nullptr;      // Imported 时：成员符号（若可解析）
        bool isMethod = false;                      // 是否 dot 后的方法/成员访问
        bool isCallTarget = false;                  // 是否被调用（Call/Invoke/MethodCall 的 callee）
    };

    // shadowing 警告（声明覆盖内置符号）
    struct ShadowWarning {
        std::string name;
        int pos = 0;
    };

    class NameResolver : public ExprVisitor {
    public:
        NameResolver(Document* doc, BuiltinIndex& index);

        void resolve(Expr* root);

        // 查询节点解析结果
        const NameRes* resolveAt(Expr* node) const;

        // 按位置查询解析结果（hover/definition 用）
        const NameRes* resolveAtPos(const Position& pos) const;

        // 作用域符号查询（completion 用）：位置处可见的用户符号
        std::vector<UserSymbol*> visibleSymbolsAt(const Position& pos) const;

        // 全局作用域（diagnostics/documentSymbol 用）
        const std::unordered_map<std::string, UserSymbol>& globalSymbols() const { return globalScope.symbols; }

        // shadowing 警告（TypeChecker 消费）
        const std::vector<ShadowWarning>& shadowWarnings() const { return shadowWarn; }

        // 文档符号（大纲视图）
        std::vector<UserSymbol*> documentSymbols() const;

    private:
        Document* doc;
        BuiltinIndex& index;

        struct Scope {
            std::string name;
            Scope* parent = nullptr;
            std::unordered_map<std::string, UserSymbol> symbols;
            std::vector<std::unique_ptr<Scope>> children;
            Range range;
            bool isFunction = false;
            bool isNamespace = false;
            bool isClass = false;
        };

        Scope globalScope;
        Scope* current = nullptr;

        // 已 import 的模块名 → 命名空间符号（从 VM execImport 收集）
        std::unordered_map<std::string, std::unordered_map<std::string, BuiltinSymbol>> importedModules;

        std::unordered_map<Expr*, NameRes> nameRes;
        std::vector<ShadowWarning> shadowWarn;

        // 作用域管理
        void enterScope(Scope* parent, Range range, bool isFunction = false, bool isNamespace = false, bool isClass = false);
        void leaveScope();
        UserSymbol* declare(const std::string& name, UserSymbol::Kind kind, int startPos, int endPos,
                            const std::string& typeHint = "", const std::string& inferredType = "");
        UserSymbol* findUser(const std::string& name) const;   // 沿作用域链找用户符号

        // 名字解析
        NameRes resolveName(const std::string& name);
        void record(Expr* node, const NameRes& res);

        // import
        void handleImport(ImportExpr* expr);

        // docstring
        std::string extractDocstring(int nodeStartPos);

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
        void visitImportExpr(ImportExpr* e) override;
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

#endif // JC2_LSP_NAME_RESOLVER_H
