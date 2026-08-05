#include "../../vm/VM.h"
#include "../../vm/BuiltinRegistry.h"
#include "../../frontend/Lexer.h"
#include "../../frontend/Parser.h"
#include "../../compiler/Resolver.h"
#include "../../compiler/IRBuilder.h"
#include "../../compiler/IROptimizer.h"
#include "../../compiler/RegisterAllocator.h"
#include "../../compiler/Emitter.h"
#include <iostream>

using namespace jc;

// 声明全局变量，规避链接错误 (因为我们排除了 main.cpp)
bool g_showIR = true;
bool g_autoDebug = false;
bool g_profile = false;

int main() {
    std::cout << "Running Phase 16: JIT OSR End-to-End Test..." << std::endl;
    
    VM vm;
    BuiltinRegistry registry;
    registry.registerAll();
    
    // 编写一段单次调用但包含长循环的 JC2 代码
    // 循环次数为 2000 次，足以在第 1000 次时触发 OSR 编译和热切换
    std::string code = R"(
        local sum = 0
        local i = 0
        while (i < 2000) {
            sum = sum + i
            i = i + 1
        }
        return sum
    )";
    
    try {
        jc::Lexer lexer(code, "<osr_test>");
        auto tokens = lexer.tokenize();
        jc::Parser parser(tokens);
        auto ast = parser.parse();
        
        jc::Resolver resolver;
        resolver.resolve(ast.get());
        
        auto mainFn = std::make_shared<CompiledFunction>();
        mainFn->name = "<osr_test>";
        mainFn->sourceFile = "<osr_test>";
        mainFn->arity = 0;
        mainFn->maxArity = 0;
        mainFn->hasRestParam = false;
        
        auto fns = vm.getCompiledFunctions();
        
        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, &fns, nullptr, mainFn.get(), &resolver.exprSymbols, &resolver.patternSymbols);
        fnBuilder.build(ast.get());
        
        IROptimizer::optimize(&fnGraph);
        RegisterAllocator::allocate(&fnGraph);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                IRNode* localNode = target.localNode;
                int upvalIdx = target.index;
                CompiledFunction* childFn = mainFn.get();
                fnGraph.postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                    childFn->upvalues[upvalIdx].index = localNode->getResolved()->physicalReg;
                });
            }
        }
        
        mainFn->localCount = Emitter::emit(&fnGraph, mainFn->chunk);
        
        fns.push_back(mainFn);
        int mainFnIdx = static_cast<int>(fns.size()) - 1;
        vm.setCompiledFunctions(fns);
        
        ObjClosure* cls = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{}, std::vector<bool>{}, "<osr_test>", nullptr
        );
        cls->compiledFnIndex = mainFnIdx;
        Value testFunc(cls);
        
        std::cout << "Executing OSR test function..." << std::endl;
        
        // 执行闭包，这将在解释器中运行，并在中途触发 OSR 跃入机器码
        Value res = vm.callVMFunction(testFunc.asFunction()->compiledFnIndex, {}, testFunc.asFunction());
        
        int expected = 0;
        for (int i = 0; i < 2000; ++i) {
            expected += i;
        }
        
        std::cout << "Result: " << res.asInt32() << std::endl;
        std::cout << "Expected: " << expected << std::endl;
        
        if (res.isInt32() && res.asInt32() == expected) {
            std::cout << "\nPhase 16: JIT OSR End-to-End Test passed successfully!" << std::endl;
            return 0;
        } else {
            std::cerr << "\nTest failed! Result mismatch." << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
