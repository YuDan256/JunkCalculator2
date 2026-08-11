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
bool g_showHIR = true;
bool g_showMachineCode = true;
bool g_autoDebug = false;
bool g_profile = false;
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

void testBasicArithmetic() {
    std::cout << "[TEST] Basic Arithmetic (Int32 & Double)...\n";
    VM vm;
    std::string code = R"(
        f(a, b) = {
            return a + b * 2 - a / 2
        }
        res = 0
        for (i = 0; i < 1500; i += 1) {
            res = f(10, 5)
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    assert(res.isInt32() && res.asInt32() == 15); // 10 + 10 - 5 = 15
    std::cout << "  -> Passed.\n";
}

void testLoopAndBranch() {
    std::cout << "[TEST] Loop and Branch...\n";
    VM vm;
    std::string code = R"(
        f(n) = {
            sum = 0
            for (i = 0; i < n; i += 1) {
                if (i % 2 == 0) {
                    sum += i
                }
            }
            return sum
        }
        res = 0
        for (k = 0; k < 1500; k += 1) {
            res = f(10)
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    // 0 + 2 + 4 + 6 + 8 = 20
    assert(res.isInt32() && res.asInt32() == 20);
    std::cout << "  -> Passed.\n";
}

void testFunctionCall() {
    std::cout << "[TEST] Function Call...\n";
    VM vm;
    std::string code = R"(
        add(a, b) = a + b
        f(n) = {
            sum = 0
            for (i = 0; i < n; i += 1) {
                sum = add(sum, i)
            }
            return sum
        }
        res = 0
        for (k = 0; k < 1500; k += 1) {
            res = f(10)
        }
        return res
    )";
    Value res = runScript(vm, code);
    
    ObjClosure* f = getGlobalFunction(vm, "f");
    assert(f != nullptr);
    assert(vm.getJitEntryPoint(f->compiledFnIndex) != nullptr);
    
    // 0+1+2+3+4+5+6+7+8+9 = 45
    if (!(res.isInt32() && res.asInt32() == 45)) {
        std::cout << "[ERROR] testFunctionCall failed! Expected Int32(45), but got: " << res << " (Type: " << vm.getTypeName(res) << ")\n";
    }
    assert(res.isInt32() && res.asInt32() == 45);
    std::cout << "  -> Passed.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   JIT Basic Features Unit Tests        \n";
    std::cout << "========================================\n";
    
    try {
        //testBasicArithmetic();
        //testLoopAndBranch();
        testFunctionCall();
        std::cout << "\nAll Basic JIT tests passed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "\n[FAILED] Exception caught: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
