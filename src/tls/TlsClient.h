// TLS 1.3 客户端（RFC 8446）：记录层 + 握手状态机 + 密钥调度。
// 传输无关：调用方把 start()/feed() 返回的字节写进 TCP，把读到的字节喂给 feed()。
// 生产接线见 P3（modules/net.jc2 的 TlsSocket + modules/http.jc2）。
#pragma once

#include "../crypto/BytesUtil.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jc2::tls {

using crypto::Bytes;

enum class TlsFail {
    None,
    RecordTooLarge,
    RecordMalformed,
    UnexpectedMessage,
    UnsupportedCipherSuite,
    UnsupportedGroup,
    MissingExtension,
    DecryptFailed,
    BadCertificateVerify,
    BadFinished,
    CertificateRejected,
    HrrNotSupported,
    AlertReceived,
    ConnectionClosed,
    Internal,
};

const char* tlsFailName(TlsFail f);
const char* tlsAlertName(uint8_t description);

struct TlsConfig {
    std::string serverName;                 // SNI，同时作为证书主机名
    bool verifyChain = true;                // 是否校验证书链（CertificateVerify 永远校验）
    std::vector<Bytes> roots;               // 空 = 系统信任库（Windows ROOT）
    int64_t verifyTime = 0;                 // 0 = 当前时间
    std::vector<std::string> alpn{"http/1.1"};

    // ↓ 回放/确定性模式：用于 RFC 8448 轨迹回放测试，正常连接不要设置
    bool deterministic = false;
    Bytes clientRandom;                     // 32 字节
    Bytes legacySessionId;                  // 32 字节
    Bytes x25519Private;                    // 32 字节
    Bytes p256Private;                      // 32 字节
    Bytes clientHelloOverride;              // 完整 ClientHello 握手消息（以 01 开头）
};

// 密钥调度中间值：回放测试拿它与 RFC 8448 的轨迹逐项比对
struct TlsSecrets {
    Bytes handshakeSecret;
    Bytes clientHandshakeTraffic;
    Bytes serverHandshakeTraffic;
    Bytes masterSecret;
    Bytes clientApplicationTraffic0;
    Bytes serverApplicationTraffic0;
    Bytes exporterMaster;
    Bytes serverFinishedVerify;
    Bytes clientFinishedVerify;
};

class TlsClient {
public:
    explicit TlsClient(TlsConfig config);

    // 生成 ClientHello 记录（首次调用）。err 非空时在失败时写入原因。
    Bytes start(std::string* err = nullptr);

    // 喂入服务器字节（允许半条记录）。返回需要发送的字节（可能为空）。
    Bytes feed(const uint8_t* data, size_t len, std::string* err = nullptr);
    Bytes feed(const Bytes& data, std::string* err = nullptr) {
        return feed(data.data(), data.size(), err);
    }

    // 取出已解出的应用数据（每次取走）。
    Bytes takeApplicationData();

    // 交给服务器之后要发送的字节（start / feed 的返回值已经包含，这里用于查询）
    bool handshakeComplete() const { return state_ == State::Connected; }
    bool failed() const { return fail_ != TlsFail::None; }
    TlsFail failReason() const { return fail_; }
    const std::string& failDetail() const { return failDetail_; }
    const TlsSecrets& secrets() const { return secrets_; }
    Bytes serverCertificate() const { return leafCert_; }
    // 本客户端生成的 ClientHello 握手消息（回放模式下即注入的那份）
    const Bytes& clientHelloMessage() const { return clientHello_; }

private:
    enum class State { Fresh, WaitServerHello, WaitEncryptedExtensions, WaitCertificate, WaitCertificateVerify, WaitFinished, Connected, Failed };
    enum class Cipher { None, Aes128GcmSha256 };

    // 记录层
    Bytes sealRecord(uint8_t type, const Bytes& payload);
    void handleRecord(uint8_t type, const Bytes& payload, Bytes& out);

    // 握手：handler 收到**完整握手消息**（含 4 字节头），以便按时点追加转录
    void handleHandshake(const Bytes& msg, Bytes& out);
    void onServerHello(const Bytes& msg, Bytes& out);
    void onEncryptedExtensions(const Bytes& msg);
    void onCertificate(const Bytes& msg);
    void onCertificateVerify(const Bytes& msg);
    void onFinished(const Bytes& msg, Bytes& out);
    void onAlert(const Bytes& payload);
    void prepareKeys();
    void buildClientHello();

    // 密钥调度
    void deriveHandshakeSecrets(const Bytes& ecdheShared);
    void deriveApplicationSecrets();
    void installHandshakeKeys();
    void installApplicationKeys();

    void fail(TlsFail reason, const std::string& detail);

    TlsConfig cfg_;
    State state_ = State::Fresh;
    Cipher cipher_ = Cipher::None;

    Bytes transcript_;              // 握手消息累积（不含记录头）
    Bytes hsBuffer_;                // 跨记录/跨消息的重组缓冲
    Bytes recvBuffer_;              // 原始接收缓冲
    Bytes appData_;                 // 解出的应用数据
    Bytes clientHello_;             // 自己生成的 ClientHello 握手消息
    std::string err_;

    // ECDHE
    Bytes x25519Priv_, x25519Pub_;
    Bytes p256Priv_, p256Pub_;
    Bytes clientRandom_;
    Bytes legacySessionId_;
    uint16_t selectedGroup_ = 0;

    // 记录层计数与密钥
    uint64_t sendSeq_ = 0;
    uint64_t recvSeq_ = 0;
    bool sendEncrypted_ = false;
    bool recvEncrypted_ = false;
    Bytes sendKey_, sendIv_;
    Bytes recvKey_, recvIv_;

    // 应用阶段（握手结束后切换）
    Bytes appSendKey_, appSendIv_;
    Bytes appRecvKey_, appRecvIv_;
    bool applicationKeysInstalled_ = false;

    TlsSecrets secrets_;
    Bytes leafCert_;
    std::vector<Bytes> chainCerts_;

    TlsFail fail_ = TlsFail::None;
    std::string failDetail_;
};

} // namespace jc2::tls
