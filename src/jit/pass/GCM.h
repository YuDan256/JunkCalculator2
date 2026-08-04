#ifndef JC2_JIT_GCM_H
#define JC2_JIT_GCM_H

#include "../ir/HIR.h"
#include "../ir/LIRBuilder.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <algorithm>
#include <cassert>
#include <functional>

namespace jc {
namespace jit {

// ============================================================================
// 全局代码移动 (Global Code Motion, GCM) (Step 38)
// 负责将无序的 HIR 节点调度到 LIR 基本块中
// ============================================================================
class GCM {
public:
    GCM(const HIRGraph& hir, LIRGraph& lir) : hir_(hir), lir_(lir) {}

    void schedule() {
        // 1. 识别基本块并构建 CFG
        buildCFG();
        
        // 2. 计算支配树 (Dominator Tree)
        computeDominators();
        
        // 3. 调度：Schedule Early
        scheduleEarly();
        
        // 4. 调度：Schedule Late & Block Selection
        scheduleLate();
    }

    LIRBlock* getBlockForNode(HIRNode* node) const {
        auto it = nodeToBlock_.find(node);
        return it != nodeToBlock_.end() ? it->second : nullptr;
    }

private:
    const HIRGraph& hir_;
    LIRGraph& lir_;

    std::unordered_map<HIRNode*, LIRBlock*> controlToBlock_;
    std::unordered_map<HIRNode*, LIRBlock*> nodeToBlock_;
    std::vector<LIRBlock*> blocks_;
    std::unordered_map<LIRBlock*, LIRBlock*> idom_;
    std::unordered_map<LIRBlock*, int> domDepth_;

    bool isControlNode(HIRNode* node) const {
        return node->type() == JITType::Control;
    }

    bool isPinned(HIRNode* node) const {
        return isControlNode(node) || node->opcode() == HIROp::Phi;
    }

    void buildCFG() {
        HIRNode* startNode = nullptr;
        for (auto node : hir_.nodes()) {
            if (node->opcode() == HIROp::Start) {
                startNode = node;
                break;
            }
        }
        if (!startNode) return;

        std::queue<HIRNode*> worklist;
        std::unordered_set<HIRNode*> visited;

        auto getOrCreateBlock = [&](HIRNode* ctrl) -> LIRBlock* {
            if (controlToBlock_.count(ctrl)) return controlToBlock_[ctrl];
            LIRBlock* block = lir_.createBlock();
            controlToBlock_[ctrl] = block;
            blocks_.push_back(block);
            return block;
        };

        worklist.push(startNode);
        visited.insert(startNode);

        while (!worklist.empty()) {
            HIRNode* ctrl = worklist.front();
            worklist.pop();

            LIRBlock* block = getOrCreateBlock(ctrl);
            nodeToBlock_[ctrl] = block;

            // 显式处理 Jump 和 LoopEnd 的无条件跳转边
            if (ctrl->opcode() == HIROp::Jump || ctrl->opcode() == HIROp::LoopEnd) {
                if (ctrl->inputs().size() > 1 && ctrl->inputs()[1]) {
                    HIRNode* target = ctrl->inputs()[1];
                    LIRBlock* succBlock = getOrCreateBlock(target);
                    block->addSuccessor(succBlock);
                    succBlock->addPredecessor(block);
                    
                    if (visited.find(target) == visited.end()) {
                        visited.insert(target);
                        worklist.push(target);
                    }
                }
            }

            for (HIRNode* use : ctrl->uses()) {
                if (isControlNode(use)) {
                    bool isControlEdge = false;
                    if (use->opcode() == HIROp::LoopBegin || use->opcode() == HIROp::Merge) {
                        // LoopBegin 和 Merge 的所有 inputs 都是流入它的控制边
                        for (HIRNode* in : use->inputs()) {
                            if (in == ctrl) { isControlEdge = true; break; }
                        }
                    } else if (!use->inputs().empty() && use->inputs()[0] == ctrl) {
                        // 对于绝大多数控制节点，inputs()[0] 才是真正的控制流依赖
                        isControlEdge = true;
                    }

                    if (isControlEdge) {
                        LIRBlock* succBlock = getOrCreateBlock(use);
                        block->addSuccessor(succBlock);
                        succBlock->addPredecessor(block);

                        if (visited.find(use) == visited.end()) {
                            visited.insert(use);
                            worklist.push(use);
                        }
                    }
                }
            }
        }
    }

    void computeDominators() {
        if (blocks_.empty()) return;
        
        std::unordered_map<LIRBlock*, int> postOrder;
        std::vector<LIRBlock*> reversePostOrder;
        std::unordered_set<LIRBlock*> visited;
        
        std::function<void(LIRBlock*)> dfs = [&](LIRBlock* b) {
            visited.insert(b);
            for (LIRBlock* succ : b->successors()) {
                if (visited.find(succ) == visited.end()) {
                    dfs(succ);
                }
            }
            postOrder[b] = static_cast<int>(postOrder.size());
            reversePostOrder.push_back(b);
        };
        
        dfs(blocks_[0]);
        std::reverse(reversePostOrder.begin(), reversePostOrder.end());

        LIRBlock* entry = blocks_[0];
        idom_[entry] = entry;

        auto intersectPostOrder = [&](LIRBlock* b1, LIRBlock* b2) -> LIRBlock* {
            LIRBlock* finger1 = b1;
            LIRBlock* finger2 = b2;
            while (finger1 != finger2 && finger1 != nullptr && finger2 != nullptr) {
                if (postOrder[finger1] < postOrder[finger2]) {
                    finger1 = idom_[finger1];
                } else if (postOrder[finger2] < postOrder[finger1]) {
                    finger2 = idom_[finger2];
                } else {
                    break;
                }
            }
            return finger1;
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (LIRBlock* b : reversePostOrder) {
                if (b == entry) continue;

                LIRBlock* newIdom = nullptr;
                for (LIRBlock* p : b->predecessors()) {
                    if (idom_.count(p)) {
                        if (newIdom == nullptr) {
                            newIdom = p;
                        } else {
                            newIdom = intersectPostOrder(p, newIdom);
                        }
                    }
                }

                if (newIdom != nullptr && idom_[b] != newIdom) {
                    idom_[b] = newIdom;
                    changed = true;
                }
            }
        }

        domDepth_[entry] = 0;
        for (LIRBlock* b : reversePostOrder) {
            if (b == entry) continue;
            domDepth_[b] = domDepth_[idom_[b]] + 1;
        }
    }

    LIRBlock* intersect(LIRBlock* b1, LIRBlock* b2) {
        LIRBlock* finger1 = b1;
        LIRBlock* finger2 = b2;
        while (finger1 != finger2 && finger1 != nullptr && finger2 != nullptr) {
            if (domDepth_[finger1] > domDepth_[finger2]) {
                finger1 = idom_[finger1];
            } else if (domDepth_[finger2] > domDepth_[finger1]) {
                finger2 = idom_[finger2];
            } else {
                finger1 = idom_[finger1];
                finger2 = idom_[finger2];
            }
        }
        return finger1;
    }

    void scheduleEarly() {
        std::unordered_set<HIRNode*> visited;
        for (auto node : hir_.nodes()) {
            scheduleEarlyNode(node, visited);
        }
    }

    void scheduleEarlyNode(HIRNode* node, std::unordered_set<HIRNode*>& visited) {
        if (visited.count(node)) return;
        visited.insert(node);

        if (isPinned(node)) {
            if (node->opcode() == HIROp::Phi) {
                HIRNode* ctrl = node->inputs()[0];
                if (ctrl) {
                    scheduleEarlyNode(ctrl, visited);
                    nodeToBlock_[node] = nodeToBlock_[ctrl];
                }
            }
            return;
        }

        LIRBlock* earlyBlock = blocks_.empty() ? nullptr : blocks_[0];

        for (HIRNode* input : node->inputs()) {
            if (!input) continue;
            scheduleEarlyNode(input, visited);
            
            LIRBlock* inputBlock = nodeToBlock_[input];
            if (inputBlock && earlyBlock && domDepth_[inputBlock] > domDepth_[earlyBlock]) {
                earlyBlock = inputBlock;
            }
        }

        nodeToBlock_[node] = earlyBlock;
    }

    void scheduleLate() {
        std::unordered_set<HIRNode*> visited;
        for (auto node : hir_.nodes()) {
            scheduleLateNode(node, visited);
        }
    }

    void scheduleLateNode(HIRNode* node, std::unordered_set<HIRNode*>& visited) {
        if (visited.count(node)) return;
        visited.insert(node);

        if (isPinned(node)) return;

        LIRBlock* lca = nullptr;

        for (HIRNode* use : node->uses()) {
            scheduleLateNode(use, visited);
            
            LIRBlock* useBlock = nodeToBlock_[use];
            if (!useBlock) continue;

            if (use->opcode() == HIROp::Phi) {
                for (size_t i = 1; i < use->inputs().size(); ++i) {
                    if (use->inputs()[i] == node) {
                        HIRNode* mergeNode = use->inputs()[0];
                        HIRNode* ctrlIn = mergeNode->inputs()[i - 1];
                        useBlock = nodeToBlock_[ctrlIn];
                        break;
                    }
                }
            }

            if (lca == nullptr) {
                lca = useBlock;
            } else {
                lca = intersect(lca, useBlock);
            }
        }

        if (lca == nullptr) {
            return; // Dead code
        }

        LIRBlock* earlyBlock = nodeToBlock_[node];
        if (!earlyBlock && !blocks_.empty()) earlyBlock = blocks_[0];
        
        LIRBlock* bestBlock = lca;
        LIRBlock* curr = lca;

        // Block Selection: 在 earlyBlock 和 lca 之间选择循环深度最小的块 (LICM)
        while (curr != nullptr && curr != earlyBlock && (!blocks_.empty() && curr != blocks_[0])) {
            if (curr->loopDepth() < bestBlock->loopDepth()) {
                bestBlock = curr;
            }
            curr = idom_[curr];
        }
        if (earlyBlock && earlyBlock->loopDepth() < bestBlock->loopDepth()) {
            bestBlock = earlyBlock;
        }

        nodeToBlock_[node] = bestBlock;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_GCM_H
