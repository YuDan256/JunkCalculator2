// inflate 原生模块：把 gzip / zlib / 裸 DEFLATE 解压暴露给脚本。
//
// 约定：**二进制输入输出都是小写十六进制字符串**（解码时大小写皆可），与 crypto
// 模块一致 —— 接口无歧义，测试能直接对着 RFC 1951/1952 的向量写。
//
// 实现上这里只是薄薄一层壳：容器解析与校验和在 src/lib/internal/codec/Gzip.cpp，
// 裸 DEFLATE 复用 src/utils/deflate/Deflate.cpp，本文件不实现任何解压算法。

#include "../jc2_extension_cpp.h"

#include "../internal/codec/Gzip.h"
#include "../internal/codec/Hex.h"

#include <cstdint>
#include <string>
#include <vector>

static jc2::Class* g_inflateErrorClass = nullptr;

[[noreturn]] inline void throwInflateError(const std::string& msg) {
    jc2::throw_error_class(*g_inflateErrorClass, msg);
}

// ─── 参数处理 ─────────────────────────────────────────────────

static jc::codec::Bytes hexArg(const jc2::Value& v, const char* what) {
    if (!v.is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(what) + " must be a hexadecimal string.");
    }
    jc::codec::Bytes out;
    if (!jc::codec::hexDecodeStrict(v.as_string(), out)) {
        throwInflateError(std::string(what) + " must be an even number of hexadecimal digits (got \"" +
                          v.as_string() + "\").");
    }
    return out;
}

static void requireArity(int argc, int want, const char* name) {
    if (argc != want) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(name) + " expects " + std::to_string(want) +
                                                        " arguments, got " + std::to_string(argc) + ".");
    }
}

// ─── 解压入口 ─────────────────────────────────────────────────

enum class Container { Raw, Zlib, Gzip };

static JC2_ValueHandle decodeToHex(Container container, const char* name, int argc,
                                   JC2_ValueHandle* argv) {
    requireArity(argc, 1, name);
    const jc::codec::Bytes in = hexArg(jc2::Value(argv[0]), "data");

    jc::codec::Bytes out;
    std::string err;
    bool ok = false;
    switch (container) {
        case Container::Raw:  ok = jc::codec::rawDecode(in.data(), in.size(), out, err); break;
        case Container::Zlib: ok = jc::codec::zlibDecode(in.data(), in.size(), out, err); break;
        case Container::Gzip: ok = jc::codec::gzipDecode(in.data(), in.size(), out, err); break;
    }
    if (!ok) {
        throwInflateError(std::string(name) + ": " + err + ".");
    }
    return jc2::Value(jc::codec::hexEncode(out)).get_handle();
}

static JC2_ValueHandle inflate_raw(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    return decodeToHex(Container::Raw, "inflate", argc, argv);
}

static JC2_ValueHandle inflate_unzlib(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    return decodeToHex(Container::Zlib, "unzlib", argc, argv);
}

static JC2_ValueHandle inflate_gunzip(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    return decodeToHex(Container::Gzip, "gunzip", argc, argv);
}

// 校验和：返回 8 位小写十六进制（与 gzip/zlib 尾部字段的写法一致）
static JC2_ValueHandle inflate_crc32(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    requireArity(argc, 1, "crc32");
    const jc::codec::Bytes in = hexArg(jc2::Value(argv[0]), "data");
    const uint32_t crc = jc::codec::crc32(in.data(), in.size());
    static const char* digits = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 0; i < 8; ++i) {
        s[static_cast<size_t>(i)] = digits[(crc >> (4 * (7 - i))) & 0xF];
    }
    return jc2::Value(s).get_handle();
}

// ─── 模块注册 ─────────────────────────────────────────────────

int jc2_init(jc2::Module& mod) {
    g_inflateErrorClass = new jc2::Class("InflateError");
    jc2::Value exceptionCls = jc2::get_global("Exception");
    g_inflateErrorClass->set_parent(jc2::Class(exceptionCls.get_handle()));
    mod.register_value("InflateError", *g_inflateErrorClass);

    mod.register_function("inflate", inflate_raw, 1, 1, {"data"});
    mod.register_function("unzlib", inflate_unzlib, 1, 1, {"data"});
    mod.register_function("gunzip", inflate_gunzip, 1, 1, {"data"});
    mod.register_function("crc32", inflate_crc32, 1, 1, {"data"});

    mod.register_function_help("inflate.inflate", "inflate.inflate(hex)",
        "Raw DEFLATE (RFC 1951) decompression. No container, no checksum. Raises InflateError.",
        "inflate.inflate(\"011000efff6a6332\")");
    mod.register_function_help("inflate.unzlib", "inflate.unzlib(hex)",
        "zlib (RFC 1950) decompression: 2-byte header, raw DEFLATE, Adler-32 verified.",
        "inflate.unzlib(zlibHex)");
    mod.register_function_help("inflate.gunzip", "inflate.gunzip(hex)",
        "gzip (RFC 1952) decompression: optional header fields skipped, raw DEFLATE, CRC32 + ISIZE verified.",
        "inflate.gunzip(gzipHex)");
    mod.register_function_help("inflate.crc32", "inflate.crc32(hex)",
        "CRC-32/ISO-HDLC of the bytes, as 8 lowercase hex digits (the gzip trailer checksum).",
        "inflate.crc32(\"616263\")");

    mod.register_help("inflate",
        "═══ Decompression (Native Module) ═══\n\n"
        "  Requires: import inflate\n\n"
        "  Every binary input and output is a lowercase hexadecimal string; empty\n"
        "  hex (\"\") means an empty byte string. Three containers are understood:\n\n"
        "    inflate.inflate(hex)   raw DEFLATE (RFC 1951) - no header, no checksum\n"
        "    inflate.unzlib(hex)    zlib     (RFC 1950) - CMF/FLG header + Adler-32\n"
        "    inflate.gunzip(hex)    gzip     (RFC 1952) - 1f 8b header + CRC32 + ISIZE\n\n"
        "  inflate.crc32(hex)       CRC-32 of the bytes, as 8 hex digits\n\n"
        "  All three raise InflateError on a bad header, malformed or truncated\n"
        "  DEFLATE, a checksum mismatch, or a result above the 64 MiB output limit.\n"
        "  Extra bytes after a complete stream are rejected (strict; the RFC does not\n"
        "  require it).\n\n"
        "  gzip FLG handling: FTEXT/FHCRC/FEXTRA/FNAME/FCOMMENT fields are located and\n"
        "  skipped correctly, but the FHCRC value itself is not recomputed and checked.\n"
        "  Reserved FLG bits, a non-8 CM, zlib preset dictionaries (FDICT) and an\n"
        "  empty raw stream are reported as errors. Only the first member of a\n"
        "  multi-member gzip stream is decompressed.\n"
    );

    return 0;
}

JC2_EXTENSION_INIT
