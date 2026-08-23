#ifndef JC2_JIT_CSE_H
#define JC2_JIT_CSE_H

#include "../ir/HIR.h"
#include "../ir/HIRBuilder.h"
#include "GVN.h"

namespace jc {
namespace jit {

// ============================================================================
// 公共子表达式消除 (Common Subexpression Elimination, CSE) (Step 51)
// 基于 GVN 哈希表，合并图中结构和语义完全相同的纯数据节点
// ============================================================================
class CommonSubexpressionElimination {
public:
    CommonSubexpressionElimination(HIRGraph& graph, HIRBuilder& builder)
        : graph_(graph), builder_(builder) {}

    void run() {
        GVNMap gvn;

        // 遍历图中的所有节点。
        // 由于 HIRBuilder 是按顺序分配节点的，纯数据节点的定义必然先于其使用出现。
        // 当我们替换一个节点时，builder_.replaceNode 会自动更新所有依赖该节点的后续节点的 inputs。
        // 因此，当遍历到后续节点时，它们的 inputs 已经是被 GVN 规范化过的值了。
        for (auto node : graph_.nodes()) {
            if (isPure(node)) {
                HIRNode* existing = gvn.lookupOrAdd(node);
                if (existing && existing != node) {
                    // 发现等价的公共子表达式，将当前节点的所有使用点替换为已存在的节点
                    builder_.replaceNode(node, existing);
                }
            }
        }
    }

private:
    HIRGraph& graph_;
    HIRBuilder& builder_;

    // 判断节点是否为“纯节点” (无副作用，无隐式控制流依赖)
    // 只有纯节点才能被安全地合并和提升
    bool isPure(HIRNode* node) const {
        switch (node->opcode()) {
            // 常量节点
            case HIROp::Int32Constant:
            case HIROp::Int64Constant:
            case HIROp::DoubleConstant:
            case HIROp::BoolConstant:
            case HIROp::StringConstant:
            case HIROp::NoneConstant:
            // 算术与逻辑运算 (Int32)
            case HIROp::AddI32:
            case HIROp::SubI32:
            case HIROp::MulI32:
            case HIROp::BitAndI32:
            case HIROp::BitOrI32:
            case HIROp::BitXorI32:
            case HIROp::ShlI32:
            case HIROp::ShrI32:
            case HIROp::NegI32:
            case HIROp::NotI32:
            // 算术运算 (Double)
            case HIROp::AddF64:
            case HIROp::SubF64:
            case HIROp::MulF64:
            case HIROp::NegF64:
            case HIROp::SqrtF64:
            case HIROp::SinF64:
            case HIROp::CosF64:
            case HIROp::AbsF64:
            case HIROp::FloorF64:
            case HIROp::CeilF64:
            case HIROp::RoundF64:
            case HIROp::TruncF64:
            // 比较运算
            case HIROp::CmpEqI32:
            case HIROp::CmpNeqI32:
            case HIROp::CmpLtI32:
            case HIROp::CmpLeI32:
            case HIROp::CmpGtI32:
            case HIROp::CmpGeI32:
            case HIROp::CmpEqF64:
            case HIROp::CmpNeqF64:
            case HIROp::CmpLtF64:
            case HIROp::CmpLeF64:
            case HIROp::CmpGtF64:
            case HIROp::CmpGeF64:
            case HIROp::CmpEqTagged:
            case HIROp::CmpNeqTagged:
            // 装箱与拆箱
            // (拆箱节点虽然依赖 Guard，但 GVNMap 会严格比较 inputs 指针。
            // 只有当两个拆箱节点依赖同一个 Guard 时，它们才会被合并，这是绝对安全的)
            case HIROp::BoxInt32:
            case HIROp::BoxDouble:
            case HIROp::BoxBool:
            case HIROp::UnboxInt32:
            case HIROp::UnboxDouble:
            case HIROp::UnboxBool:
            case HIROp::Int32ToDouble:
                return true;
            default:
                return false;
        }
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_CSE_H
