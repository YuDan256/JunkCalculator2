#include "ExecutableMemory.h"
#include "MacroAssembler.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>

using namespace jc::jit;

// 定义一个带参数的函数指针类型 (Windows x64 ABI: 第一个参数在 ecx)
typedef int32_t (*JitAddFunc)(int32_t);

void test_macro_assembler() {
    std::cout << "Running MacroAssembler test..." << std::endl;

    MacroAssembler masm;

    // 生成机器码:
    // int func(int x) { return 42 + x; }
    
    // 1. mov eax, 42
    // 注意：我们的 API 默认发射 32 位指令，所以传入 rax 实际上会编码为 eax
    masm.mov(rax, 42);
    
    // 2. add eax, ecx
    // Windows x64 ABI 中，第一个 32 位整数参数存放在 ecx 中
    masm.add(rax, rcx);
    
    // 3. ret
    masm.ret();

    // 4. 写入可执行内存并修改权限
    ExecutableMemory mem;
    masm.finalize(mem);

    // 5. 转换为函数指针并调用
    JitAddFunc func = reinterpret_cast<JitAddFunc>(mem.get());
    
    int32_t arg = 10;
    int32_t result = func(arg);
    
    if (result == 52) {
        std::cout << "MacroAssembler test passed! Result: " << result << std::endl;
    } else {
        throw std::runtime_error("MacroAssembler test failed! Unexpected result: " + std::to_string(result));
    }
}

// ========================================================================
// Step 16: 浮点与 C++ ABI 调用测试
// ========================================================================

// 供 JIT 调用的 C++ 函数
extern "C" double my_cpp_multiplier(double x) {
    return x * 3.0;
}

typedef double (*JitFloatFunc)(double);

void test_float_and_abi() {
    std::cout << "Running Float and ABI test..." << std::endl;

    MacroAssembler masm;

    // 生成机器码:
    // double func(double x) { 
    //     double y = my_cpp_multiplier(x); 
    //     return y + 2.5; 
    // }

    // 1. Prologue (分配 Shadow Space 并对齐栈)
    masm.prologue(0);

    // 2. 调用 C++ 函数 my_cpp_multiplier
    // Windows x64 ABI: 第一个浮点参数在 xmm0，返回值在 xmm0
    // xmm0 已经包含了传入的参数 x，直接调用即可
    masm.movabs(rax, reinterpret_cast<uint64_t>(&my_cpp_multiplier));
    masm.emit8(0xFF); masm.emit8(0xD0); // call rax (绝对间接调用)

    // 3. 浮点运算: xmm0 = xmm0 + 2.5
    // 由于目前还没有实现常量池，我们通过通用寄存器和栈来加载 64 位浮点立即数
    double const_val = 2.5;
    uint64_t const_bits;
    std::memcpy(&const_bits, &const_val, sizeof(double));
    
    masm.movabs(rcx, const_bits);
    masm.push(rcx);
    masm.movsd(xmm1, Operand(rsp, 0)); // 从栈上加载到 xmm1
    masm.addsd(xmm0, xmm1);            // xmm0 += xmm1
    masm.pop(rcx);                     // 恢复栈指针

    // 4. Epilogue (恢复栈并返回)
    masm.epilogue();

    // 5. 写入可执行内存
    ExecutableMemory mem;
    masm.finalize(mem);

    // 6. 转换为函数指针并调用
    JitFloatFunc func = reinterpret_cast<JitFloatFunc>(mem.get());
    
    double arg = 10.0;
    double result = func(arg); // 期望: 10.0 * 3.0 + 2.5 = 32.5
    
    if (result == 32.5) {
        std::cout << "Float and ABI test passed! Result: " << result << std::endl;
    } else {
        throw std::runtime_error("Float and ABI test failed! Unexpected result: " + std::to_string(result));
    }
}

int main() {
    try {
        test_macro_assembler();
        test_float_and_abi();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
