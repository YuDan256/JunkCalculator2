#ifndef JC2_COMPLEX_H
#define JC2_COMPLEX_H

#include "Tolerance.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace jc {

    struct Complex {
        double real;
        double imag;

        // --- 常量 ---
        static constexpr double PI = 3.14159265358979323846;
        static constexpr double E = 2.71828182845904523536;

        // --- 构造机制 ---
        Complex() : real(0.0), imag(0.0) {}
        Complex(double r) : real(r), imag(0.0) {}
        Complex(double r, double i) : real(r), imag(i) {}

        // --- 基础数学与属性 ---
        double getReal() const { return real; }
        double getImage() const { return imag; }
        bool isNumber() const { return Tol::clean(imag, std::abs(real)) == 0.0; }
        bool isInteger() const { return isNumber() && real == std::round(real); }

        double modulus() const { return std::sqrt(real * real + imag * imag); }

        double argument() const {
            if (real == 0.0 && imag == 0.0) throw std::runtime_error("Math Error: The argument of 0 is arbitrary.");
            return std::atan2(imag, real);
        }

        static void cleanRoots(std::vector<Complex>& roots) {
            for (auto& r : roots) {
                double mag = r.modulus();
                if (mag == 0.0) continue;
                // 按模长规模清洗实部和虚部
                r.imag = Tol::clean(r.imag, mag);
                r.real = Tol::clean(r.real, mag);
            }
        }
        // --- 单个复数清洗机制 ---
        static Complex cleaned(const Complex& c) {
            double re = c.real, im = c.imag;
            double mag = std::sqrt(re * re + im * im);

            if (mag != 0.0) {
                im = Tol::clean(im, mag);
                re = Tol::clean(re, mag);
            }
            return { re, im };
        }

        Complex conjugate() const { return { real, -imag }; }

        // --- 算术运算符 ---
        Complex operator-() const { return { -real, -imag }; }

        Complex operator+(const Complex& other) const { return { real + other.real, imag + other.imag }; }
        Complex operator+(double other) const { return { real + other, imag }; }
        friend Complex operator+(double lhs, const Complex& rhs) { return { lhs + rhs.real, rhs.imag }; }

        Complex operator-(const Complex& other) const { return { real - other.real, imag - other.imag }; }
        Complex operator-(double other) const { return { real - other, imag }; }
        friend Complex operator-(double lhs, const Complex& rhs) { return { lhs - rhs.real, -rhs.imag }; }

        Complex operator*(const Complex& other) const {
            return { real * other.real - imag * other.imag, real * other.imag + imag * other.real };
        }
        Complex operator*(double other) const { return { real * other, imag * other }; }
        friend Complex operator*(double lhs, const Complex& rhs) { return { lhs * rhs.real, lhs * rhs.imag }; }

        Complex operator/(const Complex& other) const {
            double den = other.real * other.real + other.imag * other.imag;
            if (den == 0.0) throw std::runtime_error("Math Error: Division by zero complex number.");
            return { (real * other.real + imag * other.imag) / den, (imag * other.real - real * other.imag) / den };
        }
        Complex operator/(double other) const {
            if (other == 0.0) throw std::runtime_error("Math Error: Division by zero.");
            return { real / other, imag / other };
        }
        friend Complex operator/(double lhs, const Complex& rhs) {
            double den = rhs.real * rhs.real + rhs.imag * rhs.imag;
            if (den == 0.0) throw std::runtime_error("Math Error: Division by zero complex number.");
            return { (lhs * rhs.real) / den, (-lhs * rhs.imag) / den };
        }

        // --- 逻辑与比较 ---
        bool operator==(const Complex& other) const {
            return real == other.real && imag == other.imag;
        }
        bool operator==(double other) const {
            return real == other && imag == 0.0;
        }
        bool operator!=(const Complex& other) const { return !(*this == other); }
        bool operator!=(double other) const { return !(*this == other); }
        friend bool operator==(double lhs, const Complex& rhs) { return rhs == lhs; }

        // =================================================================================
        // 超越函数 (Transcendental Functions) - 全部设为 friend 供 ADL 完美调用
        // =================================================================================

        friend Complex exp(const Complex& z) {
            if (z.real == 0.0 && z.imag == 0.0) return { 1.0, 0.0 };
            double r = std::exp(z.real);
            return { r * std::cos(z.imag), r * std::sin(z.imag) };
        }

        friend Complex log(const Complex& z) { // 即自带的 ln
            if (z.real == 0.0 && z.imag == 0.0) throw std::runtime_error("Math Error: Logarithm of zero.");
            return { std::log(z.modulus()), z.argument() };
        }

        // 复数乘方 a^b = e^(b*ln(a))
        Complex operator^(const Complex& power) const {
            if (real == 0.0 && imag == 0.0) {
                if (power.real <= 0) throw std::runtime_error("Math Error: Base 0 requires positive real exponent.");
                return { 0.0, 0.0 };
            }
            if (power.imag == 0.0) return (*this) ^ power.real; // 降级提速
            return exp(power * log(*this));
        }

        Complex operator^(double power) const {
            if (real == 0.0 && imag == 0.0 && power <= 0) throw std::runtime_error("Math Error: Base 0 requires positive exponent.");
            if (real == 0.0 && imag == 0.0) return { 0.0, 0.0 };
            double a = power * argument();
            double r = std::pow(modulus(), power);
            return { r * std::cos(a), r * std::sin(a) };
        }

        friend Complex operator^(double base, const Complex& power) { return Complex(base) ^ power; }

        // 三角函数
        friend Complex sin(const Complex& z) {
            return { std::sin(z.real) * std::cosh(z.imag), std::cos(z.real) * std::sinh(z.imag) };
        }
        friend Complex cos(const Complex& z) {
            return { std::cos(z.real) * std::cosh(z.imag), -std::sin(z.real) * std::sinh(z.imag) };
        }
        friend Complex tan(const Complex& z) {
            Complex c = cos(z);
            if (c.real == 0.0 && c.imag == 0.0) throw std::runtime_error("Math Error: Tangent undefined.");
            return sin(z) / c;
        }

        // 双曲函数
        friend Complex sinh(const Complex& z) { return (exp(z) - exp(-z)) / 2.0; }
        friend Complex cosh(const Complex& z) { return (exp(z) + exp(-z)) / 2.0; }
        friend Complex tanh(const Complex& z) {
            Complex c = cosh(z);
            if (c.real == 0.0 && c.imag == 0.0) throw std::runtime_error("Math Error: Tanh undefined.");
            return sinh(z) / c;
        }

        // 反三角函数 (利用对数定义)
        friend Complex asin(const Complex& z) {
            Complex i(0, 1);
            return -i * log(i * z + sqrt(1.0 - (z ^ 2.0)));
        }
        friend Complex acos(const Complex& z) {
            Complex i(0, 1);
            return -i * log(z + i * sqrt(1.0 - (z ^ 2.0)));
        }
        friend Complex atan(const Complex& z) {
            Complex i(0, 1);
            return 0.5 * i * (log(1.0 + i * z) - log(1.0 - i * z));
        }

        // 开方与根
        friend Complex sqrt(const Complex& z) {
            return z.firstRoot(2);
        }

        Complex firstRoot(int n) const {
            if (n <= 0) throw std::runtime_error("Math Error: Root degree must be positive.");
            if (real == 0.0 && imag == 0.0) return { 0.0, 0.0 };
            double k = static_cast<double>(n);
            double a = argument() / k;
            double r = std::pow(modulus(), 1.0 / k);
            return { r * std::cos(a), r * std::sin(a) };
        }

        friend std::ostream& operator<<(std::ostream& os, const Complex& c) {
            Complex v = cleaned(c);
            bool reZero = (std::abs(v.real) == 0.0);
            bool imZero = (std::abs(v.imag) == 0.0);

            auto formatDouble = [](double d) {
                std::ostringstream temp;
                temp << d;
                std::string s = temp.str();
                if (std::isfinite(d) && s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
                    s += ".0";
                }
                return s;
            };

            if (reZero && imZero) return os << "0.0";
            if (!reZero) os << formatDouble(v.real);
            if (!imZero) {
                if (v.imag > 0 && !reZero) os << "+";
                if (v.imag == 1.0) os << "i";
                else if (v.imag == -1.0) os << "-i";
                else os << formatDouble(v.imag) << "i";
            }
            return os;
        }

        // =================================================================================
        // 多项式方程求解引擎 (Polynomial Solvers)
        // =================================================================================
        // 现在的版本不仅求解，所有求出来的根全部打包进 vector，未来可以直接被解析器变成矩阵向外抛！

        static std::vector<Complex> solveDegreeOne(const Complex& a, const Complex& b) {
            if (a.real == 0.0 && a.imag == 0.0) {
                if (b.real != 0.0 || b.imag != 0.0) throw std::runtime_error("Math Error: Equation has no solution.");
                throw std::runtime_error("Math Error: Infinitely many solutions.");
            }
            std::vector<Complex> roots = { -b / a };
            cleanRoots(roots);
            return roots;
        }

        static std::vector<Complex> solveDegreeTwo(const Complex& a, const Complex& b, const Complex& c) {
            if (a.real == 0.0 && a.imag == 0.0) return solveDegreeOne(b, c);
            Complex delta = sqrt(b * b - 4.0 * a * c);
            std::vector<Complex> roots = { (-b + delta) / (2.0 * a), (-b - delta) / (2.0 * a) };
            cleanRoots(roots);
            return roots;
        }

        static std::vector<Complex> solveDegreeThree(const Complex& a, const Complex& b, const Complex& c, const Complex& d) {
            if (a.real == 0.0 && a.imag == 0.0) return solveDegreeTwo(b, c, d);
            Complex u = (9.0 * a * b * c - 27.0 * (a ^ 2.0) * d - 2.0 * (b ^ 3.0)) / (54.0 * (a ^ 3.0));
            Complex v = sqrt(3.0 * (4.0 * a * (c ^ 3.0) - (b ^ 2.0) * (c ^ 2.0) - 18.0 * a * b * c * d + 27.0 * (a ^ 2.0) * (d ^ 2.0) + 4.0 * (b ^ 3.0) * d)) / (18.0 * (a ^ 2.0));
            Complex m, n, w(-0.5, std::sqrt(3.0) / 2.0);

            if ((u + v).modulus() > (u - v).modulus()) m = (u + v).firstRoot(3);
            else m = (u - v).firstRoot(3);

            if (m.real == 0.0 && m.imag == 0.0) n = { 0.0, 0.0 };
            else n = ((b ^ 2.0) - 3.0 * a * c) / (9.0 * (a ^ 2.0) * m);

            Complex offset = -b / (3.0 * a);
            std::vector<Complex> roots = {
                m + n + offset,
                w * m + (w ^ 2.0) * n + offset,
                (w ^ 2.0) * m + w * n + offset
            };
            cleanRoots(roots);
            return roots;
        }

        static std::vector<Complex> solveDegreeFour(const Complex& a, const Complex& b, const Complex& c, const Complex& d, const Complex& e) {
            if (a.real == 0.0 && a.imag == 0.0) return solveDegreeThree(b, c, d, e);

            Complex P = (c * c + 12.0 * a * e - 3.0 * b * d) / 9.0;
            Complex Q = (27.0 * a * d * d + 2.0 * c * c * c + 27.0 * b * b * e - 72.0 * a * c * e - 9.0 * b * c * d) / 54.0;
            Complex D = sqrt(Q * Q - P * P * P);

            Complex u, v, w(-0.5, std::sqrt(3.0) / 2.0);
            Complex s1 = Q + D, s2 = Q - D;

            if (s1.modulus() > s2.modulus()) u = s1.firstRoot(3);
            else u = s2.firstRoot(3);

            if (Tol::clean(u.modulus(), s1.modulus(), 1e6) == 0.0) {
                u = { 0.0, 0.0 };
                v = { 0.0, 0.0 };
            }
            else {
                v = P / u;
            }
            Complex m = { 0.0, 0.0 }, S = { 0.0, 0.0 }, T = { 0.0, 0.0 }, t = { 0.0, 0.0 };
            int k = 1;

            for (int i = 1; i < 4; i++) {
                t = b * b - 8.0 * a * c / 3.0 + 4.0 * a * ((w ^ (i - 1.0)) * u + (w ^ (4.0 - i)) * v);
                t = sqrt(t);
                if (t.modulus() > m.modulus()) {
                    m = t;
                    k = i;
                }
            }
            if (Tol::clean(m.modulus(), b.modulus() + std::abs(P.modulus()), 1e6) == 0.0) {
                m = { 0.0, 0.0 };
                S = b * b - 8.0 * a * c / 3.0;
                T = { 0.0, 0.0 };
            }
            else {
                S = 2.0 * b * b - 16.0 * a * c / 3.0 - 4.0 * a * ((w ^ (k - 1.0)) * u + (w ^ (4.0 - k)) * v);
                T = (8.0 * a * b * c - 16.0 * a * a * d - 2.0 * b * b * b) / m;
            }
            Complex p1 = sqrt(S - T);
            Complex p2 = sqrt(S + T);
            std::vector<Complex> roots = {
                (-m + p1 - b) / (4.0 * a),
                (-m - p1 - b) / (4.0 * a),
                (m + p2 - b) / (4.0 * a),
                (m - p2 - b) / (4.0 * a)
            };
            cleanRoots(roots);
            return roots;
        }
    };

} // namespace jc
#endif // JC2_COMPLEX_H
