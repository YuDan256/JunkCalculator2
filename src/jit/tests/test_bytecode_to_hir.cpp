#include "../frontend/BytecodeToHIR.h"
#include "../../vm/Bytecode.h"
#include <iostream>

using namespace jc;
using namespace jc::jit;

// 为独立的单元测试提供 JIT 辅助函数的空实现，以满足链接器要求
namespace jc {
    uint64_t jc2_jit_build_list(uint32_t, uint32_t) { return 0; }
    uint64_t jc2_jit_build_dict(uint32_t, uint32_t) { return 0; }
    uint64_t jc2_jit_build_set(uint32_t, uint32_t) { return 0; }
    uint64_t jc2_jit_build_matrix(uint32_t, uint32_t, const Chunk*) { return 0; }
    uint64_t jc2_jit_build_slice(uint32_t) { return 0; }
    uint64_t jc2_jit_build_class(uint32_t, const Chunk*) { return 0; }
    uint64_t jc2_jit_build_namespace(uint32_t, uint32_t, uint32_t, const Chunk*, uint32_t) { return 0; }
    uint64_t jc2_jit_concat_strings(uint32_t, uint32_t) { return 0; }
    uint64_t jc2_jit_format_string(uint32_t, uint32_t, const Chunk*) { return 0; }
    uint64_t jc2_jit_dict_rest(uint32_t, uint32_t) { return 0; }
    uint64_t jc2_jit_closure(uint32_t, uint32_t) { return 0; }
}

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
    HIRBuilder builder(&graph, 256);
    BytecodeToHIR converter(chunk, builder, 256);

    converter.build();

    // 打印为 Graphviz DOT 格式
    std::cout << "\n--- Graphviz DOT Output ---\n\n";
    graph.printDOT(std::cout);
    std::cout << "\n---------------------------\n";

    std::cout << "BytecodeToHIR test passed!" << std::endl;
    return 0;
}
