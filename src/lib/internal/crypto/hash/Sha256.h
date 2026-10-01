// SHA-256（FIPS 180-4）。TLS 1.3 的 SHA-256 套件、HMAC、HKDF 都建立在它之上。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

// 增量 SHA-256。update 可多次调用，final 输出 32 字节并清零内部状态。
struct Sha256 {
    uint32_t state[8];
    uint64_t bitLen;
    uint8_t buffer[64];
    size_t bufLen;

    void init();
    void update(const uint8_t* data, size_t len);
    void final(uint8_t out[32]);
};

// 一次性接口。
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

} // namespace jc2::crypto
