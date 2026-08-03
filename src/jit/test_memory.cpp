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

// ========================================================================
// Step 21: 控制流与常量池验证测试
// ========================================================================

typedef double (*JitControlFlowFunc)(int32_t);

void test_control_flow_and_constants() {
    std::cout << "Running Control Flow and Constants test..." << std::endl;

    MacroAssembler masm;

    // 生成机器码:
    // double func(int32_t n) {
    //     double sum = 0.0;
    //     for (int32_t i = 0; i < n; ++i) {
    //         if (i % 2 == 0) sum += 1.5;
    //         else sum += 2.5;
    //     }
    //     return sum;
    // }

    masm.prologue(0);

    // xmm0 = 0.0 (sum)
    masm.xor_(rax, rax);
    masm.cvtsi2sd(xmm0, rax);

    // rcx = n (参数)
    // rdx = 0 (i)
    masm.xor_(rdx, rdx);

    // 注册常量到常量池
    Label& c1_lbl = masm.addConstantDouble(1.5);
    Label& c2_lbl = masm.addConstantDouble(2.5);

    Label loop_start;
    Label loop_end;
    Label is_odd;
    Label next_iter;

    // loop_start:
    masm.bind(loop_start);
    masm.cmp(rdx, rcx);
    masm.jcc(Condition::GreaterOrEqual, loop_end); // if (i >= n) goto loop_end

    // i % 2 == 0 ?
    masm.mov(rax, rdx);
    masm.and_(rax, 1);
    masm.test(rax, rax);
    masm.jcc(Condition::NotZero, is_odd); // if (i % 2 != 0) goto is_odd

    // even: sum += 1.5
    masm.movsd(xmm1, c1_lbl); // RIP-relative load
    masm.addsd(xmm0, xmm1);
    masm.jmp(next_iter);

    // odd: sum += 2.5
    masm.bind(is_odd);
    masm.movsd(xmm1, c2_lbl); // RIP-relative load
    masm.addsd(xmm0, xmm1);

    // next_iter:
    masm.bind(next_iter);
    masm.add(rdx, 1); // ++i
    masm.jmp(loop_start);

    // loop_end:
    masm.bind(loop_end);
    masm.epilogue();

    // 发射常量池 (必须在函数返回后发射，避免被当成指令执行)
    masm.emitConstantPool();

    ExecutableMemory mem;
    masm.finalize(mem);

    JitControlFlowFunc func = reinterpret_cast<JitControlFlowFunc>(mem.get());
    
    double result1 = func(4); // 1.5 + 2.5 + 1.5 + 2.5 = 8.0
    double result2 = func(5); // 8.0 + 1.5 = 9.5
    
    if (result1 == 8.0 && result2 == 9.5) {
        std::cout << "Control Flow and Constants test passed! Results: " << result1 << ", " << result2 << std::endl;
    } else {
        throw std::runtime_error("Control Flow and Constants test failed! Unexpected results: " + std::to_string(result1) + ", " + std::to_string(result2));
    }
}

int main() {
    try {
        test_macro_assembler();
        test_float_and_abi();
        test_control_flow_and_constants();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
