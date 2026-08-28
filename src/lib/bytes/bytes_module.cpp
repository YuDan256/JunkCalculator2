#include "../jc2_extension_cpp.h"
#include <vector>
#include <cstdint>
#include <memory>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <string>
#include <cmath>
#include <algorithm>
#include <filesystem>

static std::filesystem::path to_path(const std::string& utf8_str) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8_str.c_str()));
}

static jc2::Class* g_bytesClass = nullptr;

struct ByteBufferContext {
    std::shared_ptr<std::vector<uint8_t>> shared_data;
    uint8_t* view_data;
    size_t view_size;
    size_t pos = 0;
};

static ByteBufferContext* getBuf(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("Type Error: Expected a Bytes instance.");
    auto ptr = val.get_native_data<ByteBufferContext>();
    if (!ptr) jc2::throw_error("Type Error: Expected a Bytes native object.");
    return ptr;
}

static jc2::Value makeBytesInstance(std::vector<uint8_t> data) {
    jc2::Instance inst(*g_bytesClass);
    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(data));
    auto ptr = new ByteBufferContext{shared, shared->data(), shared->size(), 0};
    inst.set_native_data(ptr, [](void* p) {
        delete static_cast<ByteBufferContext*>(p);
    });
    inst.set_buffer_data(ptr->view_data, ptr->view_size);
    return inst;
}

static jc2::Value makeBytesView(std::shared_ptr<std::vector<uint8_t>> shared, uint8_t* view_data, size_t view_size) {
    jc2::Instance inst(*g_bytesClass);
    auto ptr = new ByteBufferContext{std::move(shared), view_data, view_size, 0};
    inst.set_native_data(ptr, [](void* p) {
        delete static_cast<ByteBufferContext*>(p);
    });
    inst.set_buffer_data(ptr->view_data, ptr->view_size);
    return inst;
}

static std::vector<double> extractDS(const jc2::Value& v, const std::string& f) {
    if (v.is_list()) {
        jc2::List list(v.get_handle());
        std::vector<double> r(list.size());
        for (size_t i = 0; i < list.size(); ++i) r[i] = list.get(i).as_double();
        return r;
    }
    if (v.is_real_matrix()) {
        jc2::RealMatrix mat(v.get_handle());
        std::vector<double> r(mat.rows() * mat.cols());
        for (int i = 0; i < mat.rows(); ++i) {
            for (int j = 0; j < mat.cols(); ++j) {
                r[i * mat.cols() + j] = mat.get(i, j);
            }
        }
        return r;
    }
    jc2::throw_error("Type Error: " + f + "() requires a list or real matrix.");
}

struct BufWrapper {
    uint8_t* d;
    size_t s;
    uint8_t* data() const { return d; }
    size_t size() const { return s; }
    uint8_t* begin() const { return d; }
    uint8_t* end() const { return d + s; }
    uint8_t& operator[](size_t i) { return d[i]; }
    const uint8_t& operator[](size_t i) const { return d[i]; }
};

#define METHOD(name) JC2_ValueHandle bytes_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)
#define GET_SELF \
    (void)argc; \
    auto ctx = getBuf(jc2::Value(argv[0])); \
    BufWrapper buf{ctx->view_data, ctx->view_size}; \
    auto& pos = ctx->pos; \
    (void)buf; \
    (void)pos

METHOD(writeFile) {
    GET_SELF;
    if (!jc2::Value(argv[1]).is_string()) jc2::throw_error("Type Error: expects string path.");
    std::string path = jc2::Value(argv[1]).as_string();
    std::ofstream f(to_path(path), std::ios::binary);
    if (!f) jc2::throw_error("IO Error: Cannot write to file '" + path + "'.");
    f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    return jc2::Value().get_handle();
}

METHOD(set) {
    GET_SELF;
    size_t offset = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
    if (!jc2::Value(argv[3]).is_string()) jc2::throw_error("Type Error: type must be string.");
    std::string type = jc2::Value(argv[3]).as_string();

    auto writeMem = [&](const void* data, size_t size) {
        if (offset + size > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds.");
        std::memcpy(buf.data() + offset, data, size);
    };

    if (type == "str") {
        if (!jc2::Value(argv[2]).is_string()) jc2::throw_error("Type Error: Expected string for 'str' type.");
        std::string s = jc2::Value(argv[2]).as_string();
        writeMem(s.data(), s.size());
    } else if (type == "i64" || type == "u64") {
        if (!jc2::Value(argv[2]).is_bigint()) jc2::throw_error("Type Error: Expected BigInt for 64-bit integer.");
        std::string s = jc2::BigInt(argv[2]).to_string();
        int64_t v = std::stoll(s);
        writeMem(&v, 8);
    } else {
        double val = jc2::Value(argv[2]).as_double();
        if (type == "u8") { uint8_t  v = static_cast<uint8_t>(val);  writeMem(&v, 1); }
        else if (type == "i8") { int8_t   v = static_cast<int8_t>(val);   writeMem(&v, 1); }
        else if (type == "u16") { uint16_t v = static_cast<uint16_t>(val); writeMem(&v, 2); }
        else if (type == "i16") { int16_t  v = static_cast<int16_t>(val);  writeMem(&v, 2); }
        else if (type == "u32") { uint32_t v = static_cast<uint32_t>(val); writeMem(&v, 4); }
        else if (type == "i32") { int32_t  v = static_cast<int32_t>(val);  writeMem(&v, 4); }
        else if (type == "f32") { float    v = static_cast<float>(val);    writeMem(&v, 4); }
        else if (type == "f64") { double   v = val;                        writeMem(&v, 8); }
        else jc2::throw_error("Buffer Error: Unknown format type '" + type + "'.");
    }
    return jc2::Value().get_handle();
}

METHOD(write_arr) {
    GET_SELF;
    size_t offset = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
    auto arr = extractDS(jc2::Value(argv[2]), "write_arr");
    if (!jc2::Value(argv[3]).is_string()) jc2::throw_error("Type Error: type must be string.");
    std::string type = jc2::Value(argv[3]).as_string();

    if (type == "i16") {
        if (offset + arr.size() * 2 > buf.size()) jc2::throw_error("Buffer out of bounds.");
        int16_t* ptr = reinterpret_cast<int16_t*>(buf.data() + offset);
        for (size_t i = 0; i < arr.size(); ++i) {
            double val = std::max(-1.0, std::min(1.0, arr[i]));
            ptr[i] = static_cast<int16_t>(val * 32767.0);
        }
    } else if (type == "f64") {
        if (offset + arr.size() * 8 > buf.size()) jc2::throw_error("Buffer out of bounds.");
        std::memcpy(buf.data() + offset, arr.data(), arr.size() * sizeof(double));
    } else {
        jc2::throw_error("b_write_arr currently supports 'i16' and 'f64'");
    }
    return jc2::Value().get_handle();
}

METHOD(get) {
    GET_SELF;
    size_t offset = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
    if (!jc2::Value(argv[2]).is_string()) jc2::throw_error("Type Error: type must be string.");
    std::string type = jc2::Value(argv[2]).as_string();

    auto readMem = [&](void* data, size_t size) {
        if (offset + size > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds.");
        std::memcpy(data, buf.data() + offset, size);
    };

    if (type == "str") {
        if (argc != 4) jc2::throw_error("Buffer Error: String read requires length.");
        size_t len = static_cast<size_t>(std::max(0.0, jc2::Value(argv[3]).as_double()));
        if (offset + len > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds.");
        return jc2::Value(std::string(reinterpret_cast<char*>(buf.data() + offset), len)).get_handle();
    }

    if (argc != 3) jc2::throw_error("Buffer Error: Incorrect argument count.");
    if (type == "u8") { uint8_t  v; readMem(&v, 1); return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
    else if (type == "i8") { int8_t   v; readMem(&v, 1); return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
    else if (type == "u16") { uint16_t v; readMem(&v, 2); return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
    else if (type == "i16") { int16_t  v; readMem(&v, 2); return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
    else if (type == "u32") { uint32_t v; readMem(&v, 4); return jc2::BigInt(std::to_string(v)).get_handle(); }
    else if (type == "i32") { int32_t  v; readMem(&v, 4); return jc2::Value(v).get_handle(); }
    else if (type == "i64") { int64_t  v; readMem(&v, 8); return jc2::BigInt(std::to_string(v)).get_handle(); }
    else if (type == "u64") { uint64_t v; readMem(&v, 8); return jc2::BigInt(std::to_string(v)).get_handle(); }
    else if (type == "f32") { float    v; readMem(&v, 4); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "f64") { double   v; readMem(&v, 8); return jc2::Value(v).get_handle(); }
    else jc2::throw_error("Buffer Error: Unknown format type '" + type + "'.");
}

METHOD(len) {
    GET_SELF;
    if (buf.size() <= static_cast<size_t>(INT32_MAX)) {
        return jc2::Value(static_cast<int32_t>(buf.size())).get_handle();
    }
    return jc2::BigInt(std::to_string(buf.size())).get_handle();
}

METHOD(toHex) {
    GET_SELF;
    static const char hex_chars[] = "0123456789abcdef";
    std::string res;
    res.reserve(buf.size() * 2);
    for (uint8_t b : buf) {
        res.push_back(hex_chars[b >> 4]);
        res.push_back(hex_chars[b & 0x0F]);
    }
    return jc2::Value(res).get_handle();
}

METHOD(view) {
    GET_SELF;
    if (argc < 2) jc2::throw_error("Type Error: view expects at least a start offset.");
    size_t start = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
    size_t len = buf.size() > start ? buf.size() - start : 0;
    if (argc >= 3) {
        len = static_cast<size_t>(std::max(0.0, jc2::Value(argv[2]).as_double()));
    }
    if (start > buf.size() || start + len > buf.size()) {
        jc2::throw_error("Buffer Error: View out of bounds.");
    }
    return makeBytesView(ctx->shared_data, buf.data() + start, len).get_handle();
}

METHOD(toBase64) {
    GET_SELF;
    static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string res;
    size_t i = 0;
    while (i < buf.size()) {
        size_t old_i = i;
        uint32_t octet_a = i < buf.size() ? buf[i++] : 0;
        uint32_t octet_b = i < buf.size() ? buf[i++] : 0;
        uint32_t octet_c = i < buf.size() ? buf[i++] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
        res.push_back(b64_chars[(triple >> 18) & 0x3F]);
        res.push_back(b64_chars[(triple >> 12) & 0x3F]);
        res.push_back((i - old_i) > 1 ? b64_chars[(triple >> 6) & 0x3F] : '=');
        res.push_back((i - old_i) > 2 ? b64_chars[triple & 0x3F] : '=');
    }
    return jc2::Value(res).get_handle();
}

// --- Cursor & State Control ---
METHOD(seek) { GET_SELF; pos = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double())); return argv[0]; }
METHOD(skip) { GET_SELF; pos += static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double())); return argv[0]; }
METHOD(tell) { GET_SELF; return jc2::Value(static_cast<double>(pos)).get_handle(); }
METHOD(isEnd) { GET_SELF; return jc2::Value(pos >= buf.size()).get_handle(); }

// --- Chainable Writers ---
METHOD(writeStr) {
    GET_SELF;
    std::string s = jc2::Value(argv[1]).as_string();
    if (pos + s.size() > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds.");
    std::memcpy(buf.data() + pos, s.data(), s.size());
    pos += s.size();
    return argv[0];
}
METHOD(writeU8) { GET_SELF; if (pos + 1 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); buf[pos++] = static_cast<uint8_t>(jc2::Value(argv[1]).as_double()); return argv[0]; }
METHOD(writeI8)  { GET_SELF; if (pos + 1 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); int8_t v = static_cast<int8_t>(jc2::Value(argv[1]).as_double()); std::memcpy(buf.data() + pos, &v, 1); pos += 1; return argv[0]; }
METHOD(writeU16) { GET_SELF; if (pos + 2 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); uint16_t v = static_cast<uint16_t>(jc2::Value(argv[1]).as_double()); std::memcpy(buf.data() + pos, &v, 2); pos += 2; return argv[0]; }
METHOD(writeU32) { GET_SELF; if (pos + 4 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); uint32_t v = static_cast<uint32_t>(jc2::Value(argv[1]).as_double()); std::memcpy(buf.data() + pos, &v, 4); pos += 4; return argv[0]; }
METHOD(writeF32) { GET_SELF; if (pos + 4 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); float v = static_cast<float>(jc2::Value(argv[1]).as_double()); std::memcpy(buf.data() + pos, &v, 4); pos += 4; return argv[0]; }
METHOD(writeF64) { GET_SELF; if (pos + 8 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); double v = jc2::Value(argv[1]).as_double(); std::memcpy(buf.data() + pos, &v, 8); pos += 8; return argv[0]; }
METHOD(writeI16) { GET_SELF; if (pos + 2 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); int16_t v = static_cast<int16_t>(jc2::Value(argv[1]).as_double()); std::memcpy(buf.data() + pos, &v, 2); pos += 2; return argv[0]; }
METHOD(writeI32) { GET_SELF; if (pos + 4 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); int32_t v = static_cast<int32_t>(jc2::Value(argv[1]).as_double()); std::memcpy(buf.data() + pos, &v, 4); pos += 4; return argv[0]; }
METHOD(writeI64) { GET_SELF; if (pos + 8 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); int64_t v = std::stoll(jc2::Value(argv[1]).to_string()); std::memcpy(buf.data() + pos, &v, 8); pos += 8; return argv[0]; }
METHOD(writeU64) { GET_SELF; if (pos + 8 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds."); uint64_t v = std::stoull(jc2::Value(argv[1]).to_string()); std::memcpy(buf.data() + pos, &v, 8); pos += 8; return argv[0]; }
METHOD(writePcmArray) {
    GET_SELF;
    auto arr = extractDS(jc2::Value(argv[1]), "writePcmArray");
    if (pos + arr.size() * 2 > buf.size()) jc2::throw_error("Buffer Error: Write out of bounds.");
    int16_t* ptr = reinterpret_cast<int16_t*>(buf.data() + pos);
    for (size_t i = 0; i < arr.size(); ++i) {
        double val = std::max(-1.0, std::min(1.0, arr[i]));
        ptr[i] = static_cast<int16_t>(val * 32767.0);
    }
    pos += arr.size() * 2;
    return argv[0];
}

// --- Sequential Readers ---
METHOD(readStr) {
    GET_SELF;
    size_t len = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
    if (pos + len > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds.");
    std::string s(reinterpret_cast<char*>(buf.data() + pos), len);
    pos += len;
    return jc2::Value(s).get_handle();
}
METHOD(readU8) { GET_SELF; if (pos + 1 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); uint8_t v = buf[pos++]; return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
METHOD(readI8)  { GET_SELF; if (pos + 1 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); int8_t v; std::memcpy(&v, buf.data() + pos, 1); pos += 1; return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
METHOD(readU16) { GET_SELF; if (pos + 2 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); uint16_t v; std::memcpy(&v, buf.data() + pos, 2); pos += 2; return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
METHOD(readU32) { GET_SELF; if (pos + 4 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); uint32_t v; std::memcpy(&v, buf.data() + pos, 4); pos += 4; return jc2::BigInt(std::to_string(v)).get_handle(); }
METHOD(readF32) { GET_SELF; if (pos + 4 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); float v; std::memcpy(&v, buf.data() + pos, 4); pos += 4; return jc2::Value(static_cast<double>(v)).get_handle(); }
METHOD(readF64) { GET_SELF; if (pos + 8 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); double v; std::memcpy(&v, buf.data() + pos, 8); pos += 8; return jc2::Value(v).get_handle(); }
METHOD(readI16) { GET_SELF; if (pos + 2 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); int16_t v; std::memcpy(&v, buf.data() + pos, 2); pos += 2; return jc2::Value(static_cast<int32_t>(v)).get_handle(); }
METHOD(readI32) { GET_SELF; if (pos + 4 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); int32_t v; std::memcpy(&v, buf.data() + pos, 4); pos += 4; return jc2::Value(v).get_handle(); }
METHOD(readI64) { GET_SELF; if (pos + 8 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); int64_t v; std::memcpy(&v, buf.data() + pos, 8); pos += 8; return jc2::BigInt(std::to_string(v)).get_handle(); }
METHOD(readU64) { GET_SELF; if (pos + 8 > buf.size()) jc2::throw_error("Buffer Error: Read out of bounds."); uint64_t v; std::memcpy(&v, buf.data() + pos, 8); pos += 8; return jc2::BigInt(std::to_string(v)).get_handle(); }

#define FUNC(name) JC2_ValueHandle global_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)

FUNC(alloc) {
    if (argc < 1) jc2::throw_error("TypeError: Bytes() takes exactly 1 argument (size).");
    int size = static_cast<int>(std::round(jc2::Value(argv[0]).as_double()));
    if (size < 0) jc2::throw_error("Math Error: buffer size cannot be negative.");
    return makeBytesInstance(std::vector<uint8_t>(size, 0)).get_handle();
}

FUNC(pack) {
    (void)argc;
    auto arr = extractDS(jc2::Value(argv[0]), "pack");
    std::vector<uint8_t> buf(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        double v = arr[i];
        if (v < 0.0) v = 0.0;
        if (v > 255.0) v = 255.0;
        buf[i] = static_cast<uint8_t>(v);
    }
    return makeBytesInstance(std::move(buf)).get_handle();
}

FUNC(fromHex) {
    if (argc < 1 || !jc2::Value(argv[0]).is_string()) jc2::throw_error("Type Error: fromHex expects a string.");
    std::string s = jc2::Value(argv[0]).as_string();
    std::vector<uint8_t> buf;
    buf.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        auto char2int = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            jc2::throw_error("Value Error: Invalid hex character.");
        };
        buf.push_back((char2int(s[i]) << 4) | char2int(s[i+1]));
    }
    return makeBytesInstance(std::move(buf)).get_handle();
}

FUNC(fromBase64) {
    if (argc < 1 || !jc2::Value(argv[0]).is_string()) jc2::throw_error("Type Error: fromBase64 expects a string.");
    std::string s = jc2::Value(argv[0]).as_string();
    std::vector<uint8_t> buf;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[static_cast<unsigned char>("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i])] = i;
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            buf.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return makeBytesInstance(std::move(buf)).get_handle();
}

FUNC(readFile) {
    (void)argc;
    if (!jc2::Value(argv[0]).is_string()) jc2::throw_error("Type Error: expects string path.");
    std::string path = jc2::Value(argv[0]).as_string();
    std::ifstream f(to_path(path), std::ios::binary | std::ios::ate);
    if (!f) jc2::throw_error("IO Error: Cannot open file '" + path + "'.");
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (f.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return makeBytesInstance(std::move(buffer)).get_handle();
    }
    jc2::throw_error("IO Error: Failed to read file.");
}

int jc2_init(jc2::Module& mod) {
    g_bytesClass = new jc2::Class("Bytes");
    mod.register_value("Bytes", *g_bytesClass);

    g_bytesClass->bind_method("writeFile", bytes_writeFile, 1, 1, {"path"});
    g_bytesClass->bind_method("save", bytes_writeFile, 1, 1, {"path"});
    g_bytesClass->bind_method("set", bytes_set, 3, 3, {"offset", "val", "type"});
    g_bytesClass->bind_method("write_arr", bytes_write_arr, 3, 3, {"offset", "arr", "type"});
    g_bytesClass->bind_method("get", bytes_get, 2, 3, {"offset", "type", "len"});
    g_bytesClass->bind_method("len", bytes_len, 0, 0);
    g_bytesClass->bind_method("length", bytes_len, 0, 0);
    g_bytesClass->bind_method("toHex", bytes_toHex, 0, 0);
    g_bytesClass->bind_method("toBase64", bytes_toBase64, 0, 0);
    g_bytesClass->bind_method("view", bytes_view, 1, 2, {"start", "len"});
    
    g_bytesClass->bind_method("seek", bytes_seek, 1, 1, {"pos"});
    g_bytesClass->bind_method("skip", bytes_skip, 1, 1, {"n"});
    g_bytesClass->bind_method("tell", bytes_tell, 0, 0);
    g_bytesClass->bind_method("isEnd", bytes_isEnd, 0, 0);
    
    g_bytesClass->bind_method("writeStr", bytes_writeStr, 1, 1, {"s"});
    g_bytesClass->bind_method("writeU8", bytes_writeU8, 1, 1, {"v"});
    g_bytesClass->bind_method("writeI8", bytes_writeI8, 1, 1, {"v"});
    g_bytesClass->bind_method("writeU16", bytes_writeU16, 1, 1, {"v"});
    g_bytesClass->bind_method("writeU32", bytes_writeU32, 1, 1, {"v"});
    g_bytesClass->bind_method("writeF32", bytes_writeF32, 1, 1, {"v"});
    g_bytesClass->bind_method("writeF64", bytes_writeF64, 1, 1, {"v"});
    g_bytesClass->bind_method("writeI16", bytes_writeI16, 1, 1, {"v"});
    g_bytesClass->bind_method("writeI32", bytes_writeI32, 1, 1, {"v"});
    g_bytesClass->bind_method("writeI64", bytes_writeI64, 1, 1, {"v"});
    g_bytesClass->bind_method("writeU64", bytes_writeU64, 1, 1, {"v"});
    g_bytesClass->bind_method("writePcmArray", bytes_writePcmArray, 1, 1, {"arr"});
    
    g_bytesClass->bind_method("readStr", bytes_readStr, 1, 1, {"n"});
    g_bytesClass->bind_method("readU8", bytes_readU8, 0, 0);
    g_bytesClass->bind_method("readI8", bytes_readI8, 0, 0);
    g_bytesClass->bind_method("readU16", bytes_readU16, 0, 0);
    g_bytesClass->bind_method("readU32", bytes_readU32, 0, 0);
    g_bytesClass->bind_method("readF32", bytes_readF32, 0, 0);
    g_bytesClass->bind_method("readF64", bytes_readF64, 0, 0);
    g_bytesClass->bind_method("readI16", bytes_readI16, 0, 0);
    g_bytesClass->bind_method("readI32", bytes_readI32, 0, 0);
    g_bytesClass->bind_method("readI64", bytes_readI64, 0, 0);
    g_bytesClass->bind_method("readU64", bytes_readU64, 0, 0);

    g_bytesClass->set_allocator(global_alloc);

    mod.register_function("alloc", global_alloc, 1, 1, {"size"});
    mod.register_function("pack", global_pack, 1, 1, {"arr"});
    mod.register_function("readFile", global_readFile, 1, 1, {"path"});
    mod.register_function("fromFile", global_readFile, 1, 1, {"path"});
    mod.register_function("fromHex", global_fromHex, 1, 1, {"str"});
    mod.register_function("fromBase64", global_fromBase64, 1, 1, {"str"});

    mod.register_help("bytes",
        "═══ Bare-Metal Memory Engine — Native Module ═══\n\n"
        "  Requires: import bytes\n\n"
        "  The `bytes` module provides low-level, zero-dependency C++ memory buffers. \n"
        "  It grants absolute control over binary reading, writing, and file I/O.\n"
        "  \n"
        "  Buffer Allocation & I/O\n"
        "  ──────────────────────\n"
        "    bytes.Bytes(size)           Allocate a zeroed buffer of `size` bytes.\n"
        "    bytes.alloc(size)           Alias for bytes.Bytes.\n"
        "    bytes.pack(array)           Create a buffer from an array of 8-bit integers.\n"
        "    bytes.readFile(path)        Map an entire file from disk into a buffer.\n"
        "    bytes.fromFile(path)        Alias for bytes.readFile.\n"
        "    bytes.fromHex(str)          Create a buffer by decoding a Hex string.\n"
        "    bytes.fromBase64(str)       Create a buffer by decoding a Base64 string.\n"
        "    buf.save(path)              Flushes binary buffer to disk.\n"
        "    buf.len() / buf.length()    Get the total size of the buffer in bytes.\n"
        "    buf.toHex()                 Returns the buffer contents as a Hex string.\n"
        "    buf.toBase64()              Returns the buffer contents as a Base64 string.\n"
        "    buf.view(start, [len])      Creates a zero-copy memory view of the buffer.\n\n"
        "  Cursor & State Control\n"
        "  ──────────────────────\n"
        "    buf.tell()                  Returns current cursor position.\n"
        "    buf.seek(pos)               Moves cursor to the absolute byte offset `pos`.\n"
        "    buf.skip(n)                 Moves cursor forward by `n` bytes.\n"
        "    buf.isEnd()                 Returns true if cursor reached the end.\n\n"
        "  Chainable Writers (Cursor Auto-Advances)\n"
        "  ──────────────────────\n"
        "    buf.writeStr(s)             Writes text bytes.\n"
        "    buf.writeI8(v)              Writes signed 8-bit (1 byte).\n"
        "    buf.writeU8(v)              Writes unsigned 8-bit (1 byte).\n"
        "    buf.writeI16(v)             Writes signed 16-bit (2 bytes).\n"
        "    buf.writeU16(v)             Writes unsigned 16-bit (2 bytes).\n"
        "    buf.writeI32(v)             Writes signed 32-bit (4 bytes).\n"
        "    buf.writeU32(v)             Writes unsigned 32-bit (4 bytes).\n"
        "    buf.writeI64(v)             Writes signed 64-bit (8 bytes).\n"
        "    buf.writeU64(v)             Writes unsigned 64-bit (8 bytes).\n"
        "    buf.writeF32(v)             Writes 32-bit float (4 bytes).\n"
        "    buf.writeF64(v)             Writes 64-bit float (8 bytes).\n"
        "    buf.writePcmArray(arr)      Bulk-writes an array as 16-bit PCM.\n"
        "    (chainable: each returns the buffer for `.a().b().c()` style)\n\n"
        "  Readers (Cursor Auto-Advances)\n"
        "  ──────────────────────\n"
        "    buf.readStr(n)              Reads n bytes as a string.\n"
        "    buf.readI8()                Reads signed 8-bit.\n"
        "    buf.readU8()                Reads unsigned 8-bit.\n"
        "    buf.readI16()               Reads signed 16-bit.\n"
        "    buf.readU16()               Reads unsigned 16-bit.\n"
        "    buf.readI32()               Reads signed 32-bit.\n"
        "    buf.readU32()               Reads unsigned 32-bit (BigInt).\n"
        "    buf.readI64()               Reads signed 64-bit (BigInt).\n"
        "    buf.readU64()               Reads unsigned 64-bit (BigInt).\n"
        "    buf.readF32()               Reads 32-bit float.\n"
        "    buf.readF64()               Reads 64-bit float.\n\n"
        "  Low-Level Reading & Writing (Absolute Offsets)\n"
        "  ──────────────────────\n"
        "    buf.set(offset, val, type)  Write a value into memory at `offset`.\n"
        "    buf.write_arr(off, arr, t)  Bulk-write an array at `offset` ('i16'/'f64').\n"
        "    buf.get(offset, type)       Read a value from memory at `offset`.\n"
        "    buf.get(offset, \"str\", len) Read specifically a string of `len` bytes.\n\n"
        "    Supported formats (passed as strings): \n"
        "      \"u8\", \"i8\", \"u16\", \"i16\", \"u32\", \"i32\", \"i64\", \"u64\", \"f32\", \"f64\", \"str\"\n"
    );

    mod.register_function_help("bytes.Bytes", "bytes.Bytes(size)", "Allocates a zeroed byte buffer of the specified size.", "bytes.Bytes(1024)");
    mod.register_function_help("bytes.alloc", "bytes.alloc(size)", "Alias for bytes.Bytes.", "bytes.alloc(1024)");
    mod.register_function_help("bytes.pack", "bytes.pack(array)", "Creates a byte buffer from an array of 8-bit integers.", "bytes.pack([255, 0, 128])");
    mod.register_function_help("bytes.readFile", "bytes.readFile(path)", "Reads an entire file into a byte buffer.", "bytes.readFile(\"data.bin\")");
    mod.register_function_help("bytes.save", "buf.save(path)", "Writes a byte buffer to a file.", "buf.save(\"out.bin\")");
    mod.register_function_help("bytes.set", "buf.set(offset, val, type)", "Writes a value into the buffer at the specified offset.", "buf.set(0, 255, \"u8\")");
    mod.register_function_help("bytes.get", "buf.get(offset, type, [len])", "Reads a value from the buffer at the specified offset.", "buf.get(0, \"u16\")");
    mod.register_function_help("bytes.len", "buf.len()", "Returns the total size of the buffer in bytes.", "buf.len()");
    mod.register_function_help("bytes.toHex", "buf.toHex()", "Encodes the buffer contents into a hexadecimal string.", "buf.toHex()");
    mod.register_function_help("bytes.toBase64", "buf.toBase64()", "Encodes the buffer contents into a Base64 string.", "buf.toBase64()");
    mod.register_function_help("bytes.fromHex", "bytes.fromHex(str)", "Decodes a hexadecimal string into a new byte buffer.", "bytes.fromHex(\"FF00AA\")");
    mod.register_function_help("bytes.fromBase64", "bytes.fromBase64(str)", "Decodes a Base64 string into a new byte buffer.", "bytes.fromBase64(\"SGVsbG8=\")");
    mod.register_function_help("bytes.view", "buf.view(start, [len])", "Creates a zero-copy memory view of the buffer. Modifying the view modifies the original buffer.", "buf.view(4, 10)");
    mod.register_function_help("bytes.length", "buf.length()", "Alias for buf.len(). Returns the total buffer size in bytes.", "buf.length()");
    mod.register_function_help("bytes.writeFile", "buf.writeFile(path)", "Writes a byte buffer to a file.", "buf.writeFile(\"out.bin\")");
    mod.register_function_help("bytes.write_arr", "buf.write_arr(offset, arr, type)", "Bulk-writes an array of numbers at an absolute offset. Supported types: \"i16\", \"f64\".", "buf.write_arr(0, [1, 2, 3], \"i16\")");
    mod.register_function_help("bytes.seek", "buf.seek(pos)", "Moves the cursor to an absolute byte offset.", "buf.seek(4)");
    mod.register_function_help("bytes.skip", "buf.skip(n)", "Moves the cursor forward by n bytes.", "buf.skip(4)");
    mod.register_function_help("bytes.tell", "buf.tell()", "Returns the current cursor position.", "buf.tell()");
    mod.register_function_help("bytes.isEnd", "buf.isEnd()", "Returns true if the cursor reached the end of the buffer.", "buf.isEnd()");
    mod.register_function_help("bytes.writeStr", "buf.writeStr(s)", "Writes text bytes at the cursor and advances it.", "buf.writeStr(\"RIFF\")");
    mod.register_function_help("bytes.writeU8", "buf.writeU8(v)", "Writes an 8-bit unsigned integer and advances the cursor.", "buf.writeU8(255)");
    mod.register_function_help("bytes.writeI8", "buf.writeI8(v)", "Writes an 8-bit signed integer and advances the cursor.", "buf.writeI8(-1)");
    mod.register_function_help("bytes.writeU16", "buf.writeU16(v)", "Writes a 16-bit unsigned integer and advances the cursor.", "buf.writeU16(65535)");
    mod.register_function_help("bytes.writeU32", "buf.writeU32(v)", "Writes a 32-bit unsigned integer and advances the cursor.", "buf.writeU32(4294967295)");
    mod.register_function_help("bytes.writeF32", "buf.writeF32(v)", "Writes a 32-bit float and advances the cursor.", "buf.writeF32(1.5)");
    mod.register_function_help("bytes.writeF64", "buf.writeF64(v)", "Writes a 64-bit float and advances the cursor.", "buf.writeF64(1.5)");
    mod.register_function_help("bytes.writeI16", "buf.writeI16(v)", "Writes a 16-bit signed integer and advances the cursor.", "buf.writeI16(-2)");
    mod.register_function_help("bytes.writeI32", "buf.writeI32(v)", "Writes a 32-bit signed integer and advances the cursor.", "buf.writeI32(1024)");
    mod.register_function_help("bytes.writeI64", "buf.writeI64(v)", "Writes a 64-bit signed integer and advances the cursor.", "buf.writeI64(9000000000)");
    mod.register_function_help("bytes.writeU64", "buf.writeU64(v)", "Writes a 64-bit unsigned integer and advances the cursor.", "buf.writeU64(9000000000)");
    mod.register_function_help("bytes.writePcmArray", "buf.writePcmArray(arr)", "Bulk-writes an array as 16-bit PCM samples.", "buf.writePcmArray([0.5, -0.5])");
    mod.register_function_help("bytes.readStr", "buf.readStr(n)", "Reads n bytes as a string and advances the cursor.", "buf.readStr(4)");
    mod.register_function_help("bytes.readU8", "buf.readU8()", "Reads 1 unsigned byte and advances the cursor.", "buf.readU8()");
    mod.register_function_help("bytes.readI8", "buf.readI8()", "Reads 1 signed byte and advances the cursor.", "buf.readI8()");
    mod.register_function_help("bytes.readU16", "buf.readU16()", "Reads 2 bytes as an unsigned 16-bit integer.", "buf.readU16()");
    mod.register_function_help("bytes.readU32", "buf.readU32()", "Reads 4 bytes as an unsigned 32-bit integer (BigInt).", "buf.readU32()");
    mod.register_function_help("bytes.readF32", "buf.readF32()", "Reads 4 bytes as a 32-bit float.", "buf.readF32()");
    mod.register_function_help("bytes.readF64", "buf.readF64()", "Reads 8 bytes as a 64-bit float.", "buf.readF64()");
    mod.register_function_help("bytes.readI16", "buf.readI16()", "Reads 2 bytes as a signed 16-bit integer.", "buf.readI16()");
    mod.register_function_help("bytes.readI32", "buf.readI32()", "Reads 4 bytes as a signed 32-bit integer.", "buf.readI32()");
    mod.register_function_help("bytes.readI64", "buf.readI64()", "Reads 8 bytes as a signed 64-bit integer (BigInt).", "buf.readI64()");
    mod.register_function_help("bytes.readU64", "buf.readU64()", "Reads 8 bytes as an unsigned 64-bit integer (BigInt).", "buf.readU64()");

    return 0;
}

JC2_EXTENSION_INIT
