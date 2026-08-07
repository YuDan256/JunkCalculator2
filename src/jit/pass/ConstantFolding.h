#ifndef JC2_JIT_CONSTANT_FOLDING_H
#define JC2_JIT_CONSTANT_FOLDING_H

#include "../ir/HIR.h"
#include "../ir/HIRBuilder.h"
#include <unordered_set>
#include <queue>
#include <cmath>

namespace jc {
namespace jit {

// ============================================================================
// 常量折叠 (Constant Folding) (Step 48)
// 负责在编译期预计算输入全为常量的算术与逻辑指令
// ============================================================================
class ConstantFolding {
public:
    ConstantFolding(HIRGraph& graph, HIRBuilder& builder)
        : graph_(graph), builder_(builder) {}

    void run() {
        std::queue<HIRNode*> worklist;
        std::unordered_set<HIRNode*> inWorklist;

        // 1. 初始化工作队列：将图中所有节点加入队列
        for (auto node : graph_.nodes()) {
            worklist.push(node);
            inWorklist.insert(node);
        }

        // 2. 迭代折叠
        while (!worklist.empty()) {
            HIRNode* node = worklist.front();
            worklist.pop();
            inWorklist.erase(node);

            HIRNode* folded = tryFold(node);
            if (folded && folded != node) {
                // 如果折叠成功，将依赖于该节点的所有使用者重新加入队列，
                // 因为它们的输入变成了常量，可能触发进一步的折叠。
                for (auto use : node->uses()) {
                    if (inWorklist.find(use) == inWorklist.end()) {
                        worklist.push(use);
                        inWorklist.insert(use);
                    }
                }
                // 使用 HIRBuilder 安全地替换节点，自动维护 Use-Def 链
                builder_.replaceNode(node, folded);
            }
        }
    }

private:
    HIRGraph& graph_;
    HIRBuilder& builder_;

    HIRNode* tryFold(HIRNode* node) {
        switch (node->opcode()) {
            case HIROp::AddI32:
            case HIROp::SubI32:
            case HIROp::MulI32:
            case HIROp::DivI32:
            case HIROp::IDivI32:
            case HIROp::ModI32:
            case HIROp::BitAndI32:
            case HIROp::BitOrI32:
            case HIROp::BitXorI32:
            case HIROp::ShlI32:
            case HIROp::ShrI32: {
                if (node->inputs().size() < 2) return nullptr;
                auto lhs = dynamic_cast<Int32ConstantNode*>(node->inputs()[0]);
                auto rhs = dynamic_cast<Int32ConstantNode*>(node->inputs()[1]);
                
                if (lhs && rhs) {
                    int32_t l = lhs->value();
                    int32_t r = rhs->value();
                    // 转换为 uint32_t 进行运算，避免 C++ 有符号整数溢出导致的 UB
                    uint32_t ul = static_cast<uint32_t>(l);
                    uint32_t ur = static_cast<uint32_t>(r);

                    switch (node->opcode()) {
                        case HIROp::AddI32: return builder_.createInt32Constant(static_cast<int32_t>(ul + ur));
                        case HIROp::SubI32: return builder_.createInt32Constant(static_cast<int32_t>(ul - ur));
                        case HIROp::MulI32: return builder_.createInt32Constant(static_cast<int32_t>(ul * ur));
                        case HIROp::DivI32:
                            if (r != 0 && !(l == INT32_MIN && r == -1)) {
                                if (l % r == 0) {
                                    return builder_.createInt32Constant(l / r);
                                }
                            }
                            break;
                        case HIROp::IDivI32: 
                            // 避免除以零和 INT32_MIN / -1 导致的硬件异常
                            if (r != 0 && !(l == INT32_MIN && r == -1)) {
                                return builder_.createInt32Constant(l / r);
                            }
                            break;
                        case HIROp::ModI32:
                            if (r != 0 && !(l == INT32_MIN && r == -1)) {
                                return builder_.createInt32Constant(l % r);
                            }
                            break;
                        case HIROp::BitAndI32: return builder_.createInt32Constant(l & r);
                        case HIROp::BitOrI32: return builder_.createInt32Constant(l | r);
                        case HIROp::BitXorI32: return builder_.createInt32Constant(l ^ r);
                        case HIROp::ShlI32: 
                            if (r >= 0 && r < 31) {
                                int64_t res = static_cast<int64_t>(l) << r;
                                if (res >= INT32_MIN && res <= INT32_MAX) {
                                    return builder_.createInt32Constant(static_cast<int32_t>(res));
                                }
                            }
                            break; // 溢出或负数移位交由运行时处理
                        case HIROp::ShrI32: 
                            if (r >= 0) {
                                if (r < 31) return builder_.createInt32Constant(l >> r);
                                else return builder_.createInt32Constant(l < 0 ? -1 : 0);
                            }
                            break;
                        default: break;
                    }
                }
                break;
            }
            case HIROp::NegI32:
            case HIROp::NotI32: {
                if (node->inputs().empty()) return nullptr;
                auto val = dynamic_cast<Int32ConstantNode*>(node->inputs()[0]);
                if (val) {
                    int32_t v = val->value();
                    if (node->opcode() == HIROp::NegI32) {
                        // 使用 0u - v 避免 MSVC C4146 警告 (一元负运算符应用于无符号类型)
                        return builder_.createInt32Constant(static_cast<int32_t>(0u - static_cast<uint32_t>(v)));
                    }
                    if (node->opcode() == HIROp::NotI32) {
                        return builder_.createInt32Constant(~v);
                    }
                }
                break;
            }
            case HIROp::Int32ToDouble: {
                if (node->inputs().empty()) return nullptr;
                auto val = dynamic_cast<Int32ConstantNode*>(node->inputs()[0]);
                if (val) {
                    return builder_.createDoubleConstant(static_cast<double>(val->value()));
                }
                break;
            }
            case HIROp::AddF64:
            case HIROp::SubF64:
            case HIROp::MulF64:
            case HIROp::DivF64:
            case HIROp::ModF64: {
                if (node->inputs().size() < 2) return nullptr;
                auto lhs = dynamic_cast<DoubleConstantNode*>(node->inputs()[0]);
                auto rhs = dynamic_cast<DoubleConstantNode*>(node->inputs()[1]);
                
                if (lhs && rhs) {
                    double l = lhs->value();
                    double r = rhs->value();
                    switch (node->opcode()) {
                        case HIROp::AddF64: return builder_.createDoubleConstant(l + r);
                        case HIROp::SubF64: return builder_.createDoubleConstant(l - r);
                        case HIROp::MulF64: return builder_.createDoubleConstant(l * r);
                        case HIROp::DivF64: return builder_.createDoubleConstant(l / r);
                        case HIROp::ModF64: return builder_.createDoubleConstant(std::fmod(l, r));
                        default: break;
                    }
                }
                break;
            }
            case HIROp::NegF64:
            case HIROp::SqrtF64:
            case HIROp::SinF64:
            case HIROp::CosF64:
            case HIROp::AbsF64:
            case HIROp::FloorF64:
            case HIROp::CeilF64:
            case HIROp::RoundF64:
            case HIROp::TruncF64: {
                if (node->inputs().empty()) return nullptr;
                auto val = dynamic_cast<DoubleConstantNode*>(node->inputs()[0]);
                if (val) {
                    double v = val->value();
                    if (node->opcode() == HIROp::NegF64) return builder_.createDoubleConstant(-v);
                    if (node->opcode() == HIROp::SqrtF64) return builder_.createDoubleConstant(std::sqrt(v));
                    if (node->opcode() == HIROp::SinF64) return builder_.createDoubleConstant(std::sin(v));
                    if (node->opcode() == HIROp::CosF64) return builder_.createDoubleConstant(std::cos(v));
                    if (node->opcode() == HIROp::AbsF64) return builder_.createDoubleConstant(std::abs(v));
                    if (node->opcode() == HIROp::FloorF64) return builder_.createDoubleConstant(std::floor(v));
                    if (node->opcode() == HIROp::CeilF64) return builder_.createDoubleConstant(std::ceil(v));
                    if (node->opcode() == HIROp::RoundF64) return builder_.createDoubleConstant(std::round(v));
                    if (node->opcode() == HIROp::TruncF64) return builder_.createDoubleConstant(std::trunc(v));
                }
                break;
            }
            case HIROp::CmpEqI32:
            case HIROp::CmpNeqI32:
            case HIROp::CmpLtI32:
            case HIROp::CmpLeI32:
            case HIROp::CmpGtI32:
            case HIROp::CmpGeI32: {
                if (node->inputs().size() < 2) return nullptr;
                auto lhs = dynamic_cast<Int32ConstantNode*>(node->inputs()[0]);
                auto rhs = dynamic_cast<Int32ConstantNode*>(node->inputs()[1]);
                if (lhs && rhs) {
                    int32_t l = lhs->value();
                    int32_t r = rhs->value();
                    switch (node->opcode()) {
                        case HIROp::CmpEqI32: return builder_.createBoolConstant(l == r);
                        case HIROp::CmpNeqI32: return builder_.createBoolConstant(l != r);
                        case HIROp::CmpLtI32: return builder_.createBoolConstant(l < r);
                        case HIROp::CmpLeI32: return builder_.createBoolConstant(l <= r);
                        case HIROp::CmpGtI32: return builder_.createBoolConstant(l > r);
                        case HIROp::CmpGeI32: return builder_.createBoolConstant(l >= r);
                        default: break;
                    }
                }
                break;
            }
            case HIROp::CmpEqF64:
            case HIROp::CmpNeqF64:
            case HIROp::CmpLtF64:
            case HIROp::CmpLeF64:
            case HIROp::CmpGtF64:
            case HIROp::CmpGeF64: {
                if (node->inputs().size() < 2) return nullptr;
                auto lhs = dynamic_cast<DoubleConstantNode*>(node->inputs()[0]);
                auto rhs = dynamic_cast<DoubleConstantNode*>(node->inputs()[1]);
                if (lhs && rhs) {
                    double l = lhs->value();
                    double r = rhs->value();
                    switch (node->opcode()) {
                        case HIROp::CmpEqF64: return builder_.createBoolConstant(l == r);
                        case HIROp::CmpNeqF64: return builder_.createBoolConstant(l != r);
                        case HIROp::CmpLtF64: return builder_.createBoolConstant(l < r);
                        case HIROp::CmpLeF64: return builder_.createBoolConstant(l <= r);
                        case HIROp::CmpGtF64: return builder_.createBoolConstant(l > r);
                        case HIROp::CmpGeF64: return builder_.createBoolConstant(l >= r);
                        default: break;
                    }
                }
                break;
            }
            default:
                break;
        }
        return nullptr;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_CONSTANT_FOLDING_H
