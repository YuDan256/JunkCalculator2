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

static std::vector<uint8_t>* getBuf(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("Type Error: Expected a Bytes instance.");
    auto ptr = val.get_native_data<std::vector<uint8_t>>();
    if (!ptr) jc2::throw_error("Type Error: Expected a Bytes native object.");
    return ptr;
}

static jc2::Value makeBytesInstance(std::vector<uint8_t> data) {
    jc2::Instance inst(*g_bytesClass);
    auto ptr = new std::vector<uint8_t>(std::move(data));
    inst.set_native_data(ptr, [](void* p) {
        delete static_cast<std::vector<uint8_t>*>(p);
    });
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
#define GET_SELF auto buf = getBuf(jc2::Value(argv[0]))

METHOD(writeFile) {
    (void)argc;
    GET_SELF;
    if (!jc2::Value(argv[1]).is_string()) jc2::throw_error("Type Error: expects string path.");
    std::string path = jc2::Value(argv[1]).as_string();
    std::ofstream f(path, std::ios::binary);
    if (!f) jc2::throw_error("IO Error: Cannot write to file '" + path + "'.");
    f.write(reinterpret_cast<const char*>(buf->data()), buf->size());
    return jc2::Value().get_handle();
}

METHOD(set) {
    (void)argc;
    GET_SELF;
    size_t offset = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
    if (!jc2::Value(argv[3]).is_string()) jc2::throw_error("Type Error: type must be string.");
    std::string type = jc2::Value(argv[3]).as_string();

    auto writeMem = [&](const void* data, size_t size) {
        if (offset + size > buf->size()) jc2::throw_error("Buffer Error: Write out of bounds.");
        std::memcpy(buf->data() + offset, data, size);
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
    (void)argc;
    GET_SELF;
    size_t offset = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
    auto arr = extractDS(jc2::Value(argv[2]), "write_arr");
    if (!jc2::Value(argv[3]).is_string()) jc2::throw_error("Type Error: type must be string.");
    std::string type = jc2::Value(argv[3]).as_string();

    if (type == "i16") {
        if (offset + arr.size() * 2 > buf->size()) jc2::throw_error("Buffer out of bounds.");
        int16_t* ptr = reinterpret_cast<int16_t*>(buf->data() + offset);
        for (size_t i = 0; i < arr.size(); ++i) {
            double val = std::max(-1.0, std::min(1.0, arr[i]));
            ptr[i] = static_cast<int16_t>(val * 32767.0);
        }
    } else if (type == "f64") {
        if (offset + arr.size() * 8 > buf->size()) jc2::throw_error("Buffer out of bounds.");
        std::memcpy(buf->data() + offset, arr.data(), arr.size() * sizeof(double));
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
        if (offset + size > buf->size()) jc2::throw_error("Buffer Error: Read out of bounds.");
        std::memcpy(data, buf->data() + offset, size);
    };

    if (type == "str") {
        if (argc != 4) jc2::throw_error("Buffer Error: String read requires length.");
        size_t len = static_cast<size_t>(std::max(0.0, jc2::Value(argv[3]).as_double()));
        if (offset + len > buf->size()) jc2::throw_error("Buffer Error: Read out of bounds.");
        return jc2::Value(std::string(reinterpret_cast<char*>(buf->data() + offset), len)).get_handle();
    }

    if (argc != 3) jc2::throw_error("Buffer Error: Incorrect argument count.");
    if (type == "u8") { uint8_t  v; readMem(&v, 1); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "i8") { int8_t   v; readMem(&v, 1); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "u16") { uint16_t v; readMem(&v, 2); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "i16") { int16_t  v; readMem(&v, 2); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "u32") { uint32_t v; readMem(&v, 4); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "i32") { int32_t  v; readMem(&v, 4); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "i64") { int64_t  v; readMem(&v, 8); return jc2::BigInt(std::to_string(v)).get_handle(); }
    else if (type == "u64") { uint64_t v; readMem(&v, 8); return jc2::BigInt(std::to_string(v)).get_handle(); }
    else if (type == "f32") { float    v; readMem(&v, 4); return jc2::Value(static_cast<double>(v)).get_handle(); }
    else if (type == "f64") { double   v; readMem(&v, 8); return jc2::Value(v).get_handle(); }
    else jc2::throw_error("Buffer Error: Unknown format type '" + type + "'.");
    return jc2::Value().get_handle();
}

METHOD(len) {
    (void)argc;
    GET_SELF;
    return jc2::Value(static_cast<double>(buf->size())).get_handle();
}

METHOD(address) {
    (void)argc;
    GET_SELF;
    uint64_t addr = reinterpret_cast<uint64_t>(buf->data());
    return jc2::BigInt(std::to_string(addr)).get_handle();
}

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

    g_bytesClass->bind_method("writeFile", bytes_writeFile, 1, 1, false);
    g_bytesClass->bind_method("set", bytes_set, 3, 3, false);
    g_bytesClass->bind_method("write_arr", bytes_write_arr, 3, 3, false);
    g_bytesClass->bind_method("get", bytes_get, 2, 3, false);
    g_bytesClass->bind_method("len", bytes_len, 0, 0, false);
    g_bytesClass->bind_method("address", bytes_address, 0, 0, false);

    g_bytesClass->set_allocator(global_alloc);

    mod.register_function("alloc", global_alloc, 1, 1, false);
    mod.register_function("pack", global_pack, 1, 1, false);
    mod.register_function("readFile", global_readFile, 1, 1, false);

    mod.register_help("bytes",
        "═══ Bare-Metal Memory Engine — Native Module ═══\n\n"
        "  Requires: import bytes\n\n"
        "  The `bytes` module provides low-level, zero-dependency C++ memory buffers. \n"
        "  It grants absolute control over binary reading, writing, and file I/O.\n"
        "  \n"
        "  ★ Note: For everyday use, it is highly recommended to use the standard \n"
        "    library wrapper `import buffer`, which provides an OOP interface.\n\n"
        "  Buffer Allocation & I/O\n"
        "  ──────────────────────\n"
        "    bytes.Bytes(size)           Allocate a zeroed buffer of `size` bytes.\n"
        "    bytes.alloc(size)           Legacy alias for bytes.Bytes.\n"
        "    bytes.pack(array)           Create a buffer from an array of 8-bit integers.\n"
        "    bytes.readFile(path)        Map an entire file from disk into a buffer.\n"
        "    buf.writeFile(path)         Flush a byte buffer directly to your disk.\n"
        "    buf.len()                   Get the total size of the buffer in bytes.\n"
        "    buf.address()               Get the raw memory address (for FFI).\n\n"
        "  Low-Level Reading & Writing (Absolute Offsets)\n"
        "  ──────────────────────\n"
        "    buf.set(offset, val, type)  Write a value into memory at `offset`.\n"
        "    buf.get(offset, type)       Read a value from memory at `offset`.\n"
        "    buf.get(offset, \"str\", len) Read specifically a string of `len` bytes.\n\n"
        "    Supported formats (passed as strings): \n"
        "      \"u8\", \"i8\", \"u16\", \"i16\", \"u32\", \"i32\", \"i64\", \"u64\", \"f32\", \"f64\", \"str\"\n\n"
        "  High-Performance Bulk Operations\n"
        "  ──────────────────────\n"
        "    buf.write_arr(off, arr, type)  \n"
        "        Directly memcpy a JC2 Array/Matrix into memory at C++ speeds. \n"
        "        (Currently supports \"i16\" and \"f64\"). \n"
        "        Provides massive performance boosts for audio/data generation.\n\n"
        "  Example\n"
        "  ──────────────────────\n"
        "    import bytes\n"
        "    b = bytes.alloc(4)\n"
        "    b.set(0, 255, \"u8\")\n"
        "    b.set(1, 65535, \"u16\") \n"
        "    b.get(1, \"u16\")             → 65535"
    );

    mod.register_function_help("bytes.Bytes", "bytes.Bytes(size)", "Allocates a zeroed byte buffer of the specified size.", "bytes.Bytes(1024)");
    mod.register_function_help("bytes.alloc", "bytes.alloc(size)", "Legacy alias for bytes.Bytes.", "bytes.alloc(1024)");
    mod.register_function_help("bytes.pack", "bytes.pack(array)", "Creates a byte buffer from an array of 8-bit integers.", "bytes.pack([255, 0, 128])");
    mod.register_function_help("bytes.readFile", "bytes.readFile(path)", "Reads an entire file into a byte buffer.", "bytes.readFile(\"data.bin\")");
    mod.register_function_help("bytes.writeFile", "buf.writeFile(path)", "Writes a byte buffer to a file.", "buf.writeFile(\"out.bin\")");
    mod.register_function_help("bytes.set", "buf.set(offset, val, type)", "Writes a value into the buffer at the specified offset.", "buf.set(0, 255, \"u8\")");
    mod.register_function_help("bytes.write_arr", "buf.write_arr(offset, arr, type)", "Directly copies an array into the buffer at C++ speeds.", "buf.write_arr(0, data, \"f64\")");
    mod.register_function_help("bytes.get", "buf.get(offset, type, [len])", "Reads a value from the buffer at the specified offset.", "buf.get(0, \"u16\")");
    mod.register_function_help("bytes.len", "buf.len()", "Returns the total size of the buffer in bytes.", "buf.len()");
    mod.register_function_help("bytes.address", "buf.address()", "Returns the raw memory address of the buffer as a 64-bit integer.", "buf.address()");

    return 0;
}

JC2_EXTENSION_INIT
