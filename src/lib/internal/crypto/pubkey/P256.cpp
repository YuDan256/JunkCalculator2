// P-256 的兼容外壳：真正的实现已经合并进 Curve（P-256/384/521 共用 Jacobian 点运算）。
// 保留这层是为了不让 X509 / TlsClient / crypto 模块的既有调用点跟着改。
#include "P256.h"

#include "Curve.h"

#include <cstring>

namespace jc2::crypto {

namespace {

const EcGroup& group() { return ecGroup(EcCurve::P256); }

EcPoint toEc(const P256Point& p) {
    EcPoint e;
    e.infinity = p.infinity;
    e.x = p.x;
    e.y = p.y;
    return e;
}

P256Point toP256(const EcPoint& e) {
    P256Point p;
    p.infinity = e.infinity;
    p.x = e.x;
    p.y = e.y;
    return p;
}

} // namespace

bool p256DecodePoint(const uint8_t* data, size_t len, P256Point& out) {
    EcPoint e;
    if (!ecDecodePoint(group(), data, len, e)) return false;
    out = toP256(e);
    return true;
}

bool p256IsOnCurve(const P256Point& p) { return ecIsOnCurve(group(), toEc(p)); }

P256Point p256ScalarMul(const Bytes& k, const P256Point& p) { return toP256(ecScalarMul(group(), k, toEc(p))); }

bool p256BasePointMul(const Bytes& privateKey, uint8_t out[65]) {
    Bytes encoded;
    if (!ecBasePointMul(group(), privateKey, encoded) || encoded.size() != 65) return false;
    std::memcpy(out, encoded.data(), 65);
    return true;
}

bool p256Ecdh(const Bytes& privateKey, const uint8_t* peerPoint, size_t peerLen, uint8_t out[32]) {
    Bytes shared;
    if (!ecEcdh(group(), privateKey, peerPoint, peerLen, shared) || shared.size() != 32) return false;
    std::memcpy(out, shared.data(), 32);
    return true;
}

bool p256EcdsaVerify(const P256Point& pub, const uint8_t hash32[32], const Bytes& r, const Bytes& s) {
    return ecEcdsaVerify(group(), toEc(pub), hash32, 32, r, s);
}

} // namespace jc2::crypto
