#ifndef JC2_GROEBNER_H
#define JC2_GROEBNER_H

#include "Symbolic.h"
#include <vector>
#include <string>
#include <cstdint>

namespace jc {

    // =================================================================
    // 多元多项式底层表示 (用于 Gröbner 基计算)
    // =================================================================

    // 单项式：变量 ID -> 指数 (例如 x^2 * z^3)
    struct Monomial {
        static constexpr size_t MAX_VARS = 8;
        std::pair<uint32_t, int> powers[MAX_VARS];
        size_t size = 0;

        bool isOne() const;
        int getDegree() const;
        
        // 字典序比较 (Lexicographic Order)
        bool operator<(const Monomial& other) const;
        bool operator==(const Monomial& other) const;
        
        Monomial multiply(const Monomial& other) const;
        bool divides(const Monomial& other) const;
        Monomial divide(const Monomial& other) const;
        Monomial lcm(const Monomial& other) const; // 最小公倍式
    };

    // 多项式的一项：系数 * 单项式
    struct Term {
        Fraction coeff;
        Monomial mono;
        Term(Fraction c, Monomial m) : coeff(std::move(c)), mono(std::move(m)) {}
    };

    // 多元多项式
    class MultiPoly {
    public:
        std::vector<Term> terms; // 始终按单项式降序排列

        MultiPoly() = default;
        explicit MultiPoly(const SymExpr& expr); // 从 AST 转换

        bool isZero() const;
        Term leadingTerm() const;
        void cleanAndSort(); // 合并同类项并按字典序降序排列
        void makePrimitive(); // 清除分母并提取整数 GCD 以控制系数膨胀

        MultiPoly operator+(const MultiPoly& other) const;
        MultiPoly operator-(const MultiPoly& other) const;
        MultiPoly operator*(const Term& term) const;
        MultiPoly operator*(const MultiPoly& other) const;
        MultiPoly exactDivide(const MultiPoly& divisor) const;

        SymExpr toSymExpr() const; // 转换回 AST

        static void clearRegistry(); // 清理变量字典，防止内存泄漏
    };

    // =================================================================
    // Gröbner 基核心算法
    // =================================================================

    MultiPoly sPolynomial(const MultiPoly& f, const MultiPoly& g);
    MultiPoly multivariateDivide(MultiPoly f, const std::vector<MultiPoly>& G);
    std::vector<MultiPoly> computeGroebnerBasis(const std::vector<MultiPoly>& generators);
    std::vector<MultiPoly> computeReducedGroebnerBasis(const std::vector<MultiPoly>& G);

} // namespace jc

#endif // JC2_GROEBNER_H
