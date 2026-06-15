#ifndef JC2_COMPILER_H
#define JC2_COMPILER_H

#include "../vm/Bytecode.h"
#include "Expr.h"
#include <map>
#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <optional>

namespace jc {

    struct Local {
        std::string name;
        int depth;
        bool isCaptured;
        bool isConst = false;
        bool isRefParam = false; // ★ 新增
        int refParamIndex = -1;  // ★ 新增
        bool isFunction = false; // ★ 新增：标记是否为预声明的函数
    };

    enum class CaptureType {
        Ref,
        State
    };

    struct CaptureModifier {
        CaptureType type;
        bool isConst = false;
        bool isExplicitState = false;
    };

    class Compiler : public ExprVisitor {
    private:
        struct CompilerState {
            CompiledFunction* function = nullptr;
            int scopeDepth = 0;
            std::vector<Local> locals;
            int maxLocals = 0;                  // ★ 新增：跟踪该函数所使用的最大局部变量数
            std::unordered_map<std::string, CaptureModifier> captures; // ★ 新增：统一跟踪当前作用域的 ref/state 变量及其修饰符
            int tryDepth = 0;
            std::string expectedReturnType = "";
        };
        bool inTailPosition = false;
        bool tailCallEmitted = false;
        struct LoopInfo {
            int loopStart;
            std::vector<int> breakJumps;
            std::vector<int> continueJumps;   // ★ 新增
            int scopeDepth;
            int tryDepth;
        };
        std::vector<LoopInfo> loopStack;

        void beginLoop(int loopStart);
        void endLoop();
        void emitBreakJumps();

        std::vector<CompilerState> stateStack;
        std::vector<std::shared_ptr<CompiledFunction>> compiledFunctions;
        int functionIndexOffset = 0;
        int lastLine = 0;
        int topLevelLocalCount = 0;
        std::string currentSourceFile;
        std::unordered_set<std::string> knownGlobals; // ★ 跟踪已知的全局变量

        CompilerState& current() { return stateStack.back(); }
        Chunk* chunk() { return &current().function->chunk; }

        void emit(OpCode op, int line = 0);
        void emit(uint8_t byte, int line = 0);
        void emit16(uint16_t val, int line = 0);
        void emit32(uint32_t val, int line = 0);
        uint16_t makeConstant(const Value& val);
        uint16_t identifierConstant(const std::string& name);

        void beginScope();
        void endScope();
        int resolveLocal(const std::string& name);
        void addLocal(const std::string& name, int depth, bool isConst = false, bool isFunction = false);
        void declareVariable(const std::string& name);

        void compileNode(Expr* expr);
        void initCompiler(CompiledFunction* fn);
        void preDeclareFunctions(Expr* ast);
        void compileCompClause(ListCompExpr* expr, size_t clauseIdx);
        void emitDefaultPreamble(const std::vector<std::shared_ptr<Expr>>& defaultExprs, int paramCount);
        int resolveUpvalue(const std::string& name);
        int resolveUpvalueAt(int level, const std::string& name, bool isRef, bool isState);
        int addUpvalue(int level, const std::string& name, bool isLocal, int index, bool isRef, bool isGlobal = false, bool isExplicitState = false, bool isRefParam = false);
        void emitStoreTarget(Expr* target, bool isConst = false);
        std::optional<Value> tryFoldConstant(Expr* expr);
        void compilePatternMatch(Pattern* p, int valSlot, std::vector<int>& failJumps, bool isConst = false, bool isStateInit = false);
        void collectPatternVars(Pattern* pat, std::vector<std::tuple<std::string, ScopeModifier, bool>>& boundVars);

    public:
        Chunk compile(Expr* ast, const std::string& sourceFile = "");
        Chunk compileModule(Expr* ast, const std::string& sourceFile, const std::string& moduleName);

        const std::vector<std::shared_ptr<CompiledFunction>>& getCompiledFunctions() const { return compiledFunctions; }
        void setCompiledFunctions(const std::vector<std::shared_ptr<CompiledFunction>>& fns) { compiledFunctions = fns; }
        void setFunctionIndexOffset(int offset) { functionIndexOffset = offset; }
        int getTopLevelLocalCount() const { return topLevelLocalCount; }

        void visitLiteral(Literal* expr) override;
        void visitVariable(Variable* expr) override;
        void visitAssign(Assign* expr) override;
        void visitUnary(Unary* expr) override;
        void visitBinary(Binary* expr) override;
        void visitCall(Call* expr) override;
        void visitBlock(Block* expr) override;
        void visitIfExpr(IfExpr* expr) override;
        void visitWhileExpr(WhileExpr* expr) override;
        void visitForExpr(ForExpr* expr) override;

        void visitMatrixNode(MatrixNode*) override;
        void visitBreakExpr(BreakExpr*) override;
        void visitContinueExpr(ContinueExpr*) override;
        void visitReturnExpr(ReturnExpr*) override;
        void visitIndexAccess(IndexAccess*) override;
        void visitIndexAssign(IndexAssign*) override;
        void visitLocalDecl(LocalDecl*) override;
        void visitRefDecl(RefDecl*) override;
        void visitStateDecl(StateDecl*) override;
        void visitConstDecl(ConstDecl*) override;
        void visitDeleteExpr(DeleteExpr*) override;
        void visitCompoundAssign(CompoundAssign*) override;
        void visitLambdaExpr(LambdaExpr*) override;
        void visitInvokeExpr(InvokeExpr*) override;
        void visitForInExpr(ForInExpr*) override;
        void visitThrowExpr(ThrowExpr*) override;
        void visitTryCatchExpr(TryCatchExpr*) override;
        void visitImportExpr(ImportExpr*) override;
        void visitSwitchExpr(SwitchExpr*) override;
        void visitClassDefExpr(ClassDefExpr*) override;
        void visitNamespaceDecl(NamespaceDecl*) override;
        void visitDotAccess(DotAccess*) override;
        void visitDotAssign(DotAssign*) override;
        void visitMethodCallExpr(MethodCallExpr*) override;
        void visitSuperExpr(SuperExpr*) override;
        void visitSelfExpr(SelfExpr*) override;
        void visitDestructAssign(DestructAssign*) override;
        void visitFStringExpr(FStringExpr*) override;
        void visitListCompExpr(ListCompExpr*) override;
        void visitDictLiteral(DictLiteral*) override;
        void visitSetLiteral(SetLiteral*) override;
        void visitSliceExpr(SliceExpr*) override;
        void visitSequenceExpr(SequenceExpr* expr) override;
        void visitMatchExpr(MatchExpr* expr) override;
        void visitGroupingExpr(GroupingExpr* expr) override;
    };

}
#endif
