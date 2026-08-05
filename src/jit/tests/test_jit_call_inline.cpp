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

// C++ Runtime function to be called from JIT (ABI Compliant)
extern "C" int32_t c_add_func(int32_t a, int32_t b) {
    return a + b;
}

int main() {
    std::cout << "Running Phase 15: JIT Callout & ABI Compliance Test..." << std::endl;

    // 1. 手动构建等价于以下代码的 HIR 图:
    // int32_t sum = 0;
    // int32_t i = 0;
    // while (i < 1000) {
    //     sum = c_add_func(sum, i); // 触发 Callout 和 Eager Sync
    //     i = i + 1;
    // }
    // return sum;

    HIRGraph hirGraph;
    HIRBuilder hirBuilder(&hirGraph, 256);

    // Block 0: Entry
    auto start = hirBuilder.createStart();
    auto sum_init = hirBuilder.createInt32Constant(0);
    auto i_init = hirBuilder.createInt32Constant(0);
    auto limit = hirBuilder.createInt32Constant(1000);
    auto step = hirBuilder.createInt32Constant(1);

    // 模拟解释器状态，供 FrameState 捕获
    hirBuilder.setLocal(0, sum_init);
    hirBuilder.setLocal(1, i_init);

    // Block 1: Loop Header
    auto loopBegin = hirBuilder.createLoopBegin({start});
    auto sum_phi = hirBuilder.createPhi(JITType::Int32, {sum_init});
    auto i_phi = hirBuilder.createPhi(JITType::Int32, {i_init});

    hirBuilder.setLocal(0, sum_phi);
    hirBuilder.setLocal(1, i_phi);

    auto cmp = hirBuilder.createCmpLtI32(i_phi, limit);
    auto branch = hirBuilder.createBranch(cmp);

    // Block 2: Loop Body (IfTrue)
    auto ifTrue = hirBuilder.createIfTrue(branch);
    hirBuilder.setCurrentControl(ifTrue);
    
    // 捕获状态，触发 Eager Sync (将活跃寄存器刷回 VM::registers)
    auto fs = hirBuilder.captureFrameState(0, 0);
    
    // Callout 节点：调用 C++ 函数 c_add_func
    // 这将测试 ABI 传参 (RCX/RDX 或 RDI/RSI) 以及 Caller-Saved 寄存器的溢出保护
    auto callout = hirBuilder.createCallout(reinterpret_cast<void*>(c_add_func), JITType::Int32, 2, {sum_phi, i_phi}, fs);
    
    auto i_next = hirBuilder.createAddI32(i_phi, step);

    auto loopEnd = hirBuilder.createLoopEnd(loopBegin);

    // 回边绑定
    loopBegin->addInput(loopEnd);
    sum_phi->addInput(callout);
    i_phi->addInput(i_next);

    // Block 3: Exit (IfFalse)
    auto ifFalse = hirBuilder.createIfFalse(branch);
    hirBuilder.setCurrentControl(ifFalse);
    auto boxedSum = hirBuilder.createBoxInt32(sum_phi);
    hirBuilder.createReturn(boxedSum);

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
    // 分配 256 个虚拟寄存器槽位，防止 Eager Sync 越界写入
    Value registers[256];
    for (int i = 0; i < 256; ++i) {
        registers[i] = Value::none();
    }

    JitFunc func = reinterpret_cast<JitFunc>(mem.get());

    std::cout << "\n=== Executing JIT Callout Code (1,000 iterations) ===\n";
    uint64_t retBits = func(registers);
    Value retVal;
    retVal.as_bits = retBits;

    int32_t expected = 0;
    for (int32_t i = 0; i < 1000; ++i) {
        expected += i;
    }

    std::cout << "JIT Execution Result: " << retVal.asInt32() << std::endl;
    std::cout << "Expected Result:      " << expected << std::endl;

    if (retVal.isInt32() && retVal.asInt32() == expected) {
        std::cout << "\nPhase 15: JIT Callout & ABI Compliance Test passed successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nTest failed! Result mismatch." << std::endl;
        return 1;
    }
}
