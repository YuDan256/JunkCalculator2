#ifndef JC2_JIT_LOOP_PROMOTION_H
#define JC2_JIT_LOOP_PROMOTION_H

#include "../ir/HIR.h"
#include "../ir/HIRBuilder.h"
#include <vector>
#include <algorithm>

namespace jc {
namespace jit {

// Promote a tagged loop-header Phi to a raw Int32 Phi when the loop body
// already maintains the value as Int32.  This is deliberately conservative:
// it only handles OSR loops with one entry and one BoxInt32 backedge.
class LoopPromotion {
public:
    explicit LoopPromotion(HIRGraph& graph) : graph_(graph) {}

    void run() {
        std::vector<HIRNode*> snapshot = graph_.nodes();
        for (HIRNode* node : snapshot) {
            if (node->opcode() == HIROp::Phi) tryPromote(node);
        }
    }

private:
    HIRGraph& graph_;

    static void bypassGuard(HIRNode* guard) {
        std::vector<HIRNode*> uses = guard->uses();
        for (HIRNode* use : uses) {
            for (size_t i = 0; i < use->inputs().size(); ++i) {
                if (use->inputs()[i] != guard) continue;
                // Guard control/effect positions differ by consumer. Control
                // consumers always take the guard's control edge; effect
                // consumers take its effect edge.
                const bool controlConsumer =
                    use->opcode() == HIROp::LoopBegin ||
                    use->opcode() == HIROp::LoopEnd ||
                    use->opcode() == HIROp::Branch ||
                    use->opcode() == HIROp::IfTrue ||
                    use->opcode() == HIROp::IfFalse ||
                    use->opcode() == HIROp::Jump ||
                    use->opcode() == HIROp::Merge;
                const bool effectEdge = !controlConsumer && i == 1;
                HIRNode* replacement = effectEdge ? guard->inputs()[1] : guard->inputs()[0];
                use->replaceInput(i, replacement);
            }
        }
        guard->clearInputs();
    }

    static void replaceFrameStateInputs(const std::vector<HIRNode*>& frameStates,
                                        HIRNode* phi, HIRNode* box) {
        for (HIRNode* use : frameStates) {
            for (size_t i = 0; i < use->inputs().size(); ++i) {
                if (use->inputs()[i] == phi) use->replaceInput(i, box);
            }
        }
    }

    void tryPromote(HIRNode* phi) {
        if (phi->type() != JITType::TaggedValue || phi->inputs().size() != 3) return;
        HIRNode* loop = phi->inputs()[0];
        HIRNode* entry = phi->inputs()[1];
        HIRNode* back = phi->inputs()[2];
        if (!loop || loop->opcode() != HIROp::LoopBegin) return;
        if (loop->inputs().empty() || !loop->inputs()[0] ||
            loop->inputs()[0]->opcode() != HIROp::OSREntry) return;
        if (!entry || entry->opcode() != HIROp::LoadRegister) return;
        if (!back || back->inputs().empty()) return;

        JITType rawType;
        HIROp boxOp;
        HIROp guardOp;
        HIROp unboxOp;
        if (back->opcode() == HIROp::BoxInt32) {
            rawType = JITType::Int32;
            boxOp = HIROp::BoxInt32;
            guardOp = HIROp::GuardIsInt32;
            unboxOp = HIROp::UnboxInt32;
        } else if (back->opcode() == HIROp::BoxDouble) {
            rawType = JITType::Double;
            boxOp = HIROp::BoxDouble;
            guardOp = HIROp::GuardIsDouble;
            unboxOp = HIROp::UnboxDouble;
        } else {
            return;
        }
        for (HIRNode* use : back->uses()) if (use != phi) return;
        HIRNode* rawBack = back->inputs()[0];
        if (!rawBack || rawBack->type() != rawType) return;

        std::vector<HIRNode*> unboxes;
        std::vector<HIRNode*> guards;
        std::vector<HIRNode*> frameStates;
        std::vector<HIRNode*> returns;
        for (HIRNode* use : phi->uses()) {
            if (use->opcode() == unboxOp && use->inputs().size() > 1 &&
                use->inputs()[1] == phi) {
                unboxes.push_back(use);
            } else if (use->opcode() == guardOp && use->inputs().size() > 2 &&
                       use->inputs()[2] == phi) {
                guards.push_back(use);
            } else if (use->opcode() == HIROp::FrameState) {
                frameStates.push_back(use);
            } else if (use->opcode() == HIROp::Return && use->inputs().size() > 2 &&
                       use->inputs()[2] == phi) {
                returns.push_back(use);
            } else {
                return;
            }
        }
        if (unboxes.empty()) return;

        // The OSR entry snapshot is the frame state for the one-time entry guard.
        FrameStateNode* oldFS = nullptr;
        for (HIRNode* guard : guards) {
            oldFS = static_cast<GuardNode*>(guard)->frameState();
            if (oldFS) break;
        }
        if (!oldFS) return;
        std::vector<HIRNode*> loads;
        int maxReg = -1;
        for (HIRNode* n : graph_.nodes()) {
            if (n->opcode() != HIROp::LoadRegister) continue;
            auto* load = static_cast<RegisterAccessNode*>(n);
            if (load->control() != loop->inputs()[0]) continue;
            if (load->regIndex() + 1 > maxReg) maxReg = load->regIndex() + 1;
            if (loads.size() <= static_cast<size_t>(load->regIndex())) {
                loads.resize(load->regIndex() + 1, nullptr);
            }
            loads[load->regIndex()] = n;
        }
        int fsSize = std::max(static_cast<int>(oldFS->inputs().size()), maxReg);
        if (fsSize <= 0) return;

        auto* entryFS = graph_.allocateNode<FrameStateNode>(oldFS->bailoutId(), oldFS->ip());
        for (int i = 0; i < fsSize; ++i) {
            HIRNode* value = (i < static_cast<int>(loads.size())) ? loads[i] : nullptr;
            entryFS->addInput(value);
        }

        HIRNode* entryControl = loop->inputs()[0];
        auto* entryGuard = graph_.allocateNode<GuardNode>(
            guardOp, entryControl, entryControl, entry, entryFS);
        auto* entryUnbox = graph_.allocateNode<HIRNode>(unboxOp, rawType);
        entryUnbox->addInput(entryGuard);
        entryUnbox->addInput(entry);

        auto* rawPhi = graph_.allocateNode<HIRNode>(HIROp::Phi, rawType);
        rawPhi->addInput(loop);
        rawPhi->addInput(entryUnbox);
        rawPhi->addInput(rawBack);

        auto* boxedPhi = graph_.allocateNode<HIRNode>(boxOp, JITType::TaggedValue);
        boxedPhi->addInput(rawPhi);

        // Loop entry: OSREntry -> entry guard -> LoopBegin.
        loop->replaceInput(0, entryGuard);

        // Effect Phi: the OSR entry effect also becomes the entry guard.
        for (HIRNode* n : graph_.nodes()) {
            if (n->opcode() != HIROp::Phi || n->type() != JITType::Effect) continue;
            if (n->inputs().empty() || n->inputs()[0] != loop) continue;
            for (size_t i = 1; i < n->inputs().size(); ++i) {
                if (n->inputs()[i] == entryControl) n->replaceInput(i, entryGuard);
            }
        }

        for (HIRNode* unbox : unboxes) unbox->replaceUsesWith(rawPhi);
        for (HIRNode* guard : guards) bypassGuard(guard);
        replaceFrameStateInputs(frameStates, phi, boxedPhi);
        for (HIRNode* ret : returns) {
            for (size_t i = 0; i < ret->inputs().size(); ++i) {
                if (ret->inputs()[i] == phi) ret->replaceInput(i, boxedPhi);
            }
        }

        phi->replaceInput(2, rawBack);
        for (HIRNode* unbox : unboxes) unbox->clearInputs();
        back->clearInputs();
        if (phi->uses().empty()) phi->clearInputs();
    }
};

} // namespace jit
} // namespace jc

#endif

