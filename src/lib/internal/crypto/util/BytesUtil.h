// 字节与十六进制工具（X.509/RSA/ECDSA 都要用）。
// 大数在 BigNum.h 里，本文件不依赖任何 JC2 运行时类型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jc2::crypto {

using Bytes = std::vector<uint8_t>;

inline std::string toHex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0f];
    }
    return out;
}

inline std::string toHex(const Bytes& b) {
    return toHex(b.data(), b.size());
}

// 解析十六进制；失败返回 false（调用方决定抛什么）。
inline bool fromHex(const std::string& s, Bytes& out) {
    if (s.size() % 2 != 0) return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nib(s[i]);
        const int lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace jc2::crypto
