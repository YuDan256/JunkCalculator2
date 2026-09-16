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
    // CONST = 数学常量节点（pi / e / i）。常量不是普通变量：它们有自己的类型，
    // 因此用户定义的同名符号（sym("PI") / sym("E") / sym("i")）不会被常量逻辑
    // 当成常数 —— 那些是自由变量，与常量节点互不冒充。
    enum class SymType { NUM, VAR, ADD, MUL, POW, FUNC, CONST };

    // ★ 符号常量表：单一事实来源（身份 / 名称 / 显示 / 数值）
    //   新增常量只需在此加一行，辨识、求值、打印三处自动跟随。
    enum class SymConstId { Pi = 0, E = 1, I = 2 };

    struct SymConstDef {
        SymConstId id;
        const char* name;       // 规范化名称（唯一身份）
        const char* display;    // 打印形式
        double value;
    };

    inline constexpr SymConstDef kSymConstDefs[] = {
        { SymConstId::Pi, "pi", "pi", 3.14159265358979323846 },
        { SymConstId::E,  "e",  "e",  2.71828182845904523536 },
        { SymConstId::I,  "i",  "i",  0.0 },
    };

    // 按名称查常量（仅规范名 pi / e / i）；找不到返回 false。
    // ★ 内部需要 π 请直写 makeConst(SymConstId::Pi)，不要用 makeVar("PI")：
    //   makeVar 会把常量名转义成自由变量（SymRules.h 曾因此吐出 sqrt(PI)）。
    bool lookupSymbolicConstant(const std::string& name, SymConstId& outId);

    // 用户符号与常量规范名冲名时的转义（sym("e") 不应变成常量 e）
    std::string escapeConstVarName(const std::string& name);
    std::string unescapeConstVarName(const std::string& name);

    // 常量的数值（pi/e 为实数，i 为虚数单位）。★ 只提供数，不把复数塞进符号节点。
    Complex symbolicConstantValue(SymConstId id);

    // 该名称是否代表某个符号常量
    inline bool isSymbolicConstantName(const std::string& name) {
        SymConstId id;
        return lookupSymbolicConstant(name, id);
    }

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
    class SymExpr;

    // 结构相等，必要时回退到展开后的规范多项式比较（见 SymExpr::operator==）
    bool symEquivalent(const SymExpr& a, const SymExpr& b);

    // factor 记忆化（定义在 Factorization.cpp）：纯函数的重复调用占了大头，
    // 清空时机由顶层 full_simplify 掌握，见那里的说明。
    void clearFactorMemo();

    class SymExpr {
    public:
        SymNode* ptr;

        static SymNode* intern(SymNode* node);
        static void cleanupPool(); // 清理全局池中失效的弱引用

    private:
        struct InternedTag {};
        SymExpr(SymNode* p, InternedTag) : ptr(p) {}
    public:
        static SymExpr fromInterned(SymNode* p) { return SymExpr(p, InternedTag{}); }

        static SymExpr makeNum(CASVal v);
        static SymExpr makeVar(const std::string& name);
        static SymExpr makeAdd(std::vector<SymNode*> args);
        static SymExpr makeMul(std::vector<SymNode*> args);
        static SymExpr makePow(SymNode* base, SymNode* exp);
        static SymExpr makeFunc(std::string name, std::vector<SymNode*> args);
        static SymExpr makeConst(SymConstId id);

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

        // ★ 结构相等 + 代数回退。
        //   曾经这里只比较 ptr（指针/内部化身份），于是"展开后同一个多项式"也判不等：
        //   expand((x+1)*(x-1)) 与 expand(x^2-1) 都打印 x^2 - 1，却 a == b 为 false。
        //   节点自身已实现 equals()（结构比较），这里改为调用它；纯结构不等时再回退到
        //   "展开成规范多项式后比较"，以覆盖结合律/交换律/分配律造成的等价写法。
        bool operator==(const SymExpr& other) const {
            if (ptr == other.ptr) return true;
            return symEquivalent(*this, other);
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

    // ==========================================
    // 三角 ↔ 复指数 双向重写
    // ==========================================
    // 以前这两个函数是 Integration.cpp 里的 file-static，只有积分引擎内部能用，
    // 于是"积分器交给你的复指数结果"在 cas.simplify 眼里就是一串互不相干的
    // exp(...)：exp(i*x) + exp(-i*x) 永远合不成 2*cos(x)。提到公共层后
    // full_simplify 也能用它们做候选，两条路径共用同一份实现。
    SymExpr trigToExp(const SymExpr& expr);
    SymExpr expToTrig(const SymExpr& expr);

    // 复指数规范形：把表达式按虚数单位 i 拆成实部 + 虚部*I，各自化简后再合起来。
    // expToTrig 只套欧拉公式（exp(a+bi) → exp(a)*(cos b + i*sin b)），本身不合并；
    // 真正让共轭对坍缩成实三角函数的是这一步的实虚归并。
    SymExpr simplifyComplex(const SymExpr& expr);

    // 表达式里是否出现"指数含虚数单位"的 exp()（即复指数）。
    // full_simplify 用它当闸门：不含复指数时跳过 expToTrig 候选，免去白跑。
    bool hasComplexExponential(SymNode* node, int depth = 0);

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
        std::string computeString() const override;
        bool equals(const SymNode* other) const override;
    };

    // ★ 数学常量节点（pi / e / i）。与 SymVar 彻底区分：常量有自己的类型，
    //   所以变量检查（SymType::VAR）不会把它当变量，用户同名符号也无法冒充它。
    class SymConst : public SymNode {
    public:
        SymConstId id;
        explicit SymConst(SymConstId i);
        SymType getType() const override { return SymType::CONST; }
        std::string computeString() const override;
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
