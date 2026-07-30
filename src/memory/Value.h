// Value.h
#ifndef JC2_VALUE_H
#define JC2_VALUE_H

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4702) 
#endif

#include <any>
#include <variant>
#include <string>
#include <stdexcept>
#include <iostream>
#include <cmath>
#include <vector>
#include <memory>
#include <sstream>
#include <iomanip>
#include <numeric> 
#include <map>
#include <unordered_map>
#include <functional>
#include "../math/Complex.h"
#include "../math/Matrix.h"
#include "../math/BigInt.h"
#include "../frontend/Expr.h"
#include "../math/Fraction.h"
#include "../math/Base.h"
#include "../math/Tolerance.h"
#include "../lib/image/Image.h"
#include "../lib/prob/Probability.h"
#include "GcHeap.h"
#include "../cas/Symbolic.h" 
#include "SipHash.h"

namespace jc {
    class Value;
    std::string setValueKey(const Value& v);

    struct ValueHasher {
        size_t operator()(const Value& v) const;
    };
    struct ValueEqual {
        bool operator()(const Value& lhs, const Value& rhs) const;
    };

    struct RecursionGuard {
        std::vector<const void*>& vis;
        bool isCycle;
        RecursionGuard(std::vector<const void*>& v, const void* p) : vis(v) {
            isCycle = (std::find(vis.begin(), vis.end(), p) != vis.end());
            if (!isCycle) vis.push_back(p);
        }
        ~RecursionGuard() {
            if (!isCycle) vis.pop_back(); // 退出时自动抹除足迹
        }
    };

    // =======================================================
    // NaN-Boxing 核心宏定义
    // =======================================================
    #define QNAN     ((uint64_t)0x7ffc000000000000)
    #define SIGN_BIT ((uint64_t)0x8000000000000000)
    #define TAG_NONE 1
    #define TAG_FALSE 2
    #define TAG_TRUE 3
    #define TAG_UNINIT 4
    #define INT32_MASK (QNAN | 0x0000000100000000ULL)

    // =======================================================
    // Obj 派生类前向声明
    // =======================================================
    struct ObjString;
    struct ObjBigInt;
    struct ObjFraction;
    struct ObjComplex;
    struct ObjBaseNum;
    struct ObjRealMatrix;
    struct ObjComplexMatrix;
    struct ObjStringMatrix;
    struct ObjList;
    struct ObjDict;
    struct ObjSet;
    struct ObjClosure;
    struct ObjClass;
    struct ObjInstance;
    struct ObjSuper;
    struct ObjSym;
    struct ObjNamespace;
    struct ObjUpVal;
    struct NamespaceField;
    struct ObjTypeDef;

    inline std::unordered_map<std::string, ObjString*> g_internedStrings;
    ObjString* internString(const std::string& str);

    // =======================================================
    // Obj 派生类定义
    // =======================================================
    struct ObjString : public Obj {
        std::string str;
        size_t hash;
        size_t charLength;
        bool isAscii;
        ObjString(std::string s) : str(std::move(s)) { 
            hash = sipHash24String(str);
            type = ObjType::STRING; 
            charLength = 0;
            isAscii = true;
            for (size_t i = 0; i < str.length(); ++i) {
                unsigned char c = static_cast<unsigned char>(str[i]);
                if (c >= 0x80) isAscii = false;
                if ((c & 0xC0) != 0x80) charLength++;
            }
        }
        ~ObjString() override {
            g_internedStrings.erase(str);
        }
    };
    struct ObjBigInt : public Obj {
        BigInt num;
        mutable bool has_cached_hash = false;
        mutable size_t cached_hash = 0;
        ObjBigInt(BigInt n) : num(std::move(n)) { type = ObjType::BIGINT; }
    };
    struct ObjFraction : public Obj {
        Fraction frac;
        mutable bool has_cached_hash = false;
        mutable size_t cached_hash = 0;
        ObjFraction(Fraction f) : frac(std::move(f)) { type = ObjType::FRACTION; }
    };
    struct ObjComplex : public Obj {
        Complex comp;
        ObjComplex(Complex c) : comp(std::move(c)) { type = ObjType::COMPLEX; }
    };
    struct ObjBaseNum : public Obj {
        BaseNum base;
        ObjBaseNum(BaseNum b) : base(std::move(b)) { type = ObjType::BASENUM; }
    };
    struct ObjRealMatrix : public Obj {
        RealMatrix mat;
        ObjRealMatrix(RealMatrix m) : mat(std::move(m)) { type = ObjType::REAL_MATRIX; }
    };
    struct ObjComplexMatrix : public Obj {
        ComplexMatrix mat;
        ObjComplexMatrix(ComplexMatrix m) : mat(std::move(m)) { type = ObjType::COMPLEX_MATRIX; }
    };
    struct ObjStringMatrix : public Obj {
        StringMatrix mat;
        ObjStringMatrix(StringMatrix m) : mat(std::move(m)) { type = ObjType::STRING_MATRIX; }
    };

    template<typename> struct always_false : std::false_type {};

    std::pair<bool, Value> invokeDunder(ObjInstance* inst, const char* methodName, const std::vector<Value>& args = {});

    class Value {
    public:
        uint64_t as_bits;

        inline double asDoubleRaw() const {
            double d;
            std::memcpy(&d, &as_bits, sizeof(double));
            return d;
        }
        inline static Value fromDouble(double d) {
            Value v;
            if (std::isnan(d)) {
                // ★ 归一化真正的 NaN (Quiet NaN)，严格保留其作为浮点数 NaN 的语义，绝不与 none 混淆
                v.as_bits = 0x7FF8000000000000ULL;
            } else {
                std::memcpy(&v.as_bits, &d, sizeof(double));
            }
            return v;
        }
        inline static Value fromInt32(int32_t v) {
            Value val;
            val.as_bits = INT32_MASK | static_cast<uint32_t>(v);
            return val;
        }
        inline static Value fromObj(Obj* obj) {
            Value v;
            v.as_bits = SIGN_BIT | QNAN | reinterpret_cast<uint64_t>(obj);
            if (obj) obj->refCount++;
            return v;
        }

        bool isDouble() const { return (as_bits & QNAN) != QNAN; }
        bool isInt32() const { return (as_bits & 0xFFFFFFFF00000000ULL) == INT32_MASK; }
        bool isNumber() const { return isDouble() || isInt32() || isBool(); }
        bool isObj() const { return (as_bits & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT); }
        bool isNone() const { return as_bits == (QNAN | TAG_NONE); }
        bool isUninit() const { return as_bits == (QNAN | TAG_UNINIT); }
        bool isBool() const { return as_bits == (QNAN | TAG_FALSE) || as_bits == (QNAN | TAG_TRUE); }
        bool asBool() const { return as_bits == (QNAN | TAG_TRUE); }
        
        int32_t asInt32() const { return static_cast<int32_t>(as_bits & 0xFFFFFFFFULL); }
        double asNumber() const { 
            if (isInt32()) return static_cast<double>(asInt32());
            if (isBool()) return asBool() ? 1.0 : 0.0;
            return asDoubleRaw(); 
        }

        Obj* asObj() const { return reinterpret_cast<Obj*>(as_bits & ~(SIGN_BIT | QNAN)); }
        bool isObjType(ObjType type) const { return isObj() && asObj()->type == type; }

    private:
        static std::pair<bool, BigInt> getExactIntRoot(const BigInt& base, int64_t n) {
            if (n <= 0) return { false, BigInt(0) };
            if (n == 1) return { true, base };
            bool isNeg = base.isNegative();
            if (isNeg && n % 2 == 0) return { false, BigInt(0) };
            BigInt absBase = base.abs();
            if (absBase.isZero()) return { true, BigInt(0) };
            if (absBase == BigInt(1)) return { true, isNeg ? (BigInt(0) - BigInt(1)) : BigInt(1) };
            // 用位数估算上界，极大缩小搜索空间
            std::string s = absBase.toString();
            size_t rootLen = s.length() / static_cast<size_t>(n) + 1;
            std::string upperStr = "1";
            upperStr.append(rootLen, '0');
            BigInt lower(1), upper(upperStr);
            while (!(upper < lower)) {
                BigInt mid = (lower + upper) / BigInt(2);
                BigInt midPow = mid.pow(BigInt(n));
                if (midPow == absBase) {
                    return { true, isNeg ? (BigInt(0) - mid) : mid };
                }
                else if (midPow < absBase) {
                    lower = mid + BigInt(1);
                }
                else {
                    upper = mid - BigInt(1);
                }
            }
            return { false, BigInt(0) };
        }
        // ========================================================
        // 有理数精确有理数次幂
        // (num/den) ^ (p/q) → 分子分母分别开 q 次根，再求 p 次幂
        // 返回 {是否成功, Value 结果}
        // ========================================================
        static std::pair<bool, Value> tryExactRationalPow(
            const BigInt& num, const BigInt& den, int64_t p, int64_t q)
        {
            if (q <= 0) return { false, Value() };
            auto [numOk, numRoot] = getExactIntRoot(num, q);
            if (!numOk) return { false, Value() };
            auto [denOk, denRoot] = getExactIntRoot(den, q);
            if (!denOk) return { false, Value() };
            if (p >= 0) {
                Fraction result = Fraction(numRoot, denRoot).pow(p);
                return { true, Value::fromFraction(result) };
            }
            else {
                Fraction result = Fraction(denRoot, numRoot).pow(-p);
                return { true, Value::fromFraction(result) };
            }
        }
    public:
        Value() : as_bits(QNAN | TAG_NONE) {}
        Value(double val) : as_bits(QNAN | TAG_NONE) { *this = fromDouble(val); }
        Value(int val) : as_bits(INT32_MASK | static_cast<uint32_t>(val)) {}
        Value(bool val) : as_bits(val ? (QNAN | TAG_TRUE) : (QNAN | TAG_FALSE)) {}
        Value(Obj* obj) : as_bits(SIGN_BIT | QNAN | reinterpret_cast<uint64_t>(obj)) { 
            if (obj) obj->refCount++; 
        }

        Value(const Value& other) : as_bits(other.as_bits) {
            if (isObj()) asObj()->refCount++;
        }

        Value(Value&& other) noexcept : as_bits(other.as_bits) {
            other.as_bits = QNAN | TAG_NONE;
        }

        Value& operator=(const Value& other) {
            if (this == &other) return *this;
            if (other.isObj()) other.asObj()->refCount++;
            if (isObj()) {
                Obj* obj = asObj();
                if (--obj->refCount == 0) GcHeap::get().freeObj(obj);
            }
            as_bits = other.as_bits;
            return *this;
        }

        Value& operator=(Value&& other) noexcept {
            if (this == &other) return *this;
            if (isObj()) {
                Obj* obj = asObj();
                if (--obj->refCount == 0) GcHeap::get().freeObj(obj);
            }
            as_bits = other.as_bits;
            other.as_bits = QNAN | TAG_NONE;
            return *this;
        }

        ~Value() {
            if (isObj()) {
                Obj* obj = asObj();
                if (--obj->refCount == 0) GcHeap::get().freeObj(obj);
            }
        }
        
        Value(std::string val) : as_bits(QNAN | TAG_NONE) { *this = fromObj(internString(val)); }
        Value(const char* val) : as_bits(QNAN | TAG_NONE) { *this = fromObj(internString(std::string(val))); }
        Value(Complex val) : as_bits(QNAN | TAG_NONE) { *this = fromObj(GcHeap::get().allocate<ObjComplex>(std::move(val))); }
        Value(RealMatrix val) : as_bits(QNAN | TAG_NONE) {
            if (val.getRows() * val.getCols() == 0) val = RealMatrix(0, 0);
            *this = fromObj(GcHeap::get().allocate<ObjRealMatrix>(std::move(val)));
        }
        Value(ComplexMatrix val) : as_bits(QNAN | TAG_NONE) {
            if (val.getRows() * val.getCols() == 0) val = ComplexMatrix(0, 0);
            *this = fromObj(GcHeap::get().allocate<ObjComplexMatrix>(std::move(val)));
        }
        Value(BigInt val) : as_bits(QNAN | TAG_NONE) {
            try {
                int64_t v = val.toInt64();
                if (v >= -2147483648LL && v <= 2147483647LL) {
                    *this = fromInt32(static_cast<int32_t>(v));
                    return;
                }
            } catch (...) {}
            *this = fromObj(GcHeap::get().allocate<ObjBigInt>(std::move(val)));
        }
        Value(Fraction val) : as_bits(QNAN | TAG_NONE) { *this = fromObj(GcHeap::get().allocate<ObjFraction>(std::move(val))); }
        Value(BaseNum val) : as_bits(QNAN | TAG_NONE) { *this = fromObj(GcHeap::get().allocate<ObjBaseNum>(std::move(val))); }
        Value(StringMatrix val) : as_bits(QNAN | TAG_NONE) {
            if (val.getRows() * val.getCols() == 0) val = StringMatrix(0, 0);
            *this = fromObj(GcHeap::get().allocate<ObjStringMatrix>(std::move(val)));
        }
        
        Value(SymExpr val);

        bool isInstance() const { return isObjType(ObjType::INSTANCE); }
        bool isClass() const { return isObjType(ObjType::CLASS); }
        bool isComplex() const { return isObjType(ObjType::COMPLEX); }
        bool isSymbolic() const { return isObjType(ObjType::SYMBOLIC); }
        bool isType() const { return isObjType(ObjType::TYPE_DEF); }

        SymExpr asSymbolic() const;

        ObjInstance* asInstance() const;

        bool isSuperProxy() const { return isObjType(ObjType::SUPER_PROXY); }
        ObjSuper* asSuperProxy() const;
        static Value makeSuperProxy(ObjInstance* inst, ObjClass* parent);

        static Value fromFraction(const Fraction& f) {
            if (f.getDen() == BigInt(1)) return Value(f.getNum());
            return Value(f);
        }

        bool isBaseNum() const { return isObjType(ObjType::BASENUM); }
        ObjClosure* asFunction() const;

        bool isFunctionClosure() const { return isObjType(ObjType::CLOSURE); }
        bool isString() const { return isObjType(ObjType::STRING); }
        ObjString* asObjString() const {
            if (isString()) return static_cast<ObjString*>(asObj());
            throw std::runtime_error("Type Error: Expected a string.");
        }

        const std::string& asString() const {
            return asObjString()->str;
        }

        static Value none() { return Value(); }
        static Value uninit() { Value v; v.as_bits = QNAN | TAG_UNINIT; return v; }

        double asDouble() const {
            if (isNumber()) return asNumber();
            if (isObjType(ObjType::BIGINT)) return static_cast<ObjBigInt*>(asObj())->num.toDouble();
            if (isObjType(ObjType::FRACTION)) return static_cast<ObjFraction*>(asObj())->frac.toDouble();
            if (isObjType(ObjType::BASENUM)) return static_cast<ObjBaseNum*>(asObj())->base.getValue().toDouble();
            throw std::runtime_error("Type Error: Expected a real number.");
        }

        Complex asComplex() const {
            if (isObjType(ObjType::COMPLEX)) return static_cast<ObjComplex*>(asObj())->comp;
            if (isNumber()) return Complex(asNumber());
            if (isObjType(ObjType::BIGINT)) return Complex(static_cast<ObjBigInt*>(asObj())->num.toDouble());
            if (isObjType(ObjType::FRACTION)) return Complex(static_cast<ObjFraction*>(asObj())->frac.toDouble());
            if (isObjType(ObjType::BASENUM)) return Complex(static_cast<ObjBaseNum*>(asObj())->base.getValue().toDouble());
            throw std::runtime_error("Type Error: Expected a number or complex.");
        }

        BigInt asBigInt() const {
            if (isInt32()) return BigInt(asInt32());
            if (isBool()) return BigInt(asBool() ? 1 : 0);
            if (isObjType(ObjType::BIGINT)) return static_cast<ObjBigInt*>(asObj())->num;
            if (isDouble()) {
                double val = asDoubleRaw();
                if (std::abs(val) > 9.22337e18) {
                    throw std::runtime_error("Math Error: Value too massively large to be safely converted to an exact layout integer.");
                }
                return BigInt(static_cast<int64_t>(std::round(val)));
            }
            if (isObjType(ObjType::BASENUM)) return static_cast<ObjBaseNum*>(asObj())->base.getValue();
            if (isObjType(ObjType::FRACTION)) {
                const auto& f = static_cast<ObjFraction*>(asObj())->frac;
                if (f.getDen() == BigInt(1)) return f.getNum();
                throw std::runtime_error("Type Error: Fraction is not an integer (" + f.toString() + ").");
            }
            throw std::runtime_error("Type Error: Expected an integer.");
        }

        bool isBigInt() const { return isObjType(ObjType::BIGINT); }

        RealMatrix asRealMatrix() const {
            if (isObjType(ObjType::REAL_MATRIX)) return static_cast<ObjRealMatrix*>(asObj())->mat;
            throw std::runtime_error("Type Error: Expected a real matrix.");
        }

        ComplexMatrix asComplexMatrix() const {
            if (isObjType(ObjType::COMPLEX_MATRIX)) return static_cast<ObjComplexMatrix*>(asObj())->mat;
            if (isObjType(ObjType::REAL_MATRIX)) {
                const RealMatrix& m = static_cast<ObjRealMatrix*>(asObj())->mat;
                std::vector<Complex> flat(m.getRows() * m.getCols());
                for (int r = 0; r < m.getRows(); ++r) {
                    for (int c = 0; c < m.getCols(); ++c) {
                        flat[r * m.getCols() + c] = Complex(m(r, c));
                    }
                }
                return ComplexMatrix(m.getRows(), m.getCols(), flat);
            }
            throw std::runtime_error("Type Error: Expected a matrix.");
        }

        // ==========================================
        // 统一哈希判定 (The One and Only Hashability)
        // ==========================================
        bool isHashable() const;

        // ==========================================
        // 统一真值判定 (The One and Only Truthiness)
        // ==========================================
        bool truthy() const;

        // ==========================================
        // 统一相等判定 (The One and Only Equality)
        // ==========================================
        static bool equals(const Value& lhs, const Value& rhs);

        std::string typeName() const;
        Value operator~() const;
        Value operator-() const;

        friend Value operator+(const Value& lhs, const Value& rhs);

        friend Value operator-(const Value& lhs, const Value& rhs);

        friend Value operator*(const Value& lhs, const Value& rhs);

        friend Value operator/(const Value& lhs, const Value& rhs);

        friend Value operator^(const Value& lhs, const Value& rhs);

        friend Value operator%(const Value& lhs, const Value& rhs);

        friend bool operator<(const Value& lhs, const Value& rhs);
        friend bool operator<=(const Value& lhs, const Value& rhs);
        friend bool operator>(const Value& lhs, const Value& rhs);
        friend bool operator>=(const Value& lhs, const Value& rhs);

        friend std::ostream& operator<<(std::ostream& os, const Value& val);

        friend Value operator&(const Value& lhs, const Value& rhs);
        friend Value operator|(const Value& lhs, const Value& rhs);
        friend Value bitXor(const Value& lhs, const Value& rhs);
        friend Value operator<<(const Value& lhs, const Value& rhs);
        friend Value operator>>(const Value& lhs, const Value& rhs);

        std::string toJC2Expression() const;

        std::string toString() const;

        std::string toRepr() const;
    }; // class Value

    struct PropertyDescriptor {
        Value val;
        bool is_const = false;
        bool is_local = false;
    };

    struct ObjClass : public Obj {
        uint64_t classId;
        std::string name;
        ObjClass* parent = nullptr;
        std::unordered_map<std::string, PropertyDescriptor> properties;
        bool is_native = false;
        std::function<Value(const std::vector<Value>&)> native_allocator;
        ObjClass() { 
            static uint64_t nextId = 1;
            classId = nextId++;
            type = ObjType::CLASS; 
        }
    };

    struct ObjTypeDef : public Obj {
        std::vector<std::variant<BuiltinType, ObjClass*>> types;
        ObjTypeDef() { type = ObjType::TYPE_DEF; }
        
        void normalize() {
            std::sort(types.begin(), types.end(), [](const auto& a, const auto& b) {
                if (a.index() != b.index()) return a.index() < b.index();
                if (std::holds_alternative<BuiltinType>(a)) return std::get<BuiltinType>(a) < std::get<BuiltinType>(b);
                return std::get<ObjClass*>(a) < std::get<ObjClass*>(b);
            });
            types.erase(std::unique(types.begin(), types.end()), types.end());
        }

        std::string name() const {
            if (types.empty()) return "Never";
            std::string res = "";
            for (size_t i = 0; i < types.size(); ++i) {
                if (i > 0) res += " | ";
                if (std::holds_alternative<BuiltinType>(types[i])) {
                    switch (std::get<BuiltinType>(types[i])) {
                        case BuiltinType::ANY: res += "any_type"; break;
                        case BuiltinType::INT: res += "int"; break;
                        case BuiltinType::FLOAT: res += "double"; break;
                        case BuiltinType::REAL: res += "real"; break;
                        case BuiltinType::NUMBER: res += "number"; break;
                        case BuiltinType::WHOLE: res += "whole"; break;
                        case BuiltinType::EXACT: res += "exact"; break;
                        case BuiltinType::STRING: res += "string"; break;
                        case BuiltinType::BOOL: res += "bool"; break;
                        case BuiltinType::BINARY: res += "binary"; break;
                        case BuiltinType::NONE_TYPE: res += "none_type"; break;
                        case BuiltinType::LIST: res += "list"; break;
                        case BuiltinType::DICT: res += "dict"; break;
                        case BuiltinType::SET: res += "set"; break;
                        case BuiltinType::FRACTION: res += "fraction"; break;
                        case BuiltinType::COMPLEX: res += "complex"; break;
                        case BuiltinType::BASENUM: res += "basenum"; break;
                        case BuiltinType::SYMBOLIC: res += "symbolic"; break;
                        case BuiltinType::REALMAT: res += "realmatrix"; break;
                        case BuiltinType::COMPLEXMAT: res += "complexmatrix"; break;
                        case BuiltinType::STRINGMAT: res += "stringmatrix"; break;
                        case BuiltinType::MATRIX: res += "matrix"; break;
                        case BuiltinType::FUNC: res += "function"; break;
                        case BuiltinType::CLASS: res += "class_type"; break;
                        case BuiltinType::INSTANCE: res += "instance"; break;
                        case BuiltinType::NAMESPACE: res += "namespace_type"; break;
                        case BuiltinType::ITERABLE: res += "iterable"; break;
                        case BuiltinType::CALLABLE: res += "callable"; break;
                        case BuiltinType::INDEXABLE: res += "indexable"; break;
                        case BuiltinType::HASHABLE: res += "hashable"; break;
                        case BuiltinType::NUMERIC: res += "numeric"; break;
                        case BuiltinType::CUSTOM_CLASS: res += "custom_class"; break;
                        case BuiltinType::TYPE_DEF: res += "type"; break;
                        default: res += "unknown"; break;
                    }
                } else {
                    ObjClass* cls = std::get<ObjClass*>(types[i]);
                    res += cls->name.empty() ? "<anonymous class>" : cls->name;
                }
            }
            return res;
        }
    };

    struct ObjInstance : public Obj {
        ObjClass* classDef = nullptr;
        std::unordered_map<std::string, PropertyDescriptor> properties;
        
        std::any nativeData;
        void* c_nativeData = nullptr;
        void (*c_nativeDtor)(void*) = nullptr;
        Value (*c_nativeNext)(ObjInstance*) = nullptr;
        bool is_frozen = false;
        bool is_finalized = false;
        mutable bool is_hashable_cached = false;
        mutable bool has_cached_hash = false;
        mutable size_t cached_hash = 0;
        ObjInstance() { type = ObjType::INSTANCE; }
        void checkModify() const { if (is_frozen) throw std::runtime_error("Runtime Error: Cannot modify frozen Instance."); }
        void clear() override { clearProperties(); }
        void clearTotal() override;

        void removeProperty(const std::string& key) {
            checkModify();
            auto it = properties.find(key);
            if (it == properties.end() || it->second.is_local) throw std::runtime_error("Runtime Error: Key not found.");
            if (it->second.is_const) throw std::runtime_error("Runtime Error: Cannot delete const property '" + key + "'.");
            properties.erase(it);
        }

        void discardProperty(const std::string& key) {
            checkModify();
            auto it = properties.find(key);
            if (it != properties.end() && !it->second.is_local) {
                if (it->second.is_const) throw std::runtime_error("Runtime Error: Cannot delete const property '" + key + "'.");
                properties.erase(it);
            }
        }

        void clearProperties() {
            checkModify();
            for (const auto& [k, prop] : properties) {
                if (!prop.is_local && prop.is_const) throw std::runtime_error("Runtime Error: Cannot delete const property '" + k + "'.");
            }
            for (auto it = properties.begin(); it != properties.end(); ) {
                if (!it->second.is_local) {
                    it = properties.erase(it);
                } else {
                    ++it;
                }
            }
        }

        void setProperty(const std::string& key, const Value& val) {
            checkModify();
            auto it = properties.find(key);
            if (it != properties.end()) {
                if (it->second.is_local) throw std::runtime_error("Runtime Error: Cannot modify private property '" + key + "'.");
                if (it->second.is_const) throw std::runtime_error("Runtime Error: Cannot modify const property '" + key + "'.");
                it->second.val = val;
            } else {
                properties[key] = {val, false, false};
            }
        }
    };
    struct ObjSuper : public Obj {
        ObjInstance* instance = nullptr;
        ObjClass* parentClass = nullptr;
        ObjSuper() { type = ObjType::SUPER_PROXY; }
    };
    struct ObjSym : public Obj {
        SymExpr sym;
        ObjSym(SymExpr s) : sym(std::move(s)) { type = ObjType::SYMBOLIC; }
    };

    inline Value::Value(SymExpr val) : as_bits(QNAN | TAG_NONE) {
        if (val.ptr && val.ptr->getType() == SymType::NUM) {
            auto numNode = std::static_pointer_cast<SymNum>(val.ptr);
            std::visit([this](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, int32_t>) {
                    *this = Value(arg);
                } else if constexpr (std::is_same_v<T, Fraction>) {
                    if (arg.getDen() == BigInt(1)) {
                        *this = Value(arg.getNum());
                    } else {
                        *this = Value(arg);
                    }
                } else if constexpr (std::is_same_v<T, double>) {
                    *this = Value(arg);
                } else if constexpr (std::is_same_v<T, BigInt>) {
                    *this = Value(arg);
                }
            }, numNode->value);
        } else {
            *this = fromObj(GcHeap::get().allocate<ObjSym>(std::move(val)));
        }
    }

    inline SymExpr Value::asSymbolic() const {
        if (isSymbolic()) return static_cast<ObjSym*>(asObj())->sym;
        if (isInt32()) return SymExpr(BigInt(asInt32()));
        if (isDouble()) return SymExpr(asDoubleRaw());
        if (isObjType(ObjType::BIGINT)) return SymExpr(static_cast<ObjBigInt*>(asObj())->num);
        if (isObjType(ObjType::FRACTION)) return SymExpr(static_cast<ObjFraction*>(asObj())->frac);
        if (isObjType(ObjType::COMPLEX)) return SymExpr(static_cast<ObjComplex*>(asObj())->comp);
        throw std::runtime_error("TypeError: Expected a symbolic expression or exact number.");
    }

    inline ObjInstance* Value::asInstance() const {
        if (!isInstance()) throw std::runtime_error("Type Error: Expected an instance.");
        return static_cast<ObjInstance*>(asObj());
    }

    inline ObjSuper* Value::asSuperProxy() const {
        if (!isSuperProxy()) throw std::runtime_error("Type Error: Expected super proxy.");
        return static_cast<ObjSuper*>(asObj());
    }

    inline Value Value::makeSuperProxy(ObjInstance* inst, ObjClass* parent) {
        ObjSuper* sp = GcHeap::get().allocate<ObjSuper>();
        sp->instance = inst;
        sp->parentClass = parent;
        return Value(sp);
    }

    inline std::string Value::toString() const {
        if (isString()) return static_cast<ObjString*>(asObj())->str;
        if (isType()) return static_cast<ObjTypeDef*>(asObj())->name();
        if (isInstance()) {
            try {
                auto [foundStr, strRes] = invokeDunder(asInstance(), "__str__");
                if (foundStr) {
                    if (strRes.isString()) return strRes.asString();
                    std::ostringstream oss;
                    oss << strRes;
                    return oss.str();
                }
            } catch (...) {}
        }
        std::ostringstream oss;
        oss << *this;
        return oss.str();
    }

    inline std::string Value::toRepr() const {
        if (isString()) return "\"" + static_cast<ObjString*>(asObj())->str + "\"";
        if (isType()) return "<type '" + static_cast<ObjTypeDef*>(asObj())->name() + "'>";
        if (isInstance()) {
            try {
                auto [foundRepl, replRes] = invokeDunder(asInstance(), "__repr__");
                if (foundRepl) {
                    if (replRes.isString()) return replRes.asString();
                    std::ostringstream oss;
                    oss << replRes;
                    return oss.str();
                }
                auto [foundStr, strRes] = invokeDunder(asInstance(), "__str__");
                if (foundStr) {
                    if (strRes.isString()) return strRes.asString();
                    std::ostringstream oss;
                    oss << strRes;
                    return oss.str();
                }
            } catch (...) {}
        }
        std::ostringstream oss;
        oss << *this;
        return oss.str();
    }

    struct ClassProperty {
        Value val;
        bool is_const = false;
        bool is_local = false;
    };

    struct PrivateProperty {
        Value val;
        bool is_const = false;
    };

    inline void ObjInstance::clearTotal() { 
        nativeData.reset(); 
        if (c_nativeDtor && c_nativeData) {
            c_nativeDtor(c_nativeData);
        }
        c_nativeData = nullptr;
        c_nativeDtor = nullptr;
        properties.clear();
    }

    struct RuntimeError : public std::exception {
        std::string type;
        Value message;
        mutable std::string whatBuffer;

        RuntimeError(std::string t, Value msg) 
            : type(std::move(t)), message(std::move(msg)) {}

        const char* what() const noexcept override {
            if (whatBuffer.empty()) {
                std::string msgStr = message.isString() ? message.asString() : message.toString();
                if (type.empty()) {
                    whatBuffer = msgStr;
                } else {
                    whatBuffer = type + ": " + msgStr;
                }
            }
            return whatBuffer.c_str();
        }
    };

    struct ObjUpVal : public Obj {
        Value* location = nullptr;
        Value closed;
        int stackIndex = -1;
        ObjUpVal* nextOpen = nullptr;
        ObjUpVal() { type = ObjType::UPVALUE; }
        void clearTotal() override { closed = Value::none(); location = nullptr; }
    };

    struct NamespaceField {
        ObjUpVal* upval = nullptr;
        bool isConst = false;
    };

    struct ObjNamespace : public Obj {
        std::string name;
        std::unordered_map<std::string, NamespaceField> fields;
        bool is_frozen = false;
        ObjNamespace() { type = ObjType::NAMESPACE; }
        void checkModify() const { if (is_frozen) throw std::runtime_error("Runtime Error: Cannot modify frozen Namespace."); }
        void clear() override { clearFields(); }
        void clearTotal() override { fields.clear(); }

        void removeField(const std::string& key) {
            checkModify();
            auto it = fields.find(key);
            if (it == fields.end()) throw std::runtime_error("Runtime Error: Key not found.");
            if (it->second.isConst) throw std::runtime_error("Runtime Error: Cannot delete const property '" + key + "'.");
            fields.erase(it);
        }

        void discardField(const std::string& key) {
            checkModify();
            auto it = fields.find(key);
            if (it != fields.end()) {
                if (it->second.isConst) throw std::runtime_error("Runtime Error: Cannot delete const property '" + key + "'.");
                fields.erase(it);
            }
        }

        void clearFields() {
            checkModify();
            for (const auto& [k, field] : fields) {
                if (field.isConst) throw std::runtime_error("Runtime Error: Cannot delete const property '" + k + "'.");
            }
            fields.clear();
        }

        void setField(const std::string& key, const Value& val) {
            checkModify();
            auto it = fields.find(key);
            if (it != fields.end()) {
                if (it->second.isConst) throw std::runtime_error("Runtime Error: Cannot modify const property '" + key + "'.");
                *(it->second.upval->location) = val;
            } else {
                auto uv = GcHeap::get().allocate<ObjUpVal>();
                uv->closed = val;
                uv->location = &uv->closed;
                fields[key] = { uv, false };
            }
        }
    };

    struct ObjList : public Obj {
        std::vector<Value> vec;
        bool is_frozen = false;
        mutable bool is_hashable_cached = false;
        mutable bool has_cached_hash = false;
        mutable size_t cached_hash = 0;
        ObjList() { type = ObjType::LIST; }
        void checkModify() const { if (is_frozen) throw std::runtime_error("Runtime Error: Cannot modify frozen List."); }
        std::vector<Value>& mut() { checkModify(); return vec; }
        void clear() override { checkModify(); clearTotal(); }
        void clearTotal() override { vec.clear(); }
    };
    struct ObjDict : public Obj {
        std::vector<std::pair<Value, Value>> elements;
        std::unordered_map<Value, size_t, ValueHasher, ValueEqual> keyMap;
        bool is_frozen = false;
        mutable bool is_hashable_cached = false;
        mutable bool has_cached_hash = false;
        mutable size_t cached_hash = 0;
        ObjDict() { type = ObjType::DICT; }
        void checkModify() const { if (is_frozen) throw std::runtime_error("Runtime Error: Cannot modify frozen Dict."); }
        void clear() override { checkModify(); clearTotal(); }
        void clearTotal() override { elements.clear(); keyMap.clear(); }
        void set(const Value& key, const Value& val) {
            checkModify();
            auto it = keyMap.find(key);
            if (it != keyMap.end()) {
                elements[it->second].second = val;
            } else {
                keyMap[key] = elements.size();
                elements.push_back({key, val});
            }
        }
        void remove(const Value& key) {
            checkModify();
            auto it = keyMap.find(key);
            if (it == keyMap.end()) throw std::runtime_error("Runtime Error: Key not found.");
            size_t idx = it->second;
            keyMap.erase(it);
            elements.erase(elements.begin() + idx);
            for (size_t i = idx; i < elements.size(); ++i) {
                keyMap[elements[i].first] = i;
            }
        }
        void discard(const Value& key) {
            checkModify();
            auto it = keyMap.find(key);
            if (it != keyMap.end()) {
                size_t idx = it->second;
                keyMap.erase(it);
                elements.erase(elements.begin() + idx);
                for (size_t i = idx; i < elements.size(); ++i) {
                    keyMap[elements[i].first] = i;
                }
            }
        }
    };
    struct ObjSet : public Obj {
        std::vector<Value> elements;
        std::unordered_set<Value, ValueHasher, ValueEqual> keys;
        bool is_frozen = false;
        mutable bool is_hashable_cached = false;
        mutable bool has_cached_hash = false;
        mutable size_t cached_hash = 0;
        ObjSet() { type = ObjType::SET; }
        void checkModify() const { if (is_frozen) throw std::runtime_error("Runtime Error: Cannot modify frozen Set."); }
        void clear() override { checkModify(); clearTotal(); }
        void clearTotal() override { elements.clear(); keys.clear(); }
        void add(const Value& val) {
            checkModify();
            if (keys.find(val) == keys.end()) {
                keys.insert(val);
                elements.push_back(val);
            }
        }
        void remove(const Value& val) {
            checkModify();
            auto it = keys.find(val);
            if (it == keys.end()) throw std::runtime_error("Runtime Error: Element not found in Set.");
            keys.erase(it);
            auto eIt = std::find_if(elements.begin(), elements.end(), [&val](const Value& v) { return Value::equals(v, val); });
            if (eIt != elements.end()) elements.erase(eIt);
        }
        void discard(const Value& val) {
            checkModify();
            auto it = keys.find(val);
            if (it != keys.end()) {
                keys.erase(it);
                auto eIt = std::find_if(elements.begin(), elements.end(), [&val](const Value& v) { return Value::equals(v, val); });
                if (eIt != elements.end()) elements.erase(eIt);
            }
        }
        Value pop() {
            checkModify();
            if (elements.empty()) throw std::runtime_error("Runtime Error: setPop() on empty Set.");
            Value result = elements.back();
            keys.erase(result);
            elements.pop_back();
            return result;
        }
    };



    inline Value operator*(const Value& lhs, const Value& rhs) {
        bool lIsInt = lhs.isInt32() || lhs.isBool();
        bool rIsInt = rhs.isInt32() || rhs.isBool();
        if (lIsInt && rIsInt) {
            int32_t a = lhs.isInt32() ? lhs.asInt32() : (lhs.asBool() ? 1 : 0);
            int32_t b = rhs.isInt32() ? rhs.asInt32() : (rhs.asBool() ? 1 : 0);
            int64_t prod = static_cast<int64_t>(a) * b;
            if (prod >= -2147483648LL && prod <= 2147483647LL) return Value::fromInt32(static_cast<int32_t>(prod));
            return Value(BigInt(prod));
        }
        if (lhs.isNumber() && rhs.isNumber()) return Value(lhs.asNumber() * rhs.asNumber());
        
        if (lhs.isObjType(ObjType::REAL_MATRIX) && rhs.isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(lhs.asObj())->mat * static_cast<ObjRealMatrix*>(rhs.asObj())->mat);
            if (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhs.isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(lhs.asObj())->mat * static_cast<ObjComplexMatrix*>(rhs.asObj())->mat);
            if (lhs.isObjType(ObjType::REAL_MATRIX) && rhs.isObjType(ObjType::COMPLEX_MATRIX)) return Value(lhs.asComplexMatrix() * rhs.asComplexMatrix());
            if (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhs.isObjType(ObjType::REAL_MATRIX)) return Value(lhs.asComplexMatrix() * rhs.asComplexMatrix());
            
            bool lhsIsMat = lhs.isObjType(ObjType::REAL_MATRIX) || lhs.isObjType(ObjType::COMPLEX_MATRIX);
            bool rhsIsMat = rhs.isObjType(ObjType::REAL_MATRIX) || rhs.isObjType(ObjType::COMPLEX_MATRIX);
            bool lhsIsScalar = lhs.isNumber() || lhs.isBigInt() || lhs.isObjType(ObjType::FRACTION) || lhs.isComplex();
            bool rhsIsScalar = rhs.isNumber() || rhs.isBigInt() || rhs.isObjType(ObjType::FRACTION) || rhs.isComplex();

            if (lhsIsMat && rhsIsScalar) {
                if (lhs.isObjType(ObjType::REAL_MATRIX) && !rhs.isComplex()) return Value(static_cast<ObjRealMatrix*>(lhs.asObj())->mat * rhs.asDouble());
                return Value(lhs.asComplexMatrix() * rhs.asComplex());
            }
            if (lhsIsScalar && rhsIsMat) {
                if (rhs.isObjType(ObjType::REAL_MATRIX) && !rhs.isComplex()) return Value(static_cast<ObjRealMatrix*>(rhs.asObj())->mat * lhs.asDouble());
                return Value(rhs.asComplexMatrix() * rhs.asComplex());
            }
            
            if (lhs.isObjType(ObjType::STRING) && (rhs.isNumber() || rhs.isObjType(ObjType::BIGINT))) {
                int n = static_cast<int>(rhs.asDouble());
                if (n < 0) throw std::runtime_error("Type Error: String repeat count must be non-negative.");
                std::string result;
                const std::string& s = static_cast<ObjString*>(lhs.asObj())->str;
                result.reserve(s.size() * n);
                for (int i = 0; i < n; ++i) result += s;
                return Value(result);
            }
            if ((lhs.isNumber() || lhs.isObjType(ObjType::BIGINT)) && rhs.isObjType(ObjType::STRING)) {
                int n = static_cast<int>(lhs.asDouble());
                if (n < 0) throw std::runtime_error("Type Error: String repeat count must be non-negative.");
                std::string result;
                const std::string& s = static_cast<ObjString*>(rhs.asObj())->str;
                result.reserve(s.size() * n);
                for (int i = 0; i < n; ++i) result += s;
                return Value(result);
            }

            if (lhs.isObjType(ObjType::SET) && rhs.isObjType(ObjType::SET)) {
                ObjSet* s1 = static_cast<ObjSet*>(lhs.asObj());
                ObjSet* s2 = static_cast<ObjSet*>(rhs.asObj());
                ObjSet* res = GcHeap::get().allocate<ObjSet>();
                GcObjGuard guard(res);
                for (const auto& v1 : s1->elements) {
                    for (const auto& v2 : s2->elements) {
                        ObjList* pair = GcHeap::get().allocate<ObjList>();
                        pair->vec.push_back(v1);
                        pair->vec.push_back(v2);
                        pair->is_frozen = true;
                        pair->is_hashable_cached = true;
                        Value pairVal(pair);
                        res->add(pairVal);
                    }
                }
                return Value(res);
            }

            if (lhs.isSymbolic() || rhs.isSymbolic()) return Value(lhs.asSymbolic() * rhs.asSymbolic());

            if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base * static_cast<ObjBaseNum*>(rhs.asObj())->base);
            if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base * BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix()));
            if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()) * static_cast<ObjBaseNum*>(rhs.asObj())->base);

            if (lhs.isObjType(ObjType::COMPLEX) || rhs.isObjType(ObjType::COMPLEX)) return Value(lhs.asComplex() * rhs.asComplex());
            
            bool lhsIsExactInt = lhs.isBigInt() || lhs.isInt32();
            bool rhsIsExactInt = rhs.isBigInt() || rhs.isInt32();
            if (lhsIsExactInt && rhsIsExactInt) return Value(lhs.asBigInt() * rhs.asBigInt());
            if (lhs.isObjType(ObjType::FRACTION) && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac * static_cast<ObjFraction*>(rhs.asObj())->frac);
            if (lhs.isObjType(ObjType::FRACTION) && rhsIsExactInt) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac * Fraction(rhs.asBigInt()));
            if (lhsIsExactInt && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(Fraction(lhs.asBigInt()) * static_cast<ObjFraction*>(rhs.asObj())->frac);
            
            if (lhs.isDouble() || rhs.isDouble()) return Value(lhs.asDouble() * rhs.asDouble());
        
        throw std::runtime_error("Type Error: Cannot multiply '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline Value operator/(const Value& lhs, const Value& rhs) {
        bool lIsInt = lhs.isInt32() || lhs.isBool();
        bool rIsInt = rhs.isInt32() || rhs.isBool();
        if (lIsInt && rIsInt) {
            int32_t a = lhs.isInt32() ? lhs.asInt32() : (lhs.asBool() ? 1 : 0);
            int32_t b = rhs.isInt32() ? rhs.asInt32() : (rhs.asBool() ? 1 : 0);
            if (b == 0) throw std::runtime_error("Math Error: Division by zero.");
            if (a % b == 0) {
                if (a == -2147483648 && b == -1) return Value(BigInt(2147483648LL));
                return Value::fromInt32(a / b);
            }
            return Value(Fraction(BigInt(a), BigInt(b)));
        }
        if (lhs.isNumber() && rhs.isNumber()) {
            double b = rhs.asNumber();
            if (b == 0.0) throw std::runtime_error("Math Error: Division by zero.");
            return Value(lhs.asNumber() / b);
        }
        
        if (rhs.isObjType(ObjType::REAL_MATRIX)) {
                return lhs * Value(rhs.asRealMatrix().inverse());
            }
            if (rhs.isObjType(ObjType::COMPLEX_MATRIX)) {
                return lhs * Value(rhs.asComplexMatrix().inverse());
            }

            bool lhsIsMat = lhs.isObjType(ObjType::REAL_MATRIX) || lhs.isObjType(ObjType::COMPLEX_MATRIX);
            bool rhsIsScalar = rhs.isNumber() || rhs.isBigInt() || rhs.isObjType(ObjType::FRACTION) || rhs.isComplex();

            if (lhsIsMat && rhsIsScalar) {
                if (rhs.isComplex()) {
                    Complex c = rhs.asComplex();
                    if (c == 0.0) throw std::runtime_error("Math Error: Division by zero complex number.");
                    return Value(lhs.asComplexMatrix() / c);
                } else {
                    double d = rhs.asDouble();
                    if (d == 0.0) throw std::runtime_error("Math Error: Division by zero.");
                    if (lhs.isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(lhs.asObj())->mat / d);
                    return Value(lhs.asComplexMatrix() / Complex(d, 0.0));
                }
            }

            if (lhs.isSymbolic() || rhs.isSymbolic()) return Value(lhs.asSymbolic() / rhs.asSymbolic());

            if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base / static_cast<ObjBaseNum*>(rhs.asObj())->base);
            if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base / BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix()));
            if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()) / static_cast<ObjBaseNum*>(rhs.asObj())->base);

            bool lhsIsExactInt = lhs.isBigInt() || lhs.isInt32();
            bool rhsIsExactInt = rhs.isBigInt() || rhs.isInt32();
            if (lhsIsExactInt && rhsIsExactInt) {
                BigInt a = lhs.asBigInt();
                BigInt b = rhs.asBigInt();
                if (b.isZero()) throw std::runtime_error("Math Error: Division by zero.");
                if ((a % b).isZero()) return Value(a / b);
                return Value(Fraction(a, b));
            }
            if (lhs.isObjType(ObjType::FRACTION) && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac / static_cast<ObjFraction*>(rhs.asObj())->frac);
            if (lhs.isObjType(ObjType::FRACTION) && rhsIsExactInt) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac / Fraction(rhs.asBigInt()));
            if (lhsIsExactInt && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(Fraction(lhs.asBigInt()) / static_cast<ObjFraction*>(rhs.asObj())->frac);

            if (lhs.isObjType(ObjType::COMPLEX) || rhs.isObjType(ObjType::COMPLEX)) {
                Complex b = rhs.asComplex();
                if (b == 0.0) throw std::runtime_error("Math Error: Division by zero.");
                return Value(lhs.asComplex() / b);
            }
            if (lhs.isDouble() || rhs.isDouble()) {
                double b = rhs.asDouble();
                if (b == 0.0) throw std::runtime_error("Math Error: Division by zero.");
                return Value(lhs.asDouble() / b);
            }
        
        throw std::runtime_error("Type Error: Cannot divide '" + lhs.typeName() + "' by '" + rhs.typeName() + "'.");
    }

    inline Value operator^(const Value& lhs, const Value& rhs) {
        if (lhs.isSymbolic() || rhs.isSymbolic()) return Value(lhs.asSymbolic() ^ rhs.asSymbolic());
        
        if (lhs.isObjType(ObjType::COMPLEX) || rhs.isObjType(ObjType::COMPLEX)) return Value(lhs.asComplex() ^ rhs.asComplex());

            if (lhs.isObjType(ObjType::REAL_MATRIX) || lhs.isObjType(ObjType::COMPLEX_MATRIX)) {
                bool rhsIsScalar = rhs.isNumber() || rhs.isBigInt() || rhs.isObjType(ObjType::FRACTION) || rhs.isComplex();
                if (rhsIsScalar) {
                    if (rhs.isComplex()) {
                        return Value((matLog(lhs.asComplexMatrix()) * rhs.asComplex()).matExp());
                    } else {
                        double b = rhs.asDouble();
                        if (Tol::isEq(b, std::round(b), 1e5)) return Value(lhs.asComplexMatrix().power(static_cast<int>(std::round(b))));
                        return Value((matLog(lhs.asComplexMatrix()) * Complex(b)).matExp());
                    }
                }
                if (rhs.isObjType(ObjType::REAL_MATRIX) || rhs.isObjType(ObjType::COMPLEX_MATRIX)) return Value(matPow(lhs.asComplexMatrix(), rhs.asComplexMatrix()));
            }
            bool lhsIsScalar = lhs.isNumber() || lhs.isBigInt() || lhs.isObjType(ObjType::FRACTION) || lhs.isComplex();
            if (lhsIsScalar && (rhs.isObjType(ObjType::REAL_MATRIX) || rhs.isObjType(ObjType::COMPLEX_MATRIX))) {
                return Value((rhs.asComplexMatrix() * log(lhs.asComplex())).matExp());
            }

            if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base ^ static_cast<ObjBaseNum*>(rhs.asObj())->base);
            if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base ^ BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix()));
            if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()) ^ static_cast<ObjBaseNum*>(rhs.asObj())->base);

            bool rhsIsExactInt = false;
            BigInt rhsInt(0);
            if (rhs.isObjType(ObjType::BIGINT)) {
                rhsIsExactInt = true;
                rhsInt = static_cast<ObjBigInt*>(rhs.asObj())->num;
            } else if (rhs.isInt32()) {
                rhsIsExactInt = true;
                rhsInt = BigInt(rhs.asInt32());
            } else if (rhs.isBool()) {
                rhsIsExactInt = true;
                rhsInt = BigInt(rhs.asBool() ? 1 : 0);
            } else if (rhs.isDouble()) {
                double d = rhs.asDoubleRaw();
                if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9e18) {
                    rhsIsExactInt = true;
                    rhsInt = BigInt(static_cast<int64_t>(d));
                }
            } else if (rhs.isObjType(ObjType::FRACTION)) {
                const auto& f = static_cast<ObjFraction*>(rhs.asObj())->frac;
                if (f.getDen() == BigInt(1)) {
                    rhsIsExactInt = true;
                    rhsInt = f.getNum();
                }
            }

            if (rhsIsExactInt) {
                if (lhs.isObjType(ObjType::BIGINT)) {
                    const BigInt& a = static_cast<ObjBigInt*>(lhs.asObj())->num;
                    if (rhsInt.isNegative()) return Value::fromFraction(Fraction(BigInt(1), a).pow(rhsInt.abs().toInt64()));
                    return Value(a.pow(rhsInt));
                }
                if (lhs.isObjType(ObjType::FRACTION)) {
                    const Fraction& a = static_cast<ObjFraction*>(lhs.asObj())->frac;
                    if (rhsInt.isNegative()) return Value::fromFraction(Fraction(a.getDen(), a.getNum()).pow(rhsInt.abs().toInt64()));
                    return Value::fromFraction(a.pow(rhsInt.toInt64()));
                }
                if (lhs.isInt32()) {
                    BigInt a(lhs.asInt32());
                    if (rhsInt.isNegative()) return Value::fromFraction(Fraction(BigInt(1), a).pow(rhsInt.abs().toInt64()));
                    return Value(a.pow(rhsInt));
                }
            }

            if (lhs.isObjType(ObjType::BIGINT) || lhs.isObjType(ObjType::FRACTION) || lhs.isInt32() || lhs.isBool()) {
                if (rhs.isObjType(ObjType::FRACTION)) {
                    Fraction b = static_cast<ObjFraction*>(rhs.asObj())->frac;
                    BigInt aNum = lhs.isObjType(ObjType::FRACTION) ? static_cast<ObjFraction*>(lhs.asObj())->frac.getNum() : lhs.asBigInt();
                    BigInt aDen = lhs.isObjType(ObjType::FRACTION) ? static_cast<ObjFraction*>(lhs.asObj())->frac.getDen() : BigInt(1);
                    
                    int64_t p = b.getNum().toInt64(), q = b.getDen().toInt64();
                    if (q < 0) { p = -p; q = -q; }
                    
                    auto [ok, val] = Value::tryExactRationalPow(aNum, aDen, p, q);
                    if (ok) return val;
                    
                    if (q > 0) {
                        int64_t k = p / q, r = p % q;
                        if (r < 0) { r += q; k -= 1; }
                        if (k != 0 && r != 0) {
                            Value exactPart = lhs ^ Value(BigInt(k));
                            GcValueGuard guard(exactPart);
                            Value symPart = Value(SymExpr(Fraction(aNum, aDen)) ^ SymExpr(Fraction(BigInt(r), BigInt(q))));
                            return exactPart * symPart;
                        }
                    }
                    return Value(SymExpr(Fraction(aNum, aDen)) ^ SymExpr(b));
                }
            }

            double a = lhs.asDouble(), b = rhs.asDouble();
            if (a < 0 && std::floor(b) != b) {
                return Value(Complex(a, 0.0) ^ Complex(b, 0.0));
            }
            double res = std::pow(a, b);
            return Value(res);

        throw std::runtime_error("Type Error: Power operation not supported for '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline bool operator<(const Value& a, const Value& b) {
        if ((a.isBigInt() || a.isInt32() || a.isBool()) && (b.isBigInt() || b.isInt32() || b.isBool())) return a.asBigInt() < b.asBigInt();
        if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) return static_cast<ObjFraction*>(a.asObj())->frac < static_cast<ObjFraction*>(b.asObj())->frac;
        if ((a.isObjType(ObjType::FRACTION) && (b.isNumber() || b.isBigInt())) || (b.isObjType(ObjType::FRACTION) && (a.isNumber() || a.isBigInt()))) {
            try {
                auto getF = [](const Value& v) { return v.isObjType(ObjType::FRACTION) ? static_cast<ObjFraction*>(v.asObj())->frac : (v.isDouble() ? Fraction::fromDouble(v.asDoubleRaw()) : Fraction(v.asBigInt())); };
                return getF(a) < getF(b);
            } catch (...) { return a.asDouble() < b.asDouble(); }
        }
        if (a.isString() && b.isString()) return a.asString() < b.asString();
        return a.asDouble() < b.asDouble();
    }

    inline bool operator<=(const Value& a, const Value& b) {
        if ((a.isBigInt() || a.isInt32() || a.isBool()) && (b.isBigInt() || b.isInt32() || b.isBool())) return a.asBigInt() <= b.asBigInt();
        if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) return static_cast<ObjFraction*>(a.asObj())->frac <= static_cast<ObjFraction*>(b.asObj())->frac;
        if ((a.isObjType(ObjType::FRACTION) && (b.isNumber() || b.isBigInt())) || (b.isObjType(ObjType::FRACTION) && (a.isNumber() || a.isBigInt()))) {
            try {
                auto getF = [](const Value& v) { return v.isObjType(ObjType::FRACTION) ? static_cast<ObjFraction*>(v.asObj())->frac : (v.isDouble() ? Fraction::fromDouble(v.asDoubleRaw()) : Fraction(v.asBigInt())); };
                return getF(a) <= getF(b);
            } catch (...) { return a.asDouble() <= b.asDouble(); }
        }
        if (a.isString() && b.isString()) return a.asString() <= b.asString();
        return a.asDouble() <= b.asDouble();
    }

    inline bool operator>(const Value& a, const Value& b) {
        if ((a.isBigInt() || a.isInt32() || a.isBool()) && (b.isBigInt() || b.isInt32() || b.isBool())) return a.asBigInt() > b.asBigInt();
        if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) return static_cast<ObjFraction*>(a.asObj())->frac > static_cast<ObjFraction*>(b.asObj())->frac;
        if ((a.isObjType(ObjType::FRACTION) && (b.isNumber() || b.isBigInt())) || (b.isObjType(ObjType::FRACTION) && (a.isNumber() || a.isBigInt()))) {
            try {
                auto getF = [](const Value& v) { return v.isObjType(ObjType::FRACTION) ? static_cast<ObjFraction*>(v.asObj())->frac : (v.isDouble() ? Fraction::fromDouble(v.asDoubleRaw()) : Fraction(v.asBigInt())); };
                return getF(a) > getF(b);
            } catch (...) { return a.asDouble() > b.asDouble(); }
        }
        if (a.isString() && b.isString()) return a.asString() > b.asString();
        return a.asDouble() > b.asDouble();
    }

    inline bool operator>=(const Value& a, const Value& b) {
        if ((a.isBigInt() || a.isInt32() || a.isBool()) && (b.isBigInt() || b.isInt32() || b.isBool())) return a.asBigInt() >= b.asBigInt();
        if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) return static_cast<ObjFraction*>(a.asObj())->frac >= static_cast<ObjFraction*>(b.asObj())->frac;
        if ((a.isObjType(ObjType::FRACTION) && (b.isNumber() || b.isBigInt())) || (b.isObjType(ObjType::FRACTION) && (a.isNumber() || a.isBigInt()))) {
            try {
                auto getF = [](const Value& v) { return v.isObjType(ObjType::FRACTION) ? static_cast<ObjFraction*>(v.asObj())->frac : (v.isDouble() ? Fraction::fromDouble(v.asDoubleRaw()) : Fraction(v.asBigInt())); };
                return getF(a) >= getF(b);
            } catch (...) { return a.asDouble() >= b.asDouble(); }
        }
        if (a.isString() && b.isString()) return a.asString() >= b.asString();
        return a.asDouble() >= b.asDouble();
    }

    inline Value operator%(const Value& lhs, const Value& rhs) {
        bool lIsInt = lhs.isInt32() || lhs.isBool();
        bool rIsInt = rhs.isInt32() || rhs.isBool();
        if (lIsInt && rIsInt) {
            int32_t a = lhs.isInt32() ? lhs.asInt32() : (lhs.asBool() ? 1 : 0);
            int32_t b = rhs.isInt32() ? rhs.asInt32() : (rhs.asBool() ? 1 : 0);
            if (b == 0) throw std::runtime_error("Math Error: Modulo by zero.");
            if (a == -2147483648 && b == -1) return Value::fromInt32(0);
            return Value::fromInt32(a % b);
        }
        if (lhs.isNumber() && rhs.isNumber()) {
            double b = rhs.asNumber();
            if (b == 0.0) throw std::runtime_error("Math Error: Modulo by zero.");
            return Value(std::fmod(lhs.asNumber(), b));
        }
        
        bool rhsIsRealScalar = rhs.isNumber() || rhs.isBigInt() || rhs.isObjType(ObjType::FRACTION);
            if (lhs.isObjType(ObjType::REAL_MATRIX) && rhsIsRealScalar) {
                double b = rhs.asDouble();
                if (b == 0.0) throw std::runtime_error("Math Error: Modulo by zero.");
                const auto& a = static_cast<ObjRealMatrix*>(lhs.asObj())->mat;
                RealMatrix res(a.getRows(), a.getCols());
                for (int i = 0; i < a.getRows(); ++i)
                    for (int j = 0; j < a.getCols(); ++j)
                        res(i, j) = std::fmod(a(i, j), b);
                return Value(res);
            }
            if (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhsIsRealScalar) {
                double b = rhs.asDouble();
                if (b == 0.0) throw std::runtime_error("Math Error: Modulo by zero.");
                const auto& a = static_cast<ObjComplexMatrix*>(lhs.asObj())->mat;
                ComplexMatrix res(a.getRows(), a.getCols());
                for (int i = 0; i < a.getRows(); ++i)
                    for (int j = 0; j < a.getCols(); ++j)
                        res(i, j) = Complex(std::fmod(a(i, j).real, b), std::fmod(a(i, j).imag, b));
                return Value(res);
            }

            if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base % static_cast<ObjBaseNum*>(rhs.asObj())->base);
            if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base % BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix()));
            if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()) % static_cast<ObjBaseNum*>(rhs.asObj())->base);

            bool lhsIsExactInt = lhs.isBigInt() || lhs.isInt32();
            bool rhsIsExactInt = rhs.isBigInt() || rhs.isInt32();
            if (lhsIsExactInt && rhsIsExactInt) return Value(lhs.asBigInt() % rhs.asBigInt());
            if (lhs.isObjType(ObjType::FRACTION) && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac % static_cast<ObjFraction*>(rhs.asObj())->frac);
            if (lhs.isObjType(ObjType::FRACTION) && rhsIsExactInt) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac % Fraction(rhs.asBigInt()));
            if (lhsIsExactInt && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(Fraction(lhs.asBigInt()) % static_cast<ObjFraction*>(rhs.asObj())->frac);
        
        throw std::runtime_error("Type Error: Modulo not supported for '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline Value operator&(const Value& lhs, const Value& rhs) {
        auto promoteToType = [](const Value& v) -> ObjTypeDef* {
            if (v.isType()) return static_cast<ObjTypeDef*>(v.asObj());
            if (v.isClass()) {
                ObjTypeDef* td = GcHeap::get().allocate<ObjTypeDef>();
                td->types.push_back(static_cast<ObjClass*>(v.asObj()));
                return td;
            }
            return nullptr;
        };
        ObjTypeDef* lType = promoteToType(lhs);
        ObjTypeDef* rType = promoteToType(rhs);
        if (lType && rType) {
            ObjTypeDef* res = GcHeap::get().allocate<ObjTypeDef>();
            for (const auto& t : lType->types) {
                if (std::find(rType->types.begin(), rType->types.end(), t) != rType->types.end()) {
                    res->types.push_back(t);
                }
            }
            res->normalize();
            return Value(res);
        }

        if (lhs.isObjType(ObjType::SET) && rhs.isObjType(ObjType::SET)) {
            ObjSet* s1 = static_cast<ObjSet*>(lhs.asObj());
            ObjSet* s2 = static_cast<ObjSet*>(rhs.asObj());
            ObjSet* res = GcHeap::get().allocate<ObjSet>();
            for (const auto& val : s1->elements) {
                if (s2->keys.find(val) != s2->keys.end()) {
                    res->add(val);
                }
            }
            return Value(res);
        }
        if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.bitAnd(static_cast<ObjBaseNum*>(rhs.asObj())->base));
        if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.bitAnd(BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix())));
        if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()).bitAnd(static_cast<ObjBaseNum*>(rhs.asObj())->base));
        
        if (lhs.isInt32() && rhs.isInt32()) return Value::fromInt32(lhs.asInt32() & rhs.asInt32());
        
        bool lhsIsInt = lhs.isInt32() || lhs.isBigInt() || lhs.isBool();
        bool rhsIsInt = rhs.isInt32() || rhs.isBigInt() || rhs.isBool();
        if (lhsIsInt && rhsIsInt) {
            BigInt lVal = lhs.isBool() ? BigInt(lhs.asBool() ? 1 : 0) : lhs.asBigInt();
            BigInt rVal = rhs.isBool() ? BigInt(rhs.asBool() ? 1 : 0) : rhs.asBigInt();
            BigInt res = BaseNum(lVal, 2).bitAnd(BaseNum(rVal, 2)).getValue();
            return Value(res);
        }
        throw std::runtime_error("Type Error: Bitwise/Set AND '&' not supported for '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline Value operator<<(const Value& lhs, const Value& rhs) {
        int shift = static_cast<int>(std::round(rhs.asDouble()));
        if (shift < 0) throw std::runtime_error("Math Error: Negative shift count.");
        if (lhs.isObjType(ObjType::BASENUM)) {
            return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.shiftLeft(shift));
        }
        if (lhs.isInt32()) {
            int32_t v = lhs.asInt32();
            if (v == 0) return Value::fromInt32(0);
            if (shift < 31) {
                int64_t res = static_cast<int64_t>(v) << shift;
                if (res >= -2147483648LL && res <= 2147483647LL) {
                    return Value::fromInt32(static_cast<int32_t>(res));
                }
            }
        }
        bool lhsIsInt = lhs.isInt32() || lhs.isBigInt() || lhs.isBool();
        if (lhsIsInt) {
            BigInt lVal = lhs.isBool() ? BigInt(lhs.asBool() ? 1 : 0) : lhs.asBigInt();
            return Value(lVal * BigInt(2).pow(shift));
        }
        throw std::runtime_error("Type Error: Bitwise SHIFT LEFT '<<' not supported for '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline Value operator>>(const Value& lhs, const Value& rhs) {
        int shift = static_cast<int>(std::round(rhs.asDouble()));
        if (shift < 0) throw std::runtime_error("Math Error: Negative shift count.");
        if (lhs.isObjType(ObjType::BASENUM)) {
            return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.shiftRight(shift));
        }
        if (lhs.isInt32()) {
            int32_t v = lhs.asInt32();
            if (shift < 31) return Value::fromInt32(v >> shift);
            return Value::fromInt32(v < 0 ? -1 : 0);
        }
        bool lhsIsInt = lhs.isInt32() || lhs.isBigInt() || lhs.isBool();
        if (lhsIsInt) {
            BigInt lVal = lhs.isBool() ? BigInt(lhs.asBool() ? 1 : 0) : lhs.asBigInt();
            BigInt divisor = BigInt(2).pow(shift);
            BigInt res = lVal / divisor;
            if (lVal.isNegative() && !(lVal % divisor).isZero()) {
                res = res - BigInt(1);
            }
            return Value(res);
        }
        throw std::runtime_error("Type Error: Bitwise SHIFT RIGHT '>>' not supported for '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline Value operator|(const Value& lhs, const Value& rhs) {
        auto promoteToType = [](const Value& v) -> ObjTypeDef* {
            if (v.isType()) return static_cast<ObjTypeDef*>(v.asObj());
            if (v.isClass()) {
                ObjTypeDef* td = GcHeap::get().allocate<ObjTypeDef>();
                td->types.push_back(static_cast<ObjClass*>(v.asObj()));
                return td;
            }
            return nullptr;
        };
        ObjTypeDef* lType = promoteToType(lhs);
        ObjTypeDef* rType = promoteToType(rhs);
        if (lType && rType) {
            ObjTypeDef* res = GcHeap::get().allocate<ObjTypeDef>();
            res->types = lType->types;
            res->types.insert(res->types.end(), rType->types.begin(), rType->types.end());
            res->normalize();
            return Value(res);
        }

        if (lhs.isObjType(ObjType::SET) && rhs.isObjType(ObjType::SET)) {
            ObjSet* s1 = static_cast<ObjSet*>(lhs.asObj());
            ObjSet* s2 = static_cast<ObjSet*>(rhs.asObj());
            ObjSet* res = GcHeap::get().allocate<ObjSet>();
            for (const auto& val : s1->elements) { res->add(val); }
            for (const auto& val : s2->elements) { res->add(val); }
            return Value(res);
        }
        if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.bitOr(static_cast<ObjBaseNum*>(rhs.asObj())->base));
        if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.bitOr(BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix())));
        if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()).bitOr(static_cast<ObjBaseNum*>(rhs.asObj())->base));
        
        if (lhs.isInt32() && rhs.isInt32()) return Value::fromInt32(lhs.asInt32() | rhs.asInt32());
        
        bool lhsIsInt = lhs.isInt32() || lhs.isBigInt() || lhs.isBool();
        bool rhsIsInt = rhs.isInt32() || rhs.isBigInt() || rhs.isBool();
        if (lhsIsInt && rhsIsInt) {
            BigInt lVal = lhs.isBool() ? BigInt(lhs.asBool() ? 1 : 0) : lhs.asBigInt();
            BigInt rVal = rhs.isBool() ? BigInt(rhs.asBool() ? 1 : 0) : rhs.asBigInt();
            BigInt res = BaseNum(lVal, 2).bitOr(BaseNum(rVal, 2)).getValue();
            return Value(res);
        }
        throw std::runtime_error("Type Error: Bitwise/Set OR '|' not supported for '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline Value bitXor(const Value& lhs, const Value& rhs) {
        if (lhs.isInt32() && rhs.isInt32()) return Value::fromInt32(lhs.asInt32() ^ rhs.asInt32());
        
        if (lhs.isObjType(ObjType::SET) && rhs.isObjType(ObjType::SET)) {
            ObjSet* s1 = static_cast<ObjSet*>(lhs.asObj());
            ObjSet* s2 = static_cast<ObjSet*>(rhs.asObj());
            ObjSet* res = GcHeap::get().allocate<ObjSet>();
            for (const auto& val : s1->elements) {
                if (s2->keys.find(val) == s2->keys.end()) {
                    res->add(val);
                }
            }
            for (const auto& val : s2->elements) {
                if (s1->keys.find(val) == s1->keys.end()) {
                    res->add(val);
                }
            }
            return Value(res);
        }
        if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.bitXor(static_cast<ObjBaseNum*>(rhs.asObj())->base));
        if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.bitXor(BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix())));
        if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()).bitXor(static_cast<ObjBaseNum*>(rhs.asObj())->base));
        
        bool lhsIsInt = lhs.isInt32() || lhs.isBigInt() || lhs.isBool();
        bool rhsIsInt = rhs.isInt32() || rhs.isBigInt() || rhs.isBool();
        if (lhsIsInt && rhsIsInt) {
            BigInt lVal = lhs.isBool() ? BigInt(lhs.asBool() ? 1 : 0) : lhs.asBigInt();
            BigInt rVal = rhs.isBool() ? BigInt(rhs.asBool() ? 1 : 0) : rhs.asBigInt();
            BigInt res = BaseNum(lVal, 2).bitXor(BaseNum(rVal, 2)).getValue();
            return Value(res);
        }
        throw std::runtime_error("Type Error: Bitwise/Set XOR '^^' not supported for '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline std::string Value::toJC2Expression() const {
        static thread_local std::vector<const void*> visited;
        if (isNone()) return "none()";
        if (isUninit()) return "uninit()";
        if (isBool()) return asBool() ? "true" : "false";
        if (isInt32()) return std::to_string(asInt32());
        if (isDouble()) {
            std::ostringstream oss;
            oss << std::setprecision(16) << asDoubleRaw();
            std::string s = oss.str();
            if (std::isfinite(asDoubleRaw()) && s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
                s += ".0";
            }
            return s;
        }
        Obj* obj = asObj();
        switch (obj->type) {
            case ObjType::STRING: return "\"" + static_cast<ObjString*>(obj)->str + "\"";
            case ObjType::BIGINT: return static_cast<ObjBigInt*>(obj)->num.toString();
            case ObjType::FRACTION: return static_cast<ObjFraction*>(obj)->frac.toString();
            case ObjType::BASENUM: return "base(" + static_cast<ObjBaseNum*>(obj)->base.getValue().toString() + ", " + std::to_string(static_cast<ObjBaseNum*>(obj)->base.getRadix()) + ")";
            case ObjType::COMPLEX: {
                auto formatDouble = [](double v) {
                    std::ostringstream temp;
                    temp << std::setprecision(16) << v;
                    std::string s = temp.str();
                    if (std::isfinite(v) && s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
                        s += ".0";
                    }
                    return s;
                };
                return "(" + formatDouble(static_cast<ObjComplex*>(obj)->comp.real) + ") + (" + formatDouble(static_cast<ObjComplex*>(obj)->comp.imag) + ")*i";
            }
            case ObjType::REAL_MATRIX: {
                const auto& mat = static_cast<ObjRealMatrix*>(obj)->mat;
                if (mat.getRows() * mat.getCols() == 0) return "[]";
                std::string res = "[";
                for (int i = 0; i < mat.getRows(); ++i) {
                    for (int j = 0; j < mat.getCols(); ++j) {
                        res += Value(mat(i, j)).toJC2Expression();
                        if (j < mat.getCols() - 1) res += ", ";
                    }
                    if (i < mat.getRows() - 1) res += "; ";
                }
                return res + "]";
            }
            case ObjType::COMPLEX_MATRIX: {
                const auto& mat = static_cast<ObjComplexMatrix*>(obj)->mat;
                if (mat.getRows() * mat.getCols() == 0) return "[]";
                std::string res = "[";
                for (int i = 0; i < mat.getRows(); ++i) {
                    for (int j = 0; j < mat.getCols(); ++j) {
                        res += Value(mat(i, j)).toJC2Expression();
                        if (j < mat.getCols() - 1) res += ", ";
                    }
                    if (i < mat.getRows() - 1) res += "; ";
                }
                return res + "]";
            }
            case ObjType::STRING_MATRIX: {
                const auto& mat = static_cast<ObjStringMatrix*>(obj)->mat;
                if (mat.getRows() * mat.getCols() == 0) return "[]";
                std::string res = "strmat(" + std::to_string(mat.getRows()) + ", " + std::to_string(mat.getCols());
                for (int i = 0; i < mat.getRows(); ++i)
                    for (int j = 0; j < mat.getCols(); ++j)
                        res += ", \"" + mat(i, j) + "\"";
                return res + ")";
            }
            case ObjType::CLOSURE: return "\"<function>\"";
            case ObjType::CLASS: {
                std::string n = static_cast<ObjClass*>(obj)->name;
                return n.empty() ? "\"<anonymous class>\"" : "\"<class " + n + ">\"";
            }
            case ObjType::INSTANCE: {
                auto inst = static_cast<ObjInstance*>(obj);
                try {
                    auto [foundRepl, replRes] = invokeDunder(inst, "__repr__");
                    if (foundRepl) {
                        return replRes.toJC2Expression();
                    }
                    auto [foundStr, strRes] = invokeDunder(inst, "__str__");
                    if (foundStr) {
                        return strRes.toJC2Expression();
                    }
                } catch (...) {}
                std::string cname = inst->classDef ? inst->classDef->name : "unknown";
                if (cname.empty()) return "\"<anonymous class instance>\"";
                return "\"<" + cname + " instance>\"";
            }
            case ObjType::SUPER_PROXY: return "\"<super>\"";
            case ObjType::SYMBOLIC: return "sym(\" " + static_cast<ObjSym*>(obj)->sym.toString() + "\")";
            case ObjType::NAMESPACE: {
                std::string n = static_cast<ObjNamespace*>(obj)->name;
                return n.empty() ? "\"<anonymous namespace>\"" : "\"<namespace " + n + ">\"";
            }
            case ObjType::TYPE_DEF: return "<type '" + static_cast<ObjTypeDef*>(obj)->name() + "'>";
            case ObjType::LIST: {
                ObjList* list = static_cast<ObjList*>(obj);
                RecursionGuard guard(visited, list);
                if (guard.isCycle) return "list()";
                std::string res = "list(";
                for (size_t i = 0; i < list->vec.size(); ++i) {
                    try { res += list->vec[i].toJC2Expression(); } catch (...) { res += "0"; }
                    if (i < list->vec.size() - 1) res += ", ";
                }
                return res + ")";
            }
            case ObjType::DICT: {
                ObjDict* dict = static_cast<ObjDict*>(obj);
                RecursionGuard guard(visited, dict);
                if (guard.isCycle) return "dict()";
                std::string res = "dict(";
                for (size_t i = 0; i < dict->elements.size(); ++i) {
                    try { res += dict->elements[i].first.toJC2Expression(); } catch (...) { res += "0"; }
                    res += ", ";
                    try { res += dict->elements[i].second.toJC2Expression(); } catch (...) { res += "0"; }
                    if (i < dict->elements.size() - 1) res += ", ";
                }
                return res + ")";
            }
            case ObjType::SET: {
                ObjSet* set = static_cast<ObjSet*>(obj);
                RecursionGuard guard(visited, set);
                if (guard.isCycle) return "Set()";
                std::string res = "Set(";
                for (size_t i = 0; i < set->elements.size(); ++i) {
                    try { res += set->elements[i].toJC2Expression(); } catch (...) { res += "0"; }
                    if (i < set->elements.size() - 1) res += ", ";
                }
                return res + ")";
            }
        }
        return "none()";
    }

    inline Value ldivide(const Value& lhs, const Value& rhs) {
        if (lhs.isObjType(ObjType::REAL_MATRIX)) {
            return Value(lhs.asRealMatrix().inverse()) * rhs;
        }
        if (lhs.isObjType(ObjType::COMPLEX_MATRIX)) {
            return Value(lhs.asComplexMatrix().inverse()) * rhs;
        }
        return rhs / lhs;
    }

    struct ObjClosure : public Obj {
        std::vector<std::string> paramNames;
        std::vector<bool> isRef;
        std::string rawBody;
        std::shared_ptr<Expr> body;
        
        // ★ 扁平化存储上值，彻底抛弃 std::vector 和 std::shared_ptr
        ObjUpVal** upvalues = nullptr;
        int upvalueCount = 0;

        Value* paramTypes = nullptr;
        int paramTypesCount = 0;
        Value returnType = Value::none();

        std::any nativeFn;
        int compiledFnIndex = -1;
        std::vector<Value> defaultValues;
        bool hasRestParam = false;
        bool isUFCS = false; // ★ 新增：标记是否为 UFCS 绑定的全局函数
        bool isTokenMacro = false; // ★ 新增：标记是否为 Token 宏
        bool is_local = false; // ★ 新增：标记是否为私有方法

        Value boundSelf;
        Value boundClass;
        ObjClass* owner_class = nullptr; // ★ 新增：闭包定义时所在的类（用于私有属性访问控制）

        int minArgs() const {
            int count = static_cast<int>(paramNames.size());
            if (hasRestParam && count > 0) count--;
            count -= static_cast<int>(defaultValues.size());
            return count < 0 ? 0 : count;
        }
        int maxArgs() const { return static_cast<int>(paramNames.size()); }
        bool acceptsArgCount(int n) const { return n >= minArgs() && (hasRestParam || n <= maxArgs()); }
        bool hasRef() const { for (bool b : isRef) if (b) return true; return false; }
        bool hasCaptures() const { return upvalueCount > 0; }
        bool isNative() const { return nativeFn.has_value(); }
        bool isBytecode() const { return compiledFnIndex >= 0; }

        ObjClosure(std::vector<std::string> paramNames, std::vector<bool> isRef,
            std::string rawBody, std::shared_ptr<Expr> body, bool hasRestParam = false)
            : paramNames(std::move(paramNames)), isRef(std::move(isRef)),
            rawBody(std::move(rawBody)), body(std::move(body)), hasRestParam(hasRestParam) {
            type = ObjType::CLOSURE;
        }

        ~ObjClosure() {
            if (upvalues) delete[] upvalues;
            if (paramTypes) delete[] paramTypes;
        }

        void clear() override {
            boundSelf = Value();
            boundClass = Value();
            defaultValues.clear();
            nativeFn.reset();
            // 注意：upvalues 数组的清理交给析构函数，这里不需要清空，GC 会处理 ObjUpVal
        }

        std::string toString() const {
            std::string params;
            for (size_t i = 0; i < paramNames.size(); ++i) {
                if (i < isRef.size() && isRef[i]) params += "ref ";
                params += paramNames[i];
                if (i < paramNames.size() - 1) params += ", ";
            }
            if (isNative() && !isBytecode()) return "<function " + rawBody + "(" + params + ")>";
            return "<function(" + params + ")>";
        }
    };

    struct ErrorSignal : public std::runtime_error {
        std::string message;
        explicit ErrorSignal(const std::string& msg) : std::runtime_error(msg), message(msg) {}
    };

    struct StackTracedException : public std::runtime_error {
        std::string rawMessage;

        StackTracedException(const std::string& raw, const std::string& fullTraceText)
            : std::runtime_error(fullTraceText), rawMessage(raw) {}
    };

    inline bool Value::isHashable() const {
        static thread_local std::vector<const void*> visited;
        if (isNumber() || isBool() || isNone() || isUninit()) return true;
        Obj* obj = asObj();
        switch (obj->type) {
            case ObjType::STRING:
            case ObjType::BIGINT:
            case ObjType::FRACTION:
            case ObjType::COMPLEX:
            case ObjType::BASENUM:
            case ObjType::REAL_MATRIX:
            case ObjType::COMPLEX_MATRIX:
            case ObjType::STRING_MATRIX:
            case ObjType::CLASS:
            case ObjType::SYMBOLIC:
            case ObjType::CLOSURE:
            case ObjType::NAMESPACE:
            case ObjType::SUPER_PROXY:
            case ObjType::UPVALUE:
            case ObjType::TYPE_DEF:
                return true;
            case ObjType::LIST: {
                ObjList* list = static_cast<ObjList*>(obj);
                if (!list->is_frozen) return false;
                if (list->is_hashable_cached) return true;
                RecursionGuard guard(visited, list);
                if (guard.isCycle) return true;
                for (const auto& e : list->vec) {
                    try { if (!e.isHashable()) return false; } catch (...) { return false; }
                }
                list->is_hashable_cached = true;
                return true;
            }
            case ObjType::DICT: {
                ObjDict* dict = static_cast<ObjDict*>(obj);
                if (!dict->is_frozen) return false;
                if (dict->is_hashable_cached) return true;
                RecursionGuard guard(visited, dict);
                if (guard.isCycle) return true;
                for (const auto& [k, v] : dict->elements) {
                    try { if (!v.isHashable()) return false; } catch (...) { return false; }
                }
                dict->is_hashable_cached = true;
                return true;
            }
            case ObjType::SET: {
                ObjSet* set = static_cast<ObjSet*>(obj);
                if (!set->is_frozen) return false;
                if (set->is_hashable_cached) return true;
                RecursionGuard guard(visited, set);
                if (guard.isCycle) return true;
                for (const auto& v : set->elements) {
                    try { if (!v.isHashable()) return false; } catch (...) { return false; }
                }
                set->is_hashable_cached = true;
                return true;
            }
            case ObjType::INSTANCE: {
                ObjInstance* inst = static_cast<ObjInstance*>(obj);
                if (inst->is_hashable_cached) return true;
                RecursionGuard guard(visited, inst);
                if (guard.isCycle) return true;
                auto c = inst->classDef;
                while (c) {
                    if (c->properties.count("__hash__")) {
                        inst->is_hashable_cached = true;
                        return true;
                    }
                    c = c->parent;
                }
                if (inst->is_frozen) {
                    for (const auto& [k, prop] : inst->properties) {
                        try { if (!prop.val.isHashable()) return false; } catch (...) { return false; }
                    }
                    inst->is_hashable_cached = true;
                    return true;
                }
                return false; // 未冻结的普通实例不可哈希（防止冻结后哈希算法突变导致字典损坏）
            }
        }
        return false;
    }

    inline bool Value::truthy() const {
        if (isNone()) return false;
        if (isUninit()) return false;
        if (isBool()) return asBool();
        if (isInt32()) return asInt32() != 0;
        if (isDouble()) {
            double d = asDoubleRaw();
            return d != 0.0 && !std::isnan(d);
        }
        Obj* obj = asObj();
        switch (obj->type) {
            case ObjType::BIGINT: return !static_cast<ObjBigInt*>(obj)->num.isZero();
            case ObjType::COMPLEX: {
                const auto& c = static_cast<ObjComplex*>(obj)->comp;
                return c.real != 0.0 || c.imag != 0.0;
            }
            case ObjType::FRACTION: return !static_cast<ObjFraction*>(obj)->frac.getNum().isZero();
            case ObjType::BASENUM: return !static_cast<ObjBaseNum*>(obj)->base.getValue().isZero();
            case ObjType::STRING: return !static_cast<ObjString*>(obj)->str.empty();
            case ObjType::LIST: return !static_cast<ObjList*>(obj)->vec.empty();
            case ObjType::DICT: return !static_cast<ObjDict*>(obj)->elements.empty();
            case ObjType::SET: return !static_cast<ObjSet*>(obj)->elements.empty();
            case ObjType::SYMBOLIC: return !static_cast<ObjSym*>(obj)->sym.isZero();
            case ObjType::NAMESPACE: return true;
            case ObjType::INSTANCE: {
                auto inst = static_cast<ObjInstance*>(obj);
                auto [found, res] = invokeDunder(inst, "__bool__");
                if (found) {
                    if (res.isBool()) return res.asBool();
                    if (res.isNumber()) return res.asDouble() != 0.0;
                    return res.truthy();
                }
                return true;
            }
            default: return true;
        }
    }

    inline bool Value::equals(const Value& lhs, const Value& rhs) {
        if (lhs.as_bits == rhs.as_bits) return true;

        // 防循环递归锁
        static thread_local std::vector<std::pair<const void*, const void*>> comparingPairs;
        struct CompGuard {
            std::vector<std::pair<const void*, const void*>>& vec;
            CompGuard(std::vector<std::pair<const void*, const void*>>& v, const std::pair<const void*, const void*>& p) : vec(v) { vec.push_back(p); }
            ~CompGuard() { vec.pop_back(); }
        };

        bool lIsInt = lhs.isInt32() || lhs.isBool();
        bool rIsInt = rhs.isInt32() || rhs.isBool();
        if (lIsInt && rIsInt) {
            int32_t a = lhs.isInt32() ? lhs.asInt32() : (lhs.asBool() ? 1 : 0);
            int32_t b = rhs.isInt32() ? rhs.asInt32() : (rhs.asBool() ? 1 : 0);
            return a == b;
        }
        if (lhs.isNumber() && rhs.isNumber()) return lhs.asNumber() == rhs.asNumber();
        if (lhs.isNone() || rhs.isNone()) return false;
        if (lhs.isUninit() || rhs.isUninit()) return false;

        // 同类型快速通道
        if (lhs.isType() && rhs.isString()) return static_cast<ObjTypeDef*>(lhs.asObj())->name() == rhs.asString();
        if (rhs.isType() && lhs.isString()) return static_cast<ObjTypeDef*>(rhs.asObj())->name() == lhs.asString();
        if (lhs.isType() && rhs.isClass()) {
            auto td = static_cast<ObjTypeDef*>(lhs.asObj());
            return td->types.size() == 1 && std::holds_alternative<ObjClass*>(td->types[0]) && std::get<ObjClass*>(td->types[0]) == static_cast<ObjClass*>(rhs.asObj());
        }
        if (rhs.isType() && lhs.isClass()) {
            auto td = static_cast<ObjTypeDef*>(rhs.asObj());
            return td->types.size() == 1 && std::holds_alternative<ObjClass*>(td->types[0]) && std::get<ObjClass*>(td->types[0]) == static_cast<ObjClass*>(lhs.asObj());
        }

        if (lhs.isObj() && rhs.isObj() && lhs.asObj()->type == rhs.asObj()->type) {
            Obj* lobj = lhs.asObj();
            Obj* robj = rhs.asObj();
            if (lobj == robj) return true; // ★ 极速通道：同一对象直接返回 true
            switch (lobj->type) {
                case ObjType::STRING: return lobj == robj; // ★ 字符串驻留池，直接比较指针
                case ObjType::BIGINT: return static_cast<ObjBigInt*>(lobj)->num == static_cast<ObjBigInt*>(robj)->num;
                case ObjType::COMPLEX: return static_cast<ObjComplex*>(lobj)->comp == static_cast<ObjComplex*>(robj)->comp;
                case ObjType::FRACTION: return static_cast<ObjFraction*>(lobj)->frac == static_cast<ObjFraction*>(robj)->frac;
                case ObjType::BASENUM: return static_cast<ObjBaseNum*>(lobj)->base.getValue() == static_cast<ObjBaseNum*>(robj)->base.getValue();
                case ObjType::SYMBOLIC: return static_cast<ObjSym*>(lobj)->sym == static_cast<ObjSym*>(robj)->sym;
                case ObjType::REAL_MATRIX: {
                    const auto& a = static_cast<ObjRealMatrix*>(lobj)->mat;
                    const auto& b = static_cast<ObjRealMatrix*>(robj)->mat;
                    if (a.getRows() != b.getRows() || a.getCols() != b.getCols()) return false;
                    for (int i = 0; i < a.getRows(); ++i)
                        for (int j = 0; j < a.getCols(); ++j)
                            if (a(i, j) != b(i, j)) return false;
                    return true;
                }
                case ObjType::COMPLEX_MATRIX: {
                    const auto& a = static_cast<ObjComplexMatrix*>(lobj)->mat;
                    const auto& b = static_cast<ObjComplexMatrix*>(robj)->mat;
                    if (a.getRows() != b.getRows() || a.getCols() != b.getCols()) return false;
                    for (int i = 0; i < a.getRows(); ++i)
                        for (int j = 0; j < a.getCols(); ++j)
                            if (!(a(i, j) == b(i, j))) return false;
                    return true;
                }
                case ObjType::STRING_MATRIX: {
                    const auto& a = static_cast<ObjStringMatrix*>(lobj)->mat;
                    const auto& b = static_cast<ObjStringMatrix*>(robj)->mat;
                    if (a.getRows() != b.getRows() || a.getCols() != b.getCols()) return false;
                    for (int i = 0; i < a.getRows(); ++i)
                        for (int j = 0; j < a.getCols(); ++j)
                            if (a(i, j) != b(i, j)) return false;
                    return true;
                }
                case ObjType::LIST: {
                    const auto& a = static_cast<ObjList*>(lobj)->vec;
                    const auto& b = static_cast<ObjList*>(robj)->vec;
                    if (a.size() != b.size()) return false;
                    auto pair = lobj < robj ? std::make_pair((const void*)lobj, (const void*)robj) : std::make_pair((const void*)robj, (const void*)lobj);
                    if (std::find(comparingPairs.begin(), comparingPairs.end(), pair) != comparingPairs.end()) return true;
                    CompGuard guard(comparingPairs, pair);
                    bool eq = true;
                    for (size_t i = 0; i < a.size(); ++i) {
                        try { if (!equals(a[i], b[i])) { eq = false; break; } }
                        catch (...) { eq = false; break; }
                    }
                    return eq;
                }
                case ObjType::DICT: {
                    auto a = static_cast<ObjDict*>(lobj);
                    auto b = static_cast<ObjDict*>(robj);
                    if (a->elements.size() != b->elements.size()) return false;
                    auto pair = lobj < robj ? std::make_pair((const void*)lobj, (const void*)robj) : std::make_pair((const void*)robj, (const void*)lobj);
                    if (std::find(comparingPairs.begin(), comparingPairs.end(), pair) != comparingPairs.end()) return true;
                    CompGuard guard(comparingPairs, pair);
                    bool eq = true;
                    for (const auto& [key, val] : a->elements) {
                        auto it = b->keyMap.find(key);
                        if (it == b->keyMap.end()) { eq = false; break; }
                        try { if (!equals(val, b->elements[it->second].second)) { eq = false; break; } }
                        catch (...) { eq = false; break; }
                    }
                    return eq;
                }
                case ObjType::SET: {
                    auto a = static_cast<ObjSet*>(lobj);
                    auto b = static_cast<ObjSet*>(robj);
                    if (a->elements.size() != b->elements.size()) return false;
                    for (const auto& val : a->elements) {
                        if (b->keys.find(val) == b->keys.end()) return false;
                    }
                    return true;
                }
                case ObjType::INSTANCE: {
                    auto inst1 = static_cast<ObjInstance*>(lobj);
                    auto [found, res] = invokeDunder(inst1, "__eq__", {rhs});
                    if (found) return res.truthy();
                    auto inst2 = static_cast<ObjInstance*>(robj);
                    if (inst1->is_frozen && inst2->is_frozen && inst1->classDef == inst2->classDef) {
                        if (inst1->properties.size() != inst2->properties.size()) return false;
                        
                        auto pair = lobj < robj ? std::make_pair((const void*)lobj, (const void*)robj) : std::make_pair((const void*)robj, (const void*)lobj);
                        if (std::find(comparingPairs.begin(), comparingPairs.end(), pair) != comparingPairs.end()) return true;
                        CompGuard guard(comparingPairs, pair);
                        
                        for (const auto& [k, prop1] : inst1->properties) {
                            auto it = inst2->properties.find(k);
                            if (it == inst2->properties.end()) return false;
                            try { if (!equals(prop1.val, it->second.val)) return false; }
                            catch (...) { return false; }
                        }
                        return true;
                    }
                    return false;
                }
                case ObjType::CLOSURE:
                case ObjType::CLASS:
                case ObjType::NAMESPACE:
                case ObjType::UPVALUE:
                    return false; // Pointer equality already checked
                case ObjType::TYPE_DEF: {
                    auto a = static_cast<ObjTypeDef*>(lobj);
                    auto b = static_cast<ObjTypeDef*>(robj);
                    if (a->types.size() != b->types.size()) return false;
                    for (size_t i = 0; i < a->types.size(); ++i) {
                        if (a->types[i] != b->types[i]) return false;
                    }
                    return true;
                }
                case ObjType::SUPER_PROXY: {
                    auto sp1 = static_cast<ObjSuper*>(lobj);
                    auto sp2 = static_cast<ObjSuper*>(robj);
                    return sp1->instance == sp2->instance && sp1->parentClass == sp2->parentClass;
                }
            }
        }

        // 跨类型兼容比较
        if (lhs.isObjType(ObjType::BIGINT) && rhs.isObjType(ObjType::FRACTION))
            return Fraction(static_cast<ObjBigInt*>(lhs.asObj())->num) == static_cast<ObjFraction*>(rhs.asObj())->frac;
        if (lhs.isObjType(ObjType::FRACTION) && rhs.isObjType(ObjType::BIGINT))
            return static_cast<ObjFraction*>(lhs.asObj())->frac == Fraction(static_cast<ObjBigInt*>(rhs.asObj())->num);
        if ((lhs.isObjType(ObjType::REAL_MATRIX) && rhs.isObjType(ObjType::COMPLEX_MATRIX)) ||
            (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhs.isObjType(ObjType::REAL_MATRIX))) {
            try {
                ComplexMatrix a = lhs.asComplexMatrix(), b = rhs.asComplexMatrix();
                if (a.getRows() != b.getRows() || a.getCols() != b.getCols()) return false;
                for (int i = 0; i < a.getRows(); ++i)
                    for (int j = 0; j < a.getCols(); ++j)
                        if (!(a(i, j) == b(i, j))) return false;
                return true;
            }
            catch (...) { return false; }
        }

        if (lhs.isInstance()) {
            auto [found, res] = invokeDunder(lhs.asInstance(), "__eq__", {rhs});
            if (found) return res.truthy();
        }
        if (rhs.isInstance()) {
            auto [found, res] = invokeDunder(rhs.asInstance(), "__eq__", {lhs});
            if (found) return res.truthy();
        }

        if (lhs.isObjType(ObjType::BASENUM)) return equals(Value(static_cast<ObjBaseNum*>(lhs.asObj())->base.getValue()), rhs);
        if (rhs.isObjType(ObjType::BASENUM)) return equals(lhs, Value(static_cast<ObjBaseNum*>(rhs.asObj())->base.getValue()));

        auto getNumeric = [](const Value& v) -> std::optional<Complex> {
            try {
                if (v.isNumber()) return Complex(v.asNumber());
                if (v.isBool()) return Complex(v.asBool() ? 1.0 : 0.0);
                if (v.isObjType(ObjType::COMPLEX)) return static_cast<ObjComplex*>(v.asObj())->comp;
            } catch (...) {}
            return std::nullopt;
        };

        auto fractionEqualsDouble = [](const Fraction& f, double d) -> bool {
            if (!std::isfinite(d)) return false;
            try {
                return Fraction::fromDouble(d) == f;
            } catch (...) { return false; }
        };

        if (lhs.isObjType(ObjType::FRACTION) && !rhs.isObjType(ObjType::FRACTION)) {
            const Fraction& f = static_cast<ObjFraction*>(lhs.asObj())->frac;
            if (rhs.isNumber()) return fractionEqualsDouble(f, rhs.asNumber());
            if (rhs.isBool()) return fractionEqualsDouble(f, rhs.asBool() ? 1.0 : 0.0);
            if (rhs.isObjType(ObjType::COMPLEX)) {
                const auto& c = static_cast<ObjComplex*>(rhs.asObj())->comp;
                return c.imag == 0.0 && fractionEqualsDouble(f, c.real);
            }
        }
        if (rhs.isObjType(ObjType::FRACTION) && !lhs.isObjType(ObjType::FRACTION)) {
            const Fraction& f = static_cast<ObjFraction*>(rhs.asObj())->frac;
            if (lhs.isNumber()) return fractionEqualsDouble(f, lhs.asNumber());
            if (lhs.isBool()) return fractionEqualsDouble(f, lhs.asBool() ? 1.0 : 0.0);
            if (lhs.isObjType(ObjType::COMPLEX)) {
                const auto& c = static_cast<ObjComplex*>(lhs.asObj())->comp;
                return c.imag == 0.0 && fractionEqualsDouble(f, c.real);
            }
        }

        auto numL = getNumeric(lhs);
        auto numR = getNumeric(rhs);

        if (numL && numR) return *numL == *numR;

        if (lhs.isObjType(ObjType::BIGINT) && numR) {
            const BigInt& b = static_cast<ObjBigInt*>(lhs.asObj())->num;
            if (numR->imag != 0.0) return false;
            double d = numR->real;
            if (std::floor(d) != d) return false;
            if (std::abs(d) > 9007199254740992.0) return false;
            if (std::abs(d) < 9e15) return b == BigInt(static_cast<int64_t>(d));
            try { return b.toDouble() == d; } catch (...) { return false; }
        }
        if (rhs.isObjType(ObjType::BIGINT) && numL) {
            const BigInt& b = static_cast<ObjBigInt*>(rhs.asObj())->num;
            if (numL->imag != 0.0) return false;
            double d = numL->real;
            if (std::floor(d) != d) return false;
            if (std::abs(d) > 9007199254740992.0) return false;
            if (std::abs(d) < 9e15) return b == BigInt(static_cast<int64_t>(d));
            try { return b.toDouble() == d; } catch (...) { return false; }
        }

        return false;
    }

    inline std::string Value::typeName() const {
        if (isNone()) return "none_type";
        if (isUninit()) return "uninit";
        if (isBool()) return "bool";
        if (isInt32()) return "int";
        if (isDouble()) return "double";
        Obj* obj = asObj();
        switch (obj->type) {
            case ObjType::STRING: return "string";
            case ObjType::BIGINT: return "int";
            case ObjType::FRACTION: return "fraction";
            case ObjType::COMPLEX: return "complex";
            case ObjType::BASENUM: return "basenum";
            case ObjType::REAL_MATRIX: return "realmatrix";
            case ObjType::COMPLEX_MATRIX: return "complexmatrix";
            case ObjType::STRING_MATRIX: return "stringmatrix";
            case ObjType::LIST: return "list";
            case ObjType::DICT: return "dict";
            case ObjType::SET: return "set";
            case ObjType::CLOSURE: return "function";
            case ObjType::CLASS: return "class_type";
            case ObjType::INSTANCE: {
                auto inst = static_cast<ObjInstance*>(obj);
                if (inst->classDef && !inst->classDef->name.empty()) return inst->classDef->name;
                return "instance";
            }
            case ObjType::SUPER_PROXY: return "super";
            case ObjType::SYMBOLIC: return "symbolic";
            case ObjType::NAMESPACE: return "namespace_type";
            case ObjType::UPVALUE: return "upvalue";
            case ObjType::TYPE_DEF: return "type";
        }
        return "unknown";
    }

    inline Value Value::operator~() const {
        if (isInt32()) return Value::fromInt32(~asInt32());
        if (isBool()) return Value::fromInt32(~(asBool() ? 1 : 0));
        if (isObjType(ObjType::BASENUM)) {
            auto& base = static_cast<ObjBaseNum*>(asObj())->base;
            return Value(BaseNum(-base.getValue() - BigInt(1), base.getRadix()));
        }
        if (isBigInt()) return Value(-asBigInt() - BigInt(1));
        throw std::runtime_error("Type Error: Bitwise NOT '~' not supported for '" + typeName() + "'.");
    }

    inline Value Value::operator-() const {
        if (isInt32()) {
            int32_t v = asInt32();
            if (v == -2147483648) return Value(BigInt(2147483648LL));
            return Value::fromInt32(-v);
        }
        if (isBool()) return Value::fromInt32(asBool() ? -1 : 0);
        if (isDouble()) return Value(-asDoubleRaw());
        if (isObj()) {
            Obj* obj = asObj();
            switch (obj->type) {
                case ObjType::BIGINT: return Value(-static_cast<ObjBigInt*>(obj)->num);
                case ObjType::FRACTION: return Value(-static_cast<ObjFraction*>(obj)->frac);
                case ObjType::COMPLEX: return Value(-static_cast<ObjComplex*>(obj)->comp);
                case ObjType::BASENUM: return Value(-static_cast<ObjBaseNum*>(obj)->base);
                case ObjType::REAL_MATRIX: return Value(-static_cast<ObjRealMatrix*>(obj)->mat);
                case ObjType::COMPLEX_MATRIX: return Value(-static_cast<ObjComplexMatrix*>(obj)->mat);
                case ObjType::SYMBOLIC: return Value(-static_cast<ObjSym*>(obj)->sym);
                default: break;
            }
        }
        throw std::runtime_error("Type Error: Cannot negate '" + typeName() + "'.");
    }

    inline Value operator+(const Value& lhs, const Value& rhs) {
        bool lIsInt = lhs.isInt32() || lhs.isBool();
        bool rIsInt = rhs.isInt32() || rhs.isBool();
        if (lIsInt && rIsInt) {
            int32_t a = lhs.isInt32() ? lhs.asInt32() : (lhs.asBool() ? 1 : 0);
            int32_t b = rhs.isInt32() ? rhs.asInt32() : (rhs.asBool() ? 1 : 0);
            int64_t sum = static_cast<int64_t>(a) + b;
            if (sum >= -2147483648LL && sum <= 2147483647LL) return Value::fromInt32(static_cast<int32_t>(sum));
            return Value(BigInt(sum));
        }
        if (lhs.isNumber() && rhs.isNumber()) return Value(lhs.asNumber() + rhs.asNumber());
        if (lhs.isObjType(ObjType::STRING) && rhs.isObjType(ObjType::STRING)) {
            return Value(static_cast<ObjString*>(lhs.asObj())->str + static_cast<ObjString*>(rhs.asObj())->str);
        }
        if (lhs.isObjType(ObjType::LIST) && rhs.isObjType(ObjType::LIST)) {
            ObjList* l1 = static_cast<ObjList*>(lhs.asObj());
            ObjList* l2 = static_cast<ObjList*>(rhs.asObj());
            ObjList* res = GcHeap::get().allocate<ObjList>();
            res->vec.reserve(l1->vec.size() + l2->vec.size());
            res->vec.insert(res->vec.end(), l1->vec.begin(), l1->vec.end());
            res->vec.insert(res->vec.end(), l2->vec.begin(), l2->vec.end());
            return Value(res);
        }
        if (lhs.isObjType(ObjType::DICT) && rhs.isObjType(ObjType::DICT)) {
            ObjDict* d1 = static_cast<ObjDict*>(lhs.asObj());
            ObjDict* d2 = static_cast<ObjDict*>(rhs.asObj());
            ObjDict* res = GcHeap::get().allocate<ObjDict>();
            for (const auto& [k, v] : d1->elements) {
                res->set(k, v);
            }
            for (const auto& [k, v] : d2->elements) {
                res->set(k, v);
            }
            return Value(res);
        }
        
        if (lhs.isObjType(ObjType::REAL_MATRIX) && rhs.isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(lhs.asObj())->mat + static_cast<ObjRealMatrix*>(rhs.asObj())->mat);
            if (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhs.isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(lhs.asObj())->mat + static_cast<ObjComplexMatrix*>(rhs.asObj())->mat);
            if (lhs.isObjType(ObjType::REAL_MATRIX) && rhs.isObjType(ObjType::COMPLEX_MATRIX)) return Value(lhs.asComplexMatrix() + rhs.asComplexMatrix());
            if (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhs.isObjType(ObjType::REAL_MATRIX)) return Value(lhs.asComplexMatrix() + rhs.asComplexMatrix());
            
            bool lhsIsMat = lhs.isObjType(ObjType::REAL_MATRIX) || lhs.isObjType(ObjType::COMPLEX_MATRIX);
            bool rhsIsMat = rhs.isObjType(ObjType::REAL_MATRIX) || rhs.isObjType(ObjType::COMPLEX_MATRIX);
            bool lhsIsScalar = lhs.isNumber() || lhs.isBigInt() || lhs.isObjType(ObjType::FRACTION) || lhs.isComplex();
            bool rhsIsScalar = rhs.isNumber() || rhs.isBigInt() || rhs.isObjType(ObjType::FRACTION) || rhs.isComplex();

            if (lhsIsMat && rhsIsScalar) {
                if (lhs.isObjType(ObjType::REAL_MATRIX) && !rhs.isComplex()) {
                    RealMatrix m = static_cast<ObjRealMatrix*>(lhs.asObj())->mat;
                    if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar addition requires a square matrix.");
                    double c = rhs.asDouble();
                    for (int i = 0; i < m.getRows(); ++i) m(i, i) += c;
                    return Value(m);
                }
                ComplexMatrix m = lhs.asComplexMatrix();
                if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar addition requires a square matrix.");
                Complex c = rhs.asComplex();
                for (int i = 0; i < m.getRows(); ++i) m(i, i) = m(i, i) + c;
                return Value(m);
            }
            if (lhsIsScalar && rhsIsMat) {
                if (rhs.isObjType(ObjType::REAL_MATRIX) && !lhs.isComplex()) {
                    RealMatrix m = static_cast<ObjRealMatrix*>(rhs.asObj())->mat;
                    if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar addition requires a square matrix.");
                    double c = lhs.asDouble();
                    for (int i = 0; i < m.getRows(); ++i) m(i, i) += c;
                    return Value(m);
                }
                ComplexMatrix m = rhs.asComplexMatrix();
                if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar addition requires a square matrix.");
                Complex c = lhs.asComplex();
                for (int i = 0; i < m.getRows(); ++i) m(i, i) = m(i, i) + c;
                return Value(m);
            }
            
            if (lhs.isObjType(ObjType::STRING_MATRIX) && rhs.isObjType(ObjType::STRING_MATRIX)) {
                const auto& a = static_cast<ObjStringMatrix*>(lhs.asObj())->mat;
                const auto& b = static_cast<ObjStringMatrix*>(rhs.asObj())->mat;
                if (a.getRows() != b.getRows() || a.getCols() != b.getCols()) throw std::runtime_error("Type Error: StringMatrix dimensions must match for +.");
                std::vector<std::string> flat(a.getRows() * a.getCols());
                for (int i = 0; i < a.getRows(); ++i)
                    for (int j = 0; j < a.getCols(); ++j)
                        flat[i * a.getCols() + j] = a(i, j) + b(i, j);
                return Value(StringMatrix(a.getRows(), a.getCols(), flat));
            }

            if (lhs.isSymbolic() || rhs.isSymbolic()) return Value(lhs.asSymbolic() + rhs.asSymbolic());

            if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base + static_cast<ObjBaseNum*>(rhs.asObj())->base);
            if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base + BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix()));
            if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()) + static_cast<ObjBaseNum*>(rhs.asObj())->base);

            if (lhs.isObjType(ObjType::COMPLEX) || rhs.isObjType(ObjType::COMPLEX)) return Value(lhs.asComplex() + rhs.asComplex());
            
            bool lhsIsExactInt = lhs.isBigInt() || lhs.isInt32();
            bool rhsIsExactInt = rhs.isBigInt() || rhs.isInt32();
            if (lhsIsExactInt && rhsIsExactInt) return Value(lhs.asBigInt() + rhs.asBigInt());
            if (lhs.isObjType(ObjType::FRACTION) && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac + static_cast<ObjFraction*>(rhs.asObj())->frac);
            if (lhs.isObjType(ObjType::FRACTION) && rhsIsExactInt) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac + Fraction(rhs.asBigInt()));
            if (lhsIsExactInt && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(Fraction(lhs.asBigInt()) + static_cast<ObjFraction*>(rhs.asObj())->frac);
            
            if (lhs.isDouble() || rhs.isDouble()) return Value(lhs.asDouble() + rhs.asDouble());
        
        throw std::runtime_error("Type Error: Cannot add '" + lhs.typeName() + "' and '" + rhs.typeName() + "'.");
    }

    inline Value operator-(const Value& lhs, const Value& rhs) {
        bool lIsInt = lhs.isInt32() || lhs.isBool();
        bool rIsInt = rhs.isInt32() || rhs.isBool();
        if (lIsInt && rIsInt) {
            int32_t a = lhs.isInt32() ? lhs.asInt32() : (lhs.asBool() ? 1 : 0);
            int32_t b = rhs.isInt32() ? rhs.asInt32() : (rhs.asBool() ? 1 : 0);
            int64_t diff = static_cast<int64_t>(a) - b;
            if (diff >= -2147483648LL && diff <= 2147483647LL) return Value::fromInt32(static_cast<int32_t>(diff));
            return Value(BigInt(diff));
        }
        if (lhs.isNumber() && rhs.isNumber()) return Value(lhs.asNumber() - rhs.asNumber());
        
        if (lhs.isObjType(ObjType::DICT)) {
            if (rhs.isObjType(ObjType::SET) || rhs.isObjType(ObjType::LIST) || rhs.isObjType(ObjType::DICT)) {
                ObjDict* d1 = static_cast<ObjDict*>(lhs.asObj());
                ObjDict* res = GcHeap::get().allocate<ObjDict>();
                
                if (rhs.isObjType(ObjType::SET)) {
                    ObjSet* s2 = static_cast<ObjSet*>(rhs.asObj());
                    for (const auto& [k, v] : d1->elements) {
                        if (s2->keys.find(k) == s2->keys.end()) res->set(k, v);
                    }
                } else if (rhs.isObjType(ObjType::LIST)) {
                    ObjList* l2 = static_cast<ObjList*>(rhs.asObj());
                    std::unordered_set<Value, ValueHasher, ValueEqual> toRemove;
                    for (const auto& k : l2->vec) toRemove.insert(k);
                    for (const auto& [k, v] : d1->elements) {
                        if (toRemove.find(k) == toRemove.end()) res->set(k, v);
                    }
                } else if (rhs.isObjType(ObjType::DICT)) {
                    ObjDict* d2 = static_cast<ObjDict*>(rhs.asObj());
                    for (const auto& [k, v] : d1->elements) {
                        if (d2->keyMap.find(k) == d2->keyMap.end()) res->set(k, v);
                    }
                }
                return Value(res);
            }
            throw std::runtime_error("Type Error: Dict subtraction requires a Set, List, or Dict on the right side.");
        }

        if (lhs.isObjType(ObjType::REAL_MATRIX) && rhs.isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(lhs.asObj())->mat - static_cast<ObjRealMatrix*>(rhs.asObj())->mat);
            if (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhs.isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(lhs.asObj())->mat - static_cast<ObjComplexMatrix*>(rhs.asObj())->mat);
            if (lhs.isObjType(ObjType::REAL_MATRIX) && rhs.isObjType(ObjType::COMPLEX_MATRIX)) return Value(lhs.asComplexMatrix() - rhs.asComplexMatrix());
            if (lhs.isObjType(ObjType::COMPLEX_MATRIX) && rhs.isObjType(ObjType::REAL_MATRIX)) return Value(lhs.asComplexMatrix() - rhs.asComplexMatrix());
            
            bool lhsIsMat = lhs.isObjType(ObjType::REAL_MATRIX) || lhs.isObjType(ObjType::COMPLEX_MATRIX);
            bool rhsIsMat = rhs.isObjType(ObjType::REAL_MATRIX) || rhs.isObjType(ObjType::COMPLEX_MATRIX);
            bool lhsIsScalar = lhs.isNumber() || lhs.isBigInt() || lhs.isObjType(ObjType::FRACTION) || lhs.isComplex();
            bool rhsIsScalar = rhs.isNumber() || rhs.isBigInt() || rhs.isObjType(ObjType::FRACTION) || rhs.isComplex();

            if (lhsIsMat && rhsIsScalar) {
                if (lhs.isObjType(ObjType::REAL_MATRIX) && !rhs.isComplex()) {
                    RealMatrix m = static_cast<ObjRealMatrix*>(lhs.asObj())->mat;
                    if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar subtraction requires a square matrix.");
                    double c = rhs.asDouble();
                    for (int i = 0; i < m.getRows(); ++i) m(i, i) -= c;
                    return Value(m);
                }
                ComplexMatrix m = lhs.asComplexMatrix();
                if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar subtraction requires a square matrix.");
                Complex c = rhs.asComplex();
                for (int i = 0; i < m.getRows(); ++i) m(i, i) = m(i, i) - c;
                return Value(m);
            }
            if (lhsIsScalar && rhsIsMat) {
                if (rhs.isObjType(ObjType::REAL_MATRIX) && !lhs.isComplex()) {
                    RealMatrix m = static_cast<ObjRealMatrix*>(rhs.asObj())->mat;
                    if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar subtraction requires a square matrix.");
                    double c = lhs.asDouble();
                    RealMatrix res(m.getRows(), m.getCols());
                    for (int i = 0; i < m.getRows(); ++i) {
                        for (int j = 0; j < m.getCols(); ++j) {
                            res(i, j) = (i == j ? c : 0.0) - m(i, j);
                        }
                    }
                    return Value(res);
                }
                ComplexMatrix m = rhs.asComplexMatrix();
                if (m.getRows() != m.getCols()) throw std::runtime_error("Math Error: Matrix-scalar subtraction requires a square matrix.");
                Complex c = lhs.asComplex();
                ComplexMatrix res(m.getRows(), m.getCols());
                for (int i = 0; i < m.getRows(); ++i) {
                    for (int j = 0; j < m.getCols(); ++j) {
                        res(i, j) = (i == j ? c : Complex(0.0, 0.0)) - m(i, j);
                    }
                }
                return Value(res);
            }
            
            if (lhs.isObjType(ObjType::SET) && rhs.isObjType(ObjType::SET)) {
                ObjSet* s1 = static_cast<ObjSet*>(lhs.asObj());
                ObjSet* s2 = static_cast<ObjSet*>(rhs.asObj());
                ObjSet* res = GcHeap::get().allocate<ObjSet>();
                for (const auto& val : s1->elements) {
                    if (s2->keys.find(val) == s2->keys.end()) {
                        res->add(val);
                    }
                }
                return Value(res);
            }

            if (lhs.isSymbolic() || rhs.isSymbolic()) return Value(lhs.asSymbolic() - rhs.asSymbolic());

            if (lhs.isObjType(ObjType::BASENUM) && rhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base - static_cast<ObjBaseNum*>(rhs.asObj())->base);
            if (lhs.isObjType(ObjType::BASENUM)) return Value(static_cast<ObjBaseNum*>(lhs.asObj())->base - BaseNum(rhs.asBigInt(), static_cast<ObjBaseNum*>(lhs.asObj())->base.getRadix()));
            if (rhs.isObjType(ObjType::BASENUM)) return Value(BaseNum(lhs.asBigInt(), static_cast<ObjBaseNum*>(rhs.asObj())->base.getRadix()) - static_cast<ObjBaseNum*>(rhs.asObj())->base);

            if (lhs.isObjType(ObjType::COMPLEX) || rhs.isObjType(ObjType::COMPLEX)) return Value(lhs.asComplex() - rhs.asComplex());
            
            bool lhsIsExactInt = lhs.isBigInt() || lhs.isInt32();
            bool rhsIsExactInt = rhs.isBigInt() || rhs.isInt32();
            if (lhsIsExactInt && rhsIsExactInt) return Value(lhs.asBigInt() - rhs.asBigInt());
            if (lhs.isObjType(ObjType::FRACTION) && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac - static_cast<ObjFraction*>(rhs.asObj())->frac);
            if (lhs.isObjType(ObjType::FRACTION) && rhsIsExactInt) return Value::fromFraction(static_cast<ObjFraction*>(lhs.asObj())->frac - Fraction(rhs.asBigInt()));
            if (lhsIsExactInt && rhs.isObjType(ObjType::FRACTION)) return Value::fromFraction(Fraction(lhs.asBigInt()) - static_cast<ObjFraction*>(rhs.asObj())->frac);
            
            if (lhs.isDouble() || rhs.isDouble()) return Value(lhs.asDouble() - rhs.asDouble());
        
        throw std::runtime_error("Type Error: Cannot subtract '" + rhs.typeName() + "' from '" + lhs.typeName() + "'.");
    }

inline ObjClosure* Value::asFunction() const {
    if (isObjType(ObjType::CLOSURE)) return static_cast<ObjClosure*>(asObj());
    throw std::runtime_error("Type Error: Expected a function.");
}

inline std::ostream& operator<<(std::ostream& os, const Value& val) {
    static thread_local std::vector<const void*> visited;
    auto printNested = [&os](const Value& v) {
        if (v.isNone()) os << "none";
        else if (v.isUninit()) os << "<uninit>";
        else os << v.toRepr();
    };

    if (val.isNone()) { os << "none"; return os; }
    if (val.isUninit()) { os << "<uninit>"; return os; }
    if (val.isBool()) { os << (val.asBool() ? "true" : "false"); return os; }
    if (val.isInt32()) { os << val.asInt32(); return os; }
    if (val.isDouble()) {
        double v = val.asDoubleRaw();
        std::ostringstream temp;
        temp << v;
        std::string s = temp.str();
        if (std::isfinite(v) && s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
            s += ".0";
        }
        os << s;
        return os;
    }

    Obj* obj = val.asObj();
    switch (obj->type) {
        case ObjType::STRING: os << static_cast<ObjString*>(obj)->str; break;
        case ObjType::BIGINT: os << static_cast<ObjBigInt*>(obj)->num; break;
        case ObjType::FRACTION: os << static_cast<ObjFraction*>(obj)->frac; break;
        case ObjType::COMPLEX: os << static_cast<ObjComplex*>(obj)->comp; break;
        case ObjType::BASENUM: os << static_cast<ObjBaseNum*>(obj)->base; break;
        case ObjType::REAL_MATRIX: {
            const auto& m = static_cast<ObjRealMatrix*>(obj)->mat;
            if (m.getRows() * m.getCols() == 0) os << "[]";
            else os << m;
            break;
        }
        case ObjType::COMPLEX_MATRIX: {
            const auto& m = static_cast<ObjComplexMatrix*>(obj)->mat;
            if (m.getRows() * m.getCols() == 0) os << "[]";
            else os << m;
            break;
        }
        case ObjType::STRING_MATRIX: {
            const auto& m = static_cast<ObjStringMatrix*>(obj)->mat;
            if (m.getRows() * m.getCols() == 0) os << "[]";
            else os << m;
            break;
        }
        case ObjType::SYMBOLIC: os << static_cast<ObjSym*>(obj)->sym.toString(); break;
        case ObjType::CLOSURE: os << static_cast<ObjClosure*>(obj)->toString(); break;
        case ObjType::CLASS: {
            std::string n = static_cast<ObjClass*>(obj)->name;
            if (n.empty()) os << "<anonymous class>";
            else os << "<class " << n << ">";
            break;
        }
        case ObjType::SUPER_PROXY: os << "<super>"; break;
        case ObjType::NAMESPACE: {
            std::string n = static_cast<ObjNamespace*>(obj)->name;
            if (n.empty()) os << "<anonymous namespace>";
            else os << "<namespace " << n << ">";
            break;
        }
        case ObjType::UPVALUE: os << "<upvalue>"; break;
        case ObjType::TYPE_DEF: os << "<type '" << static_cast<ObjTypeDef*>(obj)->name() << "'>"; break;
        case ObjType::LIST: {
            ObjList* list = static_cast<ObjList*>(obj);
            RecursionGuard guard(visited, list);
            if (guard.isCycle) { os << "@[...]"; break; }
            os << "@[";
            for (size_t i = 0; i < list->vec.size(); ++i) {
                try { printNested(list->vec[i]); } catch (...) { os << "?"; }
                if (i < list->vec.size() - 1) os << ", ";
            }
            os << "]";
            break;
        }
        case ObjType::DICT: {
            ObjDict* dict = static_cast<ObjDict*>(obj);
            RecursionGuard guard(visited, dict);
            if (guard.isCycle) { os << "{...}"; break; }
            os << "{";
            for (size_t i = 0; i < dict->elements.size(); ++i) {
                try { printNested(dict->elements[i].first); } catch (...) { os << "?"; }
                os << ": ";
                try { printNested(dict->elements[i].second); } catch (...) { os << "?"; }
                if (i < dict->elements.size() - 1) os << ", ";
            }
            os << "}";
            break;
        }
        case ObjType::SET: {
            ObjSet* set = static_cast<ObjSet*>(obj);
            RecursionGuard guard(visited, set);
            if (guard.isCycle) { os << "@{...}"; break; }
            os << "@{";
            for (size_t i = 0; i < set->elements.size(); ++i) {
                try { printNested(set->elements[i]); } catch (...) { os << "?"; }
                if (i < set->elements.size() - 1) os << ", ";
            }
            os << "}";
            break;
        }
        case ObjType::INSTANCE: {
            ObjInstance* inst = static_cast<ObjInstance*>(obj);
            RecursionGuard guard(visited, inst);
            std::string cname = inst->classDef ? inst->classDef->name : "unknown";
            std::string prefix = cname.empty() ? "anonymous class" : cname;
            if (guard.isCycle) { os << "<" << prefix << " {...}>"; break; }
                
            try {
                auto [foundRepl, replRes] = invokeDunder(inst, "__repr__");
                if (foundRepl) {
                    if (replRes.isString()) os << replRes.asString();
                    else os << replRes;
                    break;
                }
                auto [foundStr, strRes] = invokeDunder(inst, "__str__");
                if (foundStr) {
                    if (strRes.isString()) os << strRes.asString();
                    else os << strRes;
                    break;
                }
            } catch (...) {}

            bool printedNative = false;
            if (inst->nativeData.has_value()) {
                if (inst->nativeData.type() == typeid(std::shared_ptr<Image>)) {
                    auto& img = std::any_cast<std::shared_ptr<Image>&>(inst->nativeData);
                    os << "<Image " << img->width() << "x" << img->height() << ">";
                    printedNative = true;
                } else if (inst->nativeData.type() == typeid(std::shared_ptr<Distribution>)) {
                    auto& dist = std::any_cast<std::shared_ptr<Distribution>&>(inst->nativeData);
                    os << dist->toString();
                    printedNative = true;
                }
            }
            if (!printedNative) {
                os << "<" << prefix << " {";
                bool first = true;
                std::map<std::string, PropertyDescriptor> sorted_props(inst->properties.begin(), inst->properties.end());
                for (const auto& [k, prop] : sorted_props) {
                    if (!first) os << ", ";
                    if (prop.is_local) os << "local ";
                    if (prop.is_const) os << "const ";
                    
                    // Demangle private names for display
                    std::string dispName = k;
                    size_t pos = dispName.find("::");
                    if (pos != std::string::npos) dispName = dispName.substr(pos + 2);
                    
                    os << dispName << ": ";
                    try { printNested(prop.val); } catch (...) { os << "?"; }
                    first = false;
                }
                os << "}>";
            }
            break;
        }
    }
    return os;
}

inline size_t ValueHasher::operator()(const Value& v) const {
    // 统一将 int32, double, bool 转换为 double 进行哈希，确保 1 == 1.0 == true 时哈希值绝对一致
    if (v.isInt32()) return sipHash24Double(static_cast<double>(v.asInt32()));
    if (v.isDouble()) {
        double d = v.asDoubleRaw();
        if (d == 0.0) d = 0.0; // 归一化 -0.0
        return sipHash24Double(d);
    }
    if (v.isBool()) return sipHash24Double(v.asBool() ? 1.0 : 0.0);
    if (v.isNone()) return 0xA174E2C8B5D3F609ULL;
    if (v.isUninit()) return 0x7E5109F8C4A2D637ULL;
    
    Obj* obj = v.asObj();
    switch (obj->type) {
        case ObjType::STRING: return static_cast<ObjString*>(obj)->hash; // ★ O(1) 哈希
        case ObjType::BIGINT: {
            auto objBigInt = static_cast<ObjBigInt*>(obj);
            if (objBigInt->has_cached_hash) return objBigInt->cached_hash;
            const BigInt& bi = objBigInt->num;
            size_t h = 0;
            try {
                int64_t i64 = bi.toInt64();
                if (i64 >= -9007199254740992LL && i64 <= 9007199254740992LL) {
                    double d = static_cast<double>(i64);
                    if (d == 0.0) d = 0.0;
                    h = sipHash24Double(d);
                    objBigInt->cached_hash = h;
                    objBigInt->has_cached_hash = true;
                    return h;
                }
            } catch (...) {}
            const auto& raw = bi.getRawData();
            h = sipHash24(raw.data(), raw.size() * sizeof(uint32_t));
            if (bi.getSign()) h ^= 0xAAAAAAAAAAAAAAAAULL;
            h ^= 0xCF1A5E7B3D9204F6ULL;
            objBigInt->cached_hash = h;
            objBigInt->has_cached_hash = true;
            return h;
        }
        case ObjType::FRACTION: {
            auto objFrac = static_cast<ObjFraction*>(obj);
            if (objFrac->has_cached_hash) return objFrac->cached_hash;
            const Fraction& fr = objFrac->frac;
            size_t h = 0;
            if (fr.getDenRef() == BigInt(1)) {
                h = ValueHasher{}(Value(fr.getNumRef()));
                objFrac->cached_hash = h;
                objFrac->has_cached_hash = true;
                return h;
            }
            try {
                double d = fr.toDouble();
                if (std::isfinite(d)) {
                    if (Fraction::fromDouble(d == 0.0 ? 0.0 : d) == fr) {
                        if (d == 0.0) d = 0.0;
                        h = sipHash24Double(d);
                        objFrac->cached_hash = h;
                        objFrac->has_cached_hash = true;
                        return h;
                    }
                }
            } catch (...) {}
            const auto& numRaw = fr.getNumRef().getRawData();
            const auto& denRaw = fr.getDenRef().getRawData();
            size_t h1 = sipHash24(numRaw.data(), numRaw.size() * sizeof(uint32_t));
            if (fr.getNumRef().getSign()) h1 ^= 0xAAAAAAAAAAAAAAAAULL;
            size_t h2 = sipHash24(denRaw.data(), denRaw.size() * sizeof(uint32_t));
            if (fr.getDenRef().getSign()) h2 ^= 0xAAAAAAAAAAAAAAAAULL;
            h = h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2)) ^ 0xA38F6D2E1B7C54E0ULL;
            objFrac->cached_hash = h;
            objFrac->has_cached_hash = true;
            return h;
        }
        case ObjType::COMPLEX: {
            auto c = static_cast<ObjComplex*>(obj)->comp;
            double r = c.real; if (r == 0.0) r = 0.0;
            if (c.imag == 0.0) return sipHash24Double(r);
            double i = c.imag; if (i == 0.0) i = 0.0;
            return sipHash24Double(r) ^ (sipHash24Double(i) << 1);
        }
        case ObjType::BASENUM: {
            return ValueHasher{}(Value(static_cast<ObjBaseNum*>(obj)->base.getValue()));
        }
        case ObjType::REAL_MATRIX: {
            const auto& m = static_cast<ObjRealMatrix*>(obj)->mat;
            const auto& raw = m.rawData();
            size_t sz = raw.size();
            size_t seed = sipHash24(&sz, sizeof(size_t)) ^ 0x2E6A8F1D4C3B7950ULL;
            for (size_t idx = 0; idx < raw.size(); ++idx) {
                double d = raw[idx]; if (d == 0.0) d = 0.0;
                size_t h = sipHash24Double(d) + 0x9e3779b9 + idx;
                seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
        case ObjType::COMPLEX_MATRIX: {
            const auto& m = static_cast<ObjComplexMatrix*>(obj)->mat;
            const auto& raw = m.rawData();
            size_t sz = raw.size();
            size_t seed = sipHash24(&sz, sizeof(size_t)) ^ 0x4D7A1F8E2B5C6039ULL;
            for (size_t idx = 0; idx < raw.size(); ++idx) {
                double r = raw[idx].real; if (r == 0.0) r = 0.0;
                double i = raw[idx].imag; if (i == 0.0) i = 0.0;
                size_t h = (i == 0.0) ? sipHash24Double(r) : (sipHash24Double(r) ^ (sipHash24Double(i) << 1));
                h += 0x9e3779b9 + idx;
                seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
        case ObjType::STRING_MATRIX: {
            const auto& m = static_cast<ObjStringMatrix*>(obj)->mat;
            const auto& raw = m.rawData();
            size_t sz = raw.size();
            size_t seed = sipHash24(&sz, sizeof(size_t)) ^ 0x8B3E5A2F1D7C4068ULL;
            for (size_t idx = 0; idx < raw.size(); ++idx) {
                size_t h = sipHash24String(raw[idx]) + 0x9e3779b9 + idx;
                seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
        case ObjType::SYMBOLIC: return sipHash24String(static_cast<ObjSym*>(obj)->sym.toString());
        case ObjType::TYPE_DEF: return sipHash24String(static_cast<ObjTypeDef*>(obj)->name());
        case ObjType::LIST: {
            auto l = static_cast<ObjList*>(obj);
            if (!l->is_frozen) throw std::runtime_error("TypeError: unhashable type.");
            if (l->has_cached_hash) return l->cached_hash;
            size_t sz = l->vec.size();
            size_t seed = sipHash24(&sz, sizeof(size_t)) ^ 0x6F2D8A4E1C5B7093ULL;
            for (size_t idx = 0; idx < l->vec.size(); ++idx) {
                size_t h = ValueHasher{}(l->vec[idx]) + 0x9e3779b9 + idx;
                seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            l->cached_hash = seed; l->has_cached_hash = true;
            return seed;
        }
        case ObjType::DICT: {
            auto d = static_cast<ObjDict*>(obj);
            if (!d->is_frozen) throw std::runtime_error("TypeError: unhashable type.");
            if (d->has_cached_hash) return d->cached_hash;
            size_t sz = d->elements.size();
            size_t seed = sipHash24(&sz, sizeof(size_t)) ^ 0x3B8E7A1F5D2C6049ULL;
            for (const auto& [k, val] : d->elements) {
                size_t k_hash = ValueHasher{}(k);
                size_t v_hash = ValueHasher{}(val);
                size_t kv_hash = k_hash ^ (v_hash + 0x9e3779b9 + (k_hash << 6) + (k_hash >> 2));
                kv_hash *= 0xBF58476D1CE4E5B9ULL;
                kv_hash ^= kv_hash >> 31;
                kv_hash *= 0x94D049BB133111EBULL;
                seed += kv_hash;
            }
            d->cached_hash = seed; d->has_cached_hash = true;
            return seed;
        }
        case ObjType::SET: {
            auto s = static_cast<ObjSet*>(obj);
            if (!s->is_frozen) throw std::runtime_error("TypeError: unhashable type.");
            if (s->has_cached_hash) return s->cached_hash;
            size_t sz = s->elements.size();
            size_t seed = sipHash24(&sz, sizeof(size_t)) ^ 0x5C9D2E4F1A8B7063ULL;
            for (const auto& e : s->elements) {
                size_t h = ValueHasher{}(e);
                h *= 0xBF58476D1CE4E5B9ULL;
                h ^= h >> 31;
                h *= 0x94D049BB133111EBULL;
                seed += h;
            }
            s->cached_hash = seed; s->has_cached_hash = true;
            return seed;
        }
        case ObjType::SUPER_PROXY: {
            auto sp = static_cast<ObjSuper*>(obj);
            size_t h1 = sipHash24(&sp->instance, sizeof(void*));
            size_t h2 = sipHash24(&sp->parentClass, sizeof(void*));
            return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
        case ObjType::INSTANCE: {
            auto inst = static_cast<ObjInstance*>(obj);
            auto [found, res] = invokeDunder(inst, "__hash__");
            if (found) {
                if (res.isNumber()) return sipHash24Double(res.asDouble());
                if (res.isString()) return sipHash24String(res.asString());
                if (res.isBigInt()) return sipHash24String(res.asBigInt().toString());
            }
            if (inst->is_frozen) {
                if (inst->has_cached_hash) return inst->cached_hash;
                std::string cname = inst->classDef ? inst->classDef->name : "";
                size_t seed = sipHash24String(cname);
                size_t sz = inst->properties.size();
                size_t fields_hash = sipHash24(&sz, sizeof(size_t)) ^ 0xE7B2A4D8F1C56039ULL;
                for (const auto& [k, prop] : inst->properties) {
                    size_t k_hash = sipHash24String(k);
                    size_t v_hash = ValueHasher{}(prop.val);
                    size_t kv_hash = k_hash ^ (v_hash + 0x9e3779b9 + (k_hash << 6) + (k_hash >> 2));
                    kv_hash *= 0xBF58476D1CE4E5B9ULL;
                    kv_hash ^= kv_hash >> 31;
                    kv_hash *= 0x94D049BB133111EBULL;
                    fields_hash += kv_hash;
                }
                seed ^= fields_hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                inst->cached_hash = seed;
                inst->has_cached_hash = true;
                return seed;
            }
            throw std::runtime_error("TypeError: unhashable type.");
        }
        default: return sipHash24(&obj, sizeof(void*));
    }
}

inline bool ValueEqual::operator()(const Value& lhs, const Value& rhs) const {
    return Value::equals(lhs, rhs);
}

inline void GcHeap::markObj(Obj* obj) {
    if (!obj || obj->isMarked) return;
    obj->isMarked = true;
    
    switch (obj->type) {
        case ObjType::LIST: {
            for (auto& v : static_cast<ObjList*>(obj)->vec) markValue(v);
            break;
        }
        case ObjType::DICT: {
            for (auto& [k, v] : static_cast<ObjDict*>(obj)->elements) {
                markValue(k); markValue(v);
            }
            break;
        }
        case ObjType::SET: {
            for (auto& v : static_cast<ObjSet*>(obj)->elements) markValue(v);
            break;
        }
        case ObjType::CLOSURE: {
            auto closure = static_cast<ObjClosure*>(obj);
            markValue(closure->boundSelf);
            markValue(closure->boundClass);
            markObj(closure->owner_class);
            for (auto& v : closure->defaultValues) markValue(v);
            for (int i = 0; i < closure->upvalueCount; ++i) {
                markObj(closure->upvalues[i]);
            }
            if (closure->paramTypes) {
                for (int i = 0; i < closure->paramTypesCount; ++i) {
                    markValue(closure->paramTypes[i]);
                }
            }
            markValue(closure->returnType);
            break;
        }
        case ObjType::CLASS: {
            auto cls = static_cast<ObjClass*>(obj);
            markObj(cls->parent);
            for (auto& [k, prop] : cls->properties) markValue(prop.val);
            break;
        }
        case ObjType::INSTANCE: {
            auto inst = static_cast<ObjInstance*>(obj);
            markObj(inst->classDef);
            for (auto& [k, prop] : inst->properties) markValue(prop.val);
            break;
        }
        case ObjType::SUPER_PROXY: {
            auto sp = static_cast<ObjSuper*>(obj);
            markObj(sp->instance);
            markObj(sp->parentClass);
            break;
        }
        case ObjType::NAMESPACE: {
            auto ns = static_cast<ObjNamespace*>(obj);
            for (auto& [k, f] : ns->fields) markObj(f.upval);
            break;
        }
        case ObjType::UPVALUE: {
            auto uv = static_cast<ObjUpVal*>(obj);
            markValue(uv->closed);
            break;
        }
        case ObjType::TYPE_DEF: {
            auto td = static_cast<ObjTypeDef*>(obj);
            for (auto& t : td->types) {
                if (std::holds_alternative<ObjClass*>(t)) {
                    markObj(std::get<ObjClass*>(t));
                }
            }
            break;
        }
        default: break;
    }
}

inline void GcHeap::markValue(const Value& val) {
    if (val.isObj()) markObj(val.asObj());
}

inline int GcHeap::collectGarbage() {
    if (gc_locked_) return 0;
    gc_locked_ = true;

    if (markCallback) markCallback();
        
    for (Obj* obj : tempObjRoots_) markObj(obj);
    for (Value* val : tempValueRoots_) {
        if (val) markValue(*val);
    }

    std::vector<Obj*> finalizerQueue;
    if (hasFinalizerCallback) {
        Obj* curr = objects_;
        while (curr) {
            if (!curr->isMarked && curr->type == ObjType::INSTANCE) {
                if (hasFinalizerCallback(curr)) {
                    finalizerQueue.push_back(curr);
                }
            }
            curr = curr->next;
        }
        for (Obj* obj : finalizerQueue) {
            markObj(obj);
        }
    }

    if (sweepCallback) sweepCallback();

    int freed = sweep();

    if (!finalizerQueue.empty() && executeFinalizerCallback) {
        for (Obj* obj : finalizerQueue) {
            executeFinalizerCallback(obj);
        }
    }

    gc_locked_ = false;
    return freed;
}

inline ObjString* internString(const std::string& str) {
    auto it = g_internedStrings.find(str);
    if (it != g_internedStrings.end()) {
        return it->second;
    }
    ObjString* obj = GcHeap::get().allocate<ObjString>(str);
    g_internedStrings[str] = obj;
    return obj;
}

} // namespace jc

#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif // JC2_VALUE_H
