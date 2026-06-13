#include "Factorization.h"
#include <vector>
#include <set>
#include <algorithm>
#include <stdexcept>
#include <functional>
#include <map>

namespace jc {

    // =================================================================
    // 全域多元全次因式分解 (Multivariate Polynomial Factorization)
    // 包含: 二次求解 + N次完全平方式提取
    // =================================================================
    static SymExpr multivariatePolynomialFactor(const SymExpr& expr, int depth) {
        if (depth > SymConfig::maxDepth / 2) return expr; // 极限深度保护，防止复杂多元死锁

        std::set<std::string> vars;
        collectAllVars(expr.ptr, vars);
        if (vars.empty()) return expr;

        for (const std::string& mainVar : vars) {
            auto coeffs = extractCoeffs(expr, mainVar);
            if (coeffs.empty()) continue;

            int N = static_cast<int>(coeffs.size()) - 1;
            if (N < 2) continue;

            SymExpr lead = coeffs[N];
            if (lead.isZero()) continue;

            SymExpr X = SymExpr::makeVar(mainVar);

            // =======================================================
            // 策略 1：N次完全平方式嗅探
            // =======================================================
            SymExpr nextCoeff = coeffs[N - 1];
            SymExpr R = simplifyCore(nextCoeff / (SymExpr(BigInt(N)) * lead));
            SymExpr rawCandidate = simplifyCore(lead * ((X + R) ^ SymExpr(BigInt(N))));

            SymExpr checkZero;
            try { checkZero = simplifyCore(expr - rawCandidate); }
            catch (...) { checkZero = SymExpr(BigInt(1)); }
            try { checkZero = simplifyCore(expand_core(checkZero, SymConfig::maxExpandTerms)); }
            catch (...) {}

            if (checkZero.isZero()) {
                if (N == 2) {
                    auto [sqrtOk, rootLead] = trySquareRoot(lead);
                    if (sqrtOk) {
                        return simplifyCore((rootLead * X + simplifyCore(rootLead * R)) ^ SymExpr(BigInt(2)));
                    }
                }
                return rawCandidate;
            }

            // =======================================================
            // 策略 2：二次方程降维打击 (极简优雅版，不再胡乱加补丁)
            // =======================================================
            if (N == 2) {
                SymExpr C = coeffs[0];
                SymExpr B = coeffs[1];
                SymExpr A = coeffs[2];

                SymExpr delta = B * B - SymExpr(BigInt(4)) * A * C;
                // 让化简引擎算出判别式的多项式，然后扔给 factor
                SymExpr simpDelta = simplifyCore(delta);
                SymExpr factoredDelta = factor(simpDelta, depth + 1);

                auto [sqrtOk, sqrtDelta] = trySquareRoot(factoredDelta);
                if (!sqrtOk) continue;

                // 依靠系统底层卓越的 operator* 和 operator/ 来负责数学相消！
                SymExpr twoA = SymExpr(BigInt(2)) * A;
                SymExpr num1 = factor(simplifyCore(-B + sqrtDelta), depth + 1);
                SymExpr num2 = factor(simplifyCore(-B - sqrtDelta), depth + 1);
                SymExpr factTwoA = factor(twoA, depth + 1);

                // 分子分母分别 factor 后相除，底层会自动合并同底数幂，避免 full_simplify 循环引用
                SymExpr r1 = simplifyCore(num1 / factTwoA);
                SymExpr r2 = simplifyCore(num2 / factTwoA);

                if (A.isOne()) return (X - r1) * (X - r2);
                return A * (X - r1) * (X - r2);
            }
        }
        return expr;
    }

    // =================================================================
    // 有限域多项式运算与 Cantor-Zassenhaus 因式分解
    // =================================================================
    using PolyZp = std::vector<BigInt>;

    static BigInt centerModP(BigInt a, const BigInt& p) {
        BigInt r = a % p;
        if (r.isNegative()) r = r + p;
        BigInt half = p / BigInt(2);
        if (r > half) r = r - p;
        return r;
    }

    static BigInt invModP(BigInt a, const BigInt& p) {
        BigInt t(0), newt(1);
        BigInt r = p, newr = a % p;
        if (newr.isNegative()) newr = newr + p;
        while (!newr.isZero()) {
            BigInt quotient = r / newr;
            BigInt temp_t = t - quotient * newt;
            t = newt; newt = temp_t;
            BigInt temp_r = r - quotient * newr;
            r = newr; newr = temp_r;
        }
        if (r > BigInt(1)) throw std::runtime_error("Math Error: Not invertible in Zp");
        if (t.isNegative()) {
            t = t % p;
            if (t.isNegative()) t = t + p;
        }
        return t;
    }

    static void trimP(PolyZp& a) {
        while (!a.empty() && a.back().isZero()) a.pop_back();
    }

    static PolyZp subP(const PolyZp& a, const PolyZp& b, const BigInt& p) {
        PolyZp res(std::max(a.size(), b.size()), BigInt(0));
        for (size_t i = 0; i < res.size(); ++i) {
            BigInt va = i < a.size() ? a[i] : BigInt(0);
            BigInt vb = i < b.size() ? b[i] : BigInt(0);
            BigInt diff = (va - vb) % p;
            if (diff.isNegative()) diff = diff + p;
            res[i] = diff;
        }
        trimP(res);
        return res;
    }

    static PolyZp mulP(const PolyZp& a, const PolyZp& b, const BigInt& p) {
        if (a.empty() || b.empty()) return {};
        PolyZp res(a.size() + b.size() - 1, BigInt(0));
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].isZero()) continue;
            for (size_t j = 0; j < b.size(); ++j) {
                BigInt term = (a[i] * b[j]) % p;
                res[i+j] = (res[i+j] + term) % p;
            }
        }
        for (auto& x : res) {
            if (x.isNegative()) x = x + p;
        }
        trimP(res);
        return res;
    }

    static std::pair<PolyZp, PolyZp> divP(PolyZp a, const PolyZp& b, const BigInt& p) {
        if (b.empty()) throw std::runtime_error("Math Error: Division by zero poly in Zp");
        trimP(a);
        if (a.size() < b.size()) return {{}, a};
        PolyZp q(a.size() - b.size() + 1, BigInt(0));
        BigInt invLead = invModP(b.back(), p);
        for (int i = (int)a.size() - (int)b.size(); i >= 0; --i) {
            if (a[i + b.size() - 1].isZero()) continue;
            BigInt factor = (a[i + b.size() - 1] * invLead) % p;
            if (factor.isNegative()) factor = factor + p;
            q[i] = factor;
            for (size_t j = 0; j < b.size(); ++j) {
                BigInt term = (factor * b[j]) % p;
                a[i+j] = (a[i+j] - term) % p;
                if (a[i+j].isNegative()) a[i+j] = a[i+j] + p;
            }
        }
        trimP(q);
        trimP(a);
        return {q, a};
    }

    static PolyZp gcdP(PolyZp a, PolyZp b, const BigInt& p) {
        while (!b.empty()) {
            auto [q, r] = divP(a, b, p);
            a = b;
            b = r;
        }
        if (!a.empty()) {
            BigInt invL = invModP(a.back(), p);
            for (auto& x : a) {
                x = (x * invL) % p;
                if (x.isNegative()) x = x + p;
            }
        }
        return a;
    }

    static PolyZp powModP(PolyZp base, BigInt exp, const PolyZp& modPoly, const BigInt& p) {
        PolyZp res = {BigInt(1)};
        base = divP(base, modPoly, p).second;
        while (!exp.isZero()) {
            BigInt rem = exp % BigInt(2);
            if (!rem.isZero()) {
                res = divP(mulP(res, base, p), modPoly, p).second;
            }
            base = divP(mulP(base, base, p), modPoly, p).second;
            exp = exp / BigInt(2);
        }
        return res;
    }

    static PolyZp derivP(const PolyZp& a, const BigInt& p) {
        if (a.size() <= 1) return {};
        PolyZp res(a.size() - 1, BigInt(0));
        for (size_t i = 1; i < a.size(); ++i) {
            res[i-1] = (a[i] * BigInt(i)) % p;
            if (res[i-1].isNegative()) res[i-1] = res[i-1] + p;
        }
        trimP(res);
        return res;
    }

    static std::vector<PolyZp> czEDF(const PolyZp& f, int d, const BigInt& p) {
        int n = static_cast<int>(f.size()) - 1;
        if (n == d) return {f};
        std::vector<PolyZp> factors = {f};
        
        int seed = 1;
        auto randPoly = [&](int deg) {
            PolyZp r(deg, BigInt(0));
            for (int i = 0; i < deg; ++i) {
                seed = (seed * 1103515245 + 12345) & 0x7fffffff;
                r[i] = BigInt(seed) % p;
            }
            trimP(r);
            return r;
        };

        BigInt exp = p;
        for (int i = 1; i < d; ++i) exp = exp * p;
        exp = (exp - BigInt(1)) / BigInt(2);

        while (factors.size() < (size_t)(n / d)) {
            PolyZp a = randPoly(n);
            PolyZp b = powModP(a, exp, f, p);
            b = subP(b, {BigInt(1)}, p);
            
            std::vector<PolyZp> nextFactors;
            for (const auto& u : factors) {
                if ((int)u.size() - 1 == d) {
                    nextFactors.push_back(u);
                    continue;
                }
                PolyZp g = gcdP(b, u, p);
                if (!g.empty() && g.size() > 1 && g.size() < u.size()) {
                    nextFactors.push_back(g);
                    nextFactors.push_back(divP(u, g, p).first);
                } else {
                    nextFactors.push_back(u);
                }
            }
            factors = nextFactors;
        }
        return factors;
    }

    static std::vector<PolyZp> cantorZassenhaus(const PolyZp& f, const BigInt& p) {
        std::vector<PolyZp> factors;
        PolyZp f_star = f;
        PolyZp h = {BigInt(0), BigInt(1)}; // x
        int d = 1;
        
        while ((int)f_star.size() - 1 >= 2 * d) {
            h = powModP(h, p, f_star, p);
            PolyZp h_minus_x = subP(h, {BigInt(0), BigInt(1)}, p);
            PolyZp g = gcdP(h_minus_x, f_star, p);
            
            if (g.size() > 1) {
                auto edfFactors = czEDF(g, d, p);
                for (auto& fact : edfFactors) factors.push_back(fact);
                f_star = divP(f_star, g, p).first;
                h = divP(h, f_star, p).second;
            }
            d++;
        }
        if (f_star.size() > 1) {
            factors.push_back(f_star);
        }
        return factors;
    }

    static SymExpr factorPolynomialCZ(const SymExpr& expr, int depth) {
        if (depth > SymConfig::maxDepth / 2) return expr;
        std::set<std::string> vars;
        collectAllVars(expr.ptr, vars);
        if (vars.size() != 1) return expr;
        
        std::string var = *vars.begin();
        auto sqFree = polySquareFree(expr, var);
        if (sqFree.empty()) return expr;
        
        SymExpr result(BigInt(1));
        bool changed = false;
        if (sqFree.size() > 1 || sqFree[0].second > 1 || sqFree[0].first != expr) {
            changed = true;
        }
        
        for (const auto& [part, power] : sqFree) {
            if (part.ptr->getType() == SymType::NUM) {
                result = result * (part ^ SymExpr(BigInt(power)));
                continue;
            }
            
            auto coeffs = extractCoeffs(part, var);
            if (coeffs.size() <= 2) {
                result = result * (part ^ SymExpr(BigInt(power)));
                continue;
            }
            
            std::vector<BigInt> intCoeffs;
            bool allInt = true;
            for (const auto& c : coeffs) {
                if (c.ptr->getType() == SymType::NUM) {
                    auto numVal = std::static_pointer_cast<SymNum>(c.ptr)->value;
                    if (std::holds_alternative<BigInt>(numVal)) {
                        intCoeffs.push_back(std::get<BigInt>(numVal));
                    } else if (std::holds_alternative<Fraction>(numVal) && std::get<Fraction>(numVal).getDen() == BigInt(1)) {
                        intCoeffs.push_back(std::get<Fraction>(numVal).getNum());
                    } else if (std::holds_alternative<double>(numVal)) {
                        double d = std::get<double>(numVal);
                        if (d == std::round(d)) intCoeffs.push_back(BigInt(static_cast<int64_t>(std::round(d))));
                        else { allInt = false; break; }
                    } else {
                        allInt = false; break;
                    }
                } else {
                    allInt = false; break;
                }
            }
            
            if (!allInt) {
                result = result * (part ^ SymExpr(BigInt(power)));
                continue;
            }
            
            BigInt content(0);
            for (const auto& c : intCoeffs) {
                if (!c.isZero()) content = BigInt::gcd(content, c.abs());
            }
            if (content > BigInt(1)) {
                for (auto& c : intCoeffs) c = c / content;
            }
            
            int n = static_cast<int>(intCoeffs.size()) - 1;
            BigInt an = intCoeffs.back();
            
            // 转换为首一多项式 g(y) = a_n^{n-1} f(y/a_n)
            std::vector<BigInt> g_coeffs(n + 1);
            BigInt an_pow(1);
            g_coeffs[n] = BigInt(1);
            for (int i = n - 1; i >= 0; --i) {
                g_coeffs[i] = intCoeffs[i] * an_pow;
                an_pow = an_pow * an;
            }
            
            // 计算 Mignotte 边界 B = 2^n * sum(|g_i|)
            BigInt sumAbs(0);
            for (const auto& c : g_coeffs) sumAbs = sumAbs + c.abs();
            BigInt B = sumAbs;
            for (int i = 0; i < n; ++i) B = B * BigInt(2);
            
            // 选取大素数 p > 2B
            BigInt p = (B * BigInt(2)).nextPrime();
            
            PolyZp g_mod;
            while (true) {
                g_mod = g_coeffs;
                for (auto& x : g_mod) {
                    x = x % p;
                    if (x.isNegative()) x = x + p;
                }
                trimP(g_mod);
                PolyZp g_deriv = derivP(g_mod, p);
                PolyZp gcd = gcdP(g_mod, g_deriv, p);
                if (gcd.size() <= 1) break; // 确保在 Zp 上无平方
                p = p.nextPrime();
            }
            
            std::vector<PolyZp> factorsZp = cantorZassenhaus(g_mod, p);
            
            std::vector<std::vector<BigInt>> trueFactorsZ;
            std::vector<PolyZp> currentFactors = factorsZp;
            std::vector<BigInt> currentG = g_coeffs;
            
            int r_factors = static_cast<int>(currentFactors.size());
            for (int d = 1; d <= r_factors / 2; ) {
                bool found = false;
                std::vector<char> bitmask(d, 1);
                bitmask.resize(r_factors, 0);
                
                do {
                    std::vector<int> indices;
                    for (int i = 0; i < r_factors; ++i) {
                        if (bitmask[i]) indices.push_back(i);
                    }
                    
                    PolyZp prodZp = {BigInt(1)};
                    for (int idx : indices) prodZp = mulP(prodZp, currentFactors[idx], p);
                    
                    std::vector<BigInt> candZ = prodZp;
                    for (auto& x : candZ) x = centerModP(x, p);
                    
                    std::vector<BigInt> a = currentG;
                    std::vector<BigInt> b = candZ;
                    bool exact = true;
                    std::vector<BigInt> q;
                    
                    if (b.empty() || a.size() < b.size()) {
                        exact = false;
                    } else {
                        q.resize(a.size() - b.size() + 1, BigInt(0));
                        BigInt leadB = b.back();
                        
                        for (int i = (int)a.size() - (int)b.size(); i >= 0; --i) {
                            if (a[i + b.size() - 1].isZero()) continue;
                            BigInt quot = a[i + b.size() - 1] / leadB;
                            BigInt rem = a[i + b.size() - 1] % leadB;
                            if (!rem.isZero()) { exact = false; break; }
                            q[i] = quot;
                            for (size_t j = 0; j < b.size(); ++j) {
                                a[i+j] = a[i+j] - quot * b[j];
                            }
                        }
                        trimP(a);
                    }
                    
                    if (exact && a.empty()) {
                        trueFactorsZ.push_back(candZ);
                        currentG = q;
                        std::vector<PolyZp> nextFactors;
                        for (int i = 0; i < r_factors; ++i) {
                            if (!bitmask[i]) nextFactors.push_back(currentFactors[i]);
                        }
                        currentFactors = nextFactors;
                        r_factors = static_cast<int>(currentFactors.size());
                        found = true;
                        break;
                    }
                } while (std::prev_permutation(bitmask.begin(), bitmask.end()));
                
                if (!found) {
                    d++;
                }
            }
            
            if (currentG.size() > 1) {
                trueFactorsZ.push_back(currentG);
            }
            
            if (trueFactorsZ.size() == 1) {
                result = result * (part ^ SymExpr(BigInt(power)));
                continue;
            }
            
            changed = true;
            SymExpr partResult(BigInt(1));
            
            for (const auto& H_coeffs : trueFactorsZ) {
                // 还原代换 y = a_n x
                std::vector<BigInt> Hx_coeffs(H_coeffs.size());
                BigInt an_i(1);
                BigInt Hx_content(0);
                for (size_t i = 0; i < H_coeffs.size(); ++i) {
                    Hx_coeffs[i] = H_coeffs[i] * an_i;
                    if (!Hx_coeffs[i].isZero()) Hx_content = BigInt::gcd(Hx_content, Hx_coeffs[i].abs());
                    an_i = an_i * an;
                }
                
                SymExpr factorExpr(BigInt(0));
                SymExpr X = SymExpr::makeVar(var);
                for (size_t i = 0; i < Hx_coeffs.size(); ++i) {
                    if (Hx_coeffs[i].isZero()) continue;
                    BigInt c = Hx_coeffs[i] / Hx_content;
                    if (i == 0) factorExpr = factorExpr + SymExpr(c);
                    else if (i == 1) factorExpr = factorExpr + SymExpr(c) * X;
                    else factorExpr = factorExpr + SymExpr(c) * (X ^ SymExpr(BigInt(i)));
                }
                
                partResult = partResult * factorExpr;
            }
            
            if (content > BigInt(1)) partResult = SymExpr(content) * partResult;
            
            result = result * (partResult ^ SymExpr(BigInt(power)));
        }
        
        return changed ? result : expr;
    }

    // =================================================================
    // 因式分解主入口
    // =================================================================
    SymExpr factor(const SymExpr& expr, int depth) {      // 增加 depth 签名
        if (!expr.ptr || depth > SymConfig::maxDepth) return expr;          // 极限保险
        SymExpr quadResult = multivariatePolynomialFactor(expr, depth);  // 接入 depth
        if (quadResult.ptr != expr.ptr) return quadResult;
        
        SymExpr czResult = factorPolynomialCZ(expr, depth);
        if (czResult.ptr != expr.ptr) return czResult;
        // ══════════════════════════════════════════
        // 递归处理非加法节点
        // ══════════════════════════════════════════
        if (expr.ptr->getType() == SymType::MUL) {
            auto mul = std::static_pointer_cast<SymMul>(expr.ptr);
            SymExpr res(BigInt(1));
            for (auto& arg : mul->args) res = res * factor(SymExpr(arg));
            return res;
        }
        if (expr.ptr->getType() == SymType::POW) {
            auto p = std::static_pointer_cast<SymPow>(expr.ptr);
            return factor(SymExpr(p->base)) ^ factor(SymExpr(p->exp));
        }
        if (expr.ptr->getType() == SymType::FUNC) {
            auto f = std::static_pointer_cast<SymFunc>(expr.ptr);
            std::vector<std::shared_ptr<SymNode>> nArgs;
            for (auto& arg : f->args) nArgs.push_back(factor(SymExpr(arg)).ptr);
            return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
        }

        // ══════════════════════════════════════════
        // 阶段 2：公因式提取（对加法节点）
        // ══════════════════════════════════════════
        if (expr.ptr->getType() == SymType::ADD) {
            auto add = std::static_pointer_cast<SymAdd>(expr.ptr);
            if (add->args.size() < 2) return expr;

            BigInt numGcd(0);
            bool firstTerm = true;
            // 记录结构：因子字面量 -> {因子表达式, 最小指数}
            std::map<std::string, std::pair<SymExpr, int64_t>> commonSym;

            struct ParsedTerm {
                BigInt coeff;
                std::map<std::string, std::pair<SymExpr, int64_t>> syms;
            };
            std::vector<ParsedTerm> parsedTerms;

            // ── 2a. 解析加法中的每一项 ──
            for (auto& arg : add->args) {
                ParsedTerm pt;
                pt.coeff = BigInt(1);

                auto processFactor = [&](const SymExpr& f) {
                    if (f.ptr->getType() == SymType::NUM) {
                        auto [isInt, n] = extractExactInt(std::static_pointer_cast<SymNum>(f.ptr)->value);
                        if (isInt) pt.coeff = pt.coeff * BigInt(n);
                        else pt.syms[f.toString()] = { f, 1 };
                    }
                    else if (f.ptr->getType() == SymType::POW) {
                        auto p = std::static_pointer_cast<SymPow>(f.ptr);
                        if (p->exp->getType() == SymType::NUM) {
                            auto [isInt, n] = extractExactInt(std::static_pointer_cast<SymNum>(p->exp)->value);
                            if (isInt && n > 0) {
                                SymExpr base(p->base);
                                pt.syms[base.toString()] = { base, n };
                            }
                            else {
                                pt.syms[f.toString()] = { f, 1 };
                            }
                        }
                        else {
                            pt.syms[f.toString()] = { f, 1 };
                        }
                    }
                    else {
                        pt.syms[f.toString()] = { f, 1 };
                    }
                    };

                if (arg->getType() == SymType::MUL) {
                    for (auto& f : std::static_pointer_cast<SymMul>(arg)->args)
                        processFactor(SymExpr(f));
                }
                else {
                    processFactor(SymExpr(arg));
                }

                parsedTerms.push_back(pt);

                // ── 2b. 与总公因式做交集 ──
                if (firstTerm) {
                    numGcd = pt.coeff.abs();
                    commonSym = pt.syms;
                    firstTerm = false;
                }
                else {
                    if (!pt.coeff.isZero() && !numGcd.isZero())
                        numGcd = BigInt::gcd(numGcd, pt.coeff.abs());
                    else
                        numGcd = BigInt(1);

                    for (auto it = commonSym.begin(); it != commonSym.end(); ) {
                        auto fnd = pt.syms.find(it->first);
                        if (fnd == pt.syms.end()) {
                            it = commonSym.erase(it);
                        }
                        else {
                            it->second.second = std::min(it->second.second, fnd->second.second);
                            ++it;
                        }
                    }
                }
            }

            // ── 2c. 构造公因式 ──
            SymExpr commonFactor(numGcd);
            for (auto& kv : commonSym) {
                int64_t p = kv.second.second;
                if (p == 1) commonFactor = commonFactor * kv.second.first;
                else if (p > 1) commonFactor = commonFactor * (kv.second.first ^ SymExpr(BigInt(p)));
            }

            // 如果公因式就是 1，无法提取，老老实实退回
            if (commonFactor.isOne()) {
                SymExpr res(BigInt(0));
                for (auto& arg : add->args) res = res + factor(SymExpr(arg));
                return res;
            }

            // ── 2d. 剥离公因式，组装括号内的剩余加法 ──
            SymExpr newAdd(BigInt(0));
            for (auto& pt : parsedTerms) {
                SymExpr rem = numGcd.isZero() ? SymExpr(pt.coeff) : SymExpr(pt.coeff / numGcd);
                for (auto& kv : pt.syms) {
                    int64_t origP = kv.second.second;
                    int64_t commonP = 0;
                    auto fnd = commonSym.find(kv.first);
                    if (fnd != commonSym.end()) commonP = fnd->second.second;
                    int64_t remainP = origP - commonP;
                    if (remainP == 1) rem = rem * kv.second.first;
                    else if (remainP > 1) rem = rem * (kv.second.first ^ SymExpr(BigInt(remainP)));
                }
                newAdd = newAdd + rem;
            }

            // ★ 递归闭环：对括号内的剩余部分继续分解！
            return commonFactor * factor(newAdd, depth);
        }

        return expr;
    }

    // =================================================================
    // 实数域完全因式分解 (Real Domain Factorization)
    // =================================================================
    SymExpr factorReal(const SymExpr& expr) {
        if (!expr.ptr) return expr;

        // 1. 先进行有理域因式分解
        SymExpr factored = factor(expr);

        // 2. 遍历因子，在实数域上进一步分解
        std::function<SymExpr(const SymExpr&)> process = [&](const SymExpr& e) -> SymExpr {
            if (e.ptr->getType() == SymType::MUL) {
                auto mul = std::static_pointer_cast<SymMul>(e.ptr);
                SymExpr res(BigInt(1));
                for (auto& arg : mul->args) res = res * process(SymExpr(arg));
                return res;
            }
            if (e.ptr->getType() == SymType::POW) {
                auto powNode = std::static_pointer_cast<SymPow>(e.ptr);
                return process(SymExpr(powNode->base)) ^ SymExpr(powNode->exp);
            }

            std::set<std::string> vars;
            collectAllVars(e.ptr, vars);
            if (vars.size() == 1) {
                std::string var = *vars.begin();
                auto coeffs = extractCoeffs(e, var);
                if (coeffs.size() > 2) {
                    int degree = static_cast<int>(coeffs.size()) - 1;
                    SymExpr X = SymExpr::makeVar(var);

                    // 二次多项式：直接使用求根公式判断实数域可约性
                    if (degree == 2) {
                        SymExpr C = coeffs[0];
                        SymExpr B = coeffs[1];
                        SymExpr A = coeffs[2];
                        SymExpr delta = simplify(B * B - SymExpr(BigInt(4)) * A * C);

                        bool isNeg = false;
                        if (delta.ptr->getType() == SymType::NUM) {
                            isNeg = isCasNegative(std::static_pointer_cast<SymNum>(delta.ptr)->value);
                        } else if (delta.ptr->getType() == SymType::MUL) {
                            auto mul = std::static_pointer_cast<SymMul>(delta.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                isNeg = isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                            }
                        }

                        if (!isNeg) {
                            SymExpr sqrtDelta;
                            auto [ok, exactSqrt] = trySquareRoot(delta, true);
                            if (ok) sqrtDelta = exactSqrt;
                            else sqrtDelta = delta ^ SymExpr(Fraction(1, 2));

                            SymExpr twoA = SymExpr(BigInt(2)) * A;
                            SymExpr r1 = simplify((-B + sqrtDelta) / twoA);
                            SymExpr r2 = simplify((-B - sqrtDelta) / twoA);

                            if (A.isOne()) return (X - r1) * (X - r2);
                            return A * (X - r1) * (X - r2);
                        }
                        return e; // 实数域不可约
                    }

                    // 检查是否为双二次多项式 Ax^4 + Bx^2 + C
                    if (degree == 4 && coeffs[3].isZero() && coeffs[1].isZero()) {
                        SymExpr A = coeffs[4];
                        SymExpr B = coeffs[2];
                        SymExpr C = coeffs[0];
                        SymExpr delta = simplify(B * B - SymExpr(BigInt(4)) * A * C);
                        
                        bool isNeg = false;
                        if (delta.ptr->getType() == SymType::NUM) {
                            isNeg = isCasNegative(std::static_pointer_cast<SymNum>(delta.ptr)->value);
                        } else if (delta.ptr->getType() == SymType::MUL) {
                            auto mul = std::static_pointer_cast<SymMul>(delta.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                isNeg = isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                            }
                        }
                        
                        if (!isNeg) {
                            SymExpr sqrtDelta;
                            auto [ok, exactSqrt] = trySquareRoot(delta, true);
                            if (ok) sqrtDelta = exactSqrt;
                            else sqrtDelta = delta ^ SymExpr(Fraction(1, 2));

                            SymExpr twoA = SymExpr(BigInt(2)) * A;
                            SymExpr u1 = simplify((-B + sqrtDelta) / twoA);
                            SymExpr u2 = simplify((-B - sqrtDelta) / twoA);
                            
                            SymExpr f1 = X * X - u1;
                            SymExpr f2 = X * X - u2;
                            
                            if (A.isOne()) return process(f1) * process(f2);
                            return A * process(f1) * process(f2);
                        } else {
                            // delta < 0, 必定有 A 和 C 同号，可配方为平方差
                            SymExpr CA = simplify(C / A);
                            auto [ok, sqrtCA] = trySquareRoot(CA, true);
                            SymExpr rootCA = ok ? sqrtCA : (CA ^ SymExpr(Fraction(1, 2)));
                            
                            SymExpr middle = simplify(SymExpr(BigInt(2)) * rootCA - B / A);
                            auto [ok2, sqrtMiddle] = trySquareRoot(middle, true);
                            SymExpr rootMiddle = ok2 ? sqrtMiddle : (middle ^ SymExpr(Fraction(1, 2)));
                            
                            SymExpr f1 = X * X - rootMiddle * X + rootCA;
                            SymExpr f2 = X * X + rootMiddle * X + rootCA;
                            
                            if (A.isOne()) return process(f1) * process(f2);
                            return A * process(f1) * process(f2);
                        }
                    }

                    // 高次多项式：先检查是否为二项式 Ax^n + B
                    bool isBinomial = true;
                    for (int i = 1; i < degree; ++i) {
                        if (!coeffs[i].isZero()) {
                            isBinomial = false;
                            break;
                        }
                    }
                    // 高次多项式：先检查是否为二项式 Ax^n + B (且 B 不为 0，否则已经是单项式因子)
                    if (isBinomial && degree > 2 && !coeffs[0].isZero()) {
                        SymExpr A = coeffs[degree];
                        SymExpr B = coeffs[0];
                        SymExpr BA = simplify(B / A);
                        
                        bool isPos = false;
                        bool isNeg = false;
                        if (BA.ptr->getType() == SymType::NUM) {
                            isNeg = isCasNegative(std::static_pointer_cast<SymNum>(BA.ptr)->value);
                            isPos = !isNeg && !BA.isZero();
                        } else if (BA.ptr->getType() == SymType::MUL) {
                            auto mul = std::static_pointer_cast<SymMul>(BA.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                isNeg = isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                                isPos = !isNeg;
                            } else {
                                isPos = true;
                            }
                        } else {
                            isPos = true;
                        }

                        SymExpr absBA = isNeg ? simplify(-BA) : BA;
                        SymExpr R = simplify(absBA ^ SymExpr(Fraction(1, degree)));
                        SymExpr res = A;
                        int n = degree;

                        auto getExactCos = [](int m, int n_val) -> SymExpr {
                            int g = static_cast<int>(BigInt::gcd(BigInt(std::abs(m)), BigInt(n_val)).toDouble());
                            m /= g;
                            n_val /= g;
                            if (m < 0) m = -m;
                            m %= (2 * n_val);
                            if (m > n_val) m = 2 * n_val - m;
                            
                            if (m == 0) return SymExpr(BigInt(1));
                            if (m == n_val) return SymExpr(BigInt(-1));
                            if (2 * m == n_val) return SymExpr(BigInt(0));
                            
                            if (n_val == 3 && m == 1) return SymExpr(Fraction(1, 2));
                            if (n_val == 4 && m == 1) return SymExpr(BigInt(2)) ^ SymExpr(Fraction(-1, 2));
                            if (n_val == 4 && m == 3) return -(SymExpr(BigInt(2)) ^ SymExpr(Fraction(-1, 2)));
                            if (n_val == 6 && m == 1) return (SymExpr(BigInt(3)) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(2));
                            if (n_val == 6 && m == 5) return -(SymExpr(BigInt(3)) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(2));
                            if (n_val == 5) {
                                SymExpr sqrt5 = SymExpr(BigInt(5)) ^ SymExpr(Fraction(1, 2));
                                if (m == 1) return (sqrt5 + SymExpr(BigInt(1))) / SymExpr(BigInt(4));
                                if (m == 2) return (sqrt5 - SymExpr(BigInt(1))) / SymExpr(BigInt(4));
                                if (m == 3) return (SymExpr(BigInt(1)) - sqrt5) / SymExpr(BigInt(4));
                                if (m == 4) return -(sqrt5 + SymExpr(BigInt(1))) / SymExpr(BigInt(4));
                            }
                            if (n_val == 8) {
                                SymExpr sqrt2 = SymExpr(BigInt(2)) ^ SymExpr(Fraction(1, 2));
                                if (m == 1) return ((SymExpr(BigInt(2)) + sqrt2) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(2));
                                if (m == 3) return ((SymExpr(BigInt(2)) - sqrt2) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(2));
                                if (m == 5) return -((SymExpr(BigInt(2)) - sqrt2) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(2));
                                if (m == 7) return -((SymExpr(BigInt(2)) + sqrt2) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(2));
                            }
                            if (n_val == 10) {
                                SymExpr sqrt5 = SymExpr(BigInt(5)) ^ SymExpr(Fraction(1, 2));
                                if (m == 1) return ((SymExpr(BigInt(10)) + SymExpr(BigInt(2)) * sqrt5) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(4));
                                if (m == 3) return ((SymExpr(BigInt(10)) - SymExpr(BigInt(2)) * sqrt5) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(4));
                                if (m == 7) return -((SymExpr(BigInt(10)) - SymExpr(BigInt(2)) * sqrt5) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(4));
                                if (m == 9) return -((SymExpr(BigInt(10)) + SymExpr(BigInt(2)) * sqrt5) ^ SymExpr(Fraction(1, 2))) / SymExpr(BigInt(4));
                            }
                            if (n_val == 12) {
                                SymExpr sqrt6 = SymExpr(BigInt(6)) ^ SymExpr(Fraction(1, 2));
                                SymExpr sqrt2 = SymExpr(BigInt(2)) ^ SymExpr(Fraction(1, 2));
                                if (m == 1) return (sqrt6 + sqrt2) / SymExpr(BigInt(4));
                                if (m == 5) return (sqrt6 - sqrt2) / SymExpr(BigInt(4));
                                if (m == 7) return -(sqrt6 - sqrt2) / SymExpr(BigInt(4));
                                if (m == 11) return -(sqrt6 + sqrt2) / SymExpr(BigInt(4));
                            }
                            
                            SymExpr theta = simplify(SymExpr(Fraction(m, n_val)) * SymExpr::makeVar("PI"));
                            return simplify(SymExpr(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{theta.ptr})));
                        };

                        if (isPos) {
                            if (n % 2 != 0) {
                                res = res * (X + R);
                                for (int k = 1; k <= (n - 1) / 2; ++k) {
                                    SymExpr cosTheta = getExactCos(2 * k - 1, n);
                                    res = res * (X * X - SymExpr(BigInt(2)) * R * X * cosTheta + R * R);
                                }
                            } else {
                                for (int k = 1; k <= n / 2; ++k) {
                                    SymExpr cosTheta = getExactCos(2 * k - 1, n);
                                    res = res * (X * X - SymExpr(BigInt(2)) * R * X * cosTheta + R * R);
                                }
                            }
                        } else {
                            if (n % 2 != 0) {
                                res = res * (X - R);
                                for (int k = 1; k <= (n - 1) / 2; ++k) {
                                    SymExpr cosTheta = getExactCos(2 * k, n);
                                    res = res * (X * X - SymExpr(BigInt(2)) * R * X * cosTheta + R * R);
                                }
                            } else {
                                res = res * (X - R) * (X + R);
                                for (int k = 1; k <= (n - 2) / 2; ++k) {
                                    SymExpr cosTheta = getExactCos(2 * k, n);
                                    res = res * (X * X - SymExpr(BigInt(2)) * R * X * cosTheta + R * R);
                                }
                            }
                        }
                        return res;
                    }

                    // 其他高次多项式：尝试求根并提取实数一次因式
                    std::vector<SymExpr> roots = solveEq(e, var);
                    if (!roots.empty()) {
                        std::vector<SymExpr> realRoots;
                        for (const auto& r : roots) {
                            bool hasComplex = false;
                            std::function<void(const std::shared_ptr<SymNode>&)> checkComplex = [&](const std::shared_ptr<SymNode>& node) {
                                if (!node || hasComplex) return;
                                if (node->getType() == SymType::VAR) {
                                    auto varNode = std::static_pointer_cast<SymVar>(node);
                                    if (varNode->name == "i" || varNode->name == "I") hasComplex = true;
                                } else if (node->getType() == SymType::ADD) {
                                    for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) checkComplex(arg);
                                } else if (node->getType() == SymType::MUL) {
                                    for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) checkComplex(arg);
                                } else if (node->getType() == SymType::POW) {
                                    auto powNode = std::static_pointer_cast<SymPow>(node);
                                    if (powNode->base->getType() == SymType::NUM && powNode->exp->getType() == SymType::NUM) {
                                        auto baseNum = std::static_pointer_cast<SymNum>(powNode->base);
                                        auto expNum = std::static_pointer_cast<SymNum>(powNode->exp);
                                        if (isCasNegative(baseNum->value)) {
                                            auto [isInt, n_val] = extractExactInt(expNum->value);
                                            if (!isInt) hasComplex = true;
                                        }
                                    }
                                    checkComplex(powNode->base);
                                    checkComplex(powNode->exp);
                                } else if (node->getType() == SymType::FUNC) {
                                    for (auto& arg : std::static_pointer_cast<SymFunc>(node)->args) checkComplex(arg);
                                }
                            };
                            checkComplex(r.ptr);
                            if (!hasComplex) realRoots.push_back(r);
                        }

                        if (!realRoots.empty()) {
                            SymExpr rem = e;
                            SymExpr res(BigInt(1));

                            for (const auto& r : realRoots) {
                                SymExpr factorX = X - r;
                                int divIter = 0;
                                while (true) {
                                    if (++divIter > SymConfig::maxIterations) break;
                                    auto [q, rem_new] = polyDiv(rem, factorX, var);
                                    if (rem_new.isZero()) {
                                        res = res * factorX;
                                        rem = q;
                                    } else {
                                        break;
                                    }
                                }
                            }

                            if (!rem.isOne() && rem != e) {
                                // 对剩余部分递归处理（可能降次为二次，从而被上面的二次逻辑处理）
                                res = res * process(rem);
                            } else if (!rem.isOne()) {
                                res = res * rem;
                            }

                            if (res != e) {
                                return res;
                            }
                        }
                    }
                }
            }
            return e;
        };

        return process(factored);
    }

} // namespace jc
