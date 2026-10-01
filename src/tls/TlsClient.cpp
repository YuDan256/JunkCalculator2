#include "TlsClient.h"

#include "../crypto/AesGcm.h"
#include "../crypto/ConstTime.h"
#include "../crypto/Der.h"
#include "../crypto/Hkdf.h"
#include "../crypto/Hmac.h"
#include "../crypto/Curve.h"
#include "../crypto/P256.h"
#include "../crypto/Random.h"
#include "../crypto/Rsa.h"
#include "../crypto/Sha256.h"
#include "../crypto/Sha512.h"
#include "../crypto/TrustStore.h"
#include "../crypto/X25519.h"
#include "../crypto/X509.h"

#include <cstring>
#include <ctime>

namespace jc2::tls {

using namespace jc2::crypto;

namespace {

constexpr uint8_t kRecChangeCipherSpec = 20;
constexpr uint8_t kRecAlert = 21;
constexpr uint8_t kRecHandshake = 22;
constexpr uint8_t kRecApplicationData = 23;

constexpr uint8_t kHsClientHello = 1;
constexpr uint8_t kHsServerHello = 2;
constexpr uint8_t kHsNewSessionTicket = 4;
constexpr uint8_t kHsEncryptedExtensions = 8;
constexpr uint8_t kHsCertificate = 11;
constexpr uint8_t kHsCertificateVerify = 15;
constexpr uint8_t kHsFinished = 20;

constexpr uint16_t kCipherAes128GcmSha256 = 0x1301;
constexpr uint16_t kGroupX25519 = 0x001d;
constexpr uint16_t kGroupSecp256r1 = 0x0017;

constexpr uint16_t kSigRsaPkcs1Sha256 = 0x0401;
constexpr uint16_t kSigRsaPkcs1Sha384 = 0x0501;
constexpr uint16_t kSigRsaPkcs1Sha512 = 0x0601;
constexpr uint16_t kSigEcdsaP256Sha256 = 0x0403;
constexpr uint16_t kSigEcdsaP384Sha384 = 0x0503;
constexpr uint16_t kSigEcdsaP521Sha512 = 0x0603;
constexpr uint16_t kSigRsaPssRsaeSha256 = 0x0804;
constexpr uint16_t kSigRsaPssRsaeSha384 = 0x0805;
constexpr uint16_t kSigRsaPssRsaeSha512 = 0x0806;

constexpr size_t kMaxCiphertext = 16384 + 256;

// HelloRetryRequest 的 random 是 SHA-256("HelloRetryRequest")
const uint8_t kHrrMagic[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
};

void put16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xff));
}

void put24(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>(v & 0xff));
}

void putBytes(Bytes& b, const Bytes& v) {
    b.insert(b.end(), v.begin(), v.end());
}

void putBlock16(Bytes& b, const Bytes& v) {
    put16(b, static_cast<uint16_t>(v.size()));
    putBytes(b, v);
}

void putBlock8(Bytes& b, const Bytes& v) {
    b.push_back(static_cast<uint8_t>(v.size()));
    putBytes(b, v);
}

// 越界即置 ok = false 的读取器
struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t pos = 0;
    bool ok = true;

    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}

    bool need(size_t k) {
        if (!ok || pos + k > n) {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t u8() {
        if (!need(1)) return 0;
        return p[pos++];
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(p[pos]) << 8) | p[pos + 1]);
        pos += 2;
        return v;
    }
    uint32_t u24() {
        if (!need(3)) return 0;
        const uint32_t v = (static_cast<uint32_t>(p[pos]) << 16) | (static_cast<uint32_t>(p[pos + 1]) << 8) | p[pos + 2];
        pos += 3;
        return v;
    }
    Bytes bytes(size_t k) {
        if (!need(k)) return Bytes();
        Bytes out(p + pos, p + pos + k);
        pos += k;
        return out;
    }
    // 取走 k 字节作为一个子读取器；越界时返回 ok=false 的读取器
    Reader sub(size_t k) {
        if (!need(k)) {
            Reader bad(nullptr, 0);
            bad.ok = false;
            return bad;
        }
        Reader r(p + pos, k);
        pos += k;
        return r;
    }
    size_t remaining() const { return ok ? n - pos : 0; }
};

Bytes expandLabel(const Bytes& secret, const char* label, const Bytes& context, size_t length) {
    Bytes out(length);
    hkdfExpandLabel(secret.data(), label, context.empty() ? nullptr : context.data(), context.size(), out.data(),
                    out.size());
    return out;
}

} // namespace

const char* tlsFailName(TlsFail f) {
    switch (f) {
        case TlsFail::None: return "ok";
        case TlsFail::RecordTooLarge: return "record-too-large";
        case TlsFail::RecordMalformed: return "record-malformed";
        case TlsFail::UnexpectedMessage: return "unexpected-message";
        case TlsFail::UnsupportedCipherSuite: return "unsupported-cipher-suite";
        case TlsFail::UnsupportedGroup: return "unsupported-group";
        case TlsFail::MissingExtension: return "missing-extension";
        case TlsFail::DecryptFailed: return "decrypt-failed";
        case TlsFail::BadCertificateVerify: return "bad-certificate-verify";
        case TlsFail::BadFinished: return "bad-finished";
        case TlsFail::CertificateRejected: return "certificate-rejected";
        case TlsFail::HrrNotSupported: return "hrr-not-supported";
        case TlsFail::AlertReceived: return "alert-received";
        case TlsFail::ConnectionClosed: return "connection-closed";
        case TlsFail::Internal: return "internal";
    }
    return "unknown";
}

const char* tlsAlertName(uint8_t d) {
    switch (d) {
        case 0: return "close_notify";
        case 10: return "unexpected_message";
        case 20: return "bad_record_mac";
        case 22: return "record_overflow";
        case 40: return "handshake_failure";
        case 42: return "bad_certificate";
        case 43: return "unsupported_certificate";
        case 44: return "certificate_revoked";
        case 45: return "certificate_expired";
        case 46: return "certificate_unknown";
        case 47: return "illegal_parameter";
        case 48: return "unknown_ca";
        case 49: return "access_denied";
        case 50: return "decode_error";
        case 51: return "decrypt_error";
        case 70: return "protocol_version";
        case 71: return "insufficient_security";
        case 80: return "internal_error";
        case 109: return "missing_extension";
        case 110: return "unsupported_extension";
        case 112: return "unrecognized_name";
        case 116: return "certificate_required";
        case 120: return "no_application_protocol";
        default: return "unknown_alert";
    }
}

TlsClient::TlsClient(TlsConfig config) : cfg_(std::move(config)) {}

void TlsClient::fail(TlsFail reason, const std::string& detail) {
    if (fail_ == TlsFail::None) {
        fail_ = reason;
        failDetail_ = detail;
        state_ = State::Failed;
    }
}

Bytes TlsClient::start(std::string* err) {
    if (state_ != State::Fresh) {
        fail(TlsFail::Internal, "start() called twice");
        if (err) *err = failDetail_;
        return Bytes();
    }

    prepareKeys();
    if (!cfg_.clientHelloOverride.empty()) {
        // 回放模式：采用调用方给的 ClientHello，并从它取出会话 id / 随机数，
        // 这样 ServerHello 的 echo 校验仍然有意义
        clientHello_ = cfg_.clientHelloOverride;
        if (clientHello_.size() >= 6 + 32 + 1) {
            const uint8_t* p = clientHello_.data() + 6;   // 跳过类型/长度/legacy_version
            clientRandom_.assign(p, p + 32);
            const size_t sidLen = p[32];
            if (6 + 33 + sidLen <= clientHello_.size()) {
                legacySessionId_.assign(p + 33, p + 33 + sidLen);
            }
        }
    } else {
        buildClientHello();
    }
    transcript_ = clientHello_;
    state_ = State::WaitServerHello;

    Bytes record;
    record.push_back(kRecHandshake);
    // 首个 ClientHello 记录的 legacy_record_version 用 0x0301（RFC 8446 §5.1 允许，
    // 也是各家客户端的兼容做法；其余记录一律 0x0303）
    record.push_back(0x03);
    record.push_back(0x01);
    put16(record, static_cast<uint16_t>(clientHello_.size()));
    putBytes(record, clientHello_);
    return record;
}

// 随机数与两套 ECDHE 密钥（回放模式下也必须有：ServerHello 处理要用私钥）
void TlsClient::prepareKeys() {
    if (cfg_.clientRandom.size() == 32) {
        clientRandom_ = cfg_.clientRandom;
    } else {
        clientRandom_.resize(32);
        randomBytes(clientRandom_.data(), 32);
    }
    if (cfg_.legacySessionId.size() == 32) {
        legacySessionId_ = cfg_.legacySessionId;
    } else {
        legacySessionId_.resize(32);
        randomBytes(legacySessionId_.data(), 32);
    }

    // 两套密钥交换同时给出（D2：避免 HRR）
    x25519Priv_ = cfg_.x25519Private.size() == 32 ? cfg_.x25519Private : Bytes(32);
    if (cfg_.x25519Private.size() != 32) randomBytes(x25519Priv_.data(), 32);
    x25519Pub_.resize(32);
    {
        uint8_t base[32] = {0};
        base[0] = 9;
        x25519(x25519Pub_.data(), x25519Priv_.data(), base);
    }

    p256Priv_ = cfg_.p256Private.size() == 32 ? cfg_.p256Private : Bytes(32);
    if (cfg_.p256Private.size() != 32) randomBytes(p256Priv_.data(), 32);
    p256Pub_.resize(65);
    if (!p256BasePointMul(p256Priv_, p256Pub_.data())) {
        p256Pub_.clear();   // 极少数非法私钥：退化为只提供 x25519
    }

}

void TlsClient::buildClientHello() {
    Bytes ext;

    if (!cfg_.serverName.empty()) {   // server_name (0x0000)
        Bytes name(cfg_.serverName.begin(), cfg_.serverName.end());
        Bytes e;
        put16(e, static_cast<uint16_t>(1 + 2 + name.size()));   // server_name_list 长度
        e.push_back(0);                                          // host_name
        putBlock16(e, name);
        put16(ext, 0x0000);
        putBlock16(ext, e);
    }

    {   // supported_groups (0x000a)
        Bytes e;
        put16(e, static_cast<uint16_t>(p256Pub_.empty() ? 2 : 4));
        put16(e, kGroupX25519);
        if (!p256Pub_.empty()) put16(e, kGroupSecp256r1);
        put16(ext, 0x000a);
        putBlock16(ext, e);
    }

    {   // signature_algorithms (0x000d)
        Bytes e;
        put16(e, 18);
        put16(e, kSigEcdsaP256Sha256);
        put16(e, kSigEcdsaP384Sha384);
        put16(e, kSigEcdsaP521Sha512);
        put16(e, kSigRsaPssRsaeSha256);
        put16(e, kSigRsaPssRsaeSha384);
        put16(e, kSigRsaPssRsaeSha512);
        put16(e, kSigRsaPkcs1Sha256);
        put16(e, kSigRsaPkcs1Sha384);
        put16(e, kSigRsaPkcs1Sha512);
        put16(ext, 0x000d);
        putBlock16(ext, e);
    }

    if (!cfg_.alpn.empty()) {   // application_layer_protocol_negotiation (0x0010)
        Bytes list;
        for (const auto& proto : cfg_.alpn) {
            putBlock8(list, Bytes(proto.begin(), proto.end()));
        }
        Bytes e;
        putBlock16(e, list);
        put16(ext, 0x0010);
        putBlock16(ext, e);
    }

    {   // key_share (0x0033)
        Bytes shares;
        put16(shares, kGroupX25519);
        putBlock16(shares, x25519Pub_);
        if (!p256Pub_.empty()) {
            put16(shares, kGroupSecp256r1);
            putBlock16(shares, p256Pub_);
        }
        Bytes e;
        putBlock16(e, shares);
        put16(ext, 0x0033);
        putBlock16(ext, e);
    }

    {   // supported_versions (0x002b)
        Bytes e;
        e.push_back(2);
        put16(e, 0x0304);
        put16(ext, 0x002b);
        putBlock16(ext, e);
    }

    Bytes body;
    put16(body, 0x0303);
    putBytes(body, clientRandom_);
    putBlock8(body, legacySessionId_);
    put16(body, 2);
    put16(body, kCipherAes128GcmSha256);
    body.push_back(1);
    body.push_back(0);
    putBlock16(body, ext);

    clientHello_.clear();
    clientHello_.push_back(kHsClientHello);
    put24(clientHello_, static_cast<uint32_t>(body.size()));
    putBytes(clientHello_, body);
}

Bytes TlsClient::sealRecord(uint8_t type, const Bytes& payload) {
    Bytes rec;
    rec.push_back(type);
    rec.push_back(0x03);
    rec.push_back(0x03);

    if (!sendEncrypted_) {
        put16(rec, static_cast<uint16_t>(payload.size()));
        putBytes(rec, payload);
        return rec;
    }

    Bytes inner = payload;
    inner.push_back(type);   // 内层真实类型

    Bytes nonce = sendIv_;
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] ^= static_cast<uint8_t>((sendSeq_ >> (56 - 8 * i)) & 0xff);
    }

    // RFC 8446 §5.2：受保护记录的 opaque_type 恒为 application_data (23)，
    // 真实类型只出现在内层明文里（此前误用了内层类型，真实服务器直接回 decode_error）。
    Bytes header;
    header.push_back(kRecApplicationData);
    header.push_back(0x03);
    header.push_back(0x03);
    put16(header, static_cast<uint16_t>(inner.size() + 16));

    Bytes ct(inner.size());
    uint8_t tag[16];
    aes128GcmSeal(sendKey_.data(), nonce.data(), nonce.size(), header.data(), header.size(), inner.data(), inner.size(),
                  ct.data(), tag);

    rec = header;
    putBytes(rec, ct);
    rec.insert(rec.end(), tag, tag + 16);
    ++sendSeq_;
    return rec;
}

Bytes TlsClient::feed(const uint8_t* data, size_t len, std::string* err) {
    Bytes out;
    if (state_ == State::Failed) {
        if (err) *err = failDetail_;
        return out;
    }
    if (len > 0) {
        recvBuffer_.insert(recvBuffer_.end(), data, data + len);
    }

    size_t consumed = 0;
    while (recvBuffer_.size() - consumed >= 5) {
        const uint8_t* h = recvBuffer_.data() + consumed;
        const uint8_t type = h[0];
        const size_t recLen = (static_cast<size_t>(h[3]) << 8) | h[4];
        if (recLen > kMaxCiphertext) {
            fail(TlsFail::RecordTooLarge, "record longer than 2^14 + 256");
            break;
        }
        if (recvBuffer_.size() - consumed - 5 < recLen) break;   // 半条记录

        Bytes payload(recvBuffer_.begin() + static_cast<std::ptrdiff_t>(consumed + 5),
                      recvBuffer_.begin() + static_cast<std::ptrdiff_t>(consumed + 5 + recLen));
        Bytes header(recvBuffer_.begin() + static_cast<std::ptrdiff_t>(consumed),
                     recvBuffer_.begin() + static_cast<std::ptrdiff_t>(consumed + 5));
        consumed += 5 + recLen;

        // 兼容模式的 ChangeCipherSpec：明文、忽略、不进转录
        if (type == kRecChangeCipherSpec) continue;

        if (recvEncrypted_) {
            if (payload.size() < 16) {
                fail(TlsFail::RecordMalformed, "protected record shorter than the tag");
                break;
            }
            Bytes nonce = recvIv_;
            for (int i = 0; i < 8; ++i) {
                nonce[4 + i] ^= static_cast<uint8_t>((recvSeq_ >> (56 - 8 * i)) & 0xff);
            }
            const size_t ctLen = payload.size() - 16;
            Bytes pt(ctLen);
            if (!aes128GcmOpen(recvKey_.data(), nonce.data(), nonce.size(), header.data(), header.size(), payload.data(),
                               ctLen, payload.data() + ctLen, pt.data())) {
                fail(TlsFail::DecryptFailed, "AEAD authentication failed on a protected record");
                break;
            }
            ++recvSeq_;

            size_t end = pt.size();
            while (end > 0 && pt[end - 1] == 0) --end;
            if (end == 0) {
                fail(TlsFail::RecordMalformed, "empty inner plaintext");
                break;
            }
            const uint8_t innerType = pt[end - 1];
            Bytes inner(pt.begin(), pt.begin() + static_cast<std::ptrdiff_t>(end - 1));
            if (innerType != kRecHandshake && innerType != kRecAlert && innerType != kRecApplicationData) {
                fail(TlsFail::RecordMalformed, "unexpected inner content type " + std::to_string(innerType));
                break;
            }
            handleRecord(innerType, inner, out);
        } else {
            handleRecord(type, payload, out);
        }

        if (state_ == State::Failed) break;
    }

    if (consumed > 0) {
        recvBuffer_.erase(recvBuffer_.begin(), recvBuffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    if (state_ == State::Failed && err) *err = failDetail_;
    return out;
}

void TlsClient::handleRecord(uint8_t type, const Bytes& payload, Bytes& out) {
    if (type == kRecAlert) {
        onAlert(payload);
        return;
    }
    if (type == kRecApplicationData) {
        appData_.insert(appData_.end(), payload.begin(), payload.end());
        return;
    }
    if (type != kRecHandshake) {
        fail(TlsFail::UnexpectedMessage, "unexpected record type " + std::to_string(type));
        return;
    }

    hsBuffer_.insert(hsBuffer_.end(), payload.begin(), payload.end());
    size_t off = 0;
    while (hsBuffer_.size() - off >= 4) {
        const uint8_t* p = hsBuffer_.data() + off;
        const size_t msgLen = (static_cast<size_t>(p[1]) << 16) | (static_cast<size_t>(p[2]) << 8) | p[3];
        if (hsBuffer_.size() - off - 4 < msgLen) break;
        Bytes msg(hsBuffer_.begin() + static_cast<std::ptrdiff_t>(off),
                  hsBuffer_.begin() + static_cast<std::ptrdiff_t>(off + 4 + msgLen));
        off += 4 + msgLen;
        handleHandshake(msg, out);
        if (state_ == State::Failed) break;
    }
    if (off > 0) {
        hsBuffer_.erase(hsBuffer_.begin(), hsBuffer_.begin() + static_cast<std::ptrdiff_t>(off));
    }
}

void TlsClient::onAlert(const Bytes& payload) {
    if (payload.size() < 2) {
        fail(TlsFail::RecordMalformed, "alert shorter than 2 bytes");
        return;
    }
    const uint8_t level = payload[0];
    const uint8_t desc = payload[1];
    if (desc == 0) {
        peerClosed_ = true;   // close_notify：正常关闭
        return;
    }
    fail(TlsFail::AlertReceived, std::string("peer sent ") + (level == 1 ? "warning" : "fatal") +
                                     " alert: " + tlsAlertName(desc));
}

void TlsClient::handleHandshake(const Bytes& msg, Bytes& out) {
    const uint8_t type = msg[0];
    switch (type) {
        case kHsServerHello: onServerHello(msg, out); break;
        case kHsEncryptedExtensions: onEncryptedExtensions(msg); break;
        case kHsCertificate: onCertificate(msg); break;
        case kHsCertificateVerify: onCertificateVerify(msg); break;
        case kHsFinished: onFinished(msg, out); break;
        case kHsNewSessionTicket: return;   // 握手后的消息不进转录
        default:
            fail(TlsFail::UnexpectedMessage, "unexpected handshake message type " + std::to_string(type));
            return;
    }
}

void TlsClient::onServerHello(const Bytes& msg, Bytes& /*out*/) {
    const Bytes body(msg.begin() + 4, msg.end());
    Reader r(body.data(), body.size());
    (void)r.u16();
    const Bytes random = r.bytes(32);
    const Bytes sessionId = r.bytes(r.u8());
    const uint16_t suite = r.u16();
    (void)r.u8();
    const size_t extLen = r.u16();
    Reader ext = r.sub(extLen);
    if (!r.ok || !ext.ok) {
        fail(TlsFail::RecordMalformed, "malformed ServerHello");
        return;
    }

    if (random.size() == 32 && std::memcmp(random.data(), kHrrMagic, 32) == 0) {
        fail(TlsFail::HrrNotSupported,
             "server requested HelloRetryRequest; this client already offers x25519 and secp256r1");
        return;
    }
    if (!legacySessionId_.empty() && sessionId != legacySessionId_) {
        fail(TlsFail::UnexpectedMessage, "ServerHello did not echo our legacy_session_id");
        return;
    }
    if (suite != kCipherAes128GcmSha256) {
        fail(TlsFail::UnsupportedCipherSuite, "server selected cipher suite " + std::to_string(suite));
        return;
    }

    bool sawVersion = false;
    Bytes serverKey;
    while (ext.remaining() >= 4) {
        const uint16_t et = ext.u16();
        const size_t el = ext.u16();
        Reader e = ext.sub(el);
        if (!e.ok) break;
        if (et == 0x002b) {
            const uint16_t v = e.u16();
            if (v != 0x0304) {
                fail(TlsFail::UnsupportedCipherSuite, "server negotiated protocol version " + std::to_string(v));
                return;
            }
            sawVersion = true;
        } else if (et == 0x0033) {
            selectedGroup_ = e.u16();
            serverKey = e.bytes(e.u16());
        }
    }
    if (!sawVersion) {
        fail(TlsFail::MissingExtension, "ServerHello without supported_versions (not TLS 1.3)");
        return;
    }
    if (selectedGroup_ != kGroupX25519 && selectedGroup_ != kGroupSecp256r1) {
        fail(TlsFail::UnsupportedGroup, "server selected group " + std::to_string(selectedGroup_));
        return;
    }

    // 转录到 ServerHello 为止，再派生握手密钥
    transcript_.insert(transcript_.end(), msg.begin(), msg.end());

    Bytes shared(32);
    if (selectedGroup_ == kGroupX25519) {
        if (serverKey.size() != 32) {
            fail(TlsFail::RecordMalformed, "x25519 key_share must be 32 bytes");
            return;
        }
        if (!x25519(shared.data(), x25519Priv_.data(), serverKey.data())) {
            fail(TlsFail::RecordMalformed, "x25519 produced the all-zero shared secret");
            return;
        }
    } else {
        if (!p256Ecdh(p256Priv_, serverKey.data(), serverKey.size(), shared.data())) {
            fail(TlsFail::RecordMalformed, "invalid P-256 key_share");
            return;
        }
    }

    deriveHandshakeSecrets(shared);
    installHandshakeKeys();
    recvEncrypted_ = true;
    state_ = State::WaitEncryptedExtensions;
}

void TlsClient::onEncryptedExtensions(const Bytes& msg) {
    transcript_.insert(transcript_.end(), msg.begin(), msg.end());
    state_ = State::WaitCertificate;
}

void TlsClient::onCertificate(const Bytes& msg) {
    transcript_.insert(transcript_.end(), msg.begin(), msg.end());

    const Bytes body(msg.begin() + 4, msg.end());
    Reader r(body.data(), body.size());
    const size_t ctxLen = r.u8();
    (void)r.bytes(ctxLen);
    const size_t listLen = r.u24();
    Reader list = r.sub(listLen);
    if (!r.ok || !list.ok) {
        fail(TlsFail::RecordMalformed, "malformed Certificate message");
        return;
    }
    chainCerts_.clear();
    leafCert_.clear();
    while (list.remaining() >= 3) {
        const size_t certLen = list.u24();
        Bytes der = list.bytes(certLen);
        const size_t extLen = list.u16();
        (void)list.bytes(extLen);
        if (!list.ok) break;
        if (chainCerts_.empty()) leafCert_ = der;
        chainCerts_.push_back(der);
    }
    if (leafCert_.empty()) {
        fail(TlsFail::CertificateRejected, "server sent an empty certificate list");
        return;
    }
    state_ = State::WaitCertificateVerify;
}

void TlsClient::onCertificateVerify(const Bytes& msg) {
    // 签名输入覆盖到 Certificate 为止（不含本条消息）
    Bytes th(32);
    sha256(transcript_.data(), transcript_.size(), th.data());

    const Bytes body(msg.begin() + 4, msg.end());
    Reader r(body.data(), body.size());
    const uint16_t scheme = r.u16();
    const Bytes sig = r.bytes(r.u16());
    if (!r.ok) {
        fail(TlsFail::RecordMalformed, "malformed CertificateVerify");
        return;
    }

    static const char kCtx[] = "TLS 1.3, server CertificateVerify";
    Bytes input(64, 0x20);
    input.insert(input.end(), kCtx, kCtx + sizeof(kCtx) - 1);
    input.push_back(0x00);
    putBytes(input, th);
    // 摘要由签名方案决定（SHA-256 / 384 / 512）
    RsaHash hash = RsaHash::Sha256;
    if (scheme == kSigRsaPssRsaeSha384 || scheme == kSigRsaPkcs1Sha384 || scheme == kSigEcdsaP384Sha384) {
        hash = RsaHash::Sha384;
    } else if (scheme == kSigRsaPssRsaeSha512 || scheme == kSigRsaPkcs1Sha512 || scheme == kSigEcdsaP521Sha512) {
        hash = RsaHash::Sha512;
    }
    uint8_t digest[64];
    switch (hash) {
        case RsaHash::Sha384: sha384(input.data(), input.size(), digest); break;
        case RsaHash::Sha512: sha512(input.data(), input.size(), digest); break;
        case RsaHash::Sha256:
        default: sha256(input.data(), input.size(), digest); break;
    }

    X509Certificate leaf;
    if (!x509Parse(leafCert_, leaf)) {
        fail(TlsFail::CertificateRejected, "leaf certificate could not be parsed");
        return;
    }

    bool ok = false;
    if (scheme == kSigRsaPssRsaeSha256 || scheme == kSigRsaPssRsaeSha384 || scheme == kSigRsaPssRsaeSha512) {
        ok = leaf.keyType == X509Certificate::KeyType::Rsa &&
             rsaVerifyPss(leaf.rsaN, leaf.rsaE, digest, sig, rsaHashLength(hash), hash);
    } else if (scheme == kSigRsaPkcs1Sha256 || scheme == kSigRsaPkcs1Sha384 || scheme == kSigRsaPkcs1Sha512) {
        ok = leaf.keyType == X509Certificate::KeyType::Rsa &&
             rsaVerifyPkcs1(leaf.rsaN, leaf.rsaE, digest, sig, hash);
    } else if (scheme == kSigEcdsaP256Sha256 || scheme == kSigEcdsaP384Sha384 || scheme == kSigEcdsaP521Sha512) {
        const EcCurve want = (scheme == kSigEcdsaP384Sha384) ? EcCurve::P384
                             : (scheme == kSigEcdsaP521Sha512) ? EcCurve::P521
                                                               : EcCurve::P256;
        const X509Certificate::KeyType wantKey = (want == EcCurve::P384) ? X509Certificate::KeyType::EcP384
                                                 : (want == EcCurve::P521) ? X509Certificate::KeyType::EcP521
                                                                           : X509Certificate::KeyType::EcP256;
        if (leaf.keyType == wantKey) {
            const EcGroup& g = ecGroup(want);
            EcPoint pub;
            Bytes rb, sb;
            if (ecDecodePoint(g, leaf.ecPoint.data(), leaf.ecPoint.size(), pub) &&
                ecEcdsaSignatureFromDer(sig, rb, sb)) {
                ok = ecEcdsaVerify(g, pub, digest, rsaHashLength(hash), rb, sb);
            }
        }
    } else {
        fail(TlsFail::UnsupportedCipherSuite, "unsupported CertificateVerify scheme " + std::to_string(scheme));
        return;
    }

    if (!ok) {
        fail(TlsFail::BadCertificateVerify, "CertificateVerify signature did not verify");
        return;
    }
    transcript_.insert(transcript_.end(), msg.begin(), msg.end());

    // 链校验（回放自签证书时可关；CertificateVerify 永远校验，已证明持有私钥）
    if (cfg_.verifyChain) {
        std::vector<Bytes> intermediates(chainCerts_.begin() + 1, chainCerts_.end());
        std::vector<Bytes> roots = cfg_.roots;
        if (roots.empty()) roots = systemRootCertificates();
        const int64_t now = cfg_.verifyTime != 0 ? cfg_.verifyTime : static_cast<int64_t>(std::time(nullptr));
        const X509VerifyResult vr = x509VerifyChain(leafCert_, intermediates, roots, cfg_.serverName, now);
        if (!vr.ok) {
            fail(TlsFail::CertificateRejected, std::string("certificate chain rejected: ") + x509FailName(vr.reason) +
                                                   " (" + vr.detail + ")");
            return;
        }
    }
    state_ = State::WaitFinished;
}

void TlsClient::onFinished(const Bytes& msg, Bytes& out) {
    Bytes th(32);
    sha256(transcript_.data(), transcript_.size(), th.data());

    const Bytes finishedKey = expandLabel(secrets_.serverHandshakeTraffic, "finished", Bytes(), 32);
    uint8_t expected[32];
    hmacSha256(finishedKey.data(), finishedKey.size(), th.data(), th.size(), expected);

    const Bytes body(msg.begin() + 4, msg.end());
    if (body.size() != 32 || !ctEqual(body.data(), expected, 32)) {
        fail(TlsFail::BadFinished, "server Finished verify_data mismatch");
        return;
    }
    secrets_.serverFinishedVerify = body;
    transcript_.insert(transcript_.end(), msg.begin(), msg.end());

    // 应用密钥：转录到服务器 Finished 为止
    deriveApplicationSecrets();

    // 客户端 Finished：仍用握手密钥发送
    const Bytes clientFinishedKey = expandLabel(secrets_.clientHandshakeTraffic, "finished", Bytes(), 32);
    Bytes th2(32);
    sha256(transcript_.data(), transcript_.size(), th2.data());
    uint8_t verify[32];
    hmacSha256(clientFinishedKey.data(), clientFinishedKey.size(), th2.data(), th2.size(), verify);
    secrets_.clientFinishedVerify.assign(verify, verify + 32);

    Bytes msgOut;
    msgOut.push_back(kHsFinished);
    put24(msgOut, 32);
    msgOut.insert(msgOut.end(), verify, verify + 32);
    transcript_.insert(transcript_.end(), msgOut.begin(), msgOut.end());

    const Bytes rec = sealRecord(kRecHandshake, msgOut);
    out.insert(out.end(), rec.begin(), rec.end());

    installApplicationKeys();
    state_ = State::Connected;
}

void TlsClient::deriveHandshakeSecrets(const Bytes& ecdheShared) {
    const Bytes zeros(32, 0);
    Bytes early(32);
    hkdfExtract(nullptr, 0, zeros.data(), zeros.size(), early.data());

    Bytes emptyHash(32);
    sha256(nullptr, 0, emptyHash.data());

    const Bytes derived = expandLabel(early, "derived", emptyHash, 32);
    Bytes handshake(32);
    hkdfExtract(derived.data(), derived.size(), ecdheShared.data(), ecdheShared.size(), handshake.data());
    secrets_.handshakeSecret = handshake;

    Bytes th(32);
    sha256(transcript_.data(), transcript_.size(), th.data());
    secrets_.clientHandshakeTraffic = expandLabel(handshake, "c hs traffic", th, 32);
    secrets_.serverHandshakeTraffic = expandLabel(handshake, "s hs traffic", th, 32);

    const Bytes derived2 = expandLabel(handshake, "derived", emptyHash, 32);
    Bytes master(32);
    hkdfExtract(derived2.data(), derived2.size(), zeros.data(), zeros.size(), master.data());
    secrets_.masterSecret = master;
}

void TlsClient::deriveApplicationSecrets() {
    Bytes th(32);
    sha256(transcript_.data(), transcript_.size(), th.data());
    secrets_.clientApplicationTraffic0 = expandLabel(secrets_.masterSecret, "c ap traffic", th, 32);
    secrets_.serverApplicationTraffic0 = expandLabel(secrets_.masterSecret, "s ap traffic", th, 32);
    secrets_.exporterMaster = expandLabel(secrets_.masterSecret, "exp master", th, 32);
}

void TlsClient::installHandshakeKeys() {
    sendKey_ = expandLabel(secrets_.clientHandshakeTraffic, "key", Bytes(), 16);
    sendIv_ = expandLabel(secrets_.clientHandshakeTraffic, "iv", Bytes(), 12);
    recvKey_ = expandLabel(secrets_.serverHandshakeTraffic, "key", Bytes(), 16);
    recvIv_ = expandLabel(secrets_.serverHandshakeTraffic, "iv", Bytes(), 12);
    sendSeq_ = 0;
    recvSeq_ = 0;
    sendEncrypted_ = true;
}

void TlsClient::installApplicationKeys() {
    recvKey_ = expandLabel(secrets_.serverApplicationTraffic0, "key", Bytes(), 16);
    recvIv_ = expandLabel(secrets_.serverApplicationTraffic0, "iv", Bytes(), 12);
    recvSeq_ = 0;
    sendKey_ = expandLabel(secrets_.clientApplicationTraffic0, "key", Bytes(), 16);
    sendIv_ = expandLabel(secrets_.clientApplicationTraffic0, "iv", Bytes(), 12);
    sendSeq_ = 0;
    applicationKeysInstalled_ = true;
}

Bytes TlsClient::takeApplicationData() {
    Bytes out = std::move(appData_);
    appData_.clear();
    return out;
}

Bytes TlsClient::sealApplicationData(const uint8_t* data, size_t len) {
    if (!applicationKeysInstalled_) {
        fail(TlsFail::Internal, "application data before the handshake finished");
        return Bytes();
    }
    Bytes out;
    size_t off = 0;
    do {
        const size_t take = (len - off < 16384) ? (len - off) : 16384;   // 单条记录上限 2^14
        Bytes payload(data + off, data + off + take);
        const Bytes rec = sealRecord(kRecApplicationData, payload);
        out.insert(out.end(), rec.begin(), rec.end());
        off += take;
    } while (off < len);
    return out;
}

Bytes TlsClient::closeNotify() {
    if (!sendEncrypted_) return Bytes();
    const Bytes alert = {0x01, 0x00};   // warning, close_notify
    return sealRecord(kRecAlert, alert);
}

} // namespace jc2::tls
