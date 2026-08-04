#include "HIRBuilder.h"
#include "GCM.h"
#include "InstructionSelector.h"
#include "LivenessAnalysis.h"
#include "LinearScan.h"
#include "CodeEmitter.h"
#include "MacroAssembler.h"
#include <iostream>

using namespace jc;
using namespace jc::jit;

int main() {
    std::cout << "Running InstructionSelector with Control Flow test..." << std::endl;

    // 1. 手动构建带有控制流的 HIR 图
    HIRGraph hirGraph;
    HIRBuilder hirBuilder(&hirGraph);

    // Block 0: Entry
    hirBuilder.createStart();
    auto a = hirBuilder.createInt32Constant(10);
    auto b = hirBuilder.createInt32Constant(20);
    auto cmp = hirBuilder.createCmpLtI32(a, b);
    auto branch = hirBuilder.createBranch(cmp);

    // Block 1: IfTrue
    auto ifTrue = hirBuilder.createIfTrue(branch);
    hirBuilder.setCurrentControl(ifTrue);
    auto addRes = hirBuilder.createAddI32(a, b);
    auto jmpTrue = hirBuilder.createJump(nullptr);

    // Block 2: IfFalse
    auto ifFalse = hirBuilder.createIfFalse(branch);
    hirBuilder.setCurrentControl(ifFalse);
    auto subRes = hirBuilder.createSubI32(a, b);
    auto jmpFalse = hirBuilder.createJump(nullptr);

    // Block 3: Merge & Phi
    auto merge = hirBuilder.createMerge({jmpTrue, jmpFalse});
    hirBuilder.setCurrentControl(merge);
    auto phi = hirBuilder.createPhi(JITType::Int32, {addRes, subRes});
    hirBuilder.createReturn(phi);

    // 2. 全局代码移动 (GCM) 调度
    LIRGraph lirGraph;
    LIRBuilder lirBuilder(&lirGraph);
    GCM gcm(hirGraph, lirGraph);
    gcm.schedule();

    // 3. 指令选择 (Instruction Selection)
    InstructionSelector selector(gcm, hirGraph, lirGraph, lirBuilder);
    selector.select();

    // 4. 活跃区间分析 (Liveness Analysis)
    LivenessAnalyzer liveness(lirGraph);
    liveness.analyze();

    // 5. 线性扫描寄存器分配 (Linear Scan Register Allocation)
    LinearScanAllocator allocator(lirGraph, liveness);
    allocator.allocate();

    // 6. 机器码发射 (Code Emission)
    MacroAssembler masm;
    CodeEmitter emitter(lirGraph, masm);
    emitter.emit(allocator.getStackSize());

    // 7. 打印 LIR 序列
    std::cout << "\n--- LIR Output ---\n";
    for (LIRBlock* block : lirGraph.blocks()) {
        std::cout << "Block " << block->id() << ":\n";
        std::cout << "  Predecessors: ";
        for (auto p : block->predecessors()) std::cout << p->id() << " ";
        std::cout << "\n  Successors: ";
        for (auto s : block->successors()) std::cout << s->id() << " ";
        std::cout << "\n";

        for (LIRInst* inst : block->instructions()) {
            std::cout << "  " << inst->id() << ": " << to_string(inst->opcode()) << " ";
            
            // 打印 Defs (输出)
            if (!inst->defs().empty()) {
                for (size_t i = 0; i < inst->defs().size(); ++i) {
                    std::cout << inst->defs()[i].toString();
                    if (i < inst->defs().size() - 1) std::cout << ", ";
                }
                std::cout << " <- ";
            }
            
            // 打印 Uses (输入)
            for (size_t i = 0; i < inst->uses().size(); ++i) {
                std::cout << inst->uses()[i].toString();
                if (i < inst->uses().size() - 1) std::cout << ", ";
            }
            
            if (inst->opcode() == LIROpcode::Jcc || inst->opcode() == LIROpcode::Jmp) {
                if (inst->target()) {
                    std::cout << " [Target: Block " << inst->target()->id() << "]";
                }
            }
            std::cout << "\n";
        }
    }
    std::cout << "------------------\n";

    std::cout << "InstructionSelector and CodeEmitter test passed!" << std::endl;
    return 0;
}
