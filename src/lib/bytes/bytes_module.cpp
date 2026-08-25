#include "../jc2_extension_cpp.h"
#include <vector>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <string>
#include <cmath>
#include <algorithm>

static jc2::Class* g_bytesClass = nullptr;

struct ByteBufferContext {
    std::vector<uint8_t> data;
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
    auto ptr = new ByteBufferContext{std::move(data), 0};
    inst.set_native_data(ptr, [](void* p) {
        delete static_cast<ByteBufferContext*>(p);
    });
    inst.set_buffer_data(ptr->data.data(), ptr->data.size());
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
    return {};
}

#define METHOD(name) JC2_ValueHandle bytes_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)
#define GET_SELF \
    (void)argc; \
    auto ctx = getBuf(jc2::Value(argv[0])); \
    auto& buf = ctx->data; \
    auto& pos = ctx->pos; \
    (void)buf; \
    (void)pos

METHOD(writeFile) {
    GET_SELF;
    if (!jc2::Value(argv[1]).is_string()) jc2::throw_error("Type Error: expects string path.");
    std::string path = jc2::Value(argv[1]).as_string();
    std::ofstream f(path, std::ios::binary);
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
    return jc2::Value().get_handle();
}

METHOD(len) {
    GET_SELF;
    return jc2::Value(static_cast<double>(buf.size())).get_handle();
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

FUNC(readFile) {
    (void)argc;
    if (!jc2::Value(argv[0]).is_string()) jc2::throw_error("Type Error: expects string path.");
    std::string path = jc2::Value(argv[0]).as_string();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) jc2::throw_error("IO Error: Cannot open file '" + path + "'.");
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (f.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return makeBytesInstance(std::move(buffer)).get_handle();
    }
    jc2::throw_error("IO Error: Failed to read file.");
    return jc2::Value().get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_bytesClass = new jc2::Class("Bytes");
    mod.register_value("Bytes", *g_bytesClass);

    g_bytesClass->bind_method("writeFile", bytes_writeFile, 1, 1, false, {"path"});
    g_bytesClass->bind_method("save", bytes_writeFile, 1, 1, false, {"path"});
    g_bytesClass->bind_method("set", bytes_set, 3, 3, false, {"offset", "val", "type"});
    g_bytesClass->bind_method("write_arr", bytes_write_arr, 3, 3, false, {"offset", "arr", "type"});
    g_bytesClass->bind_method("get", bytes_get, 2, 3, false, {"offset", "type", "len"});
    g_bytesClass->bind_method("len", bytes_len, 0, 0, false);
    g_bytesClass->bind_method("length", bytes_len, 0, 0, false);
    
    g_bytesClass->bind_method("seek", bytes_seek, 1, 1, false, {"pos"});
    g_bytesClass->bind_method("skip", bytes_skip, 1, 1, false, {"n"});
    g_bytesClass->bind_method("tell", bytes_tell, 0, 0, false);
    g_bytesClass->bind_method("isEnd", bytes_isEnd, 0, 0, false);
    
    g_bytesClass->bind_method("writeStr", bytes_writeStr, 1, 1, false, {"s"});
    g_bytesClass->bind_method("writeU8", bytes_writeU8, 1, 1, false, {"v"});
    g_bytesClass->bind_method("writeI16", bytes_writeI16, 1, 1, false, {"v"});
    g_bytesClass->bind_method("writeI32", bytes_writeI32, 1, 1, false, {"v"});
    g_bytesClass->bind_method("writeI64", bytes_writeI64, 1, 1, false, {"v"});
    g_bytesClass->bind_method("writeU64", bytes_writeU64, 1, 1, false, {"v"});
    g_bytesClass->bind_method("writePcmArray", bytes_writePcmArray, 1, 1, false, {"arr"});
    
    g_bytesClass->bind_method("readStr", bytes_readStr, 1, 1, false, {"n"});
    g_bytesClass->bind_method("readU8", bytes_readU8, 0, 0, false);
    g_bytesClass->bind_method("readI16", bytes_readI16, 0, 0, false);
    g_bytesClass->bind_method("readI32", bytes_readI32, 0, 0, false);
    g_bytesClass->bind_method("readI64", bytes_readI64, 0, 0, false);
    g_bytesClass->bind_method("readU64", bytes_readU64, 0, 0, false);

    g_bytesClass->set_allocator(global_alloc);

    mod.register_function("alloc", global_alloc, 1, 1, false, {"size"});
    mod.register_function("pack", global_pack, 1, 1, false, {"arr"});
    mod.register_function("readFile", global_readFile, 1, 1, false, {"path"});
    mod.register_function("fromFile", global_readFile, 1, 1, false, {"path"});

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
        "    buf.save(path)              Flushes binary buffer to disk.\n"
        "    buf.len()                   Get the total size of the buffer in bytes.\n\n"
        "  Cursor & State Control\n"
        "  ──────────────────────\n"
        "    buf.tell()                  Returns current cursor position.\n"
        "    buf.seek(pos)               Moves cursor to the absolute byte offset `pos`.\n"
        "    buf.skip(n)                 Moves cursor forward by `n` bytes.\n"
        "    buf.isEnd()                 Returns true if cursor reached the end.\n\n"
        "  Chainable Writers (Cursor Auto-Advances)\n"
        "  ──────────────────────\n"
        "    (buf.writeStr(\"RIFF\")       Writes text bytes.\n"
        "       .writeI64(9000000000)    Writes 64-bit integer (8 bytes).\n"
        "       .writeI32(1024)          Writes 32-bit integer (4 bytes).\n"
        "       .writeI16(-2)            Writes 16-bit integer (2 bytes).\n"
        "       .writeU8(255)            Writes 8-bit unsigned integer (1 byte).\n"
        "       .writePcmArray(arr))     Bulk-writes an entire mapped Array instantly.\n\n"
        "  Readers (Cursor Auto-Advances)\n"
        "  ──────────────────────\n"
        "    str = buf.readStr(4)        Reads 4 bytes into a String.\n"
        "    my_int = buf.readI32()      Reads consecutive 4 bytes.\n"
        "    big_int = buf.readI64()     Reads consecutive 8 bytes.\n\n"
        "  Low-Level Reading & Writing (Absolute Offsets)\n"
        "  ──────────────────────\n"
        "    buf.set(offset, val, type)  Write a value into memory at `offset`.\n"
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

    return 0;
}

JC2_EXTENSION_INIT
