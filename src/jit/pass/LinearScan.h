#ifndef JC2_JIT_LINEAR_SCAN_H
#define JC2_JIT_LINEAR_SCAN_H

#include "../ir/LIR.h"
#include "LivenessAnalysis.h"
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace jc {
namespace jit {

// ============================================================================
// 线性扫描寄存器分配器 (Linear Scan Register Allocator) (Step 41)
// ============================================================================
class LinearScanAllocator {
public:
    LinearScanAllocator(LIRGraph& lir, LivenessAnalyzer& liveness)
        : lir_(lir), liveness_(liveness) {}

    int32_t getStackSize() const { return nextStackSlot_; }

    void allocate() {
        std::vector<LiveInterval*> unhandled;
        for (auto& pair : liveness_.intervalsMut()) {
            unhandled.push_back(&pair.second);
        }

        // 按起始位置升序排序
        std::sort(unhandled.begin(), unhandled.end(), [](LiveInterval* a, LiveInterval* b) {
            return a->ranges.front().start < b->ranges.front().start;
        });

        std::vector<LiveInterval*> activeGPR;
        std::vector<LiveInterval*> activeXMM;

        // 初始化空闲物理寄存器池 (排除 RSP, RBP, R14(专用 frameRegs), 以及作为 Scratch 寄存器的 R10, R11)
        std::vector<Register> freeGPRs = { r15, r13, r12, r9, r8, rdi, rsi, rbx, rdx, rcx, rax };
#ifdef _WIN32
        // Windows x64 ABI: xmm6-xmm15 are callee-saved. Since our prologue doesn't save them, we exclude them.
        // We also exclude xmm4 and xmm5 to use them as scratch registers.
        std::vector<XMMRegister> freeXMMs = { xmm3, xmm2, xmm1, xmm0 };
#else
        // System V ABI: all xmm registers are caller-saved.
        // We exclude xmm4 and xmm5 to use them as scratch registers.
        std::vector<XMMRegister> freeXMMs = { xmm15, xmm14, xmm13, xmm12, xmm11, xmm10, xmm9, xmm8, xmm7, xmm6, xmm3, xmm2, xmm1, xmm0 };
#endif

        buildClobbers();

        for (LiveInterval* interval : unhandled) {
            bool isFloat = lir_.isVRegFloat(interval->vreg);
            auto& active = isFloat ? activeXMM : activeGPR;

            expireOldIntervals(interval, active, isFloat ? nullptr : &freeGPRs, isFloat ? &freeXMMs : nullptr);

            if (isFloat) {
                XMMRegister chosenReg;
                bool found = false;
                for (auto it = freeXMMs.begin(); it != freeXMMs.end(); ++it) {
                    if (!isClobberedXMM(*it, interval)) {
                        chosenReg = *it;
                        freeXMMs.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    spillAtInterval(interval, active, nullptr, &freeXMMs);
                } else {
                    interval->allocatedXMM = chosenReg;
                    active.push_back(interval);
                }
            } else {
                Register chosenReg;
                bool found = false;
                for (auto it = freeGPRs.begin(); it != freeGPRs.end(); ++it) {
                    if (!isClobbered(*it, interval)) {
                        chosenReg = *it;
                        freeGPRs.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    spillAtInterval(interval, active, &freeGPRs, nullptr);
                } else {
                    interval->allocatedGPR = chosenReg;
                    active.push_back(interval);
                }
            }

            // 保持 active 列表按结束位置升序排序
            std::sort(active.begin(), active.end(), [](LiveInterval* a, LiveInterval* b) {
                return a->ranges.back().end < b->ranges.back().end;
            });
        }

        rewriteInstructions();
    }

private:
    LIRGraph& lir_;
    LivenessAnalyzer& liveness_;
    int32_t nextStackSlot_ = 0;
    std::vector<std::pair<uint32_t, Register>> clobberPoints_;
    std::vector<std::pair<uint32_t, XMMRegister>> xmmClobberPoints_;

    void buildClobbers() {
        for (LIRBlock* block : lir_.blocks()) {
            for (LIRInst* inst : block->instructions()) {
                if (inst->opcode() == LIROpcode::Call || inst->opcode() == LIROpcode::Callout) {
                    // Caller-saved GPRs
                    clobberPoints_.push_back({inst->linearId(), rax});
                    clobberPoints_.push_back({inst->linearId(), rcx});
                    clobberPoints_.push_back({inst->linearId(), rdx});
                    clobberPoints_.push_back({inst->linearId(), r8});
                    clobberPoints_.push_back({inst->linearId(), r9});
                    clobberPoints_.push_back({inst->linearId(), r10});
                    clobberPoints_.push_back({inst->linearId(), r11});
#ifndef _WIN32
                    // System V ABI: rsi and rdi are caller-saved
                    clobberPoints_.push_back({inst->linearId(), rsi});
                    clobberPoints_.push_back({inst->linearId(), rdi});
#endif
                    
                    // Caller-saved XMMs
                    xmmClobberPoints_.push_back({inst->linearId(), xmm0});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm1});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm2});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm3});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm4});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm5});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm6});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm7});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm8});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm9});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm10});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm11});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm12});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm13});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm14});
                    xmmClobberPoints_.push_back({inst->linearId(), xmm15});
                }
                for (Register reg : inst->clobbers()) {
                    clobberPoints_.push_back({inst->linearId(), reg});
                }
                for (const auto& constraint : inst->defConstraints()) {
                    if (constraint.type == LIRConstraintType::FixedReg) {
                        clobberPoints_.push_back({inst->linearId(), Register(constraint.value)});
                    }
                }
                for (const auto& constraint : inst->useConstraints()) {
                    if (constraint.type == LIRConstraintType::FixedReg) {
                        clobberPoints_.push_back({inst->linearId(), Register(constraint.value)});
                    }
                }
            }
        }
    }

    bool isClobbered(Register reg, LiveInterval* interval) {
        for (const auto& cp : clobberPoints_) {
            if (cp.second == reg) {
                for (const auto& range : interval->ranges) {
                    if (cp.first >= range.start && cp.first < range.end) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool isClobberedXMM(XMMRegister reg, LiveInterval* interval) {
        for (const auto& cp : xmmClobberPoints_) {
            if (cp.second == reg) {
                for (const auto& range : interval->ranges) {
                    if (cp.first >= range.start && cp.first < range.end) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void expireOldIntervals(LiveInterval* current, std::vector<LiveInterval*>& active, 
                            std::vector<Register>* freeGPRs, std::vector<XMMRegister>* freeXMMs) {
        auto it = active.begin();
        while (it != active.end()) {
            LiveInterval* act = *it;
            if (act->ranges.back().end <= current->ranges.front().start) {
                if (freeGPRs && act->allocatedSlot == -1) freeGPRs->push_back(act->allocatedGPR);
                if (freeXMMs && act->allocatedSlot == -1) freeXMMs->push_back(act->allocatedXMM);
                it = active.erase(it);
            } else {
                // 因为 active 是按结束位置排序的，如果当前区间没有过期，后面的也不会过期
                break;
            }
        }
    }

    void spillAtInterval(LiveInterval* current, std::vector<LiveInterval*>& active, 
                         std::vector<Register>* freeGPRs, std::vector<XMMRegister>* freeXMMs) {
        auto bestIt = active.end();
        for (auto it = active.begin(); it != active.end(); ++it) {
            LiveInterval* candidate = *it;
            bool clobbered = false;
            if (freeGPRs) {
                clobbered = isClobbered(candidate->allocatedGPR, current);
            } else if (freeXMMs) {
                clobbered = isClobberedXMM(candidate->allocatedXMM, current);
            }
            
            if (!clobbered) {
                if (bestIt == active.end() || candidate->spillWeight < (*bestIt)->spillWeight || 
                   (candidate->spillWeight == (*bestIt)->spillWeight && candidate->ranges.back().end > (*bestIt)->ranges.back().end)) {
                    bestIt = it;
                }
            }
        }
        
        if (bestIt != active.end() && (*bestIt)->spillWeight <= current->spillWeight) {
            LiveInterval* spillCandidate = *bestIt;
            spillCandidate->allocatedSlot = nextStackSlot_;
            nextStackSlot_ += 8;
            
            if (freeGPRs) {
                current->allocatedGPR = spillCandidate->allocatedGPR;
                // 保留 spillCandidate->allocatedGPR：溢出 store 之前的 deopt 点还要用物理寄存器恢复
            }
            if (freeXMMs) {
                current->allocatedXMM = spillCandidate->allocatedXMM;
            }
            active.erase(bestIt);
            active.push_back(current);
        } else {
            current->allocatedSlot = nextStackSlot_;
            nextStackSlot_ += 8;
        }
    }

    void rewriteInstructions() {
        // 预扫描：提前记录每个被溢出 vreg 的溢出 store 位置（其第一个 def 的 linearId）。
        // 必须在主循环之前完成，否则主循环里靠前的 deopt 点会看到 spillPos 还是 -1。
        for (LIRBlock* block : lir_.blocks()) {
            for (LIRInst* inst : block->instructions()) {
                for (const auto& def : inst->defs()) {
                    if (def.isVirtual()) {
                        LiveInterval& interval = liveness_.intervalsMut().at(def.vreg());
                        if (interval.allocatedSlot != -1 && interval.spillPos == -1) {
                            interval.spillPos = static_cast<int32_t>(inst->linearId());
                        }
                    }
                }
            }
        }

        for (LIRBlock* block : lir_.blocks()) {
            std::vector<LIRInst*> newInsts;
            for (LIRInst* inst : block->instructions()) {
                std::vector<LIRInst*> postInsts;
                Register scratchGPRs[] = { r10, r11 };
                XMMRegister scratchXMMs[] = { xmm4, xmm5 };
                int scratchGPRIdx = 0;
                int scratchXMMIdx = 0;

                auto getScratchGPR = [&]() {
                    if (scratchGPRIdx >= 2) throw std::runtime_error("JIT Error: Out of scratch GPRs.");
                    return scratchGPRs[scratchGPRIdx++];
                };
                
                auto getScratchXMM = [&]() {
                    if (scratchXMMIdx >= 2) throw std::runtime_error("JIT Error: Out of scratch XMMs.");
                    return scratchXMMs[scratchXMMIdx++];
                };

                // Resolve uses
                for (size_t i = 0; i < inst->uses().size(); ++i) {
                    LIROperand& use = inst->usesMut()[i];
                    LIRConstraint constraint = inst->useConstraints()[i];
                    if (use.isVirtual()) {
                        LiveInterval& interval = liveness_.intervalsMut().at(use.vreg());
                        if (interval.allocatedSlot != -1) {
                            // Spilled
                            if (inst->opcode() == LIROpcode::Move || inst->opcode() == LIROpcode::ParallelMove || constraint.type == LIRConstraintType::None) {
                                use = LIROperand::createStackSlot(interval.allocatedSlot);
                            } else {
                                LIROperand stackOp = LIROperand::createStackSlot(interval.allocatedSlot);
                                LIROperand regOp;
                                if (lir_.isVRegFloat(use.vreg())) {
                                    XMMRegister reg = (constraint.type == LIRConstraintType::FixedReg) ? XMMRegister(constraint.value) : getScratchXMM();
                                    regOp = LIROperand::createPhysicalXMM(reg);
                                } else {
                                    Register reg = (constraint.type == LIRConstraintType::FixedReg) ? Register(constraint.value) : getScratchGPR();
                                    regOp = LIROperand::createPhysicalGPR(reg);
                                }
                                
                                auto moveInst = lir_.allocateInst(0, LIROpcode::Move);
                                moveInst->addDef(regOp);
                                moveInst->addUse(stackOp);
                                newInsts.push_back(moveInst);
                                
                                use = regOp;
                            }
                        } else if (lir_.isVRegFloat(use.vreg())) {
                            use = LIROperand::createPhysicalXMM(interval.allocatedXMM);
                        } else {
                            LIROperand physOp = LIROperand::createPhysicalGPR(interval.allocatedGPR);
                            if (constraint.type == LIRConstraintType::FixedReg) {
                                Register fixedReg(constraint.value);
                                LIROperand fixedOp = LIROperand::createPhysicalGPR(fixedReg);
                                if (physOp != fixedOp) {
                                    auto moveInst = lir_.allocateInst(0, LIROpcode::Move);
                                    moveInst->addDef(fixedOp);
                                    moveInst->addUse(physOp);
                                    newInsts.push_back(moveInst);
                                    physOp = fixedOp;
                                }
                            }
                            use = physOp;
                        }
                    }
                }
                
                // Resolve defs
                for (size_t i = 0; i < inst->defs().size(); ++i) {
                    LIROperand& def = inst->defsMut()[i];
                    LIRConstraint constraint = inst->defConstraints()[i];
                    if (def.isVirtual()) {
                        LiveInterval& interval = liveness_.intervalsMut().at(def.vreg());
                        if (interval.allocatedSlot != -1) {
                            // Spilled
                            if (inst->opcode() == LIROpcode::Move || inst->opcode() == LIROpcode::ParallelMove) {
                                def = LIROperand::createStackSlot(interval.allocatedSlot);
                                if (interval.spillPos == -1) interval.spillPos = static_cast<int32_t>(inst->linearId());
                            } else {
                                LIROperand stackOp = LIROperand::createStackSlot(interval.allocatedSlot);
                                LIROperand regOp;
                                if (lir_.isVRegFloat(def.vreg())) {
                                    XMMRegister reg;
                                    if (constraint.type == LIRConstraintType::FixedReg) {
                                        reg = XMMRegister(constraint.value);
                                    } else if (constraint.type == LIRConstraintType::SameAsInput) {
                                        reg = getScratchXMM();
                                        auto moveInst = lir_.allocateInst(0, LIROpcode::Move);
                                        moveInst->addDef(LIROperand::createPhysicalXMM(reg));
                                        moveInst->addUse(inst->uses()[constraint.value]);
                                        newInsts.push_back(moveInst);
                                        inst->usesMut()[constraint.value] = LIROperand::createPhysicalXMM(reg);
                                    } else {
                                        reg = getScratchXMM();
                                    }
                                    regOp = LIROperand::createPhysicalXMM(reg);
                                } else {
                                    Register reg;
                                    if (constraint.type == LIRConstraintType::FixedReg) {
                                        reg = Register(constraint.value);
                                    } else if (constraint.type == LIRConstraintType::SameAsInput) {
                                        reg = getScratchGPR();
                                        auto moveInst = lir_.allocateInst(0, LIROpcode::Move);
                                        moveInst->addDef(LIROperand::createPhysicalGPR(reg));
                                        moveInst->addUse(inst->uses()[constraint.value]);
                                        newInsts.push_back(moveInst);
                                        inst->usesMut()[constraint.value] = LIROperand::createPhysicalGPR(reg);
                                    } else {
                                        reg = getScratchGPR();
                                    }
                                    regOp = LIROperand::createPhysicalGPR(reg);
                                }
                                
                                auto moveInst = lir_.allocateInst(0, LIROpcode::Move);
                                moveInst->addDef(stackOp);
                                moveInst->addUse(regOp);
                                postInsts.push_back(moveInst);
                                
                                def = regOp;
                            }
                        } else {
                            LIROperand physOp;
                            if (lir_.isVRegFloat(def.vreg())) {
                                physOp = LIROperand::createPhysicalXMM(interval.allocatedXMM);
                            } else {
                                physOp = LIROperand::createPhysicalGPR(interval.allocatedGPR);
                            }
                            
                            if (constraint.type == LIRConstraintType::FixedReg) {
                                LIROperand fixedOp;
                                if (lir_.isVRegFloat(def.vreg())) {
                                    fixedOp = LIROperand::createPhysicalXMM(XMMRegister(constraint.value));
                                } else {
                                    fixedOp = LIROperand::createPhysicalGPR(Register(constraint.value));
                                }
                                
                                if (physOp != fixedOp) {
                                    auto moveInst = lir_.allocateInst(0, LIROpcode::Move);
                                    moveInst->addDef(physOp);
                                    moveInst->addUse(fixedOp);
                                    postInsts.push_back(moveInst);
                                    def = fixedOp;
                                } else {
                                    def = physOp;
                                }
                            } else if (constraint.type == LIRConstraintType::SameAsInput) {
                                LIROperand inputOp = inst->uses()[constraint.value];
                                if (physOp != inputOp) {
                                    auto moveInst = lir_.allocateInst(0, LIROpcode::Move);
                                    moveInst->addDef(physOp);
                                    moveInst->addUse(inputOp);
                                    newInsts.push_back(moveInst);
                                    inst->usesMut()[constraint.value] = physOp;
                                }
                                def = physOp;
                            } else {
                                def = physOp;
                            }
                        }
                    }
                }

                // Resolve fsUses（必须在 Resolve defs 之后：此时才记录好每个 vreg 的溢出位置）
                for (auto& fsUsePair : inst->fsUsesMut()) {
                    LIROperand& use = fsUsePair.first;
                    if (use.isVirtual()) {
                        uint32_t fsvreg = use.vreg();
                        LiveInterval& interval = liveness_.intervalsMut().at(fsvreg);
                        if (interval.allocatedSlot != -1) {
                            // deopt 点在溢出 store 之前：值还在物理寄存器里，用物理寄存器；
                            // 之后：值在栈槽里，用栈槽。
                            if (interval.spillPos != -1 && inst->linearId() < static_cast<uint32_t>(interval.spillPos)) {
                                if (lir_.isVRegFloat(fsvreg)) {
                                    use = LIROperand::createPhysicalXMM(interval.allocatedXMM);
                                } else {
                                    use = LIROperand::createPhysicalGPR(interval.allocatedGPR);
                                }
                            } else {
                                use = LIROperand::createStackSlot(interval.allocatedSlot);
                            }
                        } else if (lir_.isVRegFloat(fsvreg)) {
                            use = LIROperand::createPhysicalXMM(interval.allocatedXMM);
                        } else {
                            use = LIROperand::createPhysicalGPR(interval.allocatedGPR);
                        }
                    }
                }

                newInsts.push_back(inst);
                for (auto post : postInsts) {
                    newInsts.push_back(post);
                }
            }
            block->instructionsMut() = newInsts;
        }
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_LINEAR_SCAN_H
