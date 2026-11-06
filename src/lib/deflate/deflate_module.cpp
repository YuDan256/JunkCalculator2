// deflate 原生模块：把 raw DEFLATE / zlib / gzip 压缩暴露给脚本。
//
// 与 inflate 模块保持对称：所有二进制输入输出都是小写十六进制字符串。
// 裸 DEFLATE 复用 src/utils/deflate/Deflate.cpp；本文件只构造 zlib/gzip
// 容器头与尾部校验和。

#include "../jc2_extension_cpp.h"

#include "../internal/codec/Gzip.h"
#include "../internal/codec/Hex.h"
#include "../../utils/deflate/Deflate.h"

#include <cstdint>
#include <string>
#include <vector>

static jc2::Class* g_deflateErrorClass = nullptr;

[[noreturn]] inline void throwDeflateError(const std::string& msg) {
    jc2::throw_error_class(*g_deflateErrorClass, msg);
}

static jc::codec::Bytes hexArg(const jc2::Value& v, const char* what) {
    if (!v.is_string()) {
        jc2::throw_error(jc2::ErrorType::TypeError, std::string(what) + " must be a hexadecimal string.");
    }
    jc::codec::Bytes out;
    if (!jc::codec::hexDecodeStrict(v.as_string(), out)) {
        throwDeflateError(std::string(what) + " must be an even number of hexadecimal digits (got \"" +
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

static jc::codec::Bytes rawCompress(const jc::codec::Bytes& in) {
    jc::codec::Bytes out;
    if (!jc::deflateCompress(in.data(), in.size(), out)) {
        throwDeflateError("deflate: compression failed.");
    }
    return out;
}

static void appendU32LE(jc::codec::Bytes& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static void appendU32BE(jc::codec::Bytes& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

enum class Container { Raw, Zlib, Gzip };

static jc::codec::Bytes compressTo(Container container, const jc::codec::Bytes& in) {
    jc::codec::Bytes body = rawCompress(in);
    jc::codec::Bytes out;
    switch (container) {
    case Container::Raw:
        return body;
    case Container::Zlib:
        // CMF=0x78 (CM=8, CINFO=7), FLG=0x9C: FDICT=0, (CMF<<8|FLG)%31==0.
        out.reserve(body.size() + 6);
        out.push_back(0x78);
        out.push_back(0x9C);
        out.insert(out.end(), body.begin(), body.end());
        appendU32BE(out, jc::codec::adler32(in.data(), in.size()));
        return out;
    case Container::Gzip:
        // Minimal RFC 1952 header: no filename/comment/extra, OS=255.
        out.reserve(body.size() + 18);
        out.push_back(0x1F);
        out.push_back(0x8B);
        out.push_back(0x08);
        out.push_back(0x00);
        appendU32LE(out, 0); // MTIME
        out.push_back(0x00); // XFL
        out.push_back(0xFF); // OS
        out.insert(out.end(), body.begin(), body.end());
        appendU32LE(out, jc::codec::crc32(in.data(), in.size()));
        appendU32LE(out, static_cast<uint32_t>(in.size()));
        return out;
    }
    return body;
}

static JC2_ValueHandle encodeToHex(Container container, const char* name, int argc,
                                   JC2_ValueHandle* argv) {
    requireArity(argc, 1, name);
    const jc::codec::Bytes in = hexArg(jc2::Value(argv[0]), "data");
    return jc2::Value(jc::codec::hexEncode(compressTo(container, in))).get_handle();
}

static JC2_ValueHandle deflate_raw(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    return encodeToHex(Container::Raw, "deflate", argc, argv);
}

static JC2_ValueHandle deflate_zlib(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    return encodeToHex(Container::Zlib, "zlib", argc, argv);
}

static JC2_ValueHandle deflate_gzip(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    return encodeToHex(Container::Gzip, "gzip", argc, argv);
}

int jc2_init(jc2::Module& mod) {
    g_deflateErrorClass = new jc2::Class("DeflateError");
    jc2::Value exceptionCls = jc2::get_global("Exception");
    g_deflateErrorClass->set_parent(jc2::Class(exceptionCls.get_handle()));
    mod.register_value("DeflateError", *g_deflateErrorClass);

    mod.register_function("deflate", deflate_raw, 1, 1, {"data"});
    mod.register_function("zlib", deflate_zlib, 1, 1, {"data"});
    mod.register_function("gzip", deflate_gzip, 1, 1, {"data"});

    mod.register_function_help("deflate.deflate", "deflate.deflate(hex)",
        "Raw DEFLATE (RFC 1951) compression. No container, no checksum. Raises DeflateError.",
        "deflate.deflate(\"616263\")");
    mod.register_function_help("deflate.zlib", "deflate.zlib(hex)",
        "zlib (RFC 1950) compression: 2-byte header, raw DEFLATE, Adler-32.",
        "deflate.zlib(\"616263\")");
    mod.register_function_help("deflate.gzip", "deflate.gzip(hex)",
        "gzip (RFC 1952) compression: 10-byte header, raw DEFLATE, CRC32 + ISIZE.",
        "deflate.gzip(\"616263\")");

    mod.register_help("deflate",
        "═══ Compression (Native Module) ═══\n\n"
        "  Requires: import deflate\n\n"
        "  Every binary input and output is a lowercase hexadecimal string; empty\n"
        "  hex (\"\") means an empty byte string.\n\n"
        "    deflate.deflate(hex)   raw DEFLATE (RFC 1951) - no header, no checksum\n"
        "    deflate.zlib(hex)      zlib     (RFC 1950) - CMF/FLG header + Adler-32\n"
        "    deflate.gzip(hex)      gzip     (RFC 1952) - 1f 8b header + CRC32 + ISIZE\n\n"
        "  The output can be passed to inflate.inflate / unzlib / gunzip.\n"
        "  DeflateError is raised on malformed input or compression failure.\n"
    );

    return 0;
}

JC2_EXTENSION_INIT
