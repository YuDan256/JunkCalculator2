#ifndef JC2_JIT_GVN_H
#define JC2_JIT_GVN_H

#include "../ir/HIR.h"
#include <unordered_map>
#include <functional>
#include <string>

namespace jc {
namespace jit {

// ============================================================================
// 全局值编号 (Global Value Numbering, GVN) 核心哈希表 (Step 50)
// 用于识别结构和语义上完全等价的 HIR 节点
// ============================================================================

class GVNMap {
public:
    // 组合哈希值的辅助函数 (基于 Boost hash_combine)
    static void hash_combine(size_t& seed, size_t value) {
        seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    // 判断操作码是否满足交换律
    static bool isCommutative(HIROp op) {
        switch (op) {
            case HIROp::AddI32:
            case HIROp::MulI32:
            case HIROp::BitAndI32:
            case HIROp::BitOrI32:
            case HIROp::BitXorI32:
            case HIROp::AddF64:
            case HIROp::MulF64:
            case HIROp::CmpEqI32:
            case HIROp::CmpNeqI32:
            case HIROp::CmpEqF64:
            case HIROp::CmpNeqF64:
            case HIROp::CmpEqTagged:
            case HIROp::CmpNeqTagged:
                return true;
            default:
                return false;
        }
    }

    struct NodeHasher {
        size_t operator()(HIRNode* node) const {
            size_t seed = 0;
            hash_combine(seed, static_cast<size_t>(node->opcode()));
            hash_combine(seed, static_cast<size_t>(node->type()));

            // 1. 处理常量节点的值哈希
            switch (node->opcode()) {
                case HIROp::Int32Constant:
                    hash_combine(seed, std::hash<int32_t>{}(static_cast<Int32ConstantNode*>(node)->value()));
                    break;
                case HIROp::Int64Constant:
                    hash_combine(seed, std::hash<uint64_t>{}(static_cast<Int64ConstantNode*>(node)->value()));
                    break;
                case HIROp::DoubleConstant:
                    hash_combine(seed, std::hash<double>{}(static_cast<DoubleConstantNode*>(node)->value()));
                    break;
                case HIROp::BoolConstant:
                    hash_combine(seed, std::hash<bool>{}(static_cast<BoolConstantNode*>(node)->value()));
                    break;
                case HIROp::StringConstant:
                    hash_combine(seed, std::hash<std::string>{}(static_cast<StringConstantNode*>(node)->value()));
                    break;
                default:
                    break;
            }

            // 2. 处理输入节点的哈希
            if (isCommutative(node->opcode()) && node->inputs().size() == 2) {
                // 对于满足交换律的二元操作，对输入节点的 ID 进行排序后再哈希，保证 a+b 和 b+a 哈希值一致
                HIRNode* in1 = node->inputs()[0];
                HIRNode* in2 = node->inputs()[1];
                uint32_t id1 = in1 ? in1->id() : 0;
                uint32_t id2 = in2 ? in2->id() : 0;
                if (id1 > id2) std::swap(id1, id2);
                hash_combine(seed, id1);
                hash_combine(seed, id2);
            } else {
                // 严格按照顺序哈希输入节点
                for (HIRNode* in : node->inputs()) {
                    hash_combine(seed, in ? in->id() : 0);
                }
            }
            return seed;
        }
    };

    struct NodeEqual {
        bool operator()(HIRNode* lhs, HIRNode* rhs) const {
            if (lhs->opcode() != rhs->opcode()) return false;
            if (lhs->type() != rhs->type()) return false;

            // 1. 处理常量节点的值比较
            switch (lhs->opcode()) {
                case HIROp::Int32Constant:
                    if (static_cast<Int32ConstantNode*>(lhs)->value() != static_cast<Int32ConstantNode*>(rhs)->value()) return false;
                    break;
                case HIROp::Int64Constant:
                    if (static_cast<Int64ConstantNode*>(lhs)->value() != static_cast<Int64ConstantNode*>(rhs)->value()) return false;
                    break;
                case HIROp::DoubleConstant:
                    if (static_cast<DoubleConstantNode*>(lhs)->value() != static_cast<DoubleConstantNode*>(rhs)->value()) return false;
                    break;
                case HIROp::BoolConstant:
                    if (static_cast<BoolConstantNode*>(lhs)->value() != static_cast<BoolConstantNode*>(rhs)->value()) return false;
                    break;
                case HIROp::StringConstant:
                    if (static_cast<StringConstantNode*>(lhs)->value() != static_cast<StringConstantNode*>(rhs)->value()) return false;
                    break;
                default:
                    break;
            }

            // 2. 处理输入节点的比较
            if (isCommutative(lhs->opcode()) && lhs->inputs().size() == 2 && rhs->inputs().size() == 2) {
                // 交换律比较：(L1==R1 && L2==R2) 或者 (L1==R2 && L2==R1)
                HIRNode* l1 = lhs->inputs()[0];
                HIRNode* l2 = lhs->inputs()[1];
                HIRNode* r1 = rhs->inputs()[0];
                HIRNode* r2 = rhs->inputs()[1];
                return (l1 == r1 && l2 == r2) || (l1 == r2 && l2 == r1);
            } else {
                // 严格顺序比较
                if (lhs->inputs().size() != rhs->inputs().size()) return false;
                for (size_t i = 0; i < lhs->inputs().size(); ++i) {
                    if (lhs->inputs()[i] != rhs->inputs()[i]) return false;
                }
            }
            return true;
        }
    };

    // 查找是否存在等价节点。如果存在则返回已有的等价节点，否则将新节点加入哈希表并返回 nullptr
    HIRNode* lookupOrAdd(HIRNode* node) {
        auto it = map_.find(node);
        if (it != map_.end()) {
            return it->second;
        }
        map_[node] = node;
        return nullptr;
    }

    void clear() {
        map_.clear();
    }

private:
    std::unordered_map<HIRNode*, HIRNode*, NodeHasher, NodeEqual> map_;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_GVN_H
