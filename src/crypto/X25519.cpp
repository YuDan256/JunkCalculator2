#include "X25519.h"
#include "ConstTime.h"

#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace jc2::crypto {

namespace {

// 域元素：h = h0 + h1·2^51 + h2·2^102 + h3·2^153 + h4·2^204
using fe = uint64_t[5];

constexpr uint64_t kMask51 = (1ULL << 51) - 1;

// 128 位中间量（MSVC 没有 __int128，用 _umul128）。
struct U128 {
    uint64_t lo;
    uint64_t hi;
};

inline U128 mulU(uint64_t a, uint64_t b) {
    U128 r;
#if defined(_MSC_VER)
    r.lo = _umul128(a, b, &r.hi);
#else
    unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    r.lo = static_cast<uint64_t>(p);
    r.hi = static_cast<uint64_t>(p >> 64);
#endif
    return r;
}

inline void addU(U128& acc, U128 v) {
    acc.hi += v.hi;
    acc.lo += v.lo;
    if (acc.lo < v.lo) acc.hi++;
}

// 128 位右移 51 位（用于把溢出的高位折回下一肢体）。
inline uint64_t shr51(const U128& v) {
    return (v.lo >> 51) | (v.hi << 13);
}

inline void fe_0(fe h) { h[0] = h[1] = h[2] = h[3] = h[4] = 0; }
inline void fe_1(fe h) { h[0] = 1; h[1] = h[2] = h[3] = h[4] = 0; }
inline void fe_copy(fe h, const fe f) { for (int i = 0; i < 5; ++i) h[i] = f[i]; }

inline void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 5; ++i) h[i] = f[i] + g[i];
}

// h = f - g，加 2p 避免借位：2p 的 51 位肢为 (2^52-38, 2^52-2, ...)
inline void fe_sub(fe h, const fe f, const fe g) {
    constexpr uint64_t c0 = 0xFFFFFFFFFFFDAULL;   // 2^52 - 38
    constexpr uint64_t c1 = 0xFFFFFFFFFFFFEULL;   // 2^52 - 2
    h[0] = f[0] + c0 - g[0];
    h[1] = f[1] + c1 - g[1];
    h[2] = f[2] + c1 - g[2];
    h[3] = f[3] + c1 - g[3];
    h[4] = f[4] + c1 - g[4];
}

void fe_mul(fe h, const fe f, const fe g) {
    const uint64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    const uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    const uint64_t g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4;

    U128 h0 = mulU(f0, g0);
    U128 h1 = mulU(f0, g1);
    U128 h2 = mulU(f0, g2);
    U128 h3 = mulU(f0, g3);
    U128 h4 = mulU(f0, g4);
    addU(h0, mulU(f1, g4_19)); addU(h1, mulU(f1, g0));    addU(h2, mulU(f1, g1));
    addU(h3, mulU(f1, g2));    addU(h4, mulU(f1, g3));
    addU(h0, mulU(f2, g3_19)); addU(h1, mulU(f2, g4_19)); addU(h2, mulU(f2, g0));
    addU(h3, mulU(f2, g1));    addU(h4, mulU(f2, g2));
    addU(h0, mulU(f3, g2_19)); addU(h1, mulU(f3, g3_19)); addU(h2, mulU(f3, g4_19));
    addU(h3, mulU(f3, g0));    addU(h4, mulU(f3, g1));
    addU(h0, mulU(f4, g1_19)); addU(h1, mulU(f4, g2_19)); addU(h2, mulU(f4, g3_19));
    addU(h3, mulU(f4, g4_19)); addU(h4, mulU(f4, g0));

    uint64_t r0 = h0.lo & kMask51;
    uint64_t c = shr51(h0);
    h1.lo += c; if (h1.lo < c) h1.hi++;
    uint64_t r1 = h1.lo & kMask51;
    c = shr51(h1);
    h2.lo += c; if (h2.lo < c) h2.hi++;
    uint64_t r2 = h2.lo & kMask51;
    c = shr51(h2);
    h3.lo += c; if (h3.lo < c) h3.hi++;
    uint64_t r3 = h3.lo & kMask51;
    c = shr51(h3);
    h4.lo += c; if (h4.lo < c) h4.hi++;
    uint64_t r4 = h4.lo & kMask51;
    c = shr51(h4);

    r0 += c * 19;
    c = r0 >> 51; r0 &= kMask51;
    r1 += c;      c = r1 >> 51; r1 &= kMask51;
    r2 += c;      c = r2 >> 51; r2 &= kMask51;
    r3 += c;      c = r3 >> 51; r3 &= kMask51;
    r4 += c;      c = r4 >> 51; r4 &= kMask51;
    r0 += c * 19;

    h[0] = r0; h[1] = r1; h[2] = r2; h[3] = r3; h[4] = r4;
}

inline void fe_sq(fe h, const fe f) {
    fe_mul(h, f, f);
}

void fe_mul121666(fe h, const fe f) {
    uint64_t c;
    U128 t = mulU(f[0], 121666);
    uint64_t r0 = t.lo & kMask51; c = shr51(t);
    t = mulU(f[1], 121666); t.lo += c; if (t.lo < c) t.hi++;
    uint64_t r1 = t.lo & kMask51; c = shr51(t);
    t = mulU(f[2], 121666); t.lo += c; if (t.lo < c) t.hi++;
    uint64_t r2 = t.lo & kMask51; c = shr51(t);
    t = mulU(f[3], 121666); t.lo += c; if (t.lo < c) t.hi++;
    uint64_t r3 = t.lo & kMask51; c = shr51(t);
    t = mulU(f[4], 121666); t.lo += c; if (t.lo < c) t.hi++;
    uint64_t r4 = t.lo & kMask51; c = shr51(t);

    r0 += c * 19;
    c = r0 >> 51; r0 &= kMask51;
    r1 += c;      c = r1 >> 51; r1 &= kMask51;
    r2 += c;      c = r2 >> 51; r2 &= kMask51;
    r3 += c;      c = r3 >> 51; r3 &= kMask51;
    r4 += c;      c = r4 >> 51; r4 &= kMask51;
    r0 += c * 19;

    h[0] = r0; h[1] = r1; h[2] = r2; h[3] = r3; h[4] = r4;
}

// 解码 32 字节小端（忽略最高位，RFC 7748 规定 X25519 的 u 坐标最高位被屏蔽）。
void fe_frombytes(fe h, const uint8_t s[32]) {
    uint64_t w[4];
    for (int i = 0; i < 4; ++i) {
        uint64_t v = 0;
        for (int j = 7; j >= 0; --j) {
            v = (v << 8) | s[8 * i + j];
        }
        w[i] = v;
    }
    h[0] = w[0] & kMask51;
    h[1] = ((w[0] >> 51) | (w[1] << 13)) & kMask51;
    h[2] = ((w[1] >> 38) | (w[2] << 26)) & kMask51;
    h[3] = ((w[2] >> 25) | (w[3] << 39)) & kMask51;
    h[4] = (w[3] >> 12) & kMask51;
}

// 编码 32 字节小端；先做一次完整规约（减去 p 若 ≥ p）。
void fe_tobytes(uint8_t s[32], const fe in) {
    uint64_t t[5];
    fe_copy(t, in);

    uint64_t c;
    c = t[0] >> 51; t[0] &= kMask51; t[1] += c;
    c = t[1] >> 51; t[1] &= kMask51; t[2] += c;
    c = t[2] >> 51; t[2] &= kMask51; t[3] += c;
    c = t[3] >> 51; t[3] &= kMask51; t[4] += c;
    c = t[4] >> 51; t[4] &= kMask51; t[0] += c * 19;
    c = t[0] >> 51; t[0] &= kMask51; t[1] += c;

    // q = (t + 19) >> 255，即判断 t ≥ p
    uint64_t q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51;

    t[0] += 19 * q;
    c = t[0] >> 51; t[0] &= kMask51; t[1] += c;
    c = t[1] >> 51; t[1] &= kMask51; t[2] += c;
    c = t[2] >> 51; t[2] &= kMask51; t[3] += c;
    c = t[3] >> 51; t[3] &= kMask51; t[4] += c;
    t[4] &= kMask51;   // 丢掉 2^255 位

    // 打包成小端字节
    const uint64_t w0 = t[0] | (t[1] << 51);
    const uint64_t w1 = (t[1] >> 13) | (t[2] << 38);
    const uint64_t w2 = (t[2] >> 26) | (t[3] << 25);
    const uint64_t w3 = (t[3] >> 39) | (t[4] << 12);
    const uint64_t w[4] = {w0, w1, w2, w3};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j) {
            s[8 * i + j] = static_cast<uint8_t>(w[i] >> (8 * j));
        }
    }
    ctZero(t, sizeof(t));
}

// 求逆：z^(p-2)，p-2 = 2^255 - 21（二进制里只有第 4、2 位是 0）。
void fe_invert(fe out, const fe z) {
    fe t;
    fe_copy(t, z);
    for (int i = 253; i >= 0; --i) {
        fe_sq(t, t);
        if (i != 4 && i != 2) {
            fe_mul(t, t, z);
        }
    }
    fe_copy(out, t);
    ctZero(t, sizeof(t));
}

// 常数时间条件交换：b ∈ {0,1}
inline void fe_cswap(fe f, fe g, uint64_t b) {
    const uint64_t mask = 0 - b;
    for (int i = 0; i < 5; ++i) {
        const uint64_t x = (f[i] ^ g[i]) & mask;
        f[i] ^= x;
        g[i] ^= x;
    }
}

} // namespace

void x25519Raw(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t k[32];
    std::memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    fe x1, x2, z2, x3, z3;
    fe_frombytes(x1, point);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);

    uint64_t swap = 0;
    for (int t = 254; t >= 0; --t) {
        const uint64_t kt = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        fe a, b, aa, bb, e, c, d, da, cb, tmp;
        fe_add(a, x2, z2);
        fe_sub(b, x2, z2);
        fe_sq(aa, a);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(tmp, da, cb);
        fe_sq(x3, tmp);
        fe_sub(tmp, da, cb);
        fe_sq(tmp, tmp);
        fe_mul(z3, x1, tmp);
        fe_mul(x2, aa, bb);
        // RFC 7748 写的是 AA + a24·E（a24 = 121665）；本实现用等价形式 BB + 121666·E，
        // 与下面 fe_mul121666 的名字一致（两者恒等：AA + 121665·E = BB + 121666·E）。
        fe_mul121666(tmp, e);
        fe_add(tmp, bb, tmp);
        fe_mul(z2, e, tmp);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);

    ctZero(k, sizeof(k));
}

bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    x25519Raw(out, scalar, point);
    static const uint8_t zeros[32] = {0};
    return !ctEqual(out, zeros, 32);
}

} // namespace jc2::crypto
