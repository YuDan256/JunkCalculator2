#ifndef JC2_JIT_CODE_EMITTER_H
#define JC2_JIT_CODE_EMITTER_H

#include "../ir/LIR.h"
#include "MacroAssembler.h"
#include "../runtime/Deoptimization.h"
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
    CodeEmitter(const LIRGraph& lir, MacroAssembler& masm, void* deoptRuntimeFunc = nullptr, void* globalsData = nullptr, void* callRuntimeFunc = nullptr)
        : lir_(lir), masm_(masm), deoptRuntimeFunc_(deoptRuntimeFunc), globalsData_(globalsData), callRuntimeFunc_(callRuntimeFunc) {}

    void emit(int32_t stackSize, bool isOSR = false) {
        // 0. 发射函数序言 (Prologue / OSR Prologue)
        if (isOSR) {
            masm_.osrPrologue(stackSize);
        } else {
            masm_.prologue(stackSize);
        }
        
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
    void* globalsData_;
    void* callRuntimeFunc_;
    Label deoptTrampolineLabel_;
    bool needsDeoptTrampoline_ = false;
    std::unordered_map<LIRBlock*, Label> blockLabels_;

    void registerStackMap(LIRInst* inst) {
        if (!inst->hasBailoutId()) return;
        StackMap map;
        map.bailoutId = inst->bailoutId();
        map.bytecodeIp = inst->bailoutId();
        for (const auto& fsUse : inst->fsUses()) {
            StackMapSlot slot;
            slot.location = fsUse.first;
            slot.type = fsUse.second;
            map.locals.push_back(slot);
        }
        DeoptRegistry::get().addStackMap(map);
    }

    Operand getStackOperand(int32_t slot) {
        // 假设栈槽从 rbp 向下分配，slot 是 0, 8, 16...
        // 并且 prologue 压入了 7 个 callee-saved 寄存器 (56 字节)
        // 第一个槽位是 rbp - 64
        return Operand(rbp, -slot - 64);
    }

    void emitEagerSync(LIRInst* inst) {
        if (!inst->hasBailoutId()) return;
        masm_.emitPushAll();
#ifdef _WIN32
        masm_.movq(rcx, rsp);
        masm_.mov(rdx, static_cast<int32_t>(inst->bailoutId()));
#else
        masm_.movq(rdi, rsp);
        masm_.mov(rsi, static_cast<int32_t>(inst->bailoutId()));
#endif
        masm_.callCFunction(reinterpret_cast<void*>(jc2_jit_sync_frame));
        masm_.emitPopAll();
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
                } else if (dst.isStackSlot() && src.isStackSlot()) {
                    masm_.movq(r11, getStackOperand(src.slot()));
                    masm_.movq(getStackOperand(dst.slot()), r11);
                } else if (dst.isPhysicalGPR() && src.isImm32()) {
                    masm_.mov(dst.pregGPR(), src.imm32());
                } else if (dst.isPhysicalGPR() && src.isImm64()) {
                    masm_.movabs(dst.pregGPR(), src.imm64());
                } else if (dst.isStackSlot() && src.isImm32()) {
                    masm_.mov(getStackOperand(dst.slot()), src.imm32());
                } else if (dst.isStackSlot() && src.isImm64()) {
                    masm_.movabs(r11, src.imm64());
                    masm_.movq(getStackOperand(dst.slot()), r11);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported Move operands.");
                }
                break;
            }
            case LIROpcode::ParallelMove: {
                size_t n = inst->defs().size();
                std::vector<bool> ready(n, false);
                std::vector<bool> todo(n, true);
                
                auto emitSingleMove = [&](const LIROperand& dst, const LIROperand& src) {
                    if (dst == src) return;
                    if (dst.isPhysicalGPR() && src.isPhysicalGPR()) {
                        masm_.movq(dst.pregGPR(), src.pregGPR());
                    } else if (dst.isPhysicalGPR() && src.isStackSlot()) {
                        masm_.movq(dst.pregGPR(), getStackOperand(src.slot()));
                    } else if (dst.isStackSlot() && src.isPhysicalGPR()) {
                        masm_.movq(getStackOperand(dst.slot()), src.pregGPR());
                    } else if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                        masm_.movsd(dst.pregXMM(), src.pregXMM());
                    } else if (dst.isPhysicalXMM() && src.isStackSlot()) {
                        masm_.movsd(dst.pregXMM(), getStackOperand(src.slot()));
                    } else if (dst.isStackSlot() && src.isPhysicalXMM()) {
                        masm_.movsd(getStackOperand(dst.slot()), src.pregXMM());
                    } else if (dst.isStackSlot() && src.isStackSlot()) {
                        masm_.movq(r11, getStackOperand(src.slot()));
                        masm_.movq(getStackOperand(dst.slot()), r11);
                    } else if (dst.isPhysicalGPR() && src.isImm32()) {
                        masm_.mov(dst.pregGPR(), src.imm32());
                    } else if (dst.isPhysicalGPR() && src.isImm64()) {
                        masm_.movabs(dst.pregGPR(), src.imm64());
                    } else if (dst.isStackSlot() && src.isImm32()) {
                        masm_.mov(getStackOperand(dst.slot()), src.imm32());
                    } else if (dst.isStackSlot() && src.isImm64()) {
                        masm_.movabs(r11, src.imm64());
                        masm_.movq(getStackOperand(dst.slot()), r11);
                    } else {
                        throw std::runtime_error("CodeEmitter: Unsupported ParallelMove operand combination.");
                    }
                };

                bool progress = true;
                while (progress) {
                    progress = false;
                    for (size_t i = 0; i < n; ++i) {
                        if (!todo[i]) continue;
                        
                        bool isUsed = false;
                        for (size_t j = 0; j < n; ++j) {
                            if (i != j && todo[j] && inst->uses()[j] == inst->defs()[i]) {
                                isUsed = true;
                                break;
                            }
                        }
                        
                        if (!isUsed) {
                            emitSingleMove(inst->defs()[i], inst->uses()[i]);
                            todo[i] = false;
                            ready[i] = true;
                            progress = true;
                        }
                    }
                }
                
                for (size_t i = 0; i < n; ++i) {
                    if (todo[i]) {
                        LIROperand cycleStart = inst->defs()[i];
                        bool isFloat = false;
                        if (cycleStart.isPhysicalXMM()) isFloat = true;
                        else if (cycleStart.isStackSlot()) {
                            for (size_t j = 0; j < n; ++j) {
                                if (inst->defs()[j] == cycleStart && inst->uses()[j].isPhysicalXMM()) {
                                    isFloat = true; break;
                                }
                            }
                        }
                        
                        if (isFloat) {
                            if (cycleStart.isPhysicalXMM()) {
                                masm_.movsd(xmm15, cycleStart.pregXMM());
                            } else {
                                masm_.movsd(xmm15, getStackOperand(cycleStart.slot()));
                            }
                        } else {
                            if (cycleStart.isPhysicalGPR()) {
                                masm_.movq(r11, cycleStart.pregGPR());
                            } else {
                                masm_.movq(r11, getStackOperand(cycleStart.slot()));
                            }
                        }
                        
                        size_t curr = i;
                        while (todo[curr]) {
                            todo[curr] = false;
                            size_t next = n;
                            for (size_t j = 0; j < n; ++j) {
                                if (todo[j] && inst->defs()[j] == inst->uses()[curr]) {
                                    next = j;
                                    break;
                                }
                            }
                            
                            if (next != n) {
                                emitSingleMove(inst->defs()[curr], inst->uses()[curr]);
                                curr = next;
                            } else {
                                if (isFloat) {
                                    if (inst->defs()[curr].isPhysicalXMM()) {
                                        masm_.movsd(inst->defs()[curr].pregXMM(), xmm15);
                                    } else {
                                        masm_.movsd(getStackOperand(inst->defs()[curr].slot()), xmm15);
                                    }
                                } else {
                                    if (inst->defs()[curr].isPhysicalGPR()) {
                                        masm_.movq(inst->defs()[curr].pregGPR(), r11);
                                    } else {
                                        masm_.movq(getStackOperand(inst->defs()[curr].slot()), r11);
                                    }
                                }
                                break;
                            }
                        }
                    }
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
                } else if (dst.isPhysicalXMM()) {
                    masm_.movabs(r11, src.imm64());
                    masm_.emit8(0x66);
                    masm_.emitRex(true, dst.pregXMM(), r11);
                    masm_.emit8(0x0F);
                    masm_.emit8(0x6E);
                    masm_.emitModRM(3, dst.pregXMM().id(), r11.id());
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
                if (inst->hasBailoutId()) {
                    needsDeoptTrampoline_ = true;
                    Label noOverflow;
                    masm_.jcc(Condition::NoOverflow, noOverflow);
                    masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                    masm_.jmp(deoptTrampolineLabel_);
                    masm_.bind(noOverflow);
                    
                    registerStackMap(inst);
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
                if (inst->hasBailoutId()) {
                    needsDeoptTrampoline_ = true;
                    Label noOverflow;
                    masm_.jcc(Condition::NoOverflow, noOverflow);
                    masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                    masm_.jmp(deoptTrampolineLabel_);
                    masm_.bind(noOverflow);
                    
                    registerStackMap(inst);
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
                if (inst->hasBailoutId()) {
                    needsDeoptTrampoline_ = true;
                    Label noOverflow;
                    masm_.jcc(Condition::NoOverflow, noOverflow);
                    masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                    masm_.jmp(deoptTrampolineLabel_);
                    masm_.bind(noOverflow);
                    
                    registerStackMap(inst);
                }
                break;
            }
            case LIROpcode::DivI32: {
                const LIROperand& src = inst->uses()[1];
                masm_.cdq();
                if (src.isPhysicalGPR()) {
                    masm_.idiv(src.pregGPR());
                } else if (src.isStackSlot()) {
                    masm_.idiv(getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported DivI32 operand.");
                }
                if (inst->hasBailoutId()) {
                    needsDeoptTrampoline_ = true;
                    Label isZero;
                    masm_.test(rdx, rdx);
                    masm_.jcc(Condition::Zero, isZero);
                    masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                    masm_.jmp(deoptTrampolineLabel_);
                    masm_.bind(isZero);
                    
                    registerStackMap(inst);
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
                    masm_.neg(dst.pregGPR());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported NegI32 destination.");
                }
                break;
            }
            case LIROpcode::NotI32: {
                const LIROperand& dst = inst->defs()[0];
                if (dst.isPhysicalGPR()) {
                    masm_.not_(dst.pregGPR());
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
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    if (dst.pregXMM() != src.pregXMM()) {
                        masm_.movsd(dst.pregXMM(), src.pregXMM());
                    }
                    Label& maskLbl = masm_.addConstant64(0x8000000000000000ULL);
                    masm_.xorpd(dst.pregXMM(), maskLbl);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported NegF64 operands.");
                }
                break;
            }
            case LIROpcode::SqrtF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.sqrtsd(dst.pregXMM(), src.pregXMM());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported SqrtF64 operands.");
                }
                break;
            }
            case LIROpcode::AbsF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    if (dst.pregXMM() != src.pregXMM()) {
                        masm_.movsd(dst.pregXMM(), src.pregXMM());
                    }
                    Label& maskLbl = masm_.addConstant64(0x7FFFFFFFFFFFFFFFULL);
                    masm_.andpd(dst.pregXMM(), maskLbl);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported AbsF64 operands.");
                }
                break;
            }
            case LIROpcode::FloorF64:
            case LIROpcode::CeilF64:
            case LIROpcode::RoundF64:
            case LIROpcode::TruncF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                uint8_t mode = 0;
                if (inst->opcode() == LIROpcode::FloorF64) mode = 1;
                else if (inst->opcode() == LIROpcode::CeilF64) mode = 2;
                else if (inst->opcode() == LIROpcode::TruncF64) mode = 3;
                else mode = 0; // Round to nearest

                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.roundsd(dst.pregXMM(), src.pregXMM(), mode);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported Rounding operands.");
                }
                break;
            }
            case LIROpcode::SinF64:
            case LIROpcode::CosF64: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalXMM() && src.isPhysicalXMM()) {
                    masm_.subq(rsp, 8);
                    masm_.movsd(Operand(rsp, 0), src.pregXMM());
                    masm_.fld_d(Operand(rsp, 0));
                    if (inst->opcode() == LIROpcode::SinF64) masm_.fsin();
                    else masm_.fcos();
                    masm_.fstp_d(Operand(rsp, 0));
                    masm_.movsd(dst.pregXMM(), Operand(rsp, 0));
                    masm_.addq(rsp, 8);
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported Sin/Cos operands.");
                }
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
                    masm_.setcc(inst->condition(), dst.pregGPR());
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
                
                registerStackMap(inst);
                break;
            }
            case LIROpcode::LoadGlobal: {
                const LIROperand& dst = inst->defs()[0];
                int32_t slot = inst->uses()[0].imm32();
                if (!globalsData_) throw std::runtime_error("CodeEmitter: globalsData is null.");
                uint64_t addr = reinterpret_cast<uint64_t>(globalsData_) + slot * sizeof(uint64_t);
                masm_.movabs(r11, addr);
                if (dst.isPhysicalGPR()) {
                    masm_.movq(dst.pregGPR(), Operand(r11, 0));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported LoadGlobal destination.");
                }
                break;
            }
            case LIROpcode::LoadField: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& base = inst->uses()[0];
                const LIROperand& offset = inst->uses()[1];
                
                if (!base.isPhysicalGPR() || !dst.isPhysicalGPR()) {
                    throw std::runtime_error("CodeEmitter: LoadField requires GPR for base and dst.");
                }
                
                // 剥离 NaN-Boxing 掩码，还原真实的 48 位对象指针并符号扩展
                masm_.movq(r11, base.pregGPR());
                masm_.shlq(r11, 16);
                masm_.sarq(r11, 16);
                
                if (offset.isImm32()) {
                    masm_.movq(dst.pregGPR(), Operand(r11, offset.imm32()));
                } else if (offset.isPhysicalGPR()) {
                    masm_.movq(dst.pregGPR(), Operand(r11, offset.pregGPR(), Scale::Times1, 0));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported LoadField offset.");
                }
                break;
            }
            case LIROpcode::StoreField: {
                const LIROperand& base = inst->uses()[0];
                const LIROperand& offset = inst->uses()[1];
                const LIROperand& val = inst->uses()[2];
                
                if (!base.isPhysicalGPR() || !val.isPhysicalGPR()) {
                    throw std::runtime_error("CodeEmitter: StoreField requires GPR for base and val.");
                }
                
                // 剥离 NaN-Boxing 掩码，还原真实的 48 位对象指针并符号扩展
                masm_.movq(r11, base.pregGPR());
                masm_.shlq(r11, 16);
                masm_.sarq(r11, 16);
                
                if (offset.isImm32()) {
                    masm_.movq(Operand(r11, offset.imm32()), val.pregGPR());
                } else if (offset.isPhysicalGPR()) {
                    masm_.movq(Operand(r11, offset.pregGPR(), Scale::Times1, 0), val.pregGPR());
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported StoreField offset.");
                }
                break;
            }
            case LIROpcode::GuardIsInt32: {
                const LIROperand& val = inst->uses()[0];
                if (!val.isPhysicalGPR()) throw std::runtime_error("CodeEmitter: GuardIsInt32 requires GPR.");
                needsDeoptTrampoline_ = true;
                registerStackMap(inst);
                
                masm_.movq(r11, val.pregGPR());
                masm_.movabs(r10, 0xFFFFFFFF00000000ULL);
                masm_.andq(r11, r10);
                masm_.movabs(r10, 0x7FFC000100000000ULL);
                masm_.cmpq(r11, r10);
                Label isInt32;
                masm_.jcc(Condition::Equal, isInt32);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isInt32);
                break;
            }
            case LIROpcode::GuardIsDouble: {
                const LIROperand& val = inst->uses()[0];
                if (!val.isPhysicalGPR()) throw std::runtime_error("CodeEmitter: GuardIsDouble requires GPR.");
                needsDeoptTrampoline_ = true;
                registerStackMap(inst);
                
                masm_.movq(r11, val.pregGPR());
                masm_.movabs(r10, 0x7FFC000000000000ULL);
                masm_.andq(r11, r10);
                masm_.cmpq(r11, r10);
                Label isDouble;
                masm_.jcc(Condition::NotEqual, isDouble);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isDouble);
                break;
            }
            case LIROpcode::GuardIsBool: {
                const LIROperand& val = inst->uses()[0];
                if (!val.isPhysicalGPR()) throw std::runtime_error("CodeEmitter: GuardIsBool requires GPR.");
                needsDeoptTrampoline_ = true;
                registerStackMap(inst);
                
                masm_.movq(r11, val.pregGPR());
                masm_.movabs(r10, 0xFFFFFFFFFFFFFFFEULL);
                masm_.andq(r11, r10);
                masm_.movabs(r10, 0x7FFC000000000002ULL);
                masm_.cmpq(r11, r10);
                Label isBool;
                masm_.jcc(Condition::Equal, isBool);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isBool);
                break;
            }
            case LIROpcode::GuardIsString: {
                const LIROperand& val = inst->uses()[0];
                if (!val.isPhysicalGPR()) throw std::runtime_error("CodeEmitter: GuardIsString requires GPR.");
                needsDeoptTrampoline_ = true;
                registerStackMap(inst);
                
                // 1. Check if Obj
                masm_.movq(r11, val.pregGPR());
                masm_.movabs(r10, 0xFFFC000000000000ULL);
                masm_.andq(r11, r10);
                masm_.cmpq(r11, r10);
                Label isObj;
                masm_.jcc(Condition::Equal, isObj);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isObj);
                
                // 2. Extract pointer
                masm_.movq(r11, val.pregGPR());
                masm_.shlq(r11, 16);
                masm_.sarq(r11, 16);
                
                // 3. Check ObjType == STRING (1)
                masm_.cmp(Operand(r11, 4), 1);
                Label isString;
                masm_.jcc(Condition::Equal, isString);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isString);
                break;
            }
            case LIROpcode::GuardIsObject: {
                const LIROperand& val = inst->uses()[0];
                if (!val.isPhysicalGPR()) throw std::runtime_error("CodeEmitter: GuardIsObject requires GPR.");
                needsDeoptTrampoline_ = true;
                registerStackMap(inst);
                
                masm_.movq(r11, val.pregGPR());
                masm_.movabs(r10, 0xFFFC000000000000ULL);
                masm_.andq(r11, r10);
                masm_.cmpq(r11, r10);
                Label isObj;
                masm_.jcc(Condition::Equal, isObj);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isObj);
                break;
            }
            case LIROpcode::GuardTruthy: {
                throw std::runtime_error("CodeEmitter: GuardTruthy not implemented.");
                break;
            }
            case LIROpcode::GuardIsClass: {
                const LIROperand& obj = inst->uses()[0];
                uint64_t expectedClassId = inst->uses()[1].imm64();
                int32_t typeOffset = inst->uses()[2].imm32();
                int32_t classDefOffset = inst->uses()[3].imm32();
                int32_t classIdOffset = inst->uses()[4].imm32();
                
                if (!obj.isPhysicalGPR()) throw std::runtime_error("CodeEmitter: GuardIsClass requires GPR.");
                
                needsDeoptTrampoline_ = true;
                registerStackMap(inst);
                
                // 1. 检查是否为 Obj (带有 SIGN_BIT | QNAN)
                masm_.movq(r11, obj.pregGPR());
                masm_.movabs(r10, 0xFFFC000000000000ULL);
                masm_.andq(r11, r10);
                masm_.cmpq(r11, r10);
                Label isObj;
                masm_.jcc(Condition::Equal, isObj);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isObj);
                
                // 2. 提取对象指针并符号扩展
                masm_.movq(r11, obj.pregGPR());
                masm_.shlq(r11, 16);
                masm_.sarq(r11, 16);
                
                // 3. 检查 ObjType 是否为 INSTANCE (枚举值 14)
                masm_.cmp(Operand(r11, typeOffset), 14);
                Label isInst;
                masm_.jcc(Condition::Equal, isInst);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isInst);
                
                // 4. 获取 classDef 指针
                masm_.movq(r11, Operand(r11, classDefOffset));
                
                // 5. 检查 classId
                masm_.movabs(r10, expectedClassId);
                masm_.cmpq(Operand(r11, classIdOffset), r10);
                Label isClassMatch;
                masm_.jcc(Condition::Equal, isClassMatch);
                masm_.mov(r10, static_cast<int32_t>(inst->bailoutId()));
                masm_.jmp(deoptTrampolineLabel_);
                masm_.bind(isClassMatch);
                
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
                    // 修复：MOVD r/m64, xmm 指令中，xmm 是 reg 字段，GPR 是 rm 字段
                    // 因此 REX.R 应该扩展 xmm，REX.B 应该扩展 GPR
                    masm_.emitRex(true, src.pregXMM(), dst.pregGPR());
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
            case LIROpcode::Int32ToDouble: {
                const LIROperand& dst = inst->defs()[0];
                const LIROperand& src = inst->uses()[0];
                if (dst.isPhysicalXMM() && src.isPhysicalGPR()) {
                    masm_.cvtsi2sd(dst.pregXMM(), src.pregGPR());
                } else if (dst.isPhysicalXMM() && src.isStackSlot()) {
                    masm_.cvtsi2sd(dst.pregXMM(), getStackOperand(src.slot()));
                } else {
                    throw std::runtime_error("CodeEmitter: Unsupported Int32ToDouble operands.");
                }
                break;
            }
            case LIROpcode::Call: {
                emitEagerSync(inst);
                uint32_t argc = inst->argc();
                const LIROperand& callee = inst->uses()[0];
                
                if (argc > 0) {
                    uint32_t stackSpace = (argc * 8 + 15) & ~15;
                    masm_.subq(rsp, stackSpace);
                    
                    for (uint32_t i = 0; i < argc; ++i) {
                        const LIROperand& arg = inst->uses()[i + 1];
                        if (arg.isPhysicalGPR()) {
                            masm_.movq(Operand(rsp, i * 8), arg.pregGPR());
                        } else if (arg.isStackSlot()) {
                            masm_.movq(r11, getStackOperand(arg.slot()));
                            masm_.movq(Operand(rsp, i * 8), r11);
                        } else if (arg.isImm64()) {
                            masm_.movabs(r11, arg.imm64());
                            masm_.movq(Operand(rsp, i * 8), r11);
                        } else {
                            throw std::runtime_error("CodeEmitter: Unsupported Call argument type.");
                        }
                    }
                }
                
#ifdef _WIN32
                if (argc > 0) masm_.movq(r8, rsp); else masm_.movq(r8, 0);
                if (callee.isPhysicalGPR()) masm_.movq(rcx, callee.pregGPR());
                else if (callee.isStackSlot()) masm_.movq(rcx, getStackOperand(callee.slot()));
                else throw std::runtime_error("CodeEmitter: Unsupported Call callee type.");
                masm_.movq(rdx, r14);
                masm_.mov(r9, static_cast<int32_t>(argc));
#else
                if (argc > 0) masm_.movq(rdx, rsp); else masm_.movq(rdx, 0);
                if (callee.isPhysicalGPR()) masm_.movq(rdi, callee.pregGPR());
                else if (callee.isStackSlot()) masm_.movq(rdi, getStackOperand(callee.slot()));
                else throw std::runtime_error("CodeEmitter: Unsupported Call callee type.");
                masm_.movq(rsi, r14);
                masm_.mov(rcx, static_cast<int32_t>(argc));
#endif
                
                if (!callRuntimeFunc_) throw std::runtime_error("CodeEmitter: callRuntimeFunc is null.");
                masm_.callCFunction(callRuntimeFunc_);
                
                if (argc > 0) {
                    uint32_t stackSpace = (argc * 8 + 15) & ~15;
                    masm_.addq(rsp, stackSpace);
                }
                break;
            }
            case LIROpcode::Callout: {
                emitEagerSync(inst);
                uint32_t argc = inst->argc();
                std::vector<Register> argRegs;
#ifdef _WIN32
                argRegs = {rcx, rdx, r8, r9};
#else
                argRegs = {rdi, rsi, rdx, rcx, r8, r9};
#endif
                if (argc > argRegs.size() + 2) throw std::runtime_error("CodeEmitter: Callout with too many arguments.");
                
                // Push all arguments to stack to avoid register swap problems
                for (uint32_t i = 0; i < argc; ++i) {
                    const LIROperand& arg = inst->uses()[i];
                    if (arg.isPhysicalGPR()) {
                        masm_.push(arg.pregGPR());
                    } else if (arg.isStackSlot()) {
                        masm_.push(getStackOperand(arg.slot()));
                    } else if (arg.isImm32()) {
                        masm_.push(arg.imm32());
                    } else if (arg.isImm64()) {
                        masm_.movabs(r11, arg.imm64());
                        masm_.push(r11);
                    } else {
                        throw std::runtime_error("CodeEmitter: Unsupported Callout argument type.");
                    }
                }
                
                // Pop them into the correct registers (in reverse order)
                for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
                    if (i < static_cast<int>(argRegs.size())) {
                        masm_.pop(argRegs[i]);
                    } else if (i == static_cast<int>(argRegs.size())) {
                        masm_.pop(r10);
                    } else if (i == static_cast<int>(argRegs.size()) + 1) {
                        masm_.pop(r11);
                    }
                }
                
                Register stackArg1 = Register();
                Register stackArg2 = Register();
                if (argc > argRegs.size()) stackArg1 = r10;
                if (argc > argRegs.size() + 1) stackArg2 = r11;
                
                masm_.callCFunction(inst->functionPtr(), argc, stackArg1, stackArg2);
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
