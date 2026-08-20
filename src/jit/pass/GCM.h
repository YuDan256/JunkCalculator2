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
        
        // 3. 计算循环深度 (Loop Depth) 用于 LICM
        computeLoopDepth();
        
        // 4. 调度：Schedule Early
        scheduleEarly();
        
        // 5. 调度：Schedule Late & Block Selection
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

    // 副作用节点（调用/存储/守卫）必须固定在其控制流块中，
    // 不能被 LICM 提升出循环，否则会破坏副作用顺序（例如循环条件 guard 读取循环变量）。
    bool isSideEffecting(HIRNode* node) const {
        switch (node->opcode()) {
            case HIROp::Callout:
            case HIROp::Call:
            case HIROp::CallNative:
            case HIROp::CallBuiltin:
            case HIROp::StoreGlobal:
            case HIROp::StoreField:
            case HIROp::StoreRegister:
            case HIROp::GuardIsInt32:
            case HIROp::GuardIsDouble:
            case HIROp::GuardIsBool:
            case HIROp::GuardIsString:
            case HIROp::GuardIsObject:
            case HIROp::GuardIsClass:
            case HIROp::GuardTruthy:
                return true;
            default:
                return false;
        }
    }

    void buildCFG() {
        HIRNode* startNode = nullptr;
        for (auto node : hir_.nodes()) {
            if (node->opcode() == HIROp::Start || node->opcode() == HIROp::OSREntry) {
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

            // 判断 from -> to 是否是控制边
            auto isControlEdge = [&](HIRNode* from, HIRNode* to) -> bool {
                if (to->opcode() == HIROp::LoopBegin || to->opcode() == HIROp::Merge) {
                    for (HIRNode* in : to->inputs()) {
                        if (in == from) return true;
                    }
                    return false;
                }
                return !to->inputs().empty() && to->inputs()[0] == from;
            };

            // 显式处理 Jump 和 LoopEnd 的无条件跳转边
            if (ctrl->opcode() == HIROp::Jump || ctrl->opcode() == HIROp::LoopEnd) {
                if (ctrl->inputs().size() > 1 && ctrl->inputs()[1]) {
                    HIRNode* target = ctrl->inputs()[1];
                    LIRBlock* succBlock = getOrCreateBlock(target);
                    if (std::find(block->successors().begin(), block->successors().end(), succBlock) == block->successors().end()) {
                        block->addSuccessor(succBlock);
                        succBlock->addPredecessor(block);
                    }
                    
                    if (visited.find(target) == visited.end()) {
                        visited.insert(target);
                        worklist.push(target);
                    }
                }
            }

            // 计算 ctrl 的控制后继数量（去重：同一 use 可能因 in0/in1 双引用出现两次）
            std::unordered_set<HIRNode*> ctrlSuccs;
            for (HIRNode* u2 : ctrl->uses()) {
                if (isControlNode(u2) && isControlEdge(ctrl, u2)) ctrlSuccs.insert(u2);
            }
            int ctrlSuccCount = static_cast<int>(ctrlSuccs.size());

            for (HIRNode* use : ctrl->uses()) {
                if (isControlNode(use) && isControlEdge(ctrl, use)) {
                    // 计算 use 的控制前驱数量（去重：同一节点可能同时作为 in0 控制和 in1 effect）
                    std::unordered_set<HIRNode*> uniquePreds;
                    for (HIRNode* in : use->inputs()) {
                        if (in && isControlNode(in)) uniquePreds.insert(in);
                    }
                    int usePredCount = static_cast<int>(uniquePreds.size());

                    // 线性链合并：ctrl 只有一个控制后继，且 use 只有一个控制前驱时，
                    // use 与 ctrl 属于同一基本块，不新建块。否则每个控制节点一个块，会把
                    // 回边处的 Merge → Guard 线性链拆散，导致 deopt 恢复点引用到线性链后半段
                    // 的 phi（ParallelMove 在 Merge 块），而指令被放在 Guard 块，linearId 颠倒。
                    if (ctrlSuccCount == 1 && usePredCount == 1) {
                        controlToBlock_[use] = block;
                        nodeToBlock_[use] = block;
                        if (visited.find(use) == visited.end()) {
                            visited.insert(use);
                            worklist.push(use);
                        }
                    } else {
                        LIRBlock* succBlock = getOrCreateBlock(use);
                        if (std::find(block->successors().begin(), block->successors().end(), succBlock) == block->successors().end()) {
                            block->addSuccessor(succBlock);
                            succBlock->addPredecessor(block);
                        }

                        if (visited.find(use) == visited.end()) {
                            visited.insert(use);
                            worklist.push(use);
                        }
                    }
                }
            }
        }
    }

    void computeLoopDepth() {
        std::unordered_map<LIRBlock*, std::vector<LIRBlock*>> loopHeaders;
        
        // 1. 寻找所有回边 (tail -> header)，条件是 header 支配 tail
        for (LIRBlock* tail : blocks_) {
            for (LIRBlock* header : tail->successors()) {
                LIRBlock* curr = tail;
                bool isBackEdge = false;
                while (curr != nullptr) {
                    if (curr == header) {
                        isBackEdge = true;
                        break;
                    }
                    if (idom_[curr] == curr) break; // 到达 entry
                    curr = idom_[curr];
                }
                if (isBackEdge) {
                    loopHeaders[header].push_back(tail);
                }
            }
        }

        // 2. 找出自然循环中的所有基本块，并增加循环深度
        for (const auto& pair : loopHeaders) {
            LIRBlock* header = pair.first;
            const auto& tails = pair.second;
            
            std::unordered_set<LIRBlock*> loopBlocks;
            loopBlocks.insert(header);
            std::queue<LIRBlock*> worklist;
            
            for (LIRBlock* tail : tails) {
                if (loopBlocks.find(tail) == loopBlocks.end()) {
                    loopBlocks.insert(tail);
                    worklist.push(tail);
                }
            }
            
            // 反向 BFS 遍历，直到遇到 header
            while (!worklist.empty()) {
                LIRBlock* b = worklist.front();
                worklist.pop();
                for (LIRBlock* p : b->predecessors()) {
                    if (loopBlocks.find(p) == loopBlocks.end()) {
                        loopBlocks.insert(p);
                        worklist.push(p);
                    }
                }
            }
            
            // 增加该循环内所有基本块的嵌套深度
            for (LIRBlock* b : loopBlocks) {
                b->setLoopDepth(b->loopDepth() + 1);
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
                if (!node->inputs().empty()) {
                    HIRNode* ctrl = node->inputs()[0];
                    if (ctrl) {
                        scheduleEarlyNode(ctrl, visited);
                        nodeToBlock_[node] = nodeToBlock_[ctrl];
                    }
                }
            }
            return;
        }

        LIRBlock* earlyBlock = blocks_.empty() ? nullptr : blocks_[0];

        for (HIRNode* input : node->inputs()) {
            if (!input) continue;
            // FrameState 是 deopt 恢复点，位置必须跟随其关联指令，不参与 early block 计算。
            // 否则它被独立调度到 guard 所在的回边块，会把指令的 early block 拉到叶子块，
            // 破坏 LICM 的 curr == earlyBlock 终止条件，导致循环变量（如 ty += 1）被错误提升出循环。
            if (input->type() == JITType::FrameState) continue;
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

            // FrameState 是 deopt 恢复点，位置跟随其关联指令，不参与被 use 节点的 lca 计算。
            // 否则循环变量的值（如 ty += 1 的 BoxInt32）会被外层循环的 FrameState 拉到循环头块，
            // 导致其 def（在回边块）晚于 use（在循环头块），use-before-def。
            if (use->type() == JITType::FrameState) continue;
            
            LIRBlock* useBlock = nodeToBlock_[use];
            if (!useBlock) continue;

            if (use->opcode() == HIROp::Phi) {
                for (size_t i = 1; i < use->inputs().size(); ++i) {
                    if (use->inputs()[i] == node) {
                        HIRNode* mergeNode = use->inputs()[0];
                        HIRNode* ctrlIn = (i - 1 < mergeNode->inputs().size()) ? mergeNode->inputs()[i - 1] : nullptr;
                        useBlock = ctrlIn ? nodeToBlock_[ctrlIn] : nullptr;
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
            nodeToBlock_.erase(node);
            return; // Dead code
        }

        // 副作用节点固定在控制块，不做 LICM 提升，避免破坏副作用顺序。
        if (isSideEffecting(node)) {
            if (!node->inputs().empty()) {
                HIRNode* ctrl = node->inputs()[0];
                if (ctrl && nodeToBlock_.count(ctrl)) {
                    nodeToBlock_[node] = nodeToBlock_[ctrl];
                    return;
                }
            }
            LIRBlock* eb = nodeToBlock_[node];
            if (!eb && !blocks_.empty()) eb = blocks_[0];
            nodeToBlock_[node] = eb;
            return;
        }

        // 带 FrameState（deopt 恢复点）的节点固定在其 uses 的 LCA，不能被 LICM 提升。
        // 否则 deopt 恢复的寄存器状态会对错执行位置：例如内层循环回边增量 ty += 1
        // 被提升到外层循环头，其 FrameState 引用的内层循环 phi 尚未定义，导致恢复读垃圾。
        bool hasFrameState = false;
        for (HIRNode* in : node->inputs()) {
            if (in && in->type() == JITType::FrameState) {
                hasFrameState = true;
                break;
            }
        }
        if (hasFrameState && lca != nullptr) {
            nodeToBlock_[node] = lca;
            return;
        }

        LIRBlock* earlyBlock = nodeToBlock_[node];
        if (!earlyBlock && !blocks_.empty()) earlyBlock = blocks_[0];
        
        LIRBlock* bestBlock = lca;
        LIRBlock* curr = lca;

        // Block Selection: 在 earlyBlock 和 lca 之间选择循环深度最小的块 (LICM)
        while (curr != nullptr) {
            if (curr->loopDepth() < bestBlock->loopDepth()) {
                bestBlock = curr;
            }
            if (curr == earlyBlock) break;
            // earlyBlock 可能不在 lca 的 idom 链上（当 input 是循环 phi 且回边块参与时），
            // 此时 curr == earlyBlock 永不触发，会一路走到 entry 把循环变量提升出循环。
            // 用 domDepth 作为第二道边界：一旦 curr 比 earlyBlock 更浅，说明已越过 early 位置，停止。
            if (earlyBlock && domDepth_[curr] < domDepth_[earlyBlock]) break;
            if (curr == idom_[curr]) break; // 防止到达支配树根节点后死循环
            curr = idom_[curr];
        }

        nodeToBlock_[node] = bestBlock;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_GCM_H
