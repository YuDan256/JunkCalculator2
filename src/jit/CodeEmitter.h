#ifndef JC2_JIT_CODE_EMITTER_H
#define JC2_JIT_CODE_EMITTER_H

#include "LIR.h"
#include "MacroAssembler.h"
#include <unordered_map>
#include <stdexcept>

namespace jc {
namespace jit {

// ============================================================================
// 机器码发射器 (Code Emitter) (Step 43)
// 负责遍历分配好寄存器的 LIR，调用 MacroAssembler 生成最终机器码
// ============================================================================
class CodeEmitter {
public:
    CodeEmitter(const LIRGraph& lir, MacroAssembler& masm, void* deoptRuntimeFunc = nullptr)
        : lir_(lir), masm_(masm), deoptRuntimeFunc_(deoptRuntimeFunc) {}

    void emit(int32_t stackSize) {
        // 0. 发射函数序言 (Prologue)
        masm_.prologue(stackSize);
        
        // 将 frameRegs 指针保存到专用的 R14 寄存器中
#ifdef _WIN32
        masm_.movq(r14, rcx);
#else
        masm_.movq(r14, rdi);
#endif

        // 1. 为每个 LIRBlock 创建一个 Label
        for (LIRBlock* block : lir_.blocks()) {
            blockLabels_[block] = Label();
        }

        // 2. 遍历基本块发射机器码
        for (LIRBlock* block : lir_.blocks()) {
            masm_.bind(blockLabels_[block]);

            for (LIRInst* inst : block->instructions()) {
                emitInstruction(inst);
            }
        }

        // 3. 发射去优化跳板 (Step 44)
        if (needsDeoptTrampoline_) {
            masm_.emitDeoptTrampoline(deoptTrampolineLabel_, deoptRuntimeFunc_);
        }
    }

private:
    const LIRGraph& lir_;
    MacroAssembler& masm_;
    void* deoptRuntimeFunc_;
    Label deoptTrampolineLabel_;
    bool needsDeoptTrampoline_ = false;
    std::unordered_map<LIRBlock*, Label> blockLabels_;

    Operand getStackOperand(int32_t slot) {
        // 假设栈槽从 rbp 向下分配，slot 是 0, 8, 16...
        // 第一个槽位是 rbp - 8
        return Operand(rbp, -slot - 8);
    }

    void emitInstruction(LIRInst* inst) {
        switch (inst->opcode()) {
            case LIROpcode::Label:
                break;
            case LIROpcode::Move: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    masm_.movq(dst.pregGPR(), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                    masm_.movq(dst.pregGPR(), getStackOperand(src.slot()));
                } else if (dst.isStackSlot() && src.isPhysicalGPR()) {
                    masm_.movq(getStackOperand(dst.slot()), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isMemory()) {
                    masm_.movq(dst.pregGPR(), src.memory());
                } else if (dst.isMemory() && src.isPhysicalGPR()) {
                    masm_.movq(dst.memory(), src.pregGPR());
                } else if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.movsd(dst.pregXMM(), src.pregXMM());
                } else if (dst.isPhysicalXMM() && src.isStackSlot()) {
                    masm_.movsd(dst.pregXMM(), getStackOperand(src.slot()));
                } else if (dst.isStackSlot() && src.isPhysicalXMM()) {
                    masm_.movsd(getStackOperand(dst.slot()), src.pregXMM());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported Move operands.");
                }
                break;
            }
            case LIROpcode::LoadImm32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.mov(dst.pregGPR(), src.imm32());
                } else if (dst.isStackSlot()) {
                    masm_.mov(getStackOperand(dst.slot()), src.imm32());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported LoadImm32 destination.");
                }
                break;
            }
            case LIROpcode::LoadImm64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.movabs(dst.pregGPR(), src.imm64());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported LoadImm64 destination.");
                }
                break;
            }
            case LIROpcode::AddI32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    masm_.add(dst.pregGPR(), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                    masm_.add(dst.pregGPR(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported AddI32 operands.");
                }
                break;
            }
            case LIROpcode::SubI32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    masm_.sub(dst.pregGPR(), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                    masm_.sub(dst.pregGPR(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported SubI32 operands.");
                }
                break;
            }
            case LIROpcode::MulI32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    masm_.imul(dst.pregGPR(), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                    masm_.imul(dst.pregGPR(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported MulI32 operands.");
                }
                break;
            }
            case LIROpcode::IDivI32: {
                const LIROperand& src = inst->uses()[1];
                masm_.cdq();
                if (src.isPhysicalGPR()) {
                    masm_.idiv(src.pregGPR());
                } else if (src.isStackSlot()) {
                    masm_.idiv(getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported IDivI32 operand.");
                }
                break;
            }
            case LIROpcode::ModI32: {
                const LIROperand& src = inst->uses()[1];
                masm_.cdq();
                if (src.isPhysicalGPR()) {
                    masm_.idiv(src.pregGPR());
                } else if (src.isStackSlot()) {
                    masm_.idiv(getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported ModI32 operand.");
                }
                break;
            }
            case LIROpcode::AndI32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    masm_.and_(dst.pregGPR(), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                    masm_.and_(dst.pregGPR(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported AndI32 operands.");
                }
                break;
            }
            case LIROpcode::OrI32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    masm_.or_(dst.pregGPR(), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                    masm_.or_(dst.pregGPR(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported OrI32 operands.");
                }
                break;
            }
            case LIROpcode::XorI32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    masm_.xor_(dst.pregGPR(), src.pregGPR());
                } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                    masm_.xor_(dst.pregGPR(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported XorI32 operands.");
                }
                break;
            }
            case LIROpcode::ShlI32: {
                const LIROperand& dst = inst->defs()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.shl_cl(dst.pregGPR());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported ShlI32 destination.");
                }
                break;
            }
            case LIROpcode::ShrI32: {
                const LIROperand& dst = inst->defs()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.shr_cl(dst.pregGPR());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported ShrI32 destination.");
                }
                break;
            }
            case LIROpcode::NegI32: {
                const LIROperand& dst = inst->defs()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.emitRex(false, Register(), dst.pregGPR());
                    masm_.emit8(0xF7);
                    masm_.emitModRM(3, 3, dst.pregGPR().id());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported NegI32 destination.");
                }
                break;
            }
            case LIROpcode::NotI32: {
                const LIROperand& dst = inst->defs()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.emitRex(false, Register(), dst.pregGPR());
                    masm_.emit8(0xF7);
                    masm_.emitModRM(3, 2, dst.pregGPR().id());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported NotI32 destination.");
                }
                break;
            }
            case LIROpcode::AddF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.addsd(dst.pregXMM(), src.pregXMM());
                } else if (dst.isPhysicalXMM() && src.isStackSlot()) {
                    masm_.addsd(dst.pregXMM(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported AddF64 operands.");
                }
                break;
            }
            case LIROpcode::SubF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.subsd(dst.pregXMM(), src.pregXMM());
                } else if (dst.isPhysicalXMM() && src.isStackSlot()) {
                    masm_.subsd(dst.pregXMM(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported SubF64 operands.");
                }
                break;
            }
            case LIROpcode::MulF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.mulsd(dst.pregXMM(), src.pregXMM());
                } else if (dst.isPhysicalXMM() && src.isStackSlot()) {
                    masm_.mulsd(dst.pregXMM(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported MulF64 operands.");
                }
                break;
            }
            case LIROpcode::DivF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[1];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.divsd(dst.pregXMM(), src.pregXMM());
                } else if (dst.isPhysicalXMM() && src.isStackSlot()) {
                    masm_.divsd(dst.pregXMM(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported DivF64 operands.");
                }
                break;
            }
            case LIROpcode::NegF64: {
                throw std::runtime_error("CodeEmitter: NegF64 not fully implemented.");
                break;
            }
            case LIROpcode::CmpI32: {
                const LIROperand& lhs = inst->uses()[0];
                const LIROperand& rhs = inst->uses()[1];
                if (lhs.isPhysicalGPR() && rhs.isPhysicalGPR()) {
                    masm_.cmp(lhs.pregGPR(), rhs.pregGPR());
                } else if (lhs.isPhysicalGPR() && rhs.isStackSlot()) {
                    masm_.cmp(lhs.pregGPR(), getStackOperand(rhs.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported CmpI32 operands.");
                }
                break;
            }
            case LIROpcode::TestI32: {
                const LIROperand& lhs = inst->uses()[0];
                const LIROperand& rhs = inst->uses()[1];
                if (lhs.isPhysicalGPR() && rhs.isPhysicalGPR()) {
                    masm_.test(lhs.pregGPR(), rhs.pregGPR());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported TestI32 operands.");
                }
                break;
            }
            case LIROpcode::Cmp64: {
                const LIROperand& lhs = inst->uses()[0];
                const LIROperand& rhs = inst->uses()[1];
                if (lhs.isPhysicalGPR() && rhs.isPhysicalGPR()) {
                    masm_.cmpq(lhs.pregGPR(), rhs.pregGPR());
                } else if (lhs.isPhysicalGPR() && rhs.isStackSlot()) {
                    masm_.cmpq(lhs.pregGPR(), getStackOperand(rhs.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported Cmp64 operands.");
                }
                break;
            }
            case LIROpcode::CmpF64: {
                const LIROperand& lhs = inst->uses()[0];
                const LIROperand& rhs = inst->uses()[1];
                if (lhs.isPhysicalXMM() && rhs.isPhysicalXMM()) {
                    masm_.ucomisd(lhs.pregXMM(), rhs.pregXMM());
                } else if (lhs.isPhysicalXMM() && rhs.isStackSlot()) {
                    masm_.ucomisd(lhs.pregXMM(), getStackOperand(rhs.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported CmpF64 operands.");
                }
                break;
            }
            case LIROpcode::Setcc: {
                const LIROperand& dst = inst->defs()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.emitRex(false, Register(), dst.pregGPR());
                    masm_.emit8(0x0F);
                    masm_.emit8(0x90 + static_cast<uint8_t>(inst->condition()));
                    masm_.emitModRM(3, 0, dst.pregGPR().id());
                    masm_.and_(dst.pregGPR(), 1);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported Setcc destination.");
                }
                break;
            }
            case LIROpcode::Jmp: {
                if (inst->target()) {
                    masm_.jmp(blockLabels_[inst->target()]);
                } else {
                    throw std::runtime_error("CodeEmitter: Jmp without target.");
                }
                break;
            }
            case LIROpcode::Jcc: {
                if (inst->target()) {
                    masm_.jcc(inst->condition(), blockLabels_[inst->target()]);
                } else {
                    throw std::runtime_error("CodeEmitter: Jcc without target.");
                }
                break;
            }
            case LIROpcode::Ret: {
                masm_.epilogue();
                break;
            }
            case LIROpcode::Deoptimize: {
                needsDeoptTrampoline_ = true;
                // 将 BailoutId 存入 R10，供跳板使用
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                break;
            }
            case LIROpcode::BoxInt32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    if (dst.pregGPR() != src.pregGPR()) {
                        masm_.mov(dst.pregGPR(), src.pregGPR());
                    } else {
                        masm_.mov(dst.pregGPR(), dst.pregGPR()); // Force zero-extension
                    }
                    masm_.movabs(r11, 0x7FFC000100000000ULL);
                    masm_.emitRex(true, dst.pregGPR(), r11);
                    masm_.emit8(0x0B); // OR r64, r/m64
                    masm_.emitModRM(3, dst.pregGPR().id(), r11.id());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported BoxInt32 operands.");
                }
                break;
            }
            case LIROpcode::UnboxInt32: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    if (dst.pregGPR() != src.pregGPR()) {
                        masm_.mov(dst.pregGPR(), src.pregGPR());
                    }
                    masm_.mov(dst.pregGPR(), dst.pregGPR());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported UnboxInt32 operands.");
                }
                break;
            }
            case LIROpcode::BoxDouble: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR() && src.isPhysicalXMM()) {
                    masm_.emit8(0x66);
                    masm_.emitRex(true, dst.pregGPR(), src.pregXMM());
                    masm_.emit8(0x0F);
                    masm_.emit8(0x7E);
                    masm_.emitModRM(3, src.pregXMM().id(), dst.pregGPR().id());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported BoxDouble operands.");
                }
                break;
            }
            case LIROpcode::UnboxDouble: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalXMM() && src.isPhysicalGPR()) {
                    masm_.emit8(0x66);
                    masm_.emitRex(true, dst.pregXMM(), src.pregGPR());
                    masm_.emit8(0x0F);
                    masm_.emit8(0x6E);
                    masm_.emitModRM(3, dst.pregXMM().id(), src.pregGPR().id());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported UnboxDouble operands.");
                }
                break;
            }
            case LIROpcode::BoxBool: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    if (dst.pregGPR() != src.pregGPR()) {
                        masm_.mov(dst.pregGPR(), src.pregGPR());
                    } else {
                        masm_.mov(dst.pregGPR(), dst.pregGPR()); // Force zero-extension
                    }
                    masm_.movabs(r11, 0x7FFC000000000002ULL); // TAG_FALSE
                    masm_.addq(dst.pregGPR(), r11);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported BoxBool operands.");
                }
                break;
            }
            case LIROpcode::UnboxBool: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                    if (dst.pregGPR() != src.pregGPR()) {
                        masm_.mov(dst.pregGPR(), src.pregGPR());
                    }
                    masm_.and_(dst.pregGPR(), 1);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported UnboxBool operands.");
                }
                break;
            }
            default:
                throw std::runtime_error("CodeEmitter: Unimplemented LIR opcode " + to_string(inst->opcode()));
        }
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_CODE_EMITTER_H
