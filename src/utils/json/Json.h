#ifndef JC2_JSON_H
#define JC2_JSON_H

#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

namespace jc {

    enum class JsonType { Null, Bool, Number, String, Array, Object };

    class Json {
    public:
        JsonType type = JsonType::Null;
        bool boolVal = false;
        double numVal = 0.0;
        std::string strVal;
        std::vector<Json> arrVal;
        std::unordered_map<std::string, Json> objVal;

        Json() = default;
        Json(std::nullptr_t) : type(JsonType::Null) {}
        Json(bool b) : type(JsonType::Bool), boolVal(b) {}
        Json(double n) : type(JsonType::Number), numVal(n) {}
        Json(int n) : type(JsonType::Number), numVal(static_cast<double>(n)) {}
        Json(const std::string& s) : type(JsonType::String), strVal(s) {}
        Json(const char* s) : type(JsonType::String), strVal(s) {}
        Json(const std::vector<Json>& a) : type(JsonType::Array), arrVal(a) {}
        Json(const std::unordered_map<std::string, Json>& o) : type(JsonType::Object), objVal(o) {}

        // 核心解析与序列化接口
        static Json parse(const std::string& text);
        std::string serialize() const;

        // 类型检查
        bool isNull() const { return type == JsonType::Null; }
        bool isBool() const { return type == JsonType::Bool; }
        bool isNumber() const { return type == JsonType::Number; }
        bool isString() const { return type == JsonType::String; }
        bool isArray() const { return type == JsonType::Array; }
        bool isObject() const { return type == JsonType::Object; }

        // 对象访问辅助
        bool has(const std::string& key) const;
        const Json& operator[](const std::string& key) const;
        Json& operator[](const std::string& key);
    };

} // namespace jc

#endif // JC2_JSON_H
