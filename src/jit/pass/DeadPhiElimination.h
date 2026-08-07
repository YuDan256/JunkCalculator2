#ifndef JC2_JIT_DEAD_PHI_ELIMINATION_H
#define JC2_JIT_DEAD_PHI_ELIMINATION_H

#include "../ir/HIR.h"
#include "../ir/HIRBuilder.h"
#include <unordered_set>
#include <vector>

namespace jc {
namespace jit {

// ============================================================================
// 死 Phi 节点消除 (Dead Phi Elimination) (Step 57)
// 清理循环中未被实际修改的冗余 Phi 节点 (Trivial Phi Elimination)
// ============================================================================
class DeadPhiElimination {
public:
    DeadPhiElimination(HIRGraph& graph, HIRBuilder& builder)
        : graph_(graph), builder_(builder) {}

    void run() {
        std::vector<HIRNode*> worklist;
        std::unordered_set<HIRNode*> inWorklist;

        // 1. 初始化工作队列：将图中所有 Phi 节点加入队列
        for (auto node : graph_.nodes()) {
            if (node->opcode() == HIROp::Phi) {
                worklist.push_back(node);
                inWorklist.insert(node);
            }
        }

        // 2. 迭代消除冗余 Phi 节点
        while (!worklist.empty()) {
            HIRNode* phi = worklist.back();
            worklist.pop_back();
            inWorklist.erase(phi);

            HIRNode* replacement = tryEliminate(phi);
            if (replacement && replacement != phi) {
                // 如果 Phi 被替换，将所有使用该 Phi 的其他 Phi 节点重新加入队列，
                // 因为它们可能因此变成新的冗余 Phi 节点。
                for (auto use : phi->uses()) {
                    if (use->opcode() == HIROp::Phi && inWorklist.find(use) == inWorklist.end()) {
                        worklist.push_back(use);
                        inWorklist.insert(use);
                    }
                }
                // 使用 HIRBuilder 安全地替换节点，自动维护 Use-Def 链
                builder_.replaceNode(phi, replacement);
            }
        }
    }

private:
    HIRGraph& graph_;
    HIRBuilder& builder_;

    HIRNode* tryEliminate(HIRNode* phi) {
        HIRNode* same = nullptr;
        
        // inputs()[0] 是控制流边 (Merge / LoopBegin)，数据边从 inputs()[1] 开始
        for (size_t i = 1; i < phi->inputs().size(); ++i) {
            HIRNode* op = phi->inputs()[i];
            
            // 忽略对自身的引用和重复的引用
            if (op == phi || op == same) {
                continue;
            }
            
            // 如果已经有一个不同的值，说明该 Phi 节点合并了至少两个不同的值，不是冗余的
            if (same != nullptr) {
                return nullptr;
            }
            
            same = op;
        }

        // 如果 same 仍为 nullptr，说明该 Phi 节点只引用了自身（或者没有数据输入）。
        // 这是一个死循环引用，属于不可达的死代码，替换为 NoneConstant 打破循环。
        if (same == nullptr) {
            return builder_.createNoneConstant();
        }

        // 否则，该 Phi 节点是冗余的，可以直接被 same 替换
        return same;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_DEAD_PHI_ELIMINATION_H
