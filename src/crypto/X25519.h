// X25519（RFC 7748）：Curve25519 上的 Montgomery 阶梯。
// 5×51 位肢体实现，常数时间条件交换。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

// 计算 X25519(scalar, u)。若结果为全零（低阶点）返回 false，并按 RFC 8446 要求视为失败。
bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

// 不做全零检查的原始接口（RFC 7748 的迭代测试需要它）。
void x25519Raw(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

} // namespace jc2::crypto
