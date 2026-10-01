#include "BigNum.h"

#include <cstring>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace jc2::crypto {

namespace {

inline void mul64(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi) {
#if defined(_MSC_VER)
    lo = _umul128(a, b, &hi);
#else
    const unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
    lo = static_cast<uint64_t>(r);
    hi = static_cast<uint64_t>(r >> 64);
#endif
}

inline int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// r = x - y - borrow，返回新的借位
inline uint64_t subWithBorrow(uint64_t x, uint64_t y, uint64_t borrow, uint64_t& out) {
    const uint64_t d1 = x - y;
    const uint64_t b1 = (x < y) ? 1u : 0u;
    const uint64_t d2 = d1 - borrow;
    const uint64_t b2 = (d1 < borrow) ? 1u : 0u;
    out = d2;
    return b1 | b2;
}

} // namespace

void BigNum::trim() {
    while (len_ > 0 && limb_[len_ - 1] == 0) --len_;
}

BigNum BigNum::fromUint64(uint64_t v) {
    BigNum r;
    if (v != 0) {
        r.limb_[0] = v;
        r.len_ = 1;
    }
    return r;
}

BigNum BigNum::fromBytes(const uint8_t* data, size_t len) {
    BigNum r;
    for (size_t i = 0; i < len; ++i) {
        const size_t byteFromEnd = len - 1 - i;       // 0 = 最低字节
        const size_t limbIndex = byteFromEnd / 8;
        if (limbIndex >= kMaxLimbs) break;            // 超长输入：只取低 4160 位
        const size_t shift = (byteFromEnd % 8) * 8;
        r.limb_[limbIndex] |= static_cast<uint64_t>(data[i]) << shift;
        if (limbIndex + 1 > r.len_) r.len_ = limbIndex + 1;
    }
    r.trim();
    return r;
}

bool BigNum::fromHex(const char* hex, BigNum& out) {
    if (hex == nullptr) return false;
    std::string s = hex;
    if (s.empty()) return false;
    if (s.size() % 2 != 0) s.insert(s.begin(), '0');
    const size_t bytes = s.size() / 2;
    if (bytes > kMaxLimbs * 8) return false;
    std::vector<uint8_t> buf(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        const int hi = hexValue(s[2 * i]);
        const int lo = hexValue(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        buf[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    out = fromBytes(buf.data(), buf.size());
    return true;
}

bool BigNum::toBytes(uint8_t* out, size_t len) const {
    // 先检查是否放得下
    const size_t fitLimbs = (len + 7) / 8;
    for (size_t i = fitLimbs; i < len_; ++i) {
        if (limb_[i] != 0) return false;
    }
    if (fitLimbs > 0 && len % 8 != 0) {
        const unsigned highBits = static_cast<unsigned>(8 - (len % 8)) * 8;
        if (fitLimbs - 1 < len_ && (limb_[fitLimbs - 1] >> (64 - highBits)) != 0) return false;
    }
    std::memset(out, 0, len);
    for (size_t i = 0; i < len; ++i) {
        const size_t byteFromEnd = len - 1 - i;
        const size_t limbIndex = byteFromEnd / 8;
        if (limbIndex >= len_) continue;
        out[i] = static_cast<uint8_t>(limb_[limbIndex] >> ((byteFromEnd % 8) * 8));
    }
    return true;
}

std::string BigNum::toHex() const {
    static const char* digits = "0123456789abcdef";
    if (len_ == 0) return "0";
    std::string out;
    for (size_t i = len_; i-- > 0;) {
        for (int nib = 15; nib >= 0; --nib) {
            const unsigned v = static_cast<unsigned>((limb_[i] >> (nib * 4)) & 0xf);
            if (out.empty() && v == 0) continue;   // 跳过前导零
            out.push_back(digits[v]);
        }
    }
    return out.empty() ? std::string("0") : out;
}

size_t BigNum::bitLength() const {
    if (len_ == 0) return 0;
    size_t bits = (len_ - 1) * 64;
    uint64_t v = limb_[len_ - 1];
    while (v != 0) {
        ++bits;
        v >>= 1;
    }
    return bits;
}

bool BigNum::bit(size_t index) const {
    const size_t limbIndex = index / 64;
    if (limbIndex >= len_) return false;
    return ((limb_[limbIndex] >> (index % 64)) & 1u) != 0;
}

int BigNum::compare(const BigNum& other) const {
    if (len_ != other.len_) return len_ < other.len_ ? -1 : 1;
    for (size_t i = len_; i-- > 0;) {
        if (limb_[i] != other.limb_[i]) return limb_[i] < other.limb_[i] ? -1 : 1;
    }
    return 0;
}

BigNum BigNum::addRaw(const BigNum& a, const BigNum& b) {
    BigNum r;
    const size_t n = (a.len_ > b.len_) ? a.len_ : b.len_;
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t x = (i < a.len_) ? a.limb_[i] : 0;
        const uint64_t y = (i < b.len_) ? b.limb_[i] : 0;
        uint64_t s = x + y;
        uint64_t c = (s < x) ? 1u : 0u;
        const uint64_t s2 = s + carry;
        if (s2 < s) ++c;
        s = s2;
        r.limb_[i] = s;
        carry = c;
    }
    r.len_ = n;
    if (carry != 0 && n < kMaxLimbs) {
        r.limb_[n] = carry;
        r.len_ = n + 1;
    }
    r.trim();
    return r;
}

BigNum BigNum::sub(const BigNum& a, const BigNum& b) {
    BigNum r;
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.len_; ++i) {
        const uint64_t y = (i < b.len_) ? b.limb_[i] : 0;
        uint64_t out = 0;
        borrow = subWithBorrow(a.limb_[i], y, borrow, out);
        r.limb_[i] = out;
    }
    r.len_ = a.len_;
    r.trim();
    return r;
}

void BigNum::mulWide(const BigNum& a, const BigNum& b, uint64_t* out, size_t& outLen) {
    std::memset(out, 0, sizeof(uint64_t) * kMaxWideLimbs);
    for (size_t i = 0; i < a.len_; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.len_; ++j) {
            uint64_t lo = 0, hi = 0;
            mul64(a.limb_[i], b.limb_[j], lo, hi);
            uint64_t sum = out[i + j] + lo;
            uint64_t carryOut = (sum < lo) ? 1u : 0u;
            const uint64_t sum2 = sum + carry;
            if (sum2 < sum) ++carryOut;
            out[i + j] = sum2;
            carry = hi + carryOut;
        }
        size_t k = i + b.len_;
        while (carry != 0 && k < kMaxWideLimbs) {
            const uint64_t sum = out[k] + carry;
            carry = (sum < carry) ? 1u : 0u;
            out[k] = sum;
            ++k;
        }
    }
    outLen = (a.len_ + b.len_ <= kMaxWideLimbs) ? (a.len_ + b.len_) : kMaxWideLimbs;
    while (outLen > 0 && out[outLen - 1] == 0) --outLen;
}

BigNum BigNum::modWide(const uint64_t* v, size_t vLen, const BigNum& m) {
    BigNum r;
    if (m.len_ == 0) return r;
    for (size_t i = vLen; i-- > 0;) {
        for (int b = 63; b >= 0; --b) {
            // r = r * 2 + bit
            uint64_t carry = (v[i] >> b) & 1u;
            for (size_t k = 0; k < r.len_ || carry != 0; ++k) {
                if (k >= kMaxLimbs) return BigNum();   // 理论上不可达（模数 ≤ 4096 位）
                const uint64_t cur = r.limb_[k];
                const uint64_t next = (cur << 1) | carry;
                carry = cur >> 63;
                r.limb_[k] = next;
                if (k + 1 > r.len_) r.len_ = k + 1;
            }
            r.trim();
            if (r.compare(m) >= 0) {
                uint64_t borrow = 0;
                for (size_t k = 0; k < r.len_; ++k) {
                    const uint64_t sub = (k < m.len_) ? m.limb_[k] : 0;
                    uint64_t out = 0;
                    borrow = subWithBorrow(r.limb_[k], sub, borrow, out);
                    r.limb_[k] = out;
                }
                r.trim();
            }
        }
    }
    return r;
}

BigNum BigNum::mod(const BigNum& a, const BigNum& m) {
    uint64_t wide[kMaxWideLimbs];
    std::memset(wide, 0, sizeof(wide));
    const size_t n = (a.len_ < kMaxWideLimbs) ? a.len_ : kMaxWideLimbs;
    for (size_t i = 0; i < n; ++i) wide[i] = a.limb_[i];
    return modWide(wide, n, m);
}

BigNum BigNum::addMod(const BigNum& a, const BigNum& b, const BigNum& m) {
    const BigNum sum = addRaw(a, b);
    if (sum.compare(m) >= 0) {
        return sub(sum, m);   // a,b < m ⇒ a+b < 2m，减一次即可
    }
    return sum;
}

BigNum BigNum::subMod(const BigNum& a, const BigNum& b, const BigNum& m) {
    if (a.compare(b) >= 0) return sub(a, b);
    return sub(addRaw(a, m), b);   // a < b ⇒ a + m - b < m
}

BigNum BigNum::mulMod(const BigNum& a, const BigNum& b, const BigNum& m) {
    uint64_t wide[kMaxWideLimbs];
    size_t wideLen = 0;
    mulWide(a, b, wide, wideLen);
    return modWide(wide, wideLen, m);
}

BigNum BigNum::expMod(const BigNum& base, const BigNum& exponent, const BigNum& m) {
    if (m.len_ == 0) return BigNum();
    BigNum result = fromUint64(1);
    if (result.compare(m) >= 0) result = sub(result, m);   // m == 1 的退化情形
    const BigNum b = mod(base, m);
    const size_t bits = exponent.bitLength();
    for (size_t i = bits; i-- > 0;) {
        result = mulMod(result, result, m);
        if (exponent.bit(i)) {
            result = mulMod(result, b, m);
        }
    }
    return result;
}

BigNum BigNum::inverseModPrime(const BigNum& a, const BigNum& prime) {
    const BigNum pMinus2 = sub(prime, fromUint64(2));
    return expMod(a, pMinus2, prime);
}

} // namespace jc2::crypto
