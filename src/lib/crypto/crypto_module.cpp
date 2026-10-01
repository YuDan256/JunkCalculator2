// crypto 原生模块：把 P0 层的密码学原语暴露给脚本。
//
// 约定：**所有二进制输入输出都是十六进制字符串**（小写无前缀，解码时大小写皆可）。
// 这样接口无歧义（不存在"这个字符串是文本还是十六进制"的猜测），测试也能直接对着
// RFC / NIST 的向量写。
//
// 本模块只提供原语，不做协议：TLS 握手在 src/tls（后续期数），https 接线在 modules/http.jc2。

#include "../jc2_extension_cpp.h"

#include "../../crypto/AesGcm.h"
#include "../../crypto/Hkdf.h"
#include "../../crypto/Hmac.h"
#include "../../crypto/Sha256.h"
#include "../../crypto/X25519.h"

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

static JC2_ValueHandle crypto_sha256(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "sha256");
    const std::vector<uint8_t> msg = hexDecode(jc2::Value(argv[0]), "msg");
    uint8_t digest[32];
    jc2::crypto::sha256(msg.data(), msg.size(), digest);
    return jc2::Value(hexEncode(digest, sizeof(digest))).get_handle();
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

// ─── 模块注册 ─────────────────────────────────────────────────

int jc2_init(jc2::Module& mod) {
    g_cryptoErrorClass = new jc2::Class("CryptoError");
    jc2::Value exceptionCls = jc2::get_global("Exception");
    g_cryptoErrorClass->set_parent(jc2::Class(exceptionCls.get_handle()));
    mod.register_value("CryptoError", *g_cryptoErrorClass);

    // 约定：二进制一律十六进制字符串
    mod.register_function("sha256", crypto_sha256, 1, 1, {"msg"});
    mod.register_function("hmacSha256", crypto_hmacSha256, 2, 2, {"key", "msg"});
    mod.register_function("hkdfExtract", crypto_hkdfExtract, 2, 2, {"salt", "ikm"});
    mod.register_function("hkdfExpand", crypto_hkdfExpand, 3, 3, {"prk", "info", "length"});
    mod.register_function("hkdfExpandLabel", crypto_hkdfExpandLabel, 4, 4, {"secret", "label", "context", "length"});
    mod.register_function("aesGcmSeal", crypto_aesGcmSeal, 4, 4, {"key", "iv", "aad", "pt"});
    mod.register_function("aesGcmOpen", crypto_aesGcmOpen, 5, 5, {"key", "iv", "aad", "ct", "tag"});
    mod.register_function("x25519", crypto_x25519, 2, 2, {"scalar", "point"});
    mod.register_function("x25519Base", crypto_x25519Base, 1, 1, {"scalar"});

    mod.register_function_help("crypto.sha256", "crypto.sha256(msg)", "SHA-256 of msg; both are hex strings.", "crypto.sha256(\"616263\")");
    mod.register_function_help("crypto.hmacSha256", "crypto.hmacSha256(key, msg)", "HMAC-SHA256; key and msg are hex strings.", "crypto.hmacSha256(\"4a656665\", \"6869\")");
    mod.register_function_help("crypto.hkdfExtract", "crypto.hkdfExtract(salt, ikm)", "HKDF-Extract (RFC 5869), returns the 32-byte PRK as hex.", "crypto.hkdfExtract(\"\", \"0b0b\")");
    mod.register_function_help("crypto.hkdfExpand", "crypto.hkdfExpand(prk, info, length)", "HKDF-Expand; prk is 32 bytes of hex, returns length bytes as hex.", "crypto.hkdfExpand(prk, \"\", 42)");
    mod.register_function_help("crypto.hkdfExpandLabel", "crypto.hkdfExpandLabel(secret, label, context, length)", "TLS 1.3 HKDF-Expand-Label; label is text, the rest is hex.", "");
    mod.register_function_help("crypto.aesGcmSeal", "crypto.aesGcmSeal(key, iv, aad, pt)", "AES-128-GCM encryption; returns {ct, tag} as hex.", "");
    mod.register_function_help("crypto.aesGcmOpen", "crypto.aesGcmOpen(key, iv, aad, ct, tag)", "AES-128-GCM decryption; raises CryptoError if the tag does not match.", "");
    mod.register_function_help("crypto.x25519", "crypto.x25519(scalar, point)", "X25519 scalar multiplication (RFC 7748); raises CryptoError on an all-zero result.", "");
    mod.register_function_help("crypto.x25519Base", "crypto.x25519Base(scalar)", "X25519 with the standard base point u = 9.", "");

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
    );

    return 0;
}

JC2_EXTENSION_INIT
