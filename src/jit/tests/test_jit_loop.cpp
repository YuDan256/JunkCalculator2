#include "../ir/HIRBuilder.h"
#include "../pass/GCM.h"
#include "../pass/InstructionSelector.h"
#include "../pass/LivenessAnalysis.h"
#include "../pass/LinearScan.h"
#include "../backend/CodeEmitter.h"
#include "../backend/MacroAssembler.h"
#include "../backend/ExecutableMemory.h"
#include "../../memory/Value.h"
#include <iostream>

using namespace jc;
using namespace jc::jit;

typedef uint64_t (*JitFunc)(Value*);

int main() {
    std::cout << "Running End-to-End JIT Loop test..." << std::endl;

    // 1. 手动构建等价于以下 C 代码的 HIR 图:
    // int sum = 0;
    // int i = 0;
    // while (i < 10) {
    //     sum = sum + i;
    //     i = i + 1;
    // }
    // return sum;

    HIRGraph hirGraph;
    HIRBuilder hirBuilder(&hirGraph);

    // Block 0: Entry
    auto start = hirBuilder.createStart();
    auto sum_init = hirBuilder.createInt32Constant(0);
    auto i_init = hirBuilder.createInt32Constant(0);
    auto limit = hirBuilder.createInt32Constant(10);
    auto one = hirBuilder.createInt32Constant(1);
    
    // Block 1: Loop Header
    auto loopBegin = hirBuilder.createLoopBegin({start});
    auto sum_phi = hirBuilder.createPhi(JITType::Int32, {sum_init});
    auto i_phi = hirBuilder.createPhi(JITType::Int32, {i_init});
    
    auto cmp = hirBuilder.createCmpLtI32(i_phi, limit);
    auto branch = hirBuilder.createBranch(cmp);
    
    // Block 2: Loop Body (IfTrue)
    auto ifTrue = hirBuilder.createIfTrue(branch);
    hirBuilder.setCurrentControl(ifTrue);
    auto sum_next = hirBuilder.createAddI32(sum_phi, i_phi);
    auto i_next = hirBuilder.createAddI32(i_phi, one);
    
    auto loopEnd = hirBuilder.createLoopEnd(loopBegin);
    
    // 回边绑定 (Back-edge Binding)
    loopBegin->addInput(loopEnd);
    sum_phi->addInput(sum_next);
    i_phi->addInput(i_next);
    
    // Block 3: Exit (IfFalse)
    auto ifFalse = hirBuilder.createIfFalse(branch);
    hirBuilder.setCurrentControl(ifFalse);
    auto boxedSum = hirBuilder.createBoxInt32(sum_phi);
    hirBuilder.createReturn(boxedSum);

    std::cout << "\n=== HIR Graph (Graphviz DOT) ===\n";
    hirGraph.printDOT(std::cout);

    // 2. 编译管线
    LIRGraph lirGraph;
    LIRBuilder lirBuilder(&lirGraph);
    GCM gcm(hirGraph, lirGraph);
    gcm.schedule();

    InstructionSelector selector(gcm, hirGraph, lirGraph, lirBuilder);
    selector.select();

    LivenessAnalyzer liveness(lirGraph);
    liveness.analyze();

    LinearScanAllocator allocator(lirGraph, liveness);
    allocator.allocate();

    MacroAssembler masm;
    CodeEmitter emitter(lirGraph, masm);
    emitter.emit(allocator.getStackSize());
    masm.emitConstantPool();

    ExecutableMemory mem;
    masm.finalize(mem);

    // 3. 执行机器码
    Value registers[1];
    JitFunc func = reinterpret_cast<JitFunc>(mem.get());
    
    std::cout << "\n=== Executing JIT Code ===\n";
    uint64_t retBits = func(registers);
    Value retVal;
    retVal.as_bits = retBits;

    std::cout << "JIT Execution Result: " << retVal.asInt32() << std::endl;
    
    // 验证结果: 0 + 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 = 45
    if (retVal.isInt32() && retVal.asInt32() == 45) {
        std::cout << "\nEnd-to-End JIT Loop test passed successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nTest failed!" << std::endl;
        return 1;
    }
}
