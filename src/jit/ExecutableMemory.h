#ifndef JC2_JIT_EXECUTABLE_MEMORY_H
#define JC2_JIT_EXECUTABLE_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace jc {
namespace jit {

class ExecutableMemory {
public:
    ExecutableMemory() = default;
    explicit ExecutableMemory(size_t size);
    ~ExecutableMemory();

    // 禁用拷贝构造和赋值，防止底层内存被 Double-Free
    ExecutableMemory(const ExecutableMemory&) = delete;
    ExecutableMemory& operator=(const ExecutableMemory&) = delete;

    // 允许移动语义
    ExecutableMemory(ExecutableMemory&& other) noexcept;
    ExecutableMemory& operator=(ExecutableMemory&& other) noexcept;

    // 分配具有读写 (RW) 权限的内存页
    void allocate(size_t size);

    // 释放内存页
    void free();

    // 将内存权限修改为读执行 (RX)，并刷新 CPU 指令缓存 (I-Cache)
    void finalize();

    // 获取内存指针
    uint8_t* get() const { return memory_; }
    
    // 获取分配的内存大小
    size_t size() const { return size_; }

private:
    uint8_t* memory_ = nullptr;
    size_t size_ = 0;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_EXECUTABLE_MEMORY_H
