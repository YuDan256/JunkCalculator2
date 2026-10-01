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

#include "../../crypto/BytesUtil.h"
#include "../../tls/TlsClient.h"

#include <cstdint>
#include <string>
#include <vector>

static jc2::Class* g_tlsErrorClass = nullptr;
static jc2::Class* g_tlsClientClass = nullptr;

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

    g_tlsClientClass->bind_method("start", tls_start, 0, 0);
    g_tlsClientClass->bind_method("feed", tls_feed, 1, 1, {"bytes"});
    g_tlsClientClass->bind_method("data", tls_data, 0, 0);
    g_tlsClientClass->bind_method("complete", tls_complete, 0, 0);
    g_tlsClientClass->bind_method("status", tls_status, 0, 0);
    g_tlsClientClass->bind_method("secrets", tls_secrets, 0, 0);
    g_tlsClientClass->bind_method("hello", tls_hello, 0, 0);
    g_tlsClientClass->bind_method("certificate", tls_certificate, 0, 0);

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
