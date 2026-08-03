#include "ExecutableMemory.h"
#include "MacroAssembler.h"
#include <iostream>
#include <stdexcept>
#include <string>

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

int main() {
    try {
        test_macro_assembler();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
