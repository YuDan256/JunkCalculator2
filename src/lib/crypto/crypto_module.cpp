// crypto 原生模块：把 P0 层的密码学原语暴露给脚本。
//
// 约定：**所有二进制输入输出都是十六进制字符串**（小写无前缀，解码时大小写皆可）。
// 这样接口无歧义（不存在"这个字符串是文本还是十六进制"的猜测），测试也能直接对着
// RFC / NIST 的向量写。
//
// 本模块只提供原语，不做协议：TLS 握手在 src/tls（后续期数），https 接线在 modules/http.jc2。

#include "../jc2_extension_cpp.h"

#include "../internal/crypto/cipher/AesGcm.h"
#include "../internal/crypto/hash/Hkdf.h"
#include "../internal/crypto/hash/Hmac.h"
#include "../internal/crypto/pki/Hostname.h"
#include "../internal/crypto/pubkey/P256.h"
#include "../internal/crypto/util/Random.h"
#include "../internal/crypto/pubkey/Rsa.h"
#include "../internal/crypto/hash/Sha256.h"
#include "../internal/crypto/pki/TrustStore.h"
#include "../internal/crypto/pubkey/X25519.h"
#include "../internal/crypto/pubkey/Curve.h"
#include "../internal/crypto/hash/Sha256.h"
#include "../internal/crypto/hash/Sha512.h"
#include "../internal/crypto/pki/X509.h"

#include <cstdint>
#include <string>
#include <vector>

static jc2::Class* g_cryptoErrorClass = nullptr;

[[noreturn]] inline void throwCryptoError(const std::string& msg) {
    jc2::throw_error_class(*g_cryptoErrorClass, msg);
}

// ─── 十六进制编解码 ────────────────────────────────────────────

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::vector<uint8_t> hexDecode(const jc2::Value& v, const char* what) {
    if (!v.is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(what) + " must be a hexadecimal string.");
    }
    const std::string s = v.as_string();
    if (s.size() % 2 != 0) {
        throwCryptoError(std::string(what) + " must have an even number of hex digits.");
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = hexNibble(s[i]);
        const int lo = hexNibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            throwCryptoError(std::string(what) + " contains a non-hex character.");
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

static std::string hexEncode(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0f];
    }
    return out;
}

static std::vector<uint8_t> requireLength(std::vector<uint8_t> v, size_t want, const char* what) {
    if (v.size() != want) {
        throwCryptoError(std::string(what) + " must be " + std::to_string(want) + " bytes, got " +
                         std::to_string(v.size()) + ".");
    }
    return v;
}

static int requirePositiveInt(const jc2::Value& v, const char* what, int maxValue) {
    if (!v.is_int()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(what) + " must be an integer.");
    }
    const int n = static_cast<int>(v.as_int());
    if (n <= 0 || n > maxValue) {
        throwCryptoError(std::string(what) + " must be in 1.." + std::to_string(maxValue) + ".");
    }
    return n;
}

static void requireArity(int argc, int want, const char* name) {
    if (argc != want) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(name) + " expects " + std::to_string(want) +
                                                        " arguments, got " + std::to_string(argc) + ".");
    }
}

// ─── 哈希与 KDF ───────────────────────────────────────────────

static bool curveFromName(const std::string& name, jc2::crypto::EcCurve& out) {
    if (name == "P-256" || name == "p256" || name == "p-256") { out = jc2::crypto::EcCurve::P256; return true; }
    if (name == "P-384" || name == "p384" || name == "p-384") { out = jc2::crypto::EcCurve::P384; return true; }
    if (name == "P-521" || name == "p521" || name == "p-521") { out = jc2::crypto::EcCurve::P521; return true; }
    return false;
}

static jc2::crypto::EcCurve requireCurve(const jc2::Value& v) {
    if (!v.is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, "curve must be a string such as \"P-256\".");
    }
    jc2::crypto::EcCurve c = jc2::crypto::EcCurve::P256;
    if (!curveFromName(v.as_string(), c)) {
        throwCryptoError("unsupported curve '" + v.as_string() + "' (expected P-256, P-384 or P-521).");
    }
    return c;
}

static JC2_ValueHandle crypto_sha256(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "sha256");
    const std::vector<uint8_t> msg = hexDecode(jc2::Value(argv[0]), "msg");
    uint8_t digest[32];
    jc2::crypto::sha256(msg.data(), msg.size(), digest);
    return jc2::Value(hexEncode(digest, sizeof(digest))).get_handle();
}

static JC2_ValueHandle crypto_sha384(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "sha384");
    const std::vector<uint8_t> msg = hexDecode(jc2::Value(argv[0]), "msg");
    uint8_t digest[48];
    jc2::crypto::sha384(msg.data(), msg.size(), digest);
    return jc2::Value(hexEncode(digest, sizeof(digest))).get_handle();
}

static JC2_ValueHandle crypto_sha512(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "sha512");
    const std::vector<uint8_t> msg = hexDecode(jc2::Value(argv[0]), "msg");
    uint8_t digest[64];
    jc2::crypto::sha512(msg.data(), msg.size(), digest);
    return jc2::Value(hexEncode(digest, sizeof(digest))).get_handle();
}

// 通用 ECDSA 验签（r/s 分开给，方便直接喂 NIST 向量）
static JC2_ValueHandle crypto_ecdsaVerifyRS(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 6, "ecdsaVerifyRS");
    const jc2::crypto::EcCurve curve = requireCurve(jc2::Value(argv[0]));
    const std::vector<uint8_t> pub = hexDecode(jc2::Value(argv[1]), "public");
    const std::string hashName = jc2::Value(argv[2]).as_string();
    const std::vector<uint8_t> message = hexDecode(jc2::Value(argv[3]), "message");
    const jc2::crypto::Bytes r = hexDecode(jc2::Value(argv[4]), "r");
    const jc2::crypto::Bytes ss = hexDecode(jc2::Value(argv[5]), "s");

    const jc2::crypto::EcGroup& g = jc2::crypto::ecGroup(curve);
    jc2::crypto::EcPoint point;
    if (!jc2::crypto::ecDecodePoint(g, pub.data(), pub.size(), point)) {
        return jc2::Value(false).get_handle();
    }

    uint8_t digest[64];
    size_t digestLen = 0;
    if (hashName == "sha256") {
        jc2::crypto::sha256(message.data(), message.size(), digest);
        digestLen = 32;
    } else if (hashName == "sha384") {
        jc2::crypto::sha384(message.data(), message.size(), digest);
        digestLen = 48;
    } else if (hashName == "sha512") {
        jc2::crypto::sha512(message.data(), message.size(), digest);
        digestLen = 64;
    } else {
        throwCryptoError("unsupported hash '" + hashName + "' (expected sha256, sha384 or sha512).");
    }
    return jc2::Value(jc2::crypto::ecEcdsaVerify(g, point, digest, digestLen, r, ss)).get_handle();
}

static JC2_ValueHandle crypto_hmacSha256(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 2, "hmacSha256");
    const std::vector<uint8_t> key = hexDecode(jc2::Value(argv[0]), "key");
    const std::vector<uint8_t> msg = hexDecode(jc2::Value(argv[1]), "msg");
    uint8_t mac[32];
    jc2::crypto::hmacSha256(key.data(), key.size(), msg.data(), msg.size(), mac);
    return jc2::Value(hexEncode(mac, sizeof(mac))).get_handle();
}

static JC2_ValueHandle crypto_hkdfExtract(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 2, "hkdfExtract");
    const std::vector<uint8_t> salt = hexDecode(jc2::Value(argv[0]), "salt");
    const std::vector<uint8_t> ikm = hexDecode(jc2::Value(argv[1]), "ikm");
    uint8_t prk[32];
    jc2::crypto::hkdfExtract(salt.data(), salt.size(), ikm.data(), ikm.size(), prk);
    return jc2::Value(hexEncode(prk, sizeof(prk))).get_handle();
}

static JC2_ValueHandle crypto_hkdfExpand(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 3, "hkdfExpand");
    const std::vector<uint8_t> prk = requireLength(hexDecode(jc2::Value(argv[0]), "prk"), 32, "prk");
    const std::vector<uint8_t> info = hexDecode(jc2::Value(argv[1]), "info");
    const int length = requirePositiveInt(jc2::Value(argv[2]), "length", 255 * 32);
    std::vector<uint8_t> out(static_cast<size_t>(length));
    jc2::crypto::hkdfExpand(prk.data(), info.data(), info.size(), out.data(), out.size());
    return jc2::Value(hexEncode(out.data(), out.size())).get_handle();
}

static JC2_ValueHandle crypto_hkdfExpandLabel(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 4, "hkdfExpandLabel");
    const std::vector<uint8_t> secret = requireLength(hexDecode(jc2::Value(argv[0]), "secret"), 32, "secret");
    if (!jc2::Value(argv[1]).is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, "label must be a string.");
    }
    const std::string label = jc2::Value(argv[1]).as_string();
    if (label.empty() || label.size() > 249) {
        throwCryptoError("label length must be in 1..249.");
    }
    const std::vector<uint8_t> context = hexDecode(jc2::Value(argv[2]), "context");
    if (context.size() > 255) {
        throwCryptoError("context must be at most 255 bytes.");
    }
    const int length = requirePositiveInt(jc2::Value(argv[3]), "length", 255 * 32);
    std::vector<uint8_t> out(static_cast<size_t>(length));
    jc2::crypto::hkdfExpandLabel(secret.data(), label.c_str(), context.data(), context.size(), out.data(), out.size());
    return jc2::Value(hexEncode(out.data(), out.size())).get_handle();
}

// ─── AEAD ─────────────────────────────────────────────────────

static JC2_ValueHandle crypto_aesGcmSeal(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 4, "aesGcmSeal");
    const std::vector<uint8_t> key = requireLength(hexDecode(jc2::Value(argv[0]), "key"), 16, "key");
    const std::vector<uint8_t> iv = hexDecode(jc2::Value(argv[1]), "iv");
    const std::vector<uint8_t> aad = hexDecode(jc2::Value(argv[2]), "aad");
    const std::vector<uint8_t> pt = hexDecode(jc2::Value(argv[3]), "pt");
    if (iv.empty()) {
        throwCryptoError("iv must not be empty.");
    }
    std::vector<uint8_t> ct(pt.size());
    uint8_t tag[16];
    jc2::crypto::aes128GcmSeal(key.data(), iv.data(), iv.size(), aad.data(), aad.size(),
                               pt.empty() ? nullptr : pt.data(), pt.size(),
                               pt.empty() ? nullptr : ct.data(), tag);
    jc2::Dict result;
    result.set(jc2::Value("ct"), jc2::Value(hexEncode(ct.data(), ct.size())));
    result.set(jc2::Value("tag"), jc2::Value(hexEncode(tag, sizeof(tag))));
    return result.get_handle();
}

static JC2_ValueHandle crypto_aesGcmOpen(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 5, "aesGcmOpen");
    const std::vector<uint8_t> key = requireLength(hexDecode(jc2::Value(argv[0]), "key"), 16, "key");
    const std::vector<uint8_t> iv = hexDecode(jc2::Value(argv[1]), "iv");
    const std::vector<uint8_t> aad = hexDecode(jc2::Value(argv[2]), "aad");
    const std::vector<uint8_t> ct = hexDecode(jc2::Value(argv[3]), "ct");
    const std::vector<uint8_t> tag = requireLength(hexDecode(jc2::Value(argv[4]), "tag"), 16, "tag");
    if (iv.empty()) {
        throwCryptoError("iv must not be empty.");
    }
    std::vector<uint8_t> pt(ct.size());
    const bool ok = jc2::crypto::aes128GcmOpen(key.data(), iv.data(), iv.size(), aad.data(), aad.size(),
                                               ct.empty() ? nullptr : ct.data(), ct.size(), tag.data(),
                                               ct.empty() ? nullptr : pt.data());
    if (!ok) {
        throwCryptoError("authentication tag mismatch.");
    }
    return jc2::Value(hexEncode(pt.data(), pt.size())).get_handle();
}

// ─── 密钥交换 ─────────────────────────────────────────────────

static JC2_ValueHandle crypto_x25519(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 2, "x25519");
    const std::vector<uint8_t> scalar = requireLength(hexDecode(jc2::Value(argv[0]), "scalar"), 32, "scalar");
    const std::vector<uint8_t> point = requireLength(hexDecode(jc2::Value(argv[1]), "point"), 32, "point");
    uint8_t out[32];
    if (!jc2::crypto::x25519(out, scalar.data(), point.data())) {
        throwCryptoError("x25519 produced the all-zero shared secret (low-order point).");
    }
    return jc2::Value(hexEncode(out, sizeof(out))).get_handle();
}

static JC2_ValueHandle crypto_x25519Base(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "x25519Base");
    const std::vector<uint8_t> scalar = requireLength(hexDecode(jc2::Value(argv[0]), "scalar"), 32, "scalar");
    uint8_t base[32];
    for (int i = 0; i < 32; ++i) base[i] = 0;
    base[0] = 9;
    uint8_t out[32];
    if (!jc2::crypto::x25519(out, scalar.data(), base)) {
        throwCryptoError("x25519Base produced the all-zero shared secret.");
    }
    return jc2::Value(hexEncode(out, sizeof(out))).get_handle();
}

// ─── 公钥算法与证书（P1）─────────────────────────────────────

// 通用 ECDH：返回共享点 X 坐标（按曲线定长）的十六进制
static JC2_ValueHandle crypto_ecEcdh(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 3, "ecEcdh");
    const jc2::crypto::EcCurve curve = requireCurve(jc2::Value(argv[0]));
    const std::vector<uint8_t> priv = hexDecode(jc2::Value(argv[1]), "private");
    const std::vector<uint8_t> peer = hexDecode(jc2::Value(argv[2]), "point");
    const jc2::crypto::EcGroup& g = jc2::crypto::ecGroup(curve);
    jc2::crypto::Bytes shared;
    if (!jc2::crypto::ecEcdh(g, priv, peer.data(), peer.size(), shared)) {
        throwCryptoError(std::string("ecEcdh failed (invalid private key or peer point for ") + g.name + ").");
    }
    return jc2::Value(hexEncode(shared.data(), shared.size())).get_handle();
}

// 通用基点乘法：返回未压缩点（0x04||X||Y）的十六进制
static JC2_ValueHandle crypto_ecBasePointMul(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 2, "ecBasePointMul");
    const jc2::crypto::EcCurve curve = requireCurve(jc2::Value(argv[0]));
    const std::vector<uint8_t> priv = hexDecode(jc2::Value(argv[1]), "private");
    const jc2::crypto::EcGroup& g = jc2::crypto::ecGroup(curve);
    jc2::crypto::Bytes point;
    if (!jc2::crypto::ecBasePointMul(g, priv, point)) {
        throwCryptoError(std::string("ecBasePointMul failed for ") + g.name + ".");
    }
    return jc2::Value(hexEncode(point.data(), point.size())).get_handle();
}

// 通用 ECDSA 验签：curve + 公钥点 + 摘要名 + 消息 + DER 签名
static JC2_ValueHandle crypto_ecdsaVerify(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 5, "ecdsaVerify");
    const jc2::crypto::EcCurve curve = requireCurve(jc2::Value(argv[0]));
    const std::vector<uint8_t> pub = hexDecode(jc2::Value(argv[1]), "public");
    const std::string hashName = jc2::Value(argv[2]).as_string();
    const std::vector<uint8_t> message = hexDecode(jc2::Value(argv[3]), "message");
    const std::vector<uint8_t> signature = hexDecode(jc2::Value(argv[4]), "signature");

    const jc2::crypto::EcGroup& g = jc2::crypto::ecGroup(curve);
    jc2::crypto::EcPoint point;
    if (!jc2::crypto::ecDecodePoint(g, pub.data(), pub.size(), point)) {
        return jc2::Value(false).get_handle();   // 点不在曲线上 → 判为验签失败
    }

    uint8_t digest[64];
    size_t digestLen = 0;
    if (hashName == "sha256") {
        jc2::crypto::sha256(message.data(), message.size(), digest);
        digestLen = 32;
    } else if (hashName == "sha384") {
        jc2::crypto::sha384(message.data(), message.size(), digest);
        digestLen = 48;
    } else if (hashName == "sha512") {
        jc2::crypto::sha512(message.data(), message.size(), digest);
        digestLen = 64;
    } else {
        throwCryptoError("unsupported hash '" + hashName + "' (expected sha256, sha384 or sha512).");
    }

    jc2::crypto::Bytes r, ss;
    if (!jc2::crypto::ecEcdsaSignatureFromDer(signature, r, ss)) {
        return jc2::Value(false).get_handle();
    }
    return jc2::Value(jc2::crypto::ecEcdsaVerify(g, point, digest, digestLen, r, ss)).get_handle();
}

static JC2_ValueHandle crypto_p256Ecdh(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 2, "p256Ecdh");
    const std::vector<uint8_t> priv = requireLength(hexDecode(jc2::Value(argv[0]), "private"), 32, "private");
    const std::vector<uint8_t> peer = requireLength(hexDecode(jc2::Value(argv[1]), "point"), 65, "point");
    uint8_t out[32];
    if (!jc2::crypto::p256Ecdh(priv, peer.data(), peer.size(), out)) {
        throwCryptoError("p256Ecdh failed (point not on curve or result at infinity).");
    }
    return jc2::Value(hexEncode(out, sizeof(out))).get_handle();
}

static JC2_ValueHandle crypto_ecdsaVerifyP256(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 4, "ecdsaVerifyP256");
    const std::vector<uint8_t> pub = requireLength(hexDecode(jc2::Value(argv[0]), "public"), 65, "public");
    const std::vector<uint8_t> hash = requireLength(hexDecode(jc2::Value(argv[1]), "hash"), 32, "hash");
    const std::vector<uint8_t> r = hexDecode(jc2::Value(argv[2]), "r");
    const std::vector<uint8_t> s = hexDecode(jc2::Value(argv[3]), "s");
    jc2::crypto::P256Point point;
    if (!jc2::crypto::p256DecodePoint(pub.data(), pub.size(), point)) return jc2::Value(false).get_handle();
    return jc2::Value(jc2::crypto::p256EcdsaVerify(point, hash.data(), r, s)).get_handle();
}

static JC2_ValueHandle crypto_rsaVerifyPkcs1(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 4, "rsaVerifyPkcs1");
    const std::vector<uint8_t> n = hexDecode(jc2::Value(argv[0]), "n");
    const std::vector<uint8_t> e = hexDecode(jc2::Value(argv[1]), "e");
    const std::vector<uint8_t> hash = requireLength(hexDecode(jc2::Value(argv[2]), "hash"), 32, "hash");
    const std::vector<uint8_t> sig = hexDecode(jc2::Value(argv[3]), "signature");
    return jc2::Value(jc2::crypto::rsaVerifyPkcs1Sha256(n, e, hash.data(), sig)).get_handle();
}

static JC2_ValueHandle crypto_rsaVerifyPss(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 5, "rsaVerifyPss");
    const std::vector<uint8_t> n = hexDecode(jc2::Value(argv[0]), "n");
    const std::vector<uint8_t> e = hexDecode(jc2::Value(argv[1]), "e");
    const std::vector<uint8_t> hash = requireLength(hexDecode(jc2::Value(argv[2]), "hash"), 32, "hash");
    const std::vector<uint8_t> sig = hexDecode(jc2::Value(argv[3]), "signature");
    const int saltLen = requirePositiveInt(jc2::Value(argv[4]), "saltLen", 64);
    return jc2::Value(jc2::crypto::rsaVerifyPssSha256(n, e, hash.data(), sig, static_cast<size_t>(saltLen)))
        .get_handle();
}

static JC2_ValueHandle crypto_hostnameMatch(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 2, "hostnameMatch");
    if (!jc2::Value(argv[0]).is_string() || !jc2::Value(argv[1]).is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, "hostnameMatch expects two strings.");
    }
    const bool ok = jc2::crypto::hostnameMatch(jc2::Value(argv[0]).as_string(), jc2::Value(argv[1]).as_string());
    return jc2::Value(ok).get_handle();
}

static JC2_ValueHandle crypto_x509Parse(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "x509Parse");
    const std::vector<uint8_t> der = hexDecode(jc2::Value(argv[0]), "der");
    jc2::crypto::X509Certificate cert;
    if (!jc2::crypto::x509Parse(der, cert)) {
        throwCryptoError("certificate could not be parsed.");
    }
    jc2::Dict out;
    out.set(jc2::Value("subject"), jc2::Value(cert.subjectCN));
    out.set(jc2::Value("issuer"), jc2::Value(cert.issuerCN));
    out.set(jc2::Value("notBefore"), jc2::BigInt(std::to_string(cert.notBefore)));
    out.set(jc2::Value("notAfter"), jc2::BigInt(std::to_string(cert.notAfter)));
    out.set(jc2::Value("isCa"), jc2::Value(cert.isCa));
    out.set(jc2::Value("pathLen"), jc2::Value(static_cast<int32_t>(cert.pathLen)));
    const char* keyType = "unknown";
    switch (cert.keyType) {
        case jc2::crypto::X509Certificate::KeyType::Rsa: keyType = "rsa"; break;
        case jc2::crypto::X509Certificate::KeyType::EcP256: keyType = "ec-p256"; break;
        case jc2::crypto::X509Certificate::KeyType::EcP384: keyType = "ec-p384"; break;
        case jc2::crypto::X509Certificate::KeyType::EcP521: keyType = "ec-p521"; break;
        case jc2::crypto::X509Certificate::KeyType::None:
        default: keyType = "unknown"; break;
    }
    out.set(jc2::Value("keyType"), jc2::Value(keyType));
    out.set(jc2::Value("rsaBits"), jc2::Value(static_cast<int32_t>(cert.rsaBits)));
    out.set(jc2::Value("sigAlg"), jc2::Value(cert.sigAlgOid));
    jc2::List sans;
    for (const auto& name : cert.sanDns) {
        sans.push_back(jc2::Value(name));
    }
    out.set(jc2::Value("san"), sans);
    return out.get_handle();
}

// 把 JC2 的十六进制字符串列表转成 DER 列表
static std::vector<std::vector<uint8_t>> hexListArg(const jc2::Value& v, const char* what) {
    std::vector<std::vector<uint8_t>> out;
    if (!v.is_list()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(what) + " must be a list of hex strings.");
    }
    jc2::List list(v.get_handle());
    const size_t n = list.size();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(hexDecode(list.get(i), what));
    }
    return out;
}

static JC2_ValueHandle crypto_x509Verify(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 5, "x509Verify");
    const std::vector<uint8_t> leaf = hexDecode(jc2::Value(argv[0]), "leaf");
    const auto intermediates = hexListArg(jc2::Value(argv[1]), "intermediates");
    const auto roots = hexListArg(jc2::Value(argv[2]), "roots");
    const std::string hostname = jc2::Value(argv[3]).is_none() ? std::string() : jc2::Value(argv[3]).as_string();
    int64_t now = 0;
    {
        const jc2::Value t(argv[4]);
        if (t.is_bigint()) {
            now = std::stoll(t.to_string());
        } else if (t.is_int() || t.is_float()) {
            now = static_cast<int64_t>(t.as_float());
        } else {
            jc2::throw_error(jc2::ErrorType::TypeError, "time must be a number of Unix seconds.");
        }
    }

    const auto result = jc2::crypto::x509VerifyChain(leaf, intermediates, roots, hostname, now);
    jc2::Dict out;
    out.set(jc2::Value("ok"), jc2::Value(result.ok));
    out.set(jc2::Value("reason"), jc2::Value(std::string(jc2::crypto::x509FailName(result.reason))));
    out.set(jc2::Value("chainLength"), jc2::Value(static_cast<int32_t>(result.chainLength)));
    out.set(jc2::Value("detail"), jc2::Value(result.detail));
    return out.get_handle();
}

static JC2_ValueHandle crypto_systemRoots(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    (void)argv;
    requireArity(argc, 0, "systemRoots");
    jc2::List out;
    for (const auto& der : jc2::crypto::systemRootCertificates()) {
        out.push_back(jc2::Value(hexEncode(der.data(), der.size())));
    }
    return out.get_handle();
}

static JC2_ValueHandle crypto_randomBytes(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "randomBytes");
    const int n = requirePositiveInt(jc2::Value(argv[0]), "length", 4096);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (!jc2::crypto::randomBytes(buf.data(), buf.size())) {
        throwCryptoError("the system random number generator failed.");
    }
    return jc2::Value(hexEncode(buf.data(), buf.size())).get_handle();
}

// ─── 模块注册 ─────────────────────────────────────────────────

int jc2_init(jc2::Module& mod) {
    g_cryptoErrorClass = new jc2::Class("CryptoError");
    jc2::Value exceptionCls = jc2::get_global("Exception");
    g_cryptoErrorClass->set_parent(jc2::Class(exceptionCls.get_handle()));
    mod.register_value("CryptoError", *g_cryptoErrorClass);

    // 约定：二进制一律十六进制字符串
    mod.register_function("sha256", crypto_sha256, 1, 1, {"msg"});
    mod.register_function("sha384", crypto_sha384, 1, 1, {"msg"});
    mod.register_function("sha512", crypto_sha512, 1, 1, {"msg"});
    mod.register_function("ecdsaVerifyRS", crypto_ecdsaVerifyRS, 6, 6,
                          {"curve", "public", "hash", "message", "r", "s"});
    mod.register_function("hmacSha256", crypto_hmacSha256, 2, 2, {"key", "msg"});
    mod.register_function("hkdfExtract", crypto_hkdfExtract, 2, 2, {"salt", "ikm"});
    mod.register_function("hkdfExpand", crypto_hkdfExpand, 3, 3, {"prk", "info", "length"});
    mod.register_function("hkdfExpandLabel", crypto_hkdfExpandLabel, 4, 4, {"secret", "label", "context", "length"});
    mod.register_function("aesGcmSeal", crypto_aesGcmSeal, 4, 4, {"key", "iv", "aad", "pt"});
    mod.register_function("aesGcmOpen", crypto_aesGcmOpen, 5, 5, {"key", "iv", "aad", "ct", "tag"});
    mod.register_function("ecEcdh", crypto_ecEcdh, 3, 3, {"curve", "private", "point"});
    mod.register_function("ecBasePointMul", crypto_ecBasePointMul, 2, 2, {"curve", "private"});
    mod.register_function("ecdsaVerify", crypto_ecdsaVerify, 5, 5, {"curve", "public", "hash", "message", "signature"});
    mod.register_function("x25519", crypto_x25519, 2, 2, {"scalar", "point"});
    mod.register_function("x25519Base", crypto_x25519Base, 1, 1, {"scalar"});
    mod.register_function("p256Ecdh", crypto_p256Ecdh, 2, 2, {"private", "point"});
    mod.register_function("ecdsaVerifyP256", crypto_ecdsaVerifyP256, 4, 4, {"public", "hash", "r", "s"});
    mod.register_function("rsaVerifyPkcs1", crypto_rsaVerifyPkcs1, 4, 4, {"n", "e", "hash", "signature"});
    mod.register_function("rsaVerifyPss", crypto_rsaVerifyPss, 5, 5, {"n", "e", "hash", "signature", "saltLen"});
    mod.register_function("hostnameMatch", crypto_hostnameMatch, 2, 2, {"pattern", "host"});
    mod.register_function("x509Parse", crypto_x509Parse, 1, 1, {"der"});
    mod.register_function("x509Verify", crypto_x509Verify, 5, 5, {"leaf", "intermediates", "roots", "hostname", "time"});
    mod.register_function("systemRoots", crypto_systemRoots, 0, 0);
    mod.register_function("randomBytes", crypto_randomBytes, 1, 1, {"length"});

    mod.register_function_help("crypto.sha256", "crypto.sha256(msg)", "SHA-256 of msg; both are hex strings.", "crypto.sha256(\"616263\")");
    mod.register_function_help("crypto.hmacSha256", "crypto.hmacSha256(key, msg)", "HMAC-SHA256; key and msg are hex strings.", "crypto.hmacSha256(\"4a656665\", \"6869\")");
    mod.register_function_help("crypto.hkdfExtract", "crypto.hkdfExtract(salt, ikm)", "HKDF-Extract (RFC 5869), returns the 32-byte PRK as hex.", "crypto.hkdfExtract(\"\", \"0b0b\")");
    mod.register_function_help("crypto.hkdfExpand", "crypto.hkdfExpand(prk, info, length)", "HKDF-Expand; prk is 32 bytes of hex, returns length bytes as hex.", "crypto.hkdfExpand(prk, \"\", 42)");
    mod.register_function_help("crypto.hkdfExpandLabel", "crypto.hkdfExpandLabel(secret, label, context, length)", "TLS 1.3 HKDF-Expand-Label; label is text, the rest is hex.", "");
    mod.register_function_help("crypto.aesGcmSeal", "crypto.aesGcmSeal(key, iv, aad, pt)", "AES-128-GCM encryption; returns {ct, tag} as hex.", "");
    mod.register_function_help("crypto.aesGcmOpen", "crypto.aesGcmOpen(key, iv, aad, ct, tag)", "AES-128-GCM decryption; raises CryptoError if the tag does not match.", "");
    mod.register_function_help("crypto.x25519", "crypto.x25519(scalar, point)", "X25519 scalar multiplication (RFC 7748); raises CryptoError on an all-zero result.", "");
    mod.register_function_help("crypto.x25519Base", "crypto.x25519Base(scalar)", "X25519 with the standard base point u = 9.", "");
    mod.register_function_help("crypto.p256Ecdh", "crypto.p256Ecdh(private, point)", "P-256 ECDH; point is the 65-byte uncompressed peer key in hex.", "");
    mod.register_function_help("crypto.ecdsaVerifyP256", "crypto.ecdsaVerifyP256(public, hash, r, s)", "ECDSA P-256 / SHA-256 signature verification; returns a bool.", "");
    mod.register_function_help("crypto.rsaVerifyPkcs1", "crypto.rsaVerifyPkcs1(n, e, hash, signature)", "RSA PKCS#1 v1.5 with SHA-256; returns a bool.", "");
    mod.register_function_help("crypto.rsaVerifyPss", "crypto.rsaVerifyPss(n, e, hash, signature, saltLen)", "RSASSA-PSS with SHA-256/MGF1-SHA-256; returns a bool.", "");
    mod.register_function_help("crypto.hostnameMatch", "crypto.hostnameMatch(pattern, host)", "RFC 6125 dNSName matching (wildcards only in the leftmost label).", "");
    mod.register_function_help("crypto.x509Parse", "crypto.x509Parse(der)", "Parses a DER certificate into a dict (subject/issuer/san/validity/keyType).", "");
    mod.register_function_help("crypto.ecEcdh", "crypto.ecEcdh(curve, private, point)",
        "ECDH on P-256/P-384/P-521; returns the shared X coordinate as hex.", "crypto.ecEcdh(\"P-384\", priv, peer)");
    mod.register_function_help("crypto.ecBasePointMul", "crypto.ecBasePointMul(curve, private)",
        "Multiplies the curve generator by a scalar; returns the uncompressed point as hex.", "");
    mod.register_function_help("crypto.sha384", "crypto.sha384(hex)", "SHA-384 of the hex-encoded bytes.", "");
    mod.register_function_help("crypto.sha512", "crypto.sha512(hex)", "SHA-512 of the hex-encoded bytes.", "");
    mod.register_function_help("crypto.ecdsaVerifyRS", "crypto.ecdsaVerifyRS(curve, public, hash, message, r, s)",
        "Like ecdsaVerify but with r/s instead of a DER signature (NIST vector friendly).", "");
    mod.register_function_help("crypto.ecdsaVerify", "crypto.ecdsaVerify(curve, public, hash, message, signature)",
        "Verifies a DER ECDSA signature over message with sha256/sha384/sha512.", "");
    mod.register_function_help("crypto.x509Verify", "crypto.x509Verify(leaf, intermediates, roots, hostname, time)", "Verifies a certificate chain at the given Unix time; returns {ok, reason, chainLength, detail}.", "");
    mod.register_function_help("crypto.systemRoots", "crypto.systemRoots()", "Returns the system trust store (Windows ROOT) as a list of DER certificates in hex.", "");
    mod.register_function_help("crypto.randomBytes", "crypto.randomBytes(length)", "Cryptographically secure random bytes (system CSPRNG) as hex.", "");

    mod.register_help("crypto",
        "═══ Cryptographic Primitives (Native Module) ═══\n\n"
        "  Requires: import crypto\n\n"
        "  Building blocks used by TLS; no protocol logic lives here.\n"
        "  Every binary input and output is a lowercase hexadecimal string.\n"
        "  Empty hex (\"\") means an empty byte string.\n\n"
        "  Hashing / KDF\n"
        "    crypto.sha256(msg)                        SHA-256, returns 32 bytes as hex\n"
        "    crypto.hmacSha256(key, msg)               HMAC-SHA256\n"
        "    crypto.hkdfExtract(salt, ikm)             HKDF-Extract, returns the 32-byte PRK\n"
        "    crypto.hkdfExpand(prk, info, length)      HKDF-Expand\n"
        "    crypto.hkdfExpandLabel(secret, label, context, length)   TLS 1.3 HKDF-Expand-Label\n\n"
        "  AEAD\n"
        "    crypto.aesGcmSeal(key, iv, aad, pt)       AES-128-GCM, returns {ct, tag}\n"
        "    crypto.aesGcmOpen(key, iv, aad, ct, tag)  AES-128-GCM, raises CryptoError on tag mismatch\n\n"
        "  Key exchange\n"
        "    crypto.x25519(scalar, point)              RFC 7748\n"
        "    crypto.x25519Base(scalar)                 u = 9\n"
        "    crypto.p256Ecdh(private, point)           NIST P-256 ECDH\n\n"
        "  Public key signatures / certificates\n"
        "    crypto.ecdsaVerifyP256(public, hash, r, s)      ECDSA P-256 / SHA-256\n"
        "    crypto.rsaVerifyPkcs1(n, e, hash, signature)    RSA PKCS#1 v1.5 / SHA-256\n"
        "    crypto.rsaVerifyPss(n, e, hash, signature, saltLen)\n"
        "    crypto.x509Parse(der)                    Certificate fields as a dict\n"
        "    crypto.x509Verify(leaf, intermediates, roots, hostname, time)\n"
        "                                             Chain verification at a given Unix time\n"
        "    crypto.hostnameMatch(pattern, host)      RFC 6125 dNSName matching\n"
        "    crypto.systemRoots()                     System trust store (Windows ROOT) as hex DER\n"
        "    crypto.randomBytes(length)               System CSPRNG\n"
    );

    return 0;
}

JC2_EXTENSION_INIT
