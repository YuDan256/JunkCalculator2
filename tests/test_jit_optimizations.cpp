// 强制启用断言，防止在 Release 模式下 assert 被优化掉
#undef NDEBUG

#include <iostream>
#include <cassert>
#include <string>
#include "vm/VM.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "compiler/Resolver.h"
#include "compiler/IRBuilder.h"
#include "compiler/IROptimizer.h"
#include "compiler/RegisterAllocator.h"
#include "compiler/Emitter.h"

using namespace jc;

// 定义在 main.cpp 中但在单元测试中缺失的全局变量
bool g_showIR = false;
bool g_autoDebug = false;
bool g_profile = false;
bool g_showMachineCode = false;
bool g_showHIR = false;
bool g_enableJit = true;

// 辅助函数：编译并执行一段 JC2 代码
Value runScript(VM& vm, const std::string& code) {
    Lexer lexer(code, "<test>");
    auto tokens = lexer.tokenize();
    Parser parser(tokens, "<test>");
    auto ast = parser.parse();
    
    auto modFn = std::make_shared<CompiledFunction>();
    modFn->name = "<main>";
    
    Resolver resolver;
    resolver.resolve(ast.get());
    
    IRGraph fnGraph;
    IRBuilder fnBuilder(&fnGraph, &vm.getCompiledFunctions(), nullptr, modFn.get(), &resolver.exprSymbols, &resolver.patternSymbols);
    fnBuilder.build(ast.get());
    
    IROptimizer::optimize(&fnGraph);
    RegisterAllocator::allocate(&fnGraph);
    modFn->localCount = Emitter::emit(&fnGraph, modFn->chunk);
    
    vm.getCompiledFunctions().push_back(modFn);
    return vm.execute(modFn->chunk, modFn->localCount);
}

ObjClosure* getGlobalFunction(VM& vm, const std::string& name) {
    Value val = vm.getGlobal(name);
    if (val.isFunctionClosure()) return val.asFunction();
    return nullptr;
}

void testConstantFolding() {
    std::cout << "[TEST] Constant Folding...\n";
    VM vm;
    std::string code = R"(
        f() = {
            a = 10 + 20 * 2
            b = a / 5
            return b
        }
        res = 0
        for (i = 0; i < 1500; i += 1) {
            res = f()
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    assert(res.isInt32() && res.asInt32() == 10);
    std::cout << "  -> Passed.\n";
}

void testAlgebraicSimplification() {
    std::cout << "[TEST] Algebraic Simplification...\n";
    VM vm;
    std::string code = R"(
        f(x) = {
            a = x + 0
            b = a * 1
            c = b - 0
            d = c / 1
            return d
        }
        res = 0
        for (i = 0; i < 1500; i += 1) {
            res = f(42)
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    assert(res.isInt32() && res.asInt32() == 42);
    std::cout << "  -> Passed.\n";
}

void testDeadCodeElimination() {
    std::cout << "[TEST] Dead Code Elimination...\n";
    VM vm;
    std::string code = R"(
        f(x) = {
            a = x * 100 // Dead code
            b = x + 200 // Dead code
            return x
        }
        res = 0
        for (i = 0; i < 1500; i += 1) {
            res = f(99)
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    assert(res.isInt32() && res.asInt32() == 99);
    std::cout << "  -> Passed.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   JIT Optimizations Unit Tests         \n";
    std::cout << "========================================\n";
    
    try {
        testConstantFolding();
        testAlgebraicSimplification();
        testDeadCodeElimination();
        std::cout << "\nAll JIT Optimization tests passed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "\n[FAILED] Exception caught: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
