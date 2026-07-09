#ifndef JC2_COMPILER_IRBUILDER_H
#define JC2_COMPILER_IRBUILDER_H

#include "IR.h"
#include "../vm/Bytecode.h"
#include "../frontend/Expr.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <stdexcept>

namespace jc {

class IRBuilder : public ExprVisitor {
private:
    IRGraph* graph;
    std::vector<std::shared_ptr<CompiledFunction>>* compiledFunctions;
    
    // Sea of Nodes 构建状态
    IRNode* currentControl; // 当前的控制流依赖节点
    IRNode* lastValue;      // 上一个表达式计算出的数据节点

    // 简单的局部变量环境 (用于初步的 SSA 构建)
    // 变量名 -> 当前定义该变量的 IRNode
    std::vector<std::unordered_map<std::string, IRNode*>> envStack;
    std::vector<int> deferCounts;
    int activeTryCount = 0;

    void pushScope() {
        envStack.emplace_back();
        deferCounts.push_back(0);
    }

    void popScope() {
        int dCount = deferCounts.back();
        deferCounts.pop_back();
        if (dCount > 0) {
            IRNode* runNode = graph->createNode(IROp::RunDefers);
            runNode->payload1 = dCount;
            runNode->setControl(currentControl);
            currentControl = runNode;
        }
        envStack.pop_back();
    }

    IRBuilder* parent = nullptr;
    CompiledFunction* currentFunction = nullptr;

    IRNode* readVariable(const std::string& name);
public:
    struct UpvalueTarget {
        int index;
        bool isLocal;
        IRNode* localNode;
    };
    std::vector<UpvalueTarget> upvalueTargets;
private:
    void writeVariable(const std::string& name, IRNode* value, bool isConst = false, bool isGlobalRef = false);
    void declareVariable(const std::string& name, IRNode* value);
    
    IRNode* getLocalNode(const std::string& name);
    int resolveUpvalue(const std::string& name);

    struct IRLoopInfo {
        IRNode* loopNode;
        std::vector<std::unordered_map<std::string, IRNode*>> loopPhisStack;
        IRNode* breakMerge;
        std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> breakEnvs;
        IRNode* continueMerge;
        std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> continueEnvs;
        int tryDepthAtLoopStart;
    };
    std::vector<IRLoopInfo> loopStack;

    std::unordered_map<std::string, int> refParams;
    std::unordered_set<std::string> capturedLocals;
    std::vector<IRNode*> capturedNodesToExtend;

    struct ExitNodeInfo {
        IRNode* node;
        std::vector<std::pair<std::string, IRNode*>> activeVars;
    };
    std::vector<ExitNodeInfo> exitNodes;

    int namespaceScopeDepth = -1;
    std::unordered_set<std::string> currentLocalVars;
    std::unordered_set<std::string> currentConstVars;

    std::string currentReturnTypeHint;
    void buildFunctionParams(const std::vector<Token>& params, const std::vector<std::shared_ptr<Expr>>& defaultExprs, bool hasRestParam, const std::vector<bool>& paramIsRef, const std::vector<bool>& paramIsConst, const std::vector<std::string>& typeHints);
    void buildPatternMatch(Pattern* pat, IRNode* valNode, IRNode* failMerge, ScopeModifier globalMod = ScopeModifier::None, bool globalConst = false, bool isAssignment = false);
    void buildCompClause(ListCompExpr* expr, size_t clauseIdx, IRNode* listNode);

    [[noreturn]] void error(const std::string& message) const {
        throw RuntimeError("SyntaxError", "[Line " + std::to_string(graph->currentLine) + "] " + message);
    }

    [[noreturn]] void error(int line, const std::string& message) const {
        throw RuntimeError("SyntaxError", "[Line " + std::to_string(line) + "] " + message);
    }

    void recordExitNode(IRNode* node) {
        ExitNodeInfo info;
        info.node = node;
        for (const auto& scope : envStack) {
            for (const auto& pair : scope) {
                info.activeVars.push_back(pair);
            }
        }
        exitNodes.push_back(info);
    }

public:
    explicit IRBuilder(IRGraph* graph, std::vector<std::shared_ptr<CompiledFunction>>* compiledFunctions = nullptr, IRBuilder* parent = nullptr, CompiledFunction* currentFunction = nullptr);
    void build(Expr* ast);

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
    void visitDotAccess(DotAccess* expr) override;
    void visitDotAssign(DotAssign* expr) override;
    void visitMethodCallExpr(MethodCallExpr* expr) override;
    void visitSuperExpr(SuperExpr* expr) override;
    void visitSelfExpr(SelfExpr* expr) override;
    void visitDestructAssign(DestructAssign* expr) override;
    void visitFStringExpr(FStringExpr* expr) override;
    void visitListCompExpr(ListCompExpr* expr) override;
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
};

} // namespace jc

#endif // JC2_COMPILER_IRBUILDER_H
