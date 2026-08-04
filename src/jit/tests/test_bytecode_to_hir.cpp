#include "../frontend/BytecodeToHIR.h"
#include "../vm/Bytecode.h"
#include <iostream>

using namespace jc;
using namespace jc::jit;

int main() {
    std::cout << "Running BytecodeToHIR test..." << std::endl;

    Chunk chunk;
    
    // 模拟字节码:
    // 0: LOADK R(0), K(0)  ; R(0) = 10
    // 1: LOADK R(1), K(1)  ; R(1) = 20
    // 2: ADD R(2), R(0), R(1) ; R(2) = R(0) + R(1)
    // 3: RETURN R(2)

    int k1 = chunk.addConstant(Value::fromInt32(10));
    chunk.write(CREATE_ABx(OpCode::LOADK, 0, k1), 1);

    int k2 = chunk.addConstant(Value::fromInt32(20));
    chunk.write(CREATE_ABx(OpCode::LOADK, 1, k2), 2);

    chunk.write(CREATE_ABC(OpCode::ADD, 2, 0, 1), 3);
    // 模拟 Tier 0 Profiling 数据：ADD 指令遇到了纯 Int32 (Monomorphic Int32)
    chunk.typeFeedback.back() = 0x01;

    chunk.write(CREATE_ABC(OpCode::RETURN, 2, 0, 0), 4);

    // 构建 HIR 图
    HIRGraph graph;
    HIRBuilder builder(&graph);
    BytecodeToHIR converter(chunk, builder);

    converter.build();

    // 打印为 Graphviz DOT 格式
    std::cout << "\n--- Graphviz DOT Output ---\n\n";
    graph.printDOT(std::cout);
    std::cout << "\n---------------------------\n";

    std::cout << "BytecodeToHIR test passed!" << std::endl;
    return 0;
}
