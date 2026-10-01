// 密码学安全随机数：Windows 上走 BCryptGenRandom（系统 CSPRNG）。
// 注意：绝不能用 random 模块的 mt19937 作为密钥/随机数的来源。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

bool randomBytes(uint8_t* out, size_t len);

} // namespace jc2::crypto
