#ifndef JC2_JIT_ARENA_ALLOCATOR_H
#define JC2_JIT_ARENA_ALLOCATOR_H

#include <vector>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <algorithm>
#include <new>

namespace jc {
namespace jit {

class ArenaAllocator {
public:
    ArenaAllocator(size_t blockSize = 64 * 1024) : blockSize_(blockSize) {
        allocateBlock();
    }

    ~ArenaAllocator() {
        for (uint8_t* block : blocks_) {
            delete[] block;
        }
    }

    // 禁用拷贝和移动
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        size_t padding = (alignment - (reinterpret_cast<uintptr_t>(ptr_) % alignment)) % alignment;
        if (ptr_ + padding + size > end_) {
            allocateBlock(size > blockSize_ ? size : blockSize_);
            padding = (alignment - (reinterpret_cast<uintptr_t>(ptr_) % alignment)) % alignment;
        }
        uint8_t* result = ptr_ + padding;
        ptr_ = result + size;
        return result;
    }

    template <typename T, typename... Args>
    T* allocateObject(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

private:
    void allocateBlock(size_t size = 0) {
        size_t allocSize = size > 0 ? size : blockSize_;
        uint8_t* block = new uint8_t[allocSize];
        blocks_.push_back(block);
        ptr_ = block;
        end_ = block + allocSize;
    }

    size_t blockSize_;
    std::vector<uint8_t*> blocks_;
    uint8_t* ptr_ = nullptr;
    uint8_t* end_ = nullptr;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_ARENA_ALLOCATOR_H
