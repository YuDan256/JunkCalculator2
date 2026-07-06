#ifndef JC2_REGVM_IROPTIMIZER_H
#define JC2_REGVM_IROPTIMIZER_H

#include "IR.h"

namespace jc {
namespace regvm {

class IROptimizer {
public:
    // 执行完整的优化管线，直到图结构不再发生变化
    static void optimize(IRGraph* graph);

private:
    static bool foldConstants(IRGraph* graph);
    static bool foldControlFlow(IRGraph* graph);
    static bool eliminateDeadCode(IRGraph* graph);
    static bool simplifyPhis(IRGraph* graph);
    static bool deduplicateConstants(IRGraph* graph);
    static bool eliminateCommonSubexpressions(IRGraph* graph);
    
    static void replaceNode(IRGraph* graph, IRNode* oldNode, IRNode* newNode);
    
    // 判断节点是否具有副作用 (Side Effects)
    static bool hasSideEffects(IROp op);
};

} // namespace regvm
} // namespace jc

#endif // JC2_REGVM_IROPTIMIZER_H
