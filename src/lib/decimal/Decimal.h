#ifndef JC2_DECIMAL_H
#define JC2_DECIMAL_H

#include <vector>
#include <string>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <future>

namespace jc {

class DecVector {
    static constexpr size_t INLINE_CAPACITY = 8;
    size_t m_size;
    size_t m_capacity;
    uint32_t* m_data;
    uint32_t m_inline[INLINE_CAPACITY];

    void reallocate(size_t new_cap) {
        uint32_t* new_data = new uint32_t[new_cap];
        std::memcpy(new_data, m_data, m_size * sizeof(uint32_t));
        if (m_data != m_inline) delete[] m_data;
        m_data = new_data;
        m_capacity = new_cap;
    }
public:
    DecVector() : m_size(0), m_capacity(INLINE_CAPACITY), m_data(m_inline) {}
    DecVector(size_t count, uint32_t val = 0) : m_size(0), m_capacity(INLINE_CAPACITY), m_data(m_inline) {
        assign(count, val);
    }
    DecVector(const DecVector& other) : m_size(0), m_capacity(INLINE_CAPACITY), m_data(m_inline) {
        if (other.m_size > INLINE_CAPACITY) {
            m_data = new uint32_t[other.m_capacity];
            m_capacity = other.m_capacity;
        }
        m_size = other.m_size;
        std::memcpy(m_data, other.m_data, m_size * sizeof(uint32_t));
    }
    DecVector(DecVector&& other) noexcept : m_size(other.m_size), m_capacity(other.m_capacity) {
        if (other.m_data == other.m_inline) {
            m_data = m_inline;
            std::memcpy(m_inline, other.m_inline, m_size * sizeof(uint32_t));
        } else {
            m_data = other.m_data;
            other.m_data = other.m_inline;
            other.m_capacity = INLINE_CAPACITY;
            other.m_size = 0;
        }
    }
    ~DecVector() {
        if (m_data != m_inline) delete[] m_data;
    }
    DecVector& operator=(const DecVector& other) {
        if (this != &other) {
            if (other.m_size > m_capacity) {
                if (m_data != m_inline) delete[] m_data;
                m_capacity = other.m_capacity;
                m_data = new uint32_t[m_capacity];
            }
            m_size = other.m_size;
            std::memcpy(m_data, other.m_data, m_size * sizeof(uint32_t));
        }
        return *this;
    }
    DecVector& operator=(DecVector&& other) noexcept {
        if (this != &other) {
            if (m_data != m_inline) delete[] m_data;
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            if (other.m_data == other.m_inline) {
                m_data = m_inline;
                std::memcpy(m_inline, other.m_inline, m_size * sizeof(uint32_t));
            } else {
                m_data = other.m_data;
                other.m_data = other.m_inline;
                other.m_capacity = INLINE_CAPACITY;
                other.m_size = 0;
            }
        }
        return *this;
    }

    void push_back(uint32_t val) {
        if (m_size == m_capacity) reallocate(m_capacity * 2);
        m_data[m_size++] = val;
    }
    void pop_back() { if (m_size > 0) m_size--; }
    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    uint32_t& back() { return m_data[m_size - 1]; }
    const uint32_t& back() const { return m_data[m_size - 1]; }
    uint32_t& operator[](size_t idx) { return m_data[idx]; }
    const uint32_t& operator[](size_t idx) const { return m_data[idx]; }
    uint32_t* data() { return m_data; }
    const uint32_t* data() const { return m_data; }
    
    void resize(size_t new_size, uint32_t val = 0) {
        if (new_size > m_capacity) reallocate(std::max(m_capacity * 2, new_size));
        if (new_size > m_size) {
            std::fill(m_data + m_size, m_data + new_size, val);
        }
        m_size = new_size;
    }
    void assign(size_t count, uint32_t val) {
        if (count > m_capacity) reallocate(std::max(m_capacity * 2, count));
        m_size = count;
        std::fill(m_data, m_data + m_size, val);
    }
    template<typename Iter>
    void assign(Iter first, Iter last) {
        size_t count = static_cast<size_t>(last - first);
        if (count > m_capacity) reallocate(std::max(m_capacity * 2, count));
        m_size = count;
        std::copy(first, last, m_data);
    }
    void clear() { m_size = 0; }
    
    uint32_t* begin() { return m_data; }
    uint32_t* end() { return m_data + m_size; }
    const uint32_t* begin() const { return m_data; }
    const uint32_t* end() const { return m_data + m_size; }

    void insert(uint32_t* pos, size_t count, uint32_t val) {
        size_t idx = static_cast<size_t>(pos - m_data);
        if (m_size + count > m_capacity) reallocate(std::max(m_capacity * 2, m_size + count));
        std::memmove(m_data + idx + count, m_data + idx, (m_size - idx) * sizeof(uint32_t));
        std::fill(m_data + idx, m_data + idx + count, val);
        m_size += count;
    }

    bool operator==(const DecVector& other) const {
        if (m_size != other.m_size) return false;
        return std::memcmp(m_data, other.m_data, m_size * sizeof(uint32_t)) == 0;
    }
};

#ifndef JC2_UINT128_DEFINED
#define JC2_UINT128_DEFINED
#if defined(__SIZEOF_INT128__)
    using uint128_t = unsigned __int128;
#else
    struct uint128_t {
        uint64_t low;
        uint64_t high;
    };
    inline uint128_t mul64x64(uint64_t a, uint64_t b) {
        uint64_t a_lo = (uint32_t)a;
        uint64_t a_hi = a >> 32;
        uint64_t b_lo = (uint32_t)b;
        uint64_t b_hi = b >> 32;
        uint64_t p0 = a_lo * b_lo;
        uint64_t p1 = a_lo * b_hi;
        uint64_t p2 = a_hi * b_lo;
        uint64_t p3 = a_hi * b_hi;
        uint32_t cy = (uint32_t)(((p0 >> 32) + (uint32_t)p1 + (uint32_t)p2) >> 32);
        uint128_t res;
        res.low = p0 + (p1 << 32) + (p2 << 32);
        res.high = p3 + (p1 >> 32) + (p2 >> 32) + cy;
        return res;
    }
#endif
#endif // JC2_UINT128_DEFINED

#ifndef JC2_DIV128_DEFINED
#define JC2_DIV128_DEFINED
#if !defined(__SIZEOF_INT128__)
    inline uint64_t div128by64(uint128_t num, uint64_t den, uint64_t* rem_out) {
        double d_num = (double)num.high * 18446744073709551616.0 + (double)num.low;
        uint64_t q = (uint64_t)(d_num / (double)den);
        uint128_t q_den = mul64x64(q, den);
        
        while (q_den.high > num.high || (q_den.high == num.high && q_den.low > num.low)) {
            q--;
            if (q_den.low < den) q_den.high--;
            q_den.low -= den;
        }
        
        while (true) {
            uint128_t next_q_den = q_den;
            next_q_den.low += den;
            if (next_q_den.low < den) next_q_den.high++;
            
            if (num.high > next_q_den.high || (num.high == next_q_den.high && num.low >= next_q_den.low)) {
                q++;
                q_den = next_q_den;
            } else {
                break;
            }
        }
        
        if (rem_out) *rem_out = num.low - q_den.low;
        return q;
    }
#endif
#endif // JC2_DIV128_DEFINED

class DecInt {
public:
    inline static size_t NTT_THREAD_THRESHOLD = 4096;

    DecVector data;
    bool negative = false;
    static constexpr uint32_t BASE = 1000000000;

    DecInt() : data(1, 0), negative(false) {}
    DecInt(uint64_t v) {
        negative = false;
        if (v == 0) data.push_back(0);
        while (v > 0) {
            data.push_back(static_cast<uint32_t>(v % BASE));
            v /= BASE;
        }
    }
    DecInt(const std::string& s) {
        if (s.empty()) { data.push_back(0); return; }
        size_t start = 0;
        if (s[0] == '-') { negative = true; start = 1; }
        else if (s[0] == '+') { start = 1; }
        
        if (start == s.length()) { data.push_back(0); negative = false; return; }
        
        for (int i = static_cast<int>(s.length()); i > static_cast<int>(start); i -= 9) {
            int len = std::min(9, i - static_cast<int>(start));
            data.push_back(std::stoul(s.substr(i - len, len)));
        }
        trim();
    }

    void trim() {
        while (data.size() > 1 && data.back() == 0) data.pop_back();
        if (data.size() == 1 && data[0] == 0) negative = false;
    }

    bool isZero() const { return data.size() == 1 && data[0] == 0; }
    bool isNegative() const { return negative; }

    std::string to_string() const {
        if (isZero()) return "0";
        std::string res = negative ? "-" : "";
        res += std::to_string(data.back());
        for (int i = static_cast<int>(data.size()) - 2; i >= 0; --i) {
            std::string chunk = std::to_string(data[i]);
            res += std::string(9 - chunk.length(), '0') + chunk;
        }
        return res;
    }

    int compare_abs(const DecInt& o) const {
        if (data.size() != o.data.size()) return data.size() < o.data.size() ? -1 : 1;
        for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
            if (data[i] != o.data[i]) return data[i] < o.data[i] ? -1 : 1;
        }
        return 0;
    }

    bool operator==(const DecInt& o) const {
        return negative == o.negative && data == o.data;
    }
    bool operator<(const DecInt& o) const {
        if (negative != o.negative) return negative;
        int cmp = compare_abs(o);
        return negative ? (cmp > 0) : (cmp < 0);
    }
    bool operator>(const DecInt& o) const { return o < *this; }
    bool operator<=(const DecInt& o) const { return !(o < *this); }
    bool operator>=(const DecInt& o) const { return !(*this < o); }

    static DecInt add_abs(const DecInt& a, const DecInt& b) {
        return add_abs_ptr(a.data.data(), a.data.size(), b.data.data(), b.data.size());
    }

    static DecInt add_abs_ptr(const uint32_t* a, size_t n, const uint32_t* b, size_t m) {
        DecInt res;
        size_t sz = std::max(n, m);
        res.data.resize(sz, 0);
        uint32_t carry = 0;
        for (size_t i = 0; i < sz; ++i) {
            uint32_t sum = carry;
            if (i < n) sum += a[i];
            if (i < m) sum += b[i];
            if (sum >= BASE) { sum -= BASE; carry = 1; }
            else { carry = 0; }
            res.data[i] = sum;
        }
        if (carry) res.data.push_back(carry);
        return res;
    }

    static DecInt sub_abs(const DecInt& a, const DecInt& b) {
        DecInt res;
        res.data.resize(a.data.size(), 0);
        uint32_t borrow = 0;
        for (size_t i = 0; i < a.data.size(); ++i) {
            uint32_t sub = b.data.size() > i ? b.data[i] : 0;
            if (a.data[i] < sub + borrow) {
                res.data[i] = a.data[i] + BASE - sub - borrow;
                borrow = 1;
            } else {
                res.data[i] = a.data[i] - sub - borrow;
                borrow = 0;
            }
        }
        res.trim();
        return res;
    }

    DecInt operator+(const DecInt& o) const {
        if (negative == o.negative) {
            DecInt res = add_abs(*this, o);
            res.negative = negative;
            return res;
        }
        if (compare_abs(o) >= 0) {
            DecInt res = sub_abs(*this, o);
            res.negative = negative;
            return res;
        } else {
            DecInt res = sub_abs(o, *this);
            res.negative = o.negative;
            return res;
        }
    }

    DecInt operator-(const DecInt& o) const {
        DecInt neg_o = o;
        if (!neg_o.isZero()) neg_o.negative = !neg_o.negative;
        return *this + neg_o;
    }

    DecInt operator-() const {
        DecInt res = *this;
        if (!res.isZero()) res.negative = !res.negative;
        return res;
    }

    DecInt mul_small(uint32_t v) const {
        if (v == 0) return DecInt(0);
        if (v == 1) return *this;
        DecInt res; res.data.resize(data.size(), 0);
        uint64_t carry = 0;
        for (size_t i = 0; i < data.size(); ++i) {
            uint64_t prod = static_cast<uint64_t>(data[i]) * v + carry;
            uint64_t q = prod / BASE;
            res.data[i] = static_cast<uint32_t>(prod - q * BASE);
            carry = q;
        }
        if (carry > 0) res.data.push_back(static_cast<uint32_t>(carry));
        res.negative = negative;
        return res;
    }

    static DecInt mul_basecase(const uint32_t* a, size_t n, const uint32_t* b, size_t m) {
        DecInt res;
        if (n == 0 || m == 0) return res;
        res.data.assign(n + m, 0);
        for (size_t i = 0; i < n; ++i) {
            if (a[i] == 0) continue;
            uint64_t carry = 0;
            uint64_t d_i = a[i];
            for (size_t j = 0; j < m; ++j) {
                uint64_t prod = d_i * b[j] + res.data[i+j] + carry;
                uint64_t q = prod / 1000000000ULL;
                res.data[i+j] = static_cast<uint32_t>(prod - q * 1000000000ULL);
                carry = q;
            }
            if (carry > 0) res.data[i + m] += static_cast<uint32_t>(carry);
        }
        res.trim();
        return res;
    }

    static std::pair<DecInt, DecInt> divmod_knuth(const DecInt& a, const DecInt& b) {
        if (b.isZero()) throw std::runtime_error("Division by zero");
        DecInt absA = a.abs(), absB = b.abs();
        if (absA < absB) return {DecInt(0), absA};
        if (absB.data.size() == 1) {
            uint32_t rem;
            DecInt q = absA.div_small(absB.data[0], rem);
            q.negative = (a.negative != b.negative);
            if (q.isZero()) q.negative = false;
            DecInt r(rem);
            r.negative = a.negative;
            if (r.isZero()) r.negative = false;
            return {q, r};
        }

        int n_orig = static_cast<int>(absA.data.size());
        int m = static_cast<int>(absB.data.size());
        uint32_t d = BASE / (absB.data.back() + 1);

        auto mul_scalar = [](const DecInt& num, uint32_t scalar) {
            if (scalar == 1) return num;
            DecInt res; res.data.resize(num.data.size(), 0);
            uint64_t carry = 0;
            for (size_t i = 0; i < num.data.size(); ++i) {
                uint64_t prod = static_cast<uint64_t>(num.data[i]) * scalar + carry;
                uint64_t q = prod / 1000000000ULL;
                res.data[i] = static_cast<uint32_t>(prod - q * 1000000000ULL);
                carry = q;
            }
            if (carry > 0) res.data.push_back(static_cast<uint32_t>(carry));
            return res;
        };

        DecInt u = mul_scalar(absA, d);
        DecInt v = mul_scalar(absB, d);
        u.data.resize(n_orig + 1, 0);

        DecInt quotient;
        quotient.data.resize(n_orig - m + 1, 0);

        for (int j = n_orig - m; j >= 0; --j) {
            uint64_t num = (static_cast<uint64_t>(u.data[j + m]) * BASE) + u.data[j + m - 1];
            uint64_t v_m1 = v.data[m - 1];
            uint64_t q_hat = num / v_m1;
            uint64_t r_hat = num % v_m1;

            if (m >= 2) {
                uint64_t v_m2 = v.data[m - 2];
                uint64_t u_jm2 = u.data[j + m - 2];
                while (q_hat == BASE || q_hat * v_m2 > r_hat * BASE + u_jm2) {
                    q_hat--;
                    r_hat += v_m1;
                    if (r_hat >= BASE) break;
                }
            } else {
                if (q_hat == BASE) q_hat--;
            }

            if (q_hat == 0) {
                quotient.data[j] = 0;
                continue;
            }

            uint64_t carry = 0;
            for (int i = 0; i < m; ++i) {
                uint64_t prod = q_hat * v.data[i] + carry;
                uint64_t q_prod = prod / 1000000000ULL;
                uint32_t rem_prod = static_cast<uint32_t>(prod - q_prod * 1000000000ULL);
                
                uint64_t sub = static_cast<uint64_t>(u.data[j + i]) + 1000000000ULL - rem_prod;
                if (sub >= 1000000000ULL) {
                    u.data[j + i] = static_cast<uint32_t>(sub - 1000000000ULL);
                    carry = q_prod;
                } else {
                    u.data[j + i] = static_cast<uint32_t>(sub);
                    carry = q_prod + 1;
                }
            }
            
            bool is_borrow = u.data[j + m] < carry;
            u.data[j + m] -= static_cast<uint32_t>(carry);
            quotient.data[j] = static_cast<uint32_t>(q_hat);

            if (is_borrow) {
                quotient.data[j]--;
                uint32_t carry_add = 0;
                for (int i = 0; i < m; ++i) {
                    uint32_t sum = u.data[j + i] + v.data[i] + carry_add;
                    if (sum >= BASE) {
                        u.data[j + i] = sum - BASE;
                        carry_add = 1;
                    } else {
                        u.data[j + i] = sum;
                        carry_add = 0;
                    }
                }
                u.data[j + m] += carry_add;
            }
        }

        quotient.negative = (a.negative != b.negative);
        quotient.trim();

        DecInt remainder;
        remainder.data.resize(m, 0);
        uint64_t rem = 0;
        for (int i = m - 1; i >= 0; --i) {
            uint64_t cur = rem * BASE + u.data[i];
            remainder.data[i] = static_cast<uint32_t>(cur / d);
            rem = cur % d;
        }
        remainder.negative = a.negative;
        remainder.trim();

        return {quotient, remainder};
    }

    static constexpr uint64_t ct_inv_mod(int64_t a, int64_t m) {
        int64_t m0 = m, y = 0, x = 1;
        if (m == 1) return 0;
        while (a > 1) {
            int64_t q = a / m;
            int64_t t = m;
            m = a % m, a = t;
            t = y;
            y = x - q * y;
            x = t;
        }
        if (x < 0) x += m0;
        return static_cast<uint64_t>(x);
    }

    static uint32_t pow_mod(uint32_t base, uint32_t exp, uint32_t mod) {
        uint64_t res = 1;
        uint64_t b = base % mod;
        while (exp > 0) {
            if (exp & 1) res = (res * b) % mod;
            b = (b * b) % mod;
            exp >>= 1;
        }
        return static_cast<uint32_t>(res);
    }

    static void ntt(std::vector<uint32_t>& a, bool invert, uint32_t P, uint32_t g) {
        int n = static_cast<int>(a.size());
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            uint32_t wlen = pow_mod(g, (P - 1) / len, P);
            if (invert) wlen = static_cast<uint32_t>(ct_inv_mod(wlen, P));
            for (int i = 0; i < n; i += len) {
                uint32_t w = 1;
                for (int j = 0; j < len / 2; j++) {
                    uint32_t u = a[i + j];
                    uint32_t v = (1ULL * a[i + j + len / 2] * w) % P;
                    a[i + j] = u + v < P ? u + v : u + v - P;
                    a[i + j + len / 2] = u >= v ? u - v : u + P - v;
                    w = (1ULL * w * wlen) % P;
                }
            }
        }
        if (invert) {
            uint32_t n_inv = static_cast<uint32_t>(ct_inv_mod(n, P));
            for (uint32_t& x : a) x = (1ULL * x * n_inv) % P;
        }
    }

    static DecInt mul_ntt(const uint32_t* a, size_t n, const uint32_t* b, size_t m) {
        size_t res_size = n + m;
        size_t n_pow2 = 1;
        while (n_pow2 < res_size) n_pow2 <<= 1;

        std::vector<uint32_t> fa1(n_pow2, 0), fa2(n_pow2, 0), fa3(n_pow2, 0);
        std::vector<uint32_t> fb1(n_pow2, 0), fb2(n_pow2, 0), fb3(n_pow2, 0);

        for (size_t i = 0; i < n; ++i) {
            fa1[i] = a[i] % 2013265921;
            fa2[i] = a[i] % 2113929217;
            fa3[i] = a[i] % 2130706433;
        }
        for (size_t i = 0; i < m; ++i) {
            fb1[i] = b[i] % 2013265921;
            fb2[i] = b[i] % 2113929217;
            fb3[i] = b[i] % 2130706433;
        }

        if (n_pow2 >= NTT_THREAD_THRESHOLD) {
            auto future1 = std::async(std::launch::async, [&]() {
                ntt(fa1, false, 2013265921, 31);
                ntt(fb1, false, 2013265921, 31);
                for (size_t i = 0; i < n_pow2; ++i) fa1[i] = (1ULL * fa1[i] * fb1[i]) % 2013265921;
                ntt(fa1, true, 2013265921, 31);
            });
            auto future2 = std::async(std::launch::async, [&]() {
                ntt(fa2, false, 2113929217, 5);
                ntt(fb2, false, 2113929217, 5);
                for (size_t i = 0; i < n_pow2; ++i) fa2[i] = (1ULL * fa2[i] * fb2[i]) % 2113929217;
                ntt(fa2, true, 2113929217, 5);
            });
            ntt(fa3, false, 2130706433, 3);
            ntt(fb3, false, 2130706433, 3);
            for (size_t i = 0; i < n_pow2; ++i) fa3[i] = (1ULL * fa3[i] * fb3[i]) % 2130706433;
            ntt(fa3, true, 2130706433, 3);
            future1.wait();
            future2.wait();
        } else {
            ntt(fa1, false, 2013265921, 31);
            ntt(fb1, false, 2013265921, 31);
            for (size_t i = 0; i < n_pow2; ++i) fa1[i] = (1ULL * fa1[i] * fb1[i]) % 2013265921;
            ntt(fa1, true, 2013265921, 31);

            ntt(fa2, false, 2113929217, 5);
            ntt(fb2, false, 2113929217, 5);
            for (size_t i = 0; i < n_pow2; ++i) fa2[i] = (1ULL * fa2[i] * fb2[i]) % 2113929217;
            ntt(fa2, true, 2113929217, 5);

            ntt(fa3, false, 2130706433, 3);
            ntt(fb3, false, 2130706433, 3);
            for (size_t i = 0; i < n_pow2; ++i) fa3[i] = (1ULL * fa3[i] * fb3[i]) % 2130706433;
            ntt(fa3, true, 2130706433, 3);
        }

        constexpr uint64_t P1 = 2013265921;
        constexpr uint64_t P2 = 2113929217;
        constexpr uint64_t P3 = 2130706433;
        constexpr uint64_t P1P2 = P1 * P2;
        constexpr uint64_t invP1_modP2 = ct_inv_mod(P1, P2);
        constexpr uint64_t invP1P2_modP3 = ct_inv_mod(P1P2 % P3, P3);

        DecInt result;
        result.data.resize(n_pow2, 0);
        uint64_t carry = 0;

        for (size_t i = 0; i < n_pow2; ++i) {
            uint64_t a1 = fa1[i], a2 = fa2[i], a3 = fa3[i];
            uint64_t k1 = (a2 + P2 - a1) % P2 * invP1_modP2 % P2;
            uint64_t x12 = a1 + k1 * P1;
            uint64_t k2 = (a3 + P3 - x12 % P3) % P3 * invP1P2_modP3 % P3;

#if defined(__SIZEOF_INT128__)
            uint128_t term = (uint128_t)k2 * P1P2 + x12 + carry;
            uint64_t q = (uint64_t)(term / BASE);
            result.data[i] = static_cast<uint32_t>(term - (uint128_t)q * BASE);
            carry = q;
#else
            uint128_t term = mul64x64(k2, P1P2);
            term.low += x12;
            if (term.low < x12) term.high++;
            term.low += carry;
            if (term.low < carry) term.high++;
            
            uint64_t rem;
            carry = div128by64(term, BASE, &rem);
            result.data[i] = static_cast<uint32_t>(rem);
#endif
        }
        while (carry > 0) {
            result.data.push_back(static_cast<uint32_t>(carry % BASE));
            carry /= BASE;
        }
        result.trim();
        return result;
    }

    static DecInt karatsuba(const uint32_t* a, size_t n, const uint32_t* b, size_t m) {
        while (n > 1 && a[n - 1] == 0) n--;
        while (m > 1 && b[m - 1] == 0) m--;
        if (n == 0 || m == 0 || (n == 1 && a[0] == 0) || (m == 1 && b[0] == 0)) return DecInt(0);

        if (n < 96 || m < 96) return mul_basecase(a, n, b, m);

        size_t half = std::max(n, m) / 2;

        size_t a0_len = std::min(n, half);
        size_t a1_len = (n > half) ? n - half : 0;
        const uint32_t* a0 = a;
        const uint32_t* a1 = a + a0_len;

        size_t b0_len = std::min(m, half);
        size_t b1_len = (m > half) ? m - half : 0;
        const uint32_t* b0 = b;
        const uint32_t* b1 = b + b0_len;

        DecInt z0 = karatsuba(a0, a0_len, b0, b0_len);
        DecInt z2 = karatsuba(a1, a1_len, b1, b1_len);
        
        DecInt a_sum = add_abs_ptr(a0, a0_len, a1, a1_len);
        DecInt b_sum = add_abs_ptr(b0, b0_len, b1, b1_len);

        DecInt z1 = karatsuba(a_sum.data.data(), a_sum.data.size(), b_sum.data.data(), b_sum.data.size());
        z1 = sub_abs(sub_abs(z1, z2), z0);

        DecInt result = z0;
        if (!z1.isZero()) {
            DecInt z1_shifted = z1;
            z1_shifted.data.insert(z1_shifted.data.begin(), half, 0);
            result = add_abs(result, z1_shifted);
        }
        if (!z2.isZero()) {
            DecInt z2_shifted = z2;
            z2_shifted.data.insert(z2_shifted.data.begin(), 2 * half, 0);
            result = add_abs(result, z2_shifted);
        }

        result.trim();
        return result;
    }

    static DecInt toom3(const uint32_t* a, size_t n, const uint32_t* b, size_t m) {
        if (n < 128 || m < 128) return karatsuba(a, n, b, m);

        size_t k = (std::max(n, m) + 2) / 3;

        auto get_slice = [](const uint32_t* arr, size_t len, size_t start, size_t k_len) {
            if (start >= len) return DecInt(0);
            size_t slice_len = std::min(k_len, len - start);
            DecInt res;
            res.data.assign(arr + start, arr + start + slice_len);
            res.trim();
            return res;
        };

        DecInt a0 = get_slice(a, n, 0, k);
        DecInt a1 = get_slice(a, n, k, k);
        DecInt a2 = get_slice(a, n, 2 * k, k);

        DecInt b0 = get_slice(b, m, 0, k);
        DecInt b1 = get_slice(b, m, k, k);
        DecInt b2 = get_slice(b, m, 2 * k, k);

        DecInt v0 = a0;
        DecInt v1 = a0 + a1 + a2;
        DecInt vm1 = a0 - a1 + a2;
        DecInt v2 = a0 + a1.mul_small(2) + a2.mul_small(4);
        DecInt vinf = a2;

        DecInt u0 = b0;
        DecInt u1 = b0 + b1 + b2;
        DecInt um1 = b0 - b1 + b2;
        DecInt u2 = b0 + b1.mul_small(2) + b2.mul_small(4);
        DecInt uinf = b2;

        DecInt w0 = v0 * u0;
        DecInt w1 = v1 * u1;
        DecInt wm1 = vm1 * um1;
        DecInt w2 = v2 * u2;
        DecInt winf = vinf * uinf;

        uint32_t rem;
        DecInt t1 = (w1 + wm1).div_small(2, rem);
        DecInt t2 = (w1 - wm1).div_small(2, rem);

        DecInt r0 = w0;
        DecInt r4 = winf;
        DecInt r2 = t1 - w0 - winf;

        DecInt t3 = (w2 - r0 - r2.mul_small(4) - r4.mul_small(16)).div_small(2, rem);
        DecInt r3 = (t3 - t2).div_small(3, rem);
        DecInt r1 = t2 - r3;

        DecInt result = r0;
        if (!r1.isZero()) { DecInt tmp = r1; tmp.data.insert(tmp.data.begin(), k, 0); result = result + tmp; }
        if (!r2.isZero()) { DecInt tmp = r2; tmp.data.insert(tmp.data.begin(), 2 * k, 0); result = result + tmp; }
        if (!r3.isZero()) { DecInt tmp = r3; tmp.data.insert(tmp.data.begin(), 3 * k, 0); result = result + tmp; }
        if (!r4.isZero()) { DecInt tmp = r4; tmp.data.insert(tmp.data.begin(), 4 * k, 0); result = result + tmp; }

        result.trim();
        return result;
    }

    DecInt operator*(const DecInt& o) const {
        DecInt res;
        if (data.size() >= 1536 && o.data.size() >= 1536) {
            res = mul_ntt(data.data(), data.size(), o.data.data(), o.data.size());
        } else if (data.size() >= 128 && o.data.size() >= 128) {
            res = toom3(data.data(), data.size(), o.data.data(), o.data.size());
        } else {
            res = karatsuba(data.data(), data.size(), o.data.data(), o.data.size());
        }
        res.negative = (negative != o.negative);
        if (res.isZero()) res.negative = false;
        return res;
    }

    DecInt abs() const {
        DecInt res = *this;
        res.negative = false;
        return res;
    }

    int64_t digitCount() const {
        if (isZero()) return 0;
        int64_t count = (data.size() - 1) * 9;
        uint32_t top = data.back();
        while (top > 0) { count++; top /= 10; }
        return count;
    }

    uint32_t to_uint32() const {
        return data.empty() ? 0 : data[0];
    }

    DecInt mul_pow10(int64_t n) const {
        if (isZero() || n == 0) return *this;
        int64_t blocks = n / 9;
        int64_t rem = n % 9;
        DecInt res;
        res.negative = negative;
        res.data.assign(blocks + data.size(), 0);
        uint64_t carry = 0;
        uint32_t multiplier = 1;
        for(int i=0; i<rem; ++i) multiplier *= 10;
        
        for (size_t i = 0; i < data.size(); ++i) {
            uint64_t prod = static_cast<uint64_t>(data[i]) * multiplier + carry;
            uint64_t q = prod / 1000000000ULL;
            res.data[blocks + i] = static_cast<uint32_t>(prod - q * 1000000000ULL);
            carry = q;
        }
        if (carry > 0) res.data.push_back(static_cast<uint32_t>(carry));
        else res.trim();
        return res;
    }

    DecInt div_pow10(int64_t n) const {
        if (isZero() || n == 0) return *this;
        int64_t blocks = n / 9;
        int64_t rem = n % 9;
        if (blocks >= static_cast<int64_t>(data.size())) return DecInt(0);
        
        DecInt res;
        res.negative = negative;
        uint32_t divisor = 1;
        for(int i=0; i<rem; ++i) divisor *= 10;
        
        uint64_t rem_val = 0;
        res.data.resize(data.size() - blocks, 0);
        for (int i = static_cast<int>(data.size()) - 1; i >= blocks; --i) {
            uint64_t cur = rem_val * BASE + data[i];
            res.data[i - blocks] = static_cast<uint32_t>(cur / divisor);
            rem_val = cur % divisor;
        }
        res.trim();
        return res;
    }

    DecInt mod_pow10(int64_t n) const {
        if (isZero() || n == 0) return DecInt(0);
        int64_t blocks = n / 9;
        int64_t rem = n % 9;
        if (blocks >= static_cast<int64_t>(data.size())) return this->abs();
        
        DecInt res;
        res.negative = false;
        res.data.assign(data.begin(), data.begin() + blocks);
        
        if (rem > 0) {
            uint32_t mod_val = 1;
            for(int i=0; i<rem; ++i) mod_val *= 10;
            res.data.push_back(data[blocks] % mod_val);
        }
        res.trim();
        return res;
    }

    DecInt div_small(uint32_t v, uint32_t& rem_out) const {
        if (v == 0) throw std::runtime_error("Division by zero");
        DecInt res; res.data.resize(data.size(), 0);
        uint64_t r = 0;
        for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
            uint64_t cur = r * BASE + data[i];
            res.data[i] = static_cast<uint32_t>(cur / v);
            r = cur % v;
        }
        res.negative = negative;
        res.trim();
        rem_out = static_cast<uint32_t>(r);
        return res;
    }
};

class Decimal {
public:
    static inline int g_prec = 28;

    DecInt mantissa;
    int64_t exp;

    Decimal() : mantissa(0), exp(0) {}
    Decimal(DecInt m, int64_t e) : mantissa(std::move(m)), exp(e) {}

    static Decimal from_string(const std::string& s) {
        std::string m_str = "";
        int64_t e = 0;
        bool in_frac = false;
        int64_t frac_count = 0;
        
        size_t i = 0;
        while (i < s.length() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        
        if (i < s.length() && (s[i] == '+' || s[i] == '-')) {
            m_str += s[i++];
        }
        
        bool has_digits = false;
        for (; i < s.length(); ++i) {
            if (s[i] == '.') {
                if (in_frac) throw std::runtime_error("ValueError: Invalid decimal string (multiple decimal points).");
                in_frac = true;
            } else if (s[i] >= '0' && s[i] <= '9') {
                m_str += s[i];
                has_digits = true;
                if (in_frac) frac_count++;
            } else if (s[i] == 'e' || s[i] == 'E') {
                try {
                    e = std::stoll(s.substr(i + 1));
                } catch (...) {
                    throw std::runtime_error("ValueError: Invalid exponent in decimal string.");
                }
                break;
            } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
                size_t j = i;
                while (j < s.length() && std::isspace(static_cast<unsigned char>(s[j]))) j++;
                if (j == s.length()) break;
                throw std::runtime_error("ValueError: Invalid character in decimal string.");
            } else {
                throw std::runtime_error("ValueError: Invalid character in decimal string.");
            }
        }
        if (!has_digits) throw std::runtime_error("ValueError: No digits found in decimal string.");
        if (m_str.empty() || m_str == "+" || m_str == "-") m_str += "0";
        return Decimal(DecInt(m_str), e - frac_count);
    }

    std::string to_string() const {
        std::string m = mantissa.to_string();
        bool neg = false;
        if (m.length() > 0 && m[0] == '-') {
            neg = true;
            m = m.substr(1);
        }
        if (m == "0") return "0";
        
        std::string res;
        if (exp >= 0) {
            res = m + std::string(static_cast<size_t>(exp), '0');
        } else {
            int64_t pos_exp = -exp;
            if (pos_exp >= static_cast<int64_t>(m.length())) {
                res = "0." + std::string(static_cast<size_t>(pos_exp - m.length()), '0') + m;
            } else {
                res = m.substr(0, m.length() - pos_exp) + "." + m.substr(m.length() - pos_exp);
            }
        }
        
        if (res.find('.') != std::string::npos) {
            while (!res.empty() && res.back() == '0') res.pop_back();
            if (!res.empty() && res.back() == '.') res.pop_back();
        }
        
        if (neg && res != "0") res = "-" + res;
        return res;
    }

    static int guard_digits(int prec) {
        if (prec <= 0) return 8;
        return 8 + static_cast<int>(std::log2(prec));
    }

    Decimal truncate(int prec) const {
        if (mantissa.isZero()) return Decimal(DecInt(0), 0);
        int64_t current_digits = mantissa.digitCount();
        if (current_digits <= prec) return *this;
        int64_t drop = current_digits - prec;
        
        DecInt new_m = mantissa.div_pow10(drop);
        
        uint32_t next_digit = mantissa.abs().div_pow10(drop - 1).mod_pow10(1).to_uint32();
        bool exact_half = (next_digit == 5);
        if (exact_half) {
            DecInt rest = mantissa.abs().mod_pow10(drop - 1);
            if (!rest.isZero()) exact_half = false;
        }
        
        bool round_up = false;
        if (next_digit > 5) {
            round_up = true;
        } else if (next_digit == 5) {
            if (!exact_half) {
                round_up = true;
            } else {
                uint32_t last_digit = new_m.abs().mod_pow10(1).to_uint32();
                if (last_digit % 2 != 0) {
                    round_up = true;
                }
            }
        }
        
        if (round_up) {
            if (mantissa.isNegative()) {
                new_m = new_m - DecInt(1);
            } else {
                new_m = new_m + DecInt(1);
            }
        }
        
        return Decimal(new_m, exp + drop);
    }

    int64_t magnitude() const {
        if (mantissa.isZero()) return 0;
        return exp + mantissa.digitCount();
    }

    Decimal abs() const {
        return Decimal(mantissa.abs(), exp);
    }

    Decimal add(const Decimal& other) const {
        if (mantissa.isZero()) return other.truncate(g_prec);
        if (other.mantissa.isZero()) return this->truncate(g_prec);
        
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        int guard = guard_digits(g_prec);
        if (mag1 - mag2 > g_prec + guard) return this->truncate(g_prec);
        if (mag2 - mag1 > g_prec + guard) return other.truncate(g_prec);
        
        int64_t min_exp = std::min(exp, other.exp);
        DecInt m1 = mantissa;
        if (exp > min_exp) m1 = m1.mul_pow10(exp - min_exp);
        DecInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2.mul_pow10(other.exp - min_exp);
        return Decimal(m1 + m2, min_exp).truncate(g_prec);
    }

    Decimal sub(const Decimal& other) const {
        if (mantissa.isZero()) {
            return Decimal(-other.mantissa, other.exp).truncate(g_prec);
        }
        if (other.mantissa.isZero()) return this->truncate(g_prec);
        
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        int guard = guard_digits(g_prec);
        if (mag1 - mag2 > g_prec + guard) return this->truncate(g_prec);
        if (mag2 - mag1 > g_prec + guard) {
            return Decimal(-other.mantissa, other.exp).truncate(g_prec);
        }
        
        int64_t min_exp = std::min(exp, other.exp);
        DecInt m1 = mantissa;
        if (exp > min_exp) m1 = m1.mul_pow10(exp - min_exp);
        DecInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2.mul_pow10(other.exp - min_exp);
        return Decimal(m1 - m2, min_exp).truncate(g_prec);
    }

    Decimal mul(const Decimal& other) const {
        return Decimal(mantissa * other.mantissa, exp + other.exp).truncate(g_prec);
    }

    Decimal inverse() const {
        if (mantissa.isZero()) throw std::runtime_error("DivisionByZero: Decimal division by zero.");
        
        int64_t L = mantissa.digitCount();
        int64_t E = exp + L - 1;
        
        double M = 0.0;
        int sz = static_cast<int>(mantissa.data.size());
        int start = std::max(0, sz - 3);
        for (int i = sz - 1; i >= start; --i) {
            M = M * 1000000000.0 + mantissa.data[i];
        }
        
        double log10_M_exact = std::log10(M) + start * 9.0 - (L - 1);
        double M_exact = std::pow(10.0, log10_M_exact);
        
        double guess_val = 1.0 / M_exact;
        if (mantissa.isNegative()) guess_val = -guess_val;
        
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess_val);
        for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
        Decimal y = Decimal::from_string(buf);
        y.exp -= E;
        
        Decimal two(DecInt(2), 0);
        int target_prec = g_prec + guard_digits(g_prec);
        int current_prec = 16;
        int saved_prec = g_prec;
        
        while (current_prec < target_prec) {
            current_prec *= 2;
            if (current_prec > target_prec) current_prec = target_prec;
            
            g_prec = current_prec;
            Decimal cur_this = this->truncate(current_prec);
            Decimal xy = cur_this.mul(y);
            Decimal term = two.sub(xy);
            y = y.mul(term);
        }
        g_prec = saved_prec;
        return y;
    }

    Decimal div(const Decimal& other) const {
        if (other.mantissa.isZero()) {
            throw std::runtime_error("DivisionByZero: Decimal division by zero.");
        }
        if (mantissa.isZero()) return Decimal(DecInt(0), 0);
        
        if (other.mantissa.data.size() <= 1536) {
            int64_t len1 = mantissa.digitCount();
            int64_t len2 = other.mantissa.digitCount();
            int64_t extra_zeros = g_prec + guard_digits(g_prec) - len1 + len2;
            if (extra_zeros < 0) extra_zeros = 0;
            DecInt m1_shifted = mantissa.mul_pow10(extra_zeros);
            
            DecInt q = DecInt::divmod_knuth(m1_shifted, other.mantissa).first;
            q.negative = (mantissa.negative != other.mantissa.negative);
            if (q.isZero()) q.negative = false;
            return Decimal(q, exp - other.exp - extra_zeros).truncate(g_prec);
        }
        
        return this->mul(other.inverse()).truncate(g_prec);
    }
    
    bool eq(const Decimal& other) const {
        if (mantissa.isZero() && other.mantissa.isZero()) return true;
        if (mantissa.isZero() || other.mantissa.isZero()) return false;
        
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        if (std::abs(mag1 - mag2) > g_prec + guard_digits(g_prec)) return false;
        
        int64_t min_exp = std::min(exp, other.exp);
        DecInt m1 = mantissa;
        if (exp > min_exp) m1 = m1.mul_pow10(exp - min_exp);
        DecInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2.mul_pow10(other.exp - min_exp);
        return m1 == m2;
    }
    
    bool lt(const Decimal& other) const {
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        
        bool neg1 = mantissa.isNegative();
        bool neg2 = other.mantissa.isNegative();
        if (mantissa.isZero()) neg1 = false;
        if (other.mantissa.isZero()) neg2 = false;
        
        if (neg1 && !neg2) return true;
        if (!neg1 && neg2) return false;
        
        if (std::abs(mag1 - mag2) > g_prec + guard_digits(g_prec)) {
            if (neg1) return mag1 > mag2;
            return mag1 < mag2;
        }
        
        int64_t min_exp = std::min(exp, other.exp);
        DecInt m1 = mantissa;
        if (exp > min_exp) m1 = m1.mul_pow10(exp - min_exp);
        DecInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2.mul_pow10(other.exp - min_exp);
        return m1 < m2;
    }

    bool gt(const Decimal& other) const {
        return !this->lt(other) && !this->eq(other);
    }

    bool le(const Decimal& other) const {
        return this->lt(other) || this->eq(other);
    }

    bool ge(const Decimal& other) const {
        return this->gt(other) || this->eq(other);
    }

    bool neq(const Decimal& other) const {
        return !this->eq(other);
    }

    Decimal sqrt() const {
        if (mantissa.isZero()) return *this;
        if (mantissa.isNegative()) {
            throw std::runtime_error("MathError: sqrt of negative decimal.");
        }
        
        int64_t L = mantissa.digitCount();
        int64_t E = exp + L - 1;
        
        double M = 0.0;
        int sz = static_cast<int>(mantissa.data.size());
        int start = std::max(0, sz - 3);
        for (int i = sz - 1; i >= start; --i) {
            M = M * 1000000000.0 + mantissa.data[i];
        }
        
        double log10_M_exact = std::log10(M) + start * 9.0 - (L - 1);
        double M_exact = std::pow(10.0, log10_M_exact);
        
        int64_t k = E / 2;
        int64_t r = E % 2;
        if (r < 0) {
            r += 2;
            k -= 1;
        }
        
        double adjusted_M = M_exact * std::pow(10.0, r);
        double guess_val = 1.0 / std::sqrt(adjusted_M);
        
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess_val);
        for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
        Decimal y = Decimal::from_string(buf);
        y.exp -= k;
        
        Decimal three(DecInt(3), 0);
        Decimal half(DecInt(5), -1);
        
        int target_prec = g_prec + guard_digits(g_prec);
        int current_prec = 16;
        int saved_prec = g_prec;
        
        while (current_prec < target_prec) {
            current_prec *= 2;
            if (current_prec > target_prec) current_prec = target_prec;
            
            g_prec = current_prec;
            Decimal cur_this = this->truncate(current_prec);
            Decimal y2 = y.mul(y);
            Decimal xy2 = cur_this.mul(y2);
            Decimal term = three.sub(xy2);
            y = y.mul(term).mul(half);
        }
        
        g_prec = saved_prec;
        return this->mul(y).truncate(g_prec);
    }

    Decimal exp_val() const {
        if (mantissa.isZero()) return Decimal(DecInt(1), 0);

        int saved_prec = g_prec;
        int target_prec = saved_prec + guard_digits(saved_prec);
        
        double d;
        try {
            const auto& raw_data = mantissa.data;
            int sz = static_cast<int>(raw_data.size());
            double res = 0.0;
            int start = std::max(0, sz - 3);
            for (int i = sz - 1; i >= start; --i) {
                res = res * 1000000000.0 + raw_data[i];
            }
            if (mantissa.isNegative()) res = -res;
            d = res * std::pow(10.0, exp) * std::pow(1000000000.0, start);
        } catch (...) { d = mantissa.isNegative() ? -1e300 : 1e300; }

        double guess_val;
        int64_t guess_exp = 0;
        if (d > 600.0 || d < -600.0) {
            double log10_e = 0.43429448190325182765;
            double x_log10 = d * log10_e;
            guess_exp = static_cast<int64_t>(std::floor(x_log10));
            double rem = x_log10 - guess_exp;
            guess_val = std::pow(10.0, rem);
        } else {
            guess_val = std::exp(d);
        }

        if (!std::isfinite(guess_val)) {
            if (d > 0) throw std::runtime_error("Overflow: exp result too large.");
            return Decimal(DecInt(0), 0);
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess_val);
        for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
        Decimal y = Decimal::from_string(buf);
        y.exp += guess_exp;

        Decimal one(DecInt(1), 0);
        int current_prec = 16;
        
        while (current_prec < target_prec) {
            current_prec *= 2;
            if (current_prec > target_prec) current_prec = target_prec;
            
            g_prec = current_prec;
            Decimal cur_x = this->truncate(current_prec);
            Decimal ln_y = y.ln_val();
            Decimal term = cur_x.sub(ln_y).add(one);
            y = y.mul(term).truncate(current_prec);
        }
        
        g_prec = saved_prec;
        return y.truncate(g_prec);
    }

    Decimal mod_2pi() const {
        Decimal two_pi = Decimal::two_pi();
        Decimal q = this->div(two_pi);
        Decimal q_int;
        if (q.exp >= 0) {
            q_int = q;
        } else if (-q.exp >= q.mantissa.digitCount()) {
            q_int = Decimal(DecInt(0), 0);
        } else {
            q_int = Decimal(q.mantissa.div_pow10(-q.exp), 0);
        }
        return this->sub(q_int.mul(two_pi));
    }

    void sincos_val(Decimal& s_out, Decimal& c_out) const {
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        
        Decimal x = this->mod_2pi();
        int squares = 0;
        Decimal one(DecInt(1), 0);
        Decimal two(DecInt(2), 0);
        Decimal threshold(DecInt(1), -8);
        
        while (x.abs().gt(threshold)) {
            x = x.div(two).truncate(g_prec);
            squares++;
        }
        
        Decimal sum_s = x;
        Decimal term_s = x;
        Decimal sum_c = one;
        Decimal term_c = one;
        Decimal x2 = x.mul(x).truncate(g_prec);
        Decimal n_s(DecInt(2), 0);
        Decimal n_c(DecInt(1), 0);
        int sign = -1;
        
        for (int i = 1; i < 10000; ++i) {
            term_s = term_s.mul(x2).div(n_s.mul(n_s.add(one))).truncate(g_prec);
            term_c = term_c.mul(x2).div(n_c.mul(n_c.add(one))).truncate(g_prec);
            
            if (term_s.mantissa.isZero() && term_c.mantissa.isZero()) break;
            
            Decimal next_sum_s = (sign == -1) ? sum_s.sub(term_s).truncate(g_prec) : sum_s.add(term_s).truncate(g_prec);
            Decimal next_sum_c = (sign == -1) ? sum_c.sub(term_c).truncate(g_prec) : sum_c.add(term_c).truncate(g_prec);
            
            if (next_sum_s.eq(sum_s) && next_sum_c.eq(sum_c)) break;
            
            sum_s = next_sum_s;
            sum_c = next_sum_c;
            
            n_s = n_s.add(two);
            n_c = n_c.add(two);
            sign = -sign;
        }
        
        for (int i = 0; i < squares; ++i) {
            Decimal next_s = two.mul(sum_s).mul(sum_c).truncate(g_prec);
            Decimal next_c = sum_c.mul(sum_c).mul(two).sub(one).truncate(g_prec);
            sum_s = next_s;
            sum_c = next_c;
        }
        
        g_prec = saved_prec;
        s_out = sum_s.truncate(g_prec);
        c_out = sum_c.truncate(g_prec);
    }

    Decimal sin_val() const {
        Decimal s, c;
        sincos_val(s, c);
        return s;
    }

    Decimal cos_val() const {
        Decimal s, c;
        sincos_val(s, c);
        return c;
    }

    static Decimal pi() {
        static int cached_prec = -1;
        static Decimal cached_pi_val;
        
        if (g_prec <= cached_prec) {
            return cached_pi_val.truncate(g_prec);
        }

        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);

        // Chudnovsky 算法每项提供约 14.18 位十进制精度
        int64_t N = g_prec / 14 + 2;

        // 二分分裂法 (Binary Splitting) 纯整数树状合并
        struct BS {
            struct PQR { DecInt P, Q, R; };
            static PQR compute(int64_t a, int64_t b) {
                if (b - a == 1) {
                    if (a == 0) {
                        return {DecInt(1), DecInt(1), DecInt(13591409)};
                    } else {
                        int64_t p_val = -(6 * a - 5) * (2 * a - 1) * (6 * a - 1);
                        DecInt P = p_val < 0 ? -DecInt(static_cast<uint64_t>(-p_val)) : DecInt(static_cast<uint64_t>(p_val));
                        DecInt a_dec(static_cast<uint64_t>(a));
                        DecInt a3 = a_dec * a_dec * a_dec;
                        DecInt Q = DecInt(10939058860032000ULL) * a3;
                        DecInt R = P * (DecInt(545140134ULL) * a_dec + DecInt(13591409ULL));
                        return {P, Q, R};
                    }
                }
                int64_t m = (a + b) / 2;
                if (b - a > 2000) {
                    auto future_left = std::async(std::launch::async, compute, a, m);
                    PQR right = compute(m, b);
                    PQR left = future_left.get();
                    return {
                        left.P * right.P,
                        left.Q * right.Q,
                        left.R * right.Q + left.P * right.R
                    };
                } else {
                    PQR left = compute(a, m);
                    PQR right = compute(m, b);
                    return {
                        left.P * right.P,
                        left.Q * right.Q,
                        left.R * right.Q + left.P * right.R
                    };
                }
            }
        };

        BS::PQR res = BS::compute(0, N);

        // 仅在最后一步进行唯一的一次大数开方与除法
        Decimal sqrt_10005 = Decimal(DecInt(10005), 0).sqrt();
        DecInt num = res.Q * DecInt(426880);
        Decimal num_dec = Decimal(num, 0);
        Decimal den_dec = Decimal(res.R, 0);
        
        Decimal pi_val = num_dec.mul(sqrt_10005).div(den_dec).truncate(saved_prec);
        
        g_prec = saved_prec;
        cached_prec = g_prec;
        cached_pi_val = pi_val;
        
        return pi_val;
    }

    static Decimal ln10() {
        static int cached_prec = -1;
        static Decimal cached_ln10_val;
        
        if (g_prec <= cached_prec) {
            return cached_ln10_val.truncate(g_prec);
        }

        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);

        int64_t m = g_prec / 2 + 4;
        Decimal x = Decimal(DecInt(1), m);
        Decimal a(DecInt(1), 0);
        Decimal b = Decimal(DecInt(4), 0).div(x);
        Decimal half(DecInt(5), -1);

        while (true) {
            Decimal next_a = a.add(b).mul(half).truncate(g_prec);
            Decimal next_b = a.mul(b).sqrt().truncate(g_prec);
            if (a.eq(next_a)) {
                a = next_a;
                break;
            }
            a = next_a;
            b = next_b;
        }

        Decimal pi_val = Decimal::pi();
        Decimal m_dec(DecInt(m), 0);
        Decimal res = pi_val.div(a.mul(m_dec).mul(Decimal(DecInt(2), 0))).truncate(saved_prec);

        g_prec = saved_prec;
        cached_prec = g_prec;
        cached_ln10_val = res;

        return res;
    }

    static Decimal two_pi() {
        static int cached_prec = -1;
        static Decimal cached_val;
        if (g_prec <= cached_prec) return cached_val.truncate(g_prec);
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        Decimal res = Decimal::pi().mul(Decimal(DecInt(2), 0)).truncate(saved_prec);
        g_prec = saved_prec;
        cached_prec = g_prec;
        cached_val = res;
        return res;
    }

    static Decimal half_pi() {
        static int cached_prec = -1;
        static Decimal cached_val;
        if (g_prec <= cached_prec) return cached_val.truncate(g_prec);
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        Decimal res = Decimal::pi().div(Decimal(DecInt(2), 0)).truncate(saved_prec);
        g_prec = saved_prec;
        cached_prec = g_prec;
        cached_val = res;
        return res;
    }

    static Decimal ln2() {
        static int cached_prec = -1;
        static Decimal cached_val;
        if (g_prec <= cached_prec) return cached_val.truncate(g_prec);
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        Decimal res = Decimal(DecInt(2), 0).ln_val().truncate(saved_prec);
        g_prec = saved_prec;
        cached_prec = g_prec;
        cached_val = res;
        return res;
    }

    static Decimal sqrt2() {
        static int cached_prec = -1;
        static Decimal cached_val;
        if (g_prec <= cached_prec) return cached_val.truncate(g_prec);
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        Decimal res = Decimal(DecInt(2), 0).sqrt().truncate(saved_prec);
        g_prec = saved_prec;
        cached_prec = g_prec;
        cached_val = res;
        return res;
    }

    static Decimal e() {
        static int cached_prec = -1;
        static Decimal cached_val;
        if (g_prec <= cached_prec) return cached_val.truncate(g_prec);
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);

        // 估算需要的项数 N，使得 N! > 10^g_prec
        int64_t N = 1;
        double target_ln = g_prec * 2.302585092994046; // g_prec * ln(10)
        double current_ln = 0;
        while (current_ln < target_ln) {
            current_ln += std::log(N);
            N++;
        }

        // 二分分裂法计算 e = sum(1/k!)
        struct BS_E {
            struct PQ { DecInt P, Q; };
            static PQ compute(int64_t a, int64_t b) {
                if (b - a == 1) {
                    DecInt val(static_cast<uint64_t>(b));
                    return {val, val};
                }
                int64_t m = (a + b) / 2;
                if (b - a > 2000) {
                    auto future_left = std::async(std::launch::async, compute, a, m);
                    PQ right = compute(m, b);
                    PQ left = future_left.get();
                    return {
                        left.P * right.Q + right.P,
                        left.Q * right.Q
                    };
                } else {
                    PQ left = compute(a, m);
                    PQ right = compute(m, b);
                    return {
                        left.P * right.Q + right.P,
                        left.Q * right.Q
                    };
                }
            }
        };

        BS_E::PQ res = BS_E::compute(0, N);
        Decimal e_val = Decimal(res.P, 0).div(Decimal(res.Q, 0)).truncate(saved_prec);

        g_prec = saved_prec;
        cached_prec = g_prec;
        cached_val = e_val;
        return e_val;
    }

    Decimal ln_val() const {
        if (mantissa.isZero() || mantissa.isNegative()) {
            throw std::runtime_error("MathError: ln of non-positive decimal.");
        }
        if (this->eq(Decimal(DecInt(1), 0))) return Decimal(DecInt(0), 0);

        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);

        int64_t mag = magnitude();
        int64_t m = g_prec / 2 + 4 - mag;
        if (m < 0) m = 0;

        Decimal y = this->mul(Decimal(DecInt(1), m));
        Decimal a(DecInt(1), 0);
        Decimal b = Decimal(DecInt(4), 0).div(y);
        Decimal half(DecInt(5), -1);

        while (true) {
            Decimal next_a = a.add(b).mul(half).truncate(g_prec);
            Decimal next_b = a.mul(b).sqrt().truncate(g_prec);
            if (a.eq(next_a)) {
                a = next_a;
                break;
            }
            a = next_a;
            b = next_b;
        }

        Decimal pi_val = Decimal::pi();
        Decimal ln_y = pi_val.div(a.mul(Decimal(DecInt(2), 0))).truncate(g_prec);

        Decimal res;
        if (m == 0) {
            res = ln_y;
        } else {
            Decimal m_dec(DecInt(m), 0);
            res = ln_y.sub(m_dec.mul(Decimal::ln10()));
        }

        g_prec = saved_prec;
        return res.truncate(g_prec);
    }

    Decimal log10_val() const {
        return this->ln_val().div(Decimal::ln10()).truncate(g_prec);
    }

    Decimal tan_val() const {
        return this->sin_val().div(this->cos_val()).truncate(g_prec);
    }

    Decimal atan_val() const {
        double d;
        try {
            const auto& raw_data = mantissa.data;
            int sz = static_cast<int>(raw_data.size());
            double res = 0.0;
            int start = std::max(0, sz - 3);
            for (int i = sz - 1; i >= start; --i) {
                res = res * 1000000000.0 + raw_data[i];
            }
            if (mantissa.isNegative()) res = -res;
            d = res * std::pow(10.0, exp) * std::pow(1000000000.0, start);
        } catch (...) { d = mantissa.isNegative() ? -1e300 : 1e300; }
        double guess = std::atan(d);
        if (!std::isfinite(guess)) guess = mantissa.isNegative() ? -1.5707963267948966 : 1.5707963267948966;
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess);
        for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
        Decimal y = Decimal::from_string(buf);
        
        int target_prec = g_prec + guard_digits(g_prec);
        int current_prec = 16;
        int saved_prec = g_prec;
        
        while (current_prec < target_prec) {
            current_prec *= 2;
            if (current_prec > target_prec) current_prec = target_prec;
            
            g_prec = current_prec;
            Decimal cur_x = this->truncate(current_prec);
            Decimal sy, cy;
            y.sincos_val(sy, cy);
            if (cy.mantissa.isZero()) break;
            Decimal ty = sy.div(cy).truncate(current_prec);
            Decimal diff = cy.mul(cy).mul(cur_x.sub(ty)).truncate(current_prec);
            y = y.add(diff).truncate(current_prec);
        }
        g_prec = saved_prec;
        return y.truncate(g_prec);
    }

    Decimal asin_val() const {
        Decimal one(DecInt(1), 0);
        if (this->abs().lt(one) == false && !this->abs().eq(one)) {
            throw std::runtime_error("MathError: asin domain error.");
        }
        double d;
        try {
            const auto& raw_data = mantissa.data;
            int sz = static_cast<int>(raw_data.size());
            double res = 0.0;
            int start = std::max(0, sz - 3);
            for (int i = sz - 1; i >= start; --i) {
                res = res * 1000000000.0 + raw_data[i];
            }
            if (mantissa.isNegative()) res = -res;
            d = res * std::pow(10.0, exp) * std::pow(1000000000.0, start);
        } catch (...) { d = mantissa.isNegative() ? -1.0 : 1.0; }
        double guess = std::asin(d);
        if (!std::isfinite(guess)) guess = mantissa.isNegative() ? -1.5707963267948966 : 1.5707963267948966;
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess);
        for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
        Decimal y = Decimal::from_string(buf);
        
        int target_prec = g_prec + guard_digits(g_prec);
        int current_prec = 16;
        int saved_prec = g_prec;
        
        while (current_prec < target_prec) {
            current_prec *= 2;
            if (current_prec > target_prec) current_prec = target_prec;
            
            g_prec = current_prec;
            Decimal cur_x = this->truncate(current_prec);
            Decimal sy, cy;
            y.sincos_val(sy, cy);
            if (cy.mantissa.isZero()) break;
            Decimal diff = cur_x.sub(sy).div(cy).truncate(current_prec);
            y = y.add(diff).truncate(current_prec);
        }
        g_prec = saved_prec;
        return y.truncate(g_prec);
    }

    Decimal acos_val() const {
        Decimal one(DecInt(1), 0);
        if (this->abs().lt(one) == false && !this->abs().eq(one)) {
            throw std::runtime_error("MathError: acos domain error.");
        }
        double d;
        try {
            const auto& raw_data = mantissa.data;
            int sz = static_cast<int>(raw_data.size());
            double res = 0.0;
            int start = std::max(0, sz - 3);
            for (int i = sz - 1; i >= start; --i) {
                res = res * 1000000000.0 + raw_data[i];
            }
            if (mantissa.isNegative()) res = -res;
            d = res * std::pow(10.0, exp) * std::pow(1000000000.0, start);
        } catch (...) { d = mantissa.isNegative() ? -1.0 : 1.0; }
        double guess = std::acos(d);
        if (!std::isfinite(guess)) guess = mantissa.isNegative() ? 3.1415926535897932 : 0.0;
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess);
        for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
        Decimal y = Decimal::from_string(buf);
        
        int target_prec = g_prec + guard_digits(g_prec);
        int current_prec = 16;
        int saved_prec = g_prec;
        
        while (current_prec < target_prec) {
            current_prec *= 2;
            if (current_prec > target_prec) current_prec = target_prec;
            
            g_prec = current_prec;
            Decimal cur_x = this->truncate(current_prec);
            Decimal sy, cy;
            y.sincos_val(sy, cy);
            if (sy.mantissa.isZero()) break;
            Decimal diff = cur_x.sub(cy).div(sy).truncate(current_prec);
            y = y.sub(diff).truncate(current_prec);
        }
        g_prec = saved_prec;
        return y.truncate(g_prec);
    }

    Decimal sinh_val() const {
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        Decimal ex = this->exp_val();
        Decimal emx = Decimal(DecInt(1), 0).div(ex).truncate(g_prec);
        Decimal res = ex.sub(emx).div(Decimal(DecInt(2), 0));
        g_prec = saved_prec;
        return res.truncate(g_prec);
    }

    Decimal cosh_val() const {
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        Decimal ex = this->exp_val();
        Decimal emx = Decimal(DecInt(1), 0).div(ex).truncate(g_prec);
        Decimal res = ex.add(emx).div(Decimal(DecInt(2), 0));
        g_prec = saved_prec;
        return res.truncate(g_prec);
    }

    Decimal tanh_val() const {
        int saved_prec = g_prec;
        g_prec = saved_prec + guard_digits(saved_prec);
        Decimal ex = this->exp_val();
        Decimal emx = Decimal(DecInt(1), 0).div(ex).truncate(g_prec);
        Decimal num = ex.sub(emx);
        Decimal den = ex.add(emx);
        if (den.mantissa.isZero()) {
            g_prec = saved_prec;
            return *this;
        }
        Decimal res = num.div(den);
        g_prec = saved_prec;
        return res.truncate(g_prec);
    }
};

} // namespace jc

#endif // JC2_DECIMAL_H
