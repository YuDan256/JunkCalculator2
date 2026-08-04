#ifndef JC2_JIT_LINEAR_SCAN_H
#define JC2_JIT_LINEAR_SCAN_H

#include "LIR.h"
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
        std::vector<XMMRegister> freeXMMs = { xmm15, xmm14, xmm13, xmm12, xmm11, xmm10, xmm9, xmm8, xmm7, xmm6, xmm5, xmm4, xmm3, xmm2, xmm1, xmm0 };

        buildClobbers();

        for (LiveInterval* interval : unhandled) {
            bool isFloat = lir_.isVRegFloat(interval->vreg);
            auto& active = isFloat ? activeXMM : activeGPR;

            expireOldIntervals(interval, active, isFloat ? nullptr : &freeGPRs, isFloat ? &freeXMMs : nullptr);

            if (isFloat) {
                if (freeXMMs.empty()) {
                    spillAtInterval(interval, active, nullptr, &freeXMMs);
                } else {
                    interval->allocatedXMM = freeXMMs.back();
                    freeXMMs.pop_back();
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

    void buildClobbers() {
        for (LIRBlock* block : lir_.blocks()) {
            for (LIRInst* inst : block->instructions()) {
                for (Register reg : inst->clobbers()) {
                    clobberPoints_.push_back({inst->linearId(), reg});
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
        LiveInterval* spillCandidate = active.back(); // active is sorted by end time, so back() ends latest
        if (spillCandidate->ranges.back().end > current->ranges.back().end) {
            // Spill the candidate
            spillCandidate->allocatedSlot = nextStackSlot_;
            nextStackSlot_ += 8; // 8 bytes per slot
            
            if (freeGPRs) {
                current->allocatedGPR = spillCandidate->allocatedGPR;
                spillCandidate->allocatedGPR = Register(); // Invalid
            }
            if (freeXMMs) {
                current->allocatedXMM = spillCandidate->allocatedXMM;
                spillCandidate->allocatedXMM = XMMRegister(); // Invalid
            }
            
            active.pop_back();
            active.push_back(current);
        } else {
            // Spill current
            current->allocatedSlot = nextStackSlot_;
            nextStackSlot_ += 8;
        }
    }

    void rewriteInstructions() {
        for (LIRBlock* block : lir_.blocks()) {
            std::vector<LIRInst*> newInsts;
            for (LIRInst* inst : block->instructions()) {
                std::vector<LIRInst*> postInsts;
                Register scratchGPRs[] = { r10, r11 };
                int scratchIdx = 0;

                auto getScratch = [&]() {
                    if (scratchIdx >= 2) throw std::runtime_error("JIT Error: Out of scratch registers.");
                    return scratchGPRs[scratchIdx++];
                };

                // Resolve uses
                for (size_t i = 0; i < inst->uses().size(); ++i) {
                    LIROperand& use = inst->usesMut()[i];
                    LIRConstraint constraint = inst->useConstraints()[i];
                    if (use.isVirtual()) {
                        LiveInterval& interval = liveness_.intervalsMut().at(use.vreg());
                        if (interval.allocatedSlot != -1) {
                            // Spilled
                            LIROperand stackOp = LIROperand::createStackSlot(interval.allocatedSlot);
                            Register reg = (constraint.type == LIRConstraintType::FixedReg) ? Register(constraint.value) : getScratch();
                            LIROperand regOp = LIROperand::createPhysicalGPR(reg);
                            
                            auto moveInst = new LIRInst(0, LIROpcode::Move);
                            moveInst->addDef(regOp);
                            moveInst->addUse(stackOp);
                            newInsts.push_back(moveInst);
                            
                            use = regOp;
                        } else if (lir_.isVRegFloat(use.vreg())) {
                            use = LIROperand::createPhysicalXMM(interval.allocatedXMM);
                        } else {
                            LIROperand physOp = LIROperand::createPhysicalGPR(interval.allocatedGPR);
                            if (constraint.type == LIRConstraintType::FixedReg) {
                                Register fixedReg(constraint.value);
                                LIROperand fixedOp = LIROperand::createPhysicalGPR(fixedReg);
                                if (physOp != fixedOp) {
                                    auto moveInst = new LIRInst(0, LIROpcode::Move);
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
                            LIROperand stackOp = LIROperand::createStackSlot(interval.allocatedSlot);
                            Register reg;
                            if (constraint.type == LIRConstraintType::FixedReg) {
                                reg = Register(constraint.value);
                            } else if (constraint.type == LIRConstraintType::SameAsInput) {
                                reg = inst->uses()[constraint.value].pregGPR();
                            } else {
                                reg = getScratch();
                            }
                            LIROperand regOp = LIROperand::createPhysicalGPR(reg);
                            
                            auto moveInst = new LIRInst(0, LIROpcode::Move);
                            moveInst->addDef(stackOp);
                            moveInst->addUse(regOp);
                            postInsts.push_back(moveInst);
                            
                            def = regOp;
                        } else if (lir_.isVRegFloat(def.vreg())) {
                            def = LIROperand::createPhysicalXMM(interval.allocatedXMM);
                        } else {
                            LIROperand physOp = LIROperand::createPhysicalGPR(interval.allocatedGPR);
                            if (constraint.type == LIRConstraintType::FixedReg) {
                                Register fixedReg(constraint.value);
                                LIROperand fixedOp = LIROperand::createPhysicalGPR(fixedReg);
                                if (physOp != fixedOp) {
                                    auto moveInst = new LIRInst(0, LIROpcode::Move);
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
                                    auto moveInst = new LIRInst(0, LIROpcode::Move);
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
