#ifndef JC2_JIT_ALGEBRAIC_SIMPLIFICATION_H
#define JC2_JIT_ALGEBRAIC_SIMPLIFICATION_H

#include "../ir/HIR.h"
#include "../ir/HIRBuilder.h"
#include <unordered_set>
#include <queue>

namespace jc {
namespace jit {

// ============================================================================
// 代数化简 (Algebraic Simplification) (Step 52)
// 基于代数恒等式消除冗余的算术与逻辑运算
// ============================================================================
class AlgebraicSimplification {
public:
    AlgebraicSimplification(HIRGraph& graph, HIRBuilder& builder)
        : graph_(graph), builder_(builder) {}

    void run() {
        std::queue<HIRNode*> worklist;
        std::unordered_set<HIRNode*> inWorklist;

        // 1. 初始化工作队列：将图中所有节点加入队列
        for (auto node : graph_.nodes()) {
            worklist.push(node);
            inWorklist.insert(node);
        }

        // 2. 迭代化简
        while (!worklist.empty()) {
            HIRNode* node = worklist.front();
            worklist.pop();
            inWorklist.erase(node);

            HIRNode* simplified = trySimplify(node);
            if (simplified && simplified != node) {
                // 如果化简成功，将依赖于该节点的所有使用者重新加入队列，
                // 因为它们的输入发生了变化，可能触发进一步的化简。
                for (auto use : node->uses()) {
                    if (inWorklist.find(use) == inWorklist.end()) {
                        worklist.push(use);
                        inWorklist.insert(use);
                    }
                }
                // 使用 HIRBuilder 安全地替换节点，自动维护 Use-Def 链
                builder_.replaceNode(node, simplified);
            }
        }
    }

private:
    HIRGraph& graph_;
    HIRBuilder& builder_;

    HIRNode* trySimplify(HIRNode* node) {
        // 处理一元操作的双重化简 (如 Neg(Neg(x)) -> x)
        if (node->opcode() == HIROp::NegI32 || node->opcode() == HIROp::NotI32 || node->opcode() == HIROp::NegF64) {
            if (node->inputs().empty()) return nullptr;
            HIRNode* inner = node->inputs()[0];
            if (inner && inner->opcode() == node->opcode()) {
                if (!inner->inputs().empty()) {
                    return inner->inputs()[0];
                }
            }
            return nullptr;
        }

        // 仅处理双操作数指令
        if (node->inputs().size() < 2) return nullptr;

        HIRNode* lhs = node->inputs()[0];
        HIRNode* rhs = node->inputs()[1];

        auto lhsI32 = dynamic_cast<Int32ConstantNode*>(lhs);
        auto rhsI32 = dynamic_cast<Int32ConstantNode*>(rhs);
        auto lhsF64 = dynamic_cast<DoubleConstantNode*>(lhs);
        auto rhsF64 = dynamic_cast<DoubleConstantNode*>(rhs);

        switch (node->opcode()) {
            case HIROp::AddI32:
                if (rhsI32 && rhsI32->value() == 0) return lhs; // x + 0 -> x
                if (lhsI32 && lhsI32->value() == 0) return rhs; // 0 + x -> x
                break;
            case HIROp::SubI32:
                if (rhsI32 && rhsI32->value() == 0) return lhs; // x - 0 -> x
                if (lhs == rhs) return builder_.createInt32Constant(0); // x - x -> 0
                break;
            case HIROp::MulI32:
                if (rhsI32 && rhsI32->value() == 1) return lhs; // x * 1 -> x
                if (lhsI32 && lhsI32->value() == 1) return rhs; // 1 * x -> x
                if (rhsI32 && rhsI32->value() == 0) return rhs; // x * 0 -> 0
                if (lhsI32 && lhsI32->value() == 0) return lhs; // 0 * x -> 0
                break;
            case HIROp::DivI32:
            case HIROp::IDivI32:
                if (rhsI32 && rhsI32->value() == 1) return lhs; // x / 1 -> x
                // 注意：不化简 x / x -> 1，因为 x 可能为 0 导致除零异常，必须保留指令以触发硬件 Trap
                break;
            case HIROp::BitAndI32:
                if (rhsI32 && rhsI32->value() == 0) return rhs; // x & 0 -> 0
                if (lhsI32 && lhsI32->value() == 0) return lhs; // 0 & x -> 0
                if (rhsI32 && rhsI32->value() == -1) return lhs; // x & -1 -> x
                if (lhsI32 && lhsI32->value() == -1) return rhs; // -1 & x -> x
                if (lhs == rhs) return lhs; // x & x -> x
                break;
            case HIROp::BitOrI32:
                if (rhsI32 && rhsI32->value() == 0) return lhs; // x | 0 -> x
                if (lhsI32 && lhsI32->value() == 0) return rhs; // 0 | x -> x
                if (rhsI32 && rhsI32->value() == -1) return rhs; // x | -1 -> -1
                if (lhsI32 && lhsI32->value() == -1) return lhs; // -1 | x -> -1
                if (lhs == rhs) return lhs; // x | x -> x
                break;
            case HIROp::BitXorI32:
                if (rhsI32 && rhsI32->value() == 0) return lhs; // x ^ 0 -> x
                if (lhsI32 && lhsI32->value() == 0) return rhs; // 0 ^ x -> x
                if (lhs == rhs) return builder_.createInt32Constant(0); // x ^ x -> 0
                break;
            case HIROp::ShlI32:
            case HIROp::ShrI32:
                if (rhsI32 && rhsI32->value() == 0) return lhs; // x << 0 -> x, x >> 0 -> x
                break;
            case HIROp::AddF64:
                if (rhsF64 && rhsF64->value() == 0.0) return lhs; // x + 0.0 -> x
                if (lhsF64 && lhsF64->value() == 0.0) return rhs; // 0.0 + x -> x
                break;
            case HIROp::SubF64:
                if (rhsF64 && rhsF64->value() == 0.0) return lhs; // x - 0.0 -> x
                // 注意：不化简 x - x -> 0.0，因为 NaN - NaN = NaN
                break;
            case HIROp::MulF64:
                if (rhsF64 && rhsF64->value() == 1.0) return lhs; // x * 1.0 -> x
                if (lhsF64 && lhsF64->value() == 1.0) return rhs; // 1.0 * x -> x
                // 注意：不化简 x * 0.0 -> 0.0，因为 Infinity * 0.0 = NaN
                break;
            case HIROp::DivF64:
                if (rhsF64 && rhsF64->value() == 1.0) return lhs; // x / 1.0 -> x
                break;
            default:
                break;
        }
        return nullptr;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_ALGEBRAIC_SIMPLIFICATION_H
