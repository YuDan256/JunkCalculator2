// Symbolic.h
#ifndef JC2_SYMBOLIC_H
#define JC2_SYMBOLIC_H

#include "../math/BigInt.h"
#include "../math/Fraction.h"
#include "../math/Complex.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <variant>
#include <functional>
#include <type_traits>
#include <tuple>
#include <set>
#include <atomic>
#include <stdexcept>
#include "../vm/EngineInterrupt.h"

namespace jc {
    class Value;

    // ==========================================
    // 内部高精度数值载体 (隔离 VM)
    // ==========================================
    using CASVal = std::variant<double, BigInt, Fraction, int32_t>;

    bool isCasZero(const CASVal& v);
    bool isCasOne(const CASVal& v);
    std::string casToString(const CASVal& v);

    // ==========================================
    // 符号计算全局动态限制配置
    // ==========================================
    struct SymConfig {
        static inline int64_t maxExpandTerms = 2000;
        static inline int maxAstNodes = 30000;
        static inline int maxIterations = 200;
        static inline int maxDepth = 6;
        static inline int maxEigvecDim = 4;
        static inline bool debugIntegration = false;
    };

    // ==========================================
    // AST 节点定义
    // ==========================================
    enum class SymType { NUM, VAR, ADD, MUL, POW, FUNC };

    class SymNode {
    protected:
        mutable std::string cachedStr;
    public:
        uint64_t hashValue = 0;

        void* operator new(size_t size);
        void operator delete(void* ptr, size_t size);

        virtual ~SymNode() = default;
        virtual SymType getType() const = 0;
        
        std::string toString() const {
            if (cachedStr.empty()) cachedStr = computeString();
            return cachedStr;
        }
        virtual std::string computeString() const = 0;

        virtual bool equals(const SymNode* other) const = 0;

        virtual bool isZero() const { return false; }
        virtual bool isOne() const { return false; }
    };

    // ==========================================
    // 符号表达式代理类 (The Value Proxy)
    // ==========================================
    class SymExpr {
    public:
        SymNode* ptr;

        static SymNode* intern(SymNode* node);
        static void cleanupPool(); // 清理全局池中失效的弱引用

        SymExpr();
        explicit SymExpr(SymNode* p) : ptr(intern(p)) {}

        // 隐式升维构造
        SymExpr(double v);
        template<typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
        SymExpr(T v);
        SymExpr(const BigInt& v);
        SymExpr(const Fraction& v);
        SymExpr(const Complex& v);
        SymExpr(const CASVal& v);

        static SymExpr makeVar(const std::string& name);

        std::string toString() const;
        bool isZero() const;
        bool isOne() const;

        // ★ 修改：改成全局友元函数！这是 C++ 支持 1 + x 隐式转换的核心魔法
        friend SymExpr operator+(const SymExpr& a, const SymExpr& b);
        friend SymExpr operator-(const SymExpr& a, const SymExpr& b);
        friend SymExpr operator*(const SymExpr& a, const SymExpr& b);
        friend SymExpr operator/(const SymExpr& a, const SymExpr& b);
        friend SymExpr operator^(const SymExpr& a, const SymExpr& b);

        // 单目负号可以保留为成员
        SymExpr operator-() const;

        bool operator==(const SymExpr& other) const {
            return ptr == other.ptr;
        }
        bool operator!=(const SymExpr& other) const {
            return !(*this == other);
        }
    };

    // ★ 必须在类外提供全局声明，否则某些编译器无法在普通查找中找到这些运算符
    SymExpr operator+(const SymExpr& a, const SymExpr& b);
    SymExpr operator-(const SymExpr& a, const SymExpr& b);
    SymExpr operator*(const SymExpr& a, const SymExpr& b);
    SymExpr operator/(const SymExpr& a, const SymExpr& b);
    SymExpr operator^(const SymExpr& a, const SymExpr& b);

    // ==========================================
    // CAS 操作函数
    // ==========================================
    SymExpr expand_core(const SymExpr& expr, int64_t maxPowTerms = 0);
    SymExpr expand(const SymExpr& expr, int64_t maxPowTerms = 0);
    SymExpr subs(const SymExpr& expr, const std::string& var, const SymExpr& val);
    SymExpr evalFloat(const SymExpr& expr);   // 已有：全部转 double
    SymExpr evalValue(const SymExpr& expr);   // 新增：保留 Complex 等精确类型
    SymExpr diff(const SymExpr& expr, const std::string& var);

    int getAstNodeCount(const SymExpr& expr);
    int getAstComplexity(const SymExpr& expr);
    int getVarDepth(const SymExpr& expr, const std::string& var);
    int getTranscendentalWeight(const SymExpr& expr, const std::string& var);
    SymExpr contract(const SymExpr& expr);
    SymExpr simplify(const SymExpr& expr);
    SymExpr full_simplify(const SymExpr& expr);

    // 获取多项式最高次幂
    int getDegree(const SymExpr& expr, const std::string& var);
    // 多项式带余除法：返回 {商 (Quotient), 余数 (Remainder)}
    std::pair<SymExpr, SymExpr> polyDiv(const SymExpr& dividend, const SymExpr& divisor, const std::string& var);
    // 多项式伪除法：返回伪余数 (Pseudo-Remainder)
    SymExpr polyPseudoRem(const SymExpr& dividend, const SymExpr& divisor, const std::string& var);
    // 多项式最大公约数 (基于子结式余式序列)
    SymExpr polyGCD(const SymExpr& a, const SymExpr& b, const std::string& var);
    // 多项式扩展欧几里得算法 (EEA)
    std::tuple<SymExpr, SymExpr, SymExpr> polyEGCD(const SymExpr& a, const SymExpr& b, const std::string& var);
    // 多项式无平方分解 (Square-Free Factorization)
    std::vector<std::pair<SymExpr, int>> polySquareFree(const SymExpr& p, const std::string& var);
    // 多项式结式 (Sylvester Resultant)
    SymExpr polyResultant(const SymExpr& a, const SymExpr& b, const std::string& var);

    // 代数重写引擎 (Pattern Matching Engine)
    SymExpr applyRule(const SymExpr& expr, const SymExpr& pattern, const SymExpr& target);

    // 符号方程求解
    std::vector<SymExpr> solveEq(const SymExpr& expr, const std::string& var);
    std::vector<SymExpr> getExactRoots(const std::vector<SymExpr>& coeffs);

    // ==========================================
    // 🚀 下一步发展路线图 (Roadmap) 接口预留
    // ==========================================
    SymExpr limit(const SymExpr& expr, const std::string& var, const SymExpr& val, const std::string& dir = "");
    SymExpr trigsimp(const SymExpr& expr);
    SymExpr taylor(const SymExpr& expr, const std::string& var, const SymExpr& a, int order); // 泰勒展开
    SymExpr rischNormalize(const SymExpr& expr); // 刘维尔域规范化 (Risch 算法前置)

    // 内部工具暴露给 Integration 模块
    std::pair<bool, int64_t> extractExactInt(const CASVal& cval);
    bool isCasNegative(const CASVal& v);
    void collectAllVars(SymNode* node, std::set<std::string>& vars);
    std::pair<bool, SymExpr> trySquareRoot(const SymExpr& expr, bool allowPartial = false);
    bool containsVar(SymNode* node, const std::string& var);
    bool isPolynomialIn(const SymExpr& expr, const std::string& var);
    bool matchAST(SymNode* node, SymNode* pat, std::map<std::string, SymExpr>& captures);
    SymExpr substituteCaptures(const SymExpr& target, const std::map<std::string, SymExpr>& captures);
    SymExpr simplifyCore(const SymExpr& expr);
    std::vector<SymExpr> extractCoeffs(const SymExpr& expr, const std::string& var);
    std::pair<SymExpr, SymExpr> getFraction(const SymExpr& expr);
    SymExpr rationalizeDenominator(const SymExpr& expr);
    SymExpr simplifyRational(const SymExpr& expr);
    SymExpr bareissExactDiv(const SymExpr& dividend, const SymExpr& divisor);

    // ==========================================
    // 派生数学节点
    // ==========================================

    class SymNum : public SymNode {
    public:
        CASVal value;
        explicit SymNum(CASVal v);
        SymType getType() const override { return SymType::NUM; }
        std::string computeString() const override { return casToString(value); }
        bool isZero() const override { return isCasZero(value); }
        bool isOne() const override { return isCasOne(value); }
        bool equals(const SymNode* other) const override;
    };

    class SymVar : public SymNode {
    public:
        std::string name;
        explicit SymVar(std::string n);
        SymType getType() const override { return SymType::VAR; }
        std::string computeString() const override { return name; }
        bool equals(const SymNode* other) const override;
    };

    class SymAdd : public SymNode {
    public:
        std::vector<SymNode*> args;
        explicit SymAdd(std::vector<SymNode*> a);
        SymType getType() const override { return SymType::ADD; }
        std::string computeString() const override;
        bool equals(const SymNode* other) const override;
    };

    class SymMul : public SymNode {
    public:
        std::vector<SymNode*> args;
        explicit SymMul(std::vector<SymNode*> a);
        SymType getType() const override { return SymType::MUL; }
        std::string computeString() const override;
        bool equals(const SymNode* other) const override;
    };

    class SymPow : public SymNode {
    public:
        SymNode* base;
        SymNode* exp;
        SymPow(SymNode* b, SymNode* e);
        SymType getType() const override { return SymType::POW; }
        std::string computeString() const override;
        bool equals(const SymNode* other) const override;
    };

    class SymFunc : public SymNode {
    public:
        std::string name;
        std::vector<SymNode*> args;
        SymFunc(std::string n, std::vector<SymNode*> a);
        SymType getType() const override { return SymType::FUNC; }
        std::string computeString() const override;
        bool equals(const SymNode* other) const override;
    };

    // 模板构造函数实现 (必须在 SymNum 完整定义之后)
    template<typename T, std::enable_if_t<std::is_integral_v<T>, int>>
    SymExpr::SymExpr(T v) {
        int64_t val = static_cast<int64_t>(v);
        if (val >= -2147483648LL && val <= 2147483647LL) {
            ptr = intern(new SymNum(static_cast<int32_t>(val)));
        } else {
            ptr = intern(new SymNum(BigInt(val)));
        }
    }

} // namespace jc

#endif // JC2_SYMBOLIC_H
