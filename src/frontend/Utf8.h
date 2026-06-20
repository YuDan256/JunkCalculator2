#ifndef JC2_UTF8_H
#define JC2_UTF8_H

#include <string>
#include <cctype>

namespace jc {
namespace utf8 {

    // 判断一个字节是否是 ASCII 字符
    inline bool isAscii(unsigned char c) {
        return c < 0x80;
    }

    // 判断一个字节是否是 UTF-8 的后续字节 (10xxxxxx)
    inline bool isContinuation(unsigned char c) {
        return (c & 0xC0) == 0x80;
    }

    // 判断是否可以作为标识符的起始字符 (字母, 下划线, 或非 ASCII 字符)
    inline bool isIdentifierStart(unsigned char c) {
        return std::isalpha(c) || c == '_' || c >= 0x80;
    }

    // 判断是否可以作为标识符的后续字符 (字母, 数字, 下划线, 或非 ASCII 字符)
    inline bool isIdentifierPart(unsigned char c) {
        return std::isalnum(c) || c == '_' || c >= 0x80;
    }

    // 获取 UTF-8 字符串的字符数 (Codepoint 数量)
    inline size_t length(const std::string& str) {
        size_t count = 0;
        for (size_t i = 0; i < str.length(); ++i) {
            // 只要不是后续字节，就是一个新字符的开始
            if (!isContinuation(static_cast<unsigned char>(str[i]))) {
                count++;
            }
        }
        return count;
    }

    // 获取 UTF-8 字符串中第 charIndex 个字符的字节偏移量
    inline size_t byteOffset(const std::string& str, size_t charIndex, bool isAscii = false) {
        if (isAscii) return charIndex < str.length() ? charIndex : std::string::npos;
        size_t count = 0;
        for (size_t i = 0; i < str.length(); ++i) {
            if (!isContinuation(static_cast<unsigned char>(str[i]))) {
                if (count == charIndex) return i;
                count++;
            }
        }
        return std::string::npos;
    }

    // 获取字节偏移量对应的字符索引
    inline size_t charIndex(const std::string& str, size_t byteOffset, bool isAscii = false) {
        if (isAscii) return byteOffset;
        size_t count = 0;
        for (size_t i = 0; i < byteOffset && i < str.length(); ++i) {
            if (!isContinuation(static_cast<unsigned char>(str[i]))) {
                count++;
            }
        }
        return count;
    }

    // 解码指定字符索引的 Unicode Codepoint
    inline int codepoint(const std::string& str, size_t charIndex) {
        size_t b = byteOffset(str, charIndex);
        if (b == std::string::npos) return 0;
        unsigned char c = str[b];
        if (c < 0x80) return c;
        if ((c & 0xE0) == 0xC0) {
            if (b + 1 >= str.length()) return c;
            return ((c & 0x1F) << 6) | (str[b+1] & 0x3F);
        }
        if ((c & 0xF0) == 0xE0) {
            if (b + 2 >= str.length()) return c;
            return ((c & 0x0F) << 12) | ((str[b+1] & 0x3F) << 6) | (str[b+2] & 0x3F);
        }
        if ((c & 0xF8) == 0xF0) {
            if (b + 3 >= str.length()) return c;
            return ((c & 0x07) << 18) | ((str[b+1] & 0x3F) << 12) | ((str[b+2] & 0x3F) << 6) | (str[b+3] & 0x3F);
        }
        return c;
    }

    // 将 Unicode Codepoint 编码为 UTF-8 字符串
    inline std::string fromCodepoint(int code) {
        std::string res;
        if (code <= 0x7F) {
            res += static_cast<char>(code);
        } else if (code <= 0x7FF) {
            res += static_cast<char>(0xC0 | ((code >> 6) & 0x1F));
            res += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code <= 0xFFFF) {
            res += static_cast<char>(0xE0 | ((code >> 12) & 0x0F));
            res += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            res += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code <= 0x10FFFF) {
            res += static_cast<char>(0xF0 | ((code >> 18) & 0x07));
            res += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            res += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            res += static_cast<char>(0x80 | (code & 0x3F));
        }
        return res;
    }

    // 提取 UTF-8 子串 (按字符索引)
    inline std::string substring(const std::string& str, size_t startChar, size_t charLen, bool isAscii = false) {
        if (isAscii) return str.substr(startChar, charLen);
        size_t startByte = byteOffset(str, startChar, false);
        if (startByte == std::string::npos) return "";
        
        size_t endByte = byteOffset(str, startChar + charLen, false);
        if (endByte == std::string::npos) {
            return str.substr(startByte);
        }
        return str.substr(startByte, endByte - startByte);
    }

} // namespace utf8
} // namespace jc

#endif // JC2_UTF8_H
