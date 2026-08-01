#include "Integration.h"
#include "SymRules.h"
#include "Factorization.h"
#include "Groebner.h"
#include <stdexcept>
#include <iostream>

#include <algorithm>
#include <numeric>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <optional>
#include <unordered_set>

namespace jc {

    // =================================================================
    // Rothstein-Trager 算法 (Rothstein-Trager Algorithm)
    // 用于积分 A/D，其中 D 无平方，deg(A) < deg(D)
    // =================================================================
    static SymExpr rothsteinTrager(SymExpr A, SymExpr D, const std::string& var) {
        if (SymConfig::debugIntegration) std::cout << "   [RT] A: " << A.toString() << ", D: " << D.toString() << std::endl;

        SymExpr z = SymExpr::makeVar("_z");
        SymExpr dD = diff(D, var);
        SymExpr P = simplifyCore(expand_core(A - z * dD, SymConfig::maxExpandTerms));
        if (SymConfig::debugIntegration) std::cout << "   [RT] P(x,z) = A - z*D': " << P.toString() << std::endl;
        
        // 确保 polyResultant 的第一个参数次数 >= 第二个参数
        SymExpr R_z;
        if (getDegree(D, var) >= getDegree(P, var)) {
            R_z = polyResultant(D, P, var);
        } else {
            R_z = polyResultant(P, D, var);
        }
        if (SymConfig::debugIntegration) std::cout << "   [RT] Raw Resultant R_z: " << R_z.toString() << std::endl;
        R_z = getFraction(R_z).first; // 提取分子，消除伪除法引入的 z 分母
        if (SymConfig::debugIntegration) std::cout << "   [RT] Fraction-cleared R_z: " << R_z.toString() << std::endl;
        
        if (R_z.isZero() || getDegree(R_z, "_z") <= 0) {
            if (R_z.isZero()) {
                if (SymConfig::debugIntegration) std::cout << "   [RT] Resultant is zero!" << std::endl;
                throw std::runtime_error("Calculus Error: Rothstein-Trager failed to find roots of the resultant.");
            }
            if (SymConfig::debugIntegration) std::cout << "   [RT] Resultant degree <= 0, returning 0." << std::endl;
            return SymExpr(BigInt(0));
        }
        
        SymExpr result(BigInt(0));
        
        // Lazard-Rioboo-Trager 算法：通过无平方分解分离不同重数的根
        auto rSqFree = polySquareFree(R_z, "_z");
        if (SymConfig::debugIntegration) std::cout << "   [RT] rSqFree size: " << rSqFree.size() << std::endl;
        for (const auto& factor_sq : rSqFree) {
            SymExpr Q_k_full = factor_sq.first;
            if (SymConfig::debugIntegration) std::cout << "   [RT] Q_k_full(z): " << Q_k_full.toString() << " (multiplicity " << factor_sq.second << ")" << std::endl;
            if (getDegree(Q_k_full, "_z") <= 0) continue;
            
            // 提取首项系数并首一化，消除无关的复杂常数因子
            auto q_full_coeffs = extractCoeffs(Q_k_full, "_z");
            if (!q_full_coeffs.empty()) {
                SymExpr lead = q_full_coeffs.back();
                if (!lead.isZero() && !lead.isOne()) {
                    if (lead.ptr->getType() == SymType::NUM) {
                        Q_k_full = simplifyRational(Q_k_full / lead);
                    } else if (lead.ptr->getType() == SymType::MUL) {
                        auto mul = std::static_pointer_cast<SymMul>(lead.ptr);
                        if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                            Q_k_full = simplifyRational(Q_k_full / SymExpr(mul->args[0]));
                        }
                    }
                }
            }
            
            // 对 Q_k_full 进一步因式分解，提取有理根和低次因子
            SymExpr factored_Q = factor(Q_k_full);
            std::vector<SymExpr> q_factors;
            if (factored_Q.ptr->getType() == SymType::MUL) {
                for (auto& arg : std::static_pointer_cast<SymMul>(factored_Q.ptr)->args) {
                    if (arg->getType() == SymType::POW) {
                        q_factors.push_back(SymExpr(std::static_pointer_cast<SymPow>(arg)->base));
                    } else {
                        q_factors.push_back(SymExpr(arg));
                    }
                }
            } else if (factored_Q.ptr->getType() == SymType::POW) {
                q_factors.push_back(SymExpr(std::static_pointer_cast<SymPow>(factored_Q.ptr)->base));
            } else {
                q_factors.push_back(factored_Q);
            }
            
            for (const auto& Q_k : q_factors) {
                if (getDegree(Q_k, "_z") <= 0) continue;
                if (SymConfig::debugIntegration) std::cout << "   [RT] Processing irreducible factor Q_k(z): " << Q_k.toString() << std::endl;
                
                // 对每个重数 k 的因子 Q_k(z)，计算 D(x) 和 P(x, z) 模 Q_k(z) 的 GCD
                MultiPoly::clearRegistry();
                std::vector<MultiPoly> generators;
                generators.push_back(MultiPoly(P));
                generators.push_back(MultiPoly(D));
                generators.push_back(MultiPoly(Q_k));
            
            SymExpr v_z(BigInt(1));
            try {
                auto gb = computeGroebnerBasis(generators);
                if (SymConfig::debugIntegration) {
                    std::cout << "   [RT] Groebner Basis size: " << gb.size() << std::endl;
                    for (const auto& poly : gb) std::cout << "      GB poly: " << poly.toSymExpr().toString() << std::endl;
                }
                for (const auto& poly : gb) {
                    SymExpr p_expr = poly.toSymExpr();
                    if (containsVar(p_expr.ptr, var)) {
                        if (v_z.isOne() || getDegree(p_expr, var) < getDegree(v_z, var)) {
                            v_z = p_expr;
                        }
                    }
                }
            } catch (const EngineInterruptError&) {
                throw;
            } catch (...) {
                v_z = P;
            }
            if (SymConfig::debugIntegration) std::cout << "   [RT] v_z(x,z) before monic: " << v_z.toString() << std::endl;
            
            auto coeffs = extractCoeffs(v_z, var);
            if (!coeffs.empty()) {
                SymExpr lead = coeffs.back();
                if (!lead.isZero() && !lead.isOne()) {
                    if (lead.ptr->getType() == SymType::NUM) {
                        v_z = simplifyRational(v_z / lead);
                    } else if (lead.ptr->getType() == SymType::MUL) {
                        auto mul = std::static_pointer_cast<SymMul>(lead.ptr);
                        if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                            v_z = simplifyRational(v_z / SymExpr(mul->args[0]));
                        }
                    }
                }
            }
            if (SymConfig::debugIntegration) std::cout << "   [RT] v_z(x,z) after monic: " << v_z.toString() << std::endl;
            
            // 如果 Q_k 的度数 <= 2，尝试求出精确根，避免生成 RootSum
            // 对于 3 次及以上方程，如果无有理根，展开为根式会极其复杂，因此保留为 RootSum
            auto q_coeffs = extractCoeffs(Q_k, "_z");
            int q_deg = static_cast<int>(q_coeffs.size()) - 1;
            if (q_deg >= 1 && q_deg <= 2) {
                auto exactRoots = getExactRoots(q_coeffs);
                if (!exactRoots.empty()) {
                    std::vector<bool> used(exactRoots.size(), false);
                    for (size_t i = 0; i < exactRoots.size(); ++i) {
                        if (used[i]) continue;
                        
                        auto coeffs_i = extractCoeffs(exactRoots[i], "i");
                        bool is_complex = false;
                        SymExpr c(BigInt(0)), d(BigInt(0));
                        
                        SymExpr real_part(BigInt(0)), imag_part(BigInt(0));
                        for (size_t k = 0; k < coeffs_i.size(); ++k) {
                            if (k % 4 == 0) real_part = real_part + coeffs_i[k];
                            else if (k % 4 == 1) imag_part = imag_part + coeffs_i[k];
                            else if (k % 4 == 2) real_part = real_part - coeffs_i[k];
                            else if (k % 4 == 3) imag_part = imag_part - coeffs_i[k];
                        }
                        c = simplifyCore(real_part);
                        d = simplifyCore(imag_part);
                        
                        if (!d.isZero()) {
                            is_complex = true;
                        }
                        
                        if (is_complex) {
                            int conj_idx = -1;
                            for (size_t j = i + 1; j < exactRoots.size(); ++j) {
                                if (used[j]) continue;
                                auto coeffs_j = extractCoeffs(exactRoots[j], "i");
                                SymExpr r_part(BigInt(0)), i_part(BigInt(0));
                                for (size_t k = 0; k < coeffs_j.size(); ++k) {
                                    if (k % 4 == 0) r_part = r_part + coeffs_j[k];
                                    else if (k % 4 == 1) i_part = i_part + coeffs_j[k];
                                    else if (k % 4 == 2) r_part = r_part - coeffs_j[k];
                                    else if (k % 4 == 3) i_part = i_part - coeffs_j[k];
                                }
                                r_part = simplifyCore(r_part);
                                i_part = simplifyCore(i_part);
                                
                                if (r_part == c && i_part == simplifyCore(-d)) {
                                    conj_idx = static_cast<int>(j);
                                    break;
                                }
                            }
                            
                            if (conj_idx != -1) {
                                used[i] = true;
                                used[conj_idx] = true;
                                
                                SymExpr v_i_raw = subs(v_z, "_z", exactRoots[i]);
                                SymExpr v_i = simplifyRational(v_i_raw);
                                auto v_coeffs = extractCoeffs(v_i, "i");
                                SymExpr partA(BigInt(0)), partB(BigInt(0));
                                for (size_t k = 0; k < v_coeffs.size(); ++k) {
                                    if (k % 4 == 0) partA = partA + v_coeffs[k];
                                    else if (k % 4 == 1) partB = partB + v_coeffs[k];
                                    else if (k % 4 == 2) partA = partA - v_coeffs[k];
                                    else if (k % 4 == 3) partB = partB - v_coeffs[k];
                                }
                                partA = simplifyCore(partA);
                                partB = simplifyCore(partB);
                                
                                if (SymConfig::debugIntegration) {
                                    std::cout << "   [RT] Complex conjugate roots pair: " << c.toString() << " +/- " << d.toString() << "*i" << std::endl;
                                    std::cout << "   [RT] v_i(x) A: " << partA.toString() << ", B: " << partB.toString() << std::endl;
                                }
                                
                                if (!c.isZero()) {
                                    SymExpr log_term(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{simplify(partA*partA + partB*partB).ptr}));
                                    result = result + c * log_term;
                                }
                                if (!d.isZero() && !partA.isZero() && !partB.isZero()) {
                                    SymExpr atan_term(std::make_shared<SymFunc>("atan", std::vector<std::shared_ptr<SymNode>>{simplify(partB/partA).ptr}));
                                    result = result - SymExpr(BigInt(2)) * d * atan_term;
                                }
                                continue;
                            }
                        }
                        
                        used[i] = true;
                        SymExpr root = exactRoots[i];
                        if (SymConfig::debugIntegration) std::cout << "   [RT] Exact root: " << root.toString() << std::endl;
                        SymExpr v_i_raw = subs(v_z, "_z", root);
                        SymExpr v_i = simplifyRational(v_i_raw);
                        if (SymConfig::debugIntegration) std::cout << "   [RT] v_i(x): " << v_i.toString() << std::endl;
                        SymExpr log_vi(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{v_i.ptr}));
                        result = result + root * log_vi;
                    }
                    continue;
                }
            }
            
            SymExpr log_vz(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{v_z.ptr}));
            SymExpr expr_z = z * log_vz;
            
            SymExpr rootSum(std::make_shared<SymFunc>("RootSum", std::vector<std::shared_ptr<SymNode>>{
                expr_z.ptr, z.ptr, Q_k.ptr
            }));
            
            result = result + rootSum;
            }
        }
        if (SymConfig::debugIntegration) std::cout << "   [RT] Result before simplify: " << result.toString() << std::endl;
        
        SymExpr finalRes = simplifyCore(result);
        return finalRes;
    }

    // =================================================================
    // 🚀 Risch 算法核心组件 (Risch Algorithm Core Components)
    // =================================================================
    enum class RischExtType { LOG, EXP, ALG };

    struct RischExtension {
        RischExtType type = RischExtType::LOG;
        SymExpr arg;       // 扩张的参数 u (如 log(u) 或 exp(u) 中的 u)
        SymExpr deriv;     // 导数 t'
        std::string name;  // 变量名 (如 _t1)
        SymExpr t_var;     // 变量节点
        SymExpr minPoly;   // 极小多项式 (仅代数扩张使用，如 t^2 - P(x))
    };

    struct RischDiffField {
        std::string x_var;
        std::vector<RischExtension> tower;

        SymExpr derive(SymExpr expr, const std::string& exclude_var = "") const {
            SymExpr res = diff(expr, x_var);
            for (const auto& ext : tower) {
                if (ext.name != exclude_var && containsVar(expr.ptr, ext.name)) {
                    res = res + diff(expr, ext.name) * ext.deriv;
                }
            }
            return simplifyCore(expand_core(res, SymConfig::maxExpandTerms));
        }

        // 将表达式重写为微分域变量的代数式
        SymExpr rewrite(const SymExpr& expr) const {
            std::function<SymExpr(const SymExpr&)> preprocessPow = [&](const SymExpr& e) -> SymExpr {
                if (!e.ptr) return e;
                switch (e.ptr->getType()) {
                    case SymType::ADD: {
                        SymExpr r(BigInt(0));
                        for (auto& arg : std::static_pointer_cast<SymAdd>(e.ptr)->args) r = r + preprocessPow(SymExpr(arg));
                        return r;
                    }
                    case SymType::MUL: {
                        SymExpr r(BigInt(1));
                        for (auto& arg : std::static_pointer_cast<SymMul>(e.ptr)->args) r = r * preprocessPow(SymExpr(arg));
                        return r;
                    }
                    case SymType::POW: {
                        auto p = std::static_pointer_cast<SymPow>(e.ptr);
                        SymExpr base = preprocessPow(SymExpr(p->base));
                        SymExpr exp = preprocessPow(SymExpr(p->exp));
                        if (containsVar(exp.ptr, x_var)) {
                            SymExpr log_base(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{base.ptr}));
                            return SymExpr(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(exp * log_base).ptr}));
                        }
                        if (exp.ptr->getType() == SymType::NUM) {
                            auto numVal = std::static_pointer_cast<SymNum>(exp.ptr)->value;
                            if (std::holds_alternative<Fraction>(numVal)) {
                                Fraction frac = std::get<Fraction>(numVal);
                                if (frac.getDen() > BigInt(1)) {
                                    SymExpr ext_arg = base ^ SymExpr(Fraction(BigInt(1), frac.getDen()));
                                    for (auto it = tower.rbegin(); it != tower.rend(); ++it) {
                                        if (it->type == RischExtType::ALG && it->arg == ext_arg) {
                                            return it->t_var ^ SymExpr(frac.getNum());
                                        }
                                    }
                                }
                            }
                        }
                        return base ^ exp;
                    }
                    case SymType::FUNC: {
                        auto f = std::static_pointer_cast<SymFunc>(e.ptr);
                        std::vector<std::shared_ptr<SymNode>> nArgs;
                        for (auto& arg : f->args) nArgs.push_back(preprocessPow(SymExpr(arg)).ptr);
                        return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
                    }
                    default: return e;
                }
            };
            SymExpr res = preprocessPow(expr);
            
            // 从塔顶向下替换，确保嵌套扩张被正确处理 (如 exp(exp(x)))
            for (auto it = tower.rbegin(); it != tower.rend(); ++it) {
                if (it->type == RischExtType::LOG) {
                    SymExpr targetLog(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{it->arg.ptr}));
                    res = applyRule(res, targetLog, it->t_var);
                } else if (it->type == RischExtType::EXP) {
                    SymExpr targetExp(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{it->arg.ptr}));
                    res = applyRule(res, targetExp, it->t_var);
                } else if (it->type == RischExtType::ALG) {
                    res = applyRule(res, it->arg, it->t_var);
                }
            }
            return res;
        }
    };

    static void buildRischFieldRec(const SymExpr& expr, const std::string& x, RischDiffField& field, int& counter) {
        checkInterrupt();
        if (!expr.ptr) return;
        
        // 后序遍历：确保内层扩张先被加入塔中
        switch (expr.ptr->getType()) {
            case SymType::ADD:
                for (auto& arg : std::static_pointer_cast<SymAdd>(expr.ptr)->args) buildRischFieldRec(SymExpr(arg), x, field, counter);
                break;
            case SymType::MUL:
                for (auto& arg : std::static_pointer_cast<SymMul>(expr.ptr)->args) buildRischFieldRec(SymExpr(arg), x, field, counter);
                break;
            case SymType::POW: {
                auto p = std::static_pointer_cast<SymPow>(expr.ptr);
                if (containsVar(p->exp, x)) {
                    SymExpr log_base(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{p->base}));
                    SymExpr exp_node(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(SymExpr(p->exp) * log_base).ptr}));
                    buildRischFieldRec(exp_node, x, field, counter);
                } else {
                    buildRischFieldRec(SymExpr(p->base), x, field, counter);
                    buildRischFieldRec(SymExpr(p->exp), x, field, counter);
                    
                    if (p->exp->getType() == SymType::NUM && containsVar(p->base, x)) {
                        auto numVal = std::static_pointer_cast<SymNum>(p->exp)->value;
                        if (std::holds_alternative<Fraction>(numVal)) {
                            Fraction frac = std::get<Fraction>(numVal);
                            if (frac.getDen() > BigInt(1)) {
                                SymExpr base(p->base);
                                SymExpr ext_arg = base ^ SymExpr(Fraction(BigInt(1), frac.getDen()));
                                
                                bool exists = false;
                                for (const auto& ext : field.tower) {
                                    if (ext.type == RischExtType::ALG && ext.arg == ext_arg) {
                                        exists = true;
                                        break;
                                    }
                                }
                                
                                if (!exists) {
                                    RischExtension ext;
                                    ext.type = RischExtType::ALG;
                                    ext.arg = ext_arg;
                                    ext.name = "_t" + std::to_string(++counter);
                                    ext.t_var = SymExpr::makeVar(ext.name);
                                    
                                    ext.minPoly = (ext.t_var ^ SymExpr(frac.getDen())) - base;
                                    
                                    SymExpr dP_dx = diff(ext.minPoly, x);
                                    SymExpr dP_dt = diff(ext.minPoly, ext.name);
                                    ext.deriv = simplifyCore(-dP_dx / dP_dt);
                                    
                                    field.tower.push_back(ext);
                                }
                            }
                        }
                    }
                }
                break;
            }
            case SymType::FUNC: {
                auto f = std::static_pointer_cast<SymFunc>(expr.ptr);
                for (auto& arg : f->args) buildRischFieldRec(SymExpr(arg), x, field, counter);
                
                if (f->name == "log" || f->name == "exp") {
                    SymExpr arg(f->args[0]);
                    if (!containsVar(arg.ptr, x)) break; // 常数不构成扩张
                    
                    // 检查是否已在塔中
                    bool exists = false;
                    for (const auto& ext : field.tower) {
                        if ((ext.type == RischExtType::LOG && f->name == "log") ||
                            (ext.type == RischExtType::EXP && f->name == "exp")) {
                            if (ext.arg == arg) {
                                exists = true;
                                break;
                            }
                        }
                    }
                    
                    if (!exists) {
                        RischExtension ext;
                        ext.type = (f->name == "exp") ? RischExtType::EXP : RischExtType::LOG;
                        ext.arg = arg;
                        ext.name = "_t" + std::to_string(++counter);
                        ext.t_var = SymExpr::makeVar(ext.name);
                        
                        // 计算导数 t'
                        SymExpr du = diff(arg, x);
                        if (ext.type == RischExtType::LOG) {
                            ext.deriv = simplifyCore(du / arg);
                        } else {
                            ext.deriv = simplifyCore(du * ext.t_var);
                        }
                        
                        field.tower.push_back(ext);
                    }
                } else if (f->name == "RootOf" && f->args.size() == 3) {
                    SymExpr minPoly(f->args[0]);
                    SymExpr dummy(f->args[1]);
                    
                    if (!containsVar(minPoly.ptr, x)) break;
                    
                    bool exists = false;
                    for (const auto& ext : field.tower) {
                        if (ext.type == RischExtType::ALG && ext.arg == expr) {
                            exists = true;
                            break;
                        }
                    }
                    
                    if (!exists) {
                        RischExtension ext;
                        ext.type = RischExtType::ALG;
                        ext.arg = SymExpr(expr);
                        ext.name = "_t" + std::to_string(++counter);
                        ext.t_var = SymExpr::makeVar(ext.name);
                        
                        ext.minPoly = subs(minPoly, std::static_pointer_cast<SymVar>(dummy.ptr)->name, ext.t_var);
                        
                        SymExpr dP_dx = diff(ext.minPoly, x);
                        SymExpr dP_dt = diff(ext.minPoly, ext.name);
                        ext.deriv = simplifyCore(-dP_dx / dP_dt);
                        
                        field.tower.push_back(ext);
                    }
                }
                break;
            }
            default: break;
        }
    }

    static RischDiffField buildRischDifferentialField(const SymExpr& expr, const std::string& x) {
        RischDiffField field;
        field.x_var = x;
        int counter = 0;
        buildRischFieldRec(expr, x, field, counter);
        
        // 将导数重写为域变量的代数式
        for (auto& ext : field.tower) {
            ext.deriv = field.rewrite(ext.deriv);
        }
        return field;
    }

    // =================================================================
    // 🚀 Risch 算法 Step 2: Hermite 约化 (Hermite Reduction)
    // =================================================================
    static std::tuple<SymExpr, SymExpr, SymExpr, SymExpr> hermiteReduce(SymExpr A, SymExpr D, const std::string& var) {
        A = simplifyCore(expand_core(A, SymConfig::maxExpandTerms));
        D = simplifyCore(expand_core(D, SymConfig::maxExpandTerms));
        
        SymExpr gcd_AD = polyGCD(A, D, var);
        if (!gcd_AD.isOne() && !gcd_AD.isZero()) {
            A = polyDiv(A, gcd_AD, var).first;
            D = polyDiv(D, gcd_AD, var).first;
        }
        
        auto [Q, R] = polyDiv(A, D, var);
        SymExpr rationalPart(0);
        SymExpr polyPart = Q; 
        
        A = R;
        if (A.isZero()) return {rationalPart, polyPart, SymExpr(BigInt(0)), SymExpr(BigInt(1))};

        auto sqFree = polySquareFree(D, var);
        int m = 0;
        for (const auto& p : sqFree) m = std::max(m, p.second);

        SymExpr currentA = A;
        SymExpr currentD = D;

        for (int i = m; i >= 2; --i) {
            checkInterrupt();
            SymExpr V(1);
            for (auto& p : sqFree) {
                if (p.second == i) V = p.first;
            }
            if (getDegree(V, var) <= 0) continue;

            SymExpr Vi = simplifyCore(expand_core(V ^ SymExpr(i), SymConfig::maxExpandTerms));
            SymExpr U = polyDiv(currentD, Vi, var).first;

            SymExpr dV = diff(V, var);
            SymExpr U_dV = simplifyCore(expand_core(U * dV, SymConfig::maxExpandTerms));

            auto [gcd, S, T] = polyEGCD(U_dV, V, var);
            SymExpr factor_expr = simplifyCore(expand_core(currentA / gcd, SymConfig::maxExpandTerms));
            SymExpr B = simplifyCore(expand_core(S * factor_expr, SymConfig::maxExpandTerms));
            SymExpr C = simplifyCore(expand_core(T * factor_expr, SymConfig::maxExpandTerms));

            auto [q, r] = polyDiv(B, V, var);
            B = r;
            C = simplifyCore(expand_core(C + q * U_dV, SymConfig::maxExpandTerms));

            SymExpr V_im1 = simplifyCore(expand_core(V ^ SymExpr(i - 1), SymConfig::maxExpandTerms));
            SymExpr denom = simplifyCore(expand_core(SymExpr(i - 1) * V_im1, SymConfig::maxExpandTerms));
            rationalPart = rationalPart + simplifyCore(expand_core(-B / denom, SymConfig::maxExpandTerms));

            SymExpr dB = diff(B, var);
            SymExpr term2 = simplifyCore(expand_core((dB * U) / SymExpr(i - 1), SymConfig::maxExpandTerms));
            currentA = simplifyCore(expand_core(C + term2, SymConfig::maxExpandTerms));
            
            currentD = simplifyCore(expand_core(U * V_im1, SymConfig::maxExpandTerms));
            
            for (auto& p : sqFree) {
                if (p.second == i) p.second = i - 1;
            }
        }
        
        return {rationalPart, polyPart, currentA, currentD};
    }

    // =================================================================
    // 三角函数与复指数双向转换 (Trig <-> Exp)
    // =================================================================
    static SymExpr trigToExp(const SymExpr& expr) {
        if (!expr.ptr) return expr;
        switch (expr.ptr->getType()) {
            case SymType::ADD: {
                SymExpr res(BigInt(0));
                for (auto& arg : std::static_pointer_cast<SymAdd>(expr.ptr)->args) res = res + trigToExp(SymExpr(arg));
                return res;
            }
            case SymType::MUL: {
                SymExpr res(BigInt(1));
                for (auto& arg : std::static_pointer_cast<SymMul>(expr.ptr)->args) res = res * trigToExp(SymExpr(arg));
                return res;
            }
            case SymType::POW: {
                auto p = std::static_pointer_cast<SymPow>(expr.ptr);
                return trigToExp(SymExpr(p->base)) ^ trigToExp(SymExpr(p->exp));
            }
            case SymType::FUNC: {
                auto f = std::static_pointer_cast<SymFunc>(expr.ptr);
                std::vector<std::shared_ptr<SymNode>> nArgs;
                for (auto& arg : f->args) nArgs.push_back(trigToExp(SymExpr(arg)).ptr);
                
                if (nArgs.size() == 1) {
                    SymExpr arg(nArgs[0]);
                    SymExpr I = SymExpr::makeVar("i");
                    if (f->name == "sin") {
                        SymExpr exp_ix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(I * arg).ptr}));
                        SymExpr exp_mix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-I * arg).ptr}));
                        return (exp_ix - exp_mix) / (SymExpr(BigInt(2)) * I);
                    }
                    if (f->name == "cos") {
                        SymExpr exp_ix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(I * arg).ptr}));
                        SymExpr exp_mix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-I * arg).ptr}));
                        return (exp_ix + exp_mix) / SymExpr(BigInt(2));
                    }
                    if (f->name == "tan") {
                        SymExpr exp_ix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(I * arg).ptr}));
                        SymExpr exp_mix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-I * arg).ptr}));
                        return -I * (exp_ix - exp_mix) / (exp_ix + exp_mix);
                    }
                    if (f->name == "cot") {
                        SymExpr exp_ix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(I * arg).ptr}));
                        SymExpr exp_mix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-I * arg).ptr}));
                        return I * (exp_ix + exp_mix) / (exp_ix - exp_mix);
                    }
                    if (f->name == "sec") {
                        SymExpr exp_ix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(I * arg).ptr}));
                        SymExpr exp_mix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-I * arg).ptr}));
                        return SymExpr(BigInt(2)) / (exp_ix + exp_mix);
                    }
                    if (f->name == "csc") {
                        SymExpr exp_ix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(I * arg).ptr}));
                        SymExpr exp_mix(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-I * arg).ptr}));
                        return (SymExpr(BigInt(2)) * I) / (exp_ix - exp_mix);
                    }
                    if (f->name == "sinh") {
                        SymExpr exp_x(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                        SymExpr exp_mx(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-arg).ptr}));
                        return (exp_x - exp_mx) / SymExpr(BigInt(2));
                    }
                    if (f->name == "cosh") {
                        SymExpr exp_x(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                        SymExpr exp_mx(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-arg).ptr}));
                        return (exp_x + exp_mx) / SymExpr(BigInt(2));
                    }
                    if (f->name == "tanh") {
                        SymExpr exp_x(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                        SymExpr exp_mx(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-arg).ptr}));
                        return (exp_x - exp_mx) / (exp_x + exp_mx);
                    }
                    if (f->name == "coth") {
                        SymExpr exp_x(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                        SymExpr exp_mx(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-arg).ptr}));
                        return (exp_x + exp_mx) / (exp_x - exp_mx);
                    }
                    if (f->name == "sech") {
                        SymExpr exp_x(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                        SymExpr exp_mx(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-arg).ptr}));
                        return SymExpr(BigInt(2)) / (exp_x + exp_mx);
                    }
                    if (f->name == "csch") {
                        SymExpr exp_x(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                        SymExpr exp_mx(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{(-arg).ptr}));
                        return SymExpr(BigInt(2)) / (exp_x - exp_mx);
                    }
                }
                return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
            }
            default: return expr;
        }
    }

    static SymExpr expToTrig(const SymExpr& expr) {
        if (!expr.ptr) return expr;
        switch (expr.ptr->getType()) {
            case SymType::ADD: {
                SymExpr res(BigInt(0));
                for (auto& arg : std::static_pointer_cast<SymAdd>(expr.ptr)->args) res = res + expToTrig(SymExpr(arg));
                return res;
            }
            case SymType::MUL: {
                SymExpr res(BigInt(1));
                for (auto& arg : std::static_pointer_cast<SymMul>(expr.ptr)->args) res = res * expToTrig(SymExpr(arg));
                return res;
            }
            case SymType::POW: {
                auto p = std::static_pointer_cast<SymPow>(expr.ptr);
                return expToTrig(SymExpr(p->base)) ^ expToTrig(SymExpr(p->exp));
            }
            case SymType::FUNC: {
                auto f = std::static_pointer_cast<SymFunc>(expr.ptr);
                std::vector<std::shared_ptr<SymNode>> nArgs;
                for (auto& arg : f->args) nArgs.push_back(expToTrig(SymExpr(arg)).ptr);
                
                if (f->name == "exp" && nArgs.size() == 1) {
                    SymExpr arg(nArgs[0]);
                    auto coeffs = extractCoeffs(arg, "i");
                    if (coeffs.size() == 2) {
                        SymExpr A = coeffs[0];
                        SymExpr B = coeffs[1];
                        SymExpr cos_B(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{B.ptr}));
                        SymExpr sin_B(std::make_shared<SymFunc>("sin", std::vector<std::shared_ptr<SymNode>>{B.ptr}));
                        SymExpr I = SymExpr::makeVar("i");
                        SymExpr trig_part = cos_B + I * sin_B;
                        if (A.isZero()) return trig_part;
                        SymExpr exp_A(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{A.ptr}));
                        return exp_A * trig_part;
                    }
                }
                return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
            }
            default: return expr;
        }
    }

    // =================================================================
    // 🚀 Risch 算法核心 (Risch Algorithm Core)
    // =================================================================
    static SymExpr rischIntegrateCore(const SymExpr& expr, const std::string& var, int depth) {
        checkInterrupt();
        if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "-> Entering Risch Core with: " << expr.toString() << std::endl;
        // Step 0 - 刘维尔域规范化 (Liouvillian Field Normalization)
        SymExpr normalizedExpr = rischNormalize(expr);
        if (SymConfig::debugIntegration && normalizedExpr != expr) std::cout << std::string(depth * 2, ' ') << "   [Risch] Normalized: " << normalizedExpr.toString() << std::endl;

        // Step 1 - 构建微分域与刘维尔扩张
        RischDiffField field = buildRischDifferentialField(normalizedExpr, var);
        SymExpr rewritten = field.rewrite(normalizedExpr);
        if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Rewritten in diff field: " << rewritten.toString() << std::endl;
        
        // Step 1.5 - 单一扩张的变量代换降维打击 (Change of Variables for Single Extension)
        if (field.tower.size() == 1) {
            auto ext = field.tower[0];
            if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Trying Single Extension Substitution for " << ext.name << std::endl;
            
            // 形式 1: 积分恰好是 F(t) * t' dx = F(t) dt
            SymExpr F_t = simplifyCore(rewritten / ext.deriv);
            if (!containsVar(F_t.ptr, var)) {
                try {
                    SymExpr int_t = integrate(F_t, ext.name, depth + 1);
                    SymExpr orig_ext;
                    if (ext.type == RischExtType::LOG) {
                        orig_ext = SymExpr(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{ext.arg.ptr}));
                    } else if (ext.type == RischExtType::EXP) {
                        orig_ext = SymExpr(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{ext.arg.ptr}));
                    } else if (ext.type == RischExtType::ALG) {
                        orig_ext = ext.arg;
                    }
                    if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "<- Risch Single Extension (Form 1) Success" << std::endl;
                    return simplifyCore(subs(int_t, ext.name, orig_ext));
                } catch (const EngineInterruptError&) {
                    throw;
                } catch (...) {}
            }
            
            // 形式 2: 积分是 F(e^(ax+b)) dx，且 F 中不含 x
            if (ext.type == RischExtType::EXP && !containsVar(rewritten.ptr, var)) {
                SymExpr u_deriv = simplifyCore(diff(ext.arg, var));
                if (!containsVar(u_deriv.ptr, var) && !u_deriv.isZero()) {
                    try {
                        SymExpr integrand_t = simplifyCore(rewritten / (u_deriv * ext.t_var));
                        SymExpr int_t = integrate(integrand_t, ext.name, depth + 1);
                        SymExpr orig_ext(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{ext.arg.ptr}));
                        if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "<- Risch Single Extension (Form 2) Success" << std::endl;
                        return simplifyCore(subs(int_t, ext.name, orig_ext));
                    } catch (const EngineInterruptError&) {
                        throw;
                    } catch (...) {}
                }
            }
        }

        // Step 2 - Hermite 约化 (目前仅针对有理函数域 Q(x))
        if (field.tower.empty()) {
            if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Base field Q(x) detected, using Hermite Reduction." << std::endl;
            auto [num, den] = getFraction(rewritten);
            if (getDegree(den, var) > 0) {
                auto [ratPart, polyPart, cA, cD] = hermiteReduce(num, den, var);
            
                SymExpr res = ratPart;
                if (!polyPart.isZero()) {
                    res = res + integrate(polyPart, var, depth + 1);
                }
                
                if (!cA.isZero()) {
                    res = res + rothsteinTrager(cA, cD, var);
                }
                
                return res;
            }
        }
        
        // Step 4 & 5 - 扩张多项式积分 (Extension Polynomial Integration)
        if (!field.tower.empty()) {
            auto topExt = field.tower.back();
            if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Integrating over extension: " << topExt.name << " (Type: " << (int)topExt.type << ")" << std::endl;
            
            auto backSubstitute = [&](SymExpr res) {
                for (auto it = field.tower.rbegin(); it != field.tower.rend(); ++it) {
                    SymExpr orig;
                    if (it->type == RischExtType::LOG) {
                        orig = SymExpr(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{it->arg.ptr}));
                    } else if (it->type == RischExtType::EXP) {
                        orig = SymExpr(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{it->arg.ptr}));
                    } else if (it->type == RischExtType::ALG) {
                        orig = it->arg;
                    }
                    res = subs(res, it->name, orig);
                }
                return res;
            };

            if (topExt.type == RischExtType::LOG) {
                if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Step 4: Logarithmic Extension Polynomial Integration" << std::endl;
                auto coeffs = extractCoeffs(rewritten, topExt.name);
                if (!coeffs.empty()) {
                    int n = static_cast<int>(coeffs.size()) - 1;
                    std::vector<SymExpr> B(n + 2, SymExpr(BigInt(0)));
                    SymExpr t_deriv = topExt.deriv;
                    SymExpr t_sym = SymExpr::makeVar(topExt.name);

                    try {
                        SymExpr intAn_x = backSubstitute(coeffs[n]);
                        if (getAstNodeCount(intAn_x) > getAstNodeCount(expr) * 3 + 50) {
                            throw std::runtime_error("Risch Step 4: Leading coefficient expansion too large, aborting.");
                        }
                        SymExpr intAn = integrate(intAn_x, var, depth + 1);
                        SymExpr intAn_t = field.rewrite(trigToExp(intAn));
                        auto coeffs_intAn = extractCoeffs(intAn_t, topExt.name);
                        
                        SymExpr c(BigInt(0));
                        SymExpr B_n(BigInt(0));
                        if (coeffs_intAn.empty()) {
                            if (!intAn_t.isZero()) throw std::runtime_error("Non-elementary integral.");
                        } else if (coeffs_intAn.size() > 1) {
                            c = simplifyCore(coeffs_intAn[1] / SymExpr(BigInt(n + 1)));
                            B_n = coeffs_intAn[0];
                            if (coeffs_intAn.size() > 2) {
                                throw std::runtime_error("Non-elementary integral.");
                            }
                            if (containsVar(c.ptr, var)) {
                                throw std::runtime_error("Non-elementary integral: residue is not constant.");
                            }
                        } else {
                            B_n = coeffs_intAn[0];
                        }
                        
                        B[n + 1] = c;
                        B[n] = B_n;
                        
                        for (int i = n - 1; i >= 0; --i) {
                            checkInterrupt();
                            SymExpr integrand = simplifyCore(coeffs[i] - SymExpr(BigInt(i + 1)) * B[i + 1] * t_deriv);
                            SymExpr integrand_x = backSubstitute(integrand);
                            if (getAstNodeCount(integrand_x) > getAstNodeCount(expr) * 3 + 50) {
                                throw std::runtime_error("Risch Step 4: Lower coefficient expansion too large, aborting.");
                            }
                            SymExpr int_i = integrate(integrand_x, var, depth + 1);
                            SymExpr int_i_t = field.rewrite(trigToExp(int_i));
                            
                            auto coeffs_int_i = extractCoeffs(int_i_t, topExt.name);
                            if (coeffs_int_i.empty()) {
                                if (!int_i_t.isZero()) throw std::runtime_error("Non-elementary integral.");
                                B[i] = SymExpr(BigInt(0));
                            } else if (coeffs_int_i.size() > 2) {
                                throw std::runtime_error("Non-elementary integral.");
                            } else if (coeffs_int_i.size() == 2) {
                                SymExpr c_i = coeffs_int_i[1];
                                if (containsVar(c_i.ptr, var)) {
                                    throw std::runtime_error("Non-elementary integral: residue is not constant.");
                                }
                                B[i + 1] = simplifyCore(B[i + 1] + c_i / SymExpr(BigInt(i + 1)));
                                B[i] = coeffs_int_i[0];
                            } else {
                                B[i] = coeffs_int_i[0];
                            }
                        }
                        
                        SymExpr result(BigInt(0));
                        for (int i = 0; i <= n + 1; ++i) {
                            if (!B[i].isZero()) {
                                result = result + B[i] * (t_sym ^ SymExpr(BigInt(i)));
                            }
                        }
                        SymExpr finalRes = simplifyCore(backSubstitute(result));
                        if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "<- Risch Step 4 (LOG) Success" << std::endl;
                        return finalRes;
                    } catch (const EngineInterruptError&) {
                        throw;
                    } catch (...) {
                        // 失败则继续往下走，抛出调试信息
                    }
                }
            } else if (topExt.type == RischExtType::EXP) {
                if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Step 5: Exponential Extension Polynomial Integration" << std::endl;
                auto coeffs = extractCoeffs(rewritten, topExt.name);
                if (!coeffs.empty()) {
                    int n = static_cast<int>(coeffs.size()) - 1;
                    SymExpr u_deriv = simplifyCore(topExt.deriv / SymExpr::makeVar(topExt.name));
                    SymExpr t_sym = SymExpr::makeVar(topExt.name);
                    
                    bool success = true;
                    SymExpr result(BigInt(0));
                    
                    for (int i = 0; i <= n; ++i) {
                        checkInterrupt();
                        if (coeffs[i].isZero()) continue;
                        if (i == 0) {
                            try {
                                SymExpr integrand_x = backSubstitute(coeffs[0]);
                                if (getAstNodeCount(integrand_x) > getAstNodeCount(expr) * 3 + 50) {
                                    throw std::runtime_error("Risch Step 5: Constant term expansion too large, aborting.");
                                }
                                SymExpr int_0 = integrate(integrand_x, var, depth + 1);
                                result = result + field.rewrite(trigToExp(int_0));
                            } catch (const EngineInterruptError&) {
                                throw;
                            } catch (...) {
                                success = false;
                                break;
                            }
                        } else {
                            // 求解 Risch 微分方程 B_i' + i u' B_i = A_i (SPDE 多项式度数界限法)
                            SymExpr A_i = backSubstitute(coeffs[i]);
                            SymExpr V_i = backSubstitute(simplifyCore(SymExpr(BigInt(i)) * u_deriv));
                            
                            SymExpr B_i(BigInt(0));
                            SymExpr currentA = A_i;
                            bool rde_success = false;
                            
                            auto cV = extractCoeffs(V_i, var);
                            while (!cV.empty() && cV.back().isZero()) cV.pop_back();
                            
                            if (cV.empty()) {
                                // V_i 不是多项式，退化为常数除法尝试
                                SymExpr B_term = simplifyCore(currentA / V_i);
                                if (!containsVar(B_term.ptr, var)) {
                                    B_i = B_term;
                                    rde_success = true;
                                }
                            } else {
                                int degV = static_cast<int>(cV.size()) - 1;
                                SymExpr leadV = cV.back();
                                
                                // 计算多项式度数界限 b = deg(A) - deg(V)
                                int b_bound = -1;
                                auto cA_init = extractCoeffs(currentA, var);
                                while (!cA_init.empty() && cA_init.back().isZero()) cA_init.pop_back();
                                
                                if (!cA_init.empty()) {
                                    b_bound = static_cast<int>(cA_init.size()) - 1 - degV;
                                }
                                
                                if (b_bound >= 0) {
                                    for (int d = b_bound; d >= 0; --d) {
                                        if (currentA.isZero()) break;
                                        
                                        auto cA = extractCoeffs(currentA, var);
                                        while (!cA.empty() && cA.back().isZero()) cA.pop_back();
                                        if (cA.empty()) break;
                                        
                                        int degA = static_cast<int>(cA.size()) - 1;
                                        
                                        if (degA != d + degV) {
                                            if (degA > d + degV) break; // 异常升次，无多项式解
                                            continue; // 当前次项系数为0，跳过
                                        }
                                        
                                        SymExpr leadA = cA.back();
                                        SymExpr c_d = simplifyCore(leadA / leadV);
                                        SymExpr B_term = simplifyCore(c_d * (SymExpr::makeVar(var) ^ SymExpr(BigInt(d))));
                                        
                                        B_i = B_i + B_term;
                                        SymExpr B_term_deriv = field.derive(B_term);
                                        currentA = simplifyCore(expand_core(currentA - B_term_deriv - V_i * B_term, SymConfig::maxExpandTerms));
                                    }
                                }
                                
                                if (currentA.isZero()) {
                                    rde_success = true;
                                } else if (degV == 0) {
                                    auto cA_final = extractCoeffs(currentA, var);
                                    while (!cA_final.empty() && cA_final.back().isZero()) cA_final.pop_back();
                                    if (cA_final.empty()) {
                                        // 兜底常数项处理
                                        SymExpr B_term = simplifyCore(currentA / V_i);
                                        if (!containsVar(B_term.ptr, var)) {
                                            B_i = B_i + B_term;
                                            rde_success = true;
                                        }
                                    }
                                }
                            }
                            
                            if (rde_success) {
                                SymExpr B_i_t = field.rewrite(B_i);
                                result = result + B_i_t * (t_sym ^ SymExpr(BigInt(i)));
                            } else {
                                success = false;
                                break;
                            }
                        }
                    }
                    
                    if (success) {
                        SymExpr finalRes = simplifyCore(backSubstitute(result));
                        if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "<- Risch Step 5 (EXP) Success" << std::endl;
                        return finalRes;
                    }
                }
            } else if (topExt.type == RischExtType::ALG) {
                if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Algebraic Extension Integration" << std::endl;
                SymExpr minPoly = field.rewrite(topExt.minPoly);
                SymExpr t_sym = SymExpr::makeVar(topExt.name);
                
                auto [A, D] = getFraction(rewritten);
                if (SymConfig::debugIntegration) {
                    std::cout << std::string(depth * 2, ' ') << "   [Risch] A: " << A.toString() << ", D: " << D.toString() << std::endl;
                }
                
                // 1. 分母有理化 (Rationalize Denominator)
                // 使用 Gröbner 基求逆元，避免 polyEGCD 导致的有理函数系数指数级爆炸 (Coefficient Swell)
                MultiPoly::clearRegistry();
                std::vector<MultiPoly> generators;
                SymExpr z_inv = SymExpr::makeVar("~z_inv");
                MultiPoly poly_z_inv(z_inv); // 确保 ~z_inv 获得最高优先级
                
                // 清除 D 和 minPoly 的分母，转为纯多项式
                SymExpr D_clear = getFraction(D).first;
                SymExpr minPoly_clear = getFraction(minPoly).first;
                
                generators.push_back(MultiPoly(simplifyCore(expand_core(z_inv * D_clear - SymExpr(BigInt(1)), SymConfig::maxExpandTerms))));
                generators.push_back(MultiPoly(simplifyCore(expand_core(minPoly_clear, SymConfig::maxExpandTerms))));
                
                // 将底层所有代数扩张的极小多项式也加入理想，确保 Gröbner 基能在全代数域上正确约化
                for (const auto& ext : field.tower) {
                    if (ext.type == RischExtType::ALG && ext.name != topExt.name) {
                        SymExpr extMinPoly_clear = getFraction(field.rewrite(ext.minPoly)).first;
                        generators.push_back(MultiPoly(simplifyCore(expand_core(extMinPoly_clear, SymConfig::maxExpandTerms))));
                    }
                }
                
                std::vector<MultiPoly> rgb;
                try {
                    rgb = computeGroebnerBasis(generators);
                } catch (...) {}
                
                SymExpr invD(BigInt(0));
                bool found_inv = false;
                for (const auto& poly : rgb) {
                    if (poly.isZero()) continue;
                    SymExpr polyExpr = poly.toSymExpr();
                    if (containsVar(polyExpr.ptr, "~z_inv")) {
                        auto coeffs = extractCoeffs(polyExpr, "~z_inv");
                        if (coeffs.size() == 2) {
                            SymExpr c = coeffs[1];
                            // 确保提取出的逆元分母中不再包含当前扩张变量 topExt.name
                            if (!containsVar(c.ptr, topExt.name)) {
                                SymExpr U_gb = coeffs[0];
                                invD = simplifyCore(expand_core(-U_gb / c, SymConfig::maxExpandTerms));
                                found_inv = true;
                                break;
                            }
                        }
                    }
                }
                
                SymExpr res_D(BigInt(0));
                SymExpr newA = A;
                SymExpr newD = D;
                
                if (found_inv) {
                    // D_clear * invD = 1 (mod minPoly) => D * (D_clear / D) * invD = 1 => 1/D = (D_clear / D) * invD
                    SymExpr D_ratio = simplifyCore(D_clear / D);
                    SymExpr true_invD = simplifyCore(expand_core(D_ratio * invD, SymConfig::maxExpandTerms));
                    newA = simplifyCore(expand_core(A * true_invD, SymConfig::maxExpandTerms));
                    newD = SymExpr(BigInt(1));
                    res_D = SymExpr(BigInt(1));
                } else {
                    // Fallback to EGCD if GB fails (though it might swell)
                    auto [gcd_D, U, V] = polyEGCD(D, minPoly, topExt.name);
                    res_D = gcd_D; // 直接使用 EGCD 返回的 GCD，避免重新展开引入未化简的代数数
                    newA = simplifyCore(expand_core(A * U, SymConfig::maxExpandTerms));
                    newD = res_D;
                }
                
                if (SymConfig::debugIntegration) {
                    std::cout << std::string(depth * 2, ' ') << "   [Risch] Rationalized D: " << res_D.toString() << std::endl;
                }

                if (!res_D.isZero() && !containsVar(res_D.ptr, topExt.name)) {
                    
                    // 2. 伪化简 (Pseudo-reduction) 降次
                    auto [Q, R] = polyDiv(newA, minPoly, topExt.name);
                    SymExpr A_red = R;
                    SymExpr D_red = newD;
                    
                    if (SymConfig::debugIntegration) {
                        std::cout << std::string(depth * 2, ' ') << "   [Risch] Pseudo-reduced A_red: " << A_red.toString() << ", D_red: " << D_red.toString() << std::endl;
                    }

                    // 3. 纯根式扩张的完整 Risch 积分 (Complete Risch for Radical Extensions)
                    // 适用于 minPoly = t^n - P(x)
                    SymExpr P_x = simplifyCore(-subs(minPoly, topExt.name, SymExpr(BigInt(0))));
                    int n_deg = getDegree(minPoly, topExt.name);
                    bool is_radical = simplifyCore(minPoly - ((t_sym ^ SymExpr(BigInt(n_deg))) - P_x)).isZero();
                    
                    if (is_radical && n_deg > 0) {
                        if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Radical Extension detected: t^" << n_deg << " = " << P_x.toString() << std::endl;
                        
                        auto coeffs_R = extractCoeffs(R, topExt.name);
                        SymExpr alg_result(BigInt(0));
                        SymExpr log_integrand(BigInt(0));
                        bool radical_success = true;
                        
                        for (size_t i = 0; i < coeffs_R.size(); ++i) {
                            if (coeffs_R[i].isZero()) continue;
                            SymExpr A_i = coeffs_R[i];
                            SymExpr D_i = newD;
                            
                            if (i == 0) {
                                log_integrand = log_integrand + simplifyCore(A_i / D_i);
                                continue;
                            }
                            
                            // 积分 A_i / D_i * t^i
                            // 使用 Ostrogradsky-Hermite 约化提取代数部分
                            SymExpr current_A = A_i;
                            SymExpr current_D = D_i;
                            
                            auto sqFree = polySquareFree(current_D, var);
                            int m = 0;
                            for (const auto& p : sqFree) m = std::max(m, p.second);
                            
                            for (int j = m; j >= 2; --j) {
                                SymExpr V_j(1);
                                for (auto& p : sqFree) if (p.second == j) V_j = p.first;
                                if (getDegree(V_j, var) <= 0) continue;
                                
                                SymExpr V_prime = field.derive(V_j);
                                SymExpr P_prime = field.derive(P_x);
                                
                                // 解 A P = - (j-1) B V' P (mod V)
                                SymExpr rhs = simplifyCore(expand_core(current_A * P_x, SymConfig::maxExpandTerms));
                                SymExpr lhs_coeff = simplifyCore(expand_core(SymExpr(BigInt(-(j - 1))) * V_prime * P_x, SymConfig::maxExpandTerms));
                                
                                auto [gcd, S, T] = polyEGCD(lhs_coeff, V_j, var);
                                if (!gcd.isOne() && !gcd.isZero()) continue; // 极点在分支点上，保留给对数部分
                                
                                SymExpr B = polyDiv(simplifyCore(expand_core(S * rhs, SymConfig::maxExpandTerms)), V_j, var).second;
                                
                                // C = (A P - (B' V P - (j-1) B V' P + B V (i/n) P')) / (V P)
                                SymExpr B_prime = field.derive(B);
                                SymExpr term1 = simplifyCore(expand_core(B_prime * V_j * P_x, SymConfig::maxExpandTerms));
                                SymExpr term2 = simplifyCore(expand_core(SymExpr(BigInt(j - 1)) * B * V_prime * P_x, SymConfig::maxExpandTerms));
                                SymExpr term3 = simplifyCore(expand_core(B * V_j * SymExpr(Fraction(BigInt(i), BigInt(n_deg))) * P_prime, SymConfig::maxExpandTerms));
                                
                                SymExpr num = simplifyCore(expand_core(rhs - term1 + term2 - term3, SymConfig::maxExpandTerms));
                                SymExpr den = simplifyCore(expand_core(V_j * P_x, SymConfig::maxExpandTerms));
                                
                                SymExpr C = polyDiv(num, den, var).first;
                                
                                SymExpr V_jm1 = V_j ^ SymExpr(BigInt(j - 1));
                                alg_result = alg_result + simplifyCore(B / V_jm1) * (t_sym ^ SymExpr(BigInt(i)));
                                current_A = C;
                                current_D = polyDiv(current_D, V_j, var).first;
                                
                                for (auto& p : sqFree) if (p.second == j) p.second = j - 1;
                            }
                            
                            // 多项式部分约化 (Polynomial Reduction)
                            SymExpr W = simplifyCore(expand_core(current_A * P_x, SymConfig::maxExpandTerms));
                            int iter_poly = 0;
                            while (!W.isZero() && getDegree(W, var) >= getDegree(P_x, var) - 1 && iter_poly++ < 20) {
                                int degW = getDegree(W, var);
                                int degP = getDegree(P_x, var);
                                if (degP <= 0) break;
                                
                                int k = degW - degP + 1;
                                if (k < 0) break;
                                
                                SymExpr k_sym = SymExpr(BigInt(k));
                                SymExpr i_n = SymExpr(Fraction(BigInt(i), BigInt(n_deg)));
                                SymExpr P_prime = field.derive(P_x);
                                
                                SymExpr deriv_core = simplifyCore(expand_core(k_sym * P_x + SymExpr::makeVar(var) * i_n * P_prime, SymConfig::maxExpandTerms));
                                auto coeffs_deriv = extractCoeffs(deriv_core, var);
                                if (coeffs_deriv.empty()) break;
                                SymExpr lead_deriv = coeffs_deriv.back();
                                if (lead_deriv.isZero()) break;
                                
                                auto coeffs_W = extractCoeffs(W, var);
                                SymExpr leadW = coeffs_W.back();
                                
                                SymExpr coeff_c = simplifyCore(leadW / lead_deriv);
                                SymExpr Y_term = coeff_c * (SymExpr::makeVar(var) ^ SymExpr(BigInt(k)));
                                
                                SymExpr Y_prime = field.derive(Y_term);
                                SymExpr sub_term = simplifyCore(expand_core(Y_prime * P_x + Y_term * i_n * P_prime, SymConfig::maxExpandTerms));
                                SymExpr next_W = simplifyCore(expand_core(W - sub_term, SymConfig::maxExpandTerms));
                                
                                // 严格防死锁：如果度数没有下降，说明遇到了代数死锁，立即中断
                                if (!next_W.isZero() && getDegree(next_W, var) >= degW) {
                                    break;
                                }
                                
                                alg_result = alg_result + Y_term * (t_sym ^ SymExpr(BigInt(i)));
                                W = next_W;
                            }
                            
                            // 剩余部分直接作为对数积分的被积函数，不再强行除以 P_x
                            if (!W.isZero()) {
                                SymExpr rem = simplifyCore(W / (P_x * current_D));
                                log_integrand = log_integrand + rem * (t_sym ^ SymExpr(BigInt(i)));
                            }
                        }
                        
                        if (radical_success) {
                            SymExpr final_res = backSubstitute(alg_result);
                            if (!log_integrand.isZero()) {
                                SymExpr log_x = backSubstitute(log_integrand);
                                if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Integrating remaining log part: " << log_x.toString() << std::endl;
                                try {
                                    final_res = final_res + integrate(log_x, var, depth + 1);
                                } catch (...) {
                                    radical_success = false;
                                }
                            }
                            if (radical_success) {
                                if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "<- Risch Radical Extension Success" << std::endl;
                                return final_res;
                            }
                        }
                    }
                    
                    // 4. 构造 Trager 结式 (Trager's Resultant) 提取对数留数
                    if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Trager's Algorithm for Logarithmic Part" << std::endl;
                    SymExpr z = SymExpr::makeVar("_z");
                    SymExpr dP_dx = field.derive(minPoly, topExt.name);
                    SymExpr dP_dt = diff(minPoly, topExt.name);
                    
                    // Trager 核心方程: A_red * dP/dt - z * D_red * dP/dx
                    SymExpr trager_poly = simplifyCore(expand_core(A_red * dP_dt - z * D_red * dP_dx, SymConfig::maxExpandTerms));
                    trager_poly = getFraction(trager_poly).first; // 提前清除分母，防止结式计算时系数膨胀
                    
                    if (SymConfig::debugIntegration) {
                        std::cout << std::string(depth * 2, ' ') << "   [Risch] Trager poly: " << trager_poly.toString() << std::endl;
                    }

                    minPoly_clear = getFraction(minPoly).first; // 清除 minPoly 的分母，极大加速结式计算
                    SymExpr R_z = polyResultant(minPoly_clear, trager_poly, topExt.name);
                    
                    if (SymConfig::debugIntegration) {
                        std::cout << std::string(depth * 2, ' ') << "   [Risch] Raw Resultant R_z: " << R_z.toString() << std::endl;
                    }

                    // 提取分子，消除伪除法引入的分母
                    R_z = getFraction(R_z).first;
                    
                    // 处理多重代数扩张：通过连续计算结式 (Norm) 逐个消去底层的代数扩张变量
                    for (auto it = field.tower.rbegin(); it != field.tower.rend(); ++it) {
                        if (it->name == topExt.name) continue;
                        if (it->type == RischExtType::ALG) {
                            if (containsVar(R_z.ptr, it->name)) {
                                SymExpr subMinPoly = getFraction(it->minPoly).first;
                                R_z = polyResultant(subMinPoly, R_z, it->name);
                                R_z = getFraction(R_z).first;
                                if (SymConfig::debugIntegration) {
                                    std::cout << std::string(depth * 2, ' ') << "   [Risch] Resultant after eliminating " << it->name << ": " << R_z.toString() << std::endl;
                                }
                            }
                        }
                    }

                    SymExpr gcd_z(BigInt(0));
                    if (!containsVar(R_z.ptr, var)) {
                        gcd_z = R_z;
                    } else {
                        for (int pt = 1; pt <= 5; ++pt) {
                            SymExpr eval_Rz = simplifyCore(subs(R_z, var, SymExpr(BigInt(pt))));
                            eval_Rz = getFraction(eval_Rz).first;
                            if (eval_Rz.isZero()) continue;
                            if (gcd_z.isZero()) gcd_z = eval_Rz;
                            else gcd_z = polyGCD(gcd_z, eval_Rz, "_z");
                            if (gcd_z.isOne()) break;
                        }
                    }
                    
                    if (SymConfig::debugIntegration) {
                        std::cout << std::string(depth * 2, ' ') << "   [Risch] gcd_z: " << gcd_z.toString() << std::endl;
                    }

                    std::vector<SymExpr> roots;
                    if (!gcd_z.isZero() && getDegree(gcd_z, "_z") > 0) {
                        roots = solveEq(gcd_z, "_z");
                    }
                    
                    if (SymConfig::debugIntegration) {
                        std::cout << std::string(depth * 2, ' ') << "   [Risch] Roots of gcd_z: " << roots.size() << std::endl;
                        for (const auto& r : roots) {
                            std::cout << std::string(depth * 2, ' ') << "      root: " << r.toString() << std::endl;
                        }
                    }

                    if (!roots.empty()) {
                        bool allConstant = true;
                        for (const auto& root : roots) {
                            if (containsVar(root.ptr, var)) {
                                allConstant = false;
                                break;
                            }
                        }
                        if (allConstant) {
                            SymExpr result(BigInt(0));
                            for (const auto& root : roots) {
                                checkInterrupt();
                                SymExpr v_i = simplifyCore(expand_core(subs(trager_poly, "_z", root), SymConfig::maxExpandTerms));
                                    
                                // 降次短路与首一化 (注意：如果首项包含 x，除以它会改变对数求导的结果，因此仅当 lead 为常数时才首一化)
                                auto coeffs_vi = extractCoeffs(v_i, topExt.name);
                                if (!coeffs_vi.empty()) {
                                    SymExpr lead = coeffs_vi.back();
                                    if (!lead.isZero() && !lead.isOne() && !containsVar(lead.ptr, var)) {
                                        if (lead.ptr->getType() == SymType::NUM) {
                                            v_i = simplifyCore(expand_core(v_i / lead, SymConfig::maxExpandTerms));
                                        } else if (lead.ptr->getType() == SymType::MUL) {
                                            auto mul = std::static_pointer_cast<SymMul>(lead.ptr);
                                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                                v_i = simplifyCore(expand_core(v_i / SymExpr(mul->args[0]), SymConfig::maxExpandTerms));
                                            }
                                        }
                                    }
                                }
                                
                                SymExpr log_vi(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{v_i.ptr}));
                                result = result + root * log_vi;
                            }
                            SymExpr finalRes = simplifyCore(backSubstitute(result));
                            if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "<- Risch Trager (ALG) Success" << std::endl;
                            return finalRes;
                        } else {
                            if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Trager failed: roots are not constant w.r.t " << var << std::endl;
                        }
                    } else {
                        if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Trager failed: no roots found for gcd_z" << std::endl;
                    }
                } else {
                    if (SymConfig::debugIntegration) std::cout << std::string(depth * 2, ' ') << "   [Risch] Rationalize Denominator failed: res_D = " << res_D.toString() << std::endl;
                }
            }
        }

        // 调试输出：展示构建的微分域
        std::string debugInfo = "Risch Field over Q(" + var + "):\n";
        bool isElliptic = false;
        bool isHyperElliptic = false;
        
        for (const auto& ext : field.tower) {
            debugInfo += "  " + ext.name + " = ";
            if (ext.type == RischExtType::LOG) {
                debugInfo += "log(" + ext.arg.toString() + ")";
            } else if (ext.type == RischExtType::EXP) {
                debugInfo += "exp(" + ext.arg.toString() + ")";
            } else if (ext.type == RischExtType::ALG) {
                debugInfo += "RootOf(" + ext.minPoly.toString() + " = 0)";
                int degX = getDegree(ext.minPoly, var);
                int degT = getDegree(ext.minPoly, ext.name);
                if (degT == 2 && degX >= 3) {
                    // 提取 P(x) 并检查是否无平方 (Square-free check)
                    SymExpr P_neg = subs(ext.minPoly, ext.name, SymExpr(BigInt(0)));
                    auto sqFree = polySquareFree(P_neg, var);
                    bool hasMultipleRoots = false;
                    for (const auto& factor : sqFree) {
                        if (factor.second > 1 && getDegree(factor.first, var) > 0) {
                            hasMultipleRoots = true;
                            break;
                        }
                    }
                    if (!hasMultipleRoots) {
                        if (degX == 3 || degX == 4) isElliptic = true;
                        else if (degX >= 5) isHyperElliptic = true;
                    }
                }
            }
            debugInfo += ",  " + ext.name + "' = " + ext.deriv.toString() + "\n";
        }
        debugInfo += "Rewritten integrand: " + rewritten.toString();
        
        if (isElliptic) {
            throw std::runtime_error("Calculus Error: Integral is non-elementary (Elliptic curve of genus g=1 detected).\n" + debugInfo);
        } else if (isHyperElliptic) {
            throw std::runtime_error("Calculus Error: Integral is non-elementary (Hyperelliptic curve of genus g>1 detected).\n" + debugInfo);
        }
        
        throw std::runtime_error("Calculus Error: Integral is non-elementary or requires advanced Risch steps.\n" + debugInfo);
    }

    // =================================================================
    // 🚀 Risch 算法入口 (Risch Algorithm Entry Point)
    // =================================================================
    SymExpr rischIntegrate(const SymExpr& expr, const std::string& var, int depth) {
        if (depth > SymConfig::maxDepth) {
            throw std::runtime_error("Integration depth limit exceeded in Risch algorithm.");
        }

        if (getAstNodeCount(expr) > 300) {
            throw std::runtime_error("Integration AST size limit exceeded in Risch algorithm.");
        }

        // Step -1 - 三角函数转复指数 (Trig to Exp)
        SymExpr expExpr = trigToExp(expr);

        SymExpr res = rischIntegrateCore(expExpr, var, depth);

        // Step 6 - 复指数转回三角函数 (Exp to Trig)
        SymExpr trigRes = expToTrig(res);
        return simplify(trigRes);
    }

    // =================================================================
    // 🚀 符号积分 (Symbolic Integration) - 基础多项式与初等函数
    // =================================================================
    SymExpr integrate(const SymExpr& expr, const std::string& var, int start_depth) {
        if (!expr.ptr) return expr;

        SymExpr x = SymExpr::makeVar(var);

        // 从规则库获取静态积分规则
        const auto& rules = getStaticIntegRules();

        std::unordered_map<std::string, std::optional<SymExpr>> integCache;
        std::unordered_set<std::string> computing;
        std::function<std::optional<SymExpr>(const SymExpr&, int)> doInteg;

        int baseVarDepth = getVarDepth(expr, var);
        int baseTransWeight = getTranscendentalWeight(expr, var);

        auto doIntegImpl = [&](const SymExpr& e, int current_depth) -> std::optional<SymExpr> {
            checkInterrupt();
            if (SymConfig::debugIntegration) {
                std::cout << std::string(current_depth * 2, ' ') << "-> doIntegImpl depth=" << current_depth << " expr=" << e.toString() << std::endl;
            }

            if (current_depth > SymConfig::maxDepth) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Max depth exceeded" << std::endl;
                return std::nullopt;
            }

            // 积分变量下沉检测 (Variable Submergence Check)
            int currentVarDepth = getVarDepth(e, var);
            if (currentVarDepth > baseVarDepth + 5) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Variable submergence limit" << std::endl;
                return std::nullopt;
            }

            // 超越函数嵌套惩罚 (Transcendental Extension Penalty)
            int currentTransWeight = getTranscendentalWeight(e, var);
            if (currentTransWeight > baseTransWeight + 20) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Transcendental weight limit" << std::endl;
                return std::nullopt;
            }

            // 绝对体积限制 (Absolute AST Size Limit)
            if (getAstNodeCount(e) > 300) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- AST size limit" << std::endl;
                return std::nullopt;
            }

            if (!containsVar(e.ptr, var)) {
                return e * x; // ∫ c dx = c * x
            }

            if (e.ptr->getType() == SymType::ADD) {
                auto add = std::static_pointer_cast<SymAdd>(e.ptr);
                SymExpr res(BigInt(0));
                for (auto& arg : add->args) {
                    auto partRes = doInteg(SymExpr(arg), current_depth);
                    if (!partRes) return std::nullopt;
                    res = res + *partRes;
                }
                return res;
            }

            // --- 提取系数和变量部分 ---
            SymExpr coeff(BigInt(1));
            SymExpr varPart(BigInt(1));

            if (e.ptr->getType() == SymType::MUL) {
                auto mul = std::static_pointer_cast<SymMul>(e.ptr);
                for (auto& arg : mul->args) {
                    if (!containsVar(arg, var)) coeff = coeff * SymExpr(arg);
                    else varPart = varPart * SymExpr(arg);
                }
            } else {
                varPart = e;
            }

            // --- 0. 查表匹配 (Pattern Matching) ---
            SymExpr integratedPart = varPart;
            bool matched = false;
            for (const auto& rule : rules) {
                std::map<std::string, SymExpr> captures;
                if (matchAST(varPart.ptr, rule.first.ptr, captures)) {
                    // 积分规则中的 _x 必须精确匹配当前的积分变量 var
                    auto it_x = captures.find("_x");
                    if (it_x == captures.end() || it_x->second.ptr->getType() != SymType::VAR || 
                        std::static_pointer_cast<SymVar>(it_x->second.ptr)->name != var) {
                        continue;
                    }
                    
                    // 积分规则中的 _n, _a 必须是相对于积分变量的常数
                    bool validMatch = true;
                    for (const auto& cap : captures) {
                        if ((cap.first == "_n" || cap.first == "_a") && containsVar(cap.second.ptr, var)) {
                            validMatch = false;
                            break;
                        }
                    }
                    if (validMatch) {
                        integratedPart = substituteCaptures(rule.second, captures);
                        matched = true;
                        break;
                    }
                }
            }

            if (matched) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Matched Table" << std::endl;
                return coeff * integratedPart;
            }

            // --- 0.5 神级指数匹配公式 (Exponential RDE Heuristic) ---
            if (varPart.ptr->getType() == SymType::MUL) {
                auto mul = std::static_pointer_cast<SymMul>(varPart.ptr);
                SymExpr exp_factor;
                SymExpr rest_factor(BigInt(1));
                SymExpr w_arg;
                SymExpr c_val;
                bool found_exp = false;
                for (auto& arg : mul->args) {
                    if (!found_exp && arg->getType() == SymType::FUNC) {
                        auto func = std::static_pointer_cast<SymFunc>(arg);
                        if (func->name == "exp" && func->args.size() == 1) {
                            SymExpr w = SymExpr(func->args[0]);
                            SymExpr dw = simplifyCore(diff(w, var));
                            if (!dw.isZero()) {
                                exp_factor = SymExpr(arg);
                                w_arg = w;
                                c_val = dw;
                                found_exp = true;
                                continue;
                            }
                        }
                    }
                    rest_factor = rest_factor * SymExpr(arg);
                }
                if (found_exp) {
                    auto [num, den] = getFraction(rest_factor);
                    if (isPolynomialIn(num, var) && isPolynomialIn(den, var)) {
                        SymExpr den_exp = simplifyCore(expand_core(den, SymConfig::maxExpandTerms));
                        SymExpr d_den = diff(den_exp, var);
                        SymExpr gcd_den = polyGCD(den_exp, d_den, var);
                        SymExpr V = polyDiv(den_exp, gcd_den, var).first;
                        SymExpr V2 = simplifyCore(expand_core(V * V, SymConfig::maxExpandTerms));
                        SymExpr num_V2 = simplifyCore(expand_core(num * V2, SymConfig::maxExpandTerms));
                        
                        auto [RHS, R_rem] = polyDiv(num_V2, den_exp, var);
                        
                        if (R_rem.isZero()) {
                            SymExpr dV = diff(V, var);
                            SymExpr U(BigInt(0));
                            SymExpr current_RHS = RHS;
                            bool success = false;
                            
                            auto cV = extractCoeffs(V, var);
                            auto cC = extractCoeffs(c_val, var);
                            
                            if (!cV.empty() && !cV.back().isZero() && !cC.empty() && !cC.back().isZero()) {
                                SymExpr leadV = cV.back();
                                SymExpr leadC = cC.back();
                                int degV = static_cast<int>(cV.size()) - 1;
                                int degC = static_cast<int>(cC.size()) - 1;
                                int degRHS_init = getDegree(RHS, var);
                                int degU = degRHS_init - degV - degC;
                                
                                if (degU >= 0 && degU <= 20) {
                                    for (int step = 0; step <= degU + 1; ++step) {
                                        if (current_RHS.isZero()) {
                                            success = true;
                                            break;
                                        }
                                        auto cRHS = extractCoeffs(current_RHS, var);
                                        while (!cRHS.empty() && cRHS.back().isZero()) cRHS.pop_back();
                                        if (cRHS.empty()) {
                                            success = true;
                                            break;
                                        }
                                        
                                        int curDegRHS = static_cast<int>(cRHS.size()) - 1;
                                        int d = curDegRHS - degV - degC;
                                        if (d < 0) break;
                                        
                                        SymExpr leadRHS = cRHS.back();
                                        SymExpr c_d = simplifyCore(leadRHS / (leadC * leadV));
                                        SymExpr U_term = simplifyCore(c_d * (SymExpr::makeVar(var) ^ SymExpr(BigInt(d))));
                                        
                                        U = U + U_term;
                                        
                                        SymExpr dU_term = diff(U_term, var);
                                        SymExpr LHS_term = simplifyCore(expand_core(dU_term * V - U_term * dV + c_val * U_term * V, SymConfig::maxExpandTerms));
                                        current_RHS = simplifyCore(expand_core(current_RHS - LHS_term, SymConfig::maxExpandTerms));
                                    }
                                }
                            }
                            
                            if (success) {
                                SymExpr f = simplifyCore(U / V);
                                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Exponential RDE Heuristic Success" << std::endl;
                                return coeff * exp_factor * f;
                            }
                        }
                    }
                    
                    // 兜底的 f(x) + f'(x) 探测 (针对非有理分式的简单 ADD 节点)
                    if (c_val.isOne()) {
                        SymExpr expanded_rest = simplifyCore(expand_core(rest_factor, SymConfig::maxExpandTerms));
                        if (expanded_rest.ptr->getType() == SymType::ADD) {
                            auto add = std::static_pointer_cast<SymAdd>(expanded_rest.ptr);
                            for (size_t i = 0; i < add->args.size(); ++i) {
                                SymExpr f = SymExpr(add->args[i]);
                                SymExpr df = simplifyCore(diff(f, var));
                                SymExpr expected_rest = simplifyCore(expand_core(expanded_rest - f - df, SymConfig::maxExpandTerms));
                                if (expected_rest.isZero()) {
                                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Exponential f+f' Heuristic Success" << std::endl;
                                    return coeff * exp_factor * f;
                                }
                            }
                        }
                    }
                }
            }

            // --- 积分路由特征提取 (Feature Extraction for Routing) ---
            bool has_trig = false, has_exp = false, has_log = false, has_radical = false;
            std::unordered_set<const SymNode*> feat_visited;
            std::function<void(const std::shared_ptr<SymNode>&)> extract_features = [&](const std::shared_ptr<SymNode>& node) {
                if (!node || !feat_visited.insert(node.get()).second) return;
                if (node->getType() == SymType::FUNC) {
                    auto fn = std::static_pointer_cast<SymFunc>(node)->name;
                    if (fn == "sin" || fn == "cos" || fn == "tan" || fn == "cot" || fn == "sec" || fn == "csc" ||
                        fn == "sinh" || fn == "cosh" || fn == "tanh" || fn == "coth" || fn == "sech" || fn == "csch") has_trig = true;
                    else if (fn == "exp") has_exp = true;
                    else if (fn == "log") has_log = true;
                    else if (fn == "sqrt" || fn == "cbrt" || fn == "root" || fn == "RootOf" || fn == "RootSum") has_radical = true;
                } else if (node->getType() == SymType::POW) {
                    auto powNode = std::static_pointer_cast<SymPow>(node);
                    if (powNode->exp->getType() == SymType::NUM) {
                        auto numVal = std::static_pointer_cast<SymNum>(powNode->exp)->value;
                        if (std::holds_alternative<Fraction>(numVal) && std::get<Fraction>(numVal).getDen() > BigInt(1)) has_radical = true;
                    }
                }
                if (node->getType() == SymType::ADD) for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) extract_features(arg);
                else if (node->getType() == SymType::MUL) for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) extract_features(arg);
                else if (node->getType() == SymType::POW) { extract_features(std::static_pointer_cast<SymPow>(node)->base); extract_features(std::static_pointer_cast<SymPow>(node)->exp); }
                else if (node->getType() == SymType::FUNC) for (auto& arg : std::static_pointer_cast<SymFunc>(node)->args) extract_features(arg);
            };
            extract_features(varPart.ptr);

            struct IntegStrategy {
                std::string name;
                int score;
                std::function<std::optional<SymExpr>()> execute;
            };
            std::vector<IntegStrategy> strats;

            // --- 1. 线性换元法 (Linear Substitution) ---
            strats.push_back({"Linear Substitution", 1000, [&]() -> std::optional<SymExpr> {
                auto findLinearArg = [&](const std::shared_ptr<SymNode>& node, SymExpr& out_u, SymExpr& out_a) -> bool {
                if (!node) return false;
                if (node->getType() == SymType::FUNC) {
                    auto func = std::static_pointer_cast<SymFunc>(node);
                    for (auto& arg : func->args) {
                        SymExpr u(arg);
                        SymExpr du = simplifyCore(diff(u, var));
                        if (!du.isZero() && !containsVar(du.ptr, var)) {
                            if (u.ptr->getType() != SymType::VAR) {
                                out_u = u;
                                out_a = du;
                                return true;
                            }
                        }
                    }
                } else if (node->getType() == SymType::POW) {
                    auto powNode = std::static_pointer_cast<SymPow>(node);
                    SymExpr u_base(powNode->base);
                    SymExpr du_base = simplifyCore(diff(u_base, var));
                    if (!du_base.isZero() && !containsVar(du_base.ptr, var)) {
                        if (u_base.ptr->getType() != SymType::VAR) {
                            out_u = u_base;
                            out_a = du_base;
                            return true;
                        }
                    }
                    SymExpr u_exp(powNode->exp);
                    SymExpr du_exp = simplifyCore(diff(u_exp, var));
                    if (!du_exp.isZero() && !containsVar(du_exp.ptr, var)) {
                        if (u_exp.ptr->getType() != SymType::VAR) {
                            out_u = u_exp;
                            out_a = du_exp;
                            return true;
                        }
                    }
                }
                return false;
            };

            SymExpr sub_u, sub_a;
            if (findLinearArg(varPart.ptr, sub_u, sub_a)) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Linear Substitution: u = " << sub_u.toString() << std::endl;
                std::string u_var = "_u";
                SymExpr u_sym = SymExpr::makeVar(u_var);
                std::function<SymExpr(const SymExpr&)> replaceNode = [&](const SymExpr& expr_node) -> SymExpr {
                    if (!expr_node.ptr) return expr_node;
                    if (expr_node == sub_u) return u_sym;
                    switch (expr_node.ptr->getType()) {
                        case SymType::ADD: {
                            SymExpr res(BigInt(0));
                            for (auto& arg : std::static_pointer_cast<SymAdd>(expr_node.ptr)->args) res = res + replaceNode(SymExpr(arg));
                            return res;
                        }
                        case SymType::MUL: {
                            SymExpr res(BigInt(1));
                            for (auto& arg : std::static_pointer_cast<SymMul>(expr_node.ptr)->args) res = res * replaceNode(SymExpr(arg));
                            return res;
                        }
                        case SymType::POW: {
                            auto p = std::static_pointer_cast<SymPow>(expr_node.ptr);
                            return replaceNode(SymExpr(p->base)) ^ replaceNode(SymExpr(p->exp));
                        }
                        case SymType::FUNC: {
                            auto f = std::static_pointer_cast<SymFunc>(expr_node.ptr);
                            std::vector<std::shared_ptr<SymNode>> nArgs;
                            for (auto& arg : f->args) nArgs.push_back(replaceNode(SymExpr(arg)).ptr);
                            return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
                        }
                        default: return expr_node;
                    }
                };
                
                SymExpr f_u = replaceNode(varPart);
                if (!containsVar(f_u.ptr, var)) {
                    SymExpr f_var = subs(f_u, u_var, SymExpr::makeVar(var));
                    if (auto int_var = doInteg(simplify(expand_core(f_var / sub_a, SymConfig::maxExpandTerms)), current_depth + 1)) {
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Linear Substitution Success" << std::endl;
                        SymExpr res = subs(*int_var, var, sub_u);
                        return coeff * res;
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.5 广义换元法 (凑微分法) ---
            strats.push_back({"Generalized Substitution", 900, [&]() -> std::optional<SymExpr> {
            if (varPart.ptr->getType() == SymType::MUL) {
                auto mul = std::static_pointer_cast<SymMul>(varPart.ptr);
                std::vector<SymExpr> candidate_us;
                for (auto& arg : mul->args) {
                    if (arg->getType() == SymType::POW) {
                        candidate_us.push_back(SymExpr(std::static_pointer_cast<SymPow>(arg)->base));
                    } else if (arg->getType() == SymType::FUNC) {
                        auto func = std::static_pointer_cast<SymFunc>(arg);
                        if (func->args.size() == 1) {
                            candidate_us.push_back(SymExpr(func->args[0]));
                        }
                    }
                }
                for (auto& arg : mul->args) {
                    candidate_us.push_back(SymExpr(arg));
                }
                
                for (const auto& u : candidate_us) {
                    if (u.ptr->getType() == SymType::VAR || u.ptr->getType() == SymType::NUM) continue;
                    SymExpr du = simplifyCore(diff(u, var));
                    if (du.isZero()) continue;
                    
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Generalized Substitution: u = " << u.toString() << std::endl;
                    SymExpr rem = simplify(varPart / du);
                    
                    std::string u_var = "_u";
                    SymExpr u_sym = SymExpr::makeVar(u_var);
                    std::function<SymExpr(const SymExpr&)> replaceU = [&](const SymExpr& expr_node) -> SymExpr {
                        if (!expr_node.ptr) return expr_node;
                        if (expr_node == u) return u_sym;
                        switch (expr_node.ptr->getType()) {
                            case SymType::ADD: {
                                SymExpr res(BigInt(0));
                                for (auto& arg : std::static_pointer_cast<SymAdd>(expr_node.ptr)->args) res = res + replaceU(SymExpr(arg));
                                return res;
                            }
                            case SymType::MUL: {
                                SymExpr res(BigInt(1));
                                for (auto& arg : std::static_pointer_cast<SymMul>(expr_node.ptr)->args) res = res * replaceU(SymExpr(arg));
                                return res;
                            }
                            case SymType::POW: {
                                auto p = std::static_pointer_cast<SymPow>(expr_node.ptr);
                                return replaceU(SymExpr(p->base)) ^ replaceU(SymExpr(p->exp));
                            }
                            case SymType::FUNC: {
                                auto f = std::static_pointer_cast<SymFunc>(expr_node.ptr);
                                std::vector<std::shared_ptr<SymNode>> nArgs;
                                for (auto& arg : f->args) nArgs.push_back(replaceU(SymExpr(arg)).ptr);
                                return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
                            }
                            default: return expr_node;
                        }
                    };
                    
                    SymExpr rem_u = replaceU(rem);
                    
                    if (containsVar(rem_u.ptr, var)) {
                        auto coeffs = extractCoeffs(u, var);
                        if (coeffs.size() > 1) {
                            bool isBinomial = true;
                            int n = static_cast<int>(coeffs.size()) - 1;
                            for (int i = 1; i < n; ++i) {
                                if (!coeffs[i].isZero()) { isBinomial = false; break; }
                            }
                            if (isBinomial && !coeffs[n].isZero()) {
                                SymExpr A = coeffs[n];
                                SymExpr B = coeffs[0];
                                SymExpr x_n = (u_sym - B) / A;
                                
                                std::function<SymExpr(const SymExpr&)> replaceXn = [&](const SymExpr& node) -> SymExpr {
                                    if (!node.ptr) return node;
                                    if (node.ptr->getType() == SymType::POW) {
                                        auto p = std::static_pointer_cast<SymPow>(node.ptr);
                                        if (p->base->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(p->base)->name == var) {
                                            if (p->exp->getType() == SymType::NUM) {
                                                auto [isInt, exp_val] = extractExactInt(std::static_pointer_cast<SymNum>(p->exp)->value);
                                                if (isInt && exp_val % n == 0) {
                                                    return x_n ^ SymExpr(BigInt(exp_val / n));
                                                }
                                            }
                                        }
                                    }
                                    if (n == 1 && node.ptr->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(node.ptr)->name == var) {
                                        return x_n;
                                    }
                                    switch (node.ptr->getType()) {
                                        case SymType::ADD: {
                                            SymExpr res(BigInt(0));
                                            for (auto& arg : std::static_pointer_cast<SymAdd>(node.ptr)->args) res = res + replaceXn(SymExpr(arg));
                                            return res;
                                        }
                                        case SymType::MUL: {
                                            SymExpr res(BigInt(1));
                                            for (auto& arg : std::static_pointer_cast<SymMul>(node.ptr)->args) res = res * replaceXn(SymExpr(arg));
                                            return res;
                                        }
                                        case SymType::POW: {
                                            auto p = std::static_pointer_cast<SymPow>(node.ptr);
                                            return replaceXn(SymExpr(p->base)) ^ replaceXn(SymExpr(p->exp));
                                        }
                                        case SymType::FUNC: {
                                            auto f = std::static_pointer_cast<SymFunc>(node.ptr);
                                            std::vector<std::shared_ptr<SymNode>> nArgs;
                                            for (auto& arg : f->args) nArgs.push_back(replaceXn(SymExpr(arg)).ptr);
                                            return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
                                        }
                                        default: return node;
                                    }
                                };
                                rem_u = simplify(expand_core(replaceXn(rem_u), SymConfig::maxExpandTerms));
                            }
                        }
                    }

                    if (!containsVar(rem_u.ptr, var)) {
                        SymExpr f_var = subs(rem_u, u_var, SymExpr::makeVar(var));
                        if (auto int_var = doInteg(simplify(expand_core(f_var, SymConfig::maxExpandTerms)), current_depth + 1)) {
                            if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Generalized Substitution Success" << std::endl;
                            SymExpr res = subs(*int_var, var, u);
                            return coeff * res;
                        }
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.6 根式整体换元 (Radical Substitution) ---
            strats.push_back({"Radical Substitution", has_radical ? 800 : 0, [&]() -> std::optional<SymExpr> {
            SymExpr radicalBase;
            int radicalN = 0;
            bool foundRadical = false;
            SymExpr varPart_u = varPart;
            std::string u_var = "_u_rad";
            SymExpr u_sym = SymExpr::makeVar(u_var);

            if (varPart.ptr->getType() == SymType::MUL) {
                auto mul = std::static_pointer_cast<SymMul>(varPart.ptr);
                std::map<int, std::vector<SymExpr>> posRadicals;
                std::map<int, std::vector<SymExpr>> negRadicals;
                for (auto& arg : mul->args) {
                    if (arg->getType() == SymType::POW) {
                        auto powNode = std::static_pointer_cast<SymPow>(arg);
                        if (powNode->exp->getType() == SymType::NUM) {
                            auto numVal = std::static_pointer_cast<SymNum>(powNode->exp)->value;
                            if (std::holds_alternative<Fraction>(numVal)) {
                                Fraction frac = std::get<Fraction>(numVal);
                                if (frac.getDen() > BigInt(1) && containsVar(powNode->base, var)) {
                                    int n = static_cast<int>(frac.getDen().toDouble());
                                    if (frac.getNum() == BigInt(1)) posRadicals[n].push_back(SymExpr(powNode->base));
                                    else if (frac.getNum() == BigInt(-1)) negRadicals[n].push_back(SymExpr(powNode->base));
                                }
                            } else if (std::holds_alternative<BigInt>(numVal) && std::get<BigInt>(numVal) == BigInt(-1)) {
                                if (powNode->base->getType() == SymType::FUNC) {
                                    auto func = std::static_pointer_cast<SymFunc>(powNode->base);
                                    if (func->name == "sqrt" && func->args.size() == 1 && containsVar(func->args[0], var)) {
                                        negRadicals[2].push_back(SymExpr(func->args[0]));
                                    } else if (func->name == "cbrt" && func->args.size() == 1 && containsVar(func->args[0], var)) {
                                        negRadicals[3].push_back(SymExpr(func->args[0]));
                                    }
                                }
                            }
                        }
                    } else if (arg->getType() == SymType::FUNC) {
                        auto func = std::static_pointer_cast<SymFunc>(arg);
                        if (func->name == "sqrt" && func->args.size() == 1 && containsVar(func->args[0], var)) {
                            posRadicals[2].push_back(SymExpr(func->args[0]));
                        } else if (func->name == "cbrt" && func->args.size() == 1 && containsVar(func->args[0], var)) {
                            posRadicals[3].push_back(SymExpr(func->args[0]));
                        }
                    }
                }
                for (auto& [n, posList] : posRadicals) {
                    if (negRadicals.count(n) && !posList.empty() && !negRadicals[n].empty()) {
                        SymExpr A = posList[0];
                        SymExpr B = negRadicals[n][0];
                        radicalBase = simplifyCore(A / B);
                        radicalN = n;
                        foundRadical = true;
                        
                        SymExpr rest(BigInt(1));
                        bool removedA = false, removedB = false;
                        for (auto& arg : mul->args) {
                            bool skip = false;
                            if (!removedA) {
                                if (arg->getType() == SymType::POW) {
                                    auto powNode = std::static_pointer_cast<SymPow>(arg);
                                    if (SymExpr(powNode->base) == A && SymExpr(powNode->exp) == SymExpr(Fraction(1, n))) {
                                        removedA = true;
                                        skip = true;
                                    }
                                } else if (arg->getType() == SymType::FUNC) {
                                    auto func = std::static_pointer_cast<SymFunc>(arg);
                                    if (func->name == "sqrt" && n == 2 && func->args.size() == 1 && SymExpr(func->args[0]) == A) {
                                        removedA = true;
                                        skip = true;
                                    } else if (func->name == "cbrt" && n == 3 && func->args.size() == 1 && SymExpr(func->args[0]) == A) {
                                        removedA = true;
                                        skip = true;
                                    }
                                }
                            }
                            if (!skip && !removedB) {
                                if (arg->getType() == SymType::POW) {
                                    auto powNode = std::static_pointer_cast<SymPow>(arg);
                                    if (SymExpr(powNode->base) == B && SymExpr(powNode->exp) == SymExpr(Fraction(-1, n))) {
                                        removedB = true;
                                        skip = true;
                                    } else if (powNode->exp->getType() == SymType::NUM && std::holds_alternative<BigInt>(std::static_pointer_cast<SymNum>(powNode->exp)->value) && std::get<BigInt>(std::static_pointer_cast<SymNum>(powNode->exp)->value) == BigInt(-1)) {
                                        if (powNode->base->getType() == SymType::FUNC) {
                                            auto func = std::static_pointer_cast<SymFunc>(powNode->base);
                                            if (func->name == "sqrt" && n == 2 && func->args.size() == 1 && SymExpr(func->args[0]) == B) {
                                                removedB = true;
                                                skip = true;
                                            } else if (func->name == "cbrt" && n == 3 && func->args.size() == 1 && SymExpr(func->args[0]) == B) {
                                                removedB = true;
                                                skip = true;
                                            }
                                        }
                                    }
                                }
                            }
                            if (!skip) rest = rest * SymExpr(arg);
                        }
                        varPart_u = rest * u_sym;
                        break;
                    }
                }
            }

            std::function<void(const std::shared_ptr<SymNode>&)> findRadical = [&](const std::shared_ptr<SymNode>& node) {
                if (!node || foundRadical) return;
                if (node->getType() == SymType::POW) {
                    auto powNode = std::static_pointer_cast<SymPow>(node);
                    if (powNode->exp->getType() == SymType::NUM) {
                        auto numVal = std::static_pointer_cast<SymNum>(powNode->exp)->value;
                        if (std::holds_alternative<Fraction>(numVal)) {
                            Fraction frac = std::get<Fraction>(numVal);
                            if (frac.getDen() > BigInt(1) && containsVar(powNode->base, var)) {
                                radicalBase = SymExpr(powNode->base);
                                radicalN = static_cast<int>(frac.getDen().toDouble());
                                foundRadical = true;
                                
                                std::function<SymExpr(const SymExpr&)> replaceRad = [&](const SymExpr& e) -> SymExpr {
                                    if (!e.ptr) return e;
                                    if (e.ptr->getType() == SymType::POW) {
                                        auto pNode = std::static_pointer_cast<SymPow>(e.ptr);
                                        if (SymExpr(pNode->base) == radicalBase && pNode->exp->getType() == SymType::NUM) {
                                            auto nVal = std::static_pointer_cast<SymNum>(pNode->exp)->value;
                                            if (std::holds_alternative<Fraction>(nVal)) {
                                                Fraction f = std::get<Fraction>(nVal);
                                                if (f.getDen() == BigInt(radicalN)) {
                                                    return u_sym ^ SymExpr(f.getNum());
                                                }
                                            }
                                        }
                                    }
                                    switch (e.ptr->getType()) {
                                        case SymType::ADD: {
                                            SymExpr res(BigInt(0));
                                            for (auto& a : std::static_pointer_cast<SymAdd>(e.ptr)->args) res = res + replaceRad(SymExpr(a));
                                            return res;
                                        }
                                        case SymType::MUL: {
                                            SymExpr res(BigInt(1));
                                            for (auto& a : std::static_pointer_cast<SymMul>(e.ptr)->args) res = res * replaceRad(SymExpr(a));
                                            return res;
                                        }
                                        case SymType::POW: {
                                            auto pNode = std::static_pointer_cast<SymPow>(e.ptr);
                                            return replaceRad(SymExpr(pNode->base)) ^ replaceRad(SymExpr(pNode->exp));
                                        }
                                        case SymType::FUNC: {
                                            auto fNode = std::static_pointer_cast<SymFunc>(e.ptr);
                                            std::vector<std::shared_ptr<SymNode>> nArgs;
                                            for (auto& a : fNode->args) nArgs.push_back(replaceRad(SymExpr(a)).ptr);
                                            return SymExpr(std::make_shared<SymFunc>(fNode->name, std::move(nArgs)));
                                        }
                                        default: return e;
                                    }
                                };
                                varPart_u = replaceRad(varPart);
                                return;
                            }
                        }
                    }
                } else if (node->getType() == SymType::FUNC) {
                    auto func = std::static_pointer_cast<SymFunc>(node);
                    if ((func->name == "sqrt" || func->name == "cbrt") && func->args.size() == 1 && containsVar(func->args[0], var)) {
                        radicalBase = SymExpr(func->args[0]);
                        radicalN = (func->name == "sqrt") ? 2 : 3;
                        foundRadical = true;
                        
                        std::function<SymExpr(const SymExpr&)> replaceRad = [&](const SymExpr& e) -> SymExpr {
                            if (!e.ptr) return e;
                            if (e.ptr->getType() == SymType::POW) {
                                auto pNode = std::static_pointer_cast<SymPow>(e.ptr);
                                if (SymExpr(pNode->base) == radicalBase && pNode->exp->getType() == SymType::NUM) {
                                    auto nVal = std::static_pointer_cast<SymNum>(pNode->exp)->value;
                                    if (std::holds_alternative<Fraction>(nVal)) {
                                        Fraction f = std::get<Fraction>(nVal);
                                        if (f.getDen() == BigInt(radicalN)) {
                                            return u_sym ^ SymExpr(f.getNum());
                                        }
                                    }
                                }
                            } else if (e.ptr->getType() == SymType::FUNC) {
                                auto fNode = std::static_pointer_cast<SymFunc>(e.ptr);
                                if (fNode->name == "sqrt" && fNode->args.size() == 1 && SymExpr(fNode->args[0]) == radicalBase && radicalN == 2) {
                                    return u_sym;
                                } else if (fNode->name == "cbrt" && fNode->args.size() == 1 && SymExpr(fNode->args[0]) == radicalBase && radicalN == 3) {
                                    return u_sym;
                                }
                            }
                            switch (e.ptr->getType()) {
                                case SymType::ADD: {
                                    SymExpr res(BigInt(0));
                                    for (auto& a : std::static_pointer_cast<SymAdd>(e.ptr)->args) res = res + replaceRad(SymExpr(a));
                                    return res;
                                }
                                case SymType::MUL: {
                                    SymExpr res(BigInt(1));
                                    for (auto& a : std::static_pointer_cast<SymMul>(e.ptr)->args) res = res * replaceRad(SymExpr(a));
                                    return res;
                                }
                                case SymType::POW: {
                                    auto pNode = std::static_pointer_cast<SymPow>(e.ptr);
                                    return replaceRad(SymExpr(pNode->base)) ^ replaceRad(SymExpr(pNode->exp));
                                }
                                case SymType::FUNC: {
                                    auto fNode = std::static_pointer_cast<SymFunc>(e.ptr);
                                    std::vector<std::shared_ptr<SymNode>> nArgs;
                                    for (auto& a : fNode->args) nArgs.push_back(replaceRad(SymExpr(a)).ptr);
                                    return SymExpr(std::make_shared<SymFunc>(fNode->name, std::move(nArgs)));
                                }
                                default: return e;
                            }
                        };
                        varPart_u = replaceRad(varPart);
                        return;
                    }
                }
                if (node->getType() == SymType::ADD) {
                    for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) findRadical(arg);
                } else if (node->getType() == SymType::MUL) {
                    for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) findRadical(arg);
                } else if (node->getType() == SymType::POW) {
                    auto powNode = std::static_pointer_cast<SymPow>(node);
                    findRadical(powNode->base);
                    findRadical(powNode->exp);
                } else if (node->getType() == SymType::FUNC) {
                    for (auto& arg : std::static_pointer_cast<SymFunc>(node)->args) findRadical(arg);
                }
            };
            if (!foundRadical) findRadical(varPart.ptr);

            if (foundRadical && radicalN >= 2) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Radical Substitution: u = (" << radicalBase.toString() << ")^(1/" << radicalN << ")" << std::endl;
                auto invertFunction = [&](SymExpr f, SymExpr target, std::string v) -> std::optional<SymExpr> {
                    try {
                        auto [num, den] = getFraction(simplifyCore(f - target));
                        auto roots = solveEq(num, v);
                        if (roots.size() == 1) {
                            return roots[0];
                        }
                    } catch (...) {}

                    int iters = 0;
                    while (f.ptr->getType() != SymType::VAR && iters++ < 10) {
                        if (f.ptr->getType() == SymType::ADD) {
                            auto add = std::static_pointer_cast<SymAdd>(f.ptr);
                            SymExpr withV(BigInt(0)), withoutV(BigInt(0));
                            int countV = 0;
                            for (auto& arg : add->args) {
                                if (containsVar(arg, v)) { withV = SymExpr(arg); countV++; }
                                else withoutV = withoutV + SymExpr(arg);
                            }
                            if (countV != 1) return std::nullopt;
                            target = target - withoutV;
                            f = withV;
                        } else if (f.ptr->getType() == SymType::MUL) {
                            auto mul = std::static_pointer_cast<SymMul>(f.ptr);
                            SymExpr withV(BigInt(1)), withoutV(BigInt(1));
                            int countV = 0;
                            for (auto& arg : mul->args) {
                                if (containsVar(arg, v)) { withV = SymExpr(arg); countV++; }
                                else withoutV = withoutV * SymExpr(arg);
                            }
                            if (countV != 1) return std::nullopt;
                            target = target / withoutV;
                            f = withV;
                        } else if (f.ptr->getType() == SymType::POW) {
                            auto powNode = std::static_pointer_cast<SymPow>(f.ptr);
                            bool baseHasV = containsVar(powNode->base, v);
                            bool expHasV = containsVar(powNode->exp, v);
                            if (baseHasV && !expHasV) {
                                target = target ^ (SymExpr(BigInt(1)) / SymExpr(powNode->exp));
                                f = SymExpr(powNode->base);
                            } else if (!baseHasV && expHasV) {
                                SymExpr log_target(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                                SymExpr log_base(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{powNode->base}));
                                target = log_target / log_base;
                                f = SymExpr(powNode->exp);
                            } else {
                                return std::nullopt;
                            }
                        } else if (f.ptr->getType() == SymType::FUNC) {
                            auto func = std::static_pointer_cast<SymFunc>(f.ptr);
                            if (func->args.size() != 1) return std::nullopt;
                            SymExpr arg(func->args[0]);
                            if (func->name == "sin") target = SymExpr(std::make_shared<SymFunc>("asin", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "cos") target = SymExpr(std::make_shared<SymFunc>("acos", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "tan") target = SymExpr(std::make_shared<SymFunc>("atan", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "asin") target = SymExpr(std::make_shared<SymFunc>("sin", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "acos") target = SymExpr(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "atan") target = SymExpr(std::make_shared<SymFunc>("tan", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "exp") target = SymExpr(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "log") target = SymExpr(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "sinh") target = SymExpr(std::make_shared<SymFunc>("asinh", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "cosh") target = SymExpr(std::make_shared<SymFunc>("acosh", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else if (func->name == "tanh") target = SymExpr(std::make_shared<SymFunc>("atanh", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                            else return std::nullopt;
                            f = arg;
                        } else {
                            return std::nullopt;
                        }
                    }
                    if (f.ptr->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(f.ptr)->name == v) {
                        return target;
                    }
                    return std::nullopt;
                };

                SymExpr target = u_sym ^ SymExpr(BigInt(radicalN));
                
                if (auto x_sol = invertFunction(radicalBase, target, var)) {
                    SymExpr dx_du = simplify(diff(*x_sol, u_var));
                    if (!dx_du.isZero()) {
                        SymExpr new_integrand = simplify(subs(varPart_u, var, *x_sol) * dx_du);
                        SymExpr integrand_var = subs(new_integrand, u_var, SymExpr::makeVar(var));
                        if (auto int_var = doInteg(integrand_var, current_depth + 1)) {
                            if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Radical Substitution Success" << std::endl;
                            SymExpr back_sub = radicalBase ^ SymExpr(Fraction(BigInt(1), BigInt(radicalN)));
                            return coeff * simplify(subs(*int_var, var, back_sub));
                        }
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.7 同底数分数指数换元 (Common Fractional Power Substitution) ---
            strats.push_back({"Common Fractional Power Substitution", has_radical ? 750 : 0, [&]() -> std::optional<SymExpr> {
            {
                SymExpr commonBase;
                bool baseConflict = false;
                std::vector<BigInt> denominators;
                
                std::function<void(const std::shared_ptr<SymNode>&)> findFracPowers = [&](const std::shared_ptr<SymNode>& node) {
                    if (!node || baseConflict) return;
                    if (node->getType() == SymType::POW) {
                        auto powNode = std::static_pointer_cast<SymPow>(node);
                        if (powNode->exp->getType() == SymType::NUM) {
                            auto numVal = std::static_pointer_cast<SymNum>(powNode->exp)->value;
                            if (std::holds_alternative<Fraction>(numVal)) {
                                Fraction frac = std::get<Fraction>(numVal);
                                if (frac.getDen() > BigInt(1) && containsVar(powNode->base, var)) {
                                    if (!commonBase.ptr) {
                                        commonBase = SymExpr(powNode->base);
                                    } else if (commonBase != SymExpr(powNode->base)) {
                                        baseConflict = true;
                                        return;
                                    }
                                    denominators.push_back(frac.getDen());
                                }
                            }
                        }
                    } else if (node->getType() == SymType::FUNC) {
                        auto func = std::static_pointer_cast<SymFunc>(node);
                        if ((func->name == "sqrt" || func->name == "cbrt") && func->args.size() == 1 && containsVar(func->args[0], var)) {
                            if (!commonBase.ptr) {
                                commonBase = SymExpr(func->args[0]);
                            } else if (commonBase != SymExpr(func->args[0])) {
                                baseConflict = true;
                                return;
                            }
                            denominators.push_back(func->name == "sqrt" ? BigInt(2) : BigInt(3));
                        }
                    }
                    
                    if (node->getType() == SymType::ADD) {
                        for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) findFracPowers(arg);
                    } else if (node->getType() == SymType::MUL) {
                        for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) findFracPowers(arg);
                    } else if (node->getType() == SymType::POW) {
                        auto powNode = std::static_pointer_cast<SymPow>(node);
                        findFracPowers(powNode->base);
                        findFracPowers(powNode->exp);
                    } else if (node->getType() == SymType::FUNC) {
                        for (auto& arg : std::static_pointer_cast<SymFunc>(node)->args) findFracPowers(arg);
                    }
                };
                
                findFracPowers(varPart.ptr);
                
                if (commonBase.ptr && !baseConflict && denominators.size() > 1) {
                    BigInt lcm_den = denominators[0];
                    for (size_t i = 1; i < denominators.size(); ++i) {
                        lcm_den = BigInt::lcm(lcm_den, denominators[i]);
                    }
                    
                    if (lcm_den > BigInt(1) && lcm_den <= BigInt(20)) { // 限制 LCM 大小，防止指数爆炸
                        int L = static_cast<int>(lcm_den.toDouble());
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Common Fractional Power Substitution: u = (" << commonBase.toString() << ")^(1/" << L << ")" << std::endl;
                        
                        std::string u_var_frac = "_u_frac";
                        SymExpr u_sym_frac = SymExpr::makeVar(u_var_frac);
                        
                        std::function<SymExpr(const SymExpr&)> replaceFrac = [&](const SymExpr& e) -> SymExpr {
                            if (!e.ptr) return e;
                            if (e.ptr->getType() == SymType::POW) {
                                auto pNode = std::static_pointer_cast<SymPow>(e.ptr);
                                if (SymExpr(pNode->base) == commonBase && pNode->exp->getType() == SymType::NUM) {
                                    auto nVal = std::static_pointer_cast<SymNum>(pNode->exp)->value;
                                    if (std::holds_alternative<Fraction>(nVal)) {
                                        Fraction f = std::get<Fraction>(nVal);
                                        BigInt newNum = f.getNum() * (lcm_den / f.getDen());
                                        return u_sym_frac ^ SymExpr(newNum);
                                    } else if (std::holds_alternative<BigInt>(nVal)) {
                                        BigInt newNum = std::get<BigInt>(nVal) * lcm_den;
                                        return u_sym_frac ^ SymExpr(newNum);
                                    }
                                }
                            } else if (e.ptr->getType() == SymType::FUNC) {
                                auto fNode = std::static_pointer_cast<SymFunc>(e.ptr);
                                if (fNode->name == "sqrt" && fNode->args.size() == 1 && SymExpr(fNode->args[0]) == commonBase) {
                                    return u_sym_frac ^ SymExpr(lcm_den / BigInt(2));
                                } else if (fNode->name == "cbrt" && fNode->args.size() == 1 && SymExpr(fNode->args[0]) == commonBase) {
                                    return u_sym_frac ^ SymExpr(lcm_den / BigInt(3));
                                }
                            }
                            if (e == commonBase) {
                                return u_sym_frac ^ SymExpr(lcm_den);
                            }
                            switch (e.ptr->getType()) {
                                case SymType::ADD: {
                                    SymExpr res(BigInt(0));
                                    for (auto& a : std::static_pointer_cast<SymAdd>(e.ptr)->args) res = res + replaceFrac(SymExpr(a));
                                    return res;
                                }
                                case SymType::MUL: {
                                    SymExpr res(BigInt(1));
                                    for (auto& a : std::static_pointer_cast<SymMul>(e.ptr)->args) res = res * replaceFrac(SymExpr(a));
                                    return res;
                                }
                                case SymType::POW: {
                                    auto pNode = std::static_pointer_cast<SymPow>(e.ptr);
                                    return replaceFrac(SymExpr(pNode->base)) ^ replaceFrac(SymExpr(pNode->exp));
                                }
                                case SymType::FUNC: {
                                    auto fNode = std::static_pointer_cast<SymFunc>(e.ptr);
                                    std::vector<std::shared_ptr<SymNode>> nArgs;
                                    for (auto& a : fNode->args) nArgs.push_back(replaceFrac(SymExpr(a)).ptr);
                                    return SymExpr(std::make_shared<SymFunc>(fNode->name, std::move(nArgs)));
                                }
                                default: return e;
                            }
                        };
                        
                        SymExpr varPart_u_frac = replaceFrac(varPart);
                        
                        auto invertFunction = [&](SymExpr f, SymExpr target, std::string v) -> std::optional<SymExpr> {
                            try {
                                auto [num, den] = getFraction(simplifyCore(f - target));
                                auto roots = solveEq(num, v);
                                if (roots.size() == 1) {
                                    return roots[0];
                                }
                            } catch (...) {}
                            int iters = 0;
                            while (f.ptr->getType() != SymType::VAR && iters++ < 10) {
                                if (f.ptr->getType() == SymType::ADD) {
                                    auto add = std::static_pointer_cast<SymAdd>(f.ptr);
                                    SymExpr withV(BigInt(0)), withoutV(BigInt(0));
                                    int countV = 0;
                                    for (auto& arg : add->args) {
                                        if (containsVar(arg, v)) { withV = SymExpr(arg); countV++; }
                                        else withoutV = withoutV + SymExpr(arg);
                                    }
                                    if (countV != 1) return std::nullopt;
                                    target = target - withoutV;
                                    f = withV;
                                } else if (f.ptr->getType() == SymType::MUL) {
                                    auto mul = std::static_pointer_cast<SymMul>(f.ptr);
                                    SymExpr withV(BigInt(1)), withoutV(BigInt(1));
                                    int countV = 0;
                                    for (auto& arg : mul->args) {
                                        if (containsVar(arg, v)) { withV = SymExpr(arg); countV++; }
                                        else withoutV = withoutV * SymExpr(arg);
                                    }
                                    if (countV != 1) return std::nullopt;
                                    target = target / withoutV;
                                    f = withV;
                                } else if (f.ptr->getType() == SymType::POW) {
                                    auto powNode = std::static_pointer_cast<SymPow>(f.ptr);
                                    bool baseHasV = containsVar(powNode->base, v);
                                    bool expHasV = containsVar(powNode->exp, v);
                                    if (baseHasV && !expHasV) {
                                        target = target ^ (SymExpr(BigInt(1)) / SymExpr(powNode->exp));
                                        f = SymExpr(powNode->base);
                                    } else if (!baseHasV && expHasV) {
                                        SymExpr log_target(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{target.ptr}));
                                        SymExpr log_base(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{powNode->base}));
                                        target = log_target / log_base;
                                        f = SymExpr(powNode->exp);
                                    } else {
                                        return std::nullopt;
                                    }
                                } else {
                                    return std::nullopt;
                                }
                            }
                            if (f.ptr->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(f.ptr)->name == v) {
                                return target;
                            }
                            return std::nullopt;
                        };
                        
                        SymExpr target = u_sym_frac ^ SymExpr(lcm_den);
                        if (auto x_sol = invertFunction(commonBase, target, var)) {
                            SymExpr dx_du = simplify(diff(*x_sol, u_var_frac));
                            if (!dx_du.isZero()) {
                                SymExpr new_integrand = simplify(subs(varPart_u_frac, var, *x_sol) * dx_du);
                                SymExpr integrand_var = subs(new_integrand, u_var_frac, SymExpr::makeVar(var));
                                if (auto int_var = doInteg(integrand_var, current_depth + 1)) {
                                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Common Fractional Power Substitution Success" << std::endl;
                                    SymExpr back_sub = commonBase ^ SymExpr(Fraction(BigInt(1), lcm_den));
                                    return coeff * simplify(subs(*int_var, var, back_sub));
                                }
                            }
                        }
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.8 三角函数高次幂降幂与拆分 ---
            strats.push_back({"Trig Power Reduction", has_trig ? 700 : 0, [&]() -> std::optional<SymExpr> {
            if (varPart.ptr->getType() == SymType::POW) {
                auto powNode = std::static_pointer_cast<SymPow>(varPart.ptr);
                if (powNode->base->getType() == SymType::FUNC && powNode->exp->getType() == SymType::NUM) {
                    auto func = std::static_pointer_cast<SymFunc>(powNode->base);
                    auto [isInt, n] = extractExactInt(std::static_pointer_cast<SymNum>(powNode->exp)->value);
                    if (isInt && n >= 2 && func->args.size() == 1) {
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Trig Power Reduction: " << func->name << "^" << n << std::endl;
                        SymExpr arg(func->args[0]);
                        if (func->name == "sin") {
                            if (n % 2 == 0) {
                                SymExpr _C = SymExpr::makeVar("_C");
                                SymExpr halfAngle = (SymExpr(BigInt(1)) - _C) / SymExpr(BigInt(2));
                                SymExpr expanded = expand_core(halfAngle ^ SymExpr(BigInt(n / 2)), SymConfig::maxExpandTerms);
                                SymExpr cos2x = SymExpr(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{(SymExpr(BigInt(2)) * arg).ptr}));
                                expanded = subs(expanded, "_C", cos2x);
                                if (auto res = doInteg(expanded, current_depth + 1)) {
                                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Trig Power Reduction Success" << std::endl;
                                    return coeff * (*res);
                                }
                            } else {
                                SymExpr _C = SymExpr::makeVar("_C");
                                SymExpr cosSq = SymExpr(BigInt(1)) - (_C ^ SymExpr(BigInt(2)));
                                SymExpr rem = SymExpr(std::make_shared<SymFunc>("sin", std::vector<std::shared_ptr<SymNode>>{arg.ptr})) * (cosSq ^ SymExpr(BigInt((n - 1) / 2)));
                                SymExpr expanded = expand_core(rem, SymConfig::maxExpandTerms);
                                SymExpr cosx = SymExpr(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                                expanded = subs(expanded, "_C", cosx);
                                if (auto res = doInteg(expanded, current_depth + 1)) {
                                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Trig Power Reduction Success" << std::endl;
                                    return coeff * (*res);
                                }
                            }
                        } else if (func->name == "cos") {
                            if (n % 2 == 0) {
                                SymExpr _C = SymExpr::makeVar("_C");
                                SymExpr halfAngle = (SymExpr(BigInt(1)) + _C) / SymExpr(BigInt(2));
                                SymExpr expanded = expand_core(halfAngle ^ SymExpr(BigInt(n / 2)), SymConfig::maxExpandTerms);
                                SymExpr cos2x = SymExpr(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{(SymExpr(BigInt(2)) * arg).ptr}));
                                expanded = subs(expanded, "_C", cos2x);
                                if (auto res = doInteg(expanded, current_depth + 1)) {
                                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Trig Power Reduction Success" << std::endl;
                                    return coeff * (*res);
                                }
                            } else {
                                SymExpr _S = SymExpr::makeVar("_S");
                                SymExpr sinSq = SymExpr(BigInt(1)) - (_S ^ SymExpr(BigInt(2)));
                                SymExpr rem = SymExpr(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{arg.ptr})) * (sinSq ^ SymExpr(BigInt((n - 1) / 2)));
                                SymExpr expanded = expand_core(rem, SymConfig::maxExpandTerms);
                                SymExpr sinx = SymExpr(std::make_shared<SymFunc>("sin", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                                expanded = subs(expanded, "_S", sinx);
                                if (auto res = doInteg(expanded, current_depth + 1)) {
                                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Trig Power Reduction Success" << std::endl;
                                    return coeff * (*res);
                                }
                            }
                        }
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.9 二次根式三角换元 (Trigonometric Substitution) ---
            strats.push_back({"Trigonometric Substitution", 650, [&]() -> std::optional<SymExpr> {
            if (var != "_t" && var != "_weierstrass_t") {
                SymExpr quadBase;
                bool foundQuad = false;
                
                std::function<void(const std::shared_ptr<SymNode>&)> findQuad = [&](const std::shared_ptr<SymNode>& node) {
                    if (!node || foundQuad) return;
                    if (node->getType() == SymType::POW) {
                        auto powNode = std::static_pointer_cast<SymPow>(node);
                        if (powNode->exp->getType() == SymType::NUM) {
                            auto [isInt, n] = extractExactInt(std::static_pointer_cast<SymNum>(powNode->exp)->value);
                            if (!isInt) {
                                auto coeffs = extractCoeffs(SymExpr(powNode->base), var);
                                if (coeffs.size() == 3 && coeffs[1].isZero() && !coeffs[2].isZero() && !coeffs[0].isZero()) {
                                    quadBase = SymExpr(powNode->base);
                                    foundQuad = true;
                                    return;
                                }
                            }
                        }
                    }
                    if (node->getType() == SymType::ADD) {
                        for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) findQuad(arg);
                    } else if (node->getType() == SymType::MUL) {
                        for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) findQuad(arg);
                    } else if (node->getType() == SymType::FUNC) {
                        for (auto& arg : std::static_pointer_cast<SymFunc>(node)->args) findQuad(arg);
                    }
                };
                
                findQuad(varPart.ptr);
                
                if (foundQuad) {
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Trig Substitution for quadratic: " << quadBase.toString() << std::endl;
                    auto coeffs = extractCoeffs(quadBase, var);
                    SymExpr A = coeffs[2];
                    SymExpr C = coeffs[0];
                    
                    auto isPos = [](const SymExpr& e) {
                        if (e.ptr->getType() == SymType::NUM) return !isCasNegative(std::static_pointer_cast<SymNum>(e.ptr)->value) && !e.isZero();
                        if (e.ptr->getType() == SymType::MUL) {
                            auto mul = std::static_pointer_cast<SymMul>(e.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                return !isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                            }
                        }
                        return true; // 默认符号参数为正，允许代数换元继续
                    };
                    auto isNeg = [](const SymExpr& e) {
                        if (e.ptr->getType() == SymType::NUM) return isCasNegative(std::static_pointer_cast<SymNum>(e.ptr)->value);
                        if (e.ptr->getType() == SymType::MUL) {
                            auto mul = std::static_pointer_cast<SymMul>(e.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                return isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                            }
                        }
                        return false;
                    };
                    
                    bool A_pos = isPos(A), A_neg = isNeg(A);
                    bool C_pos = isPos(C), C_neg = isNeg(C);
                    
                    SymExpr t = SymExpr::makeVar("_t");
                    SymExpr x_sub, dx_sub, t_back;
                    bool valid = false;
                    
                    if (A_pos && C_pos) {
                        SymExpr sqrtCA = simplifyCore((C / A) ^ SymExpr(Fraction(1, 2)));
                        SymExpr sinh_t(std::make_shared<SymFunc>("sinh", std::vector<std::shared_ptr<SymNode>>{t.ptr}));
                        SymExpr cosh_t(std::make_shared<SymFunc>("cosh", std::vector<std::shared_ptr<SymNode>>{t.ptr}));
                        x_sub = sqrtCA * sinh_t;
                        dx_sub = sqrtCA * cosh_t;
                        SymExpr asinh_arg = simplifyCore(SymExpr::makeVar(var) / sqrtCA);
                        t_back = SymExpr(std::make_shared<SymFunc>("asinh", std::vector<std::shared_ptr<SymNode>>{asinh_arg.ptr}));
                        valid = true;
                    } else if (A_neg && C_pos) {
                        SymExpr sqrtCA = simplifyCore((-C / A) ^ SymExpr(Fraction(1, 2)));
                        SymExpr sin_t(std::make_shared<SymFunc>("sin", std::vector<std::shared_ptr<SymNode>>{t.ptr}));
                        SymExpr cos_t(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{t.ptr}));
                        x_sub = sqrtCA * sin_t;
                        dx_sub = sqrtCA * cos_t;
                        SymExpr asin_arg = simplifyCore(SymExpr::makeVar(var) / sqrtCA);
                        t_back = SymExpr(std::make_shared<SymFunc>("asin", std::vector<std::shared_ptr<SymNode>>{asin_arg.ptr}));
                        valid = true;
                    } else if (A_pos && C_neg) {
                        SymExpr sqrtCA = simplifyCore((-C / A) ^ SymExpr(Fraction(1, 2)));
                        SymExpr cosh_t(std::make_shared<SymFunc>("cosh", std::vector<std::shared_ptr<SymNode>>{t.ptr}));
                        SymExpr sinh_t(std::make_shared<SymFunc>("sinh", std::vector<std::shared_ptr<SymNode>>{t.ptr}));
                        x_sub = sqrtCA * cosh_t;
                        dx_sub = sqrtCA * sinh_t;
                        SymExpr acosh_arg = simplifyCore(SymExpr::makeVar(var) / sqrtCA);
                        t_back = SymExpr(std::make_shared<SymFunc>("acosh", std::vector<std::shared_ptr<SymNode>>{acosh_arg.ptr}));
                        valid = true;
                    }
                    
                    if (valid) {
                        SymExpr subbed = simplifyCore(subs(varPart, var, x_sub) * dx_sub);
                        SymExpr subbed_var = subs(subbed, "_t", SymExpr::makeVar(var));
                        
                        auto hyperbolicSimp = [&](const SymExpr& e) -> SymExpr {
                            SymExpr _x = SymExpr::makeVar("_x");
                            SymExpr _c = SymExpr::makeVar("_c");
                            auto func = [](const std::string& name, const SymExpr& arg) {
                                return SymExpr(std::make_shared<SymFunc>(name, std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                            };
                            SymExpr sinh_x = func("sinh", _x);
                            SymExpr cosh_x = func("cosh", _x);
                            
                            SymExpr res = e;
                            // 基础平方关系
                            res = applyRule(res, (sinh_x ^ SymExpr(BigInt(2))) + SymExpr(BigInt(1)), cosh_x ^ SymExpr(BigInt(2)));
                            res = applyRule(res, (cosh_x ^ SymExpr(BigInt(2))) - SymExpr(BigInt(1)), sinh_x ^ SymExpr(BigInt(2)));
                            res = applyRule(res, _c * (sinh_x ^ SymExpr(BigInt(2))) + _c, _c * (cosh_x ^ SymExpr(BigInt(2))));
                            res = applyRule(res, _c * (cosh_x ^ SymExpr(BigInt(2))) - _c, _c * (sinh_x ^ SymExpr(BigInt(2))));

                            // 根式关系
                            res = applyRule(res, ((sinh_x ^ SymExpr(BigInt(2))) + SymExpr(BigInt(1))) ^ SymExpr(Fraction(1, 2)), cosh_x);
                            res = applyRule(res, ((cosh_x ^ SymExpr(BigInt(2))) - SymExpr(BigInt(1))) ^ SymExpr(Fraction(1, 2)), sinh_x);
                            res = applyRule(res, (_c * (sinh_x ^ SymExpr(BigInt(2))) + _c) ^ SymExpr(Fraction(1, 2)), (_c ^ SymExpr(Fraction(1, 2))) * cosh_x);
                            res = applyRule(res, (_c * (cosh_x ^ SymExpr(BigInt(2))) - _c) ^ SymExpr(Fraction(1, 2)), (_c ^ SymExpr(Fraction(1, 2))) * sinh_x);
                            
                            return simplifyCore(res);
                        };
                        
                        subbed_var = hyperbolicSimp(subbed_var);
                        
                        if (auto int_var = doInteg(trigsimp(subbed_var), current_depth + 1)) {
                            if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Trig Substitution Success" << std::endl;
                            SymExpr res = subs(*int_var, var, t_back);
                            return coeff * res;
                        }
                        SymExpr exp_subbed = trigToExp(subbed_var);
                        if (exp_subbed != subbed_var) {
                            exp_subbed = simplifyCore(expand_core(exp_subbed, SymConfig::maxExpandTerms));
                            if (auto int_exp = doInteg(exp_subbed, current_depth + 1)) {
                                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Trig Substitution (via Exp) Success" << std::endl;
                                SymExpr res = subs(expToTrig(*int_exp), var, t_back);
                                return coeff * res;
                            }
                        }
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.94 双二次分式积分 (Biquadratic Fraction) ---
            strats.push_back({"Biquadratic Fraction", 870, [&]() -> std::optional<SymExpr> {
            if (varPart.ptr->getType() == SymType::MUL || varPart.ptr->getType() == SymType::POW) {
                SymExpr bqBase;
                int bqM = 0;
                bool isBq = false;
                
                if (varPart.ptr->getType() == SymType::POW) {
                    auto powNode = std::static_pointer_cast<SymPow>(varPart.ptr);
                    if (powNode->exp->getType() == SymType::NUM) {
                        auto [isIntExp, expVal] = extractExactInt(std::static_pointer_cast<SymNum>(powNode->exp)->value);
                        if (isIntExp && expVal == -1) {
                            bqBase = SymExpr(powNode->base);
                            bqM = 0;
                            isBq = true;
                        }
                    }
                } else if (varPart.ptr->getType() == SymType::MUL) {
                    auto mul = std::static_pointer_cast<SymMul>(varPart.ptr);
                    if (mul->args.size() == 2) {
                        SymExpr arg1(mul->args[0]);
                        SymExpr arg2(mul->args[1]);
                        auto checkBq = [&](const SymExpr& p1, const SymExpr& p2) {
                            if (p2.ptr->getType() == SymType::POW) {
                                auto powNode = std::static_pointer_cast<SymPow>(p2.ptr);
                                if (powNode->exp->getType() == SymType::NUM) {
                                    auto [isIntExp, expVal] = extractExactInt(std::static_pointer_cast<SymNum>(powNode->exp)->value);
                                    if (isIntExp && expVal == -1) {
                                        if (p1.ptr->getType() == SymType::POW) {
                                            auto p1Pow = std::static_pointer_cast<SymPow>(p1.ptr);
                                            if (p1Pow->base->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(p1Pow->base)->name == var) {
                                                if (p1Pow->exp->getType() == SymType::NUM) {
                                                    auto [isMInt, mVal] = extractExactInt(std::static_pointer_cast<SymNum>(p1Pow->exp)->value);
                                                    if (isMInt && (mVal == 2)) {
                                                        bqBase = SymExpr(powNode->base);
                                                        bqM = 2;
                                                        isBq = true;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        };
                        checkBq(arg1, arg2);
                        if (!isBq) checkBq(arg2, arg1);
                    }
                }

                if (isBq) {
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Biquadratic Fraction: " << bqBase.toString() << std::endl;
                    auto coeffs = extractCoeffs(bqBase, var);
                    if (coeffs.size() == 5 && !coeffs[4].isZero() && coeffs[3].isZero() && coeffs[2].isZero() && coeffs[1].isZero() && !coeffs[0].isZero()) {
                        SymExpr A = coeffs[4];
                        SymExpr B = coeffs[0];
                        
                        auto isPos = [](const SymExpr& e) {
                            if (e.ptr->getType() == SymType::NUM) return !isCasNegative(std::static_pointer_cast<SymNum>(e.ptr)->value) && !e.isZero();
                            if (e.ptr->getType() == SymType::MUL) {
                                auto mul = std::static_pointer_cast<SymMul>(e.ptr);
                                if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                    return !isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                                }
                            }
                            return true; // 默认符号参数为正，允许代数换元继续
                        };
                        
                        if (isPos(A) && isPos(B)) {
                            SymExpr C = simplifyCore((B / A) ^ SymExpr(Fraction(1, 2)));
                            SymExpr sqrt2C = simplifyCore((SymExpr(BigInt(2)) * C) ^ SymExpr(Fraction(1, 2)));
                            SymExpr sqrtC_half = simplifyCore((C / SymExpr(BigInt(2))) ^ SymExpr(Fraction(1, 2)));
                            
                            SymExpr x_sym = SymExpr::makeVar(var);
                            SymExpr x2 = x_sym * x_sym;
                            
                            SymExpr log_term1 = simplifyCore(x2 - sqrt2C * x_sym + C);
                            SymExpr log_term2 = simplifyCore(x2 + sqrt2C * x_sym + C);
                            
                            SymExpr atan_arg1 = simplifyCore((x_sym - sqrtC_half) / sqrtC_half);
                            SymExpr atan_arg2 = simplifyCore((x_sym + sqrtC_half) / sqrtC_half);
                            
                            SymExpr atan1(std::make_shared<SymFunc>("atan", std::vector<std::shared_ptr<SymNode>>{atan_arg1.ptr}));
                            SymExpr atan2(std::make_shared<SymFunc>("atan", std::vector<std::shared_ptr<SymNode>>{atan_arg2.ptr}));
                            SymExpr log1(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{log_term1.ptr}));
                            SymExpr log2(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{log_term2.ptr}));
                            
                            if (bqM == 2) {
                                SymExpr part1 = simplifyCore(SymExpr(BigInt(1)) / (SymExpr(BigInt(4)) * A * sqrt2C));
                                SymExpr part2 = simplifyCore(SymExpr(BigInt(1)) / (SymExpr(BigInt(2)) * A * sqrt2C));
                                SymExpr res = part1 * (log1 - log2) + part2 * (atan1 + atan2);
                                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Biquadratic Fraction Success" << std::endl;
                                return coeff * res;
                            } else if (bqM == 0) {
                                SymExpr part1 = simplifyCore(SymExpr(BigInt(1)) / (SymExpr(BigInt(4)) * A * C * sqrt2C));
                                SymExpr part2 = simplifyCore(SymExpr(BigInt(1)) / (SymExpr(BigInt(2)) * A * C * sqrt2C));
                                SymExpr res = part1 * (log2 - log1) + part2 * (atan1 + atan2);
                                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Biquadratic Fraction Success" << std::endl;
                                return coeff * res;
                            }
                        }
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.95 高次二项式分式积分 (Binomial Fraction Integration) ---
            strats.push_back({"Binomial Fraction Integration", 860, [&]() -> std::optional<SymExpr> {
            if (varPart.ptr->getType() == SymType::POW) {
                auto powNode = std::static_pointer_cast<SymPow>(varPart.ptr);
                if (powNode->exp->getType() == SymType::NUM) {
                    auto [isIntExp, expVal] = extractExactInt(std::static_pointer_cast<SymNum>(powNode->exp)->value);
                    if (isIntExp && expVal == -1) {
                        auto coeffs = extractCoeffs(SymExpr(powNode->base), var);
                        if (coeffs.size() > 2) {
                            bool isBinomial = true;
                            int n = static_cast<int>(coeffs.size()) - 1;
                            for (int i = 1; i < n; ++i) {
                                if (!coeffs[i].isZero()) { isBinomial = false; break; }
                            }
                            if (isBinomial && !coeffs[n].isZero() && !coeffs[0].isZero()) {
                                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Binomial Fraction: " << powNode->base->toString() << "^-1" << std::endl;
                                SymExpr A = coeffs[n];
                                SymExpr B = coeffs[0];
                                
                                SymExpr C = simplifyCore(A / B);
                                bool isPos = false;
                                bool isNeg = false;
                                if (C.ptr->getType() == SymType::NUM) {
                                    isNeg = isCasNegative(std::static_pointer_cast<SymNum>(C.ptr)->value);
                                    isPos = !isNeg && !C.isZero();
                                } else if (C.ptr->getType() == SymType::MUL) {
                                    auto mul = std::static_pointer_cast<SymMul>(C.ptr);
                                    if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                        isNeg = isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                                        isPos = !isNeg;
                                    } else {
                                        isPos = true;
                                    }
                                } else {
                                    isPos = true;
                                }
                                
                                SymExpr absC = isNeg ? simplifyCore(-C) : C;
                                SymExpr u_coeff = simplifyCore(absC ^ SymExpr(Fraction(1, n)));
                                SymExpr u = simplifyCore(u_coeff * SymExpr::makeVar(var));
                                SymExpr du_dx = u_coeff;
                                
                                SymExpr n_sym = SymExpr(BigInt(n));
                                SymExpr res(BigInt(0));
                                SymExpr PI = SymExpr::makeVar("PI");
                                
                                auto makeCos = [&](SymExpr theta) {
                                    return SymExpr(std::make_shared<SymFunc>("cos", std::vector<std::shared_ptr<SymNode>>{theta.ptr}));
                                };
                                auto makeSin = [&](SymExpr theta) {
                                    return SymExpr(std::make_shared<SymFunc>("sin", std::vector<std::shared_ptr<SymNode>>{theta.ptr}));
                                };
                                auto makeLog = [&](SymExpr arg) {
                                    return SymExpr(std::make_shared<SymFunc>("log", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                                };
                                auto makeAtan = [&](SymExpr arg) {
                                    return SymExpr(std::make_shared<SymFunc>("atan", std::vector<std::shared_ptr<SymNode>>{arg.ptr}));
                                };
                                
                                if (isPos) {
                                    if (n % 2 != 0) {
                                        res = res + (SymExpr(BigInt(1)) / n_sym) * makeLog(u + SymExpr(BigInt(1)));
                                        for (int k = 1; k <= (n - 1) / 2; ++k) {
                                            SymExpr theta = simplifyCore((SymExpr(BigInt(2 * k - 1)) * PI) / n_sym);
                                            SymExpr cos_th = makeCos(theta);
                                            SymExpr sin_th = makeSin(theta);
                                            SymExpr log_arg = simplifyCore((u ^ SymExpr(BigInt(2))) - SymExpr(BigInt(2)) * u * cos_th + SymExpr(BigInt(1)));
                                            SymExpr atan_arg = simplifyCore((u - cos_th) / sin_th);
                                            res = res - (cos_th / n_sym) * makeLog(log_arg) + (SymExpr(BigInt(2)) * sin_th / n_sym) * makeAtan(atan_arg);
                                        }
                                    } else {
                                        for (int k = 1; k <= n / 2; ++k) {
                                            SymExpr theta = simplifyCore((SymExpr(BigInt(2 * k - 1)) * PI) / n_sym);
                                            SymExpr cos_th = makeCos(theta);
                                            SymExpr sin_th = makeSin(theta);
                                            SymExpr log_arg = simplifyCore((u ^ SymExpr(BigInt(2))) - SymExpr(BigInt(2)) * u * cos_th + SymExpr(BigInt(1)));
                                            SymExpr atan_arg = simplifyCore((u - cos_th) / sin_th);
                                            res = res - (cos_th / n_sym) * makeLog(log_arg) + (SymExpr(BigInt(2)) * sin_th / n_sym) * makeAtan(atan_arg);
                                        }
                                    }
                                } else {
                                    SymExpr res_minus(BigInt(0));
                                    if (n % 2 != 0) {
                                        res_minus = res_minus + (SymExpr(BigInt(1)) / n_sym) * makeLog(u - SymExpr(BigInt(1)));
                                        for (int k = 1; k <= (n - 1) / 2; ++k) {
                                            SymExpr phi = simplifyCore((SymExpr(BigInt(2 * k)) * PI) / n_sym);
                                            SymExpr cos_ph = makeCos(phi);
                                            SymExpr sin_ph = makeSin(phi);
                                            SymExpr log_arg = simplifyCore((u ^ SymExpr(BigInt(2))) - SymExpr(BigInt(2)) * u * cos_ph + SymExpr(BigInt(1)));
                                            SymExpr atan_arg = simplifyCore((u - cos_ph) / sin_ph);
                                            res_minus = res_minus + (cos_ph / n_sym) * makeLog(log_arg) - (SymExpr(BigInt(2)) * sin_ph / n_sym) * makeAtan(atan_arg);
                                        }
                                    } else {
                                        res_minus = res_minus + (SymExpr(BigInt(1)) / n_sym) * makeLog(u - SymExpr(BigInt(1))) - (SymExpr(BigInt(1)) / n_sym) * makeLog(u + SymExpr(BigInt(1)));
                                        for (int k = 1; k <= n / 2 - 1; ++k) {
                                            SymExpr phi = simplifyCore((SymExpr(BigInt(2 * k)) * PI) / n_sym);
                                            SymExpr cos_ph = makeCos(phi);
                                            SymExpr sin_ph = makeSin(phi);
                                            SymExpr log_arg = simplifyCore((u ^ SymExpr(BigInt(2))) - SymExpr(BigInt(2)) * u * cos_ph + SymExpr(BigInt(1)));
                                            SymExpr atan_arg = simplifyCore((u - cos_ph) / sin_ph);
                                            res_minus = res_minus + (cos_ph / n_sym) * makeLog(log_arg) - (SymExpr(BigInt(2)) * sin_ph / n_sym) * makeAtan(atan_arg);
                                        }
                                    }
                                    res = -res_minus;
                                }
                                
                                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Binomial Fraction Success" << std::endl;
                                return coeff * simplifyCore(res / (B * du_dx));
                            }
                        }
                    }
                }
            }
            return std::nullopt;
            }});

            auto [num, den] = getFraction(varPart);
            bool isFraction = !den.isOne();

            // --- 1.98 二次配方法启发式 (Inverse Quadratic Integration) ---
            strats.push_back({"Inverse Quadratic Integration", isFraction ? 875 : 0, [&]() -> std::optional<SymExpr> {
            if (isFraction && !containsVar(num.ptr, var)) {
                auto coeffs = extractCoeffs(den, var);
                if (coeffs.size() == 3 && !coeffs[2].isZero()) {
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Inverse Quadratic: " << den.toString() << std::endl;
                    SymExpr A = coeffs[2];
                    SymExpr B = coeffs[1];
                    SymExpr C = coeffs[0];
                    
                    SymExpr Delta = simplifyCore((B ^ SymExpr(BigInt(2))) - SymExpr(BigInt(4)) * A * C);
                    
                    bool isDeltaNeg = false;
                    if (Delta.ptr->getType() == SymType::NUM) {
                        isDeltaNeg = isCasNegative(std::static_pointer_cast<SymNum>(Delta.ptr)->value);
                    } else if (Delta.ptr->getType() == SymType::MUL) {
                        auto mul = std::static_pointer_cast<SymMul>(Delta.ptr);
                        if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                            isDeltaNeg = isCasNegative(std::static_pointer_cast<SymNum>(mul->args[0])->value);
                        }
                    }
                    
                    if (isDeltaNeg) {
                        SymExpr negDelta = simplifyCore(-Delta);
                        SymExpr sqrtNegDelta = simplifyCore(negDelta ^ SymExpr(Fraction(1, 2)));
                        
                        SymExpr atan_arg = simplifyCore((SymExpr(BigInt(2)) * A * SymExpr::makeVar(var) + B) / sqrtNegDelta);
                        SymExpr atan_term = SymExpr(std::make_shared<SymFunc>("atan", std::vector<std::shared_ptr<SymNode>>{atan_arg.ptr}));
                        
                        SymExpr res = simplifyCore((SymExpr(BigInt(2)) * num / sqrtNegDelta) * atan_term);
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Inverse Quadratic Success" << std::endl;
                        return coeff * res;
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 1.99 多项式-指数型速通 (Poly-Exp Fast Path) ---
            strats.push_back({"Poly-Exp Fast Path", (has_exp || has_trig) ? 1100 : 0, [&]() -> std::optional<SymExpr> {
            if (varPart.ptr->getType() == SymType::MUL || varPart.ptr->getType() == SymType::FUNC || varPart.ptr->getType() == SymType::POW) {
                SymExpr exp_varPart = simplifyCore(expand_core(trigToExp(varPart), SymConfig::maxExpandTerms));
                
                auto processTerm = [&](const SymExpr& term) -> std::optional<SymExpr> {
                    SymExpr poly_factor(BigInt(1));
                    SymExpr total_w(BigInt(0));
                    bool found_exp = false;
                    
                    auto checkExp = [&](const SymExpr& arg) -> bool {
                        if (arg.ptr->getType() == SymType::FUNC) {
                            auto func = std::static_pointer_cast<SymFunc>(arg.ptr);
                            if (func->name == "exp" && func->args.size() == 1) {
                                SymExpr w = SymExpr(func->args[0]);
                                SymExpr dw = simplifyCore(diff(w, var));
                                if (!containsVar(dw.ptr, var)) {
                                    total_w = simplifyCore(total_w + w);
                                    found_exp = true;
                                    return true;
                                }
                            }
                        }
                        return false;
                    };

                    if (term.ptr->getType() == SymType::MUL) {
                        auto mul = std::static_pointer_cast<SymMul>(term.ptr);
                        for (auto& arg : mul->args) {
                            if (!checkExp(SymExpr(arg))) {
                                poly_factor = poly_factor * SymExpr(arg);
                            }
                        }
                    } else if (term.ptr->getType() == SymType::FUNC) {
                        if (!checkExp(term)) {
                            poly_factor = term;
                        }
                    } else {
                        poly_factor = term;
                    }

                    poly_factor = simplifyCore(expand_core(poly_factor, SymConfig::maxExpandTerms));
                    
                    if (found_exp) {
                        SymExpr c_val = simplifyCore(diff(total_w, var));
                        if (!c_val.isZero()) {
                            SymExpr exp_factor(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{total_w.ptr}));
                            if (isPolynomialIn(poly_factor, var)) {
                                auto coeffs = extractCoeffs(poly_factor, var);
                                SymExpr result(BigInt(0));
                                
                                for (size_t n = 0; n < coeffs.size(); ++n) {
                                    if (coeffs[n].isZero()) continue;
                                    
                                    SymExpr termSum(BigInt(0));
                                    BigInt perm(1);
                                    
                                    for (size_t k = 0; k <= n; ++k) {
                                        int sign = (k % 2 == 0) ? 1 : -1;
                                        SymExpr inv_c_pow = c_val ^ SymExpr(BigInt(-static_cast<int64_t>(k + 1)));
                                        SymExpr termCoeff = simplifyCore(SymExpr(BigInt(sign) * perm) * inv_c_pow);
                                        SymExpr term_x = termCoeff * (SymExpr::makeVar(var) ^ SymExpr(BigInt(static_cast<int64_t>(n - k))));
                                        termSum = termSum + term_x;
                                        
                                        perm = perm * BigInt(static_cast<int64_t>(n - k));
                                    }
                                    result = result + coeffs[n] * termSum;
                                }
                                return simplifyCore(result * exp_factor);
                            }
                        } else {
                            SymExpr exp_factor(std::make_shared<SymFunc>("exp", std::vector<std::shared_ptr<SymNode>>{total_w.ptr}));
                            poly_factor = simplifyCore(poly_factor * exp_factor);
                            if (isPolynomialIn(poly_factor, var)) {
                                auto coeffs = extractCoeffs(poly_factor, var);
                                SymExpr result(BigInt(0));
                                for (size_t n = 0; n < coeffs.size(); ++n) {
                                    if (coeffs[n].isZero()) continue;
                                    result = result + coeffs[n] * (SymExpr::makeVar(var) ^ SymExpr(BigInt(static_cast<int64_t>(n + 1)))) / SymExpr(BigInt(static_cast<int64_t>(n + 1)));
                                }
                                return result;
                            }
                        }
                    } else {
                        if (isPolynomialIn(poly_factor, var)) {
                            auto coeffs = extractCoeffs(poly_factor, var);
                            SymExpr result(BigInt(0));
                            for (size_t n = 0; n < coeffs.size(); ++n) {
                                if (coeffs[n].isZero()) continue;
                                result = result + coeffs[n] * (SymExpr::makeVar(var) ^ SymExpr(BigInt(static_cast<int64_t>(n + 1)))) / SymExpr(BigInt(static_cast<int64_t>(n + 1)));
                            }
                            return result;
                        }
                    }
                    return std::nullopt;
                };

                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Poly-Exp Fast Path" << std::endl;

                auto simplifyComplex = [](const SymExpr& e) -> SymExpr {
                    int trig_counter = 0;
                    std::map<std::string, SymExpr> trig_map;
                    std::map<std::string, std::string> sig_to_name;
                    
                    std::function<SymExpr(const SymExpr&)> protectTrig = [&](const SymExpr& node) -> SymExpr {
                        if (!node.ptr) return node;
                        if (node.ptr->getType() == SymType::FUNC) {
                            auto func = std::static_pointer_cast<SymFunc>(node.ptr);
                            if (func->name == "sin" || func->name == "cos" || func->name == "tan" ||
                                func->name == "sinh" || func->name == "cosh" || func->name == "tanh") {
                                std::string sig = node.ptr->getSignature();
                                if (sig_to_name.count(sig)) {
                                    return SymExpr::makeVar(sig_to_name[sig]);
                                }
                                std::string name = "_trig_protect_" + std::to_string(++trig_counter);
                                sig_to_name[sig] = name;
                                trig_map[name] = node;
                                return SymExpr::makeVar(name);
                            }
                            std::vector<std::shared_ptr<SymNode>> newArgs;
                            for (auto& arg : func->args) newArgs.push_back(protectTrig(SymExpr(arg)).ptr);
                            return SymExpr(std::make_shared<SymFunc>(func->name, std::move(newArgs)));
                        }
                        if (node.ptr->getType() == SymType::ADD) {
                            SymExpr res(BigInt(0));
                            for (auto& arg : std::static_pointer_cast<SymAdd>(node.ptr)->args) res = res + protectTrig(SymExpr(arg));
                            return res;
                        }
                        if (node.ptr->getType() == SymType::MUL) {
                            SymExpr res(BigInt(1));
                            for (auto& arg : std::static_pointer_cast<SymMul>(node.ptr)->args) res = res * protectTrig(SymExpr(arg));
                            return res;
                        }
                        if (node.ptr->getType() == SymType::POW) {
                            auto powNode = std::static_pointer_cast<SymPow>(node.ptr);
                            return protectTrig(SymExpr(powNode->base)) ^ protectTrig(SymExpr(powNode->exp));
                        }
                        return node;
                    };
                    
                    SymExpr protected_e = protectTrig(e);
                    SymExpr expanded = simplifyCore(expand_core(protected_e, SymConfig::maxExpandTerms));
                    auto coeffs = extractCoeffs(expanded, "i");
                    SymExpr real_part(BigInt(0)), imag_part(BigInt(0));
                    for (size_t k = 0; k < coeffs.size(); ++k) {
                        if (k % 4 == 0) real_part = real_part + coeffs[k];
                        else if (k % 4 == 1) imag_part = imag_part + coeffs[k];
                        else if (k % 4 == 2) real_part = real_part - coeffs[k];
                        else if (k % 4 == 3) imag_part = imag_part - coeffs[k];
                    }
                    
                    auto unprotectTrig = [&](SymExpr node) -> SymExpr {
                        for (auto it = trig_map.rbegin(); it != trig_map.rend(); ++it) {
                            node = subs(node, it->first, it->second);
                        }
                        return node;
                    };
                    
                    auto simplifyTerms = [&](const SymExpr& expr_node) -> SymExpr {
                        if (expr_node.ptr->getType() == SymType::ADD) {
                            std::map<std::string, SymExpr> groups;
                            for (auto& arg : std::static_pointer_cast<SymAdd>(expr_node.ptr)->args) {
                                auto [n, d] = getFraction(SymExpr(arg));
                                std::string d_sig = d.ptr->getSignature();
                                if (groups.count(d_sig)) groups[d_sig] = groups[d_sig] + SymExpr(arg);
                                else groups[d_sig] = SymExpr(arg);
                            }
                            SymExpr res_add(BigInt(0));
                            for (auto& kv : groups) {
                                res_add = res_add + simplifyRational(kv.second);
                            }
                            return res_add;
                        }
                        return simplifyRational(expr_node);
                    };
                    
                    SymExpr res = unprotectTrig(simplifyTerms(real_part));
                    if (!imag_part.isZero()) {
                        res = res + unprotectTrig(simplifyTerms(imag_part)) * SymExpr::makeVar("i");
                    }
                    return res;
                };

                if (exp_varPart.ptr->getType() == SymType::ADD) {
                    SymExpr totalRes(BigInt(0));
                    auto add = std::static_pointer_cast<SymAdd>(exp_varPart.ptr);
                    for (auto& arg : add->args) {
                        auto termRes = processTerm(SymExpr(arg));
                        if (!termRes) return std::nullopt;
                        totalRes = totalRes + *termRes;
                    }
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Poly-Exp Fast Path Success (ADD)" << std::endl;
                    return coeff * simplifyComplex(expToTrig(totalRes));
                } else {
                    auto termRes = processTerm(exp_varPart);
                    if (termRes) {
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Poly-Exp Fast Path Success" << std::endl;
                        return coeff * simplifyComplex(expToTrig(*termRes));
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 2. 有理分式积分引擎 (Rational Function Integration) ---
            strats.push_back({"Rational Function Integration", isFraction ? 850 : 0, [&]() -> std::optional<SymExpr> {
            if (isFraction && containsVar(den.ptr, var)) {
                // 参数化常数提取与隔离黑盒 (Symbolic Constant Blackboxing)
                std::map<std::string, SymExpr> constDict;
                int constCounter = 0;
                std::function<SymExpr(const SymExpr&)> blackboxConstants = [&](const SymExpr& node) -> SymExpr {
                    if (!node.ptr) return node;
                    if (!containsVar(node.ptr, var)) {
                        if (getAstNodeCount(node) > 5) {
                            std::string cName = "_C_box" + std::to_string(++constCounter);
                            constDict[cName] = node;
                            return SymExpr::makeVar(cName);
                        }
                        return node;
                    }
                    switch (node.ptr->getType()) {
                        case SymType::ADD: {
                            SymExpr res(BigInt(0));
                            for (auto& arg : std::static_pointer_cast<SymAdd>(node.ptr)->args) res = res + blackboxConstants(SymExpr(arg));
                            return res;
                        }
                        case SymType::MUL: {
                            SymExpr res(BigInt(1));
                            for (auto& arg : std::static_pointer_cast<SymMul>(node.ptr)->args) res = res * blackboxConstants(SymExpr(arg));
                            return res;
                        }
                        case SymType::POW: {
                            auto p = std::static_pointer_cast<SymPow>(node.ptr);
                            return blackboxConstants(SymExpr(p->base)) ^ blackboxConstants(SymExpr(p->exp));
                        }
                        case SymType::FUNC: {
                            auto f = std::static_pointer_cast<SymFunc>(node.ptr);
                            std::vector<std::shared_ptr<SymNode>> nArgs;
                            for (auto& arg : f->args) nArgs.push_back(blackboxConstants(SymExpr(arg)).ptr);
                            return SymExpr(std::make_shared<SymFunc>(f->name, std::move(nArgs)));
                        }
                        default: return node;
                    }
                };

                SymExpr boxed_num = blackboxConstants(num);
                SymExpr boxed_den = blackboxConstants(den);

                SymExpr num_expanded = simplifyCore(expand_core(boxed_num, SymConfig::maxExpandTerms));
                SymExpr den_expanded = simplifyCore(expand_core(boxed_den, SymConfig::maxExpandTerms));
                
                int degN = getDegree(num_expanded, var);
                int degD = getDegree(den_expanded, var);
                
                // ★ 只有当分子和分母都是多项式时，才使用有理分式积分引擎
                if (degD >= 0 && (degN >= 0 || !containsVar(num_expanded.ptr, var))) {
                    try {
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Rational Function Integration" << std::endl;
                        auto [ratPart, polyPart, cA, cD] = hermiteReduce(num_expanded, den_expanded, var);
                        if (SymConfig::debugIntegration) {
                            std::cout << std::string(current_depth * 2, ' ') << "   ratPart: " << ratPart.toString() << std::endl;
                            std::cout << std::string(current_depth * 2, ' ') << "   polyPart: " << polyPart.toString() << std::endl;
                            std::cout << std::string(current_depth * 2, ' ') << "   cA: " << cA.toString() << std::endl;
                            std::cout << std::string(current_depth * 2, ' ') << "   cD: " << cD.toString() << std::endl;
                        }
                        SymExpr res = ratPart;
                        
                        if (!polyPart.isZero()) {
                            SymExpr unboxed_poly = polyPart;
                            for (const auto& kv : constDict) unboxed_poly = subs(unboxed_poly, kv.first, kv.second);
                            if (auto polyInt = doInteg(unboxed_poly, current_depth)) {
                                res = res + *polyInt;
                            } else {
                                throw std::runtime_error("polyPart integration failed");
                            }
                        }
                        
                        if (!cA.isZero()) {
                            SymExpr rtRes = rothsteinTrager(cA, cD, var);
                            if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "   Rothstein-Trager result: " << rtRes.toString() << std::endl;
                            res = res + rtRes;
                        }
                        
                        for (const auto& kv : constDict) {
                            res = subs(res, kv.first, kv.second);
                        }
                        
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Rational Function Success" << std::endl;
                        return coeff * res;
                    } catch (const EngineInterruptError&) {
                        throw;
                    } catch (...) {
                        // 有理分式积分失败，静默吞没，继续尝试后续方法（如 Risch 算法）
                    }
                }
            }
            return std::nullopt;
            }});

            // --- 3. 万能公式换元 (Weierstrass Substitution) ---
            strats.push_back({"Weierstrass Substitution", has_trig ? 400 : 0, [&]() -> std::optional<SymExpr> {
            bool hasTrig = false;
            std::function<bool(const std::shared_ptr<SymNode>&)> isRationalTrig = [&](const std::shared_ptr<SymNode>& node) -> bool {
                if (!node) return true;
                switch (node->getType()) {
                    case SymType::NUM: return true;
                    case SymType::VAR: return std::static_pointer_cast<SymVar>(node)->name != var;
                    case SymType::ADD:
                        for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args)
                            if (!isRationalTrig(arg)) return false;
                        return true;
                    case SymType::MUL:
                        for (auto& arg : std::static_pointer_cast<SymMul>(node)->args)
                            if (!isRationalTrig(arg)) return false;
                        return true;
                    case SymType::POW: {
                        auto powNode = std::static_pointer_cast<SymPow>(node);
                        if (!isRationalTrig(powNode->base)) return false;
                        if (containsVar(powNode->exp, var)) return false;
                        if (powNode->exp->getType() == SymType::NUM) {
                            auto [isInt, n] = extractExactInt(std::static_pointer_cast<SymNum>(powNode->exp)->value);
                            if (!isInt) return false;
                        } else {
                            return false;
                        }
                        return true;
                    }
                    case SymType::FUNC: {
                        auto func = std::static_pointer_cast<SymFunc>(node);
                        if (func->args.size() == 1 && func->args[0]->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(func->args[0])->name == var) {
                            if (func->name == "sin" || func->name == "cos" || func->name == "tan") {
                                hasTrig = true;
                                return true;
                            }
                            return false;
                        }
                        for (auto& arg : func->args)
                            if (containsVar(arg, var)) return false;
                        return true;
                    }
                }
                return false;
            };

            if (isRationalTrig(varPart.ptr) && hasTrig) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Weierstrass Substitution" << std::endl;
                std::string t_var = "_weierstrass_t";
                SymExpr t = SymExpr::makeVar(t_var);
                SymExpr sin_sub = (SymExpr(BigInt(2)) * t) / (SymExpr(BigInt(1)) + t * t);
                SymExpr cos_sub = (SymExpr(BigInt(1)) - t * t) / (SymExpr(BigInt(1)) + t * t);
                SymExpr tan_sub = (SymExpr(BigInt(2)) * t) / (SymExpr(BigInt(1)) - t * t);
                SymExpr dt_sub = SymExpr(BigInt(2)) / (SymExpr(BigInt(1)) + t * t);

                std::function<SymExpr(const std::shared_ptr<SymNode>&)> applyWeierstrass = [&](const std::shared_ptr<SymNode>& node) -> SymExpr {
                    if (!node) return SymExpr(BigInt(0));
                    switch (node->getType()) {
                        case SymType::NUM: return SymExpr(node);
                        case SymType::VAR: return SymExpr(node);
                        case SymType::ADD: {
                            SymExpr res(BigInt(0));
                            for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) res = res + applyWeierstrass(arg);
                            return res;
                        }
                        case SymType::MUL: {
                            SymExpr res(BigInt(1));
                            for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) res = res * applyWeierstrass(arg);
                            return res;
                        }
                        case SymType::POW: {
                            auto powNode = std::static_pointer_cast<SymPow>(node);
                            return applyWeierstrass(powNode->base) ^ applyWeierstrass(powNode->exp);
                        }
                        case SymType::FUNC: {
                            auto func = std::static_pointer_cast<SymFunc>(node);
                            if (func->args.size() == 1 && func->args[0]->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(func->args[0])->name == var) {
                                if (func->name == "sin") return sin_sub;
                                if (func->name == "cos") return cos_sub;
                                if (func->name == "tan") return tan_sub;
                            }
                            std::vector<std::shared_ptr<SymNode>> newArgs;
                            for (auto& arg : func->args) newArgs.push_back(applyWeierstrass(arg).ptr);
                            return SymExpr(std::make_shared<SymFunc>(func->name, std::move(newArgs)));
                        }
                    }
                    return SymExpr(node);
                };

                try {
                    SymExpr subbed = applyWeierstrass(varPart.ptr) * dt_sub;
                    auto [num_t, den_t] = getFraction(subbed);
                    
                    SymExpr gcd_t = polyGCD(num_t, den_t, t_var);
                    if (!gcd_t.isOne() && !gcd_t.isZero()) {
                        num_t = polyDiv(num_t, gcd_t, t_var).first;
                        den_t = polyDiv(den_t, gcd_t, t_var).first;
                    }
                    
                    SymExpr rational_t = simplifyCore(expand_core(num_t / den_t, SymConfig::maxExpandTerms));
                    SymExpr rational_var = subs(rational_t, t_var, SymExpr::makeVar(var));
                    if (auto opt_integrated_var = doInteg(rational_var, current_depth + 1)) {
                        if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Weierstrass Substitution Success" << std::endl;
                        SymExpr back_sub = SymExpr(std::make_shared<SymFunc>("tan", std::vector<std::shared_ptr<SymNode>>{(SymExpr::makeVar(var) / SymExpr(BigInt(2))).ptr}));
                        return coeff * simplifyCore(subs(*opt_integrated_var, var, back_sub));
                    }
                } catch (const EngineInterruptError&) {
                    throw;
                } catch (...) {}
            }
            return std::nullopt;
            }});

            // --- 4. 启发式分部积分 (Integration by Parts) ---
            strats.push_back({"Integration by Parts", 300, [&]() -> std::optional<SymExpr> {
            bool tryParts = false;
            std::vector<SymExpr> factors;
            if (varPart.ptr->getType() == SymType::MUL) {
                auto mul = std::static_pointer_cast<SymMul>(varPart.ptr);
                for (auto& arg : mul->args) factors.push_back(SymExpr(arg));
                tryParts = true;
            } else if (varPart.ptr->getType() == SymType::FUNC || varPart.ptr->getType() == SymType::POW) {
                bool isGoodForIBP = false;
                if (varPart.ptr->getType() == SymType::FUNC) {
                    auto fn = std::static_pointer_cast<SymFunc>(varPart.ptr);
                    if (fn->name == "log" || fn->name == "asin" || fn->name == "acos" || fn->name == "atan" || fn->name == "asinh" || fn->name == "acosh" || fn->name == "atanh") isGoodForIBP = true;
                } else if (varPart.ptr->getType() == SymType::POW) {
                    auto p = std::static_pointer_cast<SymPow>(varPart.ptr);
                    if (p->base->getType() == SymType::FUNC) {
                        auto fn = std::static_pointer_cast<SymFunc>(p->base);
                        if (fn->name == "log" || fn->name == "asin" || fn->name == "acos" || fn->name == "atan" || fn->name == "asinh" || fn->name == "acosh" || fn->name == "atanh") isGoodForIBP = true;
                    }
                }
                if (isGoodForIBP) {
                    factors.push_back(varPart);
                    factors.push_back(SymExpr(BigInt(1)));
                    tryParts = true;
                }
            }

            bool hasTranscendental = false;
            std::function<void(const std::shared_ptr<SymNode>&)> checkTrans = [&](const std::shared_ptr<SymNode>& node) {
                if (!node || hasTranscendental) return;
                if (node->getType() == SymType::FUNC) {
                    auto func = std::static_pointer_cast<SymFunc>(node);
                    if (func->name != "sqrt" && func->name != "cbrt" && func->name != "root" && func->name != "RootOf" && func->name != "RootSum") {
                        hasTranscendental = true;
                        return;
                    }
                }
                if (node->getType() == SymType::ADD) {
                    for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) checkTrans(arg);
                } else if (node->getType() == SymType::MUL) {
                    for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) checkTrans(arg);
                } else if (node->getType() == SymType::POW) {
                    auto powNode = std::static_pointer_cast<SymPow>(node);
                    checkTrans(powNode->base);
                    checkTrans(powNode->exp);
                } else if (node->getType() == SymType::FUNC) {
                    for (auto& arg : std::static_pointer_cast<SymFunc>(node)->args) checkTrans(arg);
                }
            };
            checkTrans(varPart.ptr);
            
            if (tryParts && !hasTranscendental) {
                tryParts = false;
            }

            if (tryParts) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Integration by Parts" << std::endl;
                auto getPriority = [&](const SymExpr& f) -> int {
                    if (f.ptr->getType() == SymType::FUNC) {
                        auto fn = std::static_pointer_cast<SymFunc>(f.ptr);
                        if (fn->name == "log" || fn->name == "asin" || fn->name == "acos" || fn->name == "atan" || fn->name == "asinh" || fn->name == "acosh" || fn->name == "atanh") return 1;
                    }
                    if (f.ptr->getType() == SymType::POW) {
                        auto p = std::static_pointer_cast<SymPow>(f.ptr);
                        if (p->base->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(p->base)->name == var) {
                            if (p->exp->getType() == SymType::NUM) {
                                auto [isInt, n] = extractExactInt(std::static_pointer_cast<SymNum>(p->exp)->value);
                                if (isInt && n > 0) return 2;
                            }
                        }
                    }
                    if (f.ptr->getType() == SymType::VAR && std::static_pointer_cast<SymVar>(f.ptr)->name == var) return 2;
                    return 3;
                };

                std::vector<size_t> indices(factors.size());
                std::iota(indices.begin(), indices.end(), 0);
                std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                    return getPriority(factors[a]) < getPriority(factors[b]);
                });

                auto getIbpCandidates = [&](const std::vector<SymExpr>& facs, const std::vector<size_t>& idxs) {
                    std::vector<std::pair<SymExpr, SymExpr>> cands;
                    auto addCand = [&](SymExpr u, SymExpr dv) {
                        for (const auto& c : cands) if (c.first == u && c.second == dv) return;
                        cands.push_back({u, dv});
                    };
                    // 1. 选取优先级最高（最适合求导）的 1~2 个因子作为 u
                    for (size_t k = 0; k < std::min<size_t>(2, idxs.size()); ++k) {
                        size_t i = idxs[k];
                        SymExpr u = facs[i];
                        SymExpr dv(BigInt(1));
                        for (size_t j = 0; j < facs.size(); ++j) if (j != i) dv = dv * facs[j];
                        addCand(u, dv);
                    }
                    // 2. 选取优先级最低（最适合积分）的 1~2 个因子作为 dv
                    for (size_t k = 0; k < std::min<size_t>(2, idxs.size()); ++k) {
                        size_t i = idxs[idxs.size() - 1 - k];
                        SymExpr dv = facs[i];
                        SymExpr u(BigInt(1));
                        for (size_t j = 0; j < facs.size(); ++j) if (j != i) u = u * facs[j];
                        addCand(u, dv);
                    }
                    return cands;
                };

                auto tryPartsWith = [&](SymExpr u, SymExpr dv) -> std::optional<SymExpr> {
                    SymExpr du = simplifyCore(diff(u, var));
                    if (du.isZero()) {
                        return std::nullopt; // 剪枝：常数求导为0，分部积分无意义
                    }

                    // 导数膨胀率探测 (Derivative Swell Metric)
                    if (getAstNodeCount(du) >= 2.5 * getAstNodeCount(u)) {
                        return std::nullopt;
                    }

                    auto opt_v = doInteg(dv, current_depth + 1);
                    if (!opt_v) return std::nullopt;
                    SymExpr v = *opt_v;

                    // 剪枝：如果积分出来的 v 极其复杂（体积膨胀过大），及时止损
                    if (getAstNodeCount(v) > getAstNodeCount(dv) * 4 + 10) {
                        return std::nullopt;
                    }

                    SymExpr v_du = simplifyCore(v * du);
                    
                    // 循环分部积分检测 (1-step cycle)
                    SymExpr ratio1 = simplifyCore(v_du / varPart);
                    if (!containsVar(ratio1.ptr, var) && !ratio1.isZero()) {
                        SymExpr k = ratio1;
                        SymExpr one_plus_k = simplifyCore(SymExpr(BigInt(1)) + k);
                        if (!one_plus_k.isZero()) {
                            return coeff * simplify(expand_core((u * v) / one_plus_k, SymConfig::maxExpandTerms));
                        }
                    }
                    
                    // 循环分部积分检测 (2-step cycle)
                    if (v_du.ptr->getType() == SymType::MUL) {
                        auto mul2 = std::static_pointer_cast<SymMul>(v_du.ptr);
                        std::vector<SymExpr> factors2;
                        for (auto& arg : mul2->args) factors2.push_back(SymExpr(arg));
                        
                        std::vector<size_t> indices2(factors2.size());
                        std::iota(indices2.begin(), indices2.end(), 0);
                        std::sort(indices2.begin(), indices2.end(), [&](size_t a, size_t b) {
                            return getPriority(factors2[a]) < getPriority(factors2[b]);
                        });
                        
                        auto candidates2 = getIbpCandidates(factors2, indices2);
                        int ibp2_attempts = 0;
                        for (const auto& cand2 : candidates2) {
                            if (current_depth > SymConfig::maxDepth / 2 && ibp2_attempts > 0) {
                                break;
                            }
                            ibp2_attempts++;
                            SymExpr u2_inner = cand2.first;
                            SymExpr dv2_inner = cand2.second;
                            SymExpr du2_inner = simplifyCore(diff(u2_inner, var));
                            if (du2_inner.isZero()) continue;

                            // 导数膨胀率探测 (Derivative Swell Metric)
                            if (getAstNodeCount(du2_inner) >= 2.5 * getAstNodeCount(u2_inner)) continue;

                            auto opt_v2_inner = doInteg(dv2_inner, current_depth + 2);
                            if (opt_v2_inner) {
                                SymExpr v2_inner = *opt_v2_inner;
                                SymExpr v2_du2 = simplifyCore(v2_inner * du2_inner);
                                
                                SymExpr ratio2 = simplifyCore(v2_du2 / varPart);
                                if (!containsVar(ratio2.ptr, var) && !ratio2.isZero()) {
                                    SymExpr k = ratio2;
                                    SymExpr one_minus_k = simplifyCore(SymExpr(BigInt(1)) - k);
                                    if (!one_minus_k.isZero()) {
                                        return coeff * simplifyCore((u * v - u2_inner * v2_inner) / one_minus_k);
                                    }
                                }
                            }
                        }
                    }

                    auto opt_int_v_du = doInteg(v_du, current_depth + 1);
                    if (!opt_int_v_du) return std::nullopt;
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Integration by Parts Success" << std::endl;
                    return coeff * simplifyCore(u * v - *opt_int_v_du);
                };

                auto candidates = getIbpCandidates(factors, indices);
                int ibp_attempts = 0;
                for (const auto& cand : candidates) {
                    if (current_depth > SymConfig::maxDepth / 2 && ibp_attempts > 0) {
                        break;
                    }
                    ibp_attempts++;
                    if (auto res = tryPartsWith(cand.first, cand.second)) return *res;
                }
            }
            return std::nullopt;
            }});

            // --- 5. Risch 算法 (Risch Algorithm) ---
            int risch_score = 100; // 基础兜底分数
            bool allow_deeper_risch = false;
            if (current_depth <= start_depth + 3) {
                if ((has_exp || has_log) && !has_radical) {
                    allow_deeper_risch = true;
                    risch_score = 880; // 优先处理纯指数/对数
                }
            }
            
            if (current_depth > start_depth + 1 && !allow_deeper_risch) {
                risch_score = 0; // 剪枝
            }

            std::optional<std::string> risch_error;

            strats.push_back({"Risch Algorithm", risch_score, [&]() -> std::optional<SymExpr> {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Trying Risch Algorithm" << std::endl;
                try {
                    auto rischRes = coeff * rischIntegrate(varPart, var, current_depth + 1);
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Risch Algorithm Success" << std::endl;
                    return rischRes;
                } catch (const EngineInterruptError&) {
                    throw;
                } catch (const std::runtime_error& e) {
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Risch Algorithm Failed" << std::endl;
                    risch_error = e.what();
                    return std::nullopt;
                } catch (...) {
                    if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Risch Algorithm Failed" << std::endl;
                    risch_error = "Unknown Risch Error";
                    return std::nullopt;
                }
            }});

            // --- 执行积分路由 (Execute Integration Routing) ---
            std::sort(strats.begin(), strats.end(), [](const auto& a, const auto& b) {
                return a.score > b.score;
            });

            for (auto& strat : strats) {
                if (strat.score <= 0) continue;
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "-> Routing to: " << strat.name << " (Score: " << strat.score << ")" << std::endl;
                if (auto res = strat.execute()) {
                    return res;
                }
            }

            if (current_depth == start_depth && risch_error) {
                throw std::runtime_error(*risch_error);
            }

            return std::nullopt;
        };

        doInteg = [&](const SymExpr& e, int current_depth) -> std::optional<SymExpr> {
            if (!e.ptr) return std::nullopt;
            std::string sig = e.ptr->getSignature();
            auto it = integCache.find(sig);
            if (it != integCache.end()) return it->second;
            
            if (!computing.insert(sig).second) {
                if (SymConfig::debugIntegration) std::cout << std::string(current_depth * 2, ' ') << "<- Cycle detected" << std::endl;
                return std::nullopt;
            }
            
            struct ComputeGuard {
                std::unordered_set<std::string>& comp;
                std::string s;
                ~ComputeGuard() { comp.erase(s); }
            } guard{computing, sig};
            
            auto res = doIntegImpl(e, current_depth);
            integCache[sig] = res;
            return res;
        };

        auto tryInteg = [&]() -> SymExpr {
            try {
                if (auto res = doInteg(expr, start_depth)) {
                    return simplify(*res);
                }
                throw std::runtime_error("Calculus Error: Function integration not supported or complex power.");
            } catch (const EngineInterruptError&) {
                throw;
            } catch (const std::runtime_error& e) {
                std::string msg = e.what();
                SymExpr expanded;
                try { expanded = expand_core(expr, SymConfig::maxExpandTerms * 2); } catch (const EngineInterruptError&) { throw; } catch (...) { expanded = expr; }
                
                // 严格防死锁：只有当展开后的表达式确实发生了变化，才允许重置 depth 再次尝试
                if (expanded != expr) {
                    if (SymConfig::debugIntegration) std::cout << "-> Retrying integration after expansion: " << expanded.toString() << std::endl;
                    try {
                        if (auto res2 = doInteg(expanded, start_depth)) {
                            return simplify(*res2);
                        }
                    } catch (const EngineInterruptError&) {
                        throw;
                    } catch (const std::runtime_error& e2) {
                        std::string msg2 = e2.what();
                        // 如果展开后再次失败，保留新的错误信息
                        if (msg2.find("Risch Field") != std::string::npos || 
                            msg2.find("Hermite Reduction") != std::string::npos ||
                            msg2.find("Integration depth limit exceeded") != std::string::npos ||
                            msg2.find("Calculus Error:") == 0) {
                            msg = msg2;
                        }
                    } catch (...) {}
                }
                
                // 如果是 Risch 算法抛出的进度信息，直接向外传递
                if (msg.find("Risch Field") != std::string::npos || 
                    msg.find("Hermite Reduction") != std::string::npos || 
                    msg.find("Integration depth limit exceeded") != std::string::npos) {
                    throw std::runtime_error(msg);
                }
                
                if (msg.find("Calculus Error:") == 0) {
                    throw std::runtime_error(msg);
                }
                
                throw std::runtime_error("Calculus Error: Function integration not supported or complex power. (" + msg + ")");
            } catch (...) {
                throw std::runtime_error("Calculus Error: Function integration not supported or complex power.");
            }
        };

        return tryInteg();
    }

    // =================================================================
    // 🚀 定积分 (Definite Integral) - 带有多值函数与支割线安全检查
    // =================================================================
    static bool hasMultiValuedFuncs(const SymExpr& expr) {
        if (!expr.ptr) return false;
        bool found = false;
        std::function<void(const std::shared_ptr<SymNode>&)> check = [&](const std::shared_ptr<SymNode>& node) {
            if (!node || found) return;
            if (node->getType() == SymType::FUNC) {
                auto func = std::static_pointer_cast<SymFunc>(node);
                if (func->name == "RootOf" || func->name == "RootSum") {
                    found = true;
                    return;
                }
            } else if (node->getType() == SymType::VAR) {
                auto varName = std::static_pointer_cast<SymVar>(node)->name;
                if (varName == "i" || varName == "I") {
                    found = true;
                    return;
                }
            }
            
            if (node->getType() == SymType::ADD) {
                for (auto& arg : std::static_pointer_cast<SymAdd>(node)->args) check(arg);
            } else if (node->getType() == SymType::MUL) {
                for (auto& arg : std::static_pointer_cast<SymMul>(node)->args) check(arg);
            } else if (node->getType() == SymType::POW) {
                auto powNode = std::static_pointer_cast<SymPow>(node);
                check(powNode->base);
                check(powNode->exp);
            } else if (node->getType() == SymType::FUNC) {
                for (auto& arg : std::static_pointer_cast<SymFunc>(node)->args) check(arg);
            }
        };
        check(expr.ptr);
        return found;
    }

    SymExpr defint(const SymExpr& expr, const std::string& var, const SymExpr& a, const SymExpr& b) {
        SymExpr antideriv = integrate(expr, var);
        if (hasMultiValuedFuncs(antideriv)) {
            throw std::runtime_error(
                "Calculus Error: The Newton-Leibniz formula requires the antiderivative to be continuous on the interval. "
                "The generated antiderivative contains multi-valued functions (like RootSum or complex logarithms) "
                "which introduce branch cuts and phase jumps. Direct substitution of limits is mathematically invalid here. "
                "Please use numerical integration instead (e.g., pass a lambda function to integ)."
            );
        }
        return simplify(subs(antideriv, var, b) - subs(antideriv, var, a));
    }

} // namespace jc
