#ifndef JC2_JIT_LIVENESS_ANALYSIS_H
#define JC2_JIT_LIVENESS_ANALYSIS_H

#include "../ir/LIR.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace jc {
namespace jit {

// ============================================================================
// 活跃区间范围 (Live Range)
// ============================================================================
struct LiveRange {
    uint32_t start;
    uint32_t end;
    bool operator<(const LiveRange& other) const { return start < other.start; }
};

// ============================================================================
// 虚拟寄存器的活跃区间 (Live Interval)
// ============================================================================
struct LiveInterval {
    uint32_t vreg = 0;
    std::vector<LiveRange> ranges;
    Register allocatedGPR;
    XMMRegister allocatedXMM;
    int32_t allocatedSlot = -1;

    void addRange(uint32_t start, uint32_t end) {
        if (start >= end) return;
        if (ranges.empty()) {
            ranges.push_back({start, end});
        } else {
            auto& first = ranges.front();
            if (end >= first.start) {
                // 合并重叠或相邻的区间
                first.start = std::min(first.start, start);
                first.end = std::max(first.end, end);
            } else {
                // 插入新的不连续区间 (Hole)
                ranges.insert(ranges.begin(), {start, end});
            }
        }
    }
};

// ============================================================================
// 活跃区间分析器 (Liveness Analyzer) (Step 40)
// ============================================================================
class LivenessAnalyzer {
public:
    LivenessAnalyzer(LIRGraph& lir) : lir_(lir) {}

    void analyze() {
        numberInstructions();
        computeLocalLiveSets();
        computeGlobalLiveSets();
        buildIntervals();
    }

    const std::unordered_map<uint32_t, LiveInterval>& intervals() const { return intervals_; }
    std::unordered_map<uint32_t, LiveInterval>& intervalsMut() { return intervals_; }

private:
    LIRGraph& lir_;
    std::unordered_map<uint32_t, LiveInterval> intervals_;

    // 1. 为指令分配递增的偶数 ID
    void numberInstructions() {
        uint32_t id = 0;
        for (LIRBlock* block : lir_.blocks()) {
            for (LIRInst* inst : block->instructions()) {
                inst->setLinearId(id);
                id += 2; // 留出奇数空隙，以便后续插入 Spill/Reload 指令
            }
        }
    }

    // 2. 计算基本块的局部活跃集 (liveGen 和 liveKill)
    void computeLocalLiveSets() {
        for (LIRBlock* block : lir_.blocks()) {
            std::unordered_set<uint32_t> liveGen;
            std::unordered_set<uint32_t> liveKill;

            for (LIRInst* inst : block->instructions()) {
                for (const auto& use : inst->uses()) {
                    if (use.isVirtual() && liveKill.find(use.vreg()) == liveKill.end()) {
                        liveGen.insert(use.vreg());
                    }
                }
                for (const auto& def : inst->defs()) {
                    if (def.isVirtual()) {
                        liveKill.insert(def.vreg());
                    }
                }
            }
            // 临时借用 liveIn 和 liveOut 存储 Gen 和 Kill
            block->liveIn = liveGen;
            block->liveOut = liveKill;
        }
    }

    // 3. 后向数据流分析，计算全局 liveIn 和 liveOut
    void computeGlobalLiveSets() {
        std::unordered_map<LIRBlock*, std::unordered_set<uint32_t>> liveGen;
        std::unordered_map<LIRBlock*, std::unordered_set<uint32_t>> liveKill;
        for (LIRBlock* block : lir_.blocks()) {
            liveGen[block] = block->liveIn;
            liveKill[block] = block->liveOut;
            block->liveIn.clear();
            block->liveOut.clear();
        }

        bool changed = true;
        while (changed) {
            changed = false;
            // 逆序遍历基本块加速收敛
            for (auto it = lir_.blocks().rbegin(); it != lir_.blocks().rend(); ++it) {
                LIRBlock* block = *it;
                
                std::unordered_set<uint32_t> newLiveOut;
                for (LIRBlock* succ : block->successors()) {
                    for (uint32_t vreg : succ->liveIn) {
                        newLiveOut.insert(vreg);
                    }
                }

                std::unordered_set<uint32_t> newLiveIn = liveGen[block];
                for (uint32_t vreg : newLiveOut) {
                    if (liveKill[block].find(vreg) == liveKill[block].end()) {
                        newLiveIn.insert(vreg);
                    }
                }

                if (newLiveIn != block->liveIn || newLiveOut != block->liveOut) {
                    block->liveIn = newLiveIn;
                    block->liveOut = newLiveOut;
                    changed = true;
                }
            }
        }
    }

    // 4. 逆序遍历指令，构建活跃区间
    void buildIntervals() {
        for (auto it = lir_.blocks().rbegin(); it != lir_.blocks().rend(); ++it) {
            LIRBlock* block = *it;
            uint32_t blockStart = block->instructions().empty() ? 0 : block->instructions().front()->linearId();
            uint32_t blockEnd = block->instructions().empty() ? blockStart + 2 : block->instructions().back()->linearId() + 2;

            std::unordered_set<uint32_t> live = block->liveOut;

            // 为所有跨越块末尾存活的变量添加区间
            for (uint32_t vreg : live) {
                getInterval(vreg).addRange(blockStart, blockEnd);
            }

            for (auto instIt = block->instructions().rbegin(); instIt != block->instructions().rend(); ++instIt) {
                LIRInst* inst = *instIt;
                uint32_t id = inst->linearId();

                for (const auto& def : inst->defs()) {
                    if (def.isVirtual()) {
                        live.erase(def.vreg());
                        LiveInterval& interval = getInterval(def.vreg());
                        if (!interval.ranges.empty()) {
                            // 截断当前区间到定义处
                            interval.ranges.front().start = id;
                        } else {
                            // 死存储 (Dead Store)：定义后从未被使用，至少存活一瞬
                            interval.addRange(id, id + 1);
                        }
                    }
                }

                for (size_t i = 0; i < inst->uses().size(); ++i) {
                    const auto& use = inst->uses()[i];
                    if (use.isVirtual()) {
                        live.insert(use.vreg());
                        bool isSameAs = false;
                        for (size_t j = 0; j < inst->defs().size(); ++j) {
                            if (inst->defConstraints()[j].type == LIRConstraintType::SameAsInput && inst->defConstraints()[j].value == i) {
                                isSameAs = true;
                                break;
                            }
                        }
                        uint32_t endPos = isSameAs ? id : (id + 1);
                        getInterval(use.vreg()).addRange(blockStart, endPos);
                    }
                }
            }
        }
    }

    LiveInterval& getInterval(uint32_t vreg) {
        auto it = intervals_.find(vreg);
        if (it == intervals_.end()) {
            intervals_[vreg] = LiveInterval{vreg, {}};
            return intervals_[vreg];
        }
        return it->second;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_LIVENESS_ANALYSIS_H
