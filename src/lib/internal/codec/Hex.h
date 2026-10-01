#ifndef JC2_INTERNAL_CODEC_HEX_H
#define JC2_INTERNAL_CODEC_HEX_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 原生模块的二进制进出约定：**所有二进制一律小写十六进制字符串**。
// 这套编解码放在这里而不是各个模块里，是为了让 inflate 模块与
// internal/codec 的向量测试共用同一份实现（crypto 模块用的是自己那份，
// 因为它的错误类型不同，这里不强行统一）。

namespace jc {
namespace codec {

using Bytes = std::vector<uint8_t>;

// 十六进制 → 字节。返回 false 表示长度奇数或含非十六进制字符。
bool hexDecodeStrict(const std::string& s, Bytes& out);

// 字节 → 小写十六进制。
std::string hexEncode(const uint8_t* data, size_t n);
inline std::string hexEncode(const Bytes& b) { return hexEncode(b.data(), b.size()); }

} // namespace codec
} // namespace jc

#endif // JC2_INTERNAL_CODEC_HEX_H
