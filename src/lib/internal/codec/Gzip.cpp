#include "Gzip.h"

#include "../../../utils/deflate/Deflate.h"

#include <array>

namespace jc {
namespace codec {
namespace {

// ─── 常量 ─────────────────────────────────────────────────────

constexpr uint8_t kGzipMagic0 = 0x1F;
constexpr uint8_t kGzipMagic1 = 0x8B;
constexpr uint8_t kDeflateMethod = 8;   // gzip CM 与 zlib CM 共用
constexpr size_t kGzipFixedHeader = 10; // ID1 ID2 CM FLG MTIME(4) XFL OS
constexpr size_t kGzipTrailer = 8;      // CRC32(4) ISIZE(4)
constexpr size_t kZlibHeader = 2;       // CMF FLG
constexpr size_t kZlibTrailer = 4;      // Adler-32(4)

constexpr uint8_t kFlagText = 0x01;     // FTEXT
constexpr uint8_t kFlagHcrc = 0x02;     // FHCRC
constexpr uint8_t kFlagExtra = 0x04;    // FEXTRA
constexpr uint8_t kFlagName = 0x08;     // FNAME
constexpr uint8_t kFlagComment = 0x10;  // FCOMMENT
// FLG 的 bit 5..7 是保留位，必须为 0（RFC 1952 §2.3.1）
constexpr uint8_t kFlagReserved = 0xE0;

// 防解压炸弹：DEFLATE 的膨胀比是有硬上界的（每个输出字节至少消耗 1 bit 输入，
// 一个 258 字节的回引匹配至少写 2 bit，故 out <= 1032 * n），所以"先按压缩长度
// 预筛、再按实际输出后检"就能在**不分配任何内存**的前提下把上限钉死。
// 预筛阈值取 4 * maxOutput 而不是理论极值：合法流要产出 maxOutput 字节至少
// 需要 maxOutput/1032 字节输入，4 倍余量对任何真实流都不构成限制，同时把
// 最坏情况下的临时分配压到 4 * maxOutput。
constexpr size_t kInputSlack = 4;

// **大端**读取：zlib 尾部的 Adler-32 就是数学意义上的大端 32 位数
// （Adler-32 = (B << 16) | A，RFC 1950 §9），而 gzip 尾部才是小端。
uint32_t loadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// **小端**读取：gzip 尾部的 CRC32 与 ISIZE 都是小端（RFC 1952 §2.3.1）。
uint32_t loadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string hex32(uint32_t v) {
    static const char* digits = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i) {
        s[static_cast<size_t>(i)] = digits[v & 0xF];
        v >>= 4;
    }
    return s;
}

// 跳过 gzip 头里的可选字段：从 pos 处读一个 NUL 结尾字符串。
bool skipZeroTerminated(const uint8_t* src, size_t n, size_t& pos) {
    while (pos < n) {
        if (src[pos++] == 0) return true;
    }
    return false;
}

// 裸 DEFLATE 解码 + 两种规模保护：
//   1) 预检：压缩长度远超输出上限时直接失败（不分配内存，见 kInputSlack）；
//   2) 后检：实际输出超限也失败。
bool inflateRaw(const uint8_t* src, size_t n, Bytes& out, std::string& err,
                size_t maxOutput) {
    if (n > maxOutput * kInputSlack) {
        err = "compressed input is " + std::to_string(n) +
              " bytes, too large to stay under the " + std::to_string(maxOutput) +
              "-byte output limit";
        return false;
    }
    Bytes scratch;
    if (!jc::deflateDecompress(src, n, scratch)) {
        err = "the DEFLATE stream is malformed or truncated";
        return false;
    }
    if (scratch.size() > maxOutput) {
        err = "decompressed size " + std::to_string(scratch.size()) +
              " exceeds the " + std::to_string(maxOutput) + "-byte output limit";
        return false;
    }
    out.insert(out.end(), scratch.begin(), scratch.end());
    return true;
}

} // namespace

// ─── 校验和 ───────────────────────────────────────────────────

uint32_t crc32(const uint8_t* data, size_t n) {
    // 标准 CRC-32/ISO-HDLC（反射多项式 0xEDB88320），表在首次调用时构造。
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < n; ++i) {
        a += data[i];
        if (a >= 65521u) a -= 65521u;
        b += a;
        if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

// ─── 裸 DEFLATE ───────────────────────────────────────────────

bool rawDecode(const uint8_t* src, size_t n, Bytes& out, std::string& err,
               size_t maxOutput) {
    if (n == 0) {
        err = "the DEFLATE stream is empty";
        return false;
    }
    return inflateRaw(src, n, out, err, maxOutput);
}

// ─── gzip（RFC 1952）──────────────────────────────────────────

bool gzipDecode(const uint8_t* src, size_t n, Bytes& out, std::string& err,
                size_t maxOutput) {
    if (n < kGzipFixedHeader) {
        err = "gzip stream is too short: " + std::to_string(n) + " of at least " +
              std::to_string(kGzipFixedHeader) + " header bytes";
        return false;
    }
    if (src[0] != kGzipMagic0 || src[1] != kGzipMagic1) {
        err = "not a gzip stream (bad magic, expected 1f8b)";
        return false;
    }
    if (src[2] != kDeflateMethod) {
        err = "unsupported gzip compression method " + std::to_string(src[2]) +
              " (only CM=8, deflate)";
        return false;
    }
    const uint8_t flg = src[3];
    if ((flg & kFlagReserved) != 0) {
        err = "gzip FLG has reserved bits set (0x" + hex32(flg) + ")";
        return false;
    }

    // 按 FLG 顺序跳过 FEXTRA / FNAME / FCOMMENT / FHCRC
    size_t pos = kGzipFixedHeader;
    if (flg & kFlagExtra) {
        if (pos + 2 > n) {
            err = "gzip FEXTRA length field is truncated";
            return false;
        }
        const size_t xlen = static_cast<size_t>(src[pos]) | (static_cast<size_t>(src[pos + 1]) << 8);
        pos += 2;
        if (xlen > n - pos) {
            err = "gzip FEXTRA payload is truncated";
            return false;
        }
        pos += xlen;
    }
    if (flg & kFlagName) {
        if (!skipZeroTerminated(src, n, pos)) {
            err = "gzip FNAME is not NUL-terminated";
            return false;
        }
    }
    if (flg & kFlagComment) {
        if (!skipZeroTerminated(src, n, pos)) {
            err = "gzip FCOMMENT is not NUL-terminated";
            return false;
        }
    }
    if (flg & kFlagHcrc) {
        if (pos + 2 > n) {
            err = "gzip FHCRC field is truncated";
            return false;
        }
        // 已知取舍：只跳过 FHCRC，不重算 CRC-32 的低 16 位去校验它。
        // 头 CRC 不保护载荷（载荷由尾部 CRC32 保护），目前没有调用方依赖它。
        pos += 2;
    }

    // 尾部必须在剩下的字节里
    if (n < pos + kGzipTrailer) {
        err = "gzip stream is truncated: no room for the 8-byte trailer";
        return false;
    }
    const size_t bodyLen = n - pos - kGzipTrailer;
    const uint8_t* trailer = src + n - kGzipTrailer;

    if (!inflateRaw(src + pos, bodyLen, out, err, maxOutput)) {
        return false;
    }

    const uint32_t wantCrc = loadLe32(trailer);
    const uint32_t wantSize = loadLe32(trailer + 4);
    const uint32_t gotCrc = crc32(out.data(), out.size());
    const uint32_t gotSize = static_cast<uint32_t>(out.size() & 0xFFFFFFFFu);
    if (gotCrc != wantCrc) {
        err = "gzip CRC32 mismatch: expected " + hex32(wantCrc) + ", computed " + hex32(gotCrc);
        return false;
    }
    if (gotSize != wantSize) {
        err = "gzip ISIZE mismatch: expected " + std::to_string(wantSize) + ", computed " +
              std::to_string(gotSize);
        return false;
    }
    return true;
}

// ─── zlib（RFC 1950）──────────────────────────────────────────

bool zlibDecode(const uint8_t* src, size_t n, Bytes& out, std::string& err,
                size_t maxOutput) {
    if (n < kZlibHeader + kZlibTrailer) {
        err = "zlib stream is too short: " + std::to_string(n) + " of at least " +
              std::to_string(kZlibHeader + kZlibTrailer) + " bytes";
        return false;
    }
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0F) != kDeflateMethod) {
        err = "unsupported zlib compression method " + std::to_string(cmf & 0x0F) +
              " (only CM=8, deflate)";
        return false;
    }
    if ((cmf >> 4) > 7) {
        err = "invalid zlib window size in CMF (CINFO=" + std::to_string(cmf >> 4) + ")";
        return false;
    }
    if ((static_cast<unsigned>(cmf) * 256u + static_cast<unsigned>(flg)) % 31u != 0) {
        err = "zlib header check failed ((CMF<<8|FLG) % 31 != 0)";
        return false;
    }
    if ((flg & 0x20) != 0) {
        err = "zlib FDICT (preset dictionary) is not supported";
        return false;
    }

    const uint8_t* trailer = src + n - kZlibTrailer;
    const size_t bodyLen = n - kZlibHeader - kZlibTrailer;

    if (!inflateRaw(src + kZlibHeader, bodyLen, out, err, maxOutput)) {
        return false;
    }

    const uint32_t want = loadBe32(trailer);
    const uint32_t got = adler32(out.data(), out.size());
    if (got != want) {
        err = "zlib Adler-32 mismatch: expected " + hex32(want) + ", computed " + hex32(got);
        return false;
    }
    return true;
}

} // namespace codec
} // namespace jc
