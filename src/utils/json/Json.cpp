#include "Json.h"
#include <cctype>
#include <sstream>
#include <iomanip>

namespace jc {

    // ========================================================================
    // JSON 解析器实现 (递归下降)
    // ========================================================================
    class JsonParser {
        const std::string& str;
        size_t pos = 0;

        void skipWhitespace() {
            while (pos < str.length() && std::isspace(static_cast<unsigned char>(str[pos]))) {
                pos++;
            }
        }

        char peek() const { return pos < str.length() ? str[pos] : '\0'; }
        char advance() { return pos < str.length() ? str[pos++] : '\0'; }
        bool match(char expected) {
            if (peek() == expected) {
                pos++;
                return true;
            }
            return false;
        }

        Json parseString() {
            advance(); // consume '"'
            std::string res;
            while (pos < str.length() && peek() != '"') {
                char c = advance();
                if (c == '\\' && pos < str.length()) {
                    char esc = advance();
                    switch (esc) {
                        case '"': res += '"'; break;
                        case '\\': res += '\\'; break;
                        case '/': res += '/'; break;
                        case 'b': res += '\b'; break;
                        case 'f': res += '\f'; break;
                        case 'n': res += '\n'; break;
                        case 'r': res += '\r'; break;
                        case 't': res += '\t'; break;
                        case 'u': {
                            if (pos + 4 <= str.length()) {
                                // 简化的 \uXXXX 处理
                                std::string hex = str.substr(pos, 4);
                                pos += 4;
                                res += "\\u" + hex; 
                            }
                            break;
                        }
                        default: res += esc; break;
                    }
                } else {
                    res += c;
                }
            }
            if (peek() == '"') advance();
            return Json(res);
        }

        Json parseNumber() {
            size_t start = pos;
            if (peek() == '-') advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
            if (peek() == '.') {
                advance();
                while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                advance();
                if (peek() == '+' || peek() == '-') advance();
                while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
            }
            std::string numStr = str.substr(start, pos - start);
            try {
                return Json(std::stod(numStr));
            } catch (...) {
                return Json(0.0);
            }
        }

        Json parseArray() {
            advance(); // consume '['
            std::vector<Json> arr;
            skipWhitespace();
            if (match(']')) return Json(arr);
            while (true) {
                arr.push_back(parseValue());
                skipWhitespace();
                if (match(']')) break;
                if (!match(',')) throw std::runtime_error("JSON Parse Error: Expected ',' or ']' in array");
            }
            return Json(arr);
        }

        Json parseObject() {
            advance(); // consume '{'
            std::unordered_map<std::string, Json> obj;
            skipWhitespace();
            if (match('}')) return Json(obj);
            while (true) {
                skipWhitespace();
                if (peek() != '"') throw std::runtime_error("JSON Parse Error: Expected string key in object");
                std::string key = parseString().strVal;
                skipWhitespace();
                if (!match(':')) throw std::runtime_error("JSON Parse Error: Expected ':' after key");
                obj[key] = parseValue();
                skipWhitespace();
                if (match('}')) break;
                if (!match(',')) throw std::runtime_error("JSON Parse Error: Expected ',' or '}' in object");
            }
            return Json(obj);
        }

    public:
        JsonParser(const std::string& s) : str(s) {}

        Json parseValue() {
            skipWhitespace();
            char c = peek();
            if (c == '"') return parseString();
            if (c == '{') return parseObject();
            if (c == '[') return parseArray();
            if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();
            if (str.compare(pos, 4, "true") == 0) { pos += 4; return Json(true); }
            if (str.compare(pos, 5, "false") == 0) { pos += 5; return Json(false); }
            if (str.compare(pos, 4, "null") == 0) { pos += 4; return Json(nullptr); }
            throw std::runtime_error("JSON Parse Error: Unexpected character");
        }
    };

    Json Json::parse(const std::string& text) {
        JsonParser parser(text);
        return parser.parseValue();
    }

    // ========================================================================
    // JSON 序列化器实现
    // ========================================================================
    static std::string escapeJsonString(const std::string& s) {
        std::ostringstream oss;
        oss << "\"";
        for (char c : s) {
            switch (c) {
                case '"': oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b"; break;
                case '\f': oss << "\\f"; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) <= 0x1F) {
                        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    } else {
                        oss << c;
                    }
            }
        }
        oss << "\"";
        return oss.str();
    }

    std::string Json::serialize() const {
        switch (type) {
            case JsonType::Null: return "null";
            case JsonType::Bool: return boolVal ? "true" : "false";
            case JsonType::Number: {
                std::ostringstream oss;
                oss << numVal;
                return oss.str();
            }
            case JsonType::String: return escapeJsonString(strVal);
            case JsonType::Array: {
                std::string res = "[";
                for (size_t i = 0; i < arrVal.size(); ++i) {
                    res += arrVal[i].serialize();
                    if (i < arrVal.size() - 1) res += ",";
                }
                res += "]";
                return res;
            }
            case JsonType::Object: {
                std::string res = "{";
                bool first = true;
                for (const auto& pair : objVal) {
                    if (!first) res += ",";
                    res += escapeJsonString(pair.first) + ":" + pair.second.serialize();
                    first = false;
                }
                res += "}";
                return res;
            }
        }
        return "null";
    }

    bool Json::has(const std::string& key) const {
        return type == JsonType::Object && objVal.find(key) != objVal.end();
    }

    const Json& Json::operator[](const std::string& key) const {
        static const Json nullJson;
        if (type == JsonType::Object) {
            auto it = objVal.find(key);
            if (it != objVal.end()) return it->second;
        }
        return nullJson;
    }

    Json& Json::operator[](const std::string& key) {
        if (type != JsonType::Object) {
            type = JsonType::Object;
            objVal.clear();
        }
        return objVal[key];
    }

} // namespace jc
