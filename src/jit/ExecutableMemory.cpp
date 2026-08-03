#include "ExecutableMemory.h"
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace jc {
namespace jit {

ExecutableMemory::ExecutableMemory(size_t size) {
    allocate(size);
}

ExecutableMemory::~ExecutableMemory() {
    free();
}

ExecutableMemory::ExecutableMemory(ExecutableMemory&& other) noexcept
    : memory_(other.memory_), size_(other.size_) {
    other.memory_ = nullptr;
    other.size_ = 0;
}

ExecutableMemory& ExecutableMemory::operator=(ExecutableMemory&& other) noexcept {
    if (this != &other) {
        free(); // 释放当前持有的内存
        memory_ = other.memory_;
        size_ = other.size_;
        other.memory_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void ExecutableMemory::allocate(size_t size) {
    if (memory_) {
        free();
    }
    if (size == 0) {
        return;
    }

#ifdef _WIN32
    // Step 2: Windows 下分配具有读写 (RW) 权限的内存页
    // 注意：初始分配时不要直接给 PAGE_EXECUTE_READWRITE，这在某些严格的安全策略下会被拦截。
    // 我们先分配 PAGE_READWRITE，写入机器码后，再在 finalize() 中修改为 PAGE_EXECUTE_READ。
    memory_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory_) {
        throw std::runtime_error("JIT Error: VirtualAlloc failed to allocate memory.");
    }
    size_ = size;
#else
    // Step 3: POSIX 下分配具有读写 (RW) 权限的内存页
    memory_ = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (memory_ == MAP_FAILED) {
        memory_ = nullptr;
        throw std::runtime_error("JIT Error: mmap failed to allocate memory.");
    }
    size_ = size;
#endif
}

void ExecutableMemory::free() {
    if (!memory_) {
        return;
    }

#ifdef _WIN32
    // Step 2: Windows 下释放内存页
    // 注意：使用 MEM_RELEASE 时，第二个参数 dwSize 必须为 0
    VirtualFree(memory_, 0, MEM_RELEASE);
#else
    // Step 3: POSIX 下释放内存页
    munmap(memory_, size_);
#endif

    memory_ = nullptr;
    size_ = 0;
}

void ExecutableMemory::finalize() {
    if (!memory_ || size_ == 0) {
        return;
    }

#ifdef _WIN32
    DWORD oldProtect;
    if (!VirtualProtect(memory_, size_, PAGE_EXECUTE_READ, &oldProtect)) {
        throw std::runtime_error("JIT Error: VirtualProtect failed to set PAGE_EXECUTE_READ.");
    }
    // 刷新当前进程的指令缓存
    FlushInstructionCache(GetCurrentProcess(), memory_, size_);
#else
    if (mprotect(memory_, size_, PROT_READ | PROT_EXEC) != 0) {
        throw std::runtime_error("JIT Error: mprotect failed to set PROT_READ | PROT_EXEC.");
    }
    // 使用 GCC/Clang 内置函数刷新指令缓存
    __builtin___clear_cache(reinterpret_cast<char*>(memory_), reinterpret_cast<char*>(memory_) + size_);
#endif
}

} // namespace jit
} // namespace jc
