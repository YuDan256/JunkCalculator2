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

void testOsrMixedArith() {
    std::cout << "[TEST] OSR + Mixed Int*Double Arithmetic...\n";
    VM vm;
    std::string code = R"(
        f() = {
            pixel_h = 10.5
            sprite_top = 100.0
            sum = 0.0
            for (ty = 0; ty < 8; ty += 1) {
                y0 = sprite_top + ty * pixel_h
                y1 = sprite_top + (ty + 1) * pixel_h
                clip_y0 = y0 > 0.0 ? y0 : 0.0
                clip_y1 = y1 < 600.0 ? y1 : 600.0
                sum = sum + (clip_y1 - clip_y0)
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 3000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    assert(res.isDouble() && res.asDoubleRaw() == 84.0);
    std::cout << "  -> Passed.\n";
}

void testOsrNestedLoop() {
    std::cout << "[TEST] OSR Nested Loop (outer x inner)...\n";
    VM vm;
    std::string code = R"(
        f() = {
            pixel_h = 10.5
            sprite_top = 100.0
            sum = 0.0
            for (x = 0; x < 50; x += 1) {
                for (ty = 0; ty < 8; ty += 1) {
                    y0 = sprite_top + ty * pixel_h
                    y1 = sprite_top + (ty + 1) * pixel_h
                    clip_y0 = y0 > 0.0 ? y0 : 0.0
                    clip_y1 = y1 < 600.0 ? y1 : 600.0
                    sum = sum + (clip_y1 - clip_y0)
                }
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 2000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    assert(res.isDouble() && res.asDoubleRaw() == 4200.0);
    std::cout << "  -> Passed.\n";
}

void testOsrMixedArithDeopt() {
    std::cout << "[TEST] OSR + Mixed Arithmetic + Deopt...\n";
    VM vm;
    std::string code = R"(
        f(n) = {
            pixel_h = 10.5
            sum = 0.0
            for (i = 0; i < n; i += 1) {
                sum = sum + i * pixel_h
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 2000; k += 1) { res = f(50) }
        return f(50.5)
    )";
    Value res = runScript(vm, code);
    // f(50.5): loop i = 0..50, sum = 10.5 * (0+1+...+50) = 10.5 * 1275 = 13387.5
    assert(res.isDouble() && res.asDoubleRaw() == 13387.5);
    std::cout << "  -> Passed.\n";
}

void testOsrInstancePropMixedArith() {
    std::cout << "[TEST] OSR + Instance Property + Mixed Arithmetic...\n";
    VM vm;
    std::string code = R"(
        class G {
            H = 600
            W = 800
        }
        g = G()
        f() = {
            pixel_h = 10.5
            sprite_top = 100.0
            sum = 0.0
            for (ty = 0; ty < 8; ty += 1) {
                y0 = sprite_top + ty * pixel_h
                y1 = sprite_top + (ty + 1) * pixel_h
                clip_y0 = y0 > 0.0 ? y0 : 0.0
                clip_y1 = y1 < g.H - 1.0 ? y1 : g.H - 1.0
                sum = sum + (clip_y1 - clip_y0)
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 3000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    assert(res.isDouble() && res.asDoubleRaw() == 84.0);
    std::cout << "  -> Passed.\n";
}

void testOsrListStore() {
    std::cout << "[TEST] OSR + List Store + Mixed Arithmetic...\n";
    VM vm;
    std::string code = R"(
        f() = {
            pixel_h = 10.5
            sprite_top = 100.0
            arr = @[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
            for (ty = 0; ty < 8; ty += 1) {
                y0 = sprite_top + ty * pixel_h
                y1 = sprite_top + (ty + 1) * pixel_h
                clip_y0 = y0 > 0.0 ? y0 : 0.0
                clip_y1 = y1 < 600.0 ? y1 : 600.0
                arr[ty] = clip_y1 - clip_y0
            }
            return arr[7]
        }
        res = 0.0
        for (k = 0; k < 3000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    assert(res.isDouble() && res.asDoubleRaw() == 10.5);
    std::cout << "  -> Passed.\n";
}

void testOsrNestedLoopOuterVar() {
    std::cout << "[TEST] OSR Nested Loop + Outer-Loop Variables...\n";
    VM vm;
    std::string code = R"(
        f() = {
            sum = 0.0
            for (e = 0; e < 5; e += 1) {
                sprite_top = 100.0 + e
                pixel_h = 10.5
                for (ty = 0; ty < 8; ty += 1) {
                    y0 = sprite_top + ty * pixel_h
                    y1 = sprite_top + (ty + 1) * pixel_h
                    clip_y0 = y0 > 0.0 ? y0 : 0.0
                    clip_y1 = y1 < 600.0 ? y1 : 600.0
                    sum = sum + (clip_y1 - clip_y0)
                }
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 3000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    assert(res.isDouble() && res.asDoubleRaw() == 420.0);
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
        testOsrMixedArith();
        testOsrNestedLoop();
        testOsrMixedArithDeopt();
        testOsrInstancePropMixedArith();
        testOsrListStore();
        testOsrNestedLoopOuterVar();
        std::cout << "\nAll JIT tests passed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "\n[FAILED] Exception caught: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
