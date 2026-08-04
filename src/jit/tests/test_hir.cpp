#include "../ir/HIRBuilder.h"
#include <iostream>

using namespace jc::jit;

int main() {
    std::cout << "Running HIRBuilder test..." << std::endl;

    HIRGraph graph;
    HIRBuilder builder(&graph);

    // 1. 创建入口控制流
    builder.createStart();

    // 2. 创建常量节点
    auto a = builder.createInt32Constant(10);
    auto b = builder.createInt32Constant(20);
    auto c = builder.createInt32Constant(42);

    // 3. a + b
    auto add1 = builder.createAddI32(a, b);

    // 4. (a + b) + 42
    auto add2 = builder.createAddI32(add1, c);

    // 5. 返回结果
    builder.createReturn(add2);

    // 打印为 Graphviz DOT 格式
    std::cout << "\n--- Graphviz DOT Output ---\n\n";
    graph.printDOT(std::cout);
    std::cout << "\n---------------------------\n";

    std::cout << "HIRBuilder test passed!" << std::endl;
    return 0;
}
