#include "../jc2_extension_cpp.h"
#include <fstream>
#include <string>
#include <algorithm>
#include <filesystem>

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
    return jc2::Value().get_handle();
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
    return jc2::Value().get_handle();
}

METHOD(readBuf) {
    GET_SELF;
    if (argc < 2) jc2::throw_error("Type Error: readBuf expects a buffer object.");
    size_t bsize = 0;
    void* bdata = jc2::Value(argv[1]).get_buffer_data(&bsize);
    if (!bdata) jc2::throw_error("Type Error: Expected a valid buffer object (e.g., from bytes module).");

    size_t read_size = bsize;
    if (argc >= 3) {
        read_size = static_cast<size_t>(std::max(0.0, jc2::Value(argv[2]).as_double()));
        if (read_size > bsize) jc2::throw_error("IO Error: Requested read size exceeds buffer capacity.");
    }
    size_t offset = 0;
    if (argc >= 4) {
        offset = static_cast<size_t>(std::max(0.0, jc2::Value(argv[3]).as_double()));
        if (offset + read_size > bsize) jc2::throw_error("IO Error: Read offset + size exceeds buffer capacity.");
    }

    stream.read(static_cast<char*>(bdata) + offset, read_size);
    return jc2::Value(static_cast<double>(stream.gcount())).get_handle();
}

METHOD(writeBuf) {
    GET_SELF;
    if (argc < 2) jc2::throw_error("Type Error: writeBuf expects a buffer object.");
    size_t bsize = 0;
    void* bdata = jc2::Value(argv[1]).get_buffer_data(&bsize);
    if (!bdata) jc2::throw_error("Type Error: Expected a valid buffer object (e.g., from bytes module).");

    size_t write_size = bsize;
    if (argc >= 3) {
        write_size = static_cast<size_t>(std::max(0.0, jc2::Value(argv[2]).as_double()));
        if (write_size > bsize) jc2::throw_error("IO Error: Requested write size exceeds buffer capacity.");
    }
    size_t offset = 0;
    if (argc >= 4) {
        offset = static_cast<size_t>(std::max(0.0, jc2::Value(argv[3]).as_double()));
        if (offset + write_size > bsize) jc2::throw_error("IO Error: Write offset + size exceeds buffer capacity.");
    }

    stream.write(static_cast<const char*>(bdata) + offset, write_size);
    return argv[0];
}

METHOD(seek) {
    GET_SELF;
    if (argc < 2) jc2::throw_error("Type Error: seek expects an offset.");
    long long offset = std::stoll(jc2::Value(argv[1]).to_string());
    int origin = 0;
    if (argc >= 3) origin = static_cast<int>(std::round(jc2::Value(argv[2]).as_double()));
    
    std::ios_base::seekdir dir = std::ios_base::beg;
    if (origin == 1) dir = std::ios_base::cur;
    else if (origin == 2) dir = std::ios_base::end;
    
    stream.clear();
    stream.seekg(offset, dir);
    stream.seekp(offset, dir);
    return argv[0];
}

METHOD(tell) {
    GET_SELF;
    if (stream.tellg() != -1) return jc2::Value(static_cast<double>(stream.tellg())).get_handle();
    return jc2::Value(static_cast<double>(stream.tellp())).get_handle();
}

METHOD(flush) {
    GET_SELF;
    stream.flush();
    return argv[0];
}

METHOD(isEOF) {
    GET_SELF;
    return jc2::Value(stream.eof()).get_handle();
}

METHOD(iter) {
    (void)argc;
    return argv[0]; // return self
}

METHOD(next) {
    return file_readLine(nullptr, argc, argv, nullptr);
}

JC2_ValueHandle io_stat(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.stat expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::error_code ec;
    auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(st)) jc2::throw_error("IO Error: Cannot stat path '" + path + "'.");
    
    jc2::Dict d;
    d.set(jc2::Value("is_dir"), jc2::Value(std::filesystem::is_directory(st)));
    d.set(jc2::Value("is_file"), jc2::Value(std::filesystem::is_regular_file(st)));
    if (std::filesystem::is_regular_file(st)) {
        d.set(jc2::Value("size"), jc2::Value(static_cast<double>(std::filesystem::file_size(path, ec))));
    } else {
        d.set(jc2::Value("size"), jc2::Value(0.0));
    }
    return d.get_handle();
}

JC2_ValueHandle io_mkdir(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.mkdir expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    bool recursive = false;
    if (argc >= 2) recursive = jc2::Value(argv[1]).truthy();
    std::error_code ec;
    if (recursive) std::filesystem::create_directories(path, ec);
    else std::filesystem::create_directory(path, ec);
    if (ec) jc2::throw_error("IO Error: Cannot create directory '" + path + "'.");
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_remove(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.remove expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) jc2::throw_error("IO Error: Cannot remove '" + path + "'.");
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_rename(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 2) jc2::throw_error("Type Error: io.rename expects old and new paths.");
    std::string old_p = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string new_p = jc2::Env::resolve_path(jc2::Value(argv[1]).as_string());
    std::error_code ec;
    std::filesystem::rename(old_p, new_p, ec);
    if (ec) jc2::throw_error("IO Error: Cannot rename '" + old_p + "' to '" + new_p + "'.");
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_exists(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.exists expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::error_code ec;
    return jc2::Value(std::filesystem::exists(path, ec)).get_handle();
}

JC2_ValueHandle io_listDir(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    std::string path = jc2::Env::resolve_path(argc >= 1 ? jc2::Value(argv[0]).as_string() : ".");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
        jc2::throw_error("IO Error: Directory '" + path + "' does not exist.");
    }
    jc2::List l;
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        l.push_back(jc2::Value(entry.path().filename().string()));
    }
    return l.get_handle();
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
    g_fileClass->bind_method("readBuf", file_readBuf, 1, 3, false, {"buf", "size", "offset"});
    g_fileClass->bind_method("writeBuf", file_writeBuf, 1, 3, false, {"buf", "size", "offset"});
    g_fileClass->bind_method("seek", file_seek, 1, 2, false, {"offset", "origin"});
    g_fileClass->bind_method("tell", file_tell, 0, 0, false);
    g_fileClass->bind_method("flush", file_flush, 0, 0, false);
    g_fileClass->bind_method("isEOF", file_isEOF, 0, 0, false);
    g_fileClass->bind_method("close", file_close, 0, 0, false);
    g_fileClass->bind_method("__iter__", file_iter, 0, 0, false);
    g_fileClass->bind_method("__next__", file_next, 0, 0, false);

    mod.register_function("open", io_open, 1, 2, false, {"path", "mode"});
    mod.register_function("stat", io_stat, 1, 1, false, {"path"});
    mod.register_function("mkdir", io_mkdir, 1, 2, false, {"path", "recursive"});
    mod.register_function("remove", io_remove, 1, 1, false, {"path"});
    mod.register_function("rename", io_rename, 2, 2, false, {"old_path", "new_path"});
    mod.register_function("exists", io_exists, 1, 1, false, {"path"});
    mod.register_function("listDir", io_listDir, 0, 1, false, {"path"});

    mod.register_help("io",
        "═══ Industrial I/O Engine — Native Module ═══\n\n"
        "  Requires: import io\n\n"
        "  The `io` module provides high-performance, object-oriented file streams.\n"
        "  \n"
        "  File Streams (Text & Basic)\n"
        "  ──────────────────────\n"
        "    file = io.open(path, [mode])  Opens a file and returns a File instance.\n"
        "                                  Modes: \"r\", \"w\", \"a\", \"rb\", \"wb\", \"ab\", \"r+\", \"w+\", \"a+\".\n"
        "                                  Default mode is \"r\".\n\n"
        "    file.read([size])             Reads `size` characters/bytes. If omitted, reads to EOF.\n"
        "    file.readLine()               Reads a single line. Returns `none` at EOF.\n"
        "    file.write(content)           Writes content to the file.\n"
        "    file.close()                  Closes the file handle. (Auto-closed by GC if forgotten).\n\n"
        "  Zero-Copy Binary I/O (Buffer Protocol)\n"
        "  ──────────────────────\n"
        "    file.readBuf(buf, [sz], [off])  Reads directly into a `bytes` buffer. Returns bytes read.\n"
        "    file.writeBuf(buf, [sz], [off]) Writes directly from a `bytes` buffer to the file.\n\n"
        "  Cursor & State Control\n"
        "  ──────────────────────\n"
        "    file.seek(offset, [origin])   Moves the file cursor. Origin: 0 (beg), 1 (cur), 2 (end).\n"
        "    file.tell()                   Returns the current cursor position.\n"
        "    file.flush()                  Flushes the output buffer to disk.\n"
        "    file.isEOF()                  Returns true if the end of the file has been reached.\n\n"
        "  Iteration\n"
        "  ──────────────────────\n"
        "    File instances are iterable! You can loop over them line-by-line:\n"
        "      for (line in io.open(\"data.txt\")) {\n"
        "          print(line)\n"
        "      }\n\n"
        "  Filesystem Operations\n"
        "  ──────────────────────\n"
        "    io.stat(path)                 Returns a dict with file metadata (size, is_dir, is_file).\n"
        "    io.mkdir(path, [recursive])   Creates a directory. Set recursive=true to create parent dirs.\n"
        "    io.remove(path)               Deletes a file or empty directory.\n"
        "    io.rename(old, new)           Renames or moves a file/directory.\n"
        "    io.exists(path)               Returns true if the path exists.\n"
        "    io.listDir([path])            Returns a list of filenames in the directory.\n"
    );

    mod.register_function_help("io.open", "io.open(path, [mode])", "Opens a file and returns a File stream instance.", "f = io.open(\"data.txt\", \"r\")");
    mod.register_function_help("io.stat", "io.stat(path)", "Returns a dictionary with file metadata.", "info = io.stat(\"data.txt\")");
    mod.register_function_help("io.mkdir", "io.mkdir(path, [recursive])", "Creates a directory.", "io.mkdir(\"new_folder\")");
    mod.register_function_help("io.remove", "io.remove(path)", "Deletes a file or empty directory.", "io.remove(\"old.txt\")");
    mod.register_function_help("io.rename", "io.rename(old_path, new_path)", "Renames or moves a file or directory.", "io.rename(\"a.txt\", \"b.txt\")");
    mod.register_function_help("io.exists", "io.exists(path)", "Returns true if the file or directory exists.", "io.exists(\"data.txt\")");
    mod.register_function_help("io.listDir", "io.listDir([path])", "Returns a list of filenames in the specified directory.", "files = io.listDir(\".\")");
    mod.register_function_help("File.read", "file.read([size])", "Reads characters from the file. Reads all remaining if size is omitted.", "content = f.read()");
    mod.register_function_help("File.readLine", "file.readLine()", "Reads a single line from the file. Returns none at EOF.", "line = f.readLine()");
    mod.register_function_help("File.write", "file.write(content)", "Writes a string to the file.", "f.write(\"Hello\")");
    mod.register_function_help("File.readBuf", "file.readBuf(buf, [size], [offset])", "Reads binary data directly into a buffer object (zero-copy).", "f.readBuf(buf)");
    mod.register_function_help("File.writeBuf", "file.writeBuf(buf, [size], [offset])", "Writes binary data directly from a buffer object (zero-copy).", "f.writeBuf(buf)");
    mod.register_function_help("File.seek", "file.seek(offset, [origin])", "Moves the file cursor. Origin: 0 (beg), 1 (cur), 2 (end).", "f.seek(0)");
    mod.register_function_help("File.tell", "file.tell()", "Returns the current cursor position.", "pos = f.tell()");
    mod.register_function_help("File.close", "file.close()", "Closes the file stream.", "f.close()");

    return 0;
}

JC2_EXTENSION_INIT
