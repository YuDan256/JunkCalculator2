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

void testOsrHighRegPressure() {
    std::cout << "[TEST] OSR + High Register Pressure + Mixed Arithmetic...\n";
    VM vm;
    std::string code = R"(
        f() = {
            a1 = 1.0; a2 = 2.0; a3 = 3.0; a4 = 4.0; a5 = 5.0; a6 = 6.0; a7 = 7.0; a8 = 8.0; a9 = 9.0; a10 = 10.0
            a11 = 11.0; a12 = 12.0; a13 = 13.0; a14 = 14.0; a15 = 15.0; a16 = 16.0; a17 = 17.0; a18 = 18.0; a19 = 19.0; a20 = 20.0
            a21 = 21.0; a22 = 22.0; a23 = 23.0; a24 = 24.0; a25 = 25.0; a26 = 26.0; a27 = 27.0; a28 = 28.0; a29 = 29.0; a30 = 30.0
            sum = 0.0
            for (ty = 0; ty < 8; ty += 1) {
                y0 = a1 + ty * a2
                y1 = a3 + (ty + 1) * a4
                c0 = y0 > 0.0 ? y0 : 0.0
                c1 = y1 < 1000.0 ? y1 : 1000.0
                t = (c1 - c0) + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30
                sum = sum + t
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 3000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    // c1-c0 = y1-y0 = 6+2*ty; sum(c1-c0) = 104; sum(a5..a30) = 455; sum = 104 + 8*455 = 3744
    assert(res.isDouble() && res.asDoubleRaw() == 3744.0);
    std::cout << "  -> Passed.\n";
}

void testOsrCombined() {
    std::cout << "[TEST] OSR Combined (prop + outer-var + list + nested + mixed)...\n";
    VM vm;
    std::string code = R"(
        class G {
            H = 600
        }
        g = G()
        f() = {
            sum = 0.0
            for (e = 0; e < 5; e += 1) {
                sprite_top = 100.0 + e
                pixel_h = 10.5
                arr = @[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
                for (ty = 0; ty < 8; ty += 1) {
                    y0 = sprite_top + ty * pixel_h
                    y1 = sprite_top + (ty + 1) * pixel_h
                    clip_y0 = y0 > 0.0 ? y0 : 0.0
                    clip_y1 = y1 < g.H - 1.0 ? y1 : g.H - 1.0
                    arr[ty] = clip_y1 - clip_y0
                }
                sum = sum + arr[7]
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 3000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    // arr[7] = pixel_h = 10.5 per e; sum = 5 * 10.5 = 52.5
    assert(res.isDouble() && res.asDoubleRaw() == 52.5);
    std::cout << "  -> Passed.\n";
}

void testOsrInstanceMethod() {
    std::cout << "[TEST] OSR + Instance Method (self.H) + Mixed Arithmetic...\n";
    VM vm;
    std::string code = R"(
        class G {
            H = 600
            W = 800
            f() = {
                pixel_h = 10.5
                sprite_top = 100.0
                sum = 0.0
                for (ty = 0; ty < 8; ty += 1) {
                    y0 = sprite_top + ty * pixel_h
                    y1 = sprite_top + (ty + 1) * pixel_h
                    clip_y1 = y1 < self.H - 1.0 ? y1 : self.H - 1.0
                    sum = sum + clip_y1
                }
                return sum
            }
        }
        g = G()
        res = 0.0
        for (k = 0; k < 3000; k += 1) { res = g.f() }
        return res
    )";
    Value res = runScript(vm, code);
    // y1 = 110.5 + 10.5*ty < 599 always; sum = sum_{ty=0..7} y1 = 8*110.5 + 10.5*28 = 1178
    assert(res.isDouble() && res.asDoubleRaw() == 1178.0);
    std::cout << "  -> Passed.\n";
}

void testOsrSpriteLikeRepro() {
    std::cout << "[TEST] OSR Sprite-Like Computed Invariants Repro...\n";
    VM vm;
    std::string code = R"(
        class G {
            H = 600
            W = 800
        }
        g = G()
        f() = {
            q1=1.0; q2=2.0; q3=3.0; q4=4.0; q5=5.0; q6=6.0; q7=7.0; q8=8.0; q9=9.0; q10=10.0
            q11=11.0; q12=12.0; q13=13.0; q14=14.0; q15=15.0; q16=16.0; q17=17.0; q18=18.0; q19=19.0; q20=20.0
            q21=21.0; q22=22.0; q23=23.0; q24=24.0; q25=25.0; q26=26.0; q27=27.0; q28=28.0; q29=29.0; q30=30.0
            q31=31.0; q32=32.0; q33=33.0; q34=34.0; q35=35.0; q36=36.0; q37=37.0; q38=38.0; q39=39.0; q40=40.0
            sum = 0.0
            for (e = 0; e < 4; e += 1) {
                transY = 2.0
                base_h = g.H / transY
                scale = 0.25
                s_h = base_h * scale
                floor_y = g.H / 2.0 + base_h / 2.0
                sprite_top = floor_y - s_h
                pixel_h = s_h / 8.0
                h_coords = @[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
                for (ty = 0; ty < 8; ty += 1) {
                    y0 = sprite_top + ty * pixel_h
                    y1 = sprite_top + (ty + 1) * pixel_h
                    clip_y0 = y0 > 0.0 ? y0 : 0.0
                    clip_y1 = y1 < g.H - 1.0 ? y1 : g.H - 1.0
                    h_coords[ty] = clip_y1 - clip_y0
                }
                sum = sum + h_coords[7]
            }
            return sum + q1+q2+q3+q4+q5+q6+q7+q8+q9+q10+q11+q12+q13+q14+q15+q16+q17+q18+q19+q20+q21+q22+q23+q24+q25+q26+q27+q28+q29+q30+q31+q32+q33+q34+q35+q36+q37+q38+q39+q40
        }
        res = 0.0
        for (k = 0; k < 5000; k += 1) { res = f() }
        return res
    )";
    Value res = runScript(vm, code);
    // sum = 4 * pixel_h = 4 * 9.375 = 37.5; q-sum = 820; total = 857.5
    assert(res.isDouble() && res.asDoubleRaw() == 857.5);
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
        testOsrHighRegPressure();
        testOsrCombined();
        testOsrInstanceMethod();
        testOsrSpriteLikeRepro();
        std::cout << "\nAll JIT tests passed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "\n[FAILED] Exception caught: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
