#include "../ir/HIRBuilder.h"
#include "../pass/ConstantFolding.h"
#include "../pass/AlgebraicSimplification.h"
#include "../pass/CSE.h"
#include "../pass/DCE.h"
#include <iostream>

using namespace jc;
using namespace jc::jit;

int main() {
    std::cout << "Running Mid-Level Optimizations test..." << std::endl;

    HIRGraph graph;
    HIRBuilder builder(&graph, 256);

    // 模拟构建一个未优化的 HIR 图
    builder.createStart();

    // a = 10, b = 20
    auto a = builder.createInt32Constant(10);
    auto b = builder.createInt32Constant(20);

    // c = a + b (常量折叠目标: 30)
    auto c = builder.createAddI32(a, b);

    // d = c * 1 (代数化简目标: 直接使用 c)
    auto one = builder.createInt32Constant(1);
    auto d = builder.createMulI32(c, one);

    // e = 10 + 20 (CSE 目标: 发现与 c 等价，合并)
    auto a2 = builder.createInt32Constant(10);
    auto b2 = builder.createInt32Constant(20);
    auto e = builder.createAddI32(a2, b2);

    // f = d + e (最终常量折叠目标: 30 + 30 = 60)
    auto f = builder.createAddI32(d, e);

    // g = 100 (死代码消除目标: 没有任何节点使用 g)
    auto g = builder.createInt32Constant(100);
    (void)g; // 抑制 unused variable 警告

    // 返回 f
    builder.createReturn(f);

    std::cout << "\n=== Before Optimization (Graphviz DOT) ===\n";
    graph.printDOT(std::cout);

    // 运行中端优化管线
    ConstantFolding(graph, builder).run();
    AlgebraicSimplification(graph, builder).run();
    CommonSubexpressionElimination(graph, builder).run();
    DeadCodeElimination(graph, builder).run();

    std::cout << "\n=== After Optimization (Graphviz DOT) ===\n";
    graph.printDOT(std::cout);

    // 验证：优化后的图中应该只剩下 Start, Return 和一个值为 60 的 Int32Constant
    int nodeCount = 0;
    bool has60 = false;
    for (auto node : graph.nodes()) {
        if (node->inputs().empty() && node->uses().empty() && node->opcode() != HIROp::Start) {
            continue; // 跳过被 DCE 杀死的悬空节点 (在实际内存中它们还在 graph.nodes_ 里，但已经断开连接)
        }
        nodeCount++;
        if (node->opcode() == HIROp::Int32Constant) {
            if (static_cast<Int32ConstantNode*>(node)->value() == 60) {
                has60 = true;
            }
        }
    }

    // 期望存活的节点: Start, Int32Constant(60), Return
    if (nodeCount == 3 && has60) {
        std::cout << "\nMid-Level Optimizations test passed successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nTest failed! Graph was not simplified as expected. Active nodes: " << nodeCount << std::endl;
        return 1;
    }
}
