#include "Curve.h"

#include "Der.h"

#include <cstring>

namespace jc2::crypto {

namespace {

BigNum hexConst(const char* hex) {
    BigNum v;
    if (!BigNum::fromHex(hex, v)) {
        v = BigNum();   // 常量写错时会在"曲线不成立"的自检里暴露
    }
    return v;
}

// 定宽大端字节（标量乘法用）
Bytes scalarBytes(const BigNum& v, size_t len) {
    Bytes out(len, 0);
    if (!v.toBytes(out.data(), len)) {
        out.clear();
    }
    return out;
}

struct GroupTable {
    EcGroup groups[3];
};

GroupTable& table() {
    static GroupTable t = [] {
        GroupTable out;
        {
            EcGroup& g = out.groups[0];
            g.id = EcCurve::P256;
            g.name = "P-256";
            g.oid = "1.2.840.10045.3.1.7";
            g.p = hexConst("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
            g.b = hexConst("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b");
            g.n = hexConst("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
            g.gx = hexConst("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
            g.gy = hexConst("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
            g.fieldBytes = 32;
            g.bits = 256;
        }
        {
            EcGroup& g = out.groups[1];
            g.id = EcCurve::P384;
            g.name = "P-384";
            g.oid = "1.3.132.0.34";
            g.p = hexConst("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe"
                           "ffffffff0000000000000000ffffffff");
            g.b = hexConst("b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875a"
                           "c656398d8a2ed19d2a85c8edd3ec2aef");
            g.n = hexConst("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf"
                           "581a0db248b0a77aecec196accc52973");
            g.gx = hexConst("aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a38"
                            "5502f25dbf55296c3a545e3872760ab7");
            g.gy = hexConst("3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c0"
                            "0a60b1ce1d7e819d7a431d7c90ea0e5f");
            g.fieldBytes = 48;
            g.bits = 384;
        }
        {
            EcGroup& g = out.groups[2];
            g.id = EcCurve::P521;
            g.name = "P-521";
            g.oid = "1.3.132.0.35";
            g.p = hexConst("01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                           "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                           "ffff");
            g.b = hexConst("0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef1"
                           "09e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00");
            g.n = hexConst("01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                           "fffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409");
            g.gx = hexConst("00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d"
                            "3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66");
            g.gy = hexConst("011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e"
                            "662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650");
            g.fieldBytes = 66;
            g.bits = 521;
        }
        for (EcGroup& g : out.groups) {
            g.a = BigNum::sub(g.p, BigNum::fromUint64(3));   // a = -3（模 p 后即 p-3）
        }
        return out;
    }();
    return t;
}

// ─── Jacobian 点运算（a = -3）────────────────────────────────

struct Jac {
    BigNum X, Y, Z;
    bool inf = true;
};

BigNum mulSmall(const BigNum& m, uint64_t k, const BigNum& p) {
    switch (k) {
        case 3: return BigNum::addMod(BigNum::addMod(m, m, p), m, p);
        case 4: return BigNum::addMod(BigNum::addMod(m, m, p), BigNum::addMod(m, m, p), p);
        case 8: {
            const BigNum d = BigNum::addMod(BigNum::addMod(m, m, p), BigNum::addMod(m, m, p), p);
            return BigNum::addMod(d, d, p);
        }
        default: break;
    }
    BigNum acc = BigNum::fromUint64(0);
    for (uint64_t i = 0; i < k; ++i) acc = BigNum::addMod(acc, m, p);
    return acc;
}

void jacDouble(const EcGroup& g, Jac& r, const Jac& h) {
    if (h.inf || h.Y.isZero()) {
        r.inf = true;
        return;
    }
    const BigNum Z2 = BigNum::mulMod(h.Z, h.Z, g.p);
    const BigNum t = BigNum::addMod(h.X, Z2, g.p);                        // X + Z²
    const BigNum u = BigNum::subMod(h.X, Z2, g.p);                        // X - Z²
    const BigNum M = mulSmall(BigNum::mulMod(t, u, g.p), 3, g.p);         // 3(X²-Z⁴) = 3X² + aZ⁴
    const BigNum Y2 = BigNum::mulMod(h.Y, h.Y, g.p);
    const BigNum S = mulSmall(BigNum::mulMod(h.X, Y2, g.p), 4, g.p);
    const BigNum X3 = BigNum::subMod(BigNum::mulMod(M, M, g.p), BigNum::addMod(S, S, g.p), g.p);
    const BigNum Y4 = BigNum::mulMod(Y2, Y2, g.p);
    const BigNum Y3 = BigNum::subMod(BigNum::mulMod(M, BigNum::subMod(S, X3, g.p), g.p), mulSmall(Y4, 8, g.p), g.p);
    const BigNum Z3 = BigNum::mulMod(BigNum::addMod(h.Y, h.Y, g.p), h.Z, g.p);

    r.inf = false;
    r.X = X3;
    r.Y = Y3;
    r.Z = Z3;
}

void jacAdd(const EcGroup& g, Jac& r, const Jac& p, const Jac& q) {
    if (p.inf) {
        r = q;
        return;
    }
    if (q.inf) {
        r = p;
        return;
    }
    const BigNum Z1Z1 = BigNum::mulMod(p.Z, p.Z, g.p);
    const BigNum Z2Z2 = BigNum::mulMod(q.Z, q.Z, g.p);
    const BigNum U1 = BigNum::mulMod(p.X, Z2Z2, g.p);
    const BigNum U2 = BigNum::mulMod(q.X, Z1Z1, g.p);
    const BigNum S1 = BigNum::mulMod(p.Y, BigNum::mulMod(q.Z, Z2Z2, g.p), g.p);
    const BigNum S2 = BigNum::mulMod(q.Y, BigNum::mulMod(p.Z, Z1Z1, g.p), g.p);
    const BigNum H = BigNum::subMod(U2, U1, g.p);
    const BigNum R = BigNum::subMod(S2, S1, g.p);

    if (H.isZero()) {
        if (R.isZero()) {
            jacDouble(g, r, p);   // P == Q
        } else {
            r.inf = true;         // P == -Q
        }
        return;
    }

    const BigNum HH = BigNum::mulMod(H, H, g.p);
    const BigNum HHH = BigNum::mulMod(H, HH, g.p);
    const BigNum V = BigNum::mulMod(U1, HH, g.p);
    const BigNum X3 = BigNum::subMod(BigNum::subMod(BigNum::mulMod(R, R, g.p), HHH, g.p), BigNum::addMod(V, V, g.p), g.p);
    const BigNum Y3 = BigNum::subMod(BigNum::mulMod(R, BigNum::subMod(V, X3, g.p), g.p), BigNum::mulMod(S1, HHH, g.p), g.p);
    const BigNum Z3 = BigNum::mulMod(BigNum::mulMod(p.Z, q.Z, g.p), H, g.p);

    r.inf = false;
    r.X = X3;
    r.Y = Y3;
    r.Z = Z3;
}

Jac toJac(const EcPoint& p) {
    Jac j;
    j.inf = p.infinity;
    j.X = p.x;
    j.Y = p.y;
    j.Z = BigNum::fromUint64(1);
    return j;
}

EcPoint toAffine(const EcGroup& g, const Jac& j) {
    EcPoint out;
    if (j.inf || j.Z.isZero()) {
        out.infinity = true;
        return out;
    }
    const BigNum zInv = BigNum::inverseModPrime(j.Z, g.p);
    const BigNum zInv2 = BigNum::mulMod(zInv, zInv, g.p);
    out.infinity = false;
    out.x = BigNum::mulMod(j.X, zInv2, g.p);
    out.y = BigNum::mulMod(j.Y, BigNum::mulMod(zInv2, zInv, g.p), g.p);
    return out;
}

} // namespace

const EcGroup& ecGroup(EcCurve curve) {
    const GroupTable& t = table();
    switch (curve) {
        case EcCurve::P384: return t.groups[1];
        case EcCurve::P521: return t.groups[2];
        case EcCurve::P256:
        default: return t.groups[0];
    }
}

bool ecCurveFromOid(const std::string& oid, EcCurve& out) {
    const GroupTable& t = table();
    for (const EcGroup& g : t.groups) {
        if (oid == g.oid) {
            out = g.id;
            return true;
        }
    }
    return false;
}

std::string ecCurveName(EcCurve curve) { return ecGroup(curve).name; }

bool ecIsOnCurve(const EcGroup& g, const EcPoint& p) {
    if (p.infinity) return true;
    if (p.x.isZero() && p.y.isZero()) return false;
    const BigNum lhs = BigNum::mulMod(p.y, p.y, g.p);
    const BigNum x2 = BigNum::mulMod(p.x, p.x, g.p);
    const BigNum x3 = BigNum::mulMod(x2, p.x, g.p);
    const BigNum ax = BigNum::mulMod(g.a, p.x, g.p);
    const BigNum rhs = BigNum::addMod(BigNum::addMod(x3, ax, g.p), g.b, g.p);
    return lhs == rhs;
}

bool ecDecodePoint(const EcGroup& g, const uint8_t* data, size_t len, EcPoint& out) {
    if (len != 1 + 2 * g.fieldBytes || data[0] != 0x04) return false;
    out.x = BigNum::fromBytes(data + 1, g.fieldBytes);
    out.y = BigNum::fromBytes(data + 1 + g.fieldBytes, g.fieldBytes);
    out.infinity = false;
    return ecIsOnCurve(g, out);
}

bool ecPointToBytes(const EcGroup& g, const EcPoint& p, Bytes& out) {
    if (p.infinity) return false;
    out.assign(1 + 2 * g.fieldBytes, 0);
    out[0] = 0x04;
    if (!p.x.toBytes(out.data() + 1, g.fieldBytes)) return false;
    if (!p.y.toBytes(out.data() + 1 + g.fieldBytes, g.fieldBytes)) return false;
    return true;
}

EcPoint ecScalarMul(const EcGroup& g, const Bytes& k, const EcPoint& p) {
    if (p.infinity) return EcPoint();
    Jac acc;
    acc.inf = true;
    const Jac base = toJac(p);

    for (uint8_t byte : k) {
        for (int i = 7; i >= 0; --i) {
            jacDouble(g, acc, acc);
            if ((byte >> i) & 1) {
                jacAdd(g, acc, acc, base);
            }
        }
    }
    return toAffine(g, acc);
}

bool ecBasePointMul(const EcGroup& g, const Bytes& privateKey, Bytes& outUncompressed) {
    EcPoint base;
    base.infinity = false;
    base.x = g.gx;
    base.y = g.gy;
    const EcPoint r = ecScalarMul(g, privateKey, base);
    if (r.infinity) return false;
    return ecPointToBytes(g, r, outUncompressed);
}

bool ecEcdh(const EcGroup& g, const Bytes& privateKey, const uint8_t* peerPoint, size_t peerLen, Bytes& shared) {
    EcPoint peer;
    if (!ecDecodePoint(g, peerPoint, peerLen, peer)) return false;
    const EcPoint r = ecScalarMul(g, privateKey, peer);
    if (r.infinity) return false;
    shared.assign(g.fieldBytes, 0);
    return r.x.toBytes(shared.data(), g.fieldBytes);
}

bool ecEcdsaVerify(const EcGroup& g, const EcPoint& pub, const uint8_t* digest, size_t digestLen, const Bytes& r,
                   const Bytes& s) {
    if (pub.infinity || !ecIsOnCurve(g, pub)) return false;
    if (r.empty() || s.empty()) return false;

    const BigNum rr = BigNum::fromBytes(r.data(), r.size());
    const BigNum ss = BigNum::fromBytes(s.data(), s.size());
    if (rr.isZero() || ss.isZero()) return false;
    if (!(rr < g.n) || !(ss < g.n)) return false;

    // e = 摘要最左 min(bits, digestBits) 位。三条曲线的 bits 与 SHA-256/384/512 的摘要长度
    // 都是 8 的倍数，所以按字节截断即可。
    const size_t take = (digestLen < g.fieldBytes) ? digestLen : g.fieldBytes;
    const BigNum e = BigNum::fromBytes(digest, take);

    const BigNum w = BigNum::inverseModPrime(ss, g.n);
    const BigNum u1 = BigNum::mulMod(e, w, g.n);
    const BigNum u2 = BigNum::mulMod(rr, w, g.n);

    EcPoint base;
    base.infinity = false;
    base.x = g.gx;
    base.y = g.gy;

    const Bytes kb1 = scalarBytes(u1, g.fieldBytes);
    const Bytes kb2 = scalarBytes(u2, g.fieldBytes);
    if (kb1.empty() || kb2.empty()) return false;

    const EcPoint p1 = ecScalarMul(g, kb1, base);
    const EcPoint p2 = ecScalarMul(g, kb2, pub);
    Jac sum;
    jacAdd(g, sum, toJac(p1), toJac(p2));
    if (sum.inf) return false;
    const EcPoint rp = toAffine(g, sum);
    if (rp.infinity) return false;

    BigNum v = rp.x;
    if (!(v < g.n)) v = BigNum::subMod(v, g.n, g.n);   // x mod n（x < p，p 与 n 同量级）
    return v == rr;
}

bool ecEcdsaSignatureFromDer(const Bytes& der, Bytes& r, Bytes& s) {
    DerReader dr(der.data(), der.size());
    const uint8_t* seq = nullptr;
    size_t seqLen = 0;
    if (!dr.readExpected(DER_SEQUENCE, seq, seqLen)) return false;
    DerReader ds(seq, seqLen);
    const uint8_t* rp = nullptr;
    size_t rLen = 0;
    const uint8_t* sp = nullptr;
    size_t sLen = 0;
    if (!ds.readExpected(DER_INTEGER, rp, rLen)) return false;
    if (!ds.readExpected(DER_INTEGER, sp, sLen)) return false;
    return derIntegerToBytes(rp, rLen, r) && derIntegerToBytes(sp, sLen, s);
}

} // namespace jc2::crypto
