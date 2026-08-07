// 强制启用断言，防止在 Release 模式下 assert 被优化掉导致变量未使用警告和测试失效
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

// 辅助函数：获取全局函数的闭包
ObjClosure* getGlobalFunction(VM& vm, const std::string& name) {
    Value val = vm.getGlobal(name);
    if (val.isFunctionClosure()) {
        return val.asFunction();
    }
    return nullptr;
}

void testTypeGuardDeopt() {
    std::cout << "[TEST] Type Guard Deoptimization...\n";
    VM vm;
    std::string code = R"(
        f(a, b) = a + b
        for (i = 0; i < 1500; i += 1) { f(1, 2) }
    )";
    runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    int fnIdx = f->compiledFnIndex;
    
    // 验证已触发 JIT 编译
    assert(vm.getJitEntryPoint(fnIdx) != nullptr);
    
    // 传入 Double 触发去优化
    std::string triggerCode = R"(
        return f(1.5, 2)
    )";
    Value res = runScript(vm, triggerCode);
    
    // 验证去优化发生（入口点被清空）
    assert(vm.getJitEntryPoint(fnIdx) == nullptr);
    // 验证结果正确（解释器接管）
    assert(res.isDouble() && res.asDoubleRaw() == 3.5);
    std::cout << "  -> Passed.\n";
}

void testArithmeticOverflowDeopt() {
    std::cout << "[TEST] Arithmetic Overflow Deoptimization...\n";
    VM vm;
    std::string code = R"(
        f(a, b) = a + b
        for (i = 0; i < 1500; i += 1) { f(10, 20) }
    )";
    runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    int fnIdx = f->compiledFnIndex;
    assert(vm.getJitEntryPoint(fnIdx) != nullptr);
    
    // 传入导致溢出的 Int32
    std::string triggerCode = R"(
        return f(2000000000, 2000000000)
    )";
    Value res = runScript(vm, triggerCode);
    
    assert(vm.getJitEntryPoint(fnIdx) == nullptr);
    // 结果应被提升为 Double 或 BigInt (JC2 中通常是 Double 4000000000)
    assert(res.asDouble() == 4000000000.0);
    std::cout << "  -> Passed.\n";
}

void testComplexStateReconstruction() {
    std::cout << "[TEST] Complex State Reconstruction...\n";
    VM vm;
    std::string code = R"(
        f(x, y) = {
            a = x + 1
            b = a * 2
            c = b - y
            return c
        }
        for (i = 0; i < 1500; i += 1) { f(10, 5) }
    )";
    runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    int fnIdx = f->compiledFnIndex;
    assert(vm.getJitEntryPoint(fnIdx) != nullptr);
    
    // 传入 Double 触发深层去优化
    std::string triggerCode = R"(
        return f(10, 5.5)
    )";
    Value res = runScript(vm, triggerCode);
    
    assert(vm.getJitEntryPoint(fnIdx) == nullptr);
    assert(res.isDouble() && res.asDoubleRaw() == 16.5);
    std::cout << "  -> Passed.\n";
}

void testCalloutStateReconstruction() {
    std::cout << "[TEST] Callout State Reconstruction...\n";
    VM vm;
    std::string code = R"(
        f(x, y) = {
            // 触发 BUILD_LIST (Callout)
            arr = @[x, x * 2, x * 3]
            // 触发 INDEX_GET (Callout)
            val = arr[1]
            // 触发类型守卫去优化
            return val + y
        }
        for (i = 0; i < 1500; i += 1) { f(10, 5) }
    )";
    runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    int fnIdx = f->compiledFnIndex;
    assert(vm.getJitEntryPoint(fnIdx) != nullptr);
    
    // 传入 Double 触发去优化
    std::string triggerCode = R"(
        return f(10, 5.5)
    )";
    Value res = runScript(vm, triggerCode);
    
    assert(vm.getJitEntryPoint(fnIdx) == nullptr);
    assert(res.isDouble() && res.asDoubleRaw() == 25.5);
    std::cout << "  -> Passed.\n";
}

void testExceptionDeopt() {
    std::cout << "[TEST] Exception Deoptimization (Callout Throw)...\n";
    VM vm;
    std::string code = R"(
        f(arr, idx) = {
            // INDEX_GET 可能会抛出越界异常
            return arr[idx] + 1
        }
        for (i = 0; i < 1500; i += 1) { f(@[10, 20, 30], 1) }
    )";
    runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    int fnIdx = f->compiledFnIndex;
    assert(vm.getJitEntryPoint(fnIdx) != nullptr);
    
    // 传入越界索引，触发 Callout 内部抛出异常 -> JIT 捕获 -> 去优化 -> 解释器抛出
    std::string triggerCode = R"(
        try {
            f(@[10, 20, 30], 5)
            return -1
        } catch (e) {
            return 42
        }
    )";
    Value res = runScript(vm, triggerCode);
    
    assert(vm.getJitEntryPoint(fnIdx) == nullptr);
    assert(res.isInt32() && res.asInt32() == 42);
    std::cout << "  -> Passed.\n";
}

void testOSR() {
    std::cout << "[TEST] On-Stack Replacement (OSR)...\n";
    VM vm;
    std::string code = R"(
        f() = {
            sum = 0
            for (i = 0; i < 2000; i += 1) {
                sum += i
            }
            return sum
        }
        return f()
    )";
    Value res = runScript(vm, code);
    
    assert(res.isInt32() && res.asInt32() == 1999000);
    std::cout << "  -> Passed.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   JIT Deoptimization & OSR Unit Tests  \n";
    std::cout << "========================================\n";
    
    try {
        testTypeGuardDeopt();
        testArithmeticOverflowDeopt();
        testComplexStateReconstruction();
        testCalloutStateReconstruction();
        testExceptionDeopt();
        testOSR();
        std::cout << "\nAll JIT tests passed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "\n[FAILED] Exception caught: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
