#include "P256.h"

#include <cstring>

namespace jc2::crypto {

namespace {

// NIST P-256 常量（FIPS 186-4 / SEC 2）
const BigNum& fieldP() {
    static const BigNum p = [] {
        BigNum v;
        BigNum::fromHex("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", v);
        return v;
    }();
    return p;
}

const BigNum& orderN() {
    static const BigNum n = [] {
        BigNum v;
        BigNum::fromHex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", v);
        return v;
    }();
    return n;
}

// a = -3 mod p
const BigNum& curveA() {
    static const BigNum a = [] { return BigNum::sub(fieldP(), BigNum::fromUint64(3)); }();
    return a;
}

const BigNum& curveB() {
    static const BigNum b = [] {
        BigNum v;
        BigNum::fromHex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b", v);
        return v;
    }();
    return b;
}

inline BigNum feMod(const BigNum& v) {
    return BigNum::mod(v, fieldP());
}

inline BigNum feInv(const BigNum& v) {
    return BigNum::inverseModPrime(v, fieldP());   // p 是素数，用费马小定理
}

// 生成元 G
const P256Point& generator() {
    static const P256Point g = [] {
        P256Point p;
        BigNum::fromHex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296", p.x);
        BigNum::fromHex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5", p.y);
        p.infinity = false;
        return p;
    }();
    return g;
}

P256Point pointDouble(const P256Point& p) {
    P256Point r;
    if (p.infinity) return r;
    if (p.y.isZero()) return r;   // 切线垂直 → 无穷远

    const BigNum pField = fieldP();
    const BigNum xx = BigNum::mulMod(p.x, p.x, pField);
    const BigNum threeXx = BigNum::mulMod(BigNum::fromUint64(3), xx, pField);
    const BigNum numerator = feMod(BigNum::addMod(threeXx, curveA(), pField));
    const BigNum denominator = feMod(BigNum::addMod(p.y, p.y, pField));
    const BigNum lambda = feMod(BigNum::mulMod(numerator, feInv(denominator), pField));

    r.x = feMod(BigNum::subMod(BigNum::mulMod(lambda, lambda, pField), BigNum::addMod(p.x, p.x, pField), pField));
    r.y = feMod(BigNum::subMod(BigNum::mulMod(lambda, BigNum::subMod(p.x, r.x, pField), pField), p.y, pField));
    r.infinity = false;
    return r;
}

P256Point pointAdd(const P256Point& a, const P256Point& b) {
    if (a.infinity) return b;
    if (b.infinity) return a;
    const BigNum pField = fieldP();
    if (a.x == b.x) {
        if (feMod(BigNum::addMod(a.y, b.y, pField)).isZero()) {
            return P256Point{};   // 互为逆元 → 无穷远
        }
        return pointDouble(a);
    }
    const BigNum numerator = feMod(BigNum::subMod(b.y, a.y, pField));
    const BigNum denominator = feMod(BigNum::subMod(b.x, a.x, pField));
    const BigNum lambda = feMod(BigNum::mulMod(numerator, feInv(denominator), pField));

    P256Point r;
    const BigNum lambdaSq = BigNum::mulMod(lambda, lambda, pField);
    r.x = feMod(BigNum::subMod(BigNum::subMod(lambdaSq, a.x, pField), b.x, pField));
    r.y = feMod(BigNum::subMod(BigNum::mulMod(lambda, BigNum::subMod(a.x, r.x, pField), pField), a.y, pField));
    r.infinity = false;
    return r;
}

} // namespace

bool p256DecodePoint(const uint8_t* data, size_t len, P256Point& out) {
    if (len != 65 || data[0] != 0x04) return false;
    out.x = BigNum::fromBytes(data + 1, 32);
    out.y = BigNum::fromBytes(data + 33, 32);
    out.infinity = false;
    return p256IsOnCurve(out);
}

bool p256IsOnCurve(const P256Point& p) {
    if (p.infinity) return false;
    const BigNum pField = fieldP();
    if (p.x >= pField || p.y >= pField) return false;
    const BigNum lhs = feMod(BigNum::mulMod(p.y, p.y, pField));
    const BigNum x3 = BigNum::mulMod(BigNum::mulMod(p.x, p.x, pField), p.x, pField);
    const BigNum rhs = feMod(BigNum::addMod(
        BigNum::addMod(x3, BigNum::mulMod(curveA(), p.x, pField), pField), curveB(), pField));
    return lhs == rhs;
}

P256Point p256ScalarMul(const Bytes& k, const P256Point& p) {
    P256Point result;   // 无穷远
    P256Point base = p;
    for (size_t i = 0; i < k.size(); ++i) {
        const uint8_t byte = k[k.size() - 1 - i];   // 从最低字节开始
        for (int bit = 0; bit < 8; ++bit) {
            if (((byte >> bit) & 1u) != 0) {
                result = pointAdd(result, base);
            }
            base = pointDouble(base);
        }
    }
    return result;
}

bool p256BasePointMul(const Bytes& privateKey, uint8_t out[65]) {
    const P256Point p = p256ScalarMul(privateKey, generator());
    if (p.infinity) return false;
    out[0] = 0x04;
    if (!p.x.toBytes(out + 1, 32)) return false;
    if (!p.y.toBytes(out + 33, 32)) return false;
    return true;
}

bool p256Ecdh(const Bytes& privateKey, const uint8_t* peerPoint, size_t peerLen, uint8_t out[32]) {    P256Point peer;
    if (!p256DecodePoint(peerPoint, peerLen, peer)) return false;
    const P256Point shared = p256ScalarMul(privateKey, peer);
    if (shared.infinity) return false;
    return shared.x.toBytes(out, 32);
}

bool p256EcdsaVerify(const P256Point& pub, const uint8_t hash32[32], const Bytes& r, const Bytes& s) {
    const BigNum n = orderN();
    const BigNum rv = BigNum::fromBytes(r.data(), r.size());
    const BigNum sv = BigNum::fromBytes(s.data(), s.size());
    if (rv.isZero() || sv.isZero()) return false;
    if (rv >= n || sv >= n) return false;
    if (pub.infinity || !p256IsOnCurve(pub)) return false;

    const BigNum e = BigNum::mod(BigNum::fromBytes(hash32, 32), n);
    const BigNum w = BigNum::inverseModPrime(sv, n);
    const BigNum u1 = BigNum::mulMod(e, w, n);
    const BigNum u2 = BigNum::mulMod(rv, w, n);

    uint8_t u1Bytes[32];
    uint8_t u2Bytes[32];
    if (!u1.toBytes(u1Bytes, 32) || !u2.toBytes(u2Bytes, 32)) return false;

    const P256Point p1 = p256ScalarMul(Bytes(u1Bytes, u1Bytes + 32), generator());
    const P256Point p2 = p256ScalarMul(Bytes(u2Bytes, u2Bytes + 32), pub);
    const P256Point sum = pointAdd(p1, p2);
    if (sum.infinity) return false;

    const BigNum x = BigNum::mod(sum.x, n);
    return x == rv;
}

} // namespace jc2::crypto
