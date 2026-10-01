// RSA 验签：PKCS#1 v1.5 与 RSASSA-PSS，摘要可选 SHA-256 / SHA-384 / SHA-512。
// 只做"验签"方向：模幂用 BigInt::modPow，公钥指数通常很小。
#pragma once

#include "BytesUtil.h"

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

enum class RsaHash { Sha256, Sha384, Sha512 };

// 摘要长度（32 / 48 / 64）
size_t rsaHashLength(RsaHash h);
const char* rsaHashName(RsaHash h);

// 模数 n 的比特长度（用于 PSS 的 emBits）。
size_t rsaModulusBits(const Bytes& n);

// PKCS#1 v1.5 验签。
bool rsaVerifyPkcs1(const Bytes& n, const Bytes& e, const uint8_t* digest, const Bytes& sig, RsaHash hash);

// RSASSA-PSS / MGF1 验签；saltLen 由签名算法参数给出。
bool rsaVerifyPss(const Bytes& n, const Bytes& e, const uint8_t* digest, const Bytes& sig, size_t saltLen,
                  RsaHash hash);

// SHA-256 的旧入口（保留给既有调用点）
bool rsaVerifyPkcs1Sha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig);
bool rsaVerifyPssSha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig, size_t saltLen);

} // namespace jc2::crypto
