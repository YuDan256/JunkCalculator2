// AES-128-GCM（NIST SP 800-38D）。TLS 1.3 的 TLS_AES_128_GCM_SHA256 只需要它。
// 只实现正向分组密码：GCM 只用加密方向，不需要 AES 逆变换。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

// 加密并生成 16 字节 tag。pt 与 ct 可为同一块（就地）。iv 长度任意（96 位是 TLS 用法）。
void aes128GcmSeal(const uint8_t key[16], const uint8_t* iv, size_t ivLen, const uint8_t* aad, size_t aadLen,
                   const uint8_t* pt, size_t ptLen, uint8_t* ct, uint8_t tag[16]);

// 校验 tag 后解密。tag 不匹配返回 false（常数时间比较），且不写出明文。
bool aes128GcmOpen(const uint8_t key[16], const uint8_t* iv, size_t ivLen, const uint8_t* aad, size_t aadLen,
                   const uint8_t* ct, size_t ctLen, const uint8_t tag[16], uint8_t* pt);

} // namespace jc2::crypto
