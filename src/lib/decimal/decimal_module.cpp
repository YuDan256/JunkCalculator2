#include "../jc2_extension_cpp.h"
#include "../../math/BigInt.h"
#include <string>
#include <memory>
#include <algorithm>
#include <cmath>

static int g_prec = 28;
static jc2::Class* g_decimalClass = nullptr;

class Decimal {
public:
    jc::BigInt mantissa;
    int64_t exp;

    Decimal() : mantissa("0"), exp(0) {}
    Decimal(jc::BigInt m, int64_t e) : mantissa(std::move(m)), exp(e) {}

    static Decimal from_string(const std::string& s) {
        std::string m_str = "";
        int64_t e = 0;
        bool in_frac = false;
        int64_t frac_count = 0;
        
        size_t i = 0;
        while (i < s.length() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        
        if (i < s.length() && (s[i] == '+' || s[i] == '-')) {
            m_str += s[i++];
        }
        
        bool has_digits = false;
        for (; i < s.length(); ++i) {
            if (s[i] == '.') {
                if (in_frac) jc2::throw_error("ValueError: Invalid decimal string (multiple decimal points).");
                in_frac = true;
            } else if (s[i] >= '0' && s[i] <= '9') {
                m_str += s[i];
                has_digits = true;
                if (in_frac) frac_count++;
            } else if (s[i] == 'e' || s[i] == 'E') {
                try {
                    e = std::stoll(s.substr(i + 1));
                } catch (...) {
                    jc2::throw_error("ValueError: Invalid exponent in decimal string.");
                }
                break;
            } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
                size_t j = i;
                while (j < s.length() && std::isspace(static_cast<unsigned char>(s[j]))) j++;
                if (j == s.length()) break;
                jc2::throw_error("ValueError: Invalid character in decimal string.");
            } else {
                jc2::throw_error("ValueError: Invalid character in decimal string.");
            }
        }
        if (!has_digits) jc2::throw_error("ValueError: No digits found in decimal string.");
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
        if (n <= 0) return jc::BigInt(1);
        if (n > 1000000) jc2::throw_error("MathError: Exponent too large, out of memory risk.");
        return jc::BigInt::getPow10(static_cast<int>(n));
    }

    Decimal truncate(int prec) const {
        if (mantissa.isZero()) return Decimal(jc::BigInt(0), 0);
        int current_digits = mantissa.digitCount();
        if (current_digits <= prec) return *this;
        int drop = current_digits - prec;
        jc::BigInt new_m = mantissa / pow10(drop);
        return Decimal(new_m, exp + drop);
    }

    int64_t magnitude() const {
        if (mantissa.isZero()) return 0;
        return exp + mantissa.digitCount();
    }

    Decimal abs() const {
        return Decimal(mantissa.abs(), exp);
    }

    Decimal add(const Decimal& other) const {
        if (mantissa.isZero()) return other.truncate(g_prec);
        if (other.mantissa.isZero()) return this->truncate(g_prec);
        
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        if (mag1 - mag2 > g_prec + 2) return this->truncate(g_prec);
        if (mag2 - mag1 > g_prec + 2) return other.truncate(g_prec);
        
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return Decimal(m1 + m2, min_exp).truncate(g_prec);
    }

    Decimal sub(const Decimal& other) const {
        if (mantissa.isZero()) {
            return Decimal(-other.mantissa, other.exp).truncate(g_prec);
        }
        if (other.mantissa.isZero()) return this->truncate(g_prec);
        
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        if (mag1 - mag2 > g_prec + 2) return this->truncate(g_prec);
        if (mag2 - mag1 > g_prec + 2) {
            return Decimal(-other.mantissa, other.exp).truncate(g_prec);
        }
        
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return Decimal(m1 - m2, min_exp).truncate(g_prec);
    }

    Decimal mul(const Decimal& other) const {
        return Decimal(mantissa * other.mantissa, exp + other.exp).truncate(g_prec);
    }

    Decimal div(const Decimal& other) const {
        if (other.mantissa.isZero()) {
            jc2::throw_error("DivisionByZero: Decimal division by zero.");
            return *this;
        }
        if (mantissa.isZero()) return Decimal(jc::BigInt(0), 0);
        
        int64_t len1 = mantissa.digitCount();
        int64_t len2 = other.mantissa.digitCount();
        
        int64_t extra_zeros = g_prec + 2 - len1 + len2;
        if (extra_zeros < 0) extra_zeros = 0;
        
        jc::BigInt m1_shifted = mantissa * pow10(extra_zeros);
        jc::BigInt q = m1_shifted / other.mantissa;
        return Decimal(q, exp - other.exp - extra_zeros).truncate(g_prec);
    }
    
    bool eq(const Decimal& other) const {
        if (mantissa.isZero() && other.mantissa.isZero()) return true;
        if (mantissa.isZero() || other.mantissa.isZero()) return false;
        
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        if (std::abs(mag1 - mag2) > g_prec + 2) return false;
        
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return m1 == m2;
    }
    
    bool lt(const Decimal& other) const {
        int64_t mag1 = magnitude();
        int64_t mag2 = other.magnitude();
        
        bool neg1 = mantissa.isNegative();
        bool neg2 = other.mantissa.isNegative();
        if (mantissa.isZero()) neg1 = false;
        if (other.mantissa.isZero()) neg2 = false;
        
        if (neg1 && !neg2) return true;
        if (!neg1 && neg2) return false;
        
        if (std::abs(mag1 - mag2) > g_prec + 2) {
            if (neg1) return mag1 > mag2;
            return mag1 < mag2;
        }
        
        int64_t min_exp = std::min(exp, other.exp);
        jc::BigInt m1 = mantissa;
        if (exp > min_exp) m1 = m1 * pow10(exp - min_exp);
        jc::BigInt m2 = other.mantissa;
        if (other.exp > min_exp) m2 = m2 * pow10(other.exp - min_exp);
        return m1 < m2;
    }

    bool gt(const Decimal& other) const {
        return !this->lt(other) && !this->eq(other);
    }

    bool le(const Decimal& other) const {
        return this->lt(other) || this->eq(other);
    }

    bool ge(const Decimal& other) const {
        return this->gt(other) || this->eq(other);
    }

    bool neq(const Decimal& other) const {
        return !this->eq(other);
    }

    Decimal sqrt() const {
        if (mantissa.isZero()) return *this;
        if (mantissa.isNegative()) {
            jc2::throw_error("MathError: sqrt of negative decimal.");
            return *this;
        }
        Decimal half(jc::BigInt(5), -1);
        
        int64_t total_exp = exp + mantissa.digitCount() - 1;
        int64_t guess_exp = total_exp / 2;
        Decimal x(jc::BigInt(1), guess_exp);

        for (int i = 0; i < 100; ++i) {
            Decimal next_x = half.mul(x.add(this->div(x))).truncate(g_prec + 2);
            if (next_x.eq(x)) break;
            x = next_x;
        }
        return x.truncate(g_prec);
    }

    Decimal exp_val() const {
        Decimal x = *this;
        int squares = 0;
        Decimal two(jc::BigInt(2), 0);
        Decimal one(jc::BigInt(1), 0);
        while (!x.abs().lt(one) && !x.mantissa.isZero()) {
            x = x.div(two).truncate(g_prec + 2);
            squares++;
            if (squares > 30) break;
        }
        
        Decimal sum(jc::BigInt(1), 0);
        Decimal term(jc::BigInt(1), 0);
        Decimal n(jc::BigInt(1), 0);
        
        for (int i = 1; i < 1000; ++i) {
            term = term.mul(x).div(n).truncate(g_prec + 2);
            if (term.mantissa.isZero()) break;
            
            Decimal next_sum = sum.add(term).truncate(g_prec + 2);
            if (next_sum.eq(sum)) break;
            sum = next_sum;
            n = n.add(one);
        }
        
        for (int i = 0; i < squares; ++i) {
            sum = sum.mul(sum).truncate(g_prec + 2);
        }
        
        return sum.truncate(g_prec);
    }

    Decimal mod_2pi() const {
        Decimal two_pi = Decimal::pi().mul(Decimal(jc::BigInt(2), 0));
        Decimal q = this->div(two_pi);
        Decimal q_int;
        if (q.exp >= 0) {
            q_int = q;
        } else if (-q.exp >= q.mantissa.digitCount()) {
            q_int = Decimal(jc::BigInt(0), 0);
        } else {
            q_int = Decimal(q.mantissa / pow10(-q.exp), 0);
        }
        return this->sub(q_int.mul(two_pi));
    }

    Decimal sin_val() const {
        Decimal x = this->mod_2pi();
        Decimal sum = x;
        Decimal term = x;
        Decimal x2 = x.mul(x).truncate(g_prec + 2);
        Decimal n(jc::BigInt(2), 0);
        Decimal one(jc::BigInt(1), 0);
        Decimal two(jc::BigInt(2), 0);
        int sign = -1;
        
        for (int i = 1; i < 1000; ++i) {
            term = term.mul(x2).div(n.mul(n.add(one))).truncate(g_prec + 2);
            if (term.mantissa.isZero()) break;
            
            Decimal next_sum = (sign == -1) ? sum.sub(term).truncate(g_prec + 2) : sum.add(term).truncate(g_prec + 2);
            
            if (next_sum.eq(sum)) break;
            sum = next_sum;
            
            n = n.add(two);
            sign = -sign;
        }
        return sum.truncate(g_prec);
    }

    Decimal cos_val() const {
        Decimal x = this->mod_2pi();
        Decimal sum(jc::BigInt(1), 0);
        Decimal term(jc::BigInt(1), 0);
        Decimal x2 = x.mul(x).truncate(g_prec + 2);
        Decimal n(jc::BigInt(1), 0);
        Decimal one(jc::BigInt(1), 0);
        Decimal two(jc::BigInt(2), 0);
        int sign = -1;
        
        for (int i = 1; i < 1000; ++i) {
            term = term.mul(x2).div(n.mul(n.add(one))).truncate(g_prec + 2);
            if (term.mantissa.isZero()) break;
            
            Decimal next_sum = (sign == -1) ? sum.sub(term).truncate(g_prec + 2) : sum.add(term).truncate(g_prec + 2);
            
            if (next_sum.eq(sum)) break;
            sum = next_sum;
            
            n = n.add(two);
            sign = -sign;
        }
        return sum.truncate(g_prec);
    }

    static Decimal arctan_series(const Decimal& x) {
        Decimal sum = x;
        Decimal term = x;
        Decimal x2 = x.mul(x).truncate(g_prec + 2);
        Decimal n(jc::BigInt(3), 0);
        Decimal two(jc::BigInt(2), 0);
        int sign = -1;
        
        for (int i = 0; i < 10000; ++i) {
            term = term.mul(x2).truncate(g_prec + 2);
            Decimal cur = term.div(n).truncate(g_prec + 2);
            if (cur.mantissa.isZero()) break;
            
            Decimal next_sum = (sign == -1) ? sum.sub(cur).truncate(g_prec + 2) : sum.add(cur).truncate(g_prec + 2);
            
            if (next_sum.eq(sum)) break;
            sum = next_sum;
            
            n = n.add(two);
            sign = -sign;
        }
        return sum;
    }

    static Decimal pi() {
        Decimal a = arctan_series(Decimal(jc::BigInt(2), -1));
        Decimal b = arctan_series(Decimal(jc::BigInt(1), 0).div(Decimal(jc::BigInt(239), 0)));
        Decimal p = Decimal(jc::BigInt(16), 0).mul(a).sub(Decimal(jc::BigInt(4), 0).mul(b));
        return p.truncate(g_prec);
    }

    Decimal ln_val() const {
        if (mantissa.isZero() || mantissa.isNegative()) {
            jc2::throw_error("MathError: ln of non-positive decimal.");
            return *this;
        }
        int64_t L = mantissa.digitCount();
        double first_digit = (mantissa.abs() / pow10(L - 1)).toDouble();
        double guess = (L - 1 + exp) * 2.302585092994046 + std::log(first_digit);
        
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess);
        Decimal y = Decimal::from_string(buf);
        Decimal two(jc::BigInt(2), 0);
        
        for (int i = 0; i < 50; ++i) {
            Decimal ey = y.exp_val();
            Decimal num = this->sub(ey);
            Decimal den = this->add(ey);
            if (den.mantissa.isZero()) break;
            Decimal diff = two.mul(num).div(den).truncate(g_prec + 2);
            if (diff.mantissa.isZero()) break;
            Decimal next_y = y.add(diff).truncate(g_prec + 2);
            if (next_y.eq(y)) break;
            y = next_y;
        }
        return y.truncate(g_prec);
    }

    Decimal log10_val() const {
        Decimal ln10 = Decimal(jc::BigInt(10), 0).ln_val();
        return this->ln_val().div(ln10).truncate(g_prec);
    }

    Decimal tan_val() const {
        return this->sin_val().div(this->cos_val()).truncate(g_prec);
    }

    Decimal atan_val() const {
        double d;
        try {
            d = std::stod(to_string());
        } catch (...) {
            d = mantissa.isNegative() ? -1e300 : 1e300;
        }
        double guess = std::atan(d);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess);
        Decimal y = Decimal::from_string(buf);
        
        for (int i = 0; i < 50; ++i) {
            Decimal sy = y.sin_val();
            Decimal cy = y.cos_val();
            if (cy.mantissa.isZero()) break;
            Decimal ty = sy.div(cy).truncate(g_prec + 2);
            Decimal diff = cy.mul(cy).mul(this->sub(ty)).truncate(g_prec + 2);
            if (diff.mantissa.isZero()) break;
            Decimal next_y = y.add(diff).truncate(g_prec + 2);
            if (next_y.eq(y)) break;
            y = next_y;
        }
        return y.truncate(g_prec);
    }

    Decimal asin_val() const {
        Decimal one(jc::BigInt(1), 0);
        if (this->abs().lt(one) == false && !this->abs().eq(one)) {
            jc2::throw_error("MathError: asin domain error.");
            return *this;
        }
        double d = std::stod(to_string());
        double guess = std::asin(d);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess);
        Decimal y = Decimal::from_string(buf);
        
        for (int i = 0; i < 50; ++i) {
            Decimal sy = y.sin_val();
            Decimal cy = y.cos_val();
            if (cy.mantissa.isZero()) break;
            Decimal diff = this->sub(sy).div(cy).truncate(g_prec + 2);
            if (diff.mantissa.isZero()) break;
            Decimal next_y = y.add(diff).truncate(g_prec + 2);
            if (next_y.eq(y)) break;
            y = next_y;
        }
        return y.truncate(g_prec);
    }

    Decimal acos_val() const {
        Decimal one(jc::BigInt(1), 0);
        if (this->abs().lt(one) == false && !this->abs().eq(one)) {
            jc2::throw_error("MathError: acos domain error.");
            return *this;
        }
        double d = std::stod(to_string());
        double guess = std::acos(d);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", guess);
        Decimal y = Decimal::from_string(buf);
        
        for (int i = 0; i < 50; ++i) {
            Decimal cy = y.cos_val();
            Decimal sy = y.sin_val();
            if (sy.mantissa.isZero()) break;
            Decimal diff = this->sub(cy).div(sy).truncate(g_prec + 2);
            if (diff.mantissa.isZero()) break;
            Decimal next_y = y.sub(diff).truncate(g_prec + 2);
            if (next_y.eq(y)) break;
            y = next_y;
        }
        return y.truncate(g_prec);
    }

    Decimal sinh_val() const {
        Decimal ex = this->exp_val();
        Decimal emx = Decimal(jc::BigInt(1), 0).div(ex).truncate(g_prec + 2);
        return ex.sub(emx).div(Decimal(jc::BigInt(2), 0)).truncate(g_prec);
    }

    Decimal cosh_val() const {
        Decimal ex = this->exp_val();
        Decimal emx = Decimal(jc::BigInt(1), 0).div(ex).truncate(g_prec + 2);
        return ex.add(emx).div(Decimal(jc::BigInt(2), 0)).truncate(g_prec);
    }

    Decimal tanh_val() const {
        Decimal ex = this->exp_val();
        Decimal emx = Decimal(jc::BigInt(1), 0).div(ex).truncate(g_prec + 2);
        Decimal num = ex.sub(emx);
        Decimal den = ex.add(emx);
        if (den.mantissa.isZero()) return *this;
        return num.div(den).truncate(g_prec);
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

static bool canConvertToDecimal(const jc2::Value& val) {
    if (val.is_instance() && val.get_native_data<std::shared_ptr<Decimal>>()) return true;
    if (val.is_string() || val.is_int() || val.is_double()) return true;
    return false;
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
#define GET_SELF (void)argc; auto d1 = getDecimal(jc2::Value(argv[0]))

METHOD(__str__) { GET_SELF; return jc2::Value(d1->to_string()).get_handle(); }
METHOD(__add__) { GET_SELF; return wrapDecimal(d1->add(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__radd__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).add(*d1)).get_handle(); }
METHOD(__sub__) { GET_SELF; return wrapDecimal(d1->sub(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rsub__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).sub(*d1)).get_handle(); }
METHOD(__mul__) { GET_SELF; return wrapDecimal(d1->mul(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rmul__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).mul(*d1)).get_handle(); }
METHOD(__div__) { GET_SELF; return wrapDecimal(d1->div(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rdiv__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).div(*d1)).get_handle(); }
METHOD(__eq__) { 
    GET_SELF; 
    jc2::Value rhs(argv[1]);
    if (!canConvertToDecimal(rhs)) return jc2::Value(false).get_handle();
    return jc2::Value(d1->eq(parseDecimalArg(rhs))).get_handle(); 
}
METHOD(__lt__) { GET_SELF; return jc2::Value(d1->lt(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__gt__) { GET_SELF; return jc2::Value(d1->gt(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__le__) { GET_SELF; return jc2::Value(d1->le(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__ge__) { GET_SELF; return jc2::Value(d1->ge(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__neq__) { 
    GET_SELF; 
    jc2::Value rhs(argv[1]);
    if (!canConvertToDecimal(rhs)) return jc2::Value(true).get_handle();
    return jc2::Value(d1->neq(parseDecimalArg(rhs))).get_handle(); 
}
METHOD(__neg__) { GET_SELF; return wrapDecimal(Decimal(jc::BigInt(0), 0).sub(*d1)).get_handle(); }
METHOD(__bool__) { GET_SELF; return jc2::Value(!d1->eq(Decimal(jc::BigInt(0), 0))).get_handle(); }
METHOD(__abs__) { GET_SELF; return wrapDecimal(d1->abs()).get_handle(); }
METHOD(sqrt) { GET_SELF; return wrapDecimal(d1->sqrt()).get_handle(); }
METHOD(exp) { GET_SELF; return wrapDecimal(d1->exp_val()).get_handle(); }
METHOD(sin) { GET_SELF; return wrapDecimal(d1->sin_val()).get_handle(); }
METHOD(cos) { GET_SELF; return wrapDecimal(d1->cos_val()).get_handle(); }
METHOD(tan) { GET_SELF; return wrapDecimal(d1->tan_val()).get_handle(); }
METHOD(ln) { GET_SELF; return wrapDecimal(d1->ln_val()).get_handle(); }
METHOD(log10) { GET_SELF; return wrapDecimal(d1->log10_val()).get_handle(); }
METHOD(asin) { GET_SELF; return wrapDecimal(d1->asin_val()).get_handle(); }
METHOD(acos) { GET_SELF; return wrapDecimal(d1->acos_val()).get_handle(); }
METHOD(atan) { GET_SELF; return wrapDecimal(d1->atan_val()).get_handle(); }
METHOD(sinh) { GET_SELF; return wrapDecimal(d1->sinh_val()).get_handle(); }
METHOD(cosh) { GET_SELF; return wrapDecimal(d1->cosh_val()).get_handle(); }
METHOD(tanh) { GET_SELF; return wrapDecimal(d1->tanh_val()).get_handle(); }

JC2_ValueHandle global_Decimal(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    (void)argc;
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

JC2_ValueHandle global_pi(JC2_VMContext, int argc, JC2_ValueHandle*, void*) {
    (void)argc;
    return wrapDecimal(Decimal::pi()).get_handle();
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
    g_decimalClass->bind_method("__gt__", decimal___gt__, 1, 1, false);
    g_decimalClass->bind_method("__le__", decimal___le__, 1, 1, false);
    g_decimalClass->bind_method("__ge__", decimal___ge__, 1, 1, false);
    g_decimalClass->bind_method("__neq__", decimal___neq__, 1, 1, false);
    g_decimalClass->bind_method("__neg__", decimal___neg__, 0, 0, false);
    g_decimalClass->bind_method("__bool__", decimal___bool__, 0, 0, false);
    g_decimalClass->bind_method("__abs__", decimal___abs__, 0, 0, false);
    g_decimalClass->bind_method("sqrt", decimal_sqrt, 0, 0, false);
    g_decimalClass->bind_method("exp", decimal_exp, 0, 0, false);
    g_decimalClass->bind_method("sin", decimal_sin, 0, 0, false);
    g_decimalClass->bind_method("cos", decimal_cos, 0, 0, false);
    g_decimalClass->bind_method("tan", decimal_tan, 0, 0, false);
    g_decimalClass->bind_method("ln", decimal_ln, 0, 0, false);
    g_decimalClass->bind_method("log10", decimal_log10, 0, 0, false);
    g_decimalClass->bind_method("asin", decimal_asin, 0, 0, false);
    g_decimalClass->bind_method("acos", decimal_acos, 0, 0, false);
    g_decimalClass->bind_method("atan", decimal_atan, 0, 0, false);
    g_decimalClass->bind_method("sinh", decimal_sinh, 0, 0, false);
    g_decimalClass->bind_method("cosh", decimal_cosh, 0, 0, false);
    g_decimalClass->bind_method("tanh", decimal_tanh, 0, 0, false);

    mod.register_function("Decimal", global_Decimal, 1, 1, false);
    mod.register_function("getcontext", global_getcontext, 0, 0, false);
    mod.register_function("setcontext", global_setcontext, 1, 1, false);
    mod.register_function("pi", global_pi, 0, 0, false);

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
        "  Math Functions\n"
        "  ──────────────────────\n"
        "    decimal.pi()                  Returns Pi to the current precision\n"
        "    d.sqrt()                      Square root\n"
        "    d.exp()                       Exponential (e^x)\n"
        "    d.ln() / d.log10()            Natural and Base-10 Logarithm\n"
        "    d.sin() / d.cos() / d.tan()   Trigonometric functions\n"
        "    d.asin() / d.acos() / d.atan()Inverse Trigonometric functions\n"
        "    d.sinh() / d.cosh() / d.tanh()Hyperbolic functions\n"
        "    abs(d)                        Absolute value\n\n"
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
