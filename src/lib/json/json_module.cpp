#include "../jc2_extension_cpp.h"
#include <sstream>
#include <cmath>
#include <iomanip>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

struct JsonEngine {
    static std::string encode(const jc2::Value& val, int indent, int level) {
        std::string pad = (indent > 0) ? std::string(level * indent, ' ') : "";
        std::string pad_inner = (indent > 0) ? std::string((level + 1) * indent, ' ') : "";
        std::string nl = (indent > 0) ? "\n" : "";
        std::string sp = (indent > 0) ? " " : "";

        if (val.is_none()) return "null";
        if (val.is_bool()) return val.as_bool() ? "true" : "false";

        if (val.is_double()) {
            double d = val.as_double();
            double rounded = std::round(d);
            if (std::abs(d - rounded) < 1e-5 && std::abs(rounded) < 1e15 && rounded == std::trunc(rounded)) {
                return std::to_string(static_cast<int64_t>(rounded));
            }
            std::ostringstream oss;
            oss << d;
            return oss.str();
        }
        if (val.is_int()) return std::to_string(val.as_int());
        if (val.is_bigint()) return jc2::BigInt(val.get_handle()).to_string();
        if (val.is_fraction()) {
            jc2::Fraction f(val.get_handle());
            double d = f.num().as_double() / f.den().as_double();
            std::ostringstream oss;
            oss << d;
            return oss.str();
        }

        if (val.is_string()) {
            std::string s = val.as_string();
            std::string escaped = "\"";
            for (unsigned char c : s) {
                switch (c) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\b': escaped += "\\b";  break;
                case '\f': escaped += "\\f";  break;
                case '\n': escaped += "\\n";  break;
                case '\r': escaped += "\\r";  break;
                case '\t': escaped += "\\t";  break;
                default:
                    if (c <= 0x1F) {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        escaped += buf;
                    }
                    else {
                        escaped += c;
                    }
                }
            }
            escaped += "\"";
            return escaped;
        }

        if (val.is_list()) {
            jc2::List list(val.get_handle());
            if (list.size() == 0) return "[]";

            std::string r = "[" + nl;
            for (size_t i = 0; i < list.size(); ++i) {
                if (i > 0) r += "," + nl;
                r += pad_inner + encode(list.get(i), indent, level + 1);
            }
            r += nl + pad + "]";
            return r;
        }
        if (val.is_dict()) {
            jc2::Dict dict(val.get_handle());
            if (dict.size() == 0) return "{}";

            jc2::List keys = dict.keys();
            std::string r = "{" + nl;
            bool first = true;
            for (size_t i = 0; i < keys.size(); ++i) {
                if (!first) r += "," + nl;
                jc2::Value k = keys.get(i);
                r += pad_inner + "\"" + k.as_string() + "\":" + sp;
                try {
                    r += encode(dict.get(k), indent, level + 1);
                }
                catch (...) {
                    r += "null";
                }
                first = false;
            }
            r += nl + pad + "}";
            return r;
        }
        if (val.is_real_matrix()) {
            jc2::RealMatrix m(val.get_handle());
            std::string r = "[";
            for (int i = 0; i < m.rows(); ++i) {
                if (i > 0) r += ", ";
                if (m.rows() > 1) r += "[";
                for (int j = 0; j < m.cols(); ++j) {
                    if (j > 0) r += ", ";
                    r += encode(jc2::Value(m.get(i, j)), 0, 0);
                }
                if (m.rows() > 1) r += "]";
            }
            return r + "]";
        }

        return "\"<unserializable_type>\"";
    }
};

struct JsonParser {
    std::string s;
    size_t pos;

    JsonParser(std::string str) : s(std::move(str)), pos(0) {}

    void skipWS() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) pos++;
    }

    std::string parseString() {
        pos++;
        std::string result;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\') {
                pos++;
                if (pos >= s.size()) jc2::throw_error("JSON Parse Error: Unexpected end inside string.");
                char esc = s[pos];
                switch (esc) {
                case '"':  result += '"'; break;
                case '\\': result += '\\'; break;
                case '/':  result += '/'; break;
                case 'b':  result += '\b'; break;
                case 'f':  result += '\f'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case 'u': {
                    std::string hexStr;
                    for (int i = 0; i < 4; ++i) {
                        pos++;
                        if (pos >= s.size()) jc2::throw_error("JSON Parse Error: Unexpected end inside string.");
                        hexStr += s[pos];
                    }
                    try {
                        int cp = std::stoi(hexStr, nullptr, 16);
                        if (cp <= 0x7F) {
                            result += static_cast<char>(cp);
                        } else if (cp <= 0x7FF) {
                            result += static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    } catch (...) {
                        result += "\\u" + hexStr;
                    }
                    break;
                }
                default:
                    result += '\\'; result += esc; break;
                }
            }
            else {
                result += s[pos];
            }
            pos++;
        }
        if (pos >= s.size()) jc2::throw_error("JSON Parse Error: Unterminated string.");
        pos++;
        return result;
    }

    jc2::Value parseNumber() {
        size_t start = pos;
        if (pos < s.size() && s[pos] == '-') pos++;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
        bool isFloat = false;

        if (pos < s.size() && s[pos] == '.') {
            isFloat = true; pos++;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            isFloat = true; pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
        }

        std::string numStr = s.substr(start, pos - start);
        if (numStr == "-" || numStr.empty()) jc2::throw_error("JSON Parse Error: Invalid number structure.");

        if (isFloat) {
            try { return jc2::Value(std::stod(numStr)); }
            catch (...) { jc2::throw_error("JSON Parse Error: Float out of range."); }
        }

        try { return jc2::BigInt(numStr); }
        catch (...) { 
            try { return jc2::Value(std::stod(numStr)); }
            catch (...) { jc2::throw_error("JSON Parse Error: Number out of range."); }
        }
        return jc2::Value();
    }

    jc2::Value parseArray() {
        pos++;
        jc2::List L;
        skipWS();
        if (pos < s.size() && s[pos] == ']') {
            pos++;
            return L;
        }

        while (true) {
            jc2::Value v = parseValue();
            L.push_back(v);
            skipWS();
            if (pos < s.size() && s[pos] == ',') { 
                pos++; 
                skipWS(); 
                if (pos < s.size() && s[pos] == ']') break;
            }
            else break;
        }
        if (pos >= s.size() || s[pos] != ']') jc2::throw_error("JSON Parse Error: Expected ']' array closer.");
        pos++;
        return L;
    }

    jc2::Value parseObject() {
        pos++;
        jc2::Dict D;
        skipWS();
        if (pos < s.size() && s[pos] == '}') { pos++; return D; }

        while (true) {
            skipWS();
            if (pos >= s.size() || s[pos] != '"') jc2::throw_error("JSON Parse Error: Object keys must be strings.");
            std::string key = parseString();
            skipWS();
            if (pos >= s.size() || s[pos] != ':') jc2::throw_error("JSON Parse Error: Expected ':' separator.");
            pos++; skipWS();

            jc2::Value v = parseValue();
            D.set(jc2::Value(key), v);
            skipWS();

            if (pos < s.size() && s[pos] == ',') { 
                pos++; 
                skipWS();
                if (pos < s.size() && s[pos] == '}') break;
            }
            else break;
        }
        if (pos >= s.size() || s[pos] != '}') jc2::throw_error("JSON Parse Error: Expected '}' object closer.");
        pos++;
        return D;
    }

    jc2::Value parseValue() {
        skipWS();
        if (pos >= s.size()) jc2::throw_error("JSON Parse Error: Unexpected end of input.");
        char c = s[pos];
        if (c == '"') return parseString();
        if (c == '[') return parseArray();
        if (c == '{') return parseObject();

        if (s.compare(pos, 4, "true") == 0) { pos += 4; return jc2::Value(true); }
        if (s.compare(pos, 5, "false") == 0) { pos += 5; return jc2::Value(false); }
        if (s.compare(pos, 4, "null") == 0) { pos += 4; return jc2::Value(); }

        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();

        jc2::throw_error(std::string("JSON Parse Error: Unrecognized token at position ") + std::to_string(pos));
        return jc2::Value();
    }
};

JC2_ValueHandle global_encode(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    return jc2::Value(JsonEngine::encode(jc2::Value(argv[0]), 0, 0)).get_handle();
}

JC2_ValueHandle global_pretty(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    int indent = (argc == 2) ? static_cast<int>(std::round(jc2::Value(argv[1]).as_double())) : 4;
    return jc2::Value(JsonEngine::encode(jc2::Value(argv[0]), indent, 0)).get_handle();
}

JC2_ValueHandle global_decode(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Value arg(argv[0]);
    if (!arg.is_string()) jc2::throw_error("Type Error: decode() expects a string.");
    JsonParser parser(arg.as_string());
    return parser.parseValue().get_handle();
}

int jc2_init(jc2::Module& mod) {
    mod.register_function("encode", global_encode, 1, 1, false);
    mod.register_function("stringify", global_encode, 1, 1, false);
    mod.register_function("pretty", global_pretty, 1, 2, false);
    mod.register_function("decode", global_decode, 1, 1, false);
    mod.register_function("parse", global_decode, 1, 1, false);

    mod.register_help("json",
        "═══ JSON Module — Native Module ═══\n\n"
        "  Requires: import json\n\n"
        "  Provides high-performance, native C++ state-machine based JSON serialization\n"
        "  and deserialization. It guarantees memory safety and DOM-level formatting\n"
        "  without external dependencies.\n\n"
        "  Functions & JS Aliases\n"
        "  ──────────────────────\n"
        "    Both traditional and Node.js-style aliases are injected directly into\n"
        "    the global namespace upon import.\n\n"
        "    json.encode(val)  /  json.stringify(val)    Convert JC2 value → JSON string\n"
        "    json.decode(str)  /  json.parse(str)        Parse JSON string → JC2 value\n"
        "    json.pretty(val)                            Pretty-print with 4-space indent\n"
        "    json.pretty(val, indent)                    Custom indent width (e.g., 2)\n\n"
        "  Type Mapping (JC2 → JSON)\n"
        "  ──────────────────────\n"
        "    double / int / fraction      →  number (integers are cleanly truncated)\n"
        "    string                       →  string (Deep RFC standard escaping)\n"
        "    list                         →  array\n"
        "    dict                         →  object\n"
        "    realmatrix (1D / 2D)         →  array / array of arrays\n"
        "    none                         →  null\n"
        "    complex / Special types      →  \"<unserializable_type>\" (Fallback)\n\n"
        "  Type Mapping (JSON → JC2)\n"
        "  ──────────────────────\n"
        "    number (integer)             →  int (avoids precision loss on IDs)\n"
        "    number (float)               →  double\n"
        "    string                       →  string\n"
        "    array                        →  list\n"
        "    object                       →  dict\n"
        "    true / false                 →  true / false\n"
        "    null                         →  none\n\n"
        "  Examples\n"
        "  ──────────────────────\n"
        "    import json\n\n"
        "    // 1. Serialization (Object to String)\n"
        "    d = { name: \"Alice\", active: true, scores: [85, 92] }\n"
        "    s = stringify(d)             \n"
        "    // → '{\"name\": \"Alice\", \"active\": true, \"scores\": [85, 92]}'\n\n"
        "    // 2. Pretty Print (Perfect for writing config files)\n"
        "    print(json.pretty(d, 2))     \n"
        "    // → {\n"
        "    //     \"name\": \"Alice\",\n"
        "    //     \"active\": true,\n"
        "    //     \"scores\": [\n"
        "    //       85,\n"
        "    //       92\n"
        "    //     ]\n"
        "    //   }\n\n"
        "    // 3. Deserialization (String to Object)\n"
        "    raw = r\"({\"host\": \"127.0.0.1\", \"port\": 8080})\"\n"
        "    conf = json.parse(raw)\n"
        "    conf.port                    // → 8080 (Parsed as int)\n"
        "    conf.host                    // → \"127.0.0.1\"\n\n"
        "    // 4. File I/O Full Pipeline\n"
        "    writeFile(\"config.json\", json.pretty(conf))\n"
        "    loaded = json.parse(readFile(\"config.json\"))"
    );

    mod.register_function_help("json.encode", "json.encode(val)", "Serializes a JC2 value into a JSON string.", "json.encode({a: 1})");
    mod.register_function_help("json.decode", "json.decode(str)", "Parses a JSON string into a JC2 value.", "json.decode(\"{\\\"a\\\": 1}\")");
    mod.register_function_help("json.pretty", "json.pretty(val, [indent])", "Serializes a JC2 value into a pretty-printed JSON string.", "json.pretty({a: 1}, 2)");

    return 0;
}

JC2_EXTENSION_INIT
