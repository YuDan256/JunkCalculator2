// NIST P-256（secp256r1）点运算、ECDH 与 ECDSA 验签。
// 仿射坐标 + BigNum 模运算：每步点加/倍点做一次模逆（费马），实现简单、易验证；
// 一次验签约十几毫秒，证书链校验完全够用。
#pragma once

#include "BigNum.h"
#include "BytesUtil.h"

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

struct P256Point {
    BigNum x;
    BigNum y;
    bool infinity = true;
};

// 未压缩点：0x04 || X(32) || Y(32)，且必须在曲线上。
bool p256DecodePoint(const uint8_t* data, size_t len, P256Point& out);

bool p256IsOnCurve(const P256Point& p);

// 标量乘法 k * p（k 为大端字节）。
P256Point p256ScalarMul(const Bytes& k, const P256Point& p);

// 基点乘法：out 为 65 字节未压缩点（0x04||X||Y）。TLS 的 key_share 需要它。
bool p256BasePointMul(const Bytes& privateKey, uint8_t out[65]);

// ECDH：输出共享点的 X 坐标（32 字节）。
bool p256Ecdh(const Bytes& privateKey, const uint8_t* peerPoint, size_t peerLen, uint8_t out[32]);

// ECDSA 验签（SHA-256）。hash32 是消息摘要，r/s 为大端字节。
bool p256EcdsaVerify(const P256Point& pub, const uint8_t hash32[32], const Bytes& r, const Bytes& s);

} // namespace jc2::crypto
