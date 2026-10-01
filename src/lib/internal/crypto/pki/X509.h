// X.509 证书解析与证书链校验。
#pragma once

#include "../util/BytesUtil.h"
#include "../pubkey/Rsa.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jc2::crypto {

struct X509Certificate {
    Bytes der;
    Bytes tbs;                    // tbsCertificate 的完整 TLV（验签要重新哈希它）
    int version = 1;
    Bytes serial;
    std::string sigAlgOid;        // 外层 signatureAlgorithm
    std::string tbsSigAlgOid;     // tbsCertificate.signature（必须与外层一致）
    Bytes issuerRaw;              // Name 的原始内容（用于相等比较）
    Bytes subjectRaw;
    std::string subjectCN;
    std::string issuerCN;
    int64_t notBefore = 0;
    int64_t notAfter = 0;
    bool isCa = false;
    bool hasBasicConstraints = false;
    int64_t pathLen = -1;         // -1 = 未限制
    bool keyCertSign = false;
    bool hasKeyUsage = false;
    std::vector<std::string> sanDns;

    enum class KeyType { None, Rsa, EcP256, EcP384, EcP521 };
    KeyType keyType = KeyType::None;
    Bytes rsaN;
    Bytes rsaE;
    Bytes ecPoint;                // 未压缩点 0x04||X||Y（P-256 65、P-384 97、P-521 133 字节）
    size_t rsaBits = 0;

    Bytes signature;              // signatureValue 的 BIT STRING 内容（已去掉未使用位数）
    // RSASSA-PSS 参数（仅当 sigAlgOid 是 PSS 时有意义）
    size_t pssSaltLen = 20;
    RsaHash pssHash = RsaHash::Sha256;
    RsaHash pssMgf = RsaHash::Sha256;
};

// 解析失败的原因（用于把"实现了但没支持的算法"与"DER 坏了"分开报告）
enum class X509ParseIssue {
    Ok,
    Malformed,               // DER 结构问题
    UnsupportedKeyAlgorithm, // 公钥算法不认识（例如 SPKI 里的 id-RSASSA-PSS）
    UnsupportedCurve,        // EC 曲线不是 prime256v1（例如 P-384/P-521）
};

// 解析单个证书；失败返回 false（不抛异常、不越界）。
bool x509Parse(const Bytes& der, X509Certificate& out, X509ParseIssue* issue = nullptr);

enum class X509Fail {
    None,
    MalformedLeaf,
    MalformedChain,
    UnsupportedAlgorithm,
    WeakKey,
    Expired,
    NotYetValid,
    HostnameMismatch,
    NoTrustAnchor,
    SignatureInvalid,
    IssuerNotCa,
    KeyUsageViolation,
    PathLenExceeded,
    DepthExceeded,
};

const char* x509FailName(X509Fail f);

struct X509VerifyResult {
    bool ok = false;
    X509Fail reason = X509Fail::None;
    size_t chainLength = 0;
    std::string detail;
};

// 校验证书链：leaf 是否由 intermediates 之一链到 roots 之一，且在 nowUnix 时刻有效、
// 主机名（非空时）匹配。roots 由调用方提供（生产用系统信任库，测试用固定夹具）。
X509VerifyResult x509VerifyChain(const Bytes& leafDer, const std::vector<Bytes>& intermediates,
                                 const std::vector<Bytes>& roots, const std::string& hostname, int64_t nowUnix);

} // namespace jc2::crypto
