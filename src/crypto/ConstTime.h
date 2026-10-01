// 常数时间工具：比较与清零。
// 密码学代码里 tag / MAC / 签名的比较必须与内容无关，避免计时侧信道。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

// 常数时间相等比较：不使用短路，耗时只与长度有关。
inline bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// 常数时间清零：经 volatile 写，防止被优化掉。
inline void ctZero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) {
        *v++ = 0;
    }
}

} // namespace jc2::crypto
