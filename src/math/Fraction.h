#ifndef JC2_FRACTION_H
#define JC2_FRACTION_H

#include "BigInt.h"
#include "Complex.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace jc {

    class Fraction {
    private:
        BigInt num; // 分子
        BigInt den; // 分母 (永远保持正数)

        // 核心约分机制
        void reduce() {
            if (den.isZero()) throw std::runtime_error("Math Error: Fraction denominator cannot be zero.");
            if (num.isZero()) {
                den = BigInt(1);
                return;
            }
            if (den.isNegative()) {
                num = -num;
                den = -den; // 确保符号位总是在分子上
            }
            // 使用 BigInt::gcd 进行公约数消除
            BigInt c = BigInt::gcd(num, den);
            if (c > BigInt(1)) {
                num = num / c;
                den = den / c;
            }
        }

        struct SkipReduce {};
        Fraction(BigInt n, BigInt d, SkipReduce) : num(std::move(n)), den(std::move(d)) {
            // 仅处理符号规范化（den 恒正），不做 gcd
            if (den.isNegative()) {
                num = -num;
                den = -den;
            }
        }

    public:
        // --- 构造函数 ---
        Fraction() : num(0), den(1) {}
        explicit Fraction(BigInt n) : num(std::move(n)), den(1) {}
        Fraction(BigInt n, BigInt d) : num(std::move(n)), den(std::move(d)) { reduce(); }
        explicit Fraction(int64_t n) : num(n), den(1) {}
        Fraction(const Fraction& other) : num(other.num), den(other.den) {}

        // --- 从 double 精确还原分数 ---
        static Fraction fromDouble(double d) {
            if (!std::isfinite(d)) throw std::runtime_error("Math Error: Cannot convert non-finite double to Fraction.");
            if (d == 0.0) return Fraction(BigInt(0));
            bool neg = d < 0;
            if (neg) d = -d;
            int exp;
            double mantissa = std::frexp(d, &exp);
            // mantissa in [0.5, 1.0), d = mantissa * 2^exp
            // mantissa * 2^53 是精确整数
            int64_t significand = static_cast<int64_t>(std::ldexp(mantissa, 53));
            // d = significand * 2^(exp - 53)
            int shift = exp - 53;
            BigInt n(significand);
            BigInt one(1);
            Fraction result;
            if (shift >= 0) {
                result = Fraction(n * BigInt(2).pow(shift), one);
            } else {
                result = Fraction(n, BigInt(2).pow(-shift));
            }
            return neg ? -result : result;
        }

        // --- 属性访问 ---
        BigInt getNum() const { return num; }
        BigInt getDen() const { return den; }
        const BigInt& getNumRef() const { return num; }
        const BigInt& getDenRef() const { return den; }

        double toDouble() const {
            return BigInt::toDoubleRatio(num, den);
        }

        std::string toString() const {
            if (den == BigInt(1)) return num.toString();
            return num.toString() + "/" + den.toString();
        }

        // --- 核心数学运算 ---
        Fraction operator-() const { return Fraction(-num, den); }

        // 加减法：先通分（利用 LCM 和 GCD 防极其巨大的数字膨胀）
        Fraction operator+(const Fraction& other) const {
            BigInt l = BigInt::lcm(den, other.den);
            BigInt n = num * (l / den) + other.num * (l / other.den);
            return Fraction(n, l);
        }

        Fraction operator-(const Fraction& other) const {
            BigInt l = BigInt::lcm(den, other.den);
            BigInt n = num * (l / den) - other.num * (l / other.den);
            return Fraction(n, l);
        }

        // 乘除法：为了防止中间结果越界溢出内存，先交叉约分再乘！
        Fraction operator*(const Fraction& other) const {
            BigInt n1 = BigInt::gcd(num, other.den);
            BigInt n2 = BigInt::gcd(other.num, den);
            BigInt new_num = (num / n1) * (other.num / n2);
            BigInt new_den = (den / n2) * (other.den / n1);
            // 交叉约分后必然互质，直接跳过 reduce 提升一倍性能
            return Fraction(new_num, new_den, SkipReduce{});
        }

        Fraction operator/(const Fraction& other) const {
            if (other.num.isZero()) throw std::runtime_error("Math Error: Division by zero fraction.");
            BigInt n1 = BigInt::gcd(num, other.num);
            BigInt n2 = BigInt::gcd(other.den, den);
            BigInt new_num = (num / n1) * (other.den / n2);
            BigInt new_den = (den / n2) * (other.num / n1);
            return Fraction(new_num, new_den, SkipReduce{});
        }

        Fraction operator%(const Fraction& other) const {
            if (other.num.isZero()) throw std::runtime_error("Math Error: Modulo by zero fraction.");
            BigInt ad = num * other.den;
            BigInt bc = den * other.num;
            BigInt rem = ad % bc;
            return Fraction(rem, den * other.den);
        }

        // 整数次幂运算
        Fraction pow(int64_t p) const {
            if (p == 0) return Fraction(1);
            if (p > 0) {
                // gcd(num, den) = 1 → gcd(num^p, den^p) = 1，跳过约分
                return Fraction(num.pow(p), den.pow(p), SkipReduce{});
            }
            // 负指数：(num/den)^(-|p|) = (den/num)^|p|
            if (num.isZero()) throw std::runtime_error("Math Error: Base 0 requires positive exponent.");
            // 倒数后仍互质，跳过约分
            return Fraction(den.pow(-p), num.pow(-p), SkipReduce{});
        }

        Fraction abs() const {
            return Fraction(num.abs(), den);
        }

        // 比较运算
        bool operator==(const Fraction& o) const { return num == o.num && den == o.den; }
        bool operator!=(const Fraction& o) const { return !(*this == o); }
        bool operator<(const Fraction& o) const { return (num * o.den) < (o.num * den); }
        bool operator>(const Fraction& o) const { return o < *this; }
        bool operator<=(const Fraction& o) const { return !(*this > o); }
        bool operator>=(const Fraction& o) const { return !(*this < o); }

        // =================================================================================
        // 跨维度隐式友元接口 (迎合 Value.h)
        // =================================================================================

        // Fraction <-> BigInt (结果为 Fraction)
        friend Fraction operator+(const Fraction& f, const BigInt& b) { return f + Fraction(b); }
        friend Fraction operator+(const BigInt& b, const Fraction& f) { return Fraction(b) + f; }
        friend Fraction operator-(const Fraction& f, const BigInt& b) { return f - Fraction(b); }
        friend Fraction operator-(const BigInt& b, const Fraction& f) { return Fraction(b) - f; }
        friend Fraction operator*(const Fraction& f, const BigInt& b) { return f * Fraction(b); }
        friend Fraction operator*(const BigInt& b, const Fraction& f) { return Fraction(b) * f; }
        friend Fraction operator/(const Fraction& f, const BigInt& b) { return f / Fraction(b); }
        friend Fraction operator/(const BigInt& b, const Fraction& f) { return Fraction(b) / f; }
        friend Fraction operator%(const Fraction& f, const BigInt& b) { return f % Fraction(b); }
        friend Fraction operator%(const BigInt& b, const Fraction& f) { return Fraction(b) % f; }

        // Fraction <-> Complex (转化为 double 后与复数运算)
        friend Complex operator+(const Fraction& f, const Complex& c) { return Complex(f.toDouble()) + c; }
        friend Complex operator+(const Complex& c, const Fraction& f) { return c + Complex(f.toDouble()); }
        friend Complex operator-(const Fraction& f, const Complex& c) { return Complex(f.toDouble()) - c; }
        friend Complex operator-(const Complex& c, const Fraction& f) { return c - Complex(f.toDouble()); }
        friend Complex operator*(const Fraction& f, const Complex& c) { return Complex(f.toDouble()) * c; }
        friend Complex operator*(const Complex& c, const Fraction& f) { return c * Complex(f.toDouble()); }
        friend Complex operator/(const Fraction& f, const Complex& c) { return Complex(f.toDouble()) / c; }
        friend Complex operator/(const Complex& c, const Fraction& f) { return c / Complex(f.toDouble()); }

        // 格式化倒出
        friend std::ostream& operator<<(std::ostream& os, const Fraction& f) {
            return os << f.toString();
        }
    };

} // namespace jc
#endif // JC2_FRACTION_H
