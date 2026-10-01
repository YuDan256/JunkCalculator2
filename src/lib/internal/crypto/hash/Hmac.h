// HMAC-SHA256（RFC 2104 / RFC 4231）。仅 SHA-256 一种哈希，TLS 1.3 与 HKDF 都用它。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

// out 为 32 字节。key 超过 64 字节时先做 SHA-256（RFC 2104 规定）。
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[32]);

} // namespace jc2::crypto
