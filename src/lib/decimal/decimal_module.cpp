#include "../jc2_extension_cpp.h"
#include "../../math/BigInt.h"
#include <string>
#include <memory>
#include <algorithm>

static int g_prec = 28;
static jc2::Class* g_decimalClass = nullptr;

class Decimal {
public:
    jc::BigInt mantissa;
    int64_t exp;

    Decimal(jc::BigInt m, int64_t e) : mantissa(std::move(m)), exp(e) {}

    static Decimal from_string(const std::string& s) {
        std::string m_str = "";
        int64_t e = 0;
        bool in_frac = false;
        int64_t frac_count = 0;
        
        size_t i = 0;
        if (i < s.length() && (s[i] == '+' || s[i] == '-')) {
            m_str += s[i++];
        }
        
        for (; i < s.length(); ++i) {
            if (s[i] == '.') {
                in_frac = true;
            } else if (s[i] >= '0' && s[i] <= '9') {
                m_str += s[i];
                if (in_frac) frac_count++;
            } else if (s[i] == 'e' || s[i] == 'E') {
                e = std::stoll(s.substr(i + 1));
                break;
            }
        }
        if (m_str.empty() || m_str == "+" || m_str == "-") m_str += "0";
        return Decimal(jc::BigInt(m_str), e - frac_count);
    }

    std::string to_string() const {
        std::string m = mantissa.toString();
        bool neg = false;
        if (m.length() > 0 && m[0] == '-') {
            neg = true;
            m = m.substr(1);
        }
        if (m == "0") return "0";
        
        std::string res;
        if (exp >= 0) {
            res = m + std::string(static_cast<size_t>(exp), '0');
        } else {
            int64_t pos_exp = -exp;
            if (pos_exp >= static_cast<int64_t>(m.length())) {
                res = "0." + std::string(static_cast<size_t>(pos_exp - m.length()), '0') + m;
            } else {
                res = m.substr(0, m.length() - pos_exp) + "." + m.substr(m.length() - pos_exp);
            }
        }
        
        if (res.find('.') != std::string::npos) {
            while (!res.empty() && res.back() == '0') res.pop_back();
            if (!res.empty() && res.back() == '.') res.pop_back();
        }
        
        if (neg && res != "0") res = "-" + res;
        return res;
    }

    static jc::BigInt pow10(int64_t n) {
        if (n <= 0) return jc::BigInt("1");
        return jc::BigInt("1" + std::string(static_cast<size_t>(n), '0'));
    }

    Decimal add(const Decimal& other) const {
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return Decimal(m1 + m2, min_exp);
    }

    Decimal sub(const Decimal& other) const {
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return Decimal(m1 - m2, min_exp);
    }

    Decimal mul(const Decimal& other) const {
        return Decimal(mantissa * other.mantissa, exp + other.exp);
    }

    Decimal div(const Decimal& other) const {
        if (other.mantissa.toString() == "0") {
            jc2::throw_error("DivisionByZero: Decimal division by zero.");
            return *this;
        }
        int64_t extra_zeros = g_prec + 2;
        jc::BigInt m1_shifted = mantissa * pow10(extra_zeros);
        jc::BigInt q = m1_shifted / other.mantissa;
        return Decimal(q, exp - other.exp - extra_zeros);
    }
    
    bool eq(const Decimal& other) const {
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return m1 == m2;
    }
    
    bool lt(const Decimal& other) const {
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return m1 < m2;
    }
};

static std::shared_ptr<Decimal> getDecimal(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("TypeError: Expected a Decimal instance.");
    auto ptr = val.get_native_data<std::shared_ptr<Decimal>>();
    if (!ptr) jc2::throw_error("TypeError: Instance is not a Decimal.");
    return *ptr;
}

static jc2::Value wrapDecimal(const Decimal& d) {
    jc2::Instance inst(*g_decimalClass);
    auto data = new std::shared_ptr<Decimal>(std::make_shared<Decimal>(d));
    inst.set_native_data(data, [](void* ptr) {
        delete static_cast<std::shared_ptr<Decimal>*>(ptr);
    });
    return inst;
}

static Decimal parseDecimalArg(const jc2::Value& val) {
    if (val.is_instance() && val.get_native_data<std::shared_ptr<Decimal>>()) {
        return *(*val.get_native_data<std::shared_ptr<Decimal>>());
    }
    if (val.is_string()) {
        return Decimal::from_string(val.as_string());
    }
    if (val.is_int() || val.is_double()) {
        return Decimal::from_string(val.to_string());
    }
    jc2::throw_error("TypeError: Cannot convert to Decimal.");
    return Decimal::from_string("0");
}

#define METHOD(name) JC2_ValueHandle decimal_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)
#define GET_SELF auto d1 = getDecimal(jc2::Value(argv[0]))

METHOD(__str__) { GET_SELF; return jc2::Value(d1->to_string()).get_handle(); }
METHOD(__add__) { GET_SELF; return wrapDecimal(d1->add(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__radd__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).add(*d1)).get_handle(); }
METHOD(__sub__) { GET_SELF; return wrapDecimal(d1->sub(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rsub__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).sub(*d1)).get_handle(); }
METHOD(__mul__) { GET_SELF; return wrapDecimal(d1->mul(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rmul__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).mul(*d1)).get_handle(); }
METHOD(__div__) { GET_SELF; return wrapDecimal(d1->div(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rdiv__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).div(*d1)).get_handle(); }
METHOD(__eq__) { GET_SELF; return jc2::Value(d1->eq(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__lt__) { GET_SELF; return jc2::Value(d1->lt(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__neg__) { GET_SELF; return wrapDecimal(Decimal::from_string("0").sub(*d1)).get_handle(); }
METHOD(__bool__) { GET_SELF; return jc2::Value(!d1->eq(Decimal::from_string("0"))).get_handle(); }

JC2_ValueHandle global_Decimal(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    return wrapDecimal(parseDecimalArg(jc2::Value(argv[0]))).get_handle();
}

JC2_ValueHandle global_getcontext(JC2_VMContext, int, JC2_ValueHandle*, void*) {
    jc2::Dict ctx;
    ctx.set(jc2::Value("prec"), jc2::Value(g_prec));
    return ctx.get_handle();
}

JC2_ValueHandle global_setcontext(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc > 0) {
        g_prec = static_cast<int>(jc2::Value(argv[0]).as_double());
    }
    return jc2::Value().get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_decimalClass = new jc2::Class("Decimal");
    mod.register_value("Decimal", *g_decimalClass);

    g_decimalClass->bind_method("__str__", decimal___str__, 0, 0, false);
    g_decimalClass->bind_method("__add__", decimal___add__, 1, 1, false);
    g_decimalClass->bind_method("__radd__", decimal___radd__, 1, 1, false);
    g_decimalClass->bind_method("__sub__", decimal___sub__, 1, 1, false);
    g_decimalClass->bind_method("__rsub__", decimal___rsub__, 1, 1, false);
    g_decimalClass->bind_method("__mul__", decimal___mul__, 1, 1, false);
    g_decimalClass->bind_method("__rmul__", decimal___rmul__, 1, 1, false);
    g_decimalClass->bind_method("__div__", decimal___div__, 1, 1, false);
    g_decimalClass->bind_method("__rdiv__", decimal___rdiv__, 1, 1, false);
    g_decimalClass->bind_method("__eq__", decimal___eq__, 1, 1, false);
    g_decimalClass->bind_method("__lt__", decimal___lt__, 1, 1, false);
    g_decimalClass->bind_method("__neg__", decimal___neg__, 0, 0, false);
    g_decimalClass->bind_method("__bool__", decimal___bool__, 0, 0, false);

    mod.register_function("Decimal", global_Decimal, 1, 1, false);
    mod.register_function("getcontext", global_getcontext, 0, 0, false);
    mod.register_function("setcontext", global_setcontext, 1, 1, false);

    mod.register_help("decimal",
        "═══ Arbitrary-Precision Decimal Arithmetic — Native Module ═══\n\n"
        "  Requires: import decimal\n\n"
        "  The `decimal` module provides a Decimal data type for fast correctly-rounded\n"
        "  decimal floating point arithmetic. It avoids the precision issues of standard\n"
        "  binary floating point (double).\n\n"
        "  Construction\n"
        "  ──────────────────────\n"
        "    decimal.Decimal(\"0.1\")        Create from string (Recommended)\n"
        "    decimal.Decimal(42)           Create from integer\n"
        "    decimal.Decimal(3.14)         Create from double\n\n"
        "  Context Management\n"
        "  ──────────────────────\n"
        "    decimal.getcontext()          Returns a dict with current context settings\n"
        "    decimal.setcontext(prec)      Sets the global precision (number of digits)\n\n"
        "  Example\n"
        "  ──────────────────────\n"
        "    import decimal\n"
        "    decimal.setcontext(50)\n"
        "    a = decimal.Decimal(\"0.1\")\n"
        "    b = decimal.Decimal(\"0.2\")\n"
        "    print(a + b)                  // → 0.3 (Exact!)\n"
    );

    mod.register_function_help("decimal.Decimal", "decimal.Decimal(value)", "Creates a new Decimal object from a string, integer, or double.", "d = decimal.Decimal(\"123.45\")");
    mod.register_function_help("decimal.getcontext", "decimal.getcontext()", "Returns the current decimal context (precision).", "ctx = decimal.getcontext()");
    mod.register_function_help("decimal.setcontext", "decimal.setcontext(prec)", "Sets the global precision for decimal division.", "decimal.setcontext(50)");

    return 0;
}

JC2_EXTENSION_INIT
