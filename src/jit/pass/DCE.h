#ifndef JC2_JIT_DCE_H
#define JC2_JIT_DCE_H

#include "../ir/HIR.h"
#include "../ir/HIRBuilder.h"
#include <unordered_set>
#include <vector>

namespace jc {
namespace jit {

// ============================================================================
// 死代码消除 (Dead Code Elimination, DCE) (Step 49)
// 基于 Use-Def 链反向标记存活节点，消除无用计算
// ============================================================================
class DeadCodeElimination {
public:
    DeadCodeElimination(HIRGraph& graph, HIRBuilder& builder)
        : graph_(graph), builder_(builder) {}

    void run() {
        std::unordered_set<HIRNode*> liveNodes;
        std::vector<HIRNode*> worklist;

        // 1. 标记阶段 (Marking): 找出所有具有副作用或控制流的根节点
        for (auto node : graph_.nodes()) {
            if (isRootNode(node)) {
                liveNodes.insert(node);
                worklist.push_back(node);
            }
        }

        // 2. 传播阶段 (Propagation): 沿着 Use-Def 链反向传播存活状态
        // 注意：在传播时，我们暂时不将 FrameState 的 inputs 加入 worklist，
        // 以避免无脑保活所有虚拟寄存器 (优化寄存器压力)
        while (!worklist.empty()) {
            HIRNode* current = worklist.back();
            worklist.pop_back();

            if (current->opcode() == HIROp::FrameState) {
                continue;
            }

            for (HIRNode* input : current->inputs()) {
                if (input && liveNodes.find(input) == liveNodes.end()) {
                    liveNodes.insert(input);
                    worklist.push_back(input);
                }
            }
        }

        // 3. 二次传播 (Secondary Propagation)
        // 传播那些被 FrameState 保活的节点。
        for (auto node : graph_.nodes()) {
            if (node->opcode() == HIROp::FrameState && liveNodes.find(node) != liveNodes.end()) {
                for (size_t i = 0; i < node->inputs().size(); ++i) {
                    HIRNode* input = node->inputs()[i];
                    if (input && liveNodes.find(input) == liveNodes.end()) {
                        // ★ 修复：所有 input（包括 LoadRegister）一律保活。
                        // LoadRegister 是 TaggedValue（可能是 GC 对象，如 self.zombies 这种 DICT），
                        // GC pin (jc2_jit_gc_pin) 依赖 FrameState 记录所有活跃对象；
                        // 若移除，FrameState 会遗漏该对象，GC sweep 会回收仍在使用的对象（use-after-free）。
                        // 去优化恢复仍安全：解释器直接用 VM::registers 中的旧值。
                        liveNodes.insert(input);
                        worklist.push_back(input);
                    }
                }
            }
        }

        while (!worklist.empty()) {
            HIRNode* current = worklist.back();
            worklist.pop_back();

            for (HIRNode* input : current->inputs()) {
                if (input && liveNodes.find(input) == liveNodes.end()) {
                    liveNodes.insert(input);
                    worklist.push_back(input);
                }
            }
        }

        // 5. 消除阶段 (Elimination): 断开所有未被标记为存活的节点
        for (auto node : graph_.nodes()) {
            if (liveNodes.find(node) == liveNodes.end()) {
                // killNode 会将该节点的所有 input 设为 nullptr，
                // 从而自动从其依赖节点的 uses 链中移除自己。
                builder_.killNode(node);
            }
        }
    }

private:
    HIRGraph& graph_;
    HIRBuilder& builder_;

    bool isRootNode(HIRNode* node) const {
        // 控制流节点、副作用节点、守卫节点均视为根节点。
        // 纯数据节点（如 AddI32, Int32Constant）如果不被这些根节点依赖，将被视为死代码。
        switch (node->opcode()) {
            case HIROp::Start:
            case HIROp::OSREntry:
            case HIROp::Return:
            case HIROp::Branch:
            case HIROp::IfTrue:
            case HIROp::IfFalse:
            case HIROp::Jump:
            case HIROp::Merge:
            case HIROp::LoopBegin:
            case HIROp::LoopEnd:
            case HIROp::Deoptimize:
            case HIROp::StoreGlobal:
            case HIROp::StoreField:
            case HIROp::StoreRegister:
            case HIROp::Call:
            case HIROp::CallNative:
            case HIROp::CallBuiltin:
            case HIROp::Callout:
            case HIROp::DivI32:
            case HIROp::IDivI32:
            case HIROp::ModI32:
            case HIROp::DivF64:
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
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_DCE_H
