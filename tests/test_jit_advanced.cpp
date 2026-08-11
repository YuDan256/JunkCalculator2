// 强制启用断言，防止在 Release 模式下 assert 被优化掉
#undef NDEBUG

#include <iostream>
#include <cassert>
#include <string>
#include <cmath>
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

void testMathIntrinsics() {
    std::cout << "[TEST] Math Intrinsics (Hardware FPU/SSE2)...\n";
    VM vm;
    
    // 手动注册测试所需的内置函数
    vm.registerBuiltin("sqrtD", [](const std::vector<Value>& args) -> Value {
        return Value::fromDouble(std::sqrt(args[0].asDouble()));
    }, {1});
    vm.registerBuiltin("sin", [](const std::vector<Value>& args) -> Value {
        return Value::fromDouble(std::sin(args[0].asDouble()));
    }, {1});

    // 测试 sqrtD 和 sin 是否被正确内联为硬件指令
    std::string code = R"(
        f(x) = {
            sum = 0.0
            for (i = 0; i < 10; i += 1) {
                sum = sqrtD(x) + sin(x)
            }
            return sum
        }
        res = 0.0
        for (k = 0; k < 1500; k += 1) {
            res = f(4.0)
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    double expected = std::sqrt(4.0) + std::sin(4.0);
    assert(res.isDouble());
    assert(std::abs(res.asDoubleRaw() - expected) < 1e-9);
    std::cout << "  -> Passed.\n";
}

void testDynamicProperties() {
    std::cout << "[TEST] Dynamic Properties (Dict/Instance Callouts)...\n";
    VM vm;
    // 测试 JIT 循环中高频访问字典属性
    std::string code = R"(
        f(d) = {
            sum = 0
            for (i = 0; i < 10; i += 1) {
                sum += d.val
            }
            return sum
        }
        res = 0
        for (k = 0; k < 1500; k += 1) {
            res = f({val: 2})
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    assert(res.isInt32() && res.asInt32() == 20);
    std::cout << "  -> Passed.\n";
}

void testMatrixIndexing() {
    std::cout << "[TEST] Matrix Indexing (2D Callouts)...\n";
    VM vm;
    // 测试 JIT 循环中高频读写矩阵元素
    std::string code = R"(
        f(m) = {
            for (i = 0; i < 10; i += 1) {
                m[0, 0] = m[0, 0] + 1
            }
            return m[0, 0]
        }
        res = 0
        for (k = 0; k < 1500; k += 1) {
            res = f([10])
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    assert(res.isDouble() && res.asDoubleRaw() == 20.0);
    std::cout << "  -> Passed.\n";
}

void testPhiNodesAndControlFlow() {
    std::cout << "[TEST] Complex Control Flow & Phi Nodes...\n";
    VM vm;
    // 测试循环内部的 if-else 分支，验证 Phi 节点合并和寄存器分配
    std::string code = R"(
        f() = {
            res = 0
            for (i = 0; i < 10; i += 1) {
                if (i % 2 == 0) {
                    res += 1
                } else {
                    res += 2
                }
            }
            return res
        }
        ans = 0
        for (k = 0; k < 1500; k += 1) {
            ans = f()
        }
        return ans
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    // 5 次 +1，5 次 +2 = 15
    assert(res.isInt32() && res.asInt32() == 15);
    std::cout << "  -> Passed.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   JIT Advanced Features Unit Tests     \n";
    std::cout << "========================================\n";
    
    try {
        testMathIntrinsics();
        testDynamicProperties();
        testMatrixIndexing();
        testPhiNodesAndControlFlow();
        std::cout << "\nAll Advanced JIT tests passed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "\n[FAILED] Exception caught: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
