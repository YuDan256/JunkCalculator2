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
#include <memory>
#include <string>

using namespace jc;

// 声明全局变量，规避链接错误
bool g_showIR = false;
bool g_autoDebug = false;
bool g_profile = false;

int main() {
    std::cout << "Running Phase 17: JIT Complex Inline & GC Safety Test..." << std::endl;
    
    VM vm;
    BuiltinRegistry registry;
    registry.registerAll();
    
    // 编写一段包含复杂指令（矩阵构建、类定义）的函数，并在循环中调用它
    // 循环会触发 OSR 编译，JIT 编译器会将 complex_op 内联展开
    // 展开后的复杂指令会生成 Callout 节点，并触发 Eager Sync 保证 GC 安全
    std::string code = R"(
        complex_op(x) = {
            // 1. 定义类 (触发 CLASS Callout)
            class Temp {
                init(v) = { self.v = v }
            }
            
            // 2. 实例化并返回 (触发 CALL Callout 和 GET_PROP Guard)
            obj = Temp(x + 4)
            return obj.v
        }

        test() = {
            sum = 0
            i = 0
            while (i < 2000) {
                // 每次循环调用 complex_op(1)
                // x + 4 = 5
                sum = sum + complex_op(1)
                i = i + 1
            }
            return sum
        }
        
        return test()
    )";
    
    try {
        jc::Lexer lexer(code, "<complex_inline_test>");
        auto tokens = lexer.tokenize();
        jc::Parser parser(tokens);
        auto ast = parser.parse();
        
        jc::Resolver resolver;
        resolver.resolve(ast.get());
        
        auto mainFn = std::make_shared<CompiledFunction>();
        mainFn->name = "<complex_inline_test>";
        mainFn->sourceFile = "<complex_inline_test>";
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
            std::vector<std::string>{}, std::vector<bool>{}, "<complex_inline_test>", nullptr
        );
        cls->compiledFnIndex = mainFnIdx;
        Value testFunc(cls);
        GcValueGuard testFuncGuard(testFunc); // ★ 必须保护，防止被 GC 误杀
        
        std::cout << "Executing complex inline test function..." << std::endl;
        
        // 强制触发一次 GC，确保环境干净
        GcHeap::get().collectGarbage();
        
        Value res = vm.callVMFunction(testFunc.asFunction()->compiledFnIndex, {}, testFunc.asFunction());
        
        int expected = 2000 * 5; // 10000
        
        std::cout << "Result: " << res.asInt32() << std::endl;
        std::cout << "Expected: " << expected << std::endl;
        
        if (res.isInt32() && res.asInt32() == expected) {
            std::cout << "\nPhase 17: JIT Complex Inline & GC Safety Test passed successfully!" << std::endl;
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
