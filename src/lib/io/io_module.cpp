#include "../jc2_extension_cpp.h"
#include <fstream>
#include <string>
#include <algorithm>

static jc2::Class* g_fileClass = nullptr;

struct FileContext {
    std::fstream stream;
    bool is_open = false;
};

static FileContext* getFile(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("Type Error: Expected a File instance.");
    auto ptr = val.get_native_data<FileContext>();
    if (!ptr) jc2::throw_error("IO Error: Invalid File object.");
    if (!ptr->is_open) jc2::throw_error("IO Error: File is already closed.");
    return ptr;
}

#define METHOD(name) JC2_ValueHandle file_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)
#define GET_SELF \
    (void)argc; \
    auto ctx = getFile(jc2::Value(argv[0])); \
    auto& stream = ctx->stream

METHOD(read) {
    GET_SELF;
    if (argc == 1) {
        // Read all remaining
        std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return jc2::Value(content).get_handle();
    } else {
        size_t size = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
        std::string buf(size, '\0');
        stream.read(&buf[0], size);
        buf.resize(stream.gcount());
        return jc2::Value(buf).get_handle();
    }
}

METHOD(readLine) {
    GET_SELF;
    std::string line;
    if (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return jc2::Value(line).get_handle();
    }
    return jc2::Value::none().get_handle();
}

METHOD(write) {
    GET_SELF;
    std::string content = jc2::Value(argv[1]).to_string();
    stream.write(content.data(), content.size());
    return argv[0]; // return self for chaining
}

METHOD(close) {
    (void)argc;
    auto ctx = getFile(jc2::Value(argv[0]));
    if (ctx->is_open) {
        ctx->stream.close();
        ctx->is_open = false;
    }
    return jc2::Value::none().get_handle();
}

METHOD(iter) {
    (void)argc;
    return argv[0]; // return self
}

METHOD(next) {
    return file_readLine(nullptr, argc, argv, nullptr);
}

JC2_ValueHandle io_open(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.open expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string mode = "r";
    if (argc >= 2) mode = jc2::Value(argv[1]).as_string();

    std::ios_base::openmode ios_mode = (std::ios_base::openmode)0;
    if (mode == "r") ios_mode = std::ios::in;
    else if (mode == "w") ios_mode = std::ios::out | std::ios::trunc;
    else if (mode == "a") ios_mode = std::ios::out | std::ios::app;
    else if (mode == "rb") ios_mode = std::ios::in | std::ios::binary;
    else if (mode == "wb") ios_mode = std::ios::out | std::ios::trunc | std::ios::binary;
    else if (mode == "ab") ios_mode = std::ios::out | std::ios::app | std::ios::binary;
    else if (mode == "r+") ios_mode = std::ios::in | std::ios::out;
    else if (mode == "w+") ios_mode = std::ios::in | std::ios::out | std::ios::trunc;
    else if (mode == "a+") ios_mode = std::ios::in | std::ios::out | std::ios::app;
    else jc2::throw_error("IO Error: Unsupported mode '" + mode + "'.");

    auto ctx = new FileContext();
    ctx->stream.open(path, ios_mode);
    if (!ctx->stream.is_open()) {
        delete ctx;
        jc2::throw_error("IO Error: Cannot open file '" + path + "'.");
    }
    ctx->is_open = true;

    jc2::Instance inst(*g_fileClass);
    inst.set_native_data(ctx, [](void* p) {
        auto c = static_cast<FileContext*>(p);
        if (c->is_open) c->stream.close();
        delete c;
    });
    return inst.get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_fileClass = new jc2::Class("File");
    mod.register_value("File", *g_fileClass);

    g_fileClass->bind_method("read", file_read, 0, 1, false, {"size"});
    g_fileClass->bind_method("readLine", file_readLine, 0, 0, false);
    g_fileClass->bind_method("write", file_write, 1, 1, false, {"content"});
    g_fileClass->bind_method("close", file_close, 0, 0, false);
    g_fileClass->bind_method("__iter__", file_iter, 0, 0, false);
    g_fileClass->bind_method("__next__", file_next, 0, 0, false);

    mod.register_function("open", io_open, 1, 2, false, {"path", "mode"});

    mod.register_help("io",
        "═══ Industrial I/O Engine — Native Module ═══\n\n"
        "  Requires: import io\n\n"
        "  The `io` module provides high-performance, object-oriented file streams.\n"
        "  \n"
        "  File Streams\n"
        "  ──────────────────────\n"
        "    file = io.open(path, [mode])  Opens a file and returns a File instance.\n"
        "                                  Modes: \"r\", \"w\", \"a\", \"rb\", \"wb\", \"ab\", \"r+\", \"w+\", \"a+\".\n"
        "                                  Default mode is \"r\".\n\n"
        "    file.read([size])             Reads `size` characters/bytes. If omitted, reads to EOF.\n"
        "    file.readLine()               Reads a single line. Returns `none` at EOF.\n"
        "    file.write(content)           Writes content to the file.\n"
        "    file.close()                  Closes the file handle. (Auto-closed by GC if forgotten).\n\n"
        "  Iteration\n"
        "  ──────────────────────\n"
        "    File instances are iterable! You can loop over them line-by-line:\n"
        "      for (line in io.open(\"data.txt\")) {\n"
        "          print(line)\n"
        "      }\n"
    );

    mod.register_function_help("io.open", "io.open(path, [mode])", "Opens a file and returns a File stream instance.", "f = io.open(\"data.txt\", \"r\")");
    mod.register_function_help("File.read", "file.read([size])", "Reads characters from the file. Reads all remaining if size is omitted.", "content = f.read()");
    mod.register_function_help("File.readLine", "file.readLine()", "Reads a single line from the file. Returns none at EOF.", "line = f.readLine()");
    mod.register_function_help("File.write", "file.write(content)", "Writes a string to the file.", "f.write(\"Hello\")");
    mod.register_function_help("File.close", "file.close()", "Closes the file stream.", "f.close()");

    return 0;
}

JC2_EXTENSION_INIT
