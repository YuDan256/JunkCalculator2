// DER（X.690）最小解析器：只做 X.509 需要的那部分，但必须**永不越界**。
// 所有读取都做长度与边界检查；一旦发现非法结构就把 ok 置 false 并停止产出。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jc2::crypto {

using Bytes = std::vector<uint8_t>;

// ASN.1 universal 标签
enum : uint8_t {
    DER_BOOLEAN = 0x01,
    DER_INTEGER = 0x02,
    DER_BIT_STRING = 0x03,
    DER_OCTET_STRING = 0x04,
    DER_NULL = 0x05,
    DER_OID = 0x06,
    DER_UTF8_STRING = 0x0c,
    DER_SEQUENCE = 0x30,
    DER_SET = 0x31,
    DER_PRINTABLE_STRING = 0x13,
    DER_IA5_STRING = 0x16,
    DER_UTC_TIME = 0x17,
    DER_GENERALIZED_TIME = 0x18,
};

// 只读游标。pos 指向"下一个待读标签"。
struct DerReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool ok = true;

    DerReader() = default;
    DerReader(const uint8_t* d, size_t n) : data(d), size(n) {}

    bool atEnd() const { return pos >= size; }

    // 读一个 TLV；成功时 tag / 内容指针 / 内容长度有效并前进。
    bool read(uint8_t& tag, const uint8_t*& content, size_t& contentLen);

    // 期望特定标签（前一个参数与 tag 全等比较）。
    bool readExpected(uint8_t want, const uint8_t*& content, size_t& contentLen);

    // 不前进地看下一个标签。
    bool peek(uint8_t& tag) const;

    // 跳过当前 TLV。
    bool skip();
};

// DER INTEGER 内容转成大端字节（去掉前导零；负数返回 false）。
bool derIntegerToBytes(const uint8_t* p, size_t n, Bytes& out);

// DER INTEGER 内容 → 非负 int64（溢出返回 false）。
bool derIntegerToInt64(const uint8_t* p, size_t n, int64_t& out);

// 把 DER 时间（UTCTime / GeneralizedTime）转成 Unix 秒（UTC）。失败返回 false。
bool derTimeToUnix(uint8_t tag, const uint8_t* p, size_t n, int64_t& out);

// OID 内容（已去掉标签与长度）转成点分字符串，如 "1.2.840.113549.1.1.11"。
std::string derOidToString(const uint8_t* p, size_t n);

// 解析扩展序列（Extensions 的内容）：对每个 Extension 回调
//   fn(oidDotted, critical, extnValueContentPtr, extnValueLen)
// 回调返回 false 表示停止。
bool derForEachExtension(const uint8_t* p, size_t n,
                         bool (*fn)(const std::string& oid, bool critical, const uint8_t* value, size_t valueLen,
                                    void* user),
                         void* user);

} // namespace jc2::crypto
