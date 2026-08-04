#include "../ir/HIRBuilder.h"
#include "../pass/GCM.h"
#include "../pass/InstructionSelector.h"
#include "../pass/LivenessAnalysis.h"
#include "../pass/LinearScan.h"
#include "../backend/CodeEmitter.h"
#include "../backend/MacroAssembler.h"
#include "../backend/ExecutableMemory.h"
#include "../memory/Value.h"
#include <iostream>

using namespace jc;
using namespace jc::jit;

typedef uint64_t (*JitFunc)(Value*);

// 辅助函数：打印 LIR 状态
void printLIR(const LIRGraph& lirGraph, const std::string& stage) {
    std::cout << "\n=== LIR after " << stage << " ===\n";
    for (LIRBlock* block : lirGraph.blocks()) {
        std::cout << "Block " << block->id() << ":\n";
        std::cout << "  Predecessors: ";
        for (auto p : block->predecessors()) std::cout << p->id() << " ";
        std::cout << "\n  Successors: ";
        for (auto s : block->successors()) std::cout << s->id() << " ";
        std::cout << "\n";

        for (LIRInst* inst : block->instructions()) {
            std::cout << "  " << inst->id() << ": " << to_string(inst->opcode()) << " ";
            
            if (!inst->defs().empty()) {
                for (size_t i = 0; i < inst->defs().size(); ++i) {
                    std::cout << inst->defs()[i].toString();
                    if (i < inst->defs().size() - 1) std::cout << ", ";
                }
                std::cout << " <- ";
            }
            
            for (size_t i = 0; i < inst->uses().size(); ++i) {
                std::cout << inst->uses()[i].toString();
                if (i < inst->uses().size() - 1) std::cout << ", ";
            }
            
            if (inst->opcode() == LIROpcode::Jcc || inst->opcode() == LIROpcode::Jmp) {
                if (inst->target()) std::cout << " [Target: Block " << inst->target()->id() << "]";
            }
            std::cout << "\n";
        }
    }
}

int main() {
    std::cout << "Running End-to-End JIT Pipeline test with Control Flow..." << std::endl;

    // 1. 手动构建带有控制流的 HIR 图 (模拟 if-else 分支与 Phi 汇聚)
    // 逻辑: 
    // a = 10, b = 20
    // if (a < b) res = a + b (30)
    // else       res = a - b (-10)
    // return res
    
    HIRGraph hirGraph;
    HIRBuilder hirBuilder(&hirGraph);

    // Block 0: Entry
    hirBuilder.createStart();
    auto a = hirBuilder.createInt32Constant(10);
    auto b = hirBuilder.createInt32Constant(20);
    
    // 模拟写入寄存器 (测试 Load/Store Register)
    hirBuilder.createStoreRegister(0, hirBuilder.createBoxInt32(a));
    hirBuilder.createStoreRegister(1, hirBuilder.createBoxInt32(b));

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
    
    auto boxedPhi = hirBuilder.createBoxInt32(phi);
    hirBuilder.createStoreRegister(2, boxedPhi);
    hirBuilder.createReturn(boxedPhi);

    std::cout << "\n=== HIR Graph (Graphviz DOT) ===\n";
    hirGraph.printDOT(std::cout);

    // 2. LIR 构建与 GCM 调度
    LIRGraph lirGraph;
    LIRBuilder lirBuilder(&lirGraph);
    GCM gcm(hirGraph, lirGraph);
    gcm.schedule();

    // 3. 指令选择 (降级为机器指令)
    InstructionSelector selector(gcm, hirGraph, lirGraph, lirBuilder);
    selector.select();
    
    printLIR(lirGraph, "Instruction Selection (Pre-Allocation)");

    // 4. 活跃区间分析
    LivenessAnalyzer liveness(lirGraph);
    liveness.analyze();

    // 5. 线性扫描寄存器分配
    LinearScanAllocator allocator(lirGraph, liveness);
    allocator.allocate();
    
    printLIR(lirGraph, "Linear Scan Register Allocation (Post-Allocation)");

    // 6. 机器码发射
    MacroAssembler masm;
    CodeEmitter emitter(lirGraph, masm);
    emitter.emit(allocator.getStackSize());
    masm.emitConstantPool();

    ExecutableMemory mem;
    masm.finalize(mem);

    // 7. 执行 JIT 编译出的机器码
    Value registers[3]; // 模拟 VM 的寄存器窗口
    JitFunc func = reinterpret_cast<JitFunc>(mem.get());
    
    std::cout << "\n=== Executing JIT Code ===\n";
    uint64_t retBits = func(registers);
    Value retVal;
    retVal.as_bits = retBits;

    std::cout << "JIT Execution Result: " << retVal.asInt32() << std::endl;
    std::cout << "Register[2] (Stored by JIT): " << registers[2].asInt32() << std::endl;
    
    if (retVal.isInt32() && retVal.asInt32() == 30 && registers[2].asInt32() == 30) {
        std::cout << "\nEnd-to-End JIT Pipeline test passed successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nTest failed!" << std::endl;
        return 1;
    }
}
