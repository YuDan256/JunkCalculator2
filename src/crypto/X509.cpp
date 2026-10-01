#include "X509.h"
#include "Der.h"
#include "Hostname.h"
#include "P256.h"
#include "Rsa.h"
#include "Sha256.h"

#include <cstring>

namespace jc2::crypto {


namespace {

const char* kOidRsaEncryption = "1.2.840.113549.1.1.1";
const char* kOidSha256Rsa = "1.2.840.113549.1.1.11";
const char* kOidRsassaPss = "1.2.840.113549.1.1.10";
const char* kOidEcPublicKey = "1.2.840.10045.2.1";
const char* kOidPrime256v1 = "1.2.840.10045.3.1.7";
const char* kOidEcdsaSha256 = "1.2.840.10045.4.3.2";
const char* kOidSha256 = "2.16.840.1.101.3.4.2.1";
const char* kOidMgf1 = "1.2.840.113549.1.1.8";
const char* kOidBasicConstraints = "2.5.29.19";
const char* kOidKeyUsage = "2.5.29.15";
const char* kOidSubjectAltName = "2.5.29.17";

// ─── 名字（Name）───────────────────────────────────────────────

// Name ::= SEQUENCE OF RelativeDistinguishedName；这里只取 CN（2.5.4.3）。
void parseName(const uint8_t* p, size_t n, Bytes& rawOut, std::string& cnOut) {
    DerReader r(p, n);
    rawOut.assign(p, p + n);
    cnOut.clear();
    while (!r.atEnd()) {
        const uint8_t* rdn = nullptr;
        size_t rdnLen = 0;
        if (!r.readExpected(DER_SET, rdn, rdnLen)) return;
        DerReader rs(rdn, rdnLen);
        while (!rs.atEnd()) {
            const uint8_t* atv = nullptr;
            size_t atvLen = 0;
            if (!rs.readExpected(DER_SEQUENCE, atv, atvLen)) return;
            DerReader a(atv, atvLen);
            const uint8_t* oid = nullptr;
            size_t oidLen = 0;
            if (!a.readExpected(DER_OID, oid, oidLen)) return;
            const uint8_t* val = nullptr;
            size_t valLen = 0;
            uint8_t tag = 0;
            if (!a.read(tag, val, valLen)) return;
            if (derOidToString(oid, oidLen) == "2.5.4.3") {
                cnOut.assign(reinterpret_cast<const char*>(val), valLen);
            }
        }
    }
}

// ─── 扩展 ─────────────────────────────────────────────────────

struct ExtCtx {
    X509Certificate* cert = nullptr;
};

bool extensionCallback(const std::string& oid, bool critical, const uint8_t* value, size_t valueLen, void* user) {
    (void)critical;
    auto* ctx = static_cast<ExtCtx*>(user);
    X509Certificate& cert = *ctx->cert;

    if (oid == kOidBasicConstraints) {
        cert.hasBasicConstraints = true;
        DerReader r(value, valueLen);
        const uint8_t* seq = nullptr;
        size_t seqLen = 0;
        if (!r.readExpected(DER_SEQUENCE, seq, seqLen)) return true;
        DerReader s(seq, seqLen);
        uint8_t tag = 0;
        if (s.peek(tag) && tag == DER_BOOLEAN) {
            const uint8_t* b = nullptr;
            size_t bLen = 0;
            if (!s.readExpected(DER_BOOLEAN, b, bLen)) return true;
            cert.isCa = (bLen == 1 && b[0] != 0x00);
        }
        if (s.peek(tag) && tag == DER_INTEGER) {
            const uint8_t* i = nullptr;
            size_t iLen = 0;
            if (!s.readExpected(DER_INTEGER, i, iLen)) return true;
            int64_t v = 0;
            if (derIntegerToInt64(i, iLen, v)) cert.pathLen = v;
        }
        return true;
    }

    if (oid == kOidKeyUsage) {
        cert.hasKeyUsage = true;
        DerReader r(value, valueLen);
        const uint8_t* bits = nullptr;
        size_t bitsLen = 0;
        if (!r.readExpected(DER_BIT_STRING, bits, bitsLen)) return true;
        if (bitsLen < 2) return true;
        const uint8_t unused = bits[0];
        const uint8_t firstByte = bits[1];
        (void)unused;
        // keyCertSign 是第 5 位（从 0 数）
        cert.keyCertSign = (firstByte & (1u << 2)) != 0;
        return true;
    }

    if (oid == kOidSubjectAltName) {
        DerReader r(value, valueLen);
        const uint8_t* seq = nullptr;
        size_t seqLen = 0;
        if (!r.readExpected(DER_SEQUENCE, seq, seqLen)) return true;
        DerReader s(seq, seqLen);
        while (!s.atEnd()) {
            uint8_t tag = 0;
            const uint8_t* gn = nullptr;
            size_t gnLen = 0;
            if (!s.read(tag, gn, gnLen)) return true;
            if (tag == 0x82) {   // [2] dNSName
                cert.sanDns.emplace_back(reinterpret_cast<const char*>(gn), gnLen);
            }
        }
        return true;
    }

    return true;
}

// ─── 公钥 ─────────────────────────────────────────────────────

bool parseSubjectPublicKeyInfo(const uint8_t* p, size_t n, X509Certificate& cert) {
    // p 指向 SubjectPublicKeyInfo 的**内容**：AlgorithmIdentifier SEQUENCE + BIT STRING
    DerReader s(p, n);
    const uint8_t* alg = nullptr;
    size_t algLen = 0;
    if (!s.readExpected(DER_SEQUENCE, alg, algLen)) return false;

    DerReader a(alg, algLen);
    const uint8_t* oid = nullptr;
    size_t oidLen = 0;
    if (!a.readExpected(DER_OID, oid, oidLen)) return false;
    const std::string algOid = derOidToString(oid, oidLen);

    if (algOid == kOidEcPublicKey) {
        // 参数必须是 prime256v1（在 BIT STRING 之前读）
        const uint8_t* params = nullptr;
        size_t paramsLen = 0;
        if (!a.readExpected(DER_OID, params, paramsLen)) return false;
        if (derOidToString(params, paramsLen) != kOidPrime256v1) return false;
    }

    const uint8_t* bits = nullptr;
    size_t bitsLen = 0;
    if (!s.readExpected(DER_BIT_STRING, bits, bitsLen)) return false;
    if (bitsLen < 1 || bits[0] != 0x00) return false;   // 公钥必须是整字节
    const uint8_t* key = bits + 1;
    const size_t keyLen = bitsLen - 1;

    if (algOid == kOidRsaEncryption) {
        DerReader k(key, keyLen);
        const uint8_t* rsa = nullptr;
        size_t rsaLen = 0;
        if (!k.readExpected(DER_SEQUENCE, rsa, rsaLen)) return false;
        DerReader rs(rsa, rsaLen);
        const uint8_t* mod = nullptr;
        size_t modLen = 0;
        if (!rs.readExpected(DER_INTEGER, mod, modLen)) return false;
        if (!derIntegerToBytes(mod, modLen, cert.rsaN)) return false;
        const uint8_t* exp = nullptr;
        size_t expLen = 0;
        if (!rs.readExpected(DER_INTEGER, exp, expLen)) return false;
        if (!derIntegerToBytes(exp, expLen, cert.rsaE)) return false;
        cert.keyType = X509Certificate::KeyType::Rsa;
        cert.rsaBits = rsaModulusBits(cert.rsaN);
        return true;
    }

    if (algOid == kOidEcPublicKey) {
        if (keyLen != 65 || key[0] != 0x04) return false;
        cert.ecPoint.assign(key, key + keyLen);
        P256Point pt;
        if (!p256DecodePoint(cert.ecPoint.data(), cert.ecPoint.size(), pt)) return false;
        cert.keyType = X509Certificate::KeyType::EcP256;
        return true;
    }

    return false;
}
// ─── PSS 参数 ─────────────────────────────────────────────────

// 解析 RSASSA-PSS-params，取出 hash 与 saltLength（只支持 SHA-256 / MGF1-SHA-256）。
bool parsePssParams(const uint8_t* p, size_t n, size_t& saltLenOut, bool& hashIsSha256, bool& mgfIsSha256) {
    saltLenOut = 20;   // RFC 4055 默认值
    hashIsSha256 = false;
    mgfIsSha256 = false;
    if (n == 0) return true;   // 全默认（SHA-1 系）→ 调用方会判为不支持

    DerReader r(p, n);
    const uint8_t* seq = nullptr;
    size_t seqLen = 0;
    if (!r.readExpected(DER_SEQUENCE, seq, seqLen)) return false;

    DerReader s(seq, seqLen);
    while (!s.atEnd()) {
        uint8_t tag = 0;
        const uint8_t* item = nullptr;
        size_t itemLen = 0;
        if (!s.read(tag, item, itemLen)) return false;
        if (tag == 0xA0) {   // [0] hashAlgorithm
            DerReader h(item, itemLen);
            const uint8_t* hseq = nullptr;
            size_t hseqLen = 0;
            if (!h.readExpected(DER_SEQUENCE, hseq, hseqLen)) return false;
            DerReader hs(hseq, hseqLen);
            const uint8_t* oid = nullptr;
            size_t oidLen = 0;
            if (!hs.readExpected(DER_OID, oid, oidLen)) return false;
            hashIsSha256 = (derOidToString(oid, oidLen) == kOidSha256);
        } else if (tag == 0xA1) {   // [1] maskGenAlgorithm
            DerReader m(item, itemLen);
            const uint8_t* mseq = nullptr;
            size_t mseqLen = 0;
            if (!m.readExpected(DER_SEQUENCE, mseq, mseqLen)) return false;
            DerReader ms(mseq, mseqLen);
            const uint8_t* oid = nullptr;
            size_t oidLen = 0;
            if (!ms.readExpected(DER_OID, oid, oidLen)) return false;
            if (derOidToString(oid, oidLen) != kOidMgf1) return false;
            const uint8_t* inner = nullptr;
            size_t innerLen = 0;
            if (!ms.readExpected(DER_SEQUENCE, inner, innerLen)) return false;
            DerReader is(inner, innerLen);
            const uint8_t* innerOid = nullptr;
            size_t innerOidLen = 0;
            if (!is.readExpected(DER_OID, innerOid, innerOidLen)) return false;
            mgfIsSha256 = (derOidToString(innerOid, innerOidLen) == kOidSha256);
        } else if (tag == 0xA2) {   // [2] saltLength
            DerReader sl(item, itemLen);
            const uint8_t* i = nullptr;
            size_t iLen = 0;
            if (!sl.readExpected(DER_INTEGER, i, iLen)) return false;
            int64_t v = 0;
            if (!derIntegerToInt64(i, iLen, v) || v < 0 || v > 64) return false;
            saltLenOut = static_cast<size_t>(v);
        }
    }
    return true;
}

// ─── 验签 ─────────────────────────────────────────────────────

bool verifyOneSignature(const X509Certificate& cert, const X509Certificate& issuer, std::string& detail) {
    uint8_t digest[32];
    sha256(cert.tbs.data(), cert.tbs.size(), digest);

    const std::string& alg = cert.sigAlgOid;
    if (alg == kOidSha256Rsa) {
        if (issuer.keyType != X509Certificate::KeyType::Rsa) {
            detail = "issuer key is not RSA";
            return false;
        }
        return rsaVerifyPkcs1Sha256(issuer.rsaN, issuer.rsaE, digest, cert.signature);
    }
    if (alg == kOidRsassaPss) {
        if (issuer.keyType != X509Certificate::KeyType::Rsa) {
            detail = "issuer key is not RSA";
            return false;
        }
        if (!cert.pssHashSha256 || !cert.pssMgfSha256) {
            detail = "only RSASSA-PSS with SHA-256/MGF1-SHA-256 is supported";
            return false;
        }
        return rsaVerifyPssSha256(issuer.rsaN, issuer.rsaE, digest, cert.signature, cert.pssSaltLen);
    }
    if (alg == kOidEcdsaSha256) {
        if (issuer.keyType != X509Certificate::KeyType::EcP256) {
            detail = "issuer key is not P-256";
            return false;
        }
        // signature 是 DER SEQUENCE { r INTEGER, s INTEGER }
        DerReader r(cert.signature.data(), cert.signature.size());
        const uint8_t* seq = nullptr;
        size_t seqLen = 0;
        if (!r.readExpected(DER_SEQUENCE, seq, seqLen)) {
            detail = "malformed ECDSA signature";
            return false;
        }
        DerReader s(seq, seqLen);
        const uint8_t* rp = nullptr;
        size_t rLen = 0;
        const uint8_t* sp = nullptr;
        size_t sLen = 0;
        if (!s.readExpected(DER_INTEGER, rp, rLen) || !s.readExpected(DER_INTEGER, sp, sLen)) {
            detail = "malformed ECDSA signature";
            return false;
        }
        Bytes rBytes, sBytes;
        if (!derIntegerToBytes(rp, rLen, rBytes) || !derIntegerToBytes(sp, sLen, sBytes)) {
            detail = "malformed ECDSA signature";
            return false;
        }
        P256Point pub;
        if (!p256DecodePoint(issuer.ecPoint.data(), issuer.ecPoint.size(), pub)) {
            detail = "issuer P-256 key is not on the curve";
            return false;
        }
        return p256EcdsaVerify(pub, digest, rBytes, sBytes);
    }

    detail = "unsupported signature algorithm: " + alg;
    return false;
}

} // namespace

bool x509Parse(const Bytes& der, X509Certificate& out) {
    out = X509Certificate{};
    out.der = der;

    DerReader top(der.data(), der.size());
    const uint8_t* certSeq = nullptr;
    size_t certSeqLen = 0;
    if (!top.readExpected(DER_SEQUENCE, certSeq, certSeqLen)) return false;
    if (!top.atEnd()) return false;   // 证书之外不应有数据

    DerReader c(certSeq, certSeqLen);
    const uint8_t* tbs = nullptr;
    size_t tbsLen = 0;
    const size_t tbsStart = c.pos;   // tbsCertificate TLV 的起点（验签按原样重新哈希）
    if (!c.readExpected(DER_SEQUENCE, tbs, tbsLen)) return false;
    const size_t tbsEnd = c.pos;
    // 注意：c.pos 是相对 Certificate 内容（certSeq）的偏移，certSeq 本身指向 der 内部
    out.tbs.assign(certSeq + tbsStart, certSeq + tbsEnd);

    // signatureAlgorithm
    const uint8_t* outerAlg = nullptr;
    size_t outerAlgLen = 0;
    if (!c.readExpected(DER_SEQUENCE, outerAlg, outerAlgLen)) return false;
    {
        DerReader a(outerAlg, outerAlgLen);
        const uint8_t* oid = nullptr;
        size_t oidLen = 0;
        if (!a.readExpected(DER_OID, oid, oidLen)) return false;
        out.sigAlgOid = derOidToString(oid, oidLen);
        if (out.sigAlgOid == kOidRsassaPss) {
            uint8_t tag = 0;
            if (a.peek(tag) && tag == DER_SEQUENCE) {
                const uint8_t* params = nullptr;
                size_t paramsLen = 0;
                if (!a.readExpected(DER_SEQUENCE, params, paramsLen)) return false;
                if (!parsePssParams(params, paramsLen, out.pssSaltLen, out.pssHashSha256, out.pssMgfSha256)) {
                    return false;
                }
            }
        }
    }

    // signatureValue
    const uint8_t* sigBits = nullptr;
    size_t sigBitsLen = 0;
    if (!c.readExpected(DER_BIT_STRING, sigBits, sigBitsLen)) return false;
    if (sigBitsLen < 1) return false;
    if (sigBits[0] != 0x00) return false;
    out.signature.assign(sigBits + 1, sigBits + sigBitsLen);

    // ── tbsCertificate ──
    DerReader t(tbs, tbsLen);
    uint8_t tag = 0;
    if (!t.peek(tag)) return false;
    if (tag == 0xA0) {   // [0] EXPLICIT version
        const uint8_t* verWrap = nullptr;
        size_t verWrapLen = 0;
        if (!t.read(tag, verWrap, verWrapLen)) return false;
        DerReader v(verWrap, verWrapLen);
        const uint8_t* ver = nullptr;
        size_t verLen = 0;
        if (!v.readExpected(DER_INTEGER, ver, verLen)) return false;
        int64_t vv = 0;
        if (!derIntegerToInt64(ver, verLen, vv)) return false;
        out.version = static_cast<int>(vv) + 1;
    }

    const uint8_t* serial = nullptr;
    size_t serialLen = 0;
    if (!t.readExpected(DER_INTEGER, serial, serialLen)) return false;
    out.serial.assign(serial, serial + serialLen);

    const uint8_t* tbsAlg = nullptr;
    size_t tbsAlgLen = 0;
    if (!t.readExpected(DER_SEQUENCE, tbsAlg, tbsAlgLen)) return false;
    {
        DerReader a(tbsAlg, tbsAlgLen);
        const uint8_t* oid = nullptr;
        size_t oidLen = 0;
        if (!a.readExpected(DER_OID, oid, oidLen)) return false;
        out.tbsSigAlgOid = derOidToString(oid, oidLen);
    }

    const uint8_t* issuer = nullptr;
    size_t issuerLen = 0;
    if (!t.readExpected(DER_SEQUENCE, issuer, issuerLen)) return false;
    parseName(issuer, issuerLen, out.issuerRaw, out.issuerCN);

    const uint8_t* validity = nullptr;
    size_t validityLen = 0;
    if (!t.readExpected(DER_SEQUENCE, validity, validityLen)) return false;
    {
        DerReader v(validity, validityLen);
        uint8_t t1 = 0, t2 = 0;
        const uint8_t* nb = nullptr;
        size_t nbLen = 0;
        const uint8_t* na = nullptr;
        size_t naLen = 0;
        if (!v.read(t1, nb, nbLen)) return false;
        if (!v.read(t2, na, naLen)) return false;
        if (!derTimeToUnix(t1, nb, nbLen, out.notBefore)) return false;
        if (!derTimeToUnix(t2, na, naLen, out.notAfter)) return false;
    }

    const uint8_t* subject = nullptr;
    size_t subjectLen = 0;
    if (!t.readExpected(DER_SEQUENCE, subject, subjectLen)) return false;
    parseName(subject, subjectLen, out.subjectRaw, out.subjectCN);

    const uint8_t* spki = nullptr;
    size_t spkiLen = 0;
    if (!t.readExpected(DER_SEQUENCE, spki, spkiLen)) return false;
    if (!parseSubjectPublicKeyInfo(spki, spkiLen, out)) return false;

    // 可选：issuerUniqueID [1] / subjectUniqueID [2] / extensions [3]
    while (!t.atEnd()) {
        if (!t.peek(tag)) return false;
        if (tag == 0xA3) {
            const uint8_t* extWrap = nullptr;
            size_t extWrapLen = 0;
            if (!t.read(tag, extWrap, extWrapLen)) return false;
            DerReader e(extWrap, extWrapLen);
            const uint8_t* exts = nullptr;
            size_t extsLen = 0;
            if (!e.readExpected(DER_SEQUENCE, exts, extsLen)) return false;
            ExtCtx ctx{&out};
            if (!derForEachExtension(exts, extsLen, extensionCallback, &ctx)) return false;
        } else {
            if (!t.skip()) return false;
        }
    }

    if (out.sigAlgOid != out.tbsSigAlgOid) return false;   // RFC 5280：两者必须一致
    return true;
}

const char* x509FailName(X509Fail f) {
    switch (f) {
        case X509Fail::None: return "ok";
        case X509Fail::MalformedLeaf: return "malformed-leaf";
        case X509Fail::MalformedChain: return "malformed-chain";
        case X509Fail::UnsupportedAlgorithm: return "unsupported-algorithm";
        case X509Fail::WeakKey: return "weak-key";
        case X509Fail::Expired: return "expired";
        case X509Fail::NotYetValid: return "not-yet-valid";
        case X509Fail::HostnameMismatch: return "hostname-mismatch";
        case X509Fail::NoTrustAnchor: return "no-trust-anchor";
        case X509Fail::SignatureInvalid: return "signature-invalid";
        case X509Fail::IssuerNotCa: return "issuer-not-ca";
        case X509Fail::KeyUsageViolation: return "key-usage-violation";
        case X509Fail::PathLenExceeded: return "path-len-exceeded";
        case X509Fail::DepthExceeded: return "depth-exceeded";
    }
    return "unknown";
}

X509VerifyResult x509VerifyChain(const Bytes& leafDer, const std::vector<Bytes>& intermediates,
                                 const std::vector<Bytes>& roots, const std::string& hostname, int64_t nowUnix) {
    X509VerifyResult result;

    std::vector<X509Certificate> chain;
    chain.reserve(8);

    X509Certificate leaf;
    if (!x509Parse(leafDer, leaf)) {
        result.reason = X509Fail::MalformedLeaf;
        result.detail = "leaf certificate could not be parsed";
        return result;
    }
    if (leaf.keyType == X509Certificate::KeyType::None) {
        result.reason = X509Fail::UnsupportedAlgorithm;
        result.detail = "unsupported leaf key type";
        return result;
    }
    if (leaf.keyType == X509Certificate::KeyType::Rsa && leaf.rsaBits < 2048) {
        result.reason = X509Fail::WeakKey;
        result.detail = "RSA key below 2048 bits";
        return result;
    }

    std::vector<X509Certificate> pool;
    pool.reserve(intermediates.size() + roots.size());
    for (const auto& der : intermediates) {
        X509Certificate c;
        if (!x509Parse(der, c)) {
            result.reason = X509Fail::MalformedChain;
            result.detail = "an intermediate certificate could not be parsed";
            return result;
        }
        pool.push_back(std::move(c));
    }
    std::vector<X509Certificate> anchors;
    anchors.reserve(roots.size());
    for (const auto& der : roots) {
        X509Certificate c;
        if (!x509Parse(der, c)) continue;   // 信任库里的坏证书直接跳过
        anchors.push_back(std::move(c));
    }

    // 有效性窗口
    if (nowUnix < leaf.notBefore) {
        result.reason = X509Fail::NotYetValid;
        result.detail = "leaf not yet valid";
        return result;
    }
    if (nowUnix > leaf.notAfter) {
        result.reason = X509Fail::Expired;
        result.detail = "leaf expired";
        return result;
    }

    // 主机名（leaf 的 SAN）
    if (!hostname.empty() && !hostnameMatchAny(leaf.sanDns, hostname)) {
        result.reason = X509Fail::HostnameMismatch;
        result.detail = "leaf SAN does not match " + hostname;
        return result;
    }

    chain.push_back(leaf);

    X509Certificate current = leaf;
    bool anchored = false;
    for (int depth = 0; depth < 8; ++depth) {
        // 是否已被信任锚命中（subject/issuer 相同且 DER 相等）
        for (const auto& anchor : anchors) {
            if (anchor.der == current.der) {
                anchored = true;
                break;
            }
        }
        if (anchored) break;

        // 找签发者：先看信任库，再看中间证书
        const X509Certificate* issuer = nullptr;
        bool issuerIsAnchor = false;
        for (const auto& anchor : anchors) {
            if (anchor.subjectRaw == current.issuerRaw) {
                issuer = &anchor;
                issuerIsAnchor = true;
                break;
            }
        }
        if (!issuer) {
            for (const auto& inter : pool) {
                if (inter.subjectRaw == current.issuerRaw) {
                    issuer = &inter;
                    break;
                }
            }
        }
        if (!issuer) {
            result.reason = X509Fail::NoTrustAnchor;
            result.detail = "no certificate found for issuer \"" + current.issuerCN + "\"";
            return result;
        }

        if (nowUnix < issuer->notBefore) {
            result.reason = X509Fail::NotYetValid;
            result.detail = "issuer not yet valid";
            return result;
        }
        if (nowUnix > issuer->notAfter) {
            result.reason = X509Fail::Expired;
            result.detail = "issuer expired";
            return result;
        }
        if (!issuer->hasBasicConstraints || !issuer->isCa) {
            result.reason = X509Fail::IssuerNotCa;
            result.detail = "issuer is not a CA";
            return result;
        }
        if (issuer->hasKeyUsage && !issuer->keyCertSign) {
            result.reason = X509Fail::KeyUsageViolation;
            result.detail = "issuer keyUsage lacks keyCertSign";
            return result;
        }
        if (issuer->keyType == X509Certificate::KeyType::Rsa && issuer->rsaBits < 2048) {
            result.reason = X509Fail::WeakKey;
            result.detail = "issuer RSA key below 2048 bits";
            return result;
        }

        std::string detail;
        if (!verifyOneSignature(current, *issuer, detail)) {
            result.reason = std::strstr(detail.c_str(), "unsupported") != nullptr ? X509Fail::UnsupportedAlgorithm
                                                                                 : X509Fail::SignatureInvalid;
            result.detail = detail.empty() ? "signature verification failed" : detail;
            return result;
        }

        // 复制一份进链（issuer 指向 pool/anchors，生命周期足够，但保持链自持有更清晰）
        chain.push_back(*issuer);
        current = *issuer;

        if (issuerIsAnchor) {
            anchored = true;
            break;
        }
    }

    if (!anchored) {
        result.reason = X509Fail::DepthExceeded;
        result.detail = "chain longer than 8 or loop detected";
        return result;
    }

    // pathLenConstraint：本证书之下（更靠近叶子一侧）允许的中间 CA 数量。
    // chain[0] 是叶子，chain[1..] 向上；信任锚固定在最后一个位置。
    for (size_t i = 1; i < chain.size(); ++i) {
        const X509Certificate& c = chain[i];
        if (!c.isCa || c.pathLen < 0) continue;
        size_t below = 0;
        for (size_t j = 1; j < i; ++j) {
            if (chain[j].isCa) ++below;
        }
        if (below > static_cast<size_t>(c.pathLen)) {
            result.reason = X509Fail::PathLenExceeded;
            result.detail = "pathLenConstraint exceeded";
            return result;
        }
    }

    result.ok = true;
    result.reason = X509Fail::None;
    result.chainLength = chain.size();
    result.detail = "ok";
    return result;
}

} // namespace jc2::crypto