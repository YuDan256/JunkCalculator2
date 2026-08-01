#include "Groebner.h"
#include <algorithm>
#include <stdexcept>

namespace jc {

    // --- Monomial 实现 ---

    bool Monomial::isOne() const { return powers.empty(); }

    int Monomial::getDegree() const {
        int deg = 0;
        for (const auto& kv : powers) deg += kv.second;
        return deg;
    }

    bool Monomial::operator<(const Monomial& other) const {
        // 零内存分配的字典序比较 (Lexicographic Order)
        // 变量名大的优先级高 (例如 "x" > "_z")
        // std::map 默认是升序排列，所以我们使用 rbegin() 逆向遍历
        auto it1 = powers.rbegin();
        auto it2 = other.powers.rbegin();
        
        while (it1 != powers.rend() && it2 != other.powers.rend()) {
            if (it1->first > it2->first) {
                // it1 有一个优先级更高的变量，而 it2 没有 (即 it2 的该变量指数为 0)
                // 因为 it1 的指数 > 0，所以 p1 > p2，返回 false
                return false;
            } else if (it1->first < it2->first) {
                // it2 有一个优先级更高的变量
                // p1 (0) < p2 (> 0)，返回 true
                return true;
            } else {
                // 变量相同，比较指数
                if (it1->second != it2->second) {
                    return it1->second < it2->second;
                }
                ++it1;
                ++it2;
            }
        }
        
        if (it1 != powers.rend()) {
            // it1 还有剩余变量 (优先级较低)，it2 没有了
            return false; // p1 > 0, p2 = 0
        } else if (it2 != other.powers.rend()) {
            // it2 还有剩余变量
            return true; // p1 = 0, p2 > 0
        }
        
        return false;
    }

    bool Monomial::operator==(const Monomial& other) const {
        if (powers.size() != other.powers.size()) return false;
        auto it1 = powers.begin();
        auto it2 = other.powers.begin();
        while (it1 != powers.end()) {
            if (it1->first != it2->first || it1->second != it2->second) return false;
            ++it1; ++it2;
        }
        return true;
    }

    Monomial Monomial::multiply(const Monomial& other) const {
        Monomial res = *this;
        for (const auto& kv : other.powers) {
            res.powers[kv.first] += kv.second;
            if (res.powers[kv.first] == 0) res.powers.erase(kv.first);
        }
        return res;
    }

    bool Monomial::divides(const Monomial& other) const {
        for (const auto& kv : powers) {
            auto it = other.powers.find(kv.first);
            if (it == other.powers.end() || it->second < kv.second) return false;
        }
        return true;
    }

    Monomial Monomial::divide(const Monomial& other) const {
        Monomial res = *this;
        for (const auto& kv : other.powers) {
            res.powers[kv.first] -= kv.second;
            if (res.powers[kv.first] == 0) res.powers.erase(kv.first);
        }
        return res;
    }

    Monomial Monomial::lcm(const Monomial& other) const {
        Monomial res = *this;
        for (const auto& kv : other.powers) {
            res.powers[kv.first] = std::max(res.powers[kv.first], kv.second);
        }
        return res;
    }

    // --- MultiPoly 实现 ---

    MultiPoly::MultiPoly(const SymExpr& expr) {
        if (!expr.ptr) return;
        switch (expr.ptr->getType()) {
            case SymType::NUM: {
                if (!expr.isZero()) {
                    terms.emplace_back(expr, Monomial());
                }
                break;
            }
            case SymType::VAR: {
                Monomial m;
                m.powers[std::static_pointer_cast<SymVar>(expr.ptr)->name] = 1;
                terms.emplace_back(SymExpr(BigInt(1)), m);
                break;
            }
            case SymType::ADD: {
                auto add = std::static_pointer_cast<SymAdd>(expr.ptr);
                for (const auto& arg : add->args) {
                    *this = *this + MultiPoly(SymExpr(arg));
                }
                break;
            }
            case SymType::MUL: {
                auto mul = std::static_pointer_cast<SymMul>(expr.ptr);
                MultiPoly res(SymExpr(BigInt(1)));
                for (const auto& arg : mul->args) {
                    res = res * MultiPoly(SymExpr(arg));
                }
                *this = res;
                break;
            }
            case SymType::POW: {
                auto powNode = std::static_pointer_cast<SymPow>(expr.ptr);
                if (powNode->exp->getType() == SymType::NUM) {
                    auto [isInt, n] = extractExactInt(std::static_pointer_cast<SymNum>(powNode->exp)->value);
                    if (isInt && n >= 0) {
                        MultiPoly base(SymExpr(powNode->base));
                        MultiPoly res(SymExpr(BigInt(1)));
                        for (int i = 0; i < n; ++i) res = res * base;
                        *this = res;
                        break;
                    }
                }
                // 非多项式幂次，视为新变量
                Monomial m;
                m.powers[expr.toString()] = 1;
                terms.emplace_back(SymExpr(BigInt(1)), m);
                break;
            }
            default: {
                // 函数等其他节点，视为新变量
                Monomial m;
                m.powers[expr.toString()] = 1;
                terms.emplace_back(SymExpr(BigInt(1)), m);
                break;
            }
        }
        cleanAndSort();
    }

    bool MultiPoly::isZero() const { return terms.empty(); }

    Term MultiPoly::leadingTerm() const {
        if (isZero()) throw std::runtime_error("Zero polynomial has no leading term.");
        return terms.front();
    }

    void MultiPoly::cleanAndSort() {
        if (terms.empty()) return;
        std::sort(terms.begin(), terms.end(), [](const Term& a, const Term& b) {
            return b.mono < a.mono;
        });
        std::vector<Term> cleaned;
        for (const auto& t : terms) {
            if (cleaned.empty() || !(cleaned.back().mono == t.mono)) {
                if (!t.coeff.isZero()) cleaned.push_back(t);
            } else {
                cleaned.back().coeff = simplifyCore(cleaned.back().coeff + t.coeff);
                if (cleaned.back().coeff.isZero()) cleaned.pop_back();
            }
        }
        terms = std::move(cleaned);
    }

    void MultiPoly::makePrimitive() {
        if (isZero()) return;
        
        BigInt lcm_den(1);
        bool all_rational = true;
        
        for (const auto& t : terms) {
            if (t.coeff.ptr->getType() == SymType::NUM) {
                auto num = std::static_pointer_cast<SymNum>(t.coeff.ptr);
                if (std::holds_alternative<Fraction>(num->value)) {
                    lcm_den = BigInt::lcm(lcm_den, std::get<Fraction>(num->value).getDen());
                } else if (!std::holds_alternative<BigInt>(num->value)) {
                    all_rational = false;
                    break;
                }
            } else {
                all_rational = false;
                break;
            }
        }
        
        if (!all_rational) return;
        
        if (lcm_den > BigInt(1)) {
            SymExpr lcm_expr(lcm_den);
            for (auto& t : terms) {
                t.coeff = simplifyCore(t.coeff * lcm_expr);
            }
        }
        
        BigInt gcd_num(0);
        bool first = true;
        for (const auto& t : terms) {
            if (t.coeff.ptr->getType() == SymType::NUM) {
                auto num = std::static_pointer_cast<SymNum>(t.coeff.ptr);
                BigInt v(0);
                if (std::holds_alternative<BigInt>(num->value)) {
                    v = std::get<BigInt>(num->value);
                } else if (std::holds_alternative<Fraction>(num->value)) {
                    v = std::get<Fraction>(num->value).getNum();
                }
                if (v.isNegative()) v = -v;
                if (first) { gcd_num = v; first = false; }
                else gcd_num = BigInt::gcd(gcd_num, v);
            }
        }
        
        if (!gcd_num.isZero() && gcd_num > BigInt(1)) {
            SymExpr gcd_expr(gcd_num);
            for (auto& t : terms) {
                t.coeff = simplifyCore(t.coeff / gcd_expr);
            }
        }
        
        if (!terms.empty()) {
            if (terms[0].coeff.ptr->getType() == SymType::NUM) {
                auto num = std::static_pointer_cast<SymNum>(terms[0].coeff.ptr);
                bool is_neg = false;
                if (std::holds_alternative<BigInt>(num->value)) is_neg = std::get<BigInt>(num->value).isNegative();
                else if (std::holds_alternative<Fraction>(num->value)) is_neg = std::get<Fraction>(num->value).getNum().isNegative();
                
                if (is_neg) {
                    for (auto& t : terms) t.coeff = simplifyCore(-t.coeff);
                }
            }
        }
    }

    MultiPoly MultiPoly::operator+(const MultiPoly& other) const {
        MultiPoly res;
        res.terms = terms;
        res.terms.insert(res.terms.end(), other.terms.begin(), other.terms.end());
        res.cleanAndSort();
        return res;
    }

    MultiPoly MultiPoly::operator-(const MultiPoly& other) const {
        MultiPoly res;
        res.terms = terms;
        for (const auto& t : other.terms) {
            res.terms.emplace_back(simplifyCore(-t.coeff), t.mono);
        }
        res.cleanAndSort();
        return res;
    }

    MultiPoly MultiPoly::operator*(const Term& term) const {
        MultiPoly res;
        for (const auto& t : terms) {
            res.terms.emplace_back(simplifyCore(t.coeff * term.coeff), t.mono.multiply(term.mono));
        }
        res.cleanAndSort();
        return res;
    }

    MultiPoly MultiPoly::operator*(const MultiPoly& other) const {
        MultiPoly res;
        for (const auto& t : other.terms) {
            MultiPoly partial = (*this) * t;
            res.terms.insert(res.terms.end(), partial.terms.begin(), partial.terms.end());
        }
        res.cleanAndSort();
        return res;
    }

    MultiPoly MultiPoly::exactDivide(const MultiPoly& divisor) const {
        if (divisor.isZero()) throw std::runtime_error("Math Error: Division by zero in MultiPoly.");
        if (isZero()) return MultiPoly();
        
        if (divisor.terms.size() == 1 && divisor.terms[0].mono.isOne()) {
            MultiPoly res = *this;
            SymExpr c = divisor.terms[0].coeff;
            for (auto& t : res.terms) t.coeff = simplifyCore(t.coeff / c);
            res.cleanAndSort();
            return res;
        }

        MultiPoly q;
        MultiPoly r = *this;
        Term leadD = divisor.leadingTerm();

        int iter = 0;
        while (!r.isZero()) {
            if (++iter > 10000) return MultiPoly(bareissExactDiv(this->toSymExpr(), divisor.toSymExpr()));
            Term leadR = r.leadingTerm();
            
            if (!leadD.mono.divides(leadR.mono)) {
                return MultiPoly(bareissExactDiv(this->toSymExpr(), divisor.toSymExpr()));
            }
            
            Monomial m = leadR.mono.divide(leadD.mono);
            SymExpr c = simplifyCore(leadR.coeff / leadD.coeff);
            Term termQ(c, m);
            
            q.terms.push_back(termQ);
            r = r - (divisor * termQ);
        }
        return q;
    }

    SymExpr MultiPoly::toSymExpr() const {
        if (isZero()) return SymExpr(BigInt(0));
        SymExpr res(BigInt(0));
        for (const auto& t : terms) {
            SymExpr termExpr = t.coeff;
            for (const auto& kv : t.mono.powers) {
                SymExpr varExpr = SymExpr::makeVar(kv.first);
                if (kv.second > 1) {
                    termExpr = termExpr * (varExpr ^ SymExpr(BigInt(kv.second)));
                } else if (kv.second == 1) {
                    termExpr = termExpr * varExpr;
                }
            }
            res = res + termExpr;
        }
        return res;
    }

    // --- Gröbner 基算法 ---

    MultiPoly sPolynomial(const MultiPoly& f, const MultiPoly& g) {
        if (f.isZero() || g.isZero()) return MultiPoly();
        Term ltF = f.leadingTerm();
        Term ltG = g.leadingTerm();
        Monomial lcmMono = ltF.mono.lcm(ltG.mono);
        
        Term tF(ltG.coeff, lcmMono.divide(ltF.mono));
        Term tG(ltF.coeff, lcmMono.divide(ltG.mono));
        
        MultiPoly res = (f * tF) - (g * tG);
        res.makePrimitive();
        return res;
    }

    MultiPoly multivariateDivide(MultiPoly f, const std::vector<MultiPoly>& G) {
        MultiPoly p = f;
        MultiPoly r;
        
        while (!p.isZero()) {
            bool divisionOccurred = false;
            Term ltP = p.leadingTerm();
            
            for (const auto& g : G) {
                if (g.isZero()) continue;
                Term ltG = g.leadingTerm();
                if (ltG.mono.divides(ltP.mono)) {
                    Term multiplier_p(ltG.coeff, Monomial());
                    Term multiplier_g(ltP.coeff, ltP.mono.divide(ltG.mono));
                    
                    p = (p * multiplier_p) - (g * multiplier_g);
                    r = r * multiplier_p; // ★ 核心修复：同步放大余式，维持理想等价性
                    
                    // 提取 p 和 r 的公共内容 (Content) 以防止系数爆炸
                    BigInt gcd_val(0);
                    auto updateGcd = [&](const MultiPoly& poly) {
                        for (const auto& t : poly.terms) {
                            if (gcd_val == BigInt(1)) return;
                            if (t.coeff.ptr->getType() == SymType::NUM) {
                                auto num = std::static_pointer_cast<SymNum>(t.coeff.ptr);
                                BigInt v(0);
                                if (std::holds_alternative<BigInt>(num->value)) v = std::get<BigInt>(num->value);
                                else if (std::holds_alternative<Fraction>(num->value)) v = std::get<Fraction>(num->value).getNum();
                                if (v.isNegative()) v = -v;
                                if (gcd_val.isZero()) gcd_val = v;
                                else gcd_val = BigInt::gcd(gcd_val, v);
                            } else {
                                gcd_val = BigInt(1);
                                return;
                            }
                        }
                    };
                    updateGcd(p);
                    updateGcd(r);
                    
                    if (gcd_val > BigInt(1)) {
                        SymExpr gcd_expr(gcd_val);
                        for (auto& t : p.terms) t.coeff = simplifyCore(t.coeff / gcd_expr);
                        for (auto& t : r.terms) t.coeff = simplifyCore(t.coeff / gcd_expr);
                    }
                    
                    divisionOccurred = true;
                    break;
                }
            }
            
            if (!divisionOccurred) {
                r.terms.push_back(p.leadingTerm());
                MultiPoly leadPoly;
                leadPoly.terms.push_back(p.leadingTerm());
                p = p - leadPoly;
            }
        }
        r.cleanAndSort();
        r.makePrimitive();
        return r;
    }

    std::vector<MultiPoly> computeGroebnerBasis(const std::vector<MultiPoly>& generators) {
        std::vector<MultiPoly> G;
        for (const auto& g : generators) {
            if (!g.isZero()) {
                MultiPoly p = g;
                p.makePrimitive();
                G.push_back(p);
            }
        }
        
        // 存储临界对 (i, j)
        struct CriticalPair {
            size_t i, j;
            int lcmDegree;
            bool operator<(const CriticalPair& other) const {
                return lcmDegree > other.lcmDegree; // 降序排列，使得 degree 小的在 vector 尾部，方便 pop_back
            }
        };
        
        std::vector<CriticalPair> B;
        auto addPairs = [&](size_t k) {
            for (size_t i = 0; i < k; ++i) {
                Monomial lcmMono = G[i].leadingTerm().mono.lcm(G[k].leadingTerm().mono);
                B.push_back({i, k, lcmMono.getDegree()});
            }
            // 法向选择策略 (Normal Selection Strategy)：优先处理 LCM 次数小的对
            std::sort(B.begin(), B.end());
        };
        
        for (size_t i = 1; i < G.size(); ++i) {
            for (size_t j = 0; j < i; ++j) {
                Monomial lcmMono = G[j].leadingTerm().mono.lcm(G[i].leadingTerm().mono);
                B.push_back({j, i, lcmMono.getDegree()});
            }
        }
        std::sort(B.begin(), B.end());
        
        int max_iters = 1000; // 提高迭代上限，使用工作表和优化策略后效率大幅提升
        int iters = 0;
        
        while (!B.empty() && iters++ < max_iters) {
            auto pair = B.back();
            B.pop_back();
            size_t i = pair.i;
            size_t j = pair.j;
            
            // Buchberger 第一准则 (Buchberger's First Criterion): 
            // 如果首项互素，S-多项式必定约化为0，直接跳过
            Term ltI = G[i].leadingTerm();
            Term ltJ = G[j].leadingTerm();
            bool relativelyPrime = true;
            for (const auto& kv : ltI.mono.powers) {
                if (ltJ.mono.powers.count(kv.first) > 0) {
                    relativelyPrime = false;
                    break;
                }
            }
            if (relativelyPrime) continue;
            
            MultiPoly s = sPolynomial(G[i], G[j]);
            if (s.isZero()) continue;
            
            MultiPoly r = multivariateDivide(s, G);
            if (!r.isZero()) {
                size_t k = G.size();
                G.push_back(r);
                addPairs(k);
            }
        }
        return computeReducedGroebnerBasis(G);
    }

    std::vector<MultiPoly> computeReducedGroebnerBasis(const std::vector<MultiPoly>& G) {
        std::vector<MultiPoly> minimalG;
        for (size_t i = 0; i < G.size(); ++i) {
            bool redundant = false;
            for (size_t j = 0; j < G.size(); ++j) {
                if (i != j && G[j].leadingTerm().mono.divides(G[i].leadingTerm().mono)) {
                    redundant = true;
                    break;
                }
            }
            if (!redundant) minimalG.push_back(G[i]);
        }
        
        std::vector<MultiPoly> reducedG;
        for (size_t i = 0; i < minimalG.size(); ++i) {
            std::vector<MultiPoly> others;
            for (size_t j = 0; j < minimalG.size(); ++j) {
                if (i != j) others.push_back(minimalG[j]);
            }
            MultiPoly r = multivariateDivide(minimalG[i], others);
            if (!r.isZero()) {
                r.makePrimitive();
                reducedG.push_back(r);
            }
        }
        return reducedG;
    }

} // namespace jc
