#include "Factorization.h"
#include <vector>
#include <set>
#include <algorithm>
#include <stdexcept>
#include <functional>
#include <map>
#include <unordered_map>

namespace jc {

    // =================================================================
    // 有理根因子规范化：把 (x + p/q) 这种带分母的线性因子吸收成整系数因子。
    //   6*(x + 1/2)*(x + 1/3)  →  (2*x + 1) * (3*x + 1)
    //   线性因子 (x + c)，c = p/q：q*(x + p/q) = q*x + p，把 q 从系数里扣掉。
    //   仅当系数是精确整数、可被各分母整除时改写；否则原样返回
    //   （无分母可吸收时也不动，避免改变既有输出）。
    // =================================================================
    static SymExpr normalizeRationalFactors(const SymExpr& expr) {
        if (!expr.ptr || expr.ptr->getType() != SymType::MUL) return expr;
        auto mul = static_cast<SymMul*>(expr.ptr);

        BigInt coeffVal(1);
        std::vector<SymNode*> otherArgs;
        struct LinFac { SymNode* varNode; BigInt p; BigInt q; SymNode* node; };
        std::vector<LinFac> lins;
        bool anyDenom = false;

        for (SymNode* arg : mul->args) {
            if (arg->getType() == SymType::NUM) {
                auto [isI, v] = extractExactInt(static_cast<SymNum*>(arg)->value);
                if (!isI) return expr;
                coeffVal = coeffVal * BigInt(v);
                continue;
            }
            if (arg->getType() != SymType::ADD) { otherArgs.push_back(arg); continue; }
            auto add = static_cast<SymAdd*>(arg);
            if (add->args.size() != 2) { otherArgs.push_back(arg); continue; }
            // 线性因子形如 (x + c)：一个一次项 + 一个常数项
            SymNode* varNode = nullptr;
            SymNode* constNode = nullptr;
            bool ok = true;
            for (SymNode* t : add->args) {
                if (t->getType() == SymType::VAR) {
                    if (varNode) { ok = false; break; }
                    varNode = t;
                } else if (t->getType() == SymType::NUM) {
                    if (constNode) { ok = false; break; }
                    constNode = t;
                } else { ok = false; break; }
            }
            if (!ok || !varNode || !constNode) { otherArgs.push_back(arg); continue; }
            // c = p/q
            const CASVal& cv = static_cast<SymNum*>(constNode)->value;
            BigInt p(0), q(1);
            if (std::holds_alternative<int32_t>(cv)) p = BigInt(std::get<int32_t>(cv));
            else if (std::holds_alternative<BigInt>(cv)) p = std::get<BigInt>(cv);
            else if (std::holds_alternative<Fraction>(cv)) {
                p = std::get<Fraction>(cv).getNum();
                q = std::get<Fraction>(cv).getDen();
            } else return expr;
            if (p.isZero() || q <= BigInt(1)) { otherArgs.push_back(arg); continue; }
            anyDenom = true;
            lins.push_back({ varNode, p, q, arg });
        }

        if (!anyDenom || lins.empty()) return expr;

        std::vector<SymNode*> newArgs;
        BigInt newCoeff = coeffVal;
        for (auto& L : lins) {
            if ((newCoeff % L.q) != BigInt(0)) return expr;   // 除不尽就整体放弃
            newCoeff = newCoeff / L.q;
            SymNode* lin = (SymExpr(L.q) * SymExpr(L.varNode) + SymExpr(L.p)).ptr;
            newArgs.push_back(lin);
        }
        for (SymNode* o : otherArgs) newArgs.push_back(o);
        if (newArgs.empty()) return expr;

        SymExpr res = SymExpr(newCoeff);
        for (SymNode* a : newArgs) res = res * SymExpr(a);
        return res;
    }

    // =================================================================
    // 全域多元全次因式分解 (Multivariate Polynomial Factorization)
    // 包含: 二次求解 + N次完全平方式提取
    // =================================================================
    static SymExpr multivariatePolynomialFactor(const SymExpr& expr, int depth) {
        if (depth > SymConfig::maxDepth / 2) return expr; // 极限深度保护，防止复杂多元死锁

        std::set<std::string> vars;
        collectAllVars(expr.ptr, vars);
        if (vars.empty()) return expr;

        // 一次项分组的结果收集器：同一个表达式按不同变元分组会得到不同的（等价的）
        // 分解，质量可能差很远 —— 3*x*y + 6*x + 2*y + 4 按 x 分组得
        // (3*y + 6) * (x + 2/3)（商是分数），按 y 分组得 (3*x + 2) * (y + 2)。
        // 变元集合是按字母序遍历的，先撞上哪个就返回哪个会随机决定输出质量，
        // 所以在所有变元里取最简的一个。
        SymExpr groupBest;
        bool hasGroupBest = false;
        int groupBestSz = 0;

        for (const std::string& mainVar : vars) {
            auto coeffs = extractCoeffs(expr, mainVar);
            if (coeffs.empty()) continue;

            int N = static_cast<int>(coeffs.size()) - 1;

            // =======================================================
            // 策略 0：一次项分组（factor by grouping）
            //   P 在 mainVar 上是一次：P = c1*mainVar + c0，c1 / c0 是其余变元的多项式。
            //   若 c1 整除 c0，则 P = c1 * (mainVar + c0/c1)。
            //   这是「提公因式」在多元下的形态：
            //       x*y + 2*x + y + 2
            //   四个项没有公共因式（逐项比较最小指数的实现看不到），但按 y 看是
            //       x*(y+2) + 1*(y+2)
            //   公因式 (y+2) 藏在两项各自的组合里。下面那套「N 次完全平方式嗅探」
            //   本来也能覆盖 N==1（取 R = c0/(N*c1)、再验证 P == c1*(x+R)^1），
            //   但它把 c0/c1 交给 simplifyCore 去算 —— 那是个启发式化简、不做多项式
            //   除法，会留下 (2x+2)*(x+1)^(-1) 这种没约掉的形式，验证于是不通过。
            //   这里改用 polyDiv 精确判定整除，顺带让 N<2 不再被整类跳过。
            // =======================================================
            if (N == 1) {
                SymExpr c1 = coeffs[1];
                SymExpr c0 = coeffs[0];
                // c1 必须是真正含其余变元的多项式。c1 是纯数时（2x+3y 里的 2）
                // 所谓"整除"只是抽数值公因数，会把它改写成 2*(x + 3/2*y) ——
                // 数学值不变但形态更差，而数值内容本来由加法公因式那条路负责。
                if (!c0.isZero() && !c1.isOne() && c1.ptr->getType() != SymType::NUM) {
                    SymExpr Xv = SymExpr::makeVar(mainVar);
                    for (const std::string& w : vars) {
                        if (w == mainVar) continue;
                        try {
                            auto [q, r] = polyDiv(c0, c1, w);
                            if (r.isZero() && !q.isZero()) {
                                SymExpr cand = simplifyCore(c1 * (Xv + q));
                                if (cand.ptr != expr.ptr) {
                                    int sz = getAstNodeCount(cand);
                                    if (!hasGroupBest || sz < groupBestSz) {
                                        groupBest = cand;
                                        groupBestSz = sz;
                                        hasGroupBest = true;
                                    }
                                    break;   // 这个 mainVar 上已经找到分组，换下一个变元
                                }
                            }
                        }
                        catch (const EngineInterruptError&) { throw; }
                        catch (const std::runtime_error&) {}
                    }
                }
                continue;   // 在 mainVar 上一次的表达式不可能再按 mainVar 分解
            }

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
            // 策略 1.5：偶次降维（t = mainVar^2）
            //   若 mainVar 的奇数次系数全为 0，P 其实是 mainVar^2 的多项式。令
            //   t = mainVar^2 把次数减半，交给 factor 递归，再换回来：
            //       x^4 - y^4                t^2 - y^4              → (t-y^2)(t+y^2)
            //       x^4 + 2x^2y^2 + y^4      t^2 + 2*t*y^2 + y^4    → (t+y^2)^2
            //       x^6 + 3x^4y^2 + ...      t^3 + 3t^2y^2 + ...    → (t+y^2)^3
            //   4 次、6 次的原始形式对另外两条策略都够不着（策略 2 只管 N==2，策略 1
            //   只管 (x+c)^N），降维之后原有的完全幂嗅探与二次降维才有机会命中。
            //   这就是「降维归约」：不新增特例，而是把问题化成已有策略能处理的形状。
            //   换回来的结果再走一遍 factor，让 x^4-y^4 → (x^2-y^2)(x^2+y^2) 继续
            //   分解成 (x-y)(x+y)(x^2+y^2)；不会打转 —— 若换回来正好是原节点，
            //   下面的 ptr 比较会拦住，factorImpl 接着走 MUL / POW 分支照常推进。
            //   ★ 只对多元（vars.size() >= 2）启用。一元的偶次式 factorPolynomialCZ
            //     已经能完整处理（x^n ± 1 走 Zassenhaus、x^4+2x^2+1 走平方自由分解），
            //     再绕这一圈「构造 Q → 递归 factor → 整树 subs → 再 factor」纯属浪费：
            //     实测给 x^4-1 / x^6-1 这类式子加这道工序会让混合负载慢 11~14%，
            //     而收益（完备性探针 done 20 → 25）全部来自多元用例。
            // =======================================================
            if (N >= 4 && vars.size() >= 2) {
                bool oddZero = true;
                for (int i = 1; i < N; i += 2) {
                    if (!coeffs[i].isZero()) { oddZero = false; break; }
                }
                if (oddZero) {
                    // ★ 临时变量名必须逐层唯一。这个策略会递归，而内层降维时 mainVar
                    //   很可能正是外层刚引入的临时变量；两层同名的话，内层会执行
                    //   subs(F, t, t^2) —— 把临时变量替换进它自己，结果彻底错乱。
                    //   深度沿递归严格递增，用它做后缀即可保证嵌套链上不重名；
                    //   同深度的兄弟调用不会互相嵌套，无妨。前缀刻意取得不像用户
                    //   标识符。
                    const std::string tName = "jc2evensub" + std::to_string(depth);
                    SymExpr T = SymExpr::makeVar(tName);
                    SymExpr Q(BigInt(0));
                    for (int i = 0; i <= N; i += 2) {
                        int j = i / 2;
                        if (j == 0)      Q = Q + coeffs[i];
                        else if (j == 1) Q = Q + coeffs[i] * T;
                        else             Q = Q + coeffs[i] * (T ^ SymExpr(BigInt(j)));
                    }
                    Q = simplifyCore(Q);
                    try {
                        SymExpr F = factor(Q, depth + 1);
                        if (F.ptr != Q.ptr) {
                            SymExpr back = simplifyCore(subs(F, tName, X * X));
                            if (back.ptr != expr.ptr) return factor(back, depth + 1);
                        }
                    }
                    catch (const EngineInterruptError&) { throw; }
                    catch (const std::runtime_error&) {}
                }
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
                
                SymExpr factoredDelta = simpDelta;
                if (getAstNodeCount(simpDelta) < 150) {
                    factoredDelta = factor(simpDelta, depth + 1);
                }

                auto [sqrtOk, sqrtDelta] = trySquareRoot(factoredDelta);
                if (!sqrtOk) continue;

                // 依靠系统底层卓越的 operator* 和 operator/ 来负责数学相消！
                SymExpr twoA = SymExpr(BigInt(2)) * A;
                SymExpr num1 = factor(simplifyCore(-B + sqrtDelta), depth + 1);
                SymExpr num2 = factor(simplifyCore(-B - sqrtDelta), depth + 1);
                SymExpr factTwoA = factor(twoA, depth + 1);

                // 分子分母分别 factor 后相除，底层会自动合并同底数幂，避免 full_simplify 循环引用
                // ★ 二次公式是"只除不约"的，simplifyCore 既不会约分、也不会把数值因子
                //   分配进和，于是会留下 1/2*(-(3*y+1)+y+1) 这种读不出来的根：
                //     factor(x^2 + 3*x*y + 2*y^2 + x + y)
                //         →  -(1/2*(...) - x) * (1/2*(4*y+2) + x)     值对但没法用
                //   改用 simplifyRational —— 约分正是它的职责（getFraction + gcd 相消）。
                SymExpr r1 = simplifyRational(num1 / factTwoA);
                SymExpr r2 = simplifyRational(num2 / factTwoA);

                if (A.isOne()) return normalizeRationalFactors((X - r1) * (X - r2));
                return normalizeRationalFactors(A * (X - r1) * (X - r2));
            }
        }

        // =======================================================
        // 策略 3：线性型降维（把 x 换成 s - y，即 s = x + y）
        //   若 P 其实只是 (x+y) 的多项式，那么把 x 换成 s - y 再展开，P 里就再也
        //   不出现任何原变元、只剩 s —— 一元分解做完再把 s 换回 x+y：
        //       x^2+2xy+y^2+3x+3y+2  →  s^2+3s+2   → (s+1)(s+2) → (x+y+1)(x+y+2)
        //       x^2+2xy+y^2-1        →  s^2-1      → (s-1)(s+1) → (x+y-1)(x+y+1)
        //       x^2+2xy+y^2+x+y      →  s^2+s      → s(s+1)     → (x+y)(x+y+1)
        //   这是纯粹的换元降维：不针对某个具体形状打补丁，而是把二元问题化成一元的，
        //   把"能分解"这件事交给已经成熟的一元路径。上面两条策略都够不着这类式子
        //   （二次降维要求判别式是完全平方，而这里的判别式是 1、4、0，恰好看不出
        //   那个隐藏的 (x+y) 结构）。
        //   只做二元：三元及以上时"s 之外还剩更多变元"，能命中"只含 s"的情形更少，
        //   等有实测需求再扩。
        // =======================================================
        if (vars.size() == 2) {
            const std::string v0 = *vars.begin();
            const std::string v1 = *vars.rbegin();
            // 临时变量名带深度后缀：本策略会递归，内外层同名会互相替换（同偶次降维）。
            const std::string sName = "jc2linsub" + std::to_string(depth);
            SymExpr S = SymExpr::makeVar(sName);
            SymExpr X0 = SymExpr::makeVar(v0);
            SymExpr X1 = SymExpr::makeVar(v1);
            try {
                SymExpr Q = simplifyCore(expand_core(subs(expr, v0, S - X1), SymConfig::maxExpandTerms));
                // 只有当 Q 里真的不再有原变元时才是一次成功的降维
                std::set<std::string> qVars;
                collectAllVars(Q.ptr, qVars);
                if (qVars.size() == 1 && *qVars.begin() == sName && Q.ptr != expr.ptr) {
                    SymExpr F = factor(Q, depth + 1);
                    if (F.ptr != Q.ptr) {
                        SymExpr back = simplifyCore(subs(F, sName, X0 + X1));
                        if (back.ptr != expr.ptr) return factor(back, depth + 1);
                    }
                }
            }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}
        }

        if (hasGroupBest) return groupBest;
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
        if (r > BigInt(1)) JC2_THROW(MathError, "Not invertible in Zp");
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
        if (b.empty()) JC2_THROW(MathError, "Division by zero poly in Zp");
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

    // =================================================================
    // 机器字长快速通道 (Fast Path for p < 2^31)
    // =================================================================
    using PolyZp64 = std::vector<int64_t>;

    static int64_t invModP64(int64_t a, int64_t p) {
        int64_t t = 0, newt = 1;
        int64_t r = p, newr = a % p;
        if (newr < 0) newr += p;
        while (newr != 0) {
            int64_t quotient = r / newr;
            int64_t temp_t = t - quotient * newt;
            t = newt; newt = temp_t;
            int64_t temp_r = r - quotient * newr;
            r = newr; newr = temp_r;
        }
        if (r > 1) JC2_THROW(MathError, "Not invertible in Zp");
        if (t < 0) {
            t = t % p;
            if (t < 0) t += p;
        }
        return t;
    }

    static void trimP64(PolyZp64& a) {
        while (!a.empty() && a.back() == 0) a.pop_back();
    }

    static PolyZp64 subP64(const PolyZp64& a, const PolyZp64& b, int64_t p) {
        PolyZp64 res(std::max(a.size(), b.size()), 0);
        for (size_t i = 0; i < res.size(); ++i) {
            int64_t va = i < a.size() ? a[i] : 0;
            int64_t vb = i < b.size() ? b[i] : 0;
            int64_t diff = (va - vb) % p;
            if (diff < 0) diff += p;
            res[i] = diff;
        }
        trimP64(res);
        return res;
    }

    static PolyZp64 mulP64(const PolyZp64& a, const PolyZp64& b, int64_t p) {
        if (a.empty() || b.empty()) return {};
        PolyZp64 res(a.size() + b.size() - 1, 0);
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] == 0) continue;
            for (size_t j = 0; j < b.size(); ++j) {
                int64_t term = (a[i] * b[j]) % p;
                res[i+j] = (res[i+j] + term) % p;
            }
        }
        for (auto& x : res) {
            if (x < 0) x += p;
        }
        trimP64(res);
        return res;
    }

    static std::pair<PolyZp64, PolyZp64> divP64(PolyZp64 a, const PolyZp64& b, int64_t p) {
        if (b.empty()) JC2_THROW(MathError, "Division by zero poly in Zp");
        trimP64(a);
        if (a.size() < b.size()) return {{}, a};
        PolyZp64 q(a.size() - b.size() + 1, 0);
        int64_t invLead = invModP64(b.back(), p);
        for (int i = (int)a.size() - (int)b.size(); i >= 0; --i) {
            if (a[i + b.size() - 1] == 0) continue;
            int64_t factor = (a[i + b.size() - 1] * invLead) % p;
            if (factor < 0) factor += p;
            q[i] = factor;
            for (size_t j = 0; j < b.size(); ++j) {
                int64_t term = (factor * b[j]) % p;
                a[i+j] = (a[i+j] - term) % p;
                if (a[i+j] < 0) a[i+j] += p;
            }
        }
        trimP64(q);
        trimP64(a);
        return {q, a};
    }

    static PolyZp64 gcdP64(PolyZp64 a, PolyZp64 b, int64_t p) {
        while (!b.empty()) {
            auto [q, r] = divP64(a, b, p);
            a = b;
            b = r;
        }
        if (!a.empty()) {
            int64_t invL = invModP64(a.back(), p);
            for (auto& x : a) {
                x = (x * invL) % p;
                if (x < 0) x += p;
            }
        }
        return a;
    }

    static PolyZp64 powModP64(PolyZp64 base, BigInt exp, const PolyZp64& modPoly, int64_t p) {
        PolyZp64 res = {1};
        base = divP64(base, modPoly, p).second;
        while (!exp.isZero()) {
            BigInt rem = exp % BigInt(2);
            if (!rem.isZero()) {
                res = divP64(mulP64(res, base, p), modPoly, p).second;
            }
            base = divP64(mulP64(base, base, p), modPoly, p).second;
            exp = exp / BigInt(2);
        }
        return res;
    }

    static PolyZp64 derivP64(const PolyZp64& a, int64_t p) {
        if (a.size() <= 1) return {};
        PolyZp64 res(a.size() - 1, 0);
        for (size_t i = 1; i < a.size(); ++i) {
            res[i-1] = (a[i] * static_cast<int64_t>(i)) % p;
            if (res[i-1] < 0) res[i-1] += p;
        }
        trimP64(res);
        return res;
    }

    static std::vector<PolyZp64> czEDF64(const PolyZp64& f, int d, int64_t p) {
        int n = static_cast<int>(f.size()) - 1;
        if (n == d) return {f};
        std::vector<PolyZp64> factors = {f};
        
        int seed = 1;
        auto randPoly = [&](int deg) {
            PolyZp64 r(deg, 0);
            for (int i = 0; i < deg; ++i) {
                seed = (seed * 1103515245 + 12345) & 0x7fffffff;
                r[i] = seed % p;
            }
            trimP64(r);
            return r;
        };

        BigInt exp = BigInt(p);
        for (int i = 1; i < d; ++i) exp = exp * BigInt(p);
        exp = (exp - BigInt(1)) / BigInt(2);

        while (factors.size() < (size_t)(n / d)) {
            PolyZp64 a = randPoly(n);
            PolyZp64 b = powModP64(a, exp, f, p);
            b = subP64(b, {1}, p);
            
            std::vector<PolyZp64> nextFactors;
            for (const auto& u : factors) {
                if ((int)u.size() - 1 == d) {
                    nextFactors.push_back(u);
                    continue;
                }
                PolyZp64 g = gcdP64(b, u, p);
                if (!g.empty() && g.size() > 1 && g.size() < u.size()) {
                    nextFactors.push_back(g);
                    nextFactors.push_back(divP64(u, g, p).first);
                } else {
                    nextFactors.push_back(u);
                }
            }
            factors = nextFactors;
        }
        return factors;
    }

    static std::vector<PolyZp64> cantorZassenhaus64(const PolyZp64& f, int64_t p) {
        std::vector<PolyZp64> factors;
        PolyZp64 f_star = f;
        PolyZp64 h = {0, 1}; // x
        int d = 1;
        
        while ((int)f_star.size() - 1 >= 2 * d) {
            h = powModP64(h, BigInt(p), f_star, p);
            PolyZp64 h_minus_x = subP64(h, {0, 1}, p);
            PolyZp64 g = gcdP64(h_minus_x, f_star, p);
            
            if (g.size() > 1) {
                auto edfFactors = czEDF64(g, d, p);
                for (auto& fact : edfFactors) factors.push_back(fact);
                f_star = divP64(f_star, g, p).first;
                h = divP64(h, f_star, p).second;
            }
            d++;
        }
        if (f_star.size() > 1) {
            factors.push_back(f_star);
        }
        return factors;
    }

    // =================================================================
    // 「系数是否全为整数」的前置判定。
    //   factorPolynomialCZ 的内部会把多项式首一化（除以首项系数）再做
    //   Zassenhaus 分解。若首项系数不是整数 —— sqrt(2)、pi、1/2 —— 那个系数
    //   就被静默吞掉；而且首一化之后剩下的系数反而变成了整数，事后再检查也
    //   发现不了。实测（数值代入比对 factor(e) 与 e）：
    //       factor(x * sqrt(2))            → x              丢了 sqrt(2)
    //       factor((x + 1) * sqrt(2))      → x + 1          丢了 sqrt(2)
    //       factor((x^2 - 1) * sqrt(2))    → (x + 1)(x - 1) 丢了 sqrt(2)
    //       factor(pi * x)                 → x              丢了 pi
    //   43 个带非整数系数的用例里 19 个给出错误答案（代入 x=3 的偏差 1.2~33）。
    //   所以必须在 polySquareFree 之前判定；非整数系数整体放弃、原样返回。
    //   这也不损失能力：非整数系数多项式本来就不在 CZ 的适用范围里，交给
    //   factorImpl 的 MUL / POW 分支逐因子处理即可。
    //   同一函数下方还有一处针对 part 的 allInt 检查（覆盖符号常量 pi/e/i 等
    //   情形），那是首一化之后的第二道防线，保留不动。
    // =================================================================
    static bool coeffsAllIntegers(const SymExpr& expr, const std::string& var) {
        auto coeffs = extractCoeffs(expr, var);
        if (coeffs.empty()) return false;
        for (const auto& c : coeffs) {
            if (c.ptr->getType() != SymType::NUM) return false;
            const CASVal& v = static_cast<SymNum*>(c.ptr)->value;
            if (std::holds_alternative<BigInt>(v)) continue;
            if (std::holds_alternative<int32_t>(v)) continue;
            if (std::holds_alternative<Fraction>(v) &&
                std::get<Fraction>(v).getDen() == BigInt(1)) continue;
            if (std::holds_alternative<double>(v)) {
                double d = std::get<double>(v);
                if (d == std::round(d)) continue;
            }
            return false;
        }
        return true;
    }

    static SymExpr factorPolynomialCZ(const SymExpr& expr, int depth) {
        if (depth > SymConfig::maxDepth / 2) return expr;
        std::set<std::string> vars;
        collectAllVars(expr.ptr, vars);
        if (vars.size() != 1) return expr;
        
        std::string var = *vars.begin();
        if (!coeffsAllIntegers(expr, var)) return expr;   // 必须先于首一化，否则系数已被吞掉
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
                    auto numVal = static_cast<SymNum*>(c.ptr)->value;
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
                // ★ 含符号常量（pi/e/i）等非整数系数的多项式不走 CZ 路径。
                //   此处必须整体放弃，不能把 part 按首一形式改写：那会把
                //   x^2 - pi 变成 x^2 * pi^(-1) - 1（只是整体缩放了 1/pi），
                //   既不是因式分解，也把调用方原本的表达式改坏了。
                return expr;
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
            
            std::vector<PolyZp> factorsZp;
            
            if (p < BigInt(2147483647)) {
                int64_t p64 = p.toInt64();
                PolyZp64 g_mod64;
                while (true) {
                    g_mod64.clear();
                    for (const auto& x : g_coeffs) {
                        int64_t val = (x % p).toInt64();
                        if (val < 0) val += p64;
                        g_mod64.push_back(val);
                    }
                    trimP64(g_mod64);
                    PolyZp64 g_deriv64 = derivP64(g_mod64, p64);
                    PolyZp64 gcd64 = gcdP64(g_mod64, g_deriv64, p64);
                    if (gcd64.size() <= 1) break;
                    p = p.nextPrime();
                    p64 = p.toInt64();
                }
                
                std::vector<PolyZp64> factorsZp64 = cantorZassenhaus64(g_mod64, p64);
                for (const auto& f64 : factorsZp64) {
                    PolyZp f_big;
                    for (int64_t c : f64) f_big.push_back(BigInt(c));
                    factorsZp.push_back(f_big);
                }
            } else {
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
                factorsZp = cantorZassenhaus(g_mod, p);
            }
            
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
            
            // ★ 首项系数的符号必须由这里补回。
            //   重构阶段每个因子的系数都除以了 Hx_content = gcd(系数绝对值)（恒正），
            //   所以 ∏ h_i 相对 p_int 只差 sign(a_n)^(n-1)：由
            //     ∏ H_i(a_n·x) = a_n^(n-1) · p_int(x)  且  ∏ c_i = |a_n|^(n-1)
            //   得 p_int = sign(a_n)^(n-1) · ∏ h_i。
            //   n 为奇数时 (n-1) 是偶数，符号自动对上；n 为偶数时整体差一个负号。
            //   原实现只补 content > 1 的正公因子，于是 a_n < 0 且次数为偶的多项式
            //   会被"分解"成自己的相反数：cas.factor(-(x^2+1)) 曾返回 x^2+1。
            BigInt leadingFactor = content;
            if (an.isNegative() && (n % 2 == 0)) leadingFactor = -leadingFactor;
            if (leadingFactor != BigInt(1)) partResult = SymExpr(leadingFactor) * partResult;
            
            result = result * (partResult ^ SymExpr(BigInt(power)));
        }
        
        return changed ? result : expr;
    }

    // =================================================================
    // 因式分解主入口
    // =================================================================
    // =================================================================
    // 二项式分圆分解：x^n - 1 = ∏_{d|n} Φ_d(x)
    //   Φ_d 用 Möbius 关系 Φ_d(x) = (x^d - 1) / ∏_{e|d, e<d} Φ_e(x) 精确相除得到。
    //   这一步覆盖 x^2-1 / x^3-1 / x^4-1 / x^6-1 / x^8-1 等，factor 与 solve 同时受益
    //   （solve 的第一步就是 factor）。
    // =================================================================
    static SymExpr cyclotomicPoly(int64_t d) {
        SymExpr num = (SymExpr::makeVar("x") ^ SymExpr(BigInt(d))) - SymExpr(BigInt(1));
        for (int64_t e = 1; e < d; ++e) {
            if (d % e == 0) {
                // ★ 必须用多项式精确除法。SymExpr 的 operator/ 只是乘倒数
                //   （会得到 (x^2-1)*(x-1)^(-1) 这种"没除干净"的形状，乘起来又还原成 x^4-1）。
                auto [q, r] = polyDiv(num, cyclotomicPoly(e), "x");
                if (r.isZero()) num = q;
                else num = num / cyclotomicPoly(e);
            }
        }
        return simplifyCore(num);
    }

    // 形如 c * x^n + b 或 c * x^n - b（b 为非零整数）时给出 (c, n, b, 是否减号)
    struct BinomialForm {
        bool ok = false;
        BigInt c{1};
        int64_t n = 0;
        BigInt b{1};
        bool minus = false;
    };

    static BinomialForm detectBinomial(const SymExpr& expr, const std::string& var) {
        BinomialForm r;
        if (!expr.ptr || expr.ptr->getType() != SymType::ADD) return r;
        auto add = static_cast<SymAdd*>(expr.ptr);
        if (add->args.size() != 2) return r;

        SymNode* powNode = nullptr;
        SymNode* constNode = nullptr;
        for (SymNode* t : add->args) {
            if (t->getType() == SymType::NUM) { if (constNode) return r; constNode = t; }
            else { if (powNode) return r; powNode = t; }
        }
        if (!powNode || !constNode) return r;

        BigInt c(1);
        SymNode* base = powNode;
        if (powNode->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(powNode);
            std::vector<SymNode*> rest;
            for (SymNode* m : mul->args) {
                if (m->getType() == SymType::NUM) {
                    auto [isI, v] = extractExactInt(static_cast<SymNum*>(m)->value);
                    if (!isI) return r;
                    c = c * BigInt(v);
                } else rest.push_back(m);
            }
            if (rest.size() != 1) return r;
            base = rest[0];
        }
        if (base->getType() != SymType::POW) return r;
        auto p = static_cast<SymPow*>(base);
        if (p->base->getType() != SymType::VAR) return r;
        if (static_cast<SymVar*>(p->base)->name != var) return r;
        auto [isN, n] = extractExactInt(static_cast<SymNum*>(p->exp)->value);
        if (!isN || n < 2) return r;

        auto [isB, b] = extractExactInt(static_cast<SymNum*>(constNode)->value);
        if (!isB || b == 0) return r;

        r.ok = true;
        r.c = c;
        r.n = n;
        if (b < 0) { r.b = BigInt(-b); r.minus = true; }
        else { r.b = BigInt(b); r.minus = false; }
        return r;
    }

    // 尝试按分圆分解 x^n ± 1；不成或规模过大则原样返回
    static SymExpr tryCyclotomicFactor(const SymExpr& expr, const std::string& var) {
        BinomialForm f = detectBinomial(expr, var);
        if (!f.ok) return expr;
        if (f.c != BigInt(1)) return expr;          // 带非平凡系数时留给其他路径
        if (f.n < 2 || f.n > 12) return expr;       // 规模上限：避免深层递归与系数爆炸
        if (f.b != BigInt(1)) return expr;          // 只做 x^n ± 1

        SymExpr x = SymExpr::makeVar(var);

        // ★ cyclotomicPoly(d) 返回的是【规范变元 x】下的 Φ_d(x)（该函数内部固定用
        //   makeVar("x") 构造，见它的说明）。这里必须把它换成真正的变元 var，否则
        //   任何变量名不是 x 的 v^n ± 1 都会返回一个含幽灵变量 x 的"分解"：
        //       factor(y^8 - 1)  →  (x + 1) * (x - 1) * (x^2 + 1) * (x^4 + 1)
        //   结果里根本没有 y —— 既不是原式，也不是它的任何改写。
        //   这个漏替换平时被 factorPolynomialCZ（一元整系数 Zassenhaus）挡在前面
        //   掩盖住了：轮到分圆这条路之前，y^n - 1 已经被 CZ 完整分解。只有当某个
        //   中间表达式 CZ 处理不了、落到这里时才会暴露。
        //   下面的 plus 分支本来就用的是 x，所以只有 minus 分支受影响。
        //   var 就是 "x" 时跳过 subs，省掉一次无谓的整树替换。
        auto phi = [&](int64_t d) -> SymExpr {
            SymExpr p = cyclotomicPoly(d);
            if (var == "x") return p;
            return simplifyCore(subs(p, "x", x));
        };

        SymExpr result(BigInt(1));
        if (f.minus) {
            // x^n - 1 = ∏_{d|n} Φ_d(x)
            for (int64_t d = 1; d <= f.n; ++d) {
                if (f.n % d == 0) result = result * phi(d);
            }
        } else {
            // x^n + 1：n 为偶数时自身不可约式（在 ℚ 上）之外可直接给 x^n+1 不变；
            // n 为奇数时 x^n + 1 = (x + 1) * ∏_{d|n, d>1} Φ_{2d}(x)
            if (f.n % 2 == 0) return expr;
            result = x + SymExpr(BigInt(1));
            for (int64_t d = 3; d <= f.n; d += 2) {
                if (f.n % d == 0) result = result * phi(2 * d);
            }
        }
        SymExpr simp = simplifyCore(result);
        if (simp.ptr == expr.ptr) return expr;
        return simp;
    }

    // =================================================================
    // 精确 k 次方根
    //   与 trySquareRoot 的分工：那个还负责"部分开方"（√8 → 2√2），
    //   这里只做**严格**的恒等式识别（A^3 ± B^3、Sophie Germain 的 a^4 + 4b^4）。
    //   必须严格 —— 认错一次就是把恒等式套到别的多项式上，给出静默错误的因式分解。
    // =================================================================

    // 小指数整数幂（指数就是 3 或 4 这类常数，直接连乘）
    static BigInt ipowBig(BigInt b, int64_t k) {
        BigInt r(1);
        for (int64_t i = 0; i < k; ++i) r = r * b;
        return r;
    }

    // n 是否为完全 k 次幂；是则 out = n^(1/k)。n <= 0 一律不算（符号由调用方处理）。
    static bool exactIntRoot(const BigInt& n, int64_t k, BigInt& out) {
        if (n <= BigInt(0) || k < 2) return false;
        if (n == BigInt(1)) { out = BigInt(1); return true; }
        // 指数级扩上界，避免 hi = n 时对大整数反复做 k 次乘法
        BigInt lo(1), hi(2);
        while (ipowBig(hi, k) <= n) hi = hi * BigInt(2);   // 保证 hi^k > n
        while (hi - lo > BigInt(1)) {                      // 不变式：lo^k <= n < hi^k
            BigInt mid = (lo + hi) / BigInt(2);
            if (ipowBig(mid, k) <= n) lo = mid; else hi = mid;
        }
        // 循环结束时 hi = lo + 1 且 lo^k <= n < hi^k，n 是完全 k 次幂 ⟺ lo^k == n
        //   ★ 必须查 lo：只查 hi 会让 27、81 这类"上界恰好取到 2 的幂的邻居"被漏掉
        //     （27 的搜索区间停在 lo=3, hi=4），于是 27*y^3 判不出立方，
        //     8*x^3 + 27*y^3 这类多元立方和就分解不出来。
        if (ipowBig(lo, k) == n) { out = lo; return true; }
        return false;
    }

    // 找到 B 使 B^k == expr；找不到返回 false。整数、分数、幂、乘积都逐层下钻。
    static std::pair<bool, SymExpr> tryExactRoot(const SymExpr& expr, int64_t k) {
        if (!expr.ptr || k < 2) return { false, expr };
        switch (expr.ptr->getType()) {
            case SymType::NUM: {
                const CASVal& v = static_cast<SymNum*>(expr.ptr)->value;
                if (std::holds_alternative<Fraction>(v)) {
                    Fraction f = std::get<Fraction>(v);
                    BigInt rn, rd;
                    if (!exactIntRoot(f.getNum(), k, rn)) return { false, expr };
                    if (!exactIntRoot(f.getDen(), k, rd)) return { false, expr };
                    return { true, SymExpr(rn) / SymExpr(rd) };
                }
                auto [isInt, n] = extractExactInt(v);
                if (!isInt) return { false, expr };
                BigInt r;
                if (!exactIntRoot(BigInt(n), k, r)) return { false, expr };
                return { true, SymExpr(r) };
            }
            case SymType::POW: {
                auto p = static_cast<SymPow*>(expr.ptr);
                if (p->exp->getType() != SymType::NUM) return { false, expr };
                auto [isInt, e] = extractExactInt(static_cast<SymNum*>(p->exp)->value);
                if (!isInt || e <= 0 || e % k != 0) return { false, expr };
                return { true, SymExpr(p->base) ^ SymExpr(BigInt(e / k)) };
            }
            case SymType::MUL: {
                // 乘积逐因子开根，全部成功才算成功：x^3 * y^3 = (x*y)^3
                SymExpr result(BigInt(1));
                for (SymNode* arg : static_cast<SymMul*>(expr.ptr)->args) {
                    auto [ok, root] = tryExactRoot(SymExpr(arg), k);
                    if (!ok) return { false, expr };
                    result = result * root;
                }
                return { true, result };
            }
            default:
                // VAR / CONST / ADD / FUNC：不是完全 k 次幂
                return { false, expr };
        }
    }

    // 把加法项拆成「符号 + 绝对值节点」：-8*x^3 → (true, 8*x^3)，8*x^3 → (false, 8*x^3)。
    // 负号总是落在数值因子上（同 compareForPrint 对乘法的约定）。
    static void splitSign(SymNode* node, bool& neg, SymNode*& mag) {
        neg = false;
        mag = node;
        if (!node) return;
        bool negative = false;
        if (node->getType() == SymType::NUM) {
            negative = isCasNegative(static_cast<SymNum*>(node)->value);
        } else if (node->getType() == SymType::MUL) {
            for (SymNode* m : static_cast<SymMul*>(node)->args) {
                if (m->getType() == SymType::NUM && isCasNegative(static_cast<SymNum*>(m)->value)) {
                    negative = true;
                    break;
                }
            }
        }
        if (negative) {
            neg = true;
            mag = (-SymExpr(node)).ptr;
        }
    }

    // =================================================================
    // 立方和/差：A^3 ± B^3 = (A ± B)(A^2 ∓ A*B + B^2)
    //   覆盖二项式分圆分解管不到的多元情形（x^3 + y^3 不是 x 的单变量二项式）。
    //
    //   ★ 回归：原 asCube 对 MUL 分支把"非数值因子本身"当成了立方根，
    //     于是 8*x^3 被读成 (2*x^3)^3 = 8*x^9。cas.factor(8*x^3 + 27*y^3)
    //     因此返回的是 8*x^9 + 27*y^9 的分解 —— 静默错答，而不是漏分解。
    //     现在统一委托 tryExactRoot(·, 3)，"底数"与"立方根"不再混淆。
    // =================================================================
    static SymExpr tryCubeIdentity(const SymExpr& expr) {
        if (!expr.ptr || expr.ptr->getType() != SymType::ADD) return expr;
        auto add = static_cast<SymAdd*>(expr.ptr);
        if (add->args.size() != 2) return expr;

        bool negA = false, negB = false;
        SymNode *magA = nullptr, *magB = nullptr;
        splitSign(add->args[0], negA, magA);
        splitSign(add->args[1], negB, magB);

        auto [okA, A] = tryExactRoot(SymExpr(magA), 3);
        if (!okA) return expr;
        auto [okB, B] = tryExactRoot(SymExpr(magB), 3);
        if (!okB) return expr;

        if (negA == negB) {
            // 同为 + 或同为 -：A^3 + B^3，两项都带负号时整体提负
            SymExpr res = (A + B) * (A * A - A * B + B * B);
            if (negA) res = -res;
            return simplifyCore(res);
        }
        // 一正一负 → 差：让 A 是正项，再用 A^3 - B^3 = (A - B)(A^2 + A*B + B^2)
        if (negA) std::swap(A, B);
        return simplifyCore((A - B) * (A * A + A * B + B * B));
    }

    // =================================================================
    // Sophie Germain 恒等式：a^4 + 4b^4 = (a^2 - 2ab + 2b^2)(a^2 + 2ab + 2b^2)
    //   x^4 + 4y^4 在 Q[x,y] 上可约，但它是二元四次：
    //   factorPolynomialCZ 只管一元，multivariatePolynomialFactor 只处理二次，
    //   两条路都够不到，于是 x^4 + 4y^4 一直被原样返回。
    // =================================================================
    static SymExpr trySophieGermain(const SymExpr& expr) {
        if (!expr.ptr || expr.ptr->getType() != SymType::ADD) return expr;
        auto add = static_cast<SymAdd*>(expr.ptr);
        if (add->args.size() != 2) return expr;

        // 两项不对称（一项是四次幂、另一项是 4·四次幂），两个方向都试
        for (int order = 0; order < 2; ++order) {
            SymNode* fourthTerm = add->args[order];
            SymNode* quadTerm = add->args[1 - order];
            auto [okX, X] = tryExactRoot(SymExpr(fourthTerm), 4);
            if (!okX) continue;
            // 第二项必须是 4 * Y^4
            if (!quadTerm) continue;
            auto [okY, Y] = tryExactRoot(SymExpr(quadTerm) / SymExpr(BigInt(4)), 4);
            if (!okY) continue;

            SymExpr twoXY = SymExpr(BigInt(2)) * X * Y;
            SymExpr X2 = X * X, Y2 = SymExpr(BigInt(2)) * Y * Y;
            return simplifyCore(X2 - twoXY + Y2) * simplifyCore(X2 + twoXY + Y2);
        }
        return expr;
    }

    // =================================================================
    // A^2 + A*B + B^2 型三项式（当 A*B 恰为完全平方时）
    //   A^2 + A*B + B^2 = (A+B)^2 - A*B，而 A*B = S^2 时就是平方差：
    //     = (A + B - S)(A + B + S)
    //   例：x^4 + x^2*y^2 + y^4 = (x^2 + y^2 - x*y)(x^2 + y^2 + x*y)
    //   这一式正是 x^6 - y^6 分解的中转产物（立方差先给出
    //   (x^2 - y^2)(x^4 + x^2*y^2 + y^4)），不补上就只能停在半路。
    //   A*B 不是完全平方时（x^2 + x*y + y^2、4x^2 + 6x*y + 9y^2）本式在 Q 上
    //   不可约，必须原样返回 —— 那两个正是 Φ_3 的齐次形式，认错会把不可约式拆坏。
    // =================================================================
    static SymExpr tryQuadraticCyclotomic(const SymExpr& expr) {
        if (!expr.ptr || expr.ptr->getType() != SymType::ADD) return expr;
        auto add = static_cast<SymAdd*>(expr.ptr);
        if (add->args.size() != 3) return expr;

        // 挑两项当 A^2 与 B^2（6 种有序取法），剩下那项必须恰好等于 A*B
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                if (i == j) continue;
                const int k = 3 - i - j;
                auto [okA, A] = tryExactRoot(SymExpr(add->args[i]), 2);
                if (!okA) continue;
                auto [okB, B] = tryExactRoot(SymExpr(add->args[j]), 2);
                if (!okB) continue;

                SymExpr AB = A * B;
                SymNode* mid = add->args[k];
                if (AB.ptr != mid &&
                    !(AB.ptr->getType() == mid->getType() && AB.ptr->equals(mid))) continue;

                auto [okS, S] = tryExactRoot(AB, 2);
                if (!okS) continue;

                SymExpr sum = A + B;
                return simplifyCore(sum - S) * simplifyCore(sum + S);
            }
        }
        return expr;
    }

    // =================================================================
    // factor 记忆化
    //   factor 是纯函数（无随机性、无全局可变状态），结果又是内部化的 SymExpr，
    //   所以按 (节点, depth) 缓存是安全的。实测同一轮化简里 factor 的重复率高得
    //   离谱：24800 次调用只对应 33 个不同的 (节点, depth)，比值 751×。
    //   缓存在每次顶层 full_simplify 入口清空（与那里已有的递归缓存同生命周期），
    //   并设容量上限兜底，避免像内部化池那样无界增长。
    // =================================================================
    struct FactorMemoKey {
        const SymNode* node;
        int depth;
        bool operator==(const FactorMemoKey& o) const { return node == o.node && depth == o.depth; }
    };
    struct FactorMemoHash {
        size_t operator()(const FactorMemoKey& k) const {
            return std::hash<const void*>{}(k.node) * 1099511628211ULL ^ static_cast<size_t>(k.depth);
        }
    };
    static thread_local std::unordered_map<FactorMemoKey, SymExpr, FactorMemoHash> g_factorMemo;
    static constexpr size_t kFactorMemoCap = 200000;

    // =================================================================
    // 「子树里是否含 ADD」的 DAG 记忆化查询
    //   factor 的四类恒等式与公因式提取都以「存在一个加法节点」为前提：
    //     立方和差       A^3 ± B^3        要那个 ± 对应的加法节点
    //     Sophie Germain a^4 + 4*b^4      同上
    //     分圆分解       x^n - 1          同上
    //     二次分圆       A^2 + A*B + B^2  同上
    //     公因式提取     对 ADD 各项比较最小指数
    //   两条多项式策略同样要先 extractCoeffs 出 ≥3 个系数（次数 <2 直接放弃），
    //   而纯单项式乘积（x^2*y^3、45*x^8、sin(x)*cos(x)、x^(-1)*y^(-1)）无论走
    //   哪条路都原样返回。
    //   所以「整棵子树没有任何 ADD」是 factor 恒等的充分条件，可以安全短路。
    //   节点已内部化，memo 保证每个节点只算一次，总代价 O(DAG 节点数)，不是
    //   按树展开的 O(n²)；与 factor 记忆化同生命周期，在顶层 full_simplify
    //   入口一起清空，不跨调用累积。
    // =================================================================
    static thread_local std::unordered_map<const SymNode*, bool> g_hasAddMemo;

    static bool nodeContainsAdd(SymNode* node) {          // NOLINT(misc-no-recursion)
        if (!node) return false;
        if (node->getType() == SymType::ADD) return true;
        auto it = g_hasAddMemo.find(node);
        if (it != g_hasAddMemo.end()) return it->second;
        bool r = false;
        switch (node->getType()) {
            case SymType::MUL:
                for (SymNode* a : static_cast<SymMul*>(node)->args)
                    if (nodeContainsAdd(a)) { r = true; break; }
                break;
            case SymType::POW: {
                auto p = static_cast<SymPow*>(node);
                r = nodeContainsAdd(p->base) || nodeContainsAdd(p->exp);
                break;
            }
            case SymType::FUNC:
                for (SymNode* a : static_cast<SymFunc*>(node)->args)
                    if (nodeContainsAdd(a)) { r = true; break; }
                break;
            default: break;
        }
        g_hasAddMemo.emplace(node, r);
        return r;
    }

    void clearFactorMemo() { g_factorMemo.clear(); g_hasAddMemo.clear(); }

    static SymExpr factorImpl(const SymExpr& expr, int depth);

    SymExpr factor(const SymExpr& expr, int depth) {
        if (!expr.ptr || depth > SymConfig::maxDepth) return expr;
        SymType t = expr.ptr->getType();
        if (t == SymType::NUM || t == SymType::VAR || t == SymType::CONST) return expr;  // 叶节点
        if (!nodeContainsAdd(expr.ptr)) return expr;   // 无加法 ⇒ 没有任何策略能改变它

        FactorMemoKey key{expr.ptr, depth};
        auto it = g_factorMemo.find(key);
        if (it != g_factorMemo.end()) return it->second;

        SymExpr result = factorImpl(expr, depth);
        if (g_factorMemo.size() >= kFactorMemoCap) g_factorMemo.clear();
        g_factorMemo.emplace(key, result);
        return result;
    }

    // 真正的分解流程。叶节点短路、深度上限与记忆化都由入口 factor() 负责，
    // 所以这里不再重复那两项检查。
    static SymExpr factorImpl(const SymExpr& expr, int depth) {
        SymExpr quadResult = multivariatePolynomialFactor(expr, depth);  // 接入 depth
        if (quadResult.ptr != expr.ptr) return quadResult;
        
        SymExpr czResult = factorPolynomialCZ(expr, depth);
        if (czResult.ptr != expr.ptr) return czResult;

        // ★ 二项式分圆分解：x^n - 1 = ∏_{d|n} Φ_d(x)（以及奇数次 x^n + 1）
        {
            std::set<std::string> vars;
            collectAllVars(expr.ptr, vars);
            if (vars.size() == 1) {
                SymExpr cyc = tryCyclotomicFactor(expr, *vars.begin());
                if (cyc.ptr != expr.ptr) return cyc;
            }
        }

        // ★ 立方和/差（多元）：A^3 ± B^3
        //   结果里可能还有可分解的因子（x^6 - y^6 → (x^2 - y^2)(x^4 + x^2*y^2 + y^4)），
        //   所以再递归一轮；恒等式每用一次次数严格下降，不会打转。
        {
            SymExpr cube = tryCubeIdentity(expr);
            if (cube.ptr != expr.ptr) return factor(cube, depth + 1);
        }

        // ★ Sophie Germain（多元）：a^4 + 4*b^4
        {
            SymExpr sg = trySophieGermain(expr);
            if (sg.ptr != expr.ptr) return factor(sg, depth + 1);
        }

        // ★ A^2 + A*B + B^2 型三项式（A*B 为完全平方）
        {
            SymExpr tri = tryQuadraticCyclotomic(expr);
            if (tri.ptr != expr.ptr) return factor(tri, depth + 1);
        }

        // ══════════════════════════════════════════
        // 递归处理非加法节点
        // ══════════════════════════════════════════
        if (expr.ptr->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr res(BigInt(1));
            for (auto& arg : mul->args) res = res * factor(SymExpr(arg));
            return res;
        }
        if (expr.ptr->getType() == SymType::POW) {
            auto p = static_cast<SymPow*>(expr.ptr);
            return factor(SymExpr(p->base)) ^ factor(SymExpr(p->exp));
        }
        if (expr.ptr->getType() == SymType::FUNC) {
            auto f = static_cast<SymFunc*>(expr.ptr);
            std::vector<SymNode*> nArgs;
            for (auto& arg : f->args) nArgs.push_back(factor(SymExpr(arg)).ptr);
            return SymExpr::makeFunc(f->name, std::move(nArgs));
        }

        // ══════════════════════════════════════════
        // 阶段 2：公因式提取（对加法节点）
        // ══════════════════════════════════════════
        if (expr.ptr->getType() == SymType::ADD) {
            auto add = static_cast<SymAdd*>(expr.ptr);
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
                        auto [isInt, n] = extractExactInt(static_cast<SymNum*>(f.ptr)->value);
                        if (isInt) pt.coeff = pt.coeff * BigInt(n);
                        else pt.syms[f.toString()] = { f, 1 };
                    }
                    else if (f.ptr->getType() == SymType::POW) {
                        auto p = static_cast<SymPow*>(f.ptr);
                        if (p->exp->getType() == SymType::NUM) {
                            auto [isInt, n] = extractExactInt(static_cast<SymNum*>(p->exp)->value);
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
                    for (auto& f : static_cast<SymMul*>(arg)->args)
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
                auto mul = static_cast<SymMul*>(e.ptr);
                SymExpr res(BigInt(1));
                for (auto& arg : mul->args) res = res * process(SymExpr(arg));
                return res;
            }
            if (e.ptr->getType() == SymType::POW) {
                auto powNode = static_cast<SymPow*>(e.ptr);
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
                        SymExpr delta = simplifyCore(expand_core(B * B - SymExpr(BigInt(4)) * A * C, SymConfig::maxExpandTerms));

                        bool isNeg = false;
                        if (delta.ptr->getType() == SymType::NUM) {
                            isNeg = isCasNegative(static_cast<SymNum*>(delta.ptr)->value);
                        } else if (delta.ptr->getType() == SymType::MUL) {
                            auto mul = static_cast<SymMul*>(delta.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                isNeg = isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
                            }
                        }

                        if (!isNeg) {
                            SymExpr sqrtDelta;
                            auto [ok, exactSqrt] = trySquareRoot(delta, true);
                            if (ok) sqrtDelta = exactSqrt;
                            else sqrtDelta = delta ^ SymExpr(Fraction(1, 2));

                            SymExpr twoA = SymExpr(BigInt(2)) * A;
                            // 二次公式只除不约，用 simplifyRational 把系数约掉，
                            // 否则会留下 1/2 * (2*y + 2) 这种读不出来的根（同策略 2）。
                            SymExpr r1 = simplifyRational((-B + sqrtDelta) / twoA);
                            SymExpr r2 = simplifyRational((-B - sqrtDelta) / twoA);

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
                        SymExpr delta = simplifyCore(expand_core(B * B - SymExpr(BigInt(4)) * A * C, SymConfig::maxExpandTerms));
                        
                        bool isNeg = false;
                        if (delta.ptr->getType() == SymType::NUM) {
                            isNeg = isCasNegative(static_cast<SymNum*>(delta.ptr)->value);
                        } else if (delta.ptr->getType() == SymType::MUL) {
                            auto mul = static_cast<SymMul*>(delta.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                isNeg = isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
                            }
                        }
                        
                        if (!isNeg) {
                            SymExpr sqrtDelta;
                            auto [ok, exactSqrt] = trySquareRoot(delta, true);
                            if (ok) sqrtDelta = exactSqrt;
                            else sqrtDelta = delta ^ SymExpr(Fraction(1, 2));

                            SymExpr twoA = SymExpr(BigInt(2)) * A;
                            // 同上：双二次是"关于 X^2 的二次"，同样只除不约。
                            SymExpr u1 = simplifyRational((-B + sqrtDelta) / twoA);
                            SymExpr u2 = simplifyRational((-B - sqrtDelta) / twoA);
                            
                            SymExpr f1 = X * X - u1;
                            SymExpr f2 = X * X - u2;
                            
                            if (A.isOne()) return normalizeRationalFactors(process(f1) * process(f2));
                            return normalizeRationalFactors(A * process(f1) * process(f2));
                        } else {
                            // delta < 0, 必定有 A 和 C 同号，可配方为平方差
                            SymExpr CA = simplifyCore(C / A);
                            auto [ok, sqrtCA] = trySquareRoot(CA, true);
                            SymExpr rootCA = ok ? sqrtCA : (CA ^ SymExpr(Fraction(1, 2)));
                            
                            SymExpr middle = simplifyCore(SymExpr(BigInt(2)) * rootCA - B / A);
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
                        SymExpr BA = simplifyCore(B / A);
                        
                        bool isPos = false;
                        bool isNeg = false;
                        if (BA.ptr->getType() == SymType::NUM) {
                            isNeg = isCasNegative(static_cast<SymNum*>(BA.ptr)->value);
                            isPos = !isNeg && !BA.isZero();
                        } else if (BA.ptr->getType() == SymType::MUL) {
                            auto mul = static_cast<SymMul*>(BA.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                isNeg = isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
                                isPos = !isNeg;
                            } else {
                                isPos = true;
                            }
                        } else {
                            isPos = true;
                        }

                        SymExpr absBA = isNeg ? simplifyCore(-BA) : BA;
                        SymExpr R = simplifyCore(absBA ^ SymExpr(Fraction(1, degree)));
                        SymExpr res = A;
                        int n = degree;

                        auto getExactCos = [](int m, int n_val) -> SymExpr {
                            int g = static_cast<int>(BigInt::gcd(BigInt(std::abs(m)), BigInt(n_val)).toFloat());
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
                            
                            SymExpr theta = simplifyCore(SymExpr(Fraction(m, n_val)) * SymExpr::makeConst(SymConstId::Pi));
                            return simplifyCore(SymExpr::makeFunc("cos", std::vector<SymNode*>{theta.ptr}));
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
                            std::function<void(SymNode*)> checkComplex = [&](SymNode* node) {
                                if (!node || hasComplex) return;
                                // i 现在是 SymType::CONST 常量节点，不再是名为 "i" 的变量
                                if (node->getType() == SymType::CONST) {
                                    if (static_cast<SymConst*>(node)->id == SymConstId::I) hasComplex = true;
                                } else if (node->getType() == SymType::VAR) {
                                    auto varNode = static_cast<SymVar*>(node);
                                    if (varNode->name == "i" || varNode->name == "I") hasComplex = true;
                                } else if (node->getType() == SymType::ADD) {
                                    for (auto& arg : static_cast<SymAdd*>(node)->args) checkComplex(arg);
                                } else if (node->getType() == SymType::MUL) {
                                    for (auto& arg : static_cast<SymMul*>(node)->args) checkComplex(arg);
                                } else if (node->getType() == SymType::POW) {
                                    auto powNode = static_cast<SymPow*>(node);
                                    if (powNode->base->getType() == SymType::NUM && powNode->exp->getType() == SymType::NUM) {
                                        auto baseNum = static_cast<SymNum*>(powNode->base);
                                        auto expNum = static_cast<SymNum*>(powNode->exp);
                                        if (isCasNegative(baseNum->value)) {
                                            auto [isInt, n_val] = extractExactInt(expNum->value);
                                            if (!isInt) hasComplex = true;
                                        }
                                    }
                                    checkComplex(powNode->base);
                                    checkComplex(powNode->exp);
                                } else if (node->getType() == SymType::FUNC) {
                                    for (auto& arg : static_cast<SymFunc*>(node)->args) checkComplex(arg);
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
