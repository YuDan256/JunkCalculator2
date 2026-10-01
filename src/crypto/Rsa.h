// RSA 验签：PKCS#1 v1.5 与 RSASSA-PSS（都固定 SHA-256）。
// 只做"验签"方向：模幂用 BigInt::modPow，公钥指数通常很小。
#pragma once

#include "BytesUtil.h"

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

// 模数 n 的比特长度（用于 PSS 的 emBits）。
size_t rsaModulusBits(const Bytes& n);

// PKCS#1 v1.5 + SHA-256 验签。
bool rsaVerifyPkcs1Sha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig);

// RSASSA-PSS + SHA-256 / MGF1-SHA-256 验签；saltLen 由签名算法参数给出（通常 32）。
bool rsaVerifyPssSha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig, size_t saltLen);

} // namespace jc2::crypto
