#include "ExecutableMemory.h"
#include <iostream>
#include <stdexcept>

using namespace jc::jit;

// 定义一个简单的函数指针类型，返回 int
typedef int (*SimpleJitFunc)();

void test_executable_memory() {
    std::cout << "Running ExecutableMemory test..." << std::endl;

    // 1. 分配一页内存 (通常是 4096 字节)
    ExecutableMemory mem(4096);

    // 2. 写入 x86-64 机器码
    // mov eax, 42  (B8 2A 00 00 00)
    // ret          (C3)
    uint8_t* code = mem.get();
    code[0] = 0xB8;
    code[1] = 0x2A;
    code[2] = 0x00;
    code[3] = 0x00;
    code[4] = 0x00;
    code[5] = 0xC3;

    // 3. 修改内存权限为 RX 并刷新指令缓存
    mem.finalize();

    // 4. 转换为函数指针并调用
    // 注意：在标准 C++ 中，将数据指针转换为函数指针是条件支持的 (Conditionally-supported)。
    // 在 Windows (GetProcAddress) 和 POSIX (dlsym) 的 ABI 规范中，这是标准且唯一合法的做法。
    SimpleJitFunc func = reinterpret_cast<SimpleJitFunc>(mem.get());
    
    int result = func();
    
    if (result == 42) {
        std::cout << "ExecutableMemory test passed! Result: " << result << std::endl;
    } else {
        throw std::runtime_error("ExecutableMemory test failed! Unexpected result.");
    }
}

int main() {
    try {
        test_executable_memory();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
