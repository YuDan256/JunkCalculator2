// tls 原生模块：把 TLS 1.3 客户端暴露给脚本。
//
// 传输无关：start()/feed() 收发的都是**记录字节的十六进制**，由脚本层负责写/读 socket
// （P3 会把 modules/net.jc2 的 TlsSocket 接上去）。这样也能离线回放 RFC 8448 的握手轨迹。
//
// 配置字典（全部可省）：
//   serverName       SNI 与证书主机名
//   verifyChain      bool，默认 true（CertificateVerify 永远校验，与它无关）
//   roots            信任锚列表（DER 的十六进制）；省略则用系统 ROOT 存储
//   time             校验时刻（Unix 秒）；省略则用当前时间
//   alpn             协议名列表，默认 ["http/1.1"]
//   clientRandom / legacySessionId / x25519Private / p256Private / clientHello
//                    确定性注入，仅用于回放测试（正常连接不要设置）

#include "../jc2_extension_cpp.h"

#include "../internal/crypto/util/BytesUtil.h"
#include "../internal/net/TcpStream.h"
#include "../internal/net/tls/TlsClient.h"

#include <cstdint>
#include <string>
#include <vector>

static jc2::Class* g_tlsErrorClass = nullptr;
static jc2::Class* g_tlsClientClass = nullptr;
static jc2::Class* g_tlsSocketClass = nullptr;

[[noreturn]] inline void throwTlsError(const std::string& msg) {
    jc2::throw_error_class(*g_tlsErrorClass, msg);
}

struct ClientWrapper {
    jc2::tls::TlsClient client;
    explicit ClientWrapper(jc2::tls::TlsConfig cfg) : client(std::move(cfg)) {}
};

static std::vector<uint8_t> unhex(const jc2::Value& v, const char* what) {
    if (!v.is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(what) + " must be a hexadecimal string.");
    }
    std::vector<uint8_t> out;
    if (!jc2::crypto::fromHex(v.as_string(), out)) {
        throwTlsError(std::string(what) + " must be an even-length hexadecimal string.");
    }
    return out;
}

static ClientWrapper* getClient(const jc2::Value& v, const char* fn) {
    if (!v.is_instance()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(fn) + " expects a tls.Client.");
    }
    auto* ptr = v.get_native_data<ClientWrapper>();
    if (!ptr) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(fn) + " expects a tls.Client.");
    }
    return ptr;
}

static jc2::Value makeClient(jc2::tls::TlsConfig cfg) {
    jc2::Instance inst(*g_tlsClientClass);
    auto* ptr = new ClientWrapper(std::move(cfg));
    inst.set_native_data(ptr, [](void* p) { delete static_cast<ClientWrapper*>(p); });
    return inst;
}

#define METHOD(name) JC2_ValueHandle tls_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)

// ─── 已连接的 TLS 套接字（tls.connect）─────────────────────────

struct SocketWrapper {
    jc2::net::TcpStream stream;
    jc2::tls::TlsClient client;
    jc2::crypto::Bytes pending;      // 上次 recv 多读出来、但超过调用方 max_bytes 的部分
    int timeoutMs = 30000;
    explicit SocketWrapper(jc2::tls::TlsConfig cfg) : client(std::move(cfg)) {}
};

static SocketWrapper* getSocket(const jc2::Value& v, const char* fn) {
    if (!v.is_instance()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(fn) + " expects a tls.Socket.");
    }
    auto* ptr = v.get_native_data<SocketWrapper>();
    if (!ptr) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(fn) + " expects a tls.Socket.");
    }
    return ptr;
}

static jc2::Value makeSocket(SocketWrapper* w) {
    jc2::Instance inst(*g_tlsSocketClass);
    inst.set_native_data(w, [](void* p) { delete static_cast<SocketWrapper*>(p); });
    return inst;
}

static void sendAll(SocketWrapper& w, const jc2::crypto::Bytes& out) {
    if (out.empty()) return;
    std::string err;
    if (!w.stream.send(out.data(), out.size(), err)) throwTlsError(err);
}

// 读一轮并处理；返回 false 表示对端关闭
static bool pumpIn(SocketWrapper& w) {
    uint8_t buf[8192];
    std::string err;
    const ptrdiff_t n = w.stream.recv(buf, sizeof(buf), err);
    if (n < 0) throwTlsError(err);
    if (n == 0) return false;
    const jc2::crypto::Bytes out = w.client.feed(buf, static_cast<size_t>(n));
    if (w.client.failed()) throwTlsError(w.client.failDetail());
    sendAll(w, out);
    return true;
}

// ─── tls.connect(host, port, options) ─────────────────────────

static JC2_ValueHandle tls_connect(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 2 || argc > 3) {
        jc2::throw_error(jc2::ErrorType::TypeError, "tls.connect expects (host, port[, options]).");
    }
    const std::string host = jc2::Value(argv[0]).as_string();
    const int port = static_cast<int>(jc2::Value(argv[1]).as_float());

    jc2::tls::TlsConfig cfg;
    cfg.serverName = host;
    int timeoutMs = 30000;
    if (argc == 3 && jc2::Value(argv[2]).is_dict()) {
        jc2::Dict d(jc2::Value(argv[2]).get_handle());
        auto get = [&](const char* key) { return d.has(jc2::Value(key)) ? d.get(jc2::Value(key)) : jc2::Value(); };
        jc2::Value v = get("serverName");
        if (v.is_string()) cfg.serverName = v.as_string();
        v = get("verifyChain");
        if (v.is_bool()) cfg.verifyChain = v.as_bool();
        v = get("time");
        if (v.is_int() || v.is_float()) cfg.verifyTime = static_cast<int64_t>(v.as_float());
        v = get("timeoutMs");
        if (v.is_int() || v.is_float()) timeoutMs = static_cast<int>(v.as_float());
        v = get("alpn");
        if (v.is_list()) {
            cfg.alpn.clear();
            jc2::List l(v.get_handle());
            for (size_t i = 0; i < l.size(); ++i) cfg.alpn.push_back(l.get(i).as_string());
        }
        v = get("roots");
        if (v.is_list()) {
            jc2::List l(v.get_handle());
            for (size_t i = 0; i < l.size(); ++i) cfg.roots.push_back(unhex(l.get(i), "roots entry"));
        }
    }

    auto* w = new SocketWrapper(std::move(cfg));
    w->timeoutMs = timeoutMs;
    try {
        std::string err;
        if (!w->stream.connect(host, static_cast<uint16_t>(port), timeoutMs, err)) {
            delete w;
            throwTlsError(err);
        }
        try {
            sendAll(*w, w->client.start());
        } catch (...) {
            delete w;
            throw;
        }
        while (!w->client.handshakeComplete()) {
            uint8_t buf[8192];
            const ptrdiff_t n = w->stream.recv(buf, sizeof(buf), err);
            if (n < 0) {
                delete w;
                throwTlsError(err);
            }
            if (n == 0) {
                delete w;
                throwTlsError("the peer closed the connection during the handshake");
            }
            const jc2::crypto::Bytes out = w->client.feed(buf, static_cast<size_t>(n));
            if (w->client.failed()) {
                const std::string detail = w->client.failDetail();
                delete w;
                throwTlsError(detail.empty() ? "the TLS handshake failed" : detail);
            }
            sendAll(*w, out);
        }
    } catch (...) {
        throw;
    }
    return makeSocket(w).get_handle();
}

// ─── tls.Socket 方法 ─────────────────────────────────────────

METHOD(send) {
    (void)argc;
    auto* w = getSocket(jc2::Value(argv[0]), "send");
    if (!jc2::Value(argv[1]).is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, "send expects a string.");
    }
    const std::string text = jc2::Value(argv[1]).as_string();
    const auto rec = w->client.sealApplicationData(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    std::string err;
    if (!w->stream.send(rec.data(), rec.size(), err)) throwTlsError(err);
    return jc2::Value(static_cast<int32_t>(text.size())).get_handle();
}

METHOD(recv) {
    auto* w = getSocket(jc2::Value(argv[0]), "recv");
    int maxBytes = 16384;
    if (argc >= 2 && (jc2::Value(argv[1]).is_int() || jc2::Value(argv[1]).is_float())) {
        maxBytes = static_cast<int>(jc2::Value(argv[1]).as_float());
    }
    if (maxBytes <= 0) maxBytes = 16384;

    // 先把上次多读出来的部分交出去
    jc2::crypto::Bytes data;
    if (!w->pending.empty()) {
        data.swap(w->pending);
    } else {
        // 一直读到有应用数据为止：期间可能只收到 NewSessionTicket 之类的握手记录。
        // 返回空串表示对端关闭（close_notify 或 TCP 关闭）。
        data = w->client.takeApplicationData();
        while (data.empty()) {
            if (!pumpIn(*w)) break;
            data = w->client.takeApplicationData();
        }
    }
    if (static_cast<int>(data.size()) > maxBytes) {
        // 超出的部分必须留着，不能丢（否则调用方按 max_bytes 循环读时会静默截断）
        w->pending.assign(data.begin() + maxBytes, data.end());
        data.resize(static_cast<size_t>(maxBytes));
    }
    return jc2::Value(std::string(reinterpret_cast<const char*>(data.data()), data.size())).get_handle();
}

METHOD(close) {
    (void)argc;
    auto* w = getSocket(jc2::Value(argv[0]), "close");
    const auto bye = w->client.closeNotify();
    if (!bye.empty()) {
        std::string err;
        w->stream.send(bye.data(), bye.size(), err);   // 尽力而为
    }
    w->stream.close();
    return jc2::Value().get_handle();
}

METHOD(connected) {
    (void)argc;
    auto* w = getSocket(jc2::Value(argv[0]), "connected");
    return jc2::Value(w->stream.connected()).get_handle();
}

METHOD(socketCertificate) {
    (void)argc;
    auto* w = getSocket(jc2::Value(argv[0]), "certificate");
    return jc2::Value(jc2::crypto::toHex(w->client.serverCertificate())).get_handle();
}
// Client(配置字典) —— 构造函数（类的 allocator/构造走 init）
METHOD(init) {
    (void)argc;
    jc2::tls::TlsConfig cfg;
    const bool hasOpts = (argc >= 1 && argv != nullptr);
    const jc2::Value opts = hasOpts ? jc2::Value(argv[0]) : jc2::Value();
    if (hasOpts && opts.is_dict()) {
        jc2::Dict d(opts.get_handle());
        auto get = [&](const char* key) { return d.has(jc2::Value(key)) ? d.get(jc2::Value(key)) : jc2::Value(); };
        jc2::Value v = get("serverName");
        if (v.is_string()) cfg.serverName = v.as_string();
        v = get("verifyChain");
        if (v.is_bool()) cfg.verifyChain = v.as_bool();
        v = get("time");
        if (v.is_int() || v.is_float()) cfg.verifyTime = static_cast<int64_t>(v.as_float());
        v = get("alpn");
        if (v.is_list()) {
            cfg.alpn.clear();
            jc2::List l(v.get_handle());
            for (size_t i = 0; i < l.size(); ++i) cfg.alpn.push_back(l.get(i).as_string());
        }
        v = get("roots");
        if (v.is_list()) {
            jc2::List l(v.get_handle());
            for (size_t i = 0; i < l.size(); ++i) cfg.roots.push_back(unhex(l.get(i), "roots entry"));
        }
        v = get("clientRandom");
        if (v.is_string()) cfg.clientRandom = unhex(v, "clientRandom");
        v = get("legacySessionId");
        if (v.is_string()) cfg.legacySessionId = unhex(v, "legacySessionId");
        v = get("x25519Private");
        if (v.is_string()) cfg.x25519Private = unhex(v, "x25519Private");
        v = get("p256Private");
        if (v.is_string()) cfg.p256Private = unhex(v, "p256Private");
        v = get("clientHello");
        if (v.is_string()) cfg.clientHelloOverride = unhex(v, "clientHello");
        cfg.deterministic = true;
    } else if (!opts.is_none()) {
        jc2::throw_error(jc2::ErrorType::TypeError, "tls.Client expects an options dictionary.");
    }

    jc2::Value result = makeClient(std::move(cfg));
    return result.get_handle();
}

METHOD(start) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "start");
    std::string err;
    const auto out = w->client.start(&err);
    if (w->client.failed()) throwTlsError(err.empty() ? "handshake failed" : err);
    return jc2::Value(jc2::crypto::toHex(out)).get_handle();
}

METHOD(feed) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "feed");
    const auto in = unhex(jc2::Value(argv[1]), "feed");
    std::string err;
    const auto out = w->client.feed(in.data(), in.size(), &err);
    // 失败不抛异常：调用方需要区分"握手失败"与"连接关闭"，用 ok()/reason() 查询
    (void)err;
    return jc2::Value(jc2::crypto::toHex(out)).get_handle();
}

METHOD(data) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "data");
    return jc2::Value(jc2::crypto::toHex(w->client.takeApplicationData())).get_handle();
}

METHOD(complete) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "complete");
    return jc2::Value(w->client.handshakeComplete()).get_handle();
}

METHOD(status) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "status");
    jc2::Dict out;
    out.set(jc2::Value("ok"), jc2::Value(!w->client.failed()));
    out.set(jc2::Value("complete"), jc2::Value(w->client.handshakeComplete()));
    out.set(jc2::Value("reason"), jc2::Value(std::string(jc2::tls::tlsFailName(w->client.failReason()))));
    out.set(jc2::Value("detail"), jc2::Value(w->client.failDetail()));
    return out.get_handle();
}

METHOD(secrets) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "secrets");
    const auto& s = w->client.secrets();
    jc2::Dict out;
    auto put = [&](const char* k, const jc2::crypto::Bytes& b) {
        out.set(jc2::Value(k), jc2::Value(jc2::crypto::toHex(b)));
    };
    put("handshakeSecret", s.handshakeSecret);
    put("clientHandshakeTraffic", s.clientHandshakeTraffic);
    put("serverHandshakeTraffic", s.serverHandshakeTraffic);
    put("masterSecret", s.masterSecret);
    put("clientApplicationTraffic0", s.clientApplicationTraffic0);
    put("serverApplicationTraffic0", s.serverApplicationTraffic0);
    put("exporterMaster", s.exporterMaster);
    put("serverFinishedVerify", s.serverFinishedVerify);
    put("clientFinishedVerify", s.clientFinishedVerify);
    return out.get_handle();
}

METHOD(hello) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "hello");
    return jc2::Value(jc2::crypto::toHex(w->client.clientHelloMessage())).get_handle();
}

METHOD(certificate) {
    (void)argc;
    auto* w = getClient(jc2::Value(argv[0]), "certificate");
    return jc2::Value(jc2::crypto::toHex(w->client.serverCertificate())).get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_tlsErrorClass = new jc2::Class("TlsError");
    jc2::Value exceptionCls = jc2::get_global("Exception");
    g_tlsErrorClass->set_parent(jc2::Class(exceptionCls.get_handle()));
    mod.register_value("TlsError", *g_tlsErrorClass);

    g_tlsClientClass = new jc2::Class("Client");
    mod.register_value("Client", *g_tlsClientClass);
    g_tlsClientClass->set_allocator(tls_init);

    g_tlsSocketClass = new jc2::Class("Socket");
    mod.register_value("Socket", *g_tlsSocketClass);
    g_tlsSocketClass->bind_method("send", tls_send, 1, 1, {"text"});
    g_tlsSocketClass->bind_method("recv", tls_recv, 0, 1, {"max_bytes"});
    g_tlsSocketClass->bind_method("close", tls_close, 0, 0);
    g_tlsSocketClass->bind_method("connected", tls_connected, 0, 0);
    g_tlsSocketClass->bind_method("certificate", tls_socketCertificate, 0, 0);

    mod.register_function("connect", tls_connect, 2, 3, {"host", "port", "options"});

    g_tlsClientClass->bind_method("start", tls_start, 0, 0);
    g_tlsClientClass->bind_method("feed", tls_feed, 1, 1, {"bytes"});
    g_tlsClientClass->bind_method("data", tls_data, 0, 0);
    g_tlsClientClass->bind_method("complete", tls_complete, 0, 0);
    g_tlsClientClass->bind_method("status", tls_status, 0, 0);
    g_tlsClientClass->bind_method("secrets", tls_secrets, 0, 0);
    g_tlsClientClass->bind_method("hello", tls_hello, 0, 0);
    g_tlsClientClass->bind_method("certificate", tls_certificate, 0, 0);

    mod.register_function_help("tls.connect", "tls.connect(host, port, options)",
        "Dials TCP, performs the TLS 1.3 handshake (certificate chain verified by default) and returns a tls.Socket.",
        "s = tls.connect(\"example.com\", 443)");
    mod.register_function_help("tls.Socket.send", "s.send(text)", "Sends application data over TLS.", "");
    mod.register_function_help("tls.Socket.recv", "s.recv(max_bytes)", "Reads and decrypts application data; empty string means the peer closed.", "");
    mod.register_function_help("tls.Socket.close", "s.close()", "Sends close_notify and closes the socket.", "");
    mod.register_function_help("tls.Socket.certificate", "s.certificate()", "Leaf certificate DER of the peer as hex.", "");
    mod.register_function_help("tls.Client", "tls.Client(options)", "Creates a TLS 1.3 client; feeds and returns record bytes as hex.", "c = tls.Client({\"serverName\": \"example.com\"})");
    mod.register_function_help("tls.Client.start", "c.start()", "Returns the ClientHello record as hex.", "");
    mod.register_function_help("tls.Client.feed", "c.feed(bytes)", "Feeds server records (hex) and returns the bytes to send back.", "");
    mod.register_function_help("tls.Client.data", "c.data()", "Returns (and clears) decrypted application data as hex.", "");
    mod.register_function_help("tls.Client.status", "c.status()", "Returns {ok, complete, reason, detail}.", "");
    mod.register_function_help("tls.Client.secrets", "c.secrets()", "Handshake key-schedule values (hex); meant for tests and diagnostics.", "");
    mod.register_function_help("tls.Client.certificate", "c.certificate()", "Leaf certificate DER of the server as hex.", "");

    mod.register_help("tls",
        "═══ TLS 1.3 Client (Native Module) ═══\n\n"
        "  Requires: import tls\n\n"
        "  Transport-agnostic: you hand it record bytes from the socket and send back what it returns.\n"
        "  Everything binary is a hexadecimal string, in and out.\n\n"
        "    s = tls.connect(\"example.com\", 443)   // 一条龙：TCP + 握手 + 证书链校验\n"
        "    s.send(\"GET / HTTP/1.1\\r\\nHost: example.com\\r\\nConnection: close\\r\\n\\r\\n\")\n"
        "    while (true) { chunk = s.recv(16384); if (chunk == \"\") break; print(chunk) }\n\n"
        "    c = tls.Client({\"serverName\": \"example.com\"})\n"
        "    c.start()        → the ClientHello record (hex) to write to the socket\n"
        "    c.feed(bytes)    → feed server records, returns the bytes to send back\n"
        "    c.data()         → decrypted application data\n"
        "    c.status()       → {ok, complete, reason, detail}\n\n"
        "  Options: serverName, verifyChain (default true), roots (trust anchors as hex DER),\n"
        "           time (verification instant), alpn. The deterministic-injection options\n"
        "           (clientRandom / x25519Private / clientHello ...) exist for replaying recorded\n"
        "           handshakes in tests and must not be used for real connections.\n"
    );
    return 0;
}

JC2_EXTENSION_INIT
