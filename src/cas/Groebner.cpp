#include "Groebner.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace jc {

    namespace {
        struct VarRegistry {
            std::unordered_map<SymNode*, uint32_t> sigToId;
            std::vector<SymExpr> idToExpr;

            uint32_t getId(const SymExpr& expr) {
                SymNode* sig = expr.ptr;
                auto it = sigToId.find(sig);
                if (it != sigToId.end()) return it->second;
                uint32_t id = static_cast<uint32_t>(idToExpr.size());
                sigToId[sig] = id;
                idToExpr.push_back(expr);
                return id;
            }

            SymExpr getExpr(uint32_t id) const {
                return idToExpr[id];
            }

            void clear() {
                sigToId.clear();
                idToExpr.clear();
            }
        };
        thread_local VarRegistry g_varReg;
    }

    void MultiPoly::clearRegistry() {
        g_varReg.clear();
    }

    // --- Monomial 实现 ---

    bool Monomial::isOne() const { return size == 0; }

    int Monomial::getDegree() const {
        int deg = 0;
        for (size_t i = 0; i < size; ++i) deg += powers[i].second;
        return deg;
    }

    bool Monomial::operator<(const Monomial& other) const {
        size_t i1 = 0, i2 = 0;
        while (i1 < size && i2 < other.size) {
            if (powers[i1].first < other.powers[i2].first) return false;
            if (powers[i1].first > other.powers[i2].first) return true;
            if (powers[i1].second != other.powers[i2].second) return powers[i1].second < other.powers[i2].second;
            ++i1; ++i2;
        }
        return i1 == size && i2 < other.size;
    }

    bool Monomial::operator==(const Monomial& other) const {
        if (size != other.size) return false;
        for (size_t i = 0; i < size; ++i) {
            if (powers[i].first != other.powers[i].first || powers[i].second != other.powers[i].second) return false;
        }
        return true;
    }

    Monomial Monomial::multiply(const Monomial& other) const {
        Monomial res;
        size_t i1 = 0, i2 = 0;
        while (i1 < size && i2 < other.size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            if (powers[i1].first < other.powers[i2].first) {
                res.powers[res.size++] = powers[i1++];
            } else if (powers[i1].first > other.powers[i2].first) {
                res.powers[res.size++] = other.powers[i2++];
            } else {
                int sum = powers[i1].second + other.powers[i2].second;
                if (sum != 0) res.powers[res.size++] = {powers[i1].first, sum};
                ++i1; ++i2;
            }
        }
        while (i1 < size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            res.powers[res.size++] = powers[i1++];
        }
        while (i2 < other.size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            res.powers[res.size++] = other.powers[i2++];
        }
        return res;
    }

    bool Monomial::divides(const Monomial& other) const {
        size_t i1 = 0, i2 = 0;
        while (i1 < size && i2 < other.size) {
            if (powers[i1].first < other.powers[i2].first) return false;
            if (powers[i1].first > other.powers[i2].first) {
                ++i2;
            } else {
                if (powers[i1].second > other.powers[i2].second) return false;
                ++i1; ++i2;
            }
        }
        return i1 == size;
    }

    Monomial Monomial::divide(const Monomial& other) const {
        Monomial res;
        size_t i1 = 0, i2 = 0;
        while (i1 < size && i2 < other.size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            if (powers[i1].first < other.powers[i2].first) {
                res.powers[res.size++] = powers[i1++];
            } else if (powers[i1].first > other.powers[i2].first) {
                ++i2;
            } else {
                int diff = powers[i1].second - other.powers[i2].second;
                if (diff > 0) res.powers[res.size++] = {powers[i1].first, diff};
                ++i1; ++i2;
            }
        }
        while (i1 < size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            res.powers[res.size++] = powers[i1++];
        }
        return res;
    }

    Monomial Monomial::lcm(const Monomial& other) const {
        Monomial res;
        size_t i1 = 0, i2 = 0;
        while (i1 < size && i2 < other.size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            if (powers[i1].first < other.powers[i2].first) {
                res.powers[res.size++] = powers[i1++];
            } else if (powers[i1].first > other.powers[i2].first) {
                res.powers[res.size++] = other.powers[i2++];
            } else {
                res.powers[res.size++] = {powers[i1].first, std::max(powers[i1].second, other.powers[i2].second)};
                ++i1; ++i2;
            }
        }
        while (i1 < size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            res.powers[res.size++] = powers[i1++];
        }
        while (i2 < other.size) {
            if (res.size >= MAX_VARS) throw std::runtime_error("Groebner Error: Max variables exceeded.");
            res.powers[res.size++] = other.powers[i2++];
        }
        return res;
    }

    // --- MultiPoly 实现 ---

    MultiPoly::MultiPoly(const SymExpr& expr) {
        if (!expr.ptr) return;
        switch (expr.ptr->getType()) {
            case SymType::NUM: {
                if (!expr.isZero()) {
                    auto num = static_cast<SymNum*>(expr.ptr);
                    Fraction f(0);
                    if (std::holds_alternative<int32_t>(num->value)) f = Fraction(std::get<int32_t>(num->value));
                    else if (std::holds_alternative<BigInt>(num->value)) f = Fraction(std::get<BigInt>(num->value));
                    else if (std::holds_alternative<Fraction>(num->value)) f = std::get<Fraction>(num->value);
                    else {
                        Monomial m;
                        m.powers[m.size++] = {g_varReg.getId(expr), 1};
                        terms.emplace_back(Fraction(1), m);
                        break;
                    }
                    terms.emplace_back(f, Monomial());
                }
                break;
            }
            case SymType::VAR: {
                Monomial m;
                m.powers[m.size++] = {g_varReg.getId(expr), 1};
                terms.emplace_back(Fraction(1), m);
                break;
            }
            case SymType::ADD: {
                auto add = static_cast<SymAdd*>(expr.ptr);
                for (const auto& arg : add->args) {
                    *this = *this + MultiPoly(SymExpr(arg));
                }
                break;
            }
            case SymType::MUL: {
                auto mul = static_cast<SymMul*>(expr.ptr);
                MultiPoly res(SymExpr(BigInt(1)));
                for (const auto& arg : mul->args) {
                    res = res * MultiPoly(SymExpr(arg));
                }
                *this = res;
                break;
            }
            case SymType::POW: {
                auto powNode = static_cast<SymPow*>(expr.ptr);
                if (powNode->exp->getType() == SymType::NUM) {
                    auto [isInt, n] = extractExactInt(static_cast<SymNum*>(powNode->exp)->value);
                    if (isInt && n >= 0) {
                        MultiPoly base(SymExpr(powNode->base));
                        MultiPoly res(SymExpr(BigInt(1)));
                        for (int i = 0; i < n; ++i) res = res * base;
                        *this = res;
                        break;
                    }
                }
                Monomial m;
                m.powers[m.size++] = {g_varReg.getId(expr), 1};
                terms.emplace_back(Fraction(1), m);
                break;
            }
            default: {
                Monomial m;
                m.powers[m.size++] = {g_varReg.getId(expr), 1};
                terms.emplace_back(Fraction(1), m);
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
                if (!(t.coeff == Fraction(0))) cleaned.push_back(t);
            } else {
                cleaned.back().coeff = cleaned.back().coeff + t.coeff;
                if (cleaned.back().coeff == Fraction(0)) cleaned.pop_back();
            }
        }
        terms = std::move(cleaned);
    }

    void MultiPoly::makePrimitive() {
        if (isZero()) return;
        
        BigInt lcm_den(1);
        for (const auto& t : terms) {
            lcm_den = BigInt::lcm(lcm_den, t.coeff.getDen());
        }
        
        if (lcm_den > BigInt(1)) {
            for (auto& t : terms) {
                t.coeff = t.coeff * Fraction(lcm_den);
            }
        }
        
        BigInt gcd_num(0);
        bool first = true;
        for (const auto& t : terms) {
            BigInt v = t.coeff.getNum().abs();
            if (first) { gcd_num = v; first = false; }
            else gcd_num = BigInt::gcd(gcd_num, v);
        }
        
        if (!gcd_num.isZero() && gcd_num > BigInt(1)) {
            for (auto& t : terms) {
                t.coeff = t.coeff / Fraction(gcd_num);
            }
        }
        
        if (!terms.empty() && terms[0].coeff.getNum().isNegative()) {
            for (auto& t : terms) t.coeff = Fraction(0) - t.coeff;
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
            res.terms.emplace_back(Fraction(0) - t.coeff, t.mono);
        }
        res.cleanAndSort();
        return res;
    }

    MultiPoly MultiPoly::operator*(const Term& term) const {
        MultiPoly res;
        for (const auto& t : terms) {
            res.terms.emplace_back(t.coeff * term.coeff, t.mono.multiply(term.mono));
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
            Fraction c = divisor.terms[0].coeff;
            for (auto& t : res.terms) t.coeff = t.coeff / c;
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
            Fraction c = leadR.coeff / leadD.coeff;
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
            SymExpr termExpr(t.coeff);
            for (size_t i = 0; i < t.mono.size; ++i) {
                const auto& kv = t.mono.powers[i];
                SymExpr varExpr = g_varReg.getExpr(kv.first);
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
                            BigInt v = t.coeff.getNum().abs();
                            if (t.coeff.getDen() > BigInt(1)) {
                                gcd_val = BigInt(1);
                                return;
                            }
                            if (gcd_val.isZero()) gcd_val = v;
                            else gcd_val = BigInt::gcd(gcd_val, v);
                        }
                    };
                    updateGcd(p);
                    updateGcd(r);
                    
                    if (gcd_val > BigInt(1)) {
                        Fraction gcd_frac(gcd_val);
                        for (auto& t : p.terms) t.coeff = t.coeff / gcd_frac;
                        for (auto& t : r.terms) t.coeff = t.coeff / gcd_frac;
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
            auto itI = ltI.mono.powers;
            auto itJ = ltJ.mono.powers;
            size_t idxI = 0, idxJ = 0;
            while (idxI < ltI.mono.size && idxJ < ltJ.mono.size) {
                if (itI[idxI].first < itJ[idxJ].first) {
                    ++idxI;
                } else if (itI[idxI].first > itJ[idxJ].first) {
                    ++idxJ;
                } else {
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
