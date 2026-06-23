#ifndef JC2_PROBABILITY_H
#define JC2_PROBABILITY_H

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace jc {

    // =================================================================
    // 特殊数学函数 (Special Functions)
    // =================================================================
    namespace prob {

        static constexpr double PI_ = 3.14159265358979323846;
        static constexpr double SQRT2_ = 1.41421356237309504880;
        static constexpr double SQRT2PI = 2.50662827463100050242;
        static constexpr double LN_SQRT2PI = 0.91893853320467274178;

        // --- Lanczos Log-Gamma (g=7, ~15 位精度) ---
        inline double lngamma(double x) {
            if (x <= 0 && x == std::floor(x))
                throw std::runtime_error("Math Error: lgamma undefined at non-positive integers.");
            if (x <= 0) {
                double sinpx = std::sin(PI_ * x);
                if (std::abs(sinpx) < 1e-300)
                    throw std::runtime_error("Math Error: lgamma pole.");
                return std::log(PI_ / std::abs(sinpx)) - lngamma(1.0 - x);
            }
            static const double c[8] = {
                676.5203681218851,     -1259.1392167224028,
                771.32342877765313,    -176.61502916214059,
                12.507343278686905,    -0.13857109526572012,
                9.9843695780195716e-6,  1.5056327351493116e-7
            };
            double sum = 0.99999999999980993;
            for (int i = 0; i < 8; ++i) sum += c[i] / (x + i);
            double t = x + 6.5;
            return LN_SQRT2PI + (x - 0.5) * std::log(t) - t + std::log(sum);
        }

        inline double tgamma(double x) {
            if (x <= 0 && x == std::floor(x))
                throw std::runtime_error("Math Error: Gamma undefined at non-positive integers.");
            if (x < 0.5) return PI_ / (std::sin(PI_ * x) * tgamma(1.0 - x));
            return std::exp(lngamma(x));
        }

        inline double betafn(double a, double b) {
            return std::exp(lngamma(a) + lngamma(b) - lngamma(a + b));
        }

        // --- 误差函数 (Abramowitz & Stegun 7.1.26) ---
        inline double erf_impl(double x) {
            if (x > 6.0) return 1.0;
            if (x < -6.0) return -1.0;
            bool neg = x < 0; if (neg) x = -x;
            double t = 1.0 / (1.0 + 0.3275911 * x);
            double poly = t * (0.254829592 + t * (-0.284496736 +
                t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
            double r = 1.0 - poly * std::exp(-x * x);
            return neg ? -r : r;
        }
        inline double erfc_impl(double x) { return 1.0 - erf_impl(x); }

        // --- 正则化不完全 Gamma 函数 P(a,x) ---
        inline double gammainc_series(double a, double x, int N = 200) {
            if (x <= 0) return 0.0;
            double ap = a, sum = 1.0 / a, del = sum;
            for (int n = 1; n <= N; ++n) {
                ap += 1.0; del *= x / ap; sum += del;
                if (std::abs(del) < std::abs(sum) * 1e-15) break;
            }
            return sum * std::exp(-x + a * std::log(x) - lngamma(a));
        }
        inline double gammainc_cf(double a, double x, int N = 200) {
            double b = x + 1.0 - a, c = 1.0 / 1e-30, d = 1.0 / b, h = d;
            for (int n = 1; n <= N; ++n) {
                double an = -n * (n - a);
                b += 2.0;
                d = an * d + b; if (std::abs(d) < 1e-30) d = 1e-30;
                c = b + an / c; if (std::abs(c) < 1e-30) c = 1e-30;
                d = 1.0 / d; double del = d * c; h *= del;
                if (std::abs(del - 1.0) < 1e-15) break;
            }
            return std::exp(-x + a * std::log(x) - lngamma(a)) * h;
        }
        inline double gammainc(double a, double x) {
            if (x <= 0) return 0.0;
            return (x < a + 1.0) ? gammainc_series(a, x) : 1.0 - gammainc_cf(a, x);
        }

        // --- 正则化不完全 Beta 函数 I_x(a,b) ---
        inline double betacf(double a, double b, double x, int N = 200) {
            double qab = a + b, qap = a + 1.0, qam = a - 1.0;
            double c = 1.0, d = 1.0 - qab * x / qap;
            if (std::abs(d) < 1e-30) d = 1e-30;
            d = 1.0 / d; double h = d;
            for (int m = 1; m <= N; ++m) {
                int m2 = 2 * m;
                double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
                d = 1.0 + aa * d; if (std::abs(d) < 1e-30) d = 1e-30;
                c = 1.0 + aa / c; if (std::abs(c) < 1e-30) c = 1e-30;
                d = 1.0 / d; h *= d * c;
                aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
                d = 1.0 + aa * d; if (std::abs(d) < 1e-30) d = 1e-30;
                c = 1.0 + aa / c; if (std::abs(c) < 1e-30) c = 1e-30;
                d = 1.0 / d; double del = d * c; h *= del;
                if (std::abs(del - 1.0) < 1e-15) break;
            }
            return h;
        }
        inline double betainc(double a, double b, double x) {
            if (x <= 0) return 0.0; if (x >= 1) return 1.0;
            double bt = std::exp(lngamma(a + b) - lngamma(a) - lngamma(b)
                + a * std::log(x) + b * std::log(1.0 - x));
            if (x < (a + 1.0) / (a + b + 2.0))
                return bt * betacf(a, b, x) / a;
            return 1.0 - bt * betacf(b, a, 1.0 - x) / b;
        }

        // --- Peter Acklam 逆正态 (~1.15e-9 精度) ---
        inline double normalInvStd(double p) {
            if (p <= 0 || p >= 1) throw std::runtime_error("Math Error: p must be in (0,1).");
            static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02,
                -2.759285104469687e+02, 1.383577518672690e+02,
                -3.066479806614716e+01, 2.506628277459239e+00 };
            static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02,
                -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
            static const double c[] = { -7.784894002430293e-03,-3.223964580411365e-01,
                -2.400758277161838e+00,-2.549732539343734e+00,
                 4.374664141464968e+00, 2.938163982698783e+00 };
            static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01,
                 2.445134137142996e+00, 3.754408661907416e+00 };
            double q, r, z;
            constexpr double pLow = 0.02425, pHigh = 1.0 - pLow;
            if (p < pLow) {
                q = std::sqrt(-2.0 * std::log(p));
                z = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
                    ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
            }
            else if (p <= pHigh) {
                q = p - 0.5; r = q * q;
                z = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
                    (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
            }
            else {
                q = std::sqrt(-2.0 * std::log(1.0 - p));
                z = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
                    ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
            }
            return z;
        }

    } // namespace prob

    // =================================================================
    // Distribution 类 — 概率分布一等公民
    // =================================================================
    class Distribution {
    public:
        enum class Type {
            NORMAL, STUDENT_T, CHI_SQUARED, F_DIST,
            EXPONENTIAL, GAMMA_DIST, BETA_DIST, UNIFORM_DIST,
            BINOMIAL, POISSON, GEOMETRIC
        };

        Type type;
        std::vector<double> params;

        Distribution(Type t, std::vector<double> p) : type(t), params(std::move(p)) {}

        // ─────────────────────────────────────────────
        // 构造器 (Named Constructors)
        // ─────────────────────────────────────────────
        static Distribution normal(double mu = 0, double sigma = 1) {
            if (sigma <= 0) throw std::runtime_error("Math Error: sigma must be positive.");
            return { Type::NORMAL, {mu, sigma} };
        }
        static Distribution studentT(double df) {
            if (df <= 0) throw std::runtime_error("Math Error: df must be positive.");
            return { Type::STUDENT_T, {df} };
        }
        static Distribution chiSquared(double df) {
            if (df <= 0) throw std::runtime_error("Math Error: df must be positive.");
            return { Type::CHI_SQUARED, {df} };
        }
        static Distribution fDist(double d1, double d2) {
            if (d1 <= 0 || d2 <= 0) throw std::runtime_error("Math Error: df must be positive.");
            return { Type::F_DIST, {d1, d2} };
        }
        static Distribution exponential(double lam) {
            if (lam <= 0) throw std::runtime_error("Math Error: lambda must be positive.");
            return { Type::EXPONENTIAL, {lam} };
        }
        static Distribution gammaDist(double shape, double rate) {
            if (shape <= 0 || rate <= 0) throw std::runtime_error("Math Error: shape/rate must be positive.");
            return { Type::GAMMA_DIST, {shape, rate} };
        }
        static Distribution betaDist(double a, double b) {
            if (a <= 0 || b <= 0) throw std::runtime_error("Math Error: alpha/beta must be positive.");
            return { Type::BETA_DIST, {a, b} };
        }
        static Distribution uniformDist(double a, double b) {
            if (a >= b) throw std::runtime_error("Math Error: a must be less than b.");
            return { Type::UNIFORM_DIST, {a, b} };
        }
        static Distribution binomial(int n, double p) {
            if (n < 0) throw std::runtime_error("Math Error: n must be non-negative.");
            if (p < 0 || p > 1) throw std::runtime_error("Math Error: p must be in [0,1].");
            return { Type::BINOMIAL, {static_cast<double>(n), p} };
        }
        static Distribution poisson(double lam) {
            if (lam < 0) throw std::runtime_error("Math Error: lambda must be non-negative.");
            return { Type::POISSON, {lam} };
        }
        static Distribution geometric(double p) {
            if (p <= 0 || p > 1) throw std::runtime_error("Math Error: p must be in (0,1].");
            return { Type::GEOMETRIC, {p} };
        }

        bool isDiscrete() const {
            return type == Type::BINOMIAL || type == Type::POISSON || type == Type::GEOMETRIC;
        }

        // ─────────────────────────────────────────────
        // PDF / PMF
        // ─────────────────────────────────────────────
        double pdf(double x) const {
            using namespace prob;
            switch (type) {
            case Type::NORMAL: {
                double mu = params[0], sigma = params[1];
                double z = (x - mu) / sigma;
                return std::exp(-0.5 * z * z) / (sigma * SQRT2PI);
            }
            case Type::STUDENT_T: {
                double df = params[0];
                return std::exp(lngamma((df + 1) / 2) - lngamma(df / 2))
                    / std::sqrt(df * PI_) * std::pow(1.0 + x * x / df, -(df + 1) / 2);
            }
            case Type::CHI_SQUARED: {
                double df = params[0];
                if (x < 0) return 0.0;
                if (x == 0) return (df == 2) ? 0.5 : (df < 2 ? INFINITY : 0.0);
                double k2 = df / 2.0;
                return std::exp((k2 - 1) * std::log(x) - x / 2 - k2 * std::log(2.0) - lngamma(k2));
            }
            case Type::F_DIST: {
                double d1 = params[0], d2 = params[1];
                if (x <= 0) return 0.0;
                return std::exp(lngamma((d1 + d2) / 2) - lngamma(d1 / 2) - lngamma(d2 / 2)
                    + (d1 / 2) * std::log(d1 / d2) + (d1 / 2 - 1) * std::log(x)
                    - ((d1 + d2) / 2) * std::log(1.0 + d1 * x / d2));
            }
            case Type::EXPONENTIAL: {
                double lam = params[0];
                return x < 0 ? 0.0 : lam * std::exp(-lam * x);
            }
            case Type::GAMMA_DIST: {
                double a = params[0], b = params[1];
                if (x <= 0) return 0.0;
                return std::exp((a - 1) * std::log(x) - b * x + a * std::log(b) - lngamma(a));
            }
            case Type::BETA_DIST: {
                double a = params[0], b = params[1];
                if (x <= 0 || x >= 1) return 0.0;
                return std::exp((a - 1) * std::log(x) + (b - 1) * std::log(1 - x)
                    - lngamma(a) - lngamma(b) + lngamma(a + b));
            }
            case Type::UNIFORM_DIST: {
                double a = params[0], b = params[1];
                return (x < a || x > b) ? 0.0 : 1.0 / (b - a);
            }
            case Type::BINOMIAL: {
                int k = static_cast<int>(std::round(x));
                int n = static_cast<int>(params[0]); double p = params[1];
                if (k < 0 || k > n) return 0.0;
                if (p == 0) return k == 0 ? 1.0 : 0.0;
                if (p == 1) return k == n ? 1.0 : 0.0;
                return std::exp(lngamma(n + 1.0) - lngamma(k + 1.0) - lngamma(n - k + 1.0)
                    + k * std::log(p) + (n - k) * std::log(1 - p));
            }
            case Type::POISSON: {
                int k = static_cast<int>(std::round(x));
                double lam = params[0];
                if (k < 0) return 0.0;
                if (lam == 0) return k == 0 ? 1.0 : 0.0;
                return std::exp(k * std::log(lam) - lam - lngamma(k + 1.0));
            }
            case Type::GEOMETRIC: {
                int k = static_cast<int>(std::round(x));
                double p = params[0];
                if (k < 1) return 0.0;
                return std::pow(1.0 - p, k - 1) * p;
            }
            }
            return 0.0;
        }

        // ─────────────────────────────────────────────
        // CDF
        // ─────────────────────────────────────────────
        double cdf(double x) const {
            using namespace prob;
            switch (type) {
            case Type::NORMAL: {
                double mu = params[0], sigma = params[1];
                return 0.5 * (1.0 + erf_impl((x - mu) / (sigma * SQRT2_)));
            }
            case Type::STUDENT_T: {
                double df = params[0];
                if (x == 0) return 0.5;
                double bx = df / (df + x * x);
                double ib = betainc(df / 2.0, 0.5, bx);
                return x > 0 ? 1.0 - 0.5 * ib : 0.5 * ib;
            }
            case Type::CHI_SQUARED: {
                double df = params[0];
                if (x <= 0) return 0.0;
                return gammainc(df / 2.0, x / 2.0);
            }
            case Type::F_DIST: {
                double d1 = params[0], d2 = params[1];
                if (x <= 0) return 0.0;
                return betainc(d1 / 2, d2 / 2, d1 * x / (d1 * x + d2));
            }
            case Type::EXPONENTIAL: {
                double lam = params[0];
                return x < 0 ? 0.0 : 1.0 - std::exp(-lam * x);
            }
            case Type::GAMMA_DIST: {
                double a = params[0], b = params[1];
                if (x <= 0) return 0.0;
                return gammainc(a, b * x);
            }
            case Type::BETA_DIST: {
                double a = params[0], b = params[1];
                if (x <= 0) return 0.0; if (x >= 1) return 1.0;
                return betainc(a, b, x);
            }
            case Type::UNIFORM_DIST: {
                double a = params[0], b = params[1];
                if (x <= a) return 0.0; if (x >= b) return 1.0;
                return (x - a) / (b - a);
            }
            case Type::BINOMIAL: {
                int k = static_cast<int>(std::floor(x));
                int n = static_cast<int>(params[0]); double p = params[1];
                if (k < 0) return 0.0; if (k >= n) return 1.0;
                if (p == 0) return 1.0; if (p == 1) return 0.0;
                return 1.0 - betainc(k + 1.0, n - k, p);
            }
            case Type::POISSON: {
                int k = static_cast<int>(std::floor(x));
                double lam = params[0];
                if (k < 0) return 0.0; if (lam == 0) return 1.0;
                return 1.0 - gammainc(k + 1.0, lam);
            }
            case Type::GEOMETRIC: {
                int k = static_cast<int>(std::floor(x));
                double p = params[0];
                if (k < 1) return 0.0;
                return 1.0 - std::pow(1.0 - p, k);
            }
            }
            return 0.0;
        }

        // ─────────────────────────────────────────────
        // 逆 CDF (Quantile) — 连续分布用 Newton 迭代
        // ─────────────────────────────────────────────
        double quantile(double p) const {
            using namespace prob;
            if (p <= 0 || p >= 1) throw std::runtime_error("Math Error: p must be in (0,1).");

            switch (type) {
            case Type::NORMAL: {
                return params[0] + params[1] * normalInvStd(p);
            }
            case Type::UNIFORM_DIST: {
                return params[0] + p * (params[1] - params[0]);
            }
            case Type::EXPONENTIAL: {
                return -std::log(1.0 - p) / params[0];
            }
                                  // 离散分布：递增搜索
            case Type::BINOMIAL: case Type::POISSON: case Type::GEOMETRIC: {
                int k = 0;
                while (cdf(static_cast<double>(k)) < p) {
                    k++;
                    if (k > 1000000) throw std::runtime_error("Math Error: quantile search exceeded limit.");
                }
                return static_cast<double>(k);
            }
                               // 其他连续分布：Newton-Raphson
            default: return newtonQuantile(p);
            }
        }

        // ─────────────────────────────────────────────
        // 分布矩 (Mean / Variance)
        // ─────────────────────────────────────────────
        double distMean() const {
            switch (type) {
            case Type::NORMAL:       return params[0];
            case Type::STUDENT_T: { double df = params[0]; if (df <= 1) throw std::runtime_error("Math Error: Mean undefined for df<=1."); return 0; }
            case Type::CHI_SQUARED:  return params[0];
            case Type::F_DIST: { double d2 = params[1]; if (d2 <= 2) throw std::runtime_error("Math Error: Mean undefined for d2<=2."); return d2 / (d2 - 2); }
            case Type::EXPONENTIAL:  return 1.0 / params[0];
            case Type::GAMMA_DIST:   return params[0] / params[1];
            case Type::BETA_DIST:    return params[0] / (params[0] + params[1]);
            case Type::UNIFORM_DIST: return (params[0] + params[1]) / 2.0;
            case Type::BINOMIAL:     return params[0] * params[1];
            case Type::POISSON:      return params[0];
            case Type::GEOMETRIC:    return 1.0 / params[0];
            }
            return 0;
        }

        double distVar() const {
            switch (type) {
            case Type::NORMAL:       return params[1] * params[1];
            case Type::STUDENT_T: { double df = params[0]; if (df <= 2) throw std::runtime_error("Math Error: Variance undefined for df<=2."); return df / (df - 2); }
            case Type::CHI_SQUARED:  return 2.0 * params[0];
            case Type::F_DIST: {
                double d1 = params[0], d2 = params[1];
                if (d2 <= 4) throw std::runtime_error("Math Error: Variance undefined for d2<=4.");
                return 2.0 * d2 * d2 * (d1 + d2 - 2) / (d1 * (d2 - 2) * (d2 - 2) * (d2 - 4));
            }
            case Type::EXPONENTIAL: { double l = params[0]; return 1.0 / (l * l); }
            case Type::GAMMA_DIST: { double a = params[0], b = params[1]; return a / (b * b); }
            case Type::BETA_DIST: { double a = params[0], b = params[1], s = a + b; return a * b / (s * s * (s + 1)); }
            case Type::UNIFORM_DIST: { double w = params[1] - params[0]; return w * w / 12.0; }
            case Type::BINOMIAL: { double n = params[0], p = params[1]; return n * p * (1 - p); }
            case Type::POISSON:      return params[0];
            case Type::GEOMETRIC: { double p = params[0]; return (1 - p) / (p * p); }
            }
            return 0;
        }

        // ─────────────────────────────────────────────
        // 随机采样
        // ─────────────────────────────────────────────
        std::vector<double> sample(int n) const {
            static std::mt19937 gen(std::random_device{}());
            std::uniform_real_distribution<double> U(0.0, 1.0);
            std::vector<double> result(n);

            switch (type) {
            case Type::NORMAL: {
                std::normal_distribution<double> nd(params[0], params[1]);
                for (int i = 0; i < n; ++i) result[i] = nd(gen);
                break;
            }
            case Type::UNIFORM_DIST: {
                std::uniform_real_distribution<double> ud(params[0], params[1]);
                for (int i = 0; i < n; ++i) result[i] = ud(gen);
                break;
            }
            case Type::EXPONENTIAL: {
                std::exponential_distribution<double> ed(params[0]);
                for (int i = 0; i < n; ++i) result[i] = ed(gen);
                break;
            }
            case Type::BINOMIAL: {
                std::binomial_distribution<int> bd(static_cast<int>(params[0]), params[1]);
                for (int i = 0; i < n; ++i) result[i] = static_cast<double>(bd(gen));
                break;
            }
            case Type::POISSON: {
                std::poisson_distribution<int> pd(params[0]);
                for (int i = 0; i < n; ++i) result[i] = static_cast<double>(pd(gen));
                break;
            }
            case Type::GEOMETRIC: {
                std::geometric_distribution<int> gd(params[0]);
                for (int i = 0; i < n; ++i) result[i] = static_cast<double>(gd(gen) + 1);
                break;
            }
            default: {
                // 通用路径：逆 CDF 变换 (quantile(U))
                for (int i = 0; i < n; ++i) {
                    double u = U(gen);
                    u = std::max(1e-15, std::min(1.0 - 1e-15, u));
                    result[i] = quantile(u);
                }
                break;
            }
            }
            return result;
        }

        // ─────────────────────────────────────────────
        // 字符串表示
        // ─────────────────────────────────────────────
        std::string toString() const {
            std::ostringstream oss;
            switch (type) {
            case Type::NORMAL:       oss << "Normal(mu=" << params[0] << ", sigma=" << params[1] << ")"; break;
            case Type::STUDENT_T:    oss << "StudentT(df=" << params[0] << ")"; break;
            case Type::CHI_SQUARED:  oss << "Chi2(df=" << params[0] << ")"; break;
            case Type::F_DIST:       oss << "FDist(d1=" << params[0] << ", d2=" << params[1] << ")"; break;
            case Type::EXPONENTIAL:  oss << "Exp(lambda=" << params[0] << ")"; break;
            case Type::GAMMA_DIST:   oss << "Gamma(shape=" << params[0] << ", rate=" << params[1] << ")"; break;
            case Type::BETA_DIST:    oss << "Beta(a=" << params[0] << ", b=" << params[1] << ")"; break;
            case Type::UNIFORM_DIST: oss << "Uniform(a=" << params[0] << ", b=" << params[1] << ")"; break;
            case Type::BINOMIAL:     oss << "Binom(n=" << static_cast<int>(params[0]) << ", p=" << params[1] << ")"; break;
            case Type::POISSON:      oss << "Poisson(lambda=" << params[0] << ")"; break;
            case Type::GEOMETRIC:    oss << "Geom(p=" << params[0] << ")"; break;
            }
            return oss.str();
        }

        std::string typeName() const {
            switch (type) {
            case Type::NORMAL:       return "Normal";
            case Type::STUDENT_T:    return "StudentT";
            case Type::CHI_SQUARED:  return "Chi2";
            case Type::F_DIST:       return "FDist";
            case Type::EXPONENTIAL:  return "Exp";
            case Type::GAMMA_DIST:   return "Gamma";
            case Type::BETA_DIST:    return "Beta";
            case Type::UNIFORM_DIST: return "Uniform";
            case Type::BINOMIAL:     return "Binom";
            case Type::POISSON:      return "Poisson";
            case Type::GEOMETRIC:    return "Geom";
            }
            return "Unknown";
        }

    private:
        // Newton-Raphson 求逆 CDF（通用后备）
        double newtonQuantile(double p) const {
            double x;
            // 初始猜测
            switch (type) {
            case Type::STUDENT_T:    x = prob::normalInvStd(p); break;
            case Type::CHI_SQUARED: {
                double df = params[0];
                if (df > 2) {
                    double z = prob::normalInvStd(p);
                    double t = 1.0 - 2.0 / (9 * df) + z * std::sqrt(2.0 / (9 * df));
                    x = df * t * t * t;
                    if (x <= 0) x = 0.01;
                }
                else { x = std::max(0.01, -2.0 * std::log(1.0 - p)); }
                break;
            }
            case Type::F_DIST: x = 1.0; break;
            case Type::GAMMA_DIST: x = params[0] / params[1]; break;
            case Type::BETA_DIST: x = 0.5; break;
            default: x = 0.0; break;
            }
            for (int i = 0; i < 100; ++i) {
                double fx = cdf(x) - p;
                double fp = pdf(x);
                if (std::abs(fp) < 1e-30) { x *= (fx < 0 ? 2.0 : 0.5); continue; }
                double dx = fx / fp;
                x -= dx;
                // 边界约束
                if (type == Type::CHI_SQUARED || type == Type::F_DIST || type == Type::GAMMA_DIST) {
                    if (x <= 0) x = 1e-6;
                }
                if (type == Type::BETA_DIST) {
                    if (x <= 0) x = 1e-6; if (x >= 1) x = 1.0 - 1e-6;
                }
                if (std::abs(dx) < 1e-14 * std::max(1.0, std::abs(x))) break;
            }
            return x;
        }
    };

    // =================================================================
    // 假设检验结果
    // =================================================================
    struct TestResult {
        double statistic;
        double pValue;
        double df;
        std::string name;
    };

    inline TestResult ttest1(const std::vector<double>& data, double mu0 = 0) {
        int n = static_cast<int>(data.size());
        if (n < 2) throw std::runtime_error("Math Error: t-test requires >= 2 data points.");
        double s = 0, sq = 0;
        for (double x : data) { s += x; sq += x * x; }
        double mean = s / n, var = (sq - s * s / n) / (n - 1);
        double se = std::sqrt(var / n);
        if (se < 1e-30) throw std::runtime_error("Math Error: Zero variance.");
        double t = (mean - mu0) / se, df = n - 1.0;
        Distribution td = Distribution::studentT(df);
        double p = 2.0 * std::min(td.cdf(t), 1.0 - td.cdf(t));
        return { t, p, df, "One-sample t-test" };
    }

    inline TestResult ttest2ind(const std::vector<double>& d1, const std::vector<double>& d2) {
        int n1 = static_cast<int>(d1.size()), n2 = static_cast<int>(d2.size());
        if (n1 < 2 || n2 < 2) throw std::runtime_error("Math Error: t-test requires >= 2 per group.");
        double s1 = 0, q1 = 0, s2 = 0, q2 = 0;
        for (double x : d1) { s1 += x; q1 += x * x; }
        for (double x : d2) { s2 += x; q2 += x * x; }
        double m1 = s1 / n1, m2 = s2 / n2, v1 = (q1 - s1 * s1 / n1) / (n1 - 1), v2 = (q2 - s2 * s2 / n2) / (n2 - 1);
        double se = std::sqrt(v1 / n1 + v2 / n2);
        if (se < 1e-30) throw std::runtime_error("Math Error: Zero variance.");
        double t = (m1 - m2) / se, vn1 = v1 / n1, vn2 = v2 / n2;
        double df = (vn1 + vn2) * (vn1 + vn2) / (vn1 * vn1 / (n1 - 1) + vn2 * vn2 / (n2 - 1));
        Distribution td = Distribution::studentT(df);
        double p = 2.0 * std::min(td.cdf(t), 1.0 - td.cdf(t));
        return { t, p, df, "Welch two-sample t-test" };
    }

    inline TestResult ttestPaired(const std::vector<double>& d1, const std::vector<double>& d2) {
        if (d1.size() != d2.size()) throw std::runtime_error("Math Error: Paired test needs equal lengths.");
        std::vector<double> diff(d1.size());
        for (size_t i = 0; i < d1.size(); ++i) diff[i] = d1[i] - d2[i];
        auto r = ttest1(diff, 0); r.name = "Paired t-test"; return r;
    }

    inline TestResult chi2test(const std::vector<double>& obs, const std::vector<double>& exp) {
        if (obs.size() != exp.size()) throw std::runtime_error("Math Error: obs/exp size mismatch.");
        if (obs.size() < 2) throw std::runtime_error("Math Error: Need >= 2 categories.");
        double stat = 0;
        for (size_t i = 0; i < obs.size(); ++i) {
            if (exp[i] <= 0) throw std::runtime_error("Math Error: Expected must be positive.");
            double d = obs[i] - exp[i]; stat += d * d / exp[i];
        }
        double df = static_cast<double>(obs.size() - 1);
        Distribution cd = Distribution::chiSquared(df);
        double p = 1.0 - cd.cdf(stat);
        return { stat, p, df, "Chi-squared test" };
    }

} // namespace jc
#endif // JC2_PROBABILITY_H
