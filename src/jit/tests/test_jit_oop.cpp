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

// 模拟一个具有平坦内存布局的对象（未来 Hidden Class / Shape 的内存模型）
struct DummyObject : public ObjInstance {
    Value x;
    Value y;
    DummyObject() { type = ObjType::INSTANCE; }
};

typedef uint64_t (*JitFunc)(Value*);

int main() {
    std::cout << "Running End-to-End JIT OOP test..." << std::endl;

    // 1. 手动构建等价于以下逻辑的 HIR 图:
    // obj = registers[0]
    // sum = obj.x + obj.y
    // obj.x = sum
    // return sum

    HIRGraph hirGraph;
    HIRBuilder hirBuilder(&hirGraph);

    // Block 0: Entry
    hirBuilder.createStart();
    
    // 模拟传入的参数 registers[0] 是一个 DummyObject 实例 (NaN-Boxed)
    auto obj = hirBuilder.createLoadRegister(0);
    
    // 模拟 GuardIsClass (假设 classId 为 42)
    auto fs = hirBuilder.captureFrameState(0, 0);
    auto guard = hirBuilder.createGuardIsClass(obj, fs, 42);
    
    ObjClass dummyClass;
    dummyClass.classId = 42;
    
    DummyObject dummy;
    dummy.classDef = &dummyClass;
    
    // 安全计算字段的内存偏移量
    int32_t offsetX = static_cast<int32_t>(reinterpret_cast<char*>(&dummy.x) - reinterpret_cast<char*>(&dummy));
    int32_t offsetY = static_cast<int32_t>(reinterpret_cast<char*>(&dummy.y) - reinterpret_cast<char*>(&dummy));
    
    auto offsetNodeX = hirBuilder.createInt32Constant(offsetX);
    auto offsetNodeY = hirBuilder.createInt32Constant(offsetY);
    
    // 读取 obj.x 和 obj.y
    auto loadX = hirBuilder.createLoadField(obj, offsetNodeX);
    auto loadY = hirBuilder.createLoadField(obj, offsetNodeY);
    
    // 拆箱
    auto unboxX = hirBuilder.createUnboxInt32(loadX, guard);
    auto unboxY = hirBuilder.createUnboxInt32(loadY, guard);
    
    // sum = x + y
    auto sum = hirBuilder.createAddI32(unboxX, unboxY);
    auto boxedSum = hirBuilder.createBoxInt32(sum);
    
    // obj.x = sum
    hirBuilder.createStoreField(obj, offsetNodeX, boxedSum);
    
    // return sum
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
    dummy.x = Value::fromInt32(100);
    dummy.y = Value::fromInt32(200);
    
    Value registers[1];
    registers[0] = Value(&dummy); // 传入 NaN-Boxed 指针
    
    JitFunc func = reinterpret_cast<JitFunc>(mem.get());
    
    std::cout << "\n=== Executing JIT OOP Code ===\n";
    uint64_t retBits = func(registers);
    Value retVal;
    retVal.as_bits = retBits;

    std::cout << "JIT Execution Result: " << retVal.asInt32() << std::endl;
    std::cout << "Mutated obj.x: " << dummy.x.asInt32() << std::endl;
    
    // 验证结果: 100 + 200 = 300，且 dummy.x 被成功修改为 300
    if (retVal.isInt32() && retVal.asInt32() == 300 && dummy.x.asInt32() == 300) {
        std::cout << "\nEnd-to-End JIT OOP test passed successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nTest failed!" << std::endl;
        return 1;
    }
}
