#include "../jc2_extension_cpp.h"
#include <fstream>
#include <string>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

static std::string decode_to_utf8(const std::string& raw, const std::string& enc) {
    if (raw.empty()) return "";
    std::string lower_enc = enc;
    std::transform(lower_enc.begin(), lower_enc.end(), lower_enc.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });

    if (lower_enc == "utf-8" || lower_enc == "utf8") {
        // 自动探测并静默剥离 UTF-8 BOM 头
        if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
            return raw.substr(3);
        }
        return raw;
    }
    
#ifdef _WIN32
    if (lower_enc == "ansi" || lower_enc == "gbk" || lower_enc == "system") {
        int wlen = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), NULL, 0);
        if (wlen <= 0) return raw;
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &wstr[0], wlen);
        
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
        if (u8len <= 0) return raw;
        std::string u8str(u8len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &u8str[0], u8len, NULL, NULL);
        return u8str;
    }
    if (lower_enc == "utf-16le" || lower_enc == "utf-16") {
        size_t offset = 0;
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) offset = 2;
        int wlen = (int)(raw.size() - offset) / 2;
        if (wlen <= 0) return "";
        const wchar_t* wptr = reinterpret_cast<const wchar_t*>(raw.data() + offset);
        
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wptr, wlen, NULL, 0, NULL, NULL);
        if (u8len <= 0) return "";
        std::string u8str(u8len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wptr, wlen, &u8str[0], u8len, NULL, NULL);
        return u8str;
    }
    if (lower_enc == "utf-16be") {
        size_t offset = 0;
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) offset = 2;
        int wlen = (int)(raw.size() - offset) / 2;
        if (wlen <= 0) return "";
        std::wstring wstr(wlen, 0);
        const uint8_t* bptr = reinterpret_cast<const uint8_t*>(raw.data() + offset);
        for (int i = 0; i < wlen; ++i) {
            wstr[i] = (bptr[i*2] << 8) | bptr[i*2+1];
        }
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wlen, NULL, 0, NULL, NULL);
        if (u8len <= 0) return "";
        std::string u8str(u8len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wlen, &u8str[0], u8len, NULL, NULL);
        return u8str;
    }
#endif
    return raw;
}

static std::string encode_from_utf8(const std::string& u8str, const std::string& enc) {
    if (u8str.empty()) return "";
    std::string lower_enc = enc;
    std::transform(lower_enc.begin(), lower_enc.end(), lower_enc.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });

    if (lower_enc == "utf-8" || lower_enc == "utf8") return u8str;

#ifdef _WIN32
    if (lower_enc == "ansi" || lower_enc == "gbk" || lower_enc == "system") {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, u8str.data(), (int)u8str.size(), NULL, 0);
        if (wlen <= 0) return u8str;
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, u8str.data(), (int)u8str.size(), &wstr[0], wlen);
        
        int alen = WideCharToMultiByte(CP_ACP, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
        if (alen <= 0) return u8str;
        std::string astr(alen, 0);
        WideCharToMultiByte(CP_ACP, 0, wstr.data(), (int)wstr.size(), &astr[0], alen, NULL, NULL);
        return astr;
    }
    if (lower_enc == "utf-16le" || lower_enc == "utf-16") {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, u8str.data(), (int)u8str.size(), NULL, 0);
        if (wlen <= 0) return "";
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, u8str.data(), (int)u8str.size(), &wstr[0], wlen);
        
        std::string res(wlen * 2, 0);
        std::memcpy(&res[0], wstr.data(), wlen * 2);
        return res;
    }
    if (lower_enc == "utf-16be") {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, u8str.data(), (int)u8str.size(), NULL, 0);
        if (wlen <= 0) return "";
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, u8str.data(), (int)u8str.size(), &wstr[0], wlen);
        
        std::string res(wlen * 2, 0);
        for (int i = 0; i < wlen; ++i) {
            res[i*2] = (wstr[i] >> 8) & 0xFF;
            res[i*2+1] = wstr[i] & 0xFF;
        }
        return res;
    }
#endif
    return u8str;
}

static std::filesystem::path to_path(const std::string& utf8_str) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8_str.c_str()));
}

static std::string from_path(const std::filesystem::path& p) {
    auto u8str = p.u8string();
    return std::string(u8str.begin(), u8str.end());
}

static jc2::Class* g_fileClass = nullptr;

struct FileContext {
    std::fstream stream;
    bool is_open = false;
    std::string encoding = "utf-8";
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
        return jc2::Value(decode_to_utf8(content, ctx->encoding)).get_handle();
    } else {
        size_t size = static_cast<size_t>(std::max(0.0, jc2::Value(argv[1]).as_double()));
        std::string buf(size, '\0');
        stream.read(&buf[0], size);
        buf.resize(stream.gcount());
        return jc2::Value(decode_to_utf8(buf, ctx->encoding)).get_handle();
    }
}

METHOD(readLine) {
    GET_SELF;
    std::string lower_enc = ctx->encoding;
    std::transform(lower_enc.begin(), lower_enc.end(), lower_enc.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
    
    if (lower_enc == "utf-16le" || lower_enc == "utf-16") {
        std::string raw;
        char c[2];
        while (stream.read(c, 2)) {
            raw.push_back(c[0]);
            raw.push_back(c[1]);
            if (c[0] == '\n' && c[1] == '\0') break;
        }
        if (raw.empty()) return jc2::Value().get_handle();
        std::string decoded = decode_to_utf8(raw, ctx->encoding);
        if (!decoded.empty() && decoded.back() == '\n') decoded.pop_back();
        if (!decoded.empty() && decoded.back() == '\r') decoded.pop_back();
        return jc2::Value(decoded).get_handle();
    } else if (lower_enc == "utf-16be") {
        std::string raw;
        char c[2];
        while (stream.read(c, 2)) {
            raw.push_back(c[0]);
            raw.push_back(c[1]);
            if (c[0] == '\0' && c[1] == '\n') break;
        }
        if (raw.empty()) return jc2::Value().get_handle();
        std::string decoded = decode_to_utf8(raw, ctx->encoding);
        if (!decoded.empty() && decoded.back() == '\n') decoded.pop_back();
        if (!decoded.empty() && decoded.back() == '\r') decoded.pop_back();
        return jc2::Value(decoded).get_handle();
    } else {
        std::string line;
        if (std::getline(stream, line)) {
            std::string decoded = decode_to_utf8(line, ctx->encoding);
            if (!decoded.empty() && decoded.back() == '\r') decoded.pop_back();
            return jc2::Value(decoded).get_handle();
        }
        return jc2::Value().get_handle();
    }
}

METHOD(write) {
    GET_SELF;
    std::string content = jc2::Value(argv[1]).to_string();
    std::string encoded = encode_from_utf8(content, ctx->encoding);
    stream.write(encoded.data(), encoded.size());
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
    long long offset = 0;
    if (jc2::Value(argv[1]).is_double() || jc2::Value(argv[1]).is_int()) {
        offset = static_cast<long long>(std::round(jc2::Value(argv[1]).as_double()));
    } else {
        offset = std::stoll(jc2::Value(argv[1]).to_string());
    }
    int origin = 0;
    if (argc >= 3) origin = static_cast<int>(std::round(jc2::Value(argv[2]).as_double()));
    
    stream.clear();
    if (origin == 0) {
        stream.seekg(static_cast<std::streampos>(offset));
        stream.seekp(static_cast<std::streampos>(offset));
    } else {
        std::ios_base::seekdir dir = (origin == 1) ? std::ios_base::cur : std::ios_base::end;
        stream.seekg(static_cast<std::streamoff>(offset), dir);
        stream.seekp(static_cast<std::streamoff>(offset), dir);
    }
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

static std::vector<std::vector<std::string>> parseCSV(const std::string& path, char delim, const std::string& encoding) {
    std::ifstream file(to_path(path), std::ios::binary);
    if (!file.is_open()) jc2::throw_error("IO Error: Cannot open file '" + path + "'.");
    
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    content = decode_to_utf8(content, encoding);
    
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> current_row;
    std::string current_field;
    bool in_quotes = false;
    
    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < content.size() && content[i+1] == '"') {
                    current_field += '"';
                    ++i; // consume second quote
                } else {
                    in_quotes = false;
                }
            } else {
                current_field += c;
            }
        } else {
            if (c == '"') {
                in_quotes = true;
            } else if (c == delim) {
                current_row.push_back(current_field);
                current_field.clear();
            } else if (c == '\n') {
                current_row.push_back(current_field);
                current_field.clear();
                rows.push_back(current_row);
                current_row.clear();
            } else if (c != '\r') {
                current_field += c;
            }
        }
    }
    if (!current_field.empty() || !current_row.empty()) {
        current_row.push_back(current_field);
        rows.push_back(current_row);
    }
    return rows;
}

JC2_ValueHandle io_readCSV(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.readCSV expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    char delim = ',';
    if (argc >= 2) {
        std::string d = jc2::Value(argv[1]).as_string();
        if (!d.empty()) delim = d[0];
    }
    std::string encoding = "utf-8";
    if (argc >= 3) encoding = jc2::Value(argv[2]).as_string();
    
    auto rowsData = parseCSV(path, delim, encoding);
    jc2::List rows;
    for (const auto& row : rowsData) {
        jc2::List rowList;
        for (const auto& s : row) rowList.push_back(jc2::Value(s));
        rowList.freeze();
        rows.push_back(jc2::Value(rowList.get_handle()));
    }
    return rows.get_handle();
}

JC2_ValueHandle io_parseCSVNum(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.parseCSVNum expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    char delim = ',';
    if (argc >= 2) {
        std::string d = jc2::Value(argv[1]).as_string();
        if (!d.empty()) delim = d[0];
    }
    std::string encoding = "utf-8";
    if (argc >= 3) encoding = jc2::Value(argv[2]).as_string();
    
    auto rowsData = parseCSV(path, delim, encoding);
    if (rowsData.empty()) return jc2::RealMatrix(0, 0).get_handle();
    
    size_t maxCols = 0;
    for (const auto& row : rowsData) if (row.size() > maxCols) maxCols = row.size();
    
    jc2::RealMatrix mat(static_cast<int>(rowsData.size()), static_cast<int>(maxCols));
    for (size_t i = 0; i < rowsData.size(); ++i) {
        for (size_t j = 0; j < maxCols; ++j) {
            double val = 0.0;
            if (j < rowsData[i].size() && !rowsData[i][j].empty()) {
                try { val = std::stod(rowsData[i][j]); } catch (...) {}
            }
            mat.set(static_cast<int>(i), static_cast<int>(j), val);
        }
    }
    return mat.get_handle();
}

JC2_ValueHandle io_writeCSV(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 2) jc2::throw_error("Type Error: io.writeCSV expects path and data.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string delim = ",";
    if (argc >= 3) delim = jc2::Value(argv[2]).as_string();
    std::string encoding = "utf-8";
    if (argc >= 4) encoding = jc2::Value(argv[3]).as_string();
    
    std::ofstream file(to_path(path), std::ios::binary);
    if (!file.is_open()) jc2::throw_error("IO Error: Cannot write to file '" + path + "'.");
    
    jc2::Value data = jc2::Value(argv[1]);
    std::ostringstream oss;
    
    auto escapeCSV = [&](const std::string& s) {
        if (s.find(delim) != std::string::npos || s.find('"') != std::string::npos || s.find('\n') != std::string::npos || s.find('\r') != std::string::npos) {
            std::string res = "\"";
            for (char c : s) {
                if (c == '"') res += "\"\"";
                else res += c;
            }
            res += "\"";
            return res;
        }
        return s;
    };

    if (data.is_real_matrix()) {
        jc2::RealMatrix m(data.get_handle());
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                if (j > 0) oss << delim;
                oss << m.get(i, j);
            }
            oss << "\r\n";
        }
    } else if (data.is_complex_matrix()) {
        jc2::ComplexMatrix m(data.get_handle());
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                if (j > 0) oss << delim;
                double r = m.get_real(i, j), im = m.get_imag(i, j);
                if (im == 0) oss << r;
                else if (r == 0) oss << im << "i";
                else oss << r << (im > 0 ? "+" : "") << im << "i";
            }
            oss << "\r\n";
        }
    } else if (data.is_list()) {
        jc2::List l = jc2::List(data.get_handle());
        for (size_t i = 0; i < l.size(); ++i) {
            jc2::Value row = l.get(i);
            if (row.is_list()) {
                jc2::List rowList(row.get_handle());
                for (size_t j = 0; j < rowList.size(); ++j) {
                    if (j > 0) oss << delim;
                    oss << escapeCSV(rowList.get(j).to_string());
                }
            } else {
                oss << escapeCSV(row.to_string());
            }
            oss << "\r\n";
        }
    } else {
        jc2::throw_error("Type Error: writeCSV expects a matrix or list.");
    }
    
    std::string encoded = encode_from_utf8(oss.str(), encoding);
    file.write(encoded.data(), encoded.size());
    
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_readFile(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)user_data;
    if (argc < 1) jc2::throw_error("Type Error: io.readFile expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string encoding = "utf-8";
    if (argc >= 2) encoding = jc2::Value(argv[1]).as_string();

    std::ifstream file(to_path(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) jc2::throw_error("IO Error: Cannot open file '" + path + "'.");
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string content(static_cast<size_t>(size), '\0');
    if (file.read(&content[0], size)) {
        return jc2::Value(decode_to_utf8(content, encoding)).get_handle();
    }
    jc2::throw_error("IO Error: Failed to read file.");
    return jc2::Value().get_handle();
}

JC2_ValueHandle io_writeFile(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)user_data;
    if (argc < 2) jc2::throw_error("Type Error: io.writeFile expects path and content.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string content = jc2::Value(argv[1]).to_string();
    std::string encoding = "utf-8";
    if (argc >= 3) encoding = jc2::Value(argv[2]).as_string();

    std::ofstream file(to_path(path), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) jc2::throw_error("IO Error: Cannot write to file '" + path + "'.");
    std::string encoded = encode_from_utf8(content, encoding);
    file.write(encoded.data(), encoded.size());
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_copy(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 2) jc2::throw_error("Type Error: io.copy expects source and destination paths.");
    std::string src = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string dst = jc2::Env::resolve_path(jc2::Value(argv[1]).as_string());
    std::error_code ec;
    std::filesystem::copy(to_path(src), to_path(dst), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) jc2::throw_error("IO Error: Cannot copy '" + src + "' to '" + dst + "'.");
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_stat(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.stat expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::error_code ec;
    auto st = std::filesystem::status(to_path(path), ec);
    if (ec || !std::filesystem::exists(st)) jc2::throw_error("IO Error: Cannot stat path '" + path + "'.");
    
    jc2::Dict d;
    d.set(jc2::Value("is_dir"), jc2::Value(std::filesystem::is_directory(st)));
    d.set(jc2::Value("is_file"), jc2::Value(std::filesystem::is_regular_file(st)));
    if (std::filesystem::is_regular_file(st)) {
        d.set(jc2::Value("size"), jc2::Value(static_cast<double>(std::filesystem::file_size(to_path(path), ec))));
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
    if (recursive) std::filesystem::create_directories(to_path(path), ec);
    else std::filesystem::create_directory(to_path(path), ec);
    if (ec) jc2::throw_error("IO Error: Cannot create directory '" + path + "'.");
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_remove(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.remove expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::error_code ec;
    std::filesystem::remove(to_path(path), ec);
    if (ec) jc2::throw_error("IO Error: Cannot remove '" + path + "'.");
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_rename(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 2) jc2::throw_error("Type Error: io.rename expects old and new paths.");
    std::string old_p = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string new_p = jc2::Env::resolve_path(jc2::Value(argv[1]).as_string());
    std::error_code ec;
    std::filesystem::rename(to_path(old_p), to_path(new_p), ec);
    if (ec) jc2::throw_error("IO Error: Cannot rename '" + old_p + "' to '" + new_p + "'.");
    return jc2::Value::none().get_handle();
}

JC2_ValueHandle io_exists(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.exists expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::error_code ec;
    return jc2::Value(std::filesystem::exists(to_path(path), ec)).get_handle();
}

JC2_ValueHandle io_listDir(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    std::string path = jc2::Env::resolve_path(argc >= 1 ? jc2::Value(argv[0]).as_string() : ".");
    std::error_code ec;
    if (!std::filesystem::exists(to_path(path), ec) || !std::filesystem::is_directory(to_path(path), ec)) {
        jc2::throw_error("IO Error: Directory '" + path + "' does not exist.");
    }
    jc2::List l;
    for (const auto& entry : std::filesystem::directory_iterator(to_path(path), ec)) {
        l.push_back(jc2::Value(from_path(entry.path().filename())));
    }
    return l.get_handle();
}

JC2_ValueHandle io_open(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("Type Error: io.open expects a path.");
    std::string path = jc2::Env::resolve_path(jc2::Value(argv[0]).as_string());
    std::string mode = "r";
    if (argc >= 2) mode = jc2::Value(argv[1]).as_string();
    std::string encoding = "utf-8";
    if (argc >= 3) encoding = jc2::Value(argv[2]).as_string();

    // 强制所有模式在底层都以 binary 模式打开，避免 Windows 下 \r\n 自动转换导致 seek/tell 游标错位
    std::ios_base::openmode ios_mode = std::ios::binary;
    if (mode == "r" || mode == "rb") ios_mode |= std::ios::in;
    else if (mode == "w" || mode == "wb") ios_mode |= std::ios::out | std::ios::trunc;
    else if (mode == "a" || mode == "ab") ios_mode |= std::ios::out | std::ios::app;
    else if (mode == "r+") ios_mode |= std::ios::in | std::ios::out;
    else if (mode == "w+") ios_mode |= std::ios::in | std::ios::out | std::ios::trunc;
    else if (mode == "a+") ios_mode |= std::ios::in | std::ios::out | std::ios::app;
    else jc2::throw_error("IO Error: Unsupported mode '" + mode + "'.");

    auto ctx = new FileContext();
    ctx->encoding = encoding;
    ctx->stream.open(to_path(path), ios_mode);
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

    g_fileClass->bind_method("read", file_read, 0, 1, {"size"});
    g_fileClass->bind_method("readLine", file_readLine, 0, 0);
    g_fileClass->bind_method("write", file_write, 1, 1, {"content"});
    g_fileClass->bind_method("readBuf", file_readBuf, 1, 3, {"buf", "size", "offset"});
    g_fileClass->bind_method("writeBuf", file_writeBuf, 1, 3, {"buf", "size", "offset"});
    g_fileClass->bind_method("seek", file_seek, 1, 2, {"offset", "origin"});
    g_fileClass->bind_method("tell", file_tell, 0, 0);
    g_fileClass->bind_method("flush", file_flush, 0, 0);
    g_fileClass->bind_method("isEOF", file_isEOF, 0, 0);
    g_fileClass->bind_method("close", file_close, 0, 0);
    g_fileClass->bind_method("__iter__", file_iter, 0, 0);
    g_fileClass->bind_method("__next__", file_next, 0, 0);

    mod.register_function("open", io_open, 1, 3, {"path", "mode", "encoding"});
    mod.register_function("readFile", io_readFile, 1, 2, {"path", "encoding"});
    mod.register_function("writeFile", io_writeFile, 2, 3, {"path", "content", "encoding"});
    mod.register_function("stat", io_stat, 1, 1, {"path"});
    mod.register_function("mkdir", io_mkdir, 1, 2, {"path", "recursive"});
    mod.register_function("remove", io_remove, 1, 1, {"path"});
    mod.register_function("rename", io_rename, 2, 2, {"old_path", "new_path"});
    mod.register_function("copy", io_copy, 2, 2, {"src", "dst"});
    mod.register_function("exists", io_exists, 1, 1, {"path"});
    mod.register_function("listDir", io_listDir, 0, 1, {"path"});
    mod.register_function("readCSV", io_readCSV, 1, 3, {"path", "delim", "encoding"});
    mod.register_function("readCSVMat", io_readCSV, 1, 3, {"path", "delim", "encoding"});
    mod.register_function("parseCSVNum", io_parseCSVNum, 1, 3, {"path", "delim", "encoding"});
    mod.register_function("writeCSV", io_writeCSV, 2, 4, {"path", "data", "delim", "encoding"});

    mod.register_help("io",
        "═══ Industrial I/O Engine — Native Module ═══\n\n"
        "  Requires: import io\n\n"
        "  The `io` module provides high-performance, object-oriented file streams.\n"
        "  \n"
        "  Quick I/O (One-Shot)\n"
        "  ──────────────────────\n"
        "    io.readFile(path, [enc])      Reads the entire file into a string.\n"
        "    io.writeFile(path, str, [enc]) Writes a string to the file (overwrites).\n\n"
        "  File Streams (Text & Basic)\n"
        "  ──────────────────────\n"
        "    file = io.open(path, [mode], [enc]) Opens a file and returns a File instance.\n"
        "                                  Modes: \"r\", \"w\", \"a\", \"rb\", \"wb\", \"ab\", \"r+\", \"w+\", \"a+\".\n"
        "                                  Encodings: \"utf-8\" (default), \"ansi\" (GBK), \"utf-16le\", \"utf-16be\".\n\n"
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
        "    io.copy(src, dst)             Copies a file.\n"
        "    io.exists(path)               Returns true if the path exists.\n"
        "    io.listDir([path])            Returns a list of filenames in the directory.\n\n"
        "  Industrial CSV Engine (RFC 4180)\n"
        "  ──────────────────────\n"
        "    io.readCSV(path, [delim], [enc])     Reads a CSV into a list of lists of strings.\n"
        "    io.parseCSVNum(path, [delim], [enc]) Reads a CSV directly into a RealMatrix (fast numeric parsing).\n"
        "    io.writeCSV(path, data, [d], [enc])  Writes a matrix or list of lists to a CSV file.\n"
    );

    mod.register_function_help("io.readFile", "io.readFile(path, [encoding])", "Reads the entire contents of a file into a string.", "txt = io.readFile(\"data.txt\")");
    mod.register_function_help("io.writeFile", "io.writeFile(path, content, [encoding])", "Writes a string to a file, overwriting existing content.", "io.writeFile(\"out.txt\", \"Hello\")");
    mod.register_function_help("io.open", "io.open(path, [mode], [encoding])", "Opens a file and returns a File stream instance.", "f = io.open(\"data.txt\", \"r\", \"ansi\")");
    mod.register_function_help("io.readCSV", "io.readCSV(path, [delim], [encoding])", "Reads a CSV file into a list of lists of strings.", "data = io.readCSV(\"data.csv\", \",\", \"ansi\")");
    mod.register_function_help("io.parseCSVNum", "io.parseCSVNum(path, [delim], [encoding])", "Reads a CSV file directly into a RealMatrix.", "mat = io.parseCSVNum(\"data.csv\")");
    mod.register_function_help("io.writeCSV", "io.writeCSV(path, data, [delim], [encoding])", "Writes a matrix or list to a CSV file.", "io.writeCSV(\"out.csv\", mat)");
    mod.register_function_help("io.stat", "io.stat(path)", "Returns a dictionary with file metadata.", "info = io.stat(\"data.txt\")");
    mod.register_function_help("io.mkdir", "io.mkdir(path, [recursive])", "Creates a directory.", "io.mkdir(\"new_folder\")");
    mod.register_function_help("io.remove", "io.remove(path)", "Deletes a file or empty directory.", "io.remove(\"old.txt\")");
    mod.register_function_help("io.rename", "io.rename(old_path, new_path)", "Renames or moves a file or directory.", "io.rename(\"a.txt\", \"b.txt\")");
    mod.register_function_help("io.copy", "io.copy(src, dst)", "Copies a file to a new destination.", "io.copy(\"a.txt\", \"b.txt\")");
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
