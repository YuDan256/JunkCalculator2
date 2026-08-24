#ifndef JC2_EXPR_H
#define JC2_EXPR_H

#include "Token.h"
#include <memory>
#include <stdexcept>   // ★ std::runtime_error
#include <string>
#include <vector>
#include <new>

namespace jc {

    class ExprAllocator {
    private:
        struct Block {
            static constexpr size_t SIZE = 256 * 1024; // 256KB per block
            char data[SIZE] = {};
            size_t offset = 0;
            Block* next = nullptr;
        };
        Block* head = nullptr;
        std::vector<void*> freeLists[65]; // 8-byte aligned, up to 512 bytes

    public:
        static ExprAllocator& get() {
            static thread_local ExprAllocator instance;
            return instance;
        }

        void* allocate(size_t size) {
            if (size > 512) return ::operator new(size);
            size_t index = (size + 7) / 8;
            if (!freeLists[index].empty()) {
                void* ptr = freeLists[index].back();
                freeLists[index].pop_back();
                return ptr;
            }
            if (!head || head->offset + size > Block::SIZE) {
                Block* newBlock = new Block();
                newBlock->next = head;
                head = newBlock;
            }
            void* ptr = head->data + head->offset;
            head->offset += (size + 7) & ~7; // 8-byte align
            return ptr;
        }

        void deallocate(void* ptr, size_t size) {
            if (size > 512) {
                ::operator delete(ptr);
                return;
            }
            size_t index = (size + 7) / 8;
            freeLists[index].push_back(ptr);
        }

        ~ExprAllocator() {
            while (head) {
                Block* next = head->next;
                delete head;
                head = next;
            }
        }
    };

    struct Expr;

    // 前向声明所有节点
    struct Binary;
    struct Unary;
    struct Literal;
    struct Variable;
    struct Assign;
    struct Call;
    struct MatrixNode;
    struct FunctionDef;
    struct Block;          // ★ 新增
    struct IfExpr;         // ★ 新增
    struct WhileExpr;      // ★ 新增
    struct ForExpr;        // ★ 新增
    struct BreakExpr;      // ★ 新增
    struct ContinueExpr;   // ★ 新增
    struct ReturnExpr;
    struct LocalDecl;        // ★ 新增
    struct RefDecl;
    struct StateDecl;        // ★ 新增
    struct IndexAccess;      // ★ 新增
    struct IndexAssign;      // ★ 新增
    struct ConstDecl;
    struct DeleteExpr;
    struct CompoundAssign;
    struct LambdaExpr;
    struct InvokeExpr;
    struct ForInExpr;
    struct ThrowExpr;        // ★
    struct TryCatchExpr;     // ★
    struct ImportExpr;
    struct SwitchExpr;       // ★
    struct ClassDefExpr;       // ★
    struct NamespaceDecl;      // ★ 新增
    struct EnumDefExpr;        // ★ 新增
    struct DotAccess;          // ★
    struct DotAssign;          // ★
    struct MethodCallExpr;     // ★
    struct SuperExpr;
    struct SelfExpr;           // ★
    struct ContextKeywordExpr; // ★ class/namespace 上下文关键字占位
    struct DestructAssign;     // ★
    struct FStringExpr;
    struct ListCompExpr;       // ★
    struct SetCompExpr;        // ★ 新增
    struct DictCompExpr;       // ★ 新增
    struct DictLiteral;        // ★
    struct SetLiteral;         // ★ 新增
    struct SliceExpr;        // ★ 新增
    struct SequenceExpr;
    struct MatchExpr;
    struct TypeAssertExpr;   // ★ 类型断言表达式（x: int 求值+断言）
    struct GroupingExpr;     // ★ 新增
    struct MacroDefExpr;     // ★ 新增
    struct MacroCallExpr;    // ★ 新增
    struct QuoteExpr;        // ★ 新增
    struct UnquoteExpr;      // ★ 新增
    struct ExprAssign;       // ★ 新增
    struct DeferExpr;        // ★ 新增
    struct KeywordArgExpr;   // ★ 新增

    struct DefaultPattern;   // ★ 新增

    struct Pattern {
        virtual ~Pattern() = default;

        static void* operator new(size_t size) {
            return ExprAllocator::get().allocate(size);
        }
        static void operator delete(void* ptr, size_t size) {
            ExprAllocator::get().deallocate(ptr, size);
        }
    };

    enum class ScopeModifier { None, Local, Ref, State };

    struct LiteralPattern : public Pattern {
        std::unique_ptr<Expr> literal;
        explicit LiteralPattern(std::unique_ptr<Expr> literal) : literal(std::move(literal)) {}
    };

    struct ExprPattern : public Pattern {
        std::unique_ptr<Expr> expr;
        explicit ExprPattern(std::unique_ptr<Expr> expr) : expr(std::move(expr)) {}
    };

    struct DynamicAssertPattern : public Pattern {
        std::unique_ptr<Expr> expr;
        explicit DynamicAssertPattern(std::unique_ptr<Expr> expr) : expr(std::move(expr)) {}
    };

    struct VariablePattern : public Pattern {
        Token name;
        ScopeModifier modifier;
        bool isConst;
        std::shared_ptr<Expr> typeHint;  // ★ 类型断言
        explicit VariablePattern(Token name, ScopeModifier modifier = ScopeModifier::None, bool isConst = false, std::shared_ptr<Expr> typeHint = nullptr) 
            : name(std::move(name)), modifier(modifier), isConst(isConst), typeHint(std::move(typeHint)) {}
    };

    struct RestPattern : public Pattern {
        Token name;
        ScopeModifier modifier;
        bool isConst;
        std::shared_ptr<Expr> typeHint;  // ★ 类型断言
        explicit RestPattern(Token name, ScopeModifier modifier = ScopeModifier::None, bool isConst = false, std::shared_ptr<Expr> typeHint = nullptr) 
            : name(std::move(name)), modifier(modifier), isConst(isConst), typeHint(std::move(typeHint)) {}
    };

    struct ListPattern : public Pattern {
        std::vector<std::unique_ptr<Pattern>> elements;
        std::unique_ptr<RestPattern> rest;
        ListPattern(std::vector<std::unique_ptr<Pattern>> elements, std::unique_ptr<RestPattern> rest)
            : elements(std::move(elements)), rest(std::move(rest)) {}
    };

    struct MatrixPattern : public Pattern {
        std::vector<std::vector<std::unique_ptr<Pattern>>> rows;
        std::unique_ptr<RestPattern> restRow;
        MatrixPattern(std::vector<std::vector<std::unique_ptr<Pattern>>> rows, std::unique_ptr<RestPattern> restRow)
            : rows(std::move(rows)), restRow(std::move(restRow)) {}
    };

    struct DictPattern : public Pattern {
        std::vector<std::pair<std::string, std::unique_ptr<Pattern>>> entries;
        std::unique_ptr<RestPattern> rest;
        DictPattern(std::vector<std::pair<std::string, std::unique_ptr<Pattern>>> entries, std::unique_ptr<RestPattern> rest)
            : entries(std::move(entries)), rest(std::move(rest)) {}
    };

    struct DefaultPattern : public Pattern {
        std::unique_ptr<Pattern> inner;
        std::unique_ptr<Expr> defaultExpr;
        DefaultPattern(std::unique_ptr<Pattern> inner, std::unique_ptr<Expr> defaultExpr)
            : inner(std::move(inner)), defaultExpr(std::move(defaultExpr)) {}
    };

    struct MatchBranch {
        std::vector<std::unique_ptr<Pattern>> patterns;
        std::unique_ptr<Expr> guard;
        std::unique_ptr<Expr> body;
    };

    class ExprVisitor {
    public:
        virtual ~ExprVisitor() = default;
        virtual void visitBinary(Binary* expr) = 0;
        virtual void visitUnary(Unary* expr) = 0;
        virtual void visitLiteral(Literal* expr) = 0;
        virtual void visitVariable(Variable* expr) = 0;
        virtual void visitAssign(Assign* expr) = 0;
        virtual void visitCall(Call* expr) = 0;
        virtual void visitMatrixNode(MatrixNode* expr) = 0;
        virtual void visitBlock(Block* expr) = 0;
        virtual void visitIfExpr(IfExpr* expr) = 0;
        virtual void visitWhileExpr(WhileExpr* expr) = 0;
        virtual void visitForExpr(ForExpr* expr) = 0;
        virtual void visitBreakExpr(BreakExpr* expr) = 0;
        virtual void visitContinueExpr(ContinueExpr* expr) = 0;
        virtual void visitReturnExpr(ReturnExpr* expr) = 0;
        virtual void visitIndexAccess(IndexAccess* expr) = 0;
        virtual void visitIndexAssign(IndexAssign* expr) = 0;
        virtual void visitLocalDecl(LocalDecl* expr) = 0;
        virtual void visitRefDecl(RefDecl* expr) = 0;
        virtual void visitStateDecl(StateDecl* expr) = 0;
        virtual void visitConstDecl(ConstDecl* expr) = 0;
        virtual void visitDeleteExpr(DeleteExpr* expr) = 0;
        virtual void visitCompoundAssign(CompoundAssign* expr) = 0;
        virtual void visitLambdaExpr(LambdaExpr* expr) = 0;
        virtual void visitInvokeExpr(InvokeExpr* expr) = 0;
        virtual void visitForInExpr(ForInExpr* expr) = 0;
        virtual void visitThrowExpr(ThrowExpr* expr) = 0;
        virtual void visitTryCatchExpr(TryCatchExpr* expr) = 0;
        virtual void visitImportExpr(ImportExpr* expr) = 0;
        virtual void visitSwitchExpr(SwitchExpr* expr) = 0;
        virtual void visitClassDefExpr(ClassDefExpr* expr) = 0;
        virtual void visitNamespaceDecl(NamespaceDecl* expr) = 0;
        virtual void visitEnumDefExpr(EnumDefExpr* expr) = 0;
        virtual void visitDotAccess(DotAccess* expr) = 0;
        virtual void visitDotAssign(DotAssign* expr) = 0;
        virtual void visitMethodCallExpr(MethodCallExpr* expr) = 0;
        virtual void visitSuperExpr(SuperExpr* expr) = 0;
        virtual void visitSelfExpr(SelfExpr* expr) = 0;
        virtual void visitContextKeywordExpr(ContextKeywordExpr* expr) = 0;
        virtual void visitDestructAssign(DestructAssign* expr) = 0;
        virtual void visitFStringExpr(FStringExpr* expr) = 0;
        virtual void visitListCompExpr(ListCompExpr* expr) = 0;
        virtual void visitSetCompExpr(SetCompExpr* expr) = 0;
        virtual void visitDictCompExpr(DictCompExpr* expr) = 0;
        virtual void visitDictLiteral(DictLiteral* expr) = 0;
        virtual void visitSetLiteral(SetLiteral* expr) = 0;
        virtual void visitSliceExpr(SliceExpr* expr) = 0;
        virtual void visitSequenceExpr(SequenceExpr* expr) = 0;
        virtual void visitMatchExpr(MatchExpr* expr) = 0;
        virtual void visitGroupingExpr(GroupingExpr* expr) = 0;
        virtual void visitMacroDefExpr(MacroDefExpr* expr) = 0;
        virtual void visitMacroCallExpr(MacroCallExpr* expr) = 0;
        virtual void visitQuoteExpr(QuoteExpr* expr) = 0;
        virtual void visitUnquoteExpr(UnquoteExpr* expr) = 0;
        virtual void visitExprAssign(ExprAssign* expr) = 0;
        virtual void visitDeferExpr(DeferExpr* expr) = 0;
        virtual void visitKeywordArgExpr(KeywordArgExpr* expr) = 0;
        virtual void visitTypeAssertExpr(TypeAssertExpr* expr) = 0;
    };

    struct Expr {
        virtual ~Expr() = default;
        virtual void accept(ExprVisitor& visitor) = 0;

        static void* operator new(size_t size) {
            return ExprAllocator::get().allocate(size);
        }
        static void operator delete(void* ptr, size_t size) {
            ExprAllocator::get().deallocate(ptr, size);
        }
    };

    // ======== 原有节点 (不变) ========

    struct Binary : public Expr {
        std::unique_ptr<Expr> left;
        Token op;
        std::unique_ptr<Expr> right;
        Binary(std::unique_ptr<Expr> left, Token op, std::unique_ptr<Expr> right)
            : left(std::move(left)), op(std::move(op)), right(std::move(right)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitBinary(this); }
    };

    struct Unary : public Expr {
        Token op;
        std::unique_ptr<Expr> right;
        Unary(Token op, std::unique_ptr<Expr> right)
            : op(std::move(op)), right(std::move(right)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitUnary(this); }
    };

    struct Literal : public Expr {
        std::string value;
        bool isString;
        bool isImaginary;  // ★
        bool isKeyword;    // ★ 新增
        explicit Literal(std::string value, bool isStr = false, bool isImag = false, bool isKw = false)
            : value(std::move(value)), isString(isStr), isImaginary(isImag), isKeyword(isKw) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitLiteral(this); }
    };

    struct Variable : public Expr {
        Token name;
        explicit Variable(Token name) : name(std::move(name)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitVariable(this); }
    };

    struct Assign : public Expr {
        Token name;
        std::unique_ptr<Expr> value;
        bool isRef;
        bool isState;
        bool isLocal; // ★ 新增
        bool isConst; // ★ 新增
        std::shared_ptr<Expr> typeHint;  // ★ 类型断言（x: int = 10）
        Assign(Token name, std::unique_ptr<Expr> value, bool isRef = false, bool isState = false, bool isLocal = false, bool isConst = false, std::shared_ptr<Expr> typeHint = nullptr)
            : name(std::move(name)), value(std::move(value)), isRef(isRef), isState(isState), isLocal(isLocal), isConst(isConst), typeHint(std::move(typeHint)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitAssign(this); }
    };

    struct TypeAssertExpr : public Expr {
        Token name;                        // 变量名（错误信息用）
        std::unique_ptr<Expr> value;       // 被求值的表达式（Parser 保证是 Variable）
        std::shared_ptr<Expr> typeHint;    // 类型表达式
        TypeAssertExpr(Token name, std::unique_ptr<Expr> value, std::shared_ptr<Expr> typeHint)
            : name(std::move(name)), value(std::move(value)), typeHint(std::move(typeHint)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitTypeAssertExpr(this); }
    };

    struct LocalDecl : public Expr {
        Token name;
        bool isConst;
        std::shared_ptr<Expr> typeHint;  // ★ 类型断言（local x: int）
        explicit LocalDecl(Token name, bool isConst = false, std::shared_ptr<Expr> typeHint = nullptr) 
            : name(std::move(name)), isConst(isConst), typeHint(std::move(typeHint)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitLocalDecl(this); }
    };

    struct ConstDecl : public Expr {
        Token name;
        explicit ConstDecl(Token name) : name(std::move(name)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitConstDecl(this); }
    };

    struct Call : public Expr {
        Token callee;
        std::vector<std::unique_ptr<Expr>> arguments;
        Call(Token callee, std::vector<std::unique_ptr<Expr>> arguments)
            : callee(std::move(callee)), arguments(std::move(arguments)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitCall(this); }
    };

    struct MatrixNode : public Expr {
        std::vector<std::vector<std::unique_ptr<Expr>>> elements;
        bool forceList;
        explicit MatrixNode(std::vector<std::vector<std::unique_ptr<Expr>>> elements, bool forceList = false)
            : elements(std::move(elements)), forceList(forceList) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitMatrixNode(this); }
    };

    // ======== ★ 新增控制流节点 ========

    // { stmt1; stmt2; ... stmtN } -> 返回最后一条语句的值
    struct Block : public Expr {
        std::vector<std::unique_ptr<Expr>> statements;
        explicit Block(std::vector<std::unique_ptr<Expr>> statements)
            : statements(std::move(statements)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitBlock(this); }
    };

    // if (cond) { ... } else { ... }
    struct IfExpr : public Expr {
        std::unique_ptr<Expr> condition;
        std::unique_ptr<Expr> thenBranch;
        std::unique_ptr<Expr> elseBranch; // 可为 nullptr
        IfExpr(std::unique_ptr<Expr> condition,
            std::unique_ptr<Expr> thenBranch,
            std::unique_ptr<Expr> elseBranch)
            : condition(std::move(condition)),
            thenBranch(std::move(thenBranch)),
            elseBranch(std::move(elseBranch)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitIfExpr(this); }
    };

    // while (cond) { ... }
    struct WhileExpr : public Expr {
        std::unique_ptr<Expr> condition;
        std::unique_ptr<Expr> body;
        WhileExpr(std::unique_ptr<Expr> condition, std::unique_ptr<Expr> body)
            : condition(std::move(condition)), body(std::move(body)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitWhileExpr(this); }
    };

    // for (init; cond; update) { ... }
    struct ForExpr : public Expr {
        std::unique_ptr<Expr> initializer;
        std::unique_ptr<Expr> condition;
        std::unique_ptr<Expr> update;
        std::unique_ptr<Expr> body;
        ForExpr(std::unique_ptr<Expr> init, std::unique_ptr<Expr> cond,
            std::unique_ptr<Expr> upd, std::unique_ptr<Expr> body)
            : initializer(std::move(init)), condition(std::move(cond)),
            update(std::move(upd)), body(std::move(body)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitForExpr(this); }
    };

    struct BreakExpr : public Expr {
        Token keyword;
        explicit BreakExpr(Token keyword) : keyword(std::move(keyword)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitBreakExpr(this); }
    };

    struct ContinueExpr : public Expr {
        Token keyword;
        explicit ContinueExpr(Token keyword) : keyword(std::move(keyword)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitContinueExpr(this); }
    };

    struct ReturnExpr : public Expr {
        Token keyword;
        std::unique_ptr<Expr> value; // 可为 nullptr（裸 return）
        ReturnExpr(Token keyword, std::unique_ptr<Expr> value)
            : keyword(std::move(keyword)), value(std::move(value)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitReturnExpr(this); }
    };

    struct RefDecl : public Expr {
        Token name;
        bool isConst;
        std::shared_ptr<Expr> typeHint;  // ★ 类型断言（ref x: int）
        explicit RefDecl(Token name, bool isConst = false, std::shared_ptr<Expr> typeHint = nullptr) : name(std::move(name)), isConst(isConst), typeHint(std::move(typeHint)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitRefDecl(this); }
    };

    struct StateDecl : public Expr {
        Token name;
        bool isConst;
        std::shared_ptr<Expr> typeHint;  // ★ 类型断言（state x: int）
        explicit StateDecl(Token name, bool isConst = false, std::shared_ptr<Expr> typeHint = nullptr) : name(std::move(name)), isConst(isConst), typeHint(std::move(typeHint)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitStateDecl(this); }
    };

    struct IndexAccess : public Expr {
        std::unique_ptr<Expr> object;                   // 被索引的表达式
        std::vector<std::unique_ptr<Expr>> indices;     // 1 或 2 个索引
        IndexAccess(std::unique_ptr<Expr> object, std::vector<std::unique_ptr<Expr>> indices)
            : object(std::move(object)), indices(std::move(indices)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitIndexAccess(this); }
    };

    struct IndexAssign : public Expr {
        Token name;
        std::unique_ptr<Expr> objectExpr;  // ★ 非空时表示根是表达式（如 self.data）
        std::vector<std::vector<std::unique_ptr<Expr>>> indexChain;
        std::unique_ptr<Expr> value;

        // 原始构造函数（Variable 根）
        IndexAssign(Token name, std::vector<std::vector<std::unique_ptr<Expr>>> indexChain,
            std::unique_ptr<Expr> value)
            : name(std::move(name)), objectExpr(nullptr),
            indexChain(std::move(indexChain)), value(std::move(value)) {
        }

        // ★ 新构造函数（表达式根，如 self.data[i,j] = v）
        IndexAssign(std::unique_ptr<Expr> objectExpr,
            std::vector<std::vector<std::unique_ptr<Expr>>> indexChain,
            std::unique_ptr<Expr> value)
            : name(Token(TokenType::IDENTIFIER, "", 0)), objectExpr(std::move(objectExpr)),
            indexChain(std::move(indexChain)), value(std::move(value)) {
        }

        bool hasObjectExpr() const { return objectExpr != nullptr; }

        void accept(ExprVisitor& visitor) override { visitor.visitIndexAssign(this); }
    };

    struct DeleteExpr : public Expr {
        std::vector<Token> names;
        explicit DeleteExpr(std::vector<Token> names)
            : names(std::move(names)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitDeleteExpr(this); }
    };

    struct CompoundAssign : public Expr {
        std::unique_ptr<Expr> target;   // Variable 或 IndexAccess
        TokenType op;                    // PLUS, MINUS, STAR, SLASH, PERCENT, CARET
        std::unique_ptr<Expr> value;
        bool isRef;
        bool isState;
        bool isLocal; // ★ 新增
        CompoundAssign(std::unique_ptr<Expr> target, TokenType op, std::unique_ptr<Expr> value, bool isRef = false, bool isState = false, bool isLocal = false)
            : target(std::move(target)), op(op), value(std::move(value)), isRef(isRef), isState(isState), isLocal(isLocal) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitCompoundAssign(this); }
    };

    struct LambdaExpr : public Expr {
        std::string name;
        std::vector<Token> params;
        std::vector<bool> paramIsRef;
        std::vector<bool> paramIsConst; // ★ 新增
        std::vector<std::shared_ptr<Expr>> defaultExprs;
        bool hasRestParam;

        std::vector<std::shared_ptr<Expr>> paramTypes; // ★ 新增：参数类型约束   
        std::shared_ptr<Expr> returnType;              // ★ 新增：返回值约束    

        std::string rawBody;
        std::shared_ptr<Expr> body;
        int fnIdx = -1;

        LambdaExpr(std::string name, std::vector<Token> params, std::vector<bool> paramIsRef, std::vector<bool> paramIsConst,
            std::vector<std::shared_ptr<Expr>> defaultExprs, bool hasRestParam,
            std::vector<std::shared_ptr<Expr>> paramTypes, std::shared_ptr<Expr> returnType, // ★ 新增
            std::string rawBody, std::shared_ptr<Expr> body)
            : name(std::move(name)), params(std::move(params)), paramIsRef(std::move(paramIsRef)), paramIsConst(std::move(paramIsConst)),
            defaultExprs(std::move(defaultExprs)),
            hasRestParam(hasRestParam),
            paramTypes(std::move(paramTypes)), returnType(std::move(returnType)), // ★ 新增
            rawBody(std::move(rawBody)), body(std::move(body)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitLambdaExpr(this); }
    };

    struct InvokeExpr : public Expr {
        std::unique_ptr<Expr> callee;
        std::vector<std::unique_ptr<Expr>> arguments;
        InvokeExpr(std::unique_ptr<Expr> callee, std::vector<std::unique_ptr<Expr>> arguments)
            : callee(std::move(callee)), arguments(std::move(arguments)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitInvokeExpr(this); }
    };

    struct ForInExpr : public Expr {
        std::unique_ptr<Pattern> pattern;
        std::unique_ptr<Expr> iterable;
        std::unique_ptr<Expr> body;
        bool isLocal;
        bool isConst;

        ForInExpr(std::unique_ptr<Pattern> pattern, std::unique_ptr<Expr> iterable,
            std::unique_ptr<Expr> body, bool isLocal = false, bool isConst = false)
            : pattern(std::move(pattern)),
            iterable(std::move(iterable)), body(std::move(body)), isLocal(isLocal), isConst(isConst) {
        }

        void accept(ExprVisitor& visitor) override { visitor.visitForInExpr(this); }
    };

    // ★ throw expr
    struct ThrowExpr : public Expr {
        Token keyword;
        std::unique_ptr<Expr> value;
        ThrowExpr(Token keyword, std::unique_ptr<Expr> value)
            : keyword(std::move(keyword)), value(std::move(value)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitThrowExpr(this); }
    };

    // ★ try { ... } catch (e) { ... }
    struct TryCatchExpr : public Expr {
        std::unique_ptr<Expr> tryBody;
        std::unique_ptr<Pattern> catchPattern;
        std::unique_ptr<Expr> catchBody;
        TryCatchExpr(std::unique_ptr<Expr> tryBody, std::unique_ptr<Pattern> catchPattern, std::unique_ptr<Expr> catchBody)
            : tryBody(std::move(tryBody)), catchPattern(std::move(catchPattern)), catchBody(std::move(catchBody)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitTryCatchExpr(this); }
    };

    struct ImportExpr : public Expr {
        std::unique_ptr<Expr> path;
        ImportExpr(std::unique_ptr<Expr> path)
            : path(std::move(path)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitImportExpr(this); }
    };

    // ★ switch (expr) { case v1: { body } case v2, v3: { body } default: { body } }
    struct SwitchExpr : public Expr {
        std::unique_ptr<Expr> subject;
        // 每个 case: (匹配值列表, body)
        std::vector<std::pair<std::vector<std::unique_ptr<Expr>>, std::unique_ptr<Expr>>> cases;
        std::unique_ptr<Expr> defaultBody; // 可为 nullptr
        SwitchExpr(std::unique_ptr<Expr> subject,
            std::vector<std::pair<std::vector<std::unique_ptr<Expr>>, std::unique_ptr<Expr>>> cases,
            std::unique_ptr<Expr> defaultBody)
            : subject(std::move(subject)), cases(std::move(cases)),
            defaultBody(std::move(defaultBody)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitSwitchExpr(this); }
    };

    struct NamespaceDecl : public Expr {
        Token name;
        std::unique_ptr<Expr> body;
        NamespaceDecl(Token name, std::unique_ptr<Expr> body)
            : name(std::move(name)), body(std::move(body)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitNamespaceDecl(this); }
    };

    struct EnumDefExpr : public Expr {
        Token name;
        std::vector<std::pair<Token, std::unique_ptr<Expr>>> members;
        EnumDefExpr(Token name, std::vector<std::pair<Token, std::unique_ptr<Expr>>> members)
            : name(std::move(name)), members(std::move(members)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitEnumDefExpr(this); }
    };

    struct ClassDefExpr : public Expr {
        Token name;
        std::unique_ptr<Expr> superClassExpr;
        struct PropertyDef {
            Token name;
            std::unique_ptr<Expr> value;
            bool isLocal = false;
            bool isConst = false;
        };
        std::vector<PropertyDef> staticProperties;
        std::vector<PropertyDef> instanceProperties;

        ClassDefExpr(Token name, std::unique_ptr<Expr> superClassExpr, std::vector<PropertyDef> staticProperties = {}, std::vector<PropertyDef> instanceProperties = {})
            : name(std::move(name)), superClassExpr(std::move(superClassExpr)),
            staticProperties(std::move(staticProperties)), instanceProperties(std::move(instanceProperties)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitClassDefExpr(this); }
    };

    // ★ obj.field
    struct DotAccess : public Expr {
        std::unique_ptr<Expr> object;
        Token field;
        DotAccess(std::unique_ptr<Expr> object, Token field)
            : object(std::move(object)), field(std::move(field)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitDotAccess(this); }
    };
    // ★ obj.field = value
    struct DotAssign : public Expr {
        std::unique_ptr<Expr> object;
        Token field;
        std::unique_ptr<Expr> value;
        DotAssign(std::unique_ptr<Expr> object, Token field, std::unique_ptr<Expr> value)
            : object(std::move(object)), field(std::move(field)), value(std::move(value)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitDotAssign(this); }
    };
    // ★ obj.method(args)
    struct MethodCallExpr : public Expr {
        std::unique_ptr<Expr> object;
        Token method;
        std::vector<std::unique_ptr<Expr>> arguments;
        MethodCallExpr(std::unique_ptr<Expr> object, Token method,
            std::vector<std::unique_ptr<Expr>> arguments)
            : object(std::move(object)), method(std::move(method)),
            arguments(std::move(arguments)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitMethodCallExpr(this); }
    };

    // ★ super — evaluates to a proxy that dispatches to parent class
    struct SuperExpr : public Expr {
        void accept(ExprVisitor& visitor) override { visitor.visitSuperExpr(this); }
    };

    // ★ self
    struct SelfExpr : public Expr {
        void accept(ExprVisitor& visitor) override { visitor.visitSelfExpr(this); }
    };

    // ★ class / namespace 上下文关键字自引用（内部名 <...> 重写前的占位，走独立 visit 天然豁免 <...> 检查）
    struct ContextKeywordExpr : public Expr {
        enum class Kind { Class, Namespace };
        Kind kind;
        Token keyword;  // 占行号

        ContextKeywordExpr(Kind k, Token kw) : kind(k), keyword(std::move(kw)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitContextKeywordExpr(this); }
    };

    // ★ [a, b, c] = expr
    struct DestructAssign : public Expr {
        std::unique_ptr<Pattern> pattern;
        std::unique_ptr<Expr> value;
        bool isRef;
        bool isState;
        bool isLocal;
        bool isConst;
        DestructAssign(std::unique_ptr<Pattern> pattern, std::unique_ptr<Expr> value, bool isRef = false, bool isState = false, bool isLocal = false, bool isConst = false)
            : pattern(std::move(pattern)), value(std::move(value)), isRef(isRef), isState(isState), isLocal(isLocal), isConst(isConst) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitDestructAssign(this); }
    };

    // ★ f"Hello, {name}! x = {x:.2f}"
    struct FStringExpr : public Expr {
        // literals[0] {exprs[0]} literals[1] {exprs[1]} ... literals[N]
        std::vector<std::string> literals;            // N+1 段纯文本
        std::vector<std::unique_ptr<Expr>> exprs;     // N 个表达式
        std::vector<std::string> formatSpecs;         // N 个格式说明符（空 = 默认）
        FStringExpr(std::vector<std::string> literals,
            std::vector<std::unique_ptr<Expr>> exprs,
            std::vector<std::string> formatSpecs)
            : literals(std::move(literals)), exprs(std::move(exprs)),
            formatSpecs(std::move(formatSpecs)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitFStringExpr(this); }
    };

    struct CompClause {
        std::unique_ptr<Pattern> pattern;
        std::shared_ptr<Expr> iterable;
        std::vector<std::shared_ptr<Expr>> conditions;
        
        CompClause(std::unique_ptr<Pattern> pat, std::shared_ptr<Expr> iter)
            : pattern(std::move(pat)), iterable(std::move(iter)) {
        }
    };

    // ★ [expr for (var in iterable) if (condition)]
    struct ListCompExpr : public Expr {
        std::unique_ptr<Expr> valueExpr;
        std::vector<CompClause> clauses;
        bool forceList;

        ListCompExpr(std::unique_ptr<Expr> valueExpr, std::vector<CompClause> clauses, bool forceList = false)
            : valueExpr(std::move(valueExpr)), clauses(std::move(clauses)), forceList(forceList) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitListCompExpr(this); }
    };

    // ★ @{expr for (var in iterable) if (condition)}
    struct SetCompExpr : public Expr {
        std::unique_ptr<Expr> valueExpr;
        std::vector<CompClause> clauses;

        SetCompExpr(std::unique_ptr<Expr> valueExpr, std::vector<CompClause> clauses)
            : valueExpr(std::move(valueExpr)), clauses(std::move(clauses)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitSetCompExpr(this); }
    };

    // ★ {k: v for (var in iterable) if (condition)}
    struct DictCompExpr : public Expr {
        std::unique_ptr<Expr> keyExpr;
        std::unique_ptr<Expr> valueExpr;
        std::vector<CompClause> clauses;

        DictCompExpr(std::unique_ptr<Expr> keyExpr, std::unique_ptr<Expr> valueExpr, std::vector<CompClause> clauses)
            : keyExpr(std::move(keyExpr)), valueExpr(std::move(valueExpr)), clauses(std::move(clauses)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitDictCompExpr(this); }
    };

    // ★ @{val1, val2, ...}
    struct SetLiteral : public Expr {
        std::vector<std::unique_ptr<Expr>> elements;
        explicit SetLiteral(std::vector<std::unique_ptr<Expr>> elements)
            : elements(std::move(elements)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitSetLiteral(this); }
    };

    // ★ {key: value, key: value, ...}
    struct DictLiteral : public Expr {
        // 每个 entry: (key 表达式, value 表达式)
        // 裸标识符 key 在 Parser 中被转换为 Literal(string)
        std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> entries;
        explicit DictLiteral(std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> entries)
            : entries(std::move(entries)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitDictLiteral(this); }
    };

    // ★ 切片表达式 start:end:step (仅在索引中有效)
    struct SliceExpr : public Expr {
        std::unique_ptr<Expr> start; // nullptr = 缺省
        std::unique_ptr<Expr> end;   // nullptr = 缺省
        std::unique_ptr<Expr> step;  // nullptr = 缺省
        SliceExpr(std::unique_ptr<Expr> start, std::unique_ptr<Expr> end, std::unique_ptr<Expr> step)
            : start(std::move(start)), end(std::move(end)), step(std::move(step)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitSliceExpr(this); }
    };

    // ★ 逗号表达式序列 (expr1, expr2, expr3) -> 返回 expr3 的值
    struct SequenceExpr : public Expr {
        std::vector<std::unique_ptr<Expr>> expressions;
        explicit SequenceExpr(std::vector<std::unique_ptr<Expr>> expressions)
            : expressions(std::move(expressions)) {
        }
        void accept(ExprVisitor& visitor) override { visitor.visitSequenceExpr(this); }
    };

    struct MatchExpr : public Expr {
        std::unique_ptr<Expr> subject;
        std::vector<MatchBranch> branches;
        MatchExpr(std::unique_ptr<Expr> subject, std::vector<MatchBranch> branches)
            : subject(std::move(subject)), branches(std::move(branches)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitMatchExpr(this); }
    };

    struct GroupingExpr : public Expr {
        std::unique_ptr<Expr> expression;
        explicit GroupingExpr(std::unique_ptr<Expr> expression)
            : expression(std::move(expression)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitGroupingExpr(this); }
    };

    struct MacroDefExpr : public Expr {
        Token name;
        std::vector<Token> params;
        bool hasRestParam;
        bool isTokenMacro; // ★
        std::unique_ptr<Expr> body;
        MacroDefExpr(Token name, std::vector<Token> params, bool hasRestParam, bool isTokenMacro, std::unique_ptr<Expr> body)
            : name(std::move(name)), params(std::move(params)), hasRestParam(hasRestParam), isTokenMacro(isTokenMacro), body(std::move(body)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitMacroDefExpr(this); }
    };

    struct MacroCallExpr : public Expr {
        Token macroName;
        std::vector<std::unique_ptr<Expr>> arguments;
        MacroCallExpr(Token macroName, std::vector<std::unique_ptr<Expr>> arguments)
            : macroName(std::move(macroName)), arguments(std::move(arguments)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitMacroCallExpr(this); }
    };

    struct QuoteExpr : public Expr {
        std::unique_ptr<Expr> body;
        explicit QuoteExpr(std::unique_ptr<Expr> body) : body(std::move(body)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitQuoteExpr(this); }
    };

    struct UnquoteExpr : public Expr {
        std::unique_ptr<Expr> expr;
        explicit UnquoteExpr(std::unique_ptr<Expr> expr) : expr(std::move(expr)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitUnquoteExpr(this); }
    };

    struct ExprAssign : public Expr {
        std::unique_ptr<Expr> target;
        std::unique_ptr<Expr> value;
        bool isRef;
        bool isState;
        bool isLocal;
        bool isConst;
        ExprAssign(std::unique_ptr<Expr> target, std::unique_ptr<Expr> value, bool isRef = false, bool isState = false, bool isLocal = false, bool isConst = false)
            : target(std::move(target)), value(std::move(value)), isRef(isRef), isState(isState), isLocal(isLocal), isConst(isConst) {}
        void accept(ExprVisitor& visitor) override { visitor.visitExprAssign(this); }
    };

    struct DeferExpr : public Expr {
        std::unique_ptr<Expr> body;
        explicit DeferExpr(std::unique_ptr<Expr> body) : body(std::move(body)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitDeferExpr(this); }
    };

    struct KeywordArgExpr : public Expr {
        Token name;
        std::unique_ptr<Expr> value;
        KeywordArgExpr(Token name, std::unique_ptr<Expr> value)
            : name(std::move(name)), value(std::move(value)) {}
        void accept(ExprVisitor& visitor) override { visitor.visitKeywordArgExpr(this); }
    };

} // namespace jc
#endif // JC2_EXPR_H
