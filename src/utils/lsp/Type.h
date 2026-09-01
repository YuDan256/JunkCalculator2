#ifndef JC2_LSP_TYPE_H
#define JC2_LSP_TYPE_H

#include <string>
#include <vector>
#include <variant>
#include <algorithm>
#include "../../memory/GcHeap.h"  // BuiltinType

namespace jc {
namespace lsp {

    // 类型元素：内置类型（BuiltinType 枚举）或用户类名
    using TypeElem = std::variant<BuiltinType, std::string>;

    std::string builtinTypeName(BuiltinType bt);

    // 类型对象：types 列表（「或」关系），对齐运行时 ObjTypeDef::types
    struct Type {
        std::vector<TypeElem> types;

        Type() = default;
        explicit Type(BuiltinType bt) { types.push_back(bt); }
        explicit Type(const std::string& className) { types.push_back(className); }
        explicit Type(std::vector<TypeElem> t) : types(std::move(t)) { normalize(); }

        static Type any() { return Type(BuiltinType::ANY); }
        static Type never() { return Type(); }

        bool isAny() const {
            for (const auto& e : types) {
                if (std::holds_alternative<BuiltinType>(e) && std::get<BuiltinType>(e) == BuiltinType::ANY) return true;
            }
            return false;
        }
        bool isNever() const { return types.empty(); }

        // 排序去重（对应 ObjTypeDef::normalize）
        void normalize() {
            std::sort(types.begin(), types.end());
            types.erase(std::unique(types.begin(), types.end()), types.end());
        }

        std::string toString() const {
            if (types.empty()) return "never";
            std::string res;
            for (size_t i = 0; i < types.size(); ++i) {
                if (i > 0) res += " | ";
                if (std::holds_alternative<BuiltinType>(types[i])) {
                    res += builtinTypeName(std::get<BuiltinType>(types[i]));
                } else {
                    res += std::get<std::string>(types[i]);
                }
            }
            return res;
        }

        // 联合 = 并集（对应 operator|）
        static Type unify(const Type& a, const Type& b) {
            if (a.isAny() || b.isAny()) return any();
            std::vector<TypeElem> merged = a.types;
            merged.insert(merged.end(), b.types.begin(), b.types.end());
            return Type(std::move(merged));
        }

        // 交集（对应 operator&）
        static Type intersect(const Type& a, const Type& b) {
            std::vector<TypeElem> out;
            for (const auto& t : a.types) {
                if (std::find(b.types.begin(), b.types.end(), t) != b.types.end()) {
                    out.push_back(t);
                }
            }
            return Type(std::move(out));
        }

        // 兼容：val ⊆ expected（val 的每个元素都 ∈ expected，对应 opIsSubset / checkValueType）
        static bool compatible(const Type& val, const Type& expected) {
            if (expected.isAny()) return true;
            for (const auto& v : val.types) {
                bool covered = false;
                for (const auto& e : expected.types) {
                    if (v == e) { covered = true; break; }
                    // 子类兼容（B extends A → B 的实例兼容 A）后续按继承链展开，第一版按同名匹配
                }
                if (!covered) return false;
            }
            return true;
        }
    };

    // BuiltinType 名字（对应 ObjTypeDef::computeName 的 switch，含 COMPLEX 修复）
    inline std::string builtinTypeName(BuiltinType bt) {
        switch (bt) {
        case BuiltinType::ANY: return "any";
        case BuiltinType::INT: return "int";
        case BuiltinType::FLOAT: return "double";
        case BuiltinType::STRING: return "string";
        case BuiltinType::BOOL: return "bool";
        case BuiltinType::NONE_TYPE: return "none_type";
        case BuiltinType::LIST: return "list";
        case BuiltinType::DICT: return "dict";
        case BuiltinType::SET: return "set";
        case BuiltinType::FRACTION: return "fraction";
        case BuiltinType::COMPLEX: return "complex";
        case BuiltinType::SYMBOLIC: return "symbolic";
        case BuiltinType::REALMAT: return "realmatrix";
        case BuiltinType::COMPLEXMAT: return "complexmatrix";
        case BuiltinType::SYMMAT: return "symmatrix";
        case BuiltinType::FUNC: return "function";
        case BuiltinType::CLASS: return "class_type";
        case BuiltinType::INSTANCE: return "instance";
        case BuiltinType::NAMESPACE: return "namespace_type";
        case BuiltinType::CUSTOM_CLASS: return "custom_class";
        case BuiltinType::TYPE_DEF: return "type";
        case BuiltinType::SLICE: return "slice";
        default: return "unknown";
        }
    }

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_TYPE_H
