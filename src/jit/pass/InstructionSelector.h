#ifndef JC2_JIT_INSTRUCTION_SELECTOR_H
#define JC2_JIT_INSTRUCTION_SELECTOR_H

#include "../ir/HIR.h"
#include "../ir/LIRBuilder.h"
#include "GCM.h"
#include "../../memory/Value.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <cstring>

namespace jc {
namespace jit {

// ============================================================================
// 指令选择器 (Instruction Selector) (Step 39)
// 负责将 HIR 节点降级为 LIR 指令，并注入物理寄存器约束
// ============================================================================
class InstructionSelector {
public:
    InstructionSelector(GCM& gcm, const HIRGraph& hir, LIRGraph& lir, LIRBuilder& builder)
        : gcm_(gcm), hir_(hir), lir_(lir), builder_(builder) {}

    void select() {
        // 1. 按 LIRBlock 对 HIR 节点进行分组
        std::unordered_map<LIRBlock*, std::vector<HIRNode*>> blockNodes;
        for (auto node : hir_.nodes()) {
            LIRBlock* block = gcm_.getBlockForNode(node);
            if (block) {
                blockNodes[block].push_back(node);
            }
        }

        // 2. 为所有产生数据的 HIR 节点预分配虚拟寄存器
        for (auto node : hir_.nodes()) {
            if (node->type() != JITType::Control && 
                node->type() != JITType::Effect && 
                node->type() != JITType::FrameState &&
                node->opcode() != HIROp::NoneConstant) {
                bool isFloat = (node->type() == JITType::Double);
                nodeToOperand_[node] = LIROperand::createVirtual(builder_.allocateVirtualRegister(isFloat));
            }
        }

        // 3. 遍历每个 LIRBlock，进行块内拓扑排序并降级指令
        for (LIRBlock* block : lir_.blocks()) {
            builder_.setCurrentBlock(block);
            
            auto& nodes = blockNodes[block];
            std::vector<HIRNode*> sortedNodes = topologicalSort(nodes);

            std::vector<HIRNode*> dataNodes;
            std::vector<HIRNode*> ctrlNodes;
            for (HIRNode* node : sortedNodes) {
                if (node->opcode() == HIROp::Branch || node->opcode() == HIROp::Jump || 
                    node->opcode() == HIROp::Return || node->opcode() == HIROp::Deoptimize) {
                    ctrlNodes.push_back(node);
                } else {
                    dataNodes.push_back(node);
                }
            }

            for (HIRNode* node : dataNodes) {
                lowerNode(node);
            }
            for (HIRNode* node : ctrlNodes) {
                lowerNode(node);
            }

            // 4. 自动补全基本块的 Fallthrough 跳转
            // 如果块的最后一条指令不是控制流转移指令，且该块有唯一的后继块，则自动发射 Jmp
            if (!block->instructions().empty()) {
                LIROpcode lastOp = block->instructions().back()->opcode();
                if (lastOp != LIROpcode::Ret && lastOp != LIROpcode::Jmp && 
                    lastOp != LIROpcode::Jcc && lastOp != LIROpcode::Deoptimize) {
                    if (block->successors().size() == 1) {
                        builder_.emitJump(block->successors()[0]);
                    }
                }
            } else if (block->successors().size() == 1) {
                // 空块也需要跳转
                builder_.emitJump(block->successors()[0]);
            }
        }

        // 5. 降级 Phi 节点 (De-SSA)
        // 将 Phi 节点转换为在其各个前驱基本块末尾的 Move 指令
        for (LIRBlock* block : lir_.blocks()) {
            auto& nodes = blockNodes[block];
            for (HIRNode* node : nodes) {
                if (node->opcode() == HIROp::Phi) {
                    LIROperand out = getOperand(node);
                    HIRNode* mergeNode = node->inputs()[0];
                    
                    // Phi 的 inputs[1..N] 对应 Merge 的 inputs[0..N-1]
                    for (size_t i = 1; i < node->inputs().size(); ++i) {
                        HIRNode* dataIn = node->inputs()[i];
                        HIRNode* ctrlIn = mergeNode->inputs()[i - 1];
                        LIRBlock* predBlock = gcm_.getBlockForNode(ctrlIn);
                        
                        if (predBlock && dataIn) {
                            LIROperand inVal = getOperand(dataIn);
                            if (!inVal.isInvalid() && !out.isInvalid()) {
                                auto moveInst = new LIRInst(0, LIROpcode::Move);
                                moveInst->addDef(out);
                                moveInst->addUse(inVal);
                                
                                // 插入到前驱块的末尾，但在任何跳转指令 (Jmp/Jcc/Ret) 之前
                                auto& insts = predBlock->instructionsMut();
                                auto insertIt = insts.end();
                                while (insertIt != insts.begin()) {
                                    auto prev = std::prev(insertIt);
                                    LIROpcode op = (*prev)->opcode();
                                    if (op == LIROpcode::Jmp || op == LIROpcode::Jcc || op == LIROpcode::Ret || op == LIROpcode::Deoptimize) {
                                        insertIt = prev;
                                    } else {
                                        break;
                                    }
                                }
                                insts.insert(insertIt, moveInst);
                            }
                        }
                    }
                }
            }
        }
    }

private:
    GCM& gcm_;
    const HIRGraph& hir_;
    LIRGraph& lir_;
    LIRBuilder& builder_;
    std::unordered_map<HIRNode*, LIROperand> nodeToOperand_;

    LIROperand getOperand(HIRNode* node) {
        if (!node) return LIROperand();
        auto it = nodeToOperand_.find(node);
        if (it != nodeToOperand_.end()) {
            return it->second;
        }
        return LIROperand();
    }

    // 块内拓扑排序，确保 Def 在 Use 之前被降级
    std::vector<HIRNode*> topologicalSort(const std::vector<HIRNode*>& nodes) {
        std::vector<HIRNode*> sorted;
        std::unordered_set<HIRNode*> visited;
        std::unordered_set<HIRNode*> nodeSet(nodes.begin(), nodes.end());

        std::function<void(HIRNode*)> visit = [&](HIRNode* n) {
            if (visited.count(n)) return;
            visited.insert(n);
            for (HIRNode* input : n->inputs()) {
                if (input && nodeSet.count(input)) {
                    visit(input);
                }
            }
            sorted.push_back(n);
        };

        for (HIRNode* n : nodes) {
            visit(n);
        }
        return sorted;
    }

    void lowerNode(HIRNode* node) {
        LIROperand out = getOperand(node);

        switch (node->opcode()) {
            case HIROp::Int32Constant: {
                auto n = static_cast<Int32ConstantNode*>(node);
                builder_.emit(LIROpcode::LoadImm32, {out}, {LIROperand::createImm32(n->value())});
                break;
            }
            case HIROp::DoubleConstant: {
                auto n = static_cast<DoubleConstantNode*>(node);
                uint64_t bits;
                double val = n->value();
                std::memcpy(&bits, &val, sizeof(double));
                builder_.emit(LIROpcode::LoadImm64, {out}, {LIROperand::createImm64(bits)});
                break;
            }
            case HIROp::BoolConstant: {
                auto n = static_cast<BoolConstantNode*>(node);
                builder_.emit(LIROpcode::LoadImm32, {out}, {LIROperand::createImm32(n->value() ? 1 : 0)});
                break;
            }
            case HIROp::AddI32:
            case HIROp::SubI32:
            case HIROp::MulI32:
            case HIROp::BitAndI32:
            case HIROp::BitOrI32:
            case HIROp::BitXorI32:
            case HIROp::AddF64:
            case HIROp::SubF64:
            case HIROp::MulF64:
            case HIROp::DivF64: {
                LIROpcode lop;
                if (node->opcode() == HIROp::AddI32) lop = LIROpcode::AddI32;
                else if (node->opcode() == HIROp::SubI32) lop = LIROpcode::SubI32;
                else if (node->opcode() == HIROp::MulI32) lop = LIROpcode::MulI32;
                else if (node->opcode() == HIROp::BitAndI32) lop = LIROpcode::AndI32;
                else if (node->opcode() == HIROp::BitOrI32) lop = LIROpcode::OrI32;
                else if (node->opcode() == HIROp::BitXorI32) lop = LIROpcode::XorI32;
                else if (node->opcode() == HIROp::AddF64) lop = LIROpcode::AddF64;
                else if (node->opcode() == HIROp::SubF64) lop = LIROpcode::SubF64;
                else if (node->opcode() == HIROp::MulF64) lop = LIROpcode::MulF64;
                else lop = LIROpcode::DivF64;

                LIROperand lhs = getOperand(node->inputs()[0]);
                LIROperand rhs = getOperand(node->inputs()[1]);
                // x86-64 双操作数指令约束：输出必须与第一个输入共享物理寄存器
                builder_.emitWithConstraints(lop, 
                    {{out, LIRConstraint::sameAsInput(0)}}, 
                    {{lhs, LIRConstraint::none()}, {rhs, LIRConstraint::none()}});
                break;
            }
            case HIROp::ShlI32:
            case HIROp::ShrI32: {
                LIROpcode lop = (node->opcode() == HIROp::ShlI32) ? LIROpcode::ShlI32 : LIROpcode::ShrI32;
                LIROperand lhs = getOperand(node->inputs()[0]);
                LIROperand rhs = getOperand(node->inputs()[1]);
                // 移位指令约束：移位量必须在 RCX (CL) 中
                builder_.emitWithConstraints(lop, 
                    {{out, LIRConstraint::sameAsInput(0)}}, 
                    {{lhs, LIRConstraint::none()}, {rhs, LIRConstraint::fixedReg(rcx.id())}});
                break;
            }
            case HIROp::DivI32:
            case HIROp::IDivI32: {
                LIROperand lhs = getOperand(node->inputs()[0]);
                LIROperand rhs = getOperand(node->inputs()[1]);
                // IDIV 约束：被除数在 RAX，商在 RAX，余数在 RDX (被破坏)
                auto inst = builder_.emitWithConstraints(LIROpcode::IDivI32, 
                    {{out, LIRConstraint::fixedReg(rax.id())}}, 
                    {{lhs, LIRConstraint::fixedReg(rax.id())}, {rhs, LIRConstraint::anyReg()}});
                inst->addClobber(rdx);
                break;
            }
            case HIROp::ModI32: {
                LIROperand lhs = getOperand(node->inputs()[0]);
                LIROperand rhs = getOperand(node->inputs()[1]);
                // MOD 约束：被除数在 RAX，余数在 RDX，商在 RAX (被破坏)
                auto inst = builder_.emitWithConstraints(LIROpcode::ModI32, 
                    {{out, LIRConstraint::fixedReg(rdx.id())}}, 
                    {{lhs, LIRConstraint::fixedReg(rax.id())}, {rhs, LIRConstraint::anyReg()}});
                inst->addClobber(rax);
                break;
            }
            case HIROp::NegI32:
            case HIROp::NotI32: {
                LIROpcode lop = (node->opcode() == HIROp::NegI32) ? LIROpcode::NegI32 : LIROpcode::NotI32;
                LIROperand val = getOperand(node->inputs()[0]);
                builder_.emitWithConstraints(lop, 
                    {{out, LIRConstraint::sameAsInput(0)}}, 
                    {{val, LIRConstraint::none()}});
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
                LIROpcode lop;
                if (node->opcode() == HIROp::NegF64) lop = LIROpcode::NegF64;
                else if (node->opcode() == HIROp::SqrtF64) lop = LIROpcode::SqrtF64;
                else if (node->opcode() == HIROp::SinF64) lop = LIROpcode::SinF64;
                else if (node->opcode() == HIROp::CosF64) lop = LIROpcode::CosF64;
                else if (node->opcode() == HIROp::AbsF64) lop = LIROpcode::AbsF64;
                else if (node->opcode() == HIROp::FloorF64) lop = LIROpcode::FloorF64;
                else if (node->opcode() == HIROp::CeilF64) lop = LIROpcode::CeilF64;
                else if (node->opcode() == HIROp::RoundF64) lop = LIROpcode::RoundF64;
                else lop = LIROpcode::TruncF64;

                LIROperand val = getOperand(node->inputs()[0]);
                builder_.emitWithConstraints(lop, 
                    {{out, LIRConstraint::anyReg()}}, 
                    {{val, LIRConstraint::anyReg()}});
                break;
            }
            case HIROp::CmpEqI32:
            case HIROp::CmpNeqI32:
            case HIROp::CmpLtI32:
            case HIROp::CmpLeI32:
            case HIROp::CmpGtI32:
            case HIROp::CmpGeI32: {
                LIROperand lhs = getOperand(node->inputs()[0]);
                LIROperand rhs = getOperand(node->inputs()[1]);
                builder_.emit(LIROpcode::CmpI32, {}, {lhs, rhs});
                
                Condition cond;
                if (node->opcode() == HIROp::CmpEqI32) cond = Condition::Equal;
                else if (node->opcode() == HIROp::CmpNeqI32) cond = Condition::NotEqual;
                else if (node->opcode() == HIROp::CmpLtI32) cond = Condition::Less;
                else if (node->opcode() == HIROp::CmpLeI32) cond = Condition::LessOrEqual;
                else if (node->opcode() == HIROp::CmpGtI32) cond = Condition::Greater;
                else cond = Condition::GreaterOrEqual;

                auto inst = builder_.emit(LIROpcode::Setcc, {out}, {});
                inst->setCondition(cond);
                break;
            }
            case HIROp::CmpEqF64:
            case HIROp::CmpNeqF64:
            case HIROp::CmpLtF64:
            case HIROp::CmpLeF64:
            case HIROp::CmpGtF64:
            case HIROp::CmpGeF64: {
                LIROperand lhs = getOperand(node->inputs()[0]);
                LIROperand rhs = getOperand(node->inputs()[1]);
                builder_.emit(LIROpcode::CmpF64, {}, {lhs, rhs});
                
                Condition cond;
                if (node->opcode() == HIROp::CmpEqF64) cond = Condition::Equal;
                else if (node->opcode() == HIROp::CmpNeqF64) cond = Condition::NotEqual;
                else if (node->opcode() == HIROp::CmpLtF64) cond = Condition::Below;
                else if (node->opcode() == HIROp::CmpLeF64) cond = Condition::BelowOrEqual;
                else if (node->opcode() == HIROp::CmpGtF64) cond = Condition::Above;
                else cond = Condition::AboveOrEqual;

                auto inst = builder_.emit(LIROpcode::Setcc, {out}, {});
                inst->setCondition(cond);
                break;
            }
            case HIROp::CmpEqTagged:
            case HIROp::CmpNeqTagged: {
                LIROperand lhs = getOperand(node->inputs()[0]);
                LIROperand rhs = getOperand(node->inputs()[1]);
                builder_.emit(LIROpcode::Cmp64, {}, {lhs, rhs});
                
                Condition cond = (node->opcode() == HIROp::CmpEqTagged) ? Condition::Equal : Condition::NotEqual;
                auto inst = builder_.emit(LIROpcode::Setcc, {out}, {});
                inst->setCondition(cond);
                break;
            }
            case HIROp::Return: {
                if (node->inputs().size() > 2 && node->inputs()[2]) {
                    LIROperand val = getOperand(node->inputs()[2]);
                    builder_.emitWithConstraints(LIROpcode::Ret, {}, {{val, LIRConstraint::fixedReg(rax.id())}});
                } else {
                    builder_.emit(LIROpcode::Ret);
                }
                break;
            }
            case HIROp::Jump:
            case HIROp::LoopEnd: {
                if (node->inputs().size() > 1 && node->inputs()[1]) {
                    LIRBlock* targetBlock = gcm_.getBlockForNode(node->inputs()[1]);
                    if (targetBlock) builder_.emitJump(targetBlock);
                }
                break;
            }
            case HIROp::Branch: {
                LIROperand cond = getOperand(node->inputs()[1]);
                builder_.emit(LIROpcode::TestI32, {}, {cond, cond});
                
                LIRBlock* trueBlock = nullptr;
                LIRBlock* falseBlock = nullptr;
                for (HIRNode* use : node->uses()) {
                    if (use->opcode() == HIROp::IfTrue) trueBlock = gcm_.getBlockForNode(use);
                    if (use->opcode() == HIROp::IfFalse) falseBlock = gcm_.getBlockForNode(use);
                }
                if (trueBlock && falseBlock) {
                    builder_.emitCondJump(Condition::NotZero, trueBlock, falseBlock);
                }
                break;
            }
            case HIROp::Deoptimize: {
                auto fs = static_cast<FrameStateNode*>(node->inputs()[2]);
                auto inst = builder_.emit(LIROpcode::Deoptimize);
                if (fs) inst->setBailoutId(fs->bailoutId());
                break;
            }
            case HIROp::GuardIsInt32:
            case HIROp::GuardIsDouble:
            case HIROp::GuardIsBool:
            case HIROp::GuardIsString:
            case HIROp::GuardIsObject:
            case HIROp::GuardTruthy:
                // Guard 节点是控制流节点，不产生数据，在 LIR 阶段暂时忽略（后续会生成 cmp + jcc）
                break;
            case HIROp::GuardIsClass: {
                auto n = static_cast<GuardIsClassNode*>(node);
                LIROperand obj = getOperand(n->value());
                auto inst = builder_.emitWithConstraints(LIROpcode::GuardIsClass, {}, {{obj, LIRConstraint::anyReg()}});
                
                // 动态计算 C++ 结构体的内存偏移量，保证跨平台和跨编译器的绝对安全
                ObjInstance dummyInst;
                ObjClass dummyClass;
                int32_t typeOffset = static_cast<int32_t>(reinterpret_cast<char*>(&dummyInst.type) - reinterpret_cast<char*>(&dummyInst));
                int32_t classDefOffset = static_cast<int32_t>(reinterpret_cast<char*>(&dummyInst.classDef) - reinterpret_cast<char*>(&dummyInst));
                int32_t classIdOffset = static_cast<int32_t>(reinterpret_cast<char*>(&dummyClass.classId) - reinterpret_cast<char*>(&dummyClass));
                
                inst->addUse(LIROperand::createImm64(n->classId()));
                inst->addUse(LIROperand::createImm32(typeOffset));
                inst->addUse(LIROperand::createImm32(classDefOffset));
                inst->addUse(LIROperand::createImm32(classIdOffset));
                
                inst->setBailoutId(n->frameState()->bailoutId());
                break;
            }
            case HIROp::LoadRegister: {
                auto n = static_cast<RegisterAccessNode*>(node);
                // R14 始终保存 frameRegs 指针 (由 CodeEmitter 在 Prologue 中设置)
                LIROperand mem = LIROperand::createMemory(Operand(r14, n->regIndex() * sizeof(uint64_t)));
                builder_.emitWithConstraints(LIROpcode::Move, {{out, LIRConstraint::anyReg()}}, {{mem, LIRConstraint::none()}});
                break;
            }
            case HIROp::StoreRegister: {
                auto n = static_cast<RegisterAccessNode*>(node);
                LIROperand val = getOperand(node->inputs()[2]);
                LIROperand mem = LIROperand::createMemory(Operand(r14, n->regIndex() * sizeof(uint64_t)));
                builder_.emitWithConstraints(LIROpcode::Move, {{mem, LIRConstraint::none()}}, {{val, LIRConstraint::anyReg()}});
                break;
            }
            case HIROp::LoadGlobal: {
                auto n = static_cast<GlobalAccessNode*>(node);
                builder_.emitWithConstraints(LIROpcode::LoadGlobal, 
                    {{out, LIRConstraint::anyReg()}}, 
                    {{LIROperand::createImm32(n->slot()), LIRConstraint::none()}});
                break;
            }
            case HIROp::StoreGlobal: {
                auto n = static_cast<GlobalAccessNode*>(node);
                LIROperand val = getOperand(node->inputs()[2]);
                builder_.emitWithConstraints(LIROpcode::StoreGlobal, 
                    {}, 
                    {{LIROperand::createImm32(n->slot()), LIRConstraint::none()}, {val, LIRConstraint::anyReg()}});
                break;
            }
            case HIROp::LoadField: {
                LIROperand base = getOperand(node->inputs()[2]);
                HIRNode* offsetNode = node->inputs()[3];
                if (offsetNode->opcode() == HIROp::Int32Constant) {
                    int32_t offsetVal = static_cast<Int32ConstantNode*>(offsetNode)->value();
                    builder_.emitWithConstraints(LIROpcode::LoadField,
                        {{out, LIRConstraint::anyReg()}},
                        {{base, LIRConstraint::anyReg()}, {LIROperand::createImm32(offsetVal), LIRConstraint::none()}});
                } else {
                    LIROperand offset = getOperand(offsetNode);
                    builder_.emitWithConstraints(LIROpcode::LoadField,
                        {{out, LIRConstraint::anyReg()}},
                        {{base, LIRConstraint::anyReg()}, {offset, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::StoreField: {
                LIROperand base = getOperand(node->inputs()[2]);
                HIRNode* offsetNode = node->inputs()[3];
                LIROperand val = getOperand(node->inputs()[4]);
                if (offsetNode->opcode() == HIROp::Int32Constant) {
                    int32_t offsetVal = static_cast<Int32ConstantNode*>(offsetNode)->value();
                    builder_.emitWithConstraints(LIROpcode::StoreField,
                        {},
                        {{base, LIRConstraint::anyReg()}, {LIROperand::createImm32(offsetVal), LIRConstraint::none()}, {val, LIRConstraint::anyReg()}});
                } else {
                    LIROperand offset = getOperand(offsetNode);
                    builder_.emitWithConstraints(LIROpcode::StoreField,
                        {},
                        {{base, LIRConstraint::anyReg()}, {offset, LIRConstraint::anyReg()}, {val, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::UnboxInt32: {
                LIROperand inVal = getOperand(node->inputs()[1]);
                if (!inVal.isInvalid() && !out.isInvalid()) {
                    builder_.emitWithConstraints(LIROpcode::UnboxInt32, {{out, LIRConstraint::anyReg()}}, {{inVal, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::UnboxDouble: {
                LIROperand inVal = getOperand(node->inputs()[1]);
                if (!inVal.isInvalid() && !out.isInvalid()) {
                    builder_.emitWithConstraints(LIROpcode::UnboxDouble, {{out, LIRConstraint::anyReg()}}, {{inVal, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::UnboxBool: {
                LIROperand inVal = getOperand(node->inputs()[1]);
                if (!inVal.isInvalid() && !out.isInvalid()) {
                    builder_.emitWithConstraints(LIROpcode::UnboxBool, {{out, LIRConstraint::anyReg()}}, {{inVal, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::BoxInt32: {
                LIROperand inVal = getOperand(node->inputs()[0]);
                if (!inVal.isInvalid() && !out.isInvalid()) {
                    builder_.emitWithConstraints(LIROpcode::BoxInt32, {{out, LIRConstraint::anyReg()}}, {{inVal, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::BoxDouble: {
                LIROperand inVal = getOperand(node->inputs()[0]);
                if (!inVal.isInvalid() && !out.isInvalid()) {
                    builder_.emitWithConstraints(LIROpcode::BoxDouble, {{out, LIRConstraint::anyReg()}}, {{inVal, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::BoxBool: {
                LIROperand inVal = getOperand(node->inputs()[0]);
                if (!inVal.isInvalid() && !out.isInvalid()) {
                    builder_.emitWithConstraints(LIROpcode::BoxBool, {{out, LIRConstraint::anyReg()}}, {{inVal, LIRConstraint::anyReg()}});
                }
                break;
            }
            case HIROp::Int32ToDouble: {
                LIROperand inVal = getOperand(node->inputs()[0]);
                if (!inVal.isInvalid() && !out.isInvalid()) {
                    builder_.emitWithConstraints(LIROpcode::Int32ToDouble, {{out, LIRConstraint::anyReg()}}, {{inVal, LIRConstraint::anyReg()}});
                }
                break;
            }
            default:
                break;
        }
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_INSTRUCTION_SELECTOR_H
