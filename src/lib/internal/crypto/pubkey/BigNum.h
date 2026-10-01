// 自足定长大数（非负）：RSA 的模幂与 P-256 的域运算共用。
//
// 刻意**不依赖 JC2 运行时**（设计文档 §5：src/crypto 是纯算法层，可独立测试）：
// 若直接用 jc::BigInt，模块 DLL 与符号收集器都会缺 jc::err::* 符号而链接失败。
//
// 实现取向是"显然正确"：学校乘法 + 逐位长除规约，不做 Montgomery/Barrett。
//   - RSA 验签的公开指数很小（3 或 65537）→ 十几次模乘；
//   - P-256 一次验签约几千次模乘，每次规约 256 轮 → 十几毫秒量级，够用。
// 需要更快时再换 Montgomery（与 AES-NI 同类的后续优化，见 tls-plan §13.3）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace jc2::crypto {

class BigNum {
public:
    // 65 × 64 = 4160 位：给 4096 位模数的中间量留一肢余量。
    // 支持的模数上限按 4096 位计（RSA-4096 / P-256 都远在其内）。
    static constexpr size_t kMaxLimbs = 65;
    static constexpr size_t kMaxWideLimbs = kMaxLimbs * 2;

    BigNum() = default;

    static BigNum fromUint64(uint64_t v);
    static BigNum fromBytes(const uint8_t* data, size_t len);   // 大端
    static bool fromHex(const char* hex, BigNum& out);           // 常量书写用

    bool toBytes(uint8_t* out, size_t len) const;                // 定宽大端；溢出返回 false
    std::string toHex() const;

    bool isZero() const { return len_ == 0; }
    bool isOdd() const { return len_ > 0 && (limb_[0] & 1u) != 0; }
    size_t bitLength() const;
    bool bit(size_t index) const;
    int compare(const BigNum& other) const;

    bool operator==(const BigNum& o) const { return compare(o) == 0; }
    bool operator<(const BigNum& o) const { return compare(o) < 0; }
    bool operator>=(const BigNum& o) const { return compare(o) >= 0; }

    // 模运算：要求 0 <= a, b < m，且 m 不超过 4096 位
    static BigNum mod(const BigNum& a, const BigNum& m);
    static BigNum addMod(const BigNum& a, const BigNum& b, const BigNum& m);
    static BigNum subMod(const BigNum& a, const BigNum& b, const BigNum& m);
    static BigNum mulMod(const BigNum& a, const BigNum& b, const BigNum& m);
    static BigNum expMod(const BigNum& base, const BigNum& exponent, const BigNum& m);
    // m 必须是素数（费马小定理 a^(m-2)）：P-256 的域素数 p 与群阶 n 都满足
    static BigNum inverseModPrime(const BigNum& a, const BigNum& prime);
    // 减法，要求 a >= b
    static BigNum sub(const BigNum& a, const BigNum& b);

private:
    uint64_t limb_[kMaxLimbs] = {0};
    size_t len_ = 0;    // 有效肢数（已去高位零）；0 表示数值 0

    void trim();
    static BigNum addRaw(const BigNum& a, const BigNum& b);   // 不规约的加法
    static void mulWide(const BigNum& a, const BigNum& b, uint64_t* out, size_t& outLen);
    static BigNum modWide(const uint64_t* v, size_t vLen, const BigNum& m);
};

} // namespace jc2::crypto
