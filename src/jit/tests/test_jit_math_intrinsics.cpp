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
#include <chrono>
#include <cmath>
#include <iomanip>

using namespace jc;
using namespace jc::jit;

typedef uint64_t (*JitFunc)(Value*);

int main() {
    std::cout << "Running End-to-End JIT Math Intrinsics Benchmark..." << std::endl;

    // 1. 手动构建等价于以下 C 代码的 HIR 图:
    // double sum = 0.0;
    // double i = 0.0;
    // while (i < 1000000.0) {
    //     sum = sum + sqrt(i * i + i * i);
    //     i = i + 1.0;
    // }
    // return sum;

    HIRGraph hirGraph;
    HIRBuilder hirBuilder(&hirGraph, 256);

    // Block 0: Entry
    auto start = hirBuilder.createStart();
    auto sum_init = hirBuilder.createDoubleConstant(0.0);
    auto i_init = hirBuilder.createDoubleConstant(0.0);
    auto limit = hirBuilder.createDoubleConstant(1000000.0);
    auto step = hirBuilder.createDoubleConstant(1.0);

    // Block 1: Loop Header
    auto loopBegin = hirBuilder.createLoopBegin({start});
    auto sum_phi = hirBuilder.createPhi(JITType::Double, {sum_init});
    auto i_phi = hirBuilder.createPhi(JITType::Double, {i_init});

    auto cmp = hirBuilder.createCmpLtF64(i_phi, limit);
    auto branch = hirBuilder.createBranch(cmp);

    // Block 2: Loop Body (IfTrue)
    auto ifTrue = hirBuilder.createIfTrue(branch);
    hirBuilder.setCurrentControl(ifTrue);
    
    // 计算几何距离: sqrt(i*i + i*i)
    auto i_sq = hirBuilder.createMulF64(i_phi, i_phi);
    auto sum_sq = hirBuilder.createAddF64(i_sq, i_sq);
    auto dist = hirBuilder.createSqrtF64(sum_sq); // ★ 触发硬件 SQRTSD 指令
    
    auto sum_next = hirBuilder.createAddF64(sum_phi, dist);
    auto i_next = hirBuilder.createAddF64(i_phi, step);

    auto loopEnd = hirBuilder.createLoopEnd(loopBegin);

    // 回边绑定
    loopBegin->addInput(loopEnd);
    sum_phi->addInput(sum_next);
    i_phi->addInput(i_next);

    // Block 3: Exit (IfFalse)
    auto ifFalse = hirBuilder.createIfFalse(branch);
    hirBuilder.setCurrentControl(ifFalse);
    auto boxedSum = hirBuilder.createBoxDouble(sum_phi);
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

    // 3. 执行机器码并计时
    Value registers[256];
    JitFunc func = reinterpret_cast<JitFunc>(mem.get());

    std::cout << "\n=== Executing JIT Math Code (1,000,000 iterations) ===\n";
    
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t retBits = func(registers);
    auto t2 = std::chrono::high_resolution_clock::now();
    
    Value retVal;
    retVal.as_bits = retBits;

    double elapsed = std::chrono::duration<double, std::milli>(t2 - t1).count();
    
    // 验证结果: sum_{i=0}^{999999} i * sqrt(2) = sqrt(2) * 499999500000
    double expected = std::sqrt(2.0) * 499999500000.0;
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "JIT Execution Time:   " << elapsed << " ms" << std::endl;
    std::cout << "JIT Execution Result: " << retVal.asDoubleRaw() << std::endl;
    std::cout << "Expected Result:      " << expected << std::endl;

    if (retVal.isDouble() && std::abs(retVal.asDoubleRaw() - expected) < 1e-4) {
        std::cout << "\nEnd-to-End JIT Math Intrinsics Benchmark passed successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nTest failed! Result mismatch." << std::endl;
        return 1;
    }
}
