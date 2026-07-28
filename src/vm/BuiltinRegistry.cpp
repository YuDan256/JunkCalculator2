#include "BuiltinRegistry.h"
#include "../cas/SymEval.h"
#include "../cas/Integration.h"
#include "../cas/Factorization.h"
#include "../frontend/Highlight.h"          // ★ highlightCode(), colorsEnabled
#include "../frontend/Utf8.h"
#include "../frontend/Lexer.h"
#include "../frontend/Parser.h"
#include "../compiler/IRBuilder.h"
#include "../compiler/IROptimizer.h"
#include "../compiler/RegisterAllocator.h"
#include "../compiler/Emitter.h"
#include "../vm/VM.h"
#include "../memory/GcHeap.h"
#include "HelpRouter.h"         // ★ HelpRouter, DynamicHelp
#include "PredefinedClasses.h"
#include "../frontend/ASTConverter.h"
#ifdef _MSC_VER
#pragma warning(disable: 4702)
#endif
#include <algorithm>
#include <cctype>
#include <chrono>               // ★ clock() — high_resolution_clock
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>              // ★ std::gcd (如果用到)
#include <random>
#include <sstream>
#include <thread>               // ★ sleep() — std::this_thread::sleep_for
#include <filesystem>           // ★ Phase 2: file I/O
#include <fstream>              // ★ Phase 2: file I/O
#include <cstdlib>              // ★ std::system

namespace jc {
    // 替代 Evaluator 中的路径状态
    static std::string g_workspacePath = "";
    // 获取当前路径
    static std::string g_cwd() {
        if (!helpers::g_scriptDirStack.empty()) return helpers::g_scriptDirStack.back();
        return std::filesystem::current_path().string();
    }

    // =================================================================
    // 跨模块 Dunder 调用桥梁
    // =================================================================
    static const char* DUNDER_HASH = "__hash__";
    static const char* DUNDER_ABS = "__abs__";
    static const char* DUNDER_STR = "__str__";
    static const char* DUNDER_BOOL = "__bool__";
    static const char* DUNDER_LEN = "__len__";
    static const char* DUNDER_ADD = "__add__";
    static const char* DUNDER_SUB = "__sub__";
    static const char* DUNDER_MUL = "__mul__";
    static const char* DUNDER_DIV = "__div__";
    static const char* DUNDER_LDIV = "__ldiv__";
    static const char* DUNDER_ITER = "__iter__";
    static const char* DUNDER_NEXT = "__next__";
    static const char* DUNDER_CALL = "__call__";
    static const char* DUNDER_GETITEM = "__getitem__";

    std::pair<bool, Value> invokeDunder(ObjInstance* inst, const char* methodName, const std::vector<Value>& args) {
        ObjClosure* method = nullptr;
        ObjClass* c = inst->classDef;
        std::string sname(methodName);
        while (c) {
            auto it = c->methods.find(sname);
            if (it != c->methods.end()) {
                method = it->second;
                break;
            }
            c = c->parent;
        }
        if (!method) return {false, Value::none()};

        if (method->isNative() && !method->isBytecode()) {
            helpers::nativeSelfStack.push_back(Value(inst));
            helpers::nativeClassStack.push_back(Value(inst->classDef));
            Value result;
            try {
                auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
                result = fn(args);
            }
            catch (...) {
                helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
                throw;
            }
            helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
            return {true, result};
        }
        else if (method->isBytecode()) {
            return {true, VM::activeVM->callVMFunction(method->compiledFnIndex, args, method, Value(inst), Value(inst->classDef))};
        }
        return {false, Value::none()};
    }

    // =================================================================
    // 容器元素键生成器（保证内容相同的 Set/Dict 无论插入顺序如何，都能生成相同的键用于去重）
    // =================================================================
    std::string setValueKey(const Value& v) {
        static thread_local std::vector<const void*> visited;
        std::ostringstream oss;
        oss << (v.isObj() ? static_cast<int>(v.asObj()->type) : (v.isNumber() ? -1 : -2)) << ":";

        if (v.isNone()) {
            oss << "none";
        }
        else if (v.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(v.asObj());
            RecursionGuard guard(visited, l);
            if (guard.isCycle) { oss << "CYCLE"; return oss.str(); }
            oss << "[";
            for (const auto& e : l->vec) {
                oss << setValueKey(e) << ",";
            }
            oss << "]";
        }
        else if (v.isObjType(ObjType::DICT)) {
            auto d = static_cast<ObjDict*>(v.asObj());
            RecursionGuard guard(visited, d);
            if (guard.isCycle) { oss << "CYCLE"; return oss.str(); }
            std::vector<std::string> pairs;
            for (const auto& [k, val] : d->elements) {
                pairs.push_back(setValueKey(k) + ":" + setValueKey(val));
            }
            std::sort(pairs.begin(), pairs.end());
            oss << "{";
            for (const auto& p : pairs) oss << p << ",";
            oss << "}";
        }
        else if (v.isObjType(ObjType::SET)) {
            auto s = static_cast<ObjSet*>(v.asObj());
            RecursionGuard guard(visited, s);
            if (guard.isCycle) { oss << "CYCLE"; return oss.str(); }
            std::vector<std::string> elems;
            for (const auto& val : s->elements) {
                elems.push_back(setValueKey(val));
            }
            std::sort(elems.begin(), elems.end());
            oss << "Set{";
            for (const auto& e : elems) oss << e << ",";
            oss << "}";
        }
        else if (v.isInstance()) {
            auto inst = v.asInstance();
            auto [found, res] = invokeDunder(inst, DUNDER_HASH, {});
            if (found) {
                GcValueGuard resGuard(res);
                oss << res.toString();
            } else {
                oss << inst;
            }
        }
        else if (v.isSymbolic()) {
            auto sym = static_cast<ObjSym*>(v.asObj());
            if (sym->sym.ptr) oss << sym->sym.ptr->getSignature();
            else oss << "null";
        }
        else if (v.isObjType(ObjType::NAMESPACE)) {
            oss << static_cast<ObjNamespace*>(v.asObj())->name;
        }
        else if (v.isFunctionClosure()) {
            auto cl = v.asFunction();
            if (cl->compiledFnIndex >= 0) oss << "fn:" << cl->compiledFnIndex;
            else oss << "native:" << cl->rawBody;
            oss << "|self:" << setValueKey(cl->boundSelf);
            oss << "|cls:" << setValueKey(cl->boundClass);
        }
        else {
            oss << v;
        }
        return oss.str();
    }

    // =================================================================
// 符号函数坍缩器：递归求值所有参数已为纯数字的 SymFunc 节点
// sin(0) → 0,  cos(PI) → -1,  etc.
// =================================================================
    static Value casValToValue(const CASVal& v) {
        return std::visit([](auto&& arg) -> Value { return Value(arg); }, v);
    }

    static bool isConstantExpr(const SymExpr& expr) {
        if (!expr.ptr) return true;
        switch (expr.ptr->getType()) {
            case SymType::NUM: return true;
            case SymType::VAR: {
                auto name = std::static_pointer_cast<SymVar>(expr.ptr)->name;
                return name == "PI" || name == "E" || name == "i" || name == "I";
            }
            case SymType::ADD:
                for (auto& arg : std::static_pointer_cast<SymAdd>(expr.ptr)->args)
                    if (!isConstantExpr(SymExpr(arg))) return false;
                return true;
            case SymType::MUL:
                for (auto& arg : std::static_pointer_cast<SymMul>(expr.ptr)->args)
                    if (!isConstantExpr(SymExpr(arg))) return false;
                return true;
            case SymType::POW: {
                auto p = std::static_pointer_cast<SymPow>(expr.ptr);
                return isConstantExpr(SymExpr(p->base)) && isConstantExpr(SymExpr(p->exp));
            }
            case SymType::FUNC:
                for (auto& arg : std::static_pointer_cast<SymFunc>(expr.ptr)->args)
                    if (!isConstantExpr(SymExpr(arg))) return false;
                return true;
        }
        return false;
    }

    template<typename MapType, typename ArityMapType>
    static SymExpr collapseSymFuncs(const SymExpr& expr, const MapType& fns, const ArityMapType& arities) {
        jc::checkInterrupt();
        if (!expr.ptr) return expr;

        switch (expr.ptr->getType()) {
        case SymType::NUM:
        case SymType::VAR:
            return expr;

        case SymType::ADD: {
            auto add = std::static_pointer_cast<SymAdd>(expr.ptr);
            SymExpr result(BigInt(0));
            for (auto& arg : add->args)
                result = result + collapseSymFuncs(SymExpr(arg), fns, arities);
            return result;
        }

        case SymType::MUL: {
            auto mul = std::static_pointer_cast<SymMul>(expr.ptr);
            SymExpr result(BigInt(1));
            for (auto& arg : mul->args)
                result = result * collapseSymFuncs(SymExpr(arg), fns, arities);
            return result;
        }

        case SymType::POW: {
            auto pow = std::static_pointer_cast<SymPow>(expr.ptr);
            return collapseSymFuncs(SymExpr(pow->base), fns, arities) ^
                collapseSymFuncs(SymExpr(pow->exp), fns, arities);
        }

        case SymType::FUNC: {
            auto func = std::static_pointer_cast<SymFunc>(expr.ptr);
            std::vector<SymExpr> newArgs;
            std::vector<Value> vals;
            bool allNumeric = true;
            for (auto& arg : func->args) {
                SymExpr collapsed = collapseSymFuncs(SymExpr(arg), fns, arities);
                newArgs.push_back(collapsed);
                
                if (isConstantExpr(collapsed)) {
                    try {
                        std::map<std::string, Value> emptyEnv;
                        SymbolicFuncResolver resolver = [&fns, &arities](const std::string& name, const std::vector<Value>& fnArgs) -> Value {
                            auto it = fns.find(name);
                            if (it != fns.end()) {
                                auto ait = arities.find(name);
                                if (ait != arities.end() && !ait->second.empty()) {
                                    if (ait->second.find(static_cast<int>(fnArgs.size())) == ait->second.end()) {
                                        throw std::runtime_error("Runtime Error: Function '" + name + "' expects wrong number of arguments.");
                                    }
                                }
                                return it->second(fnArgs);
                            }
                            throw std::runtime_error("Function not found");
                        };
                        vals.push_back(evalUniversal(collapsed.ptr, emptyEnv, resolver));
                    } catch (const jc::EngineInterruptError&) {
                        throw;
                    } catch (...) {
                        allNumeric = false;
                    }
                } else {
                    allNumeric = false;
                }
            }

            // 所有参数都是纯数字 → 尝试调用实际函数求值
            if (allNumeric) {
                auto it = fns.find(func->name);
                if (it != fns.end()) {
                    try {
                        Value result = it->second(vals);
                        return result.asSymbolic();
                    }
                    catch (const std::runtime_error& e) {
                        std::string msg = e.what();
                        // 数学错误：传播给用户
                        if (msg.find("Math Error") != std::string::npos)
                            throw;
                        if (msg.find("CAS Error") != std::string::npos)
                            throw;
                        // 类型不兼容：保留符号形式
                    }
                }
            }

            // 无法求值，保留为符号函数节点
            std::vector<std::shared_ptr<SymNode>> ptrs;
            for (auto& a : newArgs) ptrs.push_back(a.ptr);
            return SymExpr(std::make_shared<SymFunc>(func->name, std::move(ptrs)));
        }
        }
        return expr;
    }

using namespace helpers;

void BuiltinRegistry::registerAll() {
    registerMath();
    registerComplex();
    registerFraction();
    registerPolySolver();
    registerMatrixOps();
    registerDecompositions();
    registerLinearSolvers();
    registerVectors();
    registerNumberTheory();
    registerBase();
    registerStatistics();
    registerRandom();
    registerSystemUtils();
    registerControlFlow();
    registerStringFunctions();
    registerArrayFunctions();
    registerStringMatrix();
    registerDictFunctions();
    registerListConversion();
    registerIntrospection();
    registerFormatType();
    registerHigherOrder();
    registerCalculus();        // ★ Phase 2
    registerCAS();             // ★ CAS
    registerFileIO();          // ★ Phase 2
    registerErrorHandling();   // ★ Phase 2
    registerSystemShell();
    registerTypeChecks();
    registerSetFunctions();
    registerPredefinedClasses();
}

// =================================================================
// [1] 基础数学函数
// =================================================================
void BuiltinRegistry::registerMath() {

    auto regMath = [&](const std::string& name, std::set<int> arities, NativeCallable fn) {
        reg(name, std::move(arities), [name, fn](const std::vector<Value>& args) -> Value {
            // 扫描：是否有任何参数是符号表达式？
            bool hasSymbolic = false;
            for (const auto& a : args) {
                if (a.isSymbolic()) { hasSymbolic = true; break; }
            }
            // 如果有，将所有参数统一提升为 SymExpr，打包成 SymFunc 节点
            if (hasSymbolic) {
                std::vector<std::shared_ptr<SymNode>> symArgs;
                symArgs.reserve(args.size());
                for (const auto& a : args) {
                    symArgs.push_back(a.asSymbolic().ptr);
                }
                // 在构建 AST 时直接将根式转换为分数幂，统一底层数学表达
                if (name == "sqrt" && symArgs.size() == 1) {
                    return Value(SymExpr(symArgs[0]) ^ SymExpr(Fraction(1, 2)));
                }
                if (name == "sqrtD" && symArgs.size() == 1) {
                    return Value(SymExpr(symArgs[0]) ^ SymExpr(0.5));
                }
                if (name == "cbrt" && symArgs.size() == 1) {
                    return Value(SymExpr(symArgs[0]) ^ SymExpr(Fraction(1, 3)));
                }
                if (name == "cbrtD" && symArgs.size() == 1) {
                    return Value(SymExpr(symArgs[0]) ^ SymExpr(1.0 / 3.0));
                }
                if (name == "root" && symArgs.size() == 2) {
                    return Value(SymExpr(symArgs[0]) ^ (SymExpr(BigInt(1)) / SymExpr(symArgs[1])));
                }
                if (name == "rootD" && symArgs.size() == 2) {
                    return Value(SymExpr(symArgs[0]) ^ (SymExpr(1.0) / SymExpr(symArgs[1])));
                }
                return Value(SymExpr(std::make_shared<SymFunc>(name, std::move(symArgs))));
            }
            // 否则正常执行数值计算
            return fn(args);
            });
        };

    // 我们在此插入您要求的常量工厂和泛类型构造：
    reg("pi", { 0 }, [](const std::vector<Value>&) -> Value { return Value(3.14159265358979323846); });
    reg("e", { 0 }, [](const std::vector<Value>&) -> Value { return Value(2.71828182845904523536); });
    reg("i", { 0 }, [](const std::vector<Value>&) -> Value { return Value(Complex(0.0, 1.0)); });
    reg("complex", { 1, 2 }, [this](const std::vector<Value>& args) -> Value {
        auto evalIfSym = [this](Value v) {
            if (v.isSymbolic()) {
                auto it = builtins.find("evalf");
                if (it != builtins.end()) return it->second({v});
            }
            return v;
        };
        if (args.size() == 1) {
            Value val = evalIfSym(args[0]);
            // complex(x) → Complex(x, 0) 或保留已有复数
            if (val.isComplex())
                return val;
            return Value(Complex(val.asDouble(), 0.0));
        }
        // complex(a, b) → Complex(a, b)
        return Value(Complex(evalIfSym(args[0]).asDouble(), evalIfSym(args[1]).asDouble()));
        });

    reg("double", { 1 }, [this](const std::vector<Value>& args) -> Value {
        Value val = args[0];
        if (val.isSymbolic()) {
            auto it = builtins.find("evalf");
            if (it != builtins.end()) val = it->second({val});
        }
        // 任意数值类型 → double（复数仅虚部为 0 时允许）
        if (val.isComplex()) {
            const auto& c = val.asComplex();
            if (!Tol::isEq(c.imag, 0.0))
                throw std::runtime_error("Type Error: Cannot convert complex with nonzero imaginary part to double.");
            return Value(c.real);
        }
        return Value(val.asDouble());
        });

    reg("int", { 1 }, [this](const std::vector<Value>& args) -> Value {
        Value val = args[0];
        if (val.isSymbolic()) {
            auto it = builtins.find("evalf");
            if (it != builtins.end()) val = it->second({val});
        }
        // 截断取整（向零方向）
        if (val.isObjType(ObjType::BIGINT) || val.isInt32())
            return val;
        if (val.isObjType(ObjType::FRACTION)) {
            const auto& f = static_cast<ObjFraction*>(val.asObj())->frac;
            return Value(f.getNum() / f.getDen());  // BigInt 除法自动截断
        }
        if (val.isComplex()) {
            const auto& c = val.asComplex();
            if (!Tol::isEq(c.imag, 0.0))
                throw std::runtime_error("Type Error: Cannot convert complex with nonzero imaginary part to int.");
            return Value(BigInt(static_cast<int64_t>(std::trunc(c.real))));
        }
        if (val.isDouble()) {
            double v = val.asDoubleRaw();
            if (!std::isfinite(v))
                throw std::runtime_error("Type Error: Cannot convert non-finite value to int.");
            return Value(BigInt(static_cast<int64_t>(std::trunc(v))));
        }
        if (val.isString()) {
            std::string s = val.asString();
            size_t start = s.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) {
                size_t end = s.find_last_not_of(" \t\r\n");
                std::string trimmed = s.substr(start, end - start + 1);
                int radix = 10;
                bool neg = false;
                size_t p = 0;
                if (trimmed[0] == '-' || trimmed[0] == '+') {
                    neg = (trimmed[0] == '-');
                    p = 1;
                }
                if (trimmed.size() > p + 1 && trimmed[p] == '0') {
                    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(trimmed[p + 1])));
                    if (c == 'x') { radix = 16; p += 2; }
                    else if (c == 'b') { radix = 2; p += 2; }
                    else if (c == 'o') { radix = 8; p += 2; }
                }
                try {
                    if (radix != 10) {
                        std::string numPart = trimmed.substr(p);
                        if (numPart.empty()) throw std::runtime_error("empty");
                        BigInt res = BaseNum::fromString(numPart, radix).getValue();
                        return Value(neg ? -res : res);
                    }
                    return Value(BigInt(trimmed));
                } catch (...) {}
            }
            throw std::runtime_error("Type Error: Cannot parse '" + val.asString() + "' as integer.");
        }
        return Value(val.asBigInt());
        });

    reg("matrix", {}, [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("Runtime Error: matrix(rows, cols [, ...]) expects at least 2 args.");
        int r = static_cast<int>(std::round(args[0].asDouble()));
        int c = static_cast<int>(std::round(args[1].asDouble()));
        if (r <= 0 || c <= 0)
            throw std::runtime_error("Runtime Error: matrix() dimensions must be positive.");

        // 无元素 → 零矩阵
        if (static_cast<int>(args.size()) == 2)
            return Value(RealMatrix(r, c));

        int total = r * c;
        if (static_cast<int>(args.size()) - 2 != total)
            throw std::runtime_error("Runtime Error: matrix() element count mismatch: "
                "expected " + std::to_string(total) + ", got " +
                std::to_string(args.size() - 2) + ".");

        // 类型检测
        bool hasString = false, hasComplex = false;
        for (int i = 2; i < static_cast<int>(args.size()); ++i) {
            if (args[i].isString())
                hasString = true;
            else if (args[i].isComplex())
                hasComplex = true;
        }

        if (hasString) {
            // StringMatrix: 所有元素转字符串
            std::vector<std::string> flat;
            flat.reserve(total);
            for (int i = 0; i < total; ++i) {
                const auto& v = args[i + 2];
                if (v.isString())
                    flat.push_back(v.asString());
                else {
                    std::ostringstream oss; oss << v;
                    flat.push_back(oss.str());
                }
            }
            return Value(StringMatrix(r, c, flat));
        }

        if (hasComplex) {
            // ComplexMatrix
            std::vector<Complex> flat;
            flat.reserve(total);
            for (int i = 0; i < total; ++i)
                flat.push_back(args[i + 2].asComplex());
            return Value(ComplexMatrix(r, c, flat));
        }

        // RealMatrix
        std::vector<double> flat;
        flat.reserve(total);
        for (int i = 0; i < total; ++i)
            flat.push_back(args[i + 2].asDouble());
        return Value(RealMatrix(r, c, flat));
        });

    regMath("sin", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matSin());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matSin());
        if (args[0].isComplex()) return Value(sin(args[0].asComplex()));
        return Value(std::sin(args[0].asDouble()));
    });
    regMath("cos", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matCos());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matCos());
        if (args[0].isComplex()) return Value(cos(args[0].asComplex()));
        return Value(std::cos(args[0].asDouble()));
    });
    regMath("tan", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matTan());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matTan());
        if (args[0].isComplex()) return Value(tan(args[0].asComplex()));
        return Value(std::tan(args[0].asDouble()));
    });
    regMath("exp", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matExp());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matExp());
        if (args[0].isComplex()) return Value(exp(args[0].asComplex()));
        return Value(std::exp(args[0].asDouble()));
    });
    regMath("sinh", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matSinh());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matSinh());
        if (args[0].isComplex()) return Value(sinh(args[0].asComplex()));
        return Value(std::sinh(args[0].asDouble()));
    });
    regMath("cosh", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matCosh());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matCosh());
        if (args[0].isComplex()) return Value(cosh(args[0].asComplex()));
        return Value(std::cosh(args[0].asDouble()));
    });
    regMath("tanh", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matTanh());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matTanh());
        if (args[0].isComplex()) return Value(tanh(args[0].asComplex()));
        return Value(std::tanh(args[0].asDouble()));
    });
    regMath("cot", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matTan();
            std::vector<double> flat = m.rawData();
            for(auto& v : flat) v = 1.0 / v;
            return Value(RealMatrix(m.getRows(), m.getCols(), flat));
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matTan();
            std::vector<Complex> flat = m.rawData();
            for(auto& v : flat) v = Complex(1.0, 0.0) / v;
            return Value(ComplexMatrix(m.getRows(), m.getCols(), flat));
        }
        if (args[0].isComplex()) return Value(Complex(1.0, 0.0) / tan(args[0].asComplex()));
        return Value(1.0 / std::tan(args[0].asDouble()));
    });
    regMath("sec", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matCos();
            std::vector<double> flat = m.rawData();
            for(auto& v : flat) v = 1.0 / v;
            return Value(RealMatrix(m.getRows(), m.getCols(), flat));
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matCos();
            std::vector<Complex> flat = m.rawData();
            for(auto& v : flat) v = Complex(1.0, 0.0) / v;
            return Value(ComplexMatrix(m.getRows(), m.getCols(), flat));
        }
        if (args[0].isComplex()) return Value(Complex(1.0, 0.0) / cos(args[0].asComplex()));
        return Value(1.0 / std::cos(args[0].asDouble()));
    });
    regMath("csc", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat.matSin();
            std::vector<double> flat = m.rawData();
            for(auto& v : flat) v = 1.0 / v;
            return Value(RealMatrix(m.getRows(), m.getCols(), flat));
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.matSin();
            std::vector<Complex> flat = m.rawData();
            for(auto& v : flat) v = Complex(1.0, 0.0) / v;
            return Value(ComplexMatrix(m.getRows(), m.getCols(), flat));
        }
        if (args[0].isComplex()) return Value(Complex(1.0, 0.0) / sin(args[0].asComplex()));
        return Value(1.0 / std::sin(args[0].asDouble()));
    });

    regMath("log", { 1, 2 }, [](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(matLog(static_cast<ObjRealMatrix*>(args[0].asObj())->mat));
            if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(matLog(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat));
            if (args[0].isComplex()) return Value(log(args[0].asComplex()));
            double x = args[0].asDouble();
            if (x == 0) throw std::runtime_error("Math Error: Logarithm of zero.");
            if (x < 0) return Value(log(Complex(x, 0.0)));
            return Value(std::log(x));
        }
        Complex base = args[0].asComplex(), x = args[1].asComplex();
        return Value(log(x) / log(base));
    });

    regMath("ln", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(matLog(static_cast<ObjRealMatrix*>(args[0].asObj())->mat));
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(matLog(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat));
        if (args[0].isComplex()) return Value(log(args[0].asComplex()));
        double x = args[0].asDouble();
        if (x == 0) throw std::runtime_error("Math Error: Logarithm of zero.");
        if (x < 0) return Value(log(Complex(x, 0.0)));
        return Value(std::log(x));
    });

    regMath("sqrt", { 1 }, [](const std::vector<Value>& args) -> Value {
        return args[0] ^ Value(Fraction(1, 2));
    });

    regMath("sqrtD", { 1 }, [](const std::vector<Value>& args) -> Value {
        Value res = args[0] ^ Value(0.5);
        if (res.isObjType(ObjType::REAL_MATRIX) || res.isObjType(ObjType::COMPLEX_MATRIX)) return res;
        return res.isComplex() ? Value(res.asComplex()) : Value(res.asDouble());
    });

    regMath("cbrt", { 1 }, [](const std::vector<Value>& args) -> Value {
        return args[0] ^ Value(Fraction(1, 3));
    });

    regMath("cbrtD", { 1 }, [](const std::vector<Value>& args) -> Value {
        Value res = args[0] ^ Value(1.0 / 3.0);
        if (res.isObjType(ObjType::REAL_MATRIX) || res.isObjType(ObjType::COMPLEX_MATRIX)) return res;
        return res.isComplex() ? Value(res.asComplex()) : Value(res.asDouble());
    });

    reg("matpow", { 2 }, [](const std::vector<Value>& args) -> Value {
        return Value(matPow(args[0].asComplexMatrix(), args[1].asComplexMatrix()));
    });

    regMath("asin", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isComplex()) return Value(asin(args[0].asComplex()));
        double x = args[0].asDouble();
        if (x < -1.0 || x > 1.0) return Value(asin(Complex(x, 0.0)));
        return Value(std::asin(x));
    });
    regMath("acos", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isComplex()) return Value(acos(args[0].asComplex()));
        double x = args[0].asDouble();
        if (x < -1.0 || x > 1.0) return Value(acos(Complex(x, 0.0)));
        return Value(std::acos(x));
    });
    regMath("atan", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isComplex()) return Value(atan(args[0].asComplex()));
        return Value(std::atan(args[0].asDouble()));
    });
    regMath("atan2", { 2 }, [](const std::vector<Value>& args) -> Value {
        return Value(std::atan2(args[0].asDouble(), args[1].asDouble()));
        });

    regMath("asinh", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isComplex()) {
            Complex z = args[0].asComplex();
            return Value(log(z + sqrt(z * z + Complex(1.0, 0.0))));
        }
        return Value(std::asinh(args[0].asDouble()));
    });
    regMath("acosh", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isComplex()) {
            Complex z = args[0].asComplex();
            return Value(log(z + sqrt(z * z - Complex(1.0, 0.0))));
        }
        double x = args[0].asDouble();
        if (x < 1.0) {
            Complex z(x, 0.0);
            return Value(log(z + sqrt(z * z - Complex(1.0, 0.0))));
        }
        return Value(std::acosh(x));
    });
    regMath("atanh", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isComplex()) {
            Complex z = args[0].asComplex();
            return Value(Complex(0.5, 0.0) * log((Complex(1.0, 0.0) + z) / (Complex(1.0, 0.0) - z)));
        }
        double x = args[0].asDouble();
        if (x <= -1.0 || x >= 1.0) {
            Complex z(x, 0.0);
            return Value(Complex(0.5, 0.0) * log((Complex(1.0, 0.0) + z) / (Complex(1.0, 0.0) - z)));
        }
        return Value(std::atanh(x));
    });

    regMath("erf", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(std::erf(args[0].asDouble()));
        });
    // 通用高精度数值积分器 (Simpson's 1/3 Rule)
    auto numInteg = [](auto f, double a, double b) -> double {
        int n = std::max(1000, static_cast<int>(std::abs(b - a) * 1000));
        if (n % 2 != 0) n++;
        if (n > 100000) n = 100000; // 限制最大迭代次数防止卡死
        double h = (b - a) / n;
        double sum = f(a) + f(b);
        for (int i = 1; i < n; i += 2) { jc::checkInterrupt(); sum += 4 * f(a + i * h); }
        for (int i = 2; i < n - 1; i += 2) { jc::checkInterrupt(); sum += 2 * f(a + i * h); }
        return sum * h / 3.0;
    };

    regMath("fresnel_s", { 1 }, [numInteg](const std::vector<Value>& args) -> Value {
        double x = args[0].asDouble();
        return Value(numInteg([](double t) { return std::sin(1.57079632679489661923 * t * t); }, 0.0, x));
        });
    regMath("fresnel_c", { 1 }, [numInteg](const std::vector<Value>& args) -> Value {
        double x = args[0].asDouble();
        return Value(numInteg([](double t) { return std::cos(1.57079632679489661923 * t * t); }, 0.0, x));
        });
    regMath("Si", { 1 }, [numInteg](const std::vector<Value>& args) -> Value {
        double x = args[0].asDouble();
        return Value(numInteg([](double t) { return t == 0.0 ? 1.0 : std::sin(t) / t; }, 0.0, x));
        });
    regMath("Ci", { 1 }, [numInteg](const std::vector<Value>& args) -> Value {
        double x = args[0].asDouble();
        if (x <= 0.0) throw std::runtime_error("Math Error: Ci(x) is only real for x > 0.");
        double gamma = 0.577215664901532860606; // Euler-Mascheroni constant
        return Value(gamma + std::log(x) + numInteg([](double t) { return t == 0.0 ? 0.0 : (std::cos(t) - 1.0) / t; }, 0.0, x));
        });
    regMath("Ei", { 1 }, [numInteg](const std::vector<Value>& args) -> Value {
        double x = args[0].asDouble();
        if (x == 0.0) throw std::runtime_error("Math Error: Ei(0) is undefined.");
        double gamma = 0.577215664901532860606; // Euler-Mascheroni constant
        return Value(gamma + std::log(std::abs(x)) + numInteg([](double t) { return t == 0.0 ? 1.0 : (std::exp(t) - 1.0) / t; }, 0.0, x));
        });
    regMath("Li", { 1 }, [numInteg](const std::vector<Value>& args) -> Value {
        double x = args[0].asDouble();
        if (x <= 0.0 || x == 1.0) throw std::runtime_error("Math Error: Li(x) is defined for x > 0 and x != 1.");
        double lnx = std::log(x);
        double gamma = 0.577215664901532860606;
        return Value(gamma + std::log(std::abs(lnx)) + numInteg([](double t) { return t == 0.0 ? 1.0 : (std::exp(t) - 1.0) / t; }, 0.0, lnx));
        });

    regMath("abs", { 1 }, [](const std::vector<Value>& args) -> Value {
        // ★ Dunder 钩子: __abs__
        if (args[0].isInstance()) {
            auto inst = args[0].asInstance();
            auto [found, result] = invokeDunder(inst, DUNDER_ABS, {});
            if (found) return result;
        }
        if (args[0].isObjType(ObjType::BIGINT)) return Value(static_cast<ObjBigInt*>(args[0].asObj())->num.abs());
        if (args[0].isComplex()) return Value(args[0].asComplex().modulus());
        if (args[0].isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(args[0].asObj())->frac.abs());
        return Value(std::abs(args[0].asDouble()));
        });

    regMath("pow", { 2 }, [](const std::vector<Value>& args) -> Value {
        return args[0] ^ args[1];
    });

    regMath("root", { 2 }, [](const std::vector<Value>& args) -> Value {
        return args[0] ^ (Value(BigInt(1)) / args[1]);
    });

    regMath("rootD", { 2 }, [](const std::vector<Value>& args) -> Value {
        Value res = args[0] ^ (Value(1.0) / args[1]);
        if (res.isObjType(ObjType::REAL_MATRIX) || res.isObjType(ObjType::COMPLEX_MATRIX)) return res;
        return res.isComplex() ? Value(res.asComplex()) : Value(res.asDouble());
    });

    // 通用取整分发器
    auto roundDispatch = [](const std::vector<Value>& args, const std::string& name,
        std::function<double(double)> baseFn) -> Value {
            if (args.size() < 1 || args.size() > 2)
                throw std::runtime_error("Runtime Error: " + name + "() expects 1 or 2 arguments.");
            int n = 0;
            bool hasN = (args.size() == 2);
            if (hasN) n = static_cast<int>(std::round(args[1].asDouble()));
            double factor = std::pow(10.0, n);
            auto fn = [baseFn, factor](double x) -> double { return baseFn(x * factor) / factor; };
            if (args[0].isComplex()) {
                const Complex& c = args[0].asComplex();
                Complex result(fn(c.real), fn(c.imag));
                if (result.imag == 0.0) {
                    if (!hasN || n <= 0) return Value(BigInt(static_cast<int64_t>(result.real)));
                    return Value(result.real);
                }
                return Value(result);
            }
            if (args[0].isObjType(ObjType::REAL_MATRIX)) {
                const RealMatrix& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
                std::vector<double> flat;
                flat.reserve(static_cast<size_t>(m.getRows()) * m.getCols());
                for (int i = 0; i < m.getRows(); ++i)
                    for (int j = 0; j < m.getCols(); ++j)
                        flat.push_back(fn(m(i, j)));
                return Value(RealMatrix(m.getRows(), m.getCols(), flat));
            }
            if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
                const ComplexMatrix& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
                std::vector<Complex> flat;
                flat.reserve(static_cast<size_t>(m.getRows()) * m.getCols());
                for (int i = 0; i < m.getRows(); ++i)
                    for (int j = 0; j < m.getCols(); ++j)
                        flat.push_back(Complex(fn(m(i, j).real), fn(m(i, j).imag)));
                return Value(ComplexMatrix(m.getRows(), m.getCols(), flat));
            }
            double result = fn(args[0].asDouble());
            if (!hasN || n <= 0) return Value(BigInt(static_cast<int64_t>(result)));
            return Value(result);
        };

    regMath("round", { 1, 2 }, [roundDispatch](const std::vector<Value>& args) -> Value { return roundDispatch(args, "round", [](double x) { return std::round(x); }); });
    regMath("floor", { 1, 2 }, [roundDispatch](const std::vector<Value>& args) -> Value { return roundDispatch(args, "floor", [](double x) { return std::floor(x); }); });
    regMath("ceil", { 1, 2 }, [roundDispatch](const std::vector<Value>& args) -> Value { return roundDispatch(args, "ceil", [](double x) { return std::ceil(x); }); });
    regMath("trunc", { 1, 2 }, [roundDispatch](const std::vector<Value>& args) -> Value { return roundDispatch(args, "trunc", [](double x) { return std::trunc(x); }); });

    regMath("sgn", { 1 }, [](const std::vector<Value>& args) -> Value { double x = args[0].asDouble(); return Value::fromInt32(x > 0 ? 1 : (x < 0 ? -1 : 0)); });
    regMath("deg", { 1 }, [](const std::vector<Value>& args) -> Value { return Value(args[0].asDouble() / Complex::PI * 180.0); });
    regMath("rad", { 1 }, [](const std::vector<Value>& args) -> Value { return Value(args[0].asDouble() / 180.0 * Complex::PI); });
    
    reg("idiv", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isBigInt() && args[1].isBigInt()) {
            BigInt a = args[0].asBigInt(), b = args[1].asBigInt();
            if (b.isZero()) throw std::runtime_error("Math Error: Division by zero.");
            return Value(a / b);
        }
        double a = args[0].asDouble(), b = args[1].asDouble();
        if (b == 0.0) throw std::runtime_error("Math Error: Division by zero.");
        return Value(BigInt(static_cast<int64_t>(std::trunc(a / b))));
    });

}

// =================================================================
// [2] 复数属性
// =================================================================
void BuiltinRegistry::registerComplex() {
    reg("Re", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isComplex()) return Value(args[0].asComplex().real); return args[0]; });
    reg("Im", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isComplex()) return Value(args[0].asComplex().imag); return Value(0.0); });
    reg("arg", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isComplex()) return Value(args[0].asComplex().argument()); return Value(args[0].asDouble() >= 0 ? 0.0 : Complex::PI); });
    reg("conj", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isComplex()) return Value(args[0].asComplex().conjugate()); return args[0]; });
}

// =================================================================
// [3] 分数
// =================================================================
void BuiltinRegistry::registerFraction() {
    reg("frac", { 2 }, [](const std::vector<Value>& args) -> Value {
        BigInt n = args[0].isBigInt() ? args[0].asBigInt() : BigInt(static_cast<int64_t>(std::round(args[0].asDouble())));
        BigInt d = args[1].isBigInt() ? args[1].asBigInt() : BigInt(static_cast<int64_t>(std::round(args[1].asDouble())));
        return Value(Fraction(n, d));
    });
    reg("toFrac", { 1 }, [](const std::vector<Value>& args) -> Value {
        Value v = args[0];
        if (v.isInt32() || v.isBigInt()) return v;
        Fraction f;
        if (v.isObjType(ObjType::FRACTION)) {
            f = static_cast<ObjFraction*>(v.asObj())->frac;
        } else {
            f = Fraction::fromDouble(v.asDouble());
        }
        if (f.getDen() == BigInt(1)) return Value(f.getNum());
        return Value(f);
    });
    reg("num", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(args[0].asObj())->frac.getNum()); return args[0]; });
    reg("den", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(args[0].asObj())->frac.getDen()); return Value(1.0); });
}

// =================================================================
// [4] 多项式求解
// =================================================================
void BuiltinRegistry::registerPolySolver() {
    reg("solve", { 2, 3, 4, 5 }, [](const std::vector<Value>& args) -> Value {
        std::vector<Complex> roots;
        if (args.size() == 2) roots = Complex::solveDegreeOne(args[0].asComplex(), args[1].asComplex());
        else if (args.size() == 3) roots = Complex::solveDegreeTwo(args[0].asComplex(), args[1].asComplex(), args[2].asComplex());
        else if (args.size() == 4) roots = Complex::solveDegreeThree(args[0].asComplex(), args[1].asComplex(), args[2].asComplex(), args[3].asComplex());
        else roots = Complex::solveDegreeFour(args[0].asComplex(), args[1].asComplex(), args[2].asComplex(), args[3].asComplex(), args[4].asComplex());
        return Value(ComplexMatrix(static_cast<int>(roots.size()), 1, roots));
    });

}

// =================================================================
// [5] 矩阵运算
// =================================================================
void BuiltinRegistry::registerMatrixOps() {

    auto matrixDispatch1 = [](const Value& arg, auto func) -> Value {
        if (arg.isObjType(ObjType::REAL_MATRIX)) return Value(func(static_cast<ObjRealMatrix*>(arg.asObj())->mat));
        if (arg.isObjType(ObjType::COMPLEX_MATRIX)) return Value(func(static_cast<ObjComplexMatrix*>(arg.asObj())->mat));
        throw std::runtime_error("Type Error: Expected a matrix.");
    };

    // --- 逐元素运算 (Element-wise) ---
    auto elementWiseOp = [](const Value& a, const Value& b, const std::string& opName, auto scalarOp) -> Value {
        bool aMat = a.isObjType(ObjType::REAL_MATRIX) || a.isObjType(ObjType::COMPLEX_MATRIX) || a.isObjType(ObjType::STRING_MATRIX);
        bool bMat = b.isObjType(ObjType::REAL_MATRIX) || b.isObjType(ObjType::COMPLEX_MATRIX) || b.isObjType(ObjType::STRING_MATRIX);

        if (!aMat && !bMat) return scalarOp(a, b);

        int r1 = 1, c1 = 1, r2 = 1, c2 = 1;
        auto getDims = [](const Value& v, int& r, int& c) {
            if (v.isObjType(ObjType::REAL_MATRIX)) { r = static_cast<ObjRealMatrix*>(v.asObj())->mat.getRows(); c = static_cast<ObjRealMatrix*>(v.asObj())->mat.getCols(); }
            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) { r = static_cast<ObjComplexMatrix*>(v.asObj())->mat.getRows(); c = static_cast<ObjComplexMatrix*>(v.asObj())->mat.getCols(); }
            else if (v.isObjType(ObjType::STRING_MATRIX)) { r = static_cast<ObjStringMatrix*>(v.asObj())->mat.getRows(); c = static_cast<ObjStringMatrix*>(v.asObj())->mat.getCols(); }
        };
        getDims(a, r1, c1); getDims(b, r2, c2);

        if (aMat && bMat && (r1 != r2 || c1 != c2)) throw std::runtime_error("Math Error: Dimension mismatch in " + opName + "().");

        int r = std::max(r1, r2);
        int c = std::max(c1, c2);

        auto getElem = [](const Value& v, int idx) -> Value {
            if (v.isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(v.asObj())->mat.rawData()[idx]);
            if (v.isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(v.asObj())->mat.rawData()[idx]);
            if (v.isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(v.asObj())->mat.rawData()[idx]);
            return v;
        };

        Value firstRes = scalarOp(getElem(a, 0), getElem(b, 0));
        bool isStr = firstRes.isString();
        bool isComp = firstRes.isComplex();

        if (isStr) {
            std::vector<std::string> flat(r * c);
            for (int i = 0; i < r * c; ++i) {
                Value res = scalarOp(getElem(a, aMat ? i : 0), getElem(b, bMat ? i : 0));
                flat[i] = res.isString() ? res.asString() : res.toString();
            }
            return Value(StringMatrix(r, c, flat));
        } else if (isComp) {
            std::vector<Complex> flat(r * c);
            for (int i = 0; i < r * c; ++i) {
                flat[i] = scalarOp(getElem(a, aMat ? i : 0), getElem(b, bMat ? i : 0)).asComplex();
            }
            return Value(ComplexMatrix(r, c, flat));
        } else {
            std::vector<double> flat(r * c);
            for (int i = 0; i < r * c; ++i) {
                flat[i] = scalarOp(getElem(a, aMat ? i : 0), getElem(b, bMat ? i : 0)).asDouble();
            }
            return Value(RealMatrix(r, c, flat));
        }
    };

    reg("addE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { 
        return elementWiseOp(args[0], args[1], "addE", [](const Value& a, const Value& b) { 
            if (a.isString() || b.isString()) {
                std::ostringstream oss; oss << a << b; return Value(oss.str());
            }
            return a + b; 
        }); 
    });
    reg("subE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "subE", [](const Value& a, const Value& b) { return a - b; }); });
    reg("mulE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "mulE", [](const Value& a, const Value& b) { return a * b; }); });
    reg("divE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "divE", [](const Value& a, const Value& b) { return a / b; }); });
    reg("idivE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { 
        return elementWiseOp(args[0], args[1], "idivE", [](const Value& a, const Value& b) { 
            if (a.isBigInt() && b.isBigInt()) {
                BigInt ba = a.asBigInt(), bb = b.asBigInt();
                if (bb.isZero()) throw std::runtime_error("Math Error: Division by zero.");
                return Value(ba / bb);
            }
            double da = a.asDouble(), db = b.asDouble();
            if (db == 0.0) throw std::runtime_error("Math Error: Division by zero.");
            return Value(BigInt(static_cast<int64_t>(std::trunc(da / db))));
        }); 
    });
    reg("powE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "powE", [](const Value& a, const Value& b) { return a ^ b; }); });
    reg("modE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { 
        return elementWiseOp(args[0], args[1], "modE", [](const Value& a, const Value& b) { 
            if (a.isComplex() && !b.isComplex()) {
                Complex ca = a.asComplex();
                double cb = b.asDouble();
                if (cb == 0.0) throw std::runtime_error("Math Error: Modulo by zero.");
                double re = std::fmod(ca.real, cb);
                double im = std::fmod(ca.imag, cb);
                if (re < 0) re += std::abs(cb);
                if (im < 0) im += std::abs(cb);
                return Value(Complex(re, im));
            }
            return a % b; 
        }); 
    });
    reg("eqE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "eqE", [](const Value& a, const Value& b) { return Value::fromInt32(helpers::checkEqual(a, b) ? 1 : 0); }); });
    reg("neqE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "neqE", [](const Value& a, const Value& b) { return Value::fromInt32(!helpers::checkEqual(a, b) ? 1 : 0); }); });
    reg("ltE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "ltE", [](const Value& a, const Value& b) { return Value::fromInt32(helpers::checkLess(a, b) ? 1 : 0); }); });
    reg("leE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "leE", [](const Value& a, const Value& b) { return Value::fromInt32(!helpers::checkGreater(a, b) ? 1 : 0); }); });
    reg("gtE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "gtE", [](const Value& a, const Value& b) { return Value::fromInt32(helpers::checkGreater(a, b) ? 1 : 0); }); });
    reg("geE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "geE", [](const Value& a, const Value& b) { return Value::fromInt32(!helpers::checkLess(a, b) ? 1 : 0); }); });
    reg("maxE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "maxE", [](const Value& a, const Value& b) { return helpers::checkGreater(a, b) ? a : b; }); });
    reg("minE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "minE", [](const Value& a, const Value& b) { return helpers::checkLess(a, b) ? a : b; }); });
    reg("andE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "andE", [](const Value& a, const Value& b) { return Value::fromInt32((a.truthy() && b.truthy()) ? 1 : 0); }); });
    reg("orE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "orE", [](const Value& a, const Value& b) { return Value::fromInt32((a.truthy() || b.truthy()) ? 1 : 0); }); });
    reg("xorE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "xorE", [](const Value& a, const Value& b) { return Value::fromInt32((a.truthy() != b.truthy()) ? 1 : 0); }); });
    reg("atan2E", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "atan2E", [](const Value& a, const Value& b) { return Value(std::atan2(a.asDouble(), b.asDouble())); }); });
    reg("hypotE", { 2 }, [elementWiseOp](const std::vector<Value>& args) -> Value { return elementWiseOp(args[0], args[1], "hypotE", [](const Value& a, const Value& b) { return Value(std::hypot(a.asDouble(), b.asDouble())); }); });
    reg("whereE", { 3 }, [](const std::vector<Value>& args) -> Value {
        const Value& mask = args[0]; const Value& a = args[1]; const Value& b = args[2];
        bool mMat = mask.isObjType(ObjType::REAL_MATRIX) || mask.isObjType(ObjType::COMPLEX_MATRIX) || mask.isObjType(ObjType::STRING_MATRIX);
        bool aMat = a.isObjType(ObjType::REAL_MATRIX) || a.isObjType(ObjType::COMPLEX_MATRIX) || a.isObjType(ObjType::STRING_MATRIX);
        bool bMat = b.isObjType(ObjType::REAL_MATRIX) || b.isObjType(ObjType::COMPLEX_MATRIX) || b.isObjType(ObjType::STRING_MATRIX);

        if (!mMat && !aMat && !bMat) return mask.truthy() ? a : b;

        int r = 1, c = 1;
        auto updateDims = [&](const Value& v) {
            if (v.isObjType(ObjType::REAL_MATRIX)) { r = std::max(r, static_cast<ObjRealMatrix*>(v.asObj())->mat.getRows()); c = std::max(c, static_cast<ObjRealMatrix*>(v.asObj())->mat.getCols()); }
            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) { r = std::max(r, static_cast<ObjComplexMatrix*>(v.asObj())->mat.getRows()); c = std::max(c, static_cast<ObjComplexMatrix*>(v.asObj())->mat.getCols()); }
            else if (v.isObjType(ObjType::STRING_MATRIX)) { r = std::max(r, static_cast<ObjStringMatrix*>(v.asObj())->mat.getRows()); c = std::max(c, static_cast<ObjStringMatrix*>(v.asObj())->mat.getCols()); }
        };
        updateDims(mask); updateDims(a); updateDims(b);

        auto checkDims = [&](const Value& v, const std::string& name) {
            if (v.isObjType(ObjType::REAL_MATRIX)) { if (static_cast<ObjRealMatrix*>(v.asObj())->mat.getRows() != r || static_cast<ObjRealMatrix*>(v.asObj())->mat.getCols() != c) throw std::runtime_error("Math Error: Dimension mismatch in whereE() for " + name + "."); }
            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) { if (static_cast<ObjComplexMatrix*>(v.asObj())->mat.getRows() != r || static_cast<ObjComplexMatrix*>(v.asObj())->mat.getCols() != c) throw std::runtime_error("Math Error: Dimension mismatch in whereE() for " + name + "."); }
            else if (v.isObjType(ObjType::STRING_MATRIX)) { if (static_cast<ObjStringMatrix*>(v.asObj())->mat.getRows() != r || static_cast<ObjStringMatrix*>(v.asObj())->mat.getCols() != c) throw std::runtime_error("Math Error: Dimension mismatch in whereE() for " + name + "."); }
        };
        checkDims(mask, "mask"); checkDims(a, "true_val"); checkDims(b, "false_val");

        auto getElem = [&](const Value& v, int idx) -> Value {
            if (v.isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(v.asObj())->mat.rawData()[idx]);
            if (v.isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(v.asObj())->mat.rawData()[idx]);
            if (v.isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(v.asObj())->mat.rawData()[idx]);
            return v;
        };

        bool isStr = false, isComp = false;
        if (a.isObjType(ObjType::STRING_MATRIX) || b.isObjType(ObjType::STRING_MATRIX)) isStr = true;
        else if (!aMat && a.isString()) isStr = true;
        else if (!bMat && b.isString()) isStr = true;
        else if (a.isObjType(ObjType::COMPLEX_MATRIX) || b.isObjType(ObjType::COMPLEX_MATRIX)) isComp = true;
        else if (!aMat && a.isComplex()) isComp = true;
        else if (!bMat && b.isComplex()) isComp = true;

        if (isStr) {
            std::vector<std::string> flat(r * c);
            for (int i = 0; i < r * c; ++i) {
                Value res = getElem(mask, mMat ? i : 0).truthy() ? getElem(a, aMat ? i : 0) : getElem(b, bMat ? i : 0);
                flat[i] = res.isString() ? res.asString() : res.toString();
            }
            return Value(StringMatrix(r, c, flat));
        } else if (isComp) {
            std::vector<Complex> flat(r * c);
            for (int i = 0; i < r * c; ++i) flat[i] = getElem(mask, mMat ? i : 0).truthy() ? getElem(a, aMat ? i : 0).asComplex() : getElem(b, bMat ? i : 0).asComplex();
            return Value(ComplexMatrix(r, c, flat));
        } else {
            std::vector<double> flat(r * c);
            for (int i = 0; i < r * c; ++i) flat[i] = getElem(mask, mMat ? i : 0).truthy() ? getElem(a, aMat ? i : 0).asDouble() : getElem(b, bMat ? i : 0).asDouble();
            return Value(RealMatrix(r, c, flat));
        }
    });

    // --- 性质 ---
    reg("det", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.determinant()); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.determinant()); throw std::runtime_error("Type Error: det() requires a matrix."); });
    reg("inv", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.inverse()); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.inverse()); throw std::runtime_error("Type Error: inv() requires a matrix."); });
    reg("trans", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.transpose()); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.transpose()); if (args[0].isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.transpose()); throw std::runtime_error("Type Error: trans() requires a matrix."); });
    reg("gauss", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.gaussianElimination().first); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.gaussianElimination().first); throw std::runtime_error("Type Error: gauss() requires a matrix."); });

    reg("rank", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return Value::fromInt32(m.rank()); }); });
    reg("tr", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.trace(); }); });
    reg("norm", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.norm(); }); });
    reg("cond", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.condition(); }); });
    reg("adj", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.adjugate(); }); });
    reg("perm", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.permanent(); }); });
    reg("sum", { 1 }, [matrixDispatch1, this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        Value s(0.0);
        GcValueGuard sGuard(s);
        bool first = true;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (first) { s = nextVal; first = false; }
            else s = s + nextVal;
            return true;
        })) {
            if (first) return Value(0.0);
            return s;
        }
        // ★ 新增：无缝支持 List 容器
        if (arg.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(arg.asObj())->vec;
            if (L.empty()) return Value(0.0);
            Value listSum = L[0];
            GcValueGuard listSumGuard(listSum);
            for (size_t i = 1; i < L.size(); ++i) {
                listSum = listSum + L[i];  // 利用 Value 的重载 +
            }
            return listSum;
        }
        return matrixDispatch1(arg, [](const auto& m) { return m.sum(); });
        });

    reg("prod", { 1 }, [matrixDispatch1, this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        Value p(1.0);
        GcValueGuard pGuard(p);
        bool first = true;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (first) { p = nextVal; first = false; }
            else p = p * nextVal;
            return true;
        })) {
            if (first) return Value(1.0);
            return p;
        }
        // ★ 新增：无缝支持 List 容器
        if (arg.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(arg.asObj())->vec;
            if (L.empty()) return Value(1.0);
            Value listProd = L[0];
            GcValueGuard listProdGuard(listProd);
            for (size_t i = 1; i < L.size(); ++i) {
                listProd = listProd * L[i];  // 利用 Value 的重载 *
            }
            return listProd;
        }
        return matrixDispatch1(arg, [](const auto& m) { return m.product(); });
        });    
    reg("null", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.nullSpace(); }); });
    reg("orth", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.orthogonalize(); }); });
    reg("ctrans", { 1 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { return matrixDispatch1(args[0], [](const auto& m) { return m.conjugateTranspose(); }); });
    reg("mpow", { 2 }, [matrixDispatch1](const std::vector<Value>& args) -> Value { int n = static_cast<int>(std::round(args[1].asDouble())); return matrixDispatch1(args[0], [n](const auto& m) { return m.power(n); }); });

    // --- 维度 ---
    reg("row", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value::fromInt32(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.getRows());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value::fromInt32(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.getRows());
        if (args[0].isObjType(ObjType::STRING_MATRIX)) return Value::fromInt32(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.getRows());
        throw std::runtime_error("Type Error: row() requires a matrix.");
    });
    reg("col", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value::fromInt32(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.getCols());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value::fromInt32(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.getCols());
        if (args[0].isObjType(ObjType::STRING_MATRIX)) return Value::fromInt32(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.getCols());
        throw std::runtime_error("Type Error: col() requires a matrix.");
    });
    reg("rows", builtinArity["row"], builtins["row"]);
    reg("cols", builtinArity["col"], builtins["col"]);

    // --- 元素/行列访问 ---
    reg("getElement", { 3 }, [](const std::vector<Value>& args) -> Value { int r = static_cast<int>(std::round(args[1].asDouble())), c = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat(r, c)); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat(r, c)); if (args[0].isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(args[0].asObj())->mat(r, c)); throw std::runtime_error("Type Error: getElement() requires a matrix."); });
    reg("setElement", { 4 }, [](const std::vector<Value>& args) -> Value { int r = static_cast<int>(std::round(args[1].asDouble())), c = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) { RealMatrix res = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; res(r, c) = args[3].asDouble(); return Value(res); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { ComplexMatrix res = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; res(r, c) = args[3].asComplex(); return Value(res); } if (args[0].isObjType(ObjType::STRING_MATRIX)) { StringMatrix res = static_cast<ObjStringMatrix*>(args[0].asObj())->mat; if (args[3].isString()) res(r, c) = args[3].asString(); else { std::ostringstream oss; oss << args[3]; res(r, c) = oss.str(); } return Value(res); } throw std::runtime_error("Type Error: setElement() requires a matrix."); });

    // 行列操作（简写宏化）
    #define ROW_COL_OP(NAME, BODY) reg(NAME, { 2 }, [](const std::vector<Value>& args) -> Value { \
        int idx = static_cast<int>(std::round(args[1].asDouble())); \
        if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.BODY); \
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.BODY); \
        if (args[0].isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.BODY); \
        throw std::runtime_error("Type Error: requires a matrix."); })

    ROW_COL_OP("getR", getRow(idx));
    ROW_COL_OP("getC", getCol(idx));
    ROW_COL_OP("delR", deleteRow(idx));
    ROW_COL_OP("delC", deleteCol(idx));
    #undef ROW_COL_OP

    reg("swapR", { 3 }, [](const std::vector<Value>& args) -> Value { int r1 = static_cast<int>(std::round(args[1].asDouble())), r2 = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) { RealMatrix m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; m.swapRows(r1, r2); return Value(m); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { ComplexMatrix m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; m.swapRows(r1, r2); return Value(m); } if (args[0].isObjType(ObjType::STRING_MATRIX)) { StringMatrix m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat; m.swapRows(r1, r2); return Value(m); } throw std::runtime_error("Type Error: requires a matrix."); });
    reg("swapC", { 3 }, [](const std::vector<Value>& args) -> Value { int c1 = static_cast<int>(std::round(args[1].asDouble())), c2 = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) { RealMatrix m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; m.swapCols(c1, c2); return Value(m); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { ComplexMatrix m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; m.swapCols(c1, c2); return Value(m); } if (args[0].isObjType(ObjType::STRING_MATRIX)) { StringMatrix m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat; m.swapCols(c1, c2); return Value(m); } throw std::runtime_error("Type Error: requires a matrix."); });
    reg("multiR", { 3 }, [](const std::vector<Value>& args) -> Value { int r = static_cast<int>(std::round(args[1].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) { RealMatrix m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; m.multiplyRow(r, args[2].asDouble()); return Value(m); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { ComplexMatrix m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; m.multiplyRow(r, args[2].asComplex()); return Value(m); } throw std::runtime_error("Type Error: requires a matrix."); });
    reg("multiC", { 3 }, [](const std::vector<Value>& args) -> Value { int c = static_cast<int>(std::round(args[1].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) { RealMatrix m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; double s = args[2].asDouble(); for (int r = 0; r < m.getRows(); ++r) m(r, c) = m(r, c) * s; return Value(m); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { ComplexMatrix m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; Complex s = args[2].asComplex(); for (int r = 0; r < m.getRows(); ++r) m(r, c) = m(r, c) * s; return Value(m); } throw std::runtime_error("Type Error: requires a matrix."); });
    reg("addR", { 4 }, [](const std::vector<Value>& args) -> Value { int r1 = static_cast<int>(std::round(args[1].asDouble())), r2 = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) { RealMatrix m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; m.addRows(r1, r2, args[3].asDouble()); return Value(m); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { ComplexMatrix m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; m.addRows(r1, r2, args[3].asComplex()); return Value(m); } throw std::runtime_error("Type Error: requires a matrix."); });
    reg("addC", { 4 }, [](const std::vector<Value>& args) -> Value { int c1 = static_cast<int>(std::round(args[1].asDouble())), c2 = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) { RealMatrix m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; double s = args[3].asDouble(); for (int r = 0; r < m.getRows(); ++r) m(r, c1) = m(r, c1) + s * m(r, c2); return Value(m); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { ComplexMatrix m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; Complex s = args[3].asComplex(); for (int r = 0; r < m.getRows(); ++r) m(r, c1) = m(r, c1) + s * m(r, c2); return Value(m); } throw std::runtime_error("Type Error: requires a matrix."); });

    // --- 结构 ---
    reg("reshape", { 3 }, [](const std::vector<Value>& args) -> Value { int r = static_cast<int>(std::round(args[1].asDouble())), c = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.reshape(r, c)); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.reshape(r, c)); if (args[0].isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.reshape(r, c)); throw std::runtime_error("Type Error: reshape() requires a matrix."); });
    reg("sub", { 3 }, [](const std::vector<Value>& args) -> Value { int r = static_cast<int>(std::round(args[1].asDouble())), c = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.subMatrix(r, c)); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.subMatrix(r, c)); throw std::runtime_error("Type Error: requires a matrix."); });
    reg("cof", { 3 }, [](const std::vector<Value>& args) -> Value { int r = static_cast<int>(std::round(args[1].asDouble())), c = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.cofactor(r, c)); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.cofactor(r, c)); throw std::runtime_error("Type Error: requires a matrix."); });
    reg("Acof", { 3 }, [](const std::vector<Value>& args) -> Value { int r = static_cast<int>(std::round(args[1].asDouble())), c = static_cast<int>(std::round(args[2].asDouble())); if (args[0].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.algebraicCofactor(r, c)); if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.algebraicCofactor(r, c)); throw std::runtime_error("Type Error: requires a matrix."); });

    // --- 拼接 ---
    reg("integR", { 2 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::REAL_MATRIX) && args[1].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.integR(static_cast<ObjRealMatrix*>(args[1].asObj())->mat)); if (args[0].isObjType(ObjType::STRING_MATRIX) && args[1].isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.integR(static_cast<ObjStringMatrix*>(args[1].asObj())->mat)); return Value(args[0].asComplexMatrix().integR(args[1].asComplexMatrix())); });
    reg("integC", { 2 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::REAL_MATRIX) && args[1].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.integC(static_cast<ObjRealMatrix*>(args[1].asObj())->mat)); if (args[0].isObjType(ObjType::STRING_MATRIX) && args[1].isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.integC(static_cast<ObjStringMatrix*>(args[1].asObj())->mat)); return Value(args[0].asComplexMatrix().integC(args[1].asComplexMatrix())); });
    reg("integD", { 2 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::REAL_MATRIX) && args[1].isObjType(ObjType::REAL_MATRIX)) return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.integD(static_cast<ObjRealMatrix*>(args[1].asObj())->mat)); return Value(args[0].asComplexMatrix().integD(args[1].asComplexMatrix())); });

    // --- 生成器 ---
    reg("id", { 1 }, [](const std::vector<Value>& args) -> Value { int n = static_cast<int>(std::round(args[0].asDouble())); if (n < 1) throw std::runtime_error("Runtime Error: Size must be positive."); return Value(RealMatrix::identity(n)); });
    reg("ones", { 1, 2 }, [](const std::vector<Value>& args) -> Value { if (args.size() == 1) { int n = static_cast<int>(std::round(args[0].asDouble())); return Value(RealMatrix::ones(n, n)); } int r = static_cast<int>(std::round(args[0].asDouble())), c = static_cast<int>(std::round(args[1].asDouble())); return Value(RealMatrix::ones(r, c)); });
    reg("zeros", { 1, 2 }, [](const std::vector<Value>& args) -> Value { if (args.size() == 1) { int n = static_cast<int>(std::round(args[0].asDouble())); return Value(RealMatrix::zeros(n, n)); } int r = static_cast<int>(std::round(args[0].asDouble())), c = static_cast<int>(std::round(args[1].asDouble())); return Value(RealMatrix::zeros(r, c)); });
}

// =================================================================
// [6] 矩阵分解与特征值
// =================================================================
void BuiltinRegistry::registerDecompositions() {
    reg("qr", { 1 }, [](const std::vector<Value>& args) -> Value {
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto [Q, R] = static_cast<ObjRealMatrix*>(args[0].asObj())->mat.qrDecomposition();
            L->vec.push_back(Value(Q)); L->vec.push_back(Value(R));
        } else if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto [Q, R] = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.qrDecomposition();
            L->vec.push_back(Value(Q)); L->vec.push_back(Value(R));
        } else throw std::runtime_error("Type Error: requires a matrix.");
        L->is_frozen = true; return Value(L);
    });
    reg("lu", { 1 }, [](const std::vector<Value>& args) -> Value {
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto res = static_cast<ObjRealMatrix*>(args[0].asObj())->mat.luDecomposition();
            L->vec.push_back(Value(res.L)); L->vec.push_back(Value(res.U)); L->vec.push_back(Value(res.P));
        } else if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto res = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.luDecomposition();
            L->vec.push_back(Value(res.L)); L->vec.push_back(Value(res.U)); L->vec.push_back(Value(res.P));
        } else throw std::runtime_error("Type Error: requires a matrix.");
        L->is_frozen = true; return Value(L);
    });
    reg("eig", { 1 }, [](const std::vector<Value>& args) -> Value { std::vector<Complex> vals; if (args[0].isObjType(ObjType::REAL_MATRIX)) vals = computeEigenvalues(static_cast<ObjRealMatrix*>(args[0].asObj())->mat); else if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) vals = computeEigenvalues(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat); else throw std::runtime_error("Type Error: requires a matrix."); return Value(ComplexMatrix(static_cast<int>(vals.size()), 1, vals)); });
    reg("eigvec", { 1 }, [](const std::vector<Value>& args) -> Value { ComplexMatrix A = args[0].isObjType(ObjType::REAL_MATRIX) ? static_cast<ObjRealMatrix*>(args[0].asObj())->mat.toComplexMatrix() : args[0].asComplexMatrix(); auto vals = computeEigenvalues(A); return Value(computeEigenvectors(A, vals)); });
    reg("diag", { 1 }, [](const std::vector<Value>& args) -> Value {
        ComplexMatrix A = args[0].isObjType(ObjType::REAL_MATRIX) ? static_cast<ObjRealMatrix*>(args[0].asObj())->mat.toComplexMatrix() : args[0].asComplexMatrix();
        auto [P, D] = diagonalize(A);
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        L->vec.push_back(Value(P)); L->vec.push_back(Value(D));
        L->is_frozen = true; return Value(L);
    });
}

// =================================================================
// [7] 线性方程组
// =================================================================
void BuiltinRegistry::registerLinearSolvers() {
    reg("lsolve", { 2 }, [](const std::vector<Value>& args) -> Value {
        ComplexMatrix A = args[0].asComplexMatrix(), b = args[1].asComplexMatrix();
        if (A.getRows() != b.getRows()) throw std::runtime_error("Math Error: Row count mismatch.");
        if (b.getCols() != 1) throw std::runtime_error("Math Error: b must be Nx1.");
        int n = A.getCols();
        ComplexMatrix aug = A.integR(b);
        int rankA = A.rank(), rankAug = aug.rank();
        
        ObjDict* d = GcHeap::get().allocate<ObjDict>();
        GcObjGuard guard(d);
        auto setField = [&](const std::string& k, Value v) {
            Value key(k);
            d->keyMap[key] = d->elements.size();
            d->elements.push_back({key, v});
        };

        if (rankA != rankAug) { 
            ComplexMatrix AH = A.conjugateTranspose(); 
            ComplexMatrix nA = AH * A, nb = AH * b; 
            ComplexMatrix a2 = nA.integR(nb); 
            auto [r2, s2] = a2.gaussianElimination(); 
            int n2 = nA.getCols(); 
            std::vector<Complex> sol(n2); 
            for (int i = 0; i < n2; ++i) sol[i] = r2(i, n2); 
            ComplexMatrix approx(n2, 1, sol);
            ComplexMatrix residual = b - A * approx;
            setField("status", Value("least_squares"));
            setField("approx", Value(approx));
            setField("residual", Value(residual));
            return Value(d);
        }
        
        auto [rref, swaps] = aug.gaussianElimination();
        std::vector<int> pivotCols; 
        for (int i = 0; i < A.getRows(); ++i) {
            for (int j = 0; j < n; ++j) { 
                if (!ComplexMatrix::isEssentiallyZero(rref(i, j))) { 
                    pivotCols.push_back(j); 
                    break; 
                } 
            }
        }
        std::vector<Complex> particular(n, Complex(0, 0)); 
        for (int p = 0; p < static_cast<int>(pivotCols.size()); ++p) {
            particular[pivotCols[p]] = rref(p, n);
        }
        ComplexMatrix partMat(n, 1, particular);
        
        if (rankA == n) {
            setField("status", Value("unique"));
            setField("solution", Value(partMat));
        } else {
            setField("status", Value("infinite"));
            setField("particular", Value(partMat));
            setField("basis", Value(A.nullSpace()));
        }
        return Value(d);
    });
    reg("lstsq", { 2 }, [](const std::vector<Value>& args) -> Value { ComplexMatrix A = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); ComplexMatrix AH = A.conjugateTranspose(); ComplexMatrix aug = (AH * A).integR(AH * b); auto [rref, sw] = aug.gaussianElimination(); int n = (AH * A).getCols(); std::vector<Complex> sol(n); for (int i = 0; i < n; ++i) sol[i] = rref(i, n); return Value(ComplexMatrix(n, 1, sol)); });
    reg("residual", { 3 }, [](const std::vector<Value>& args) -> Value { return Value(args[2].asComplexMatrix() - args[0].asComplexMatrix() * args[1].asComplexMatrix()); });
}

// =================================================================
// [8] 向量引擎
// =================================================================
void BuiltinRegistry::registerVectors() {
    auto assertVec = [](const Value& v, const std::string& f) { if (v.isObjType(ObjType::REAL_MATRIX)) { if (static_cast<ObjRealMatrix*>(v.asObj())->mat.getCols() != 1) throw std::runtime_error(f + "() expects Nx1 column vector."); } else if (v.isObjType(ObjType::COMPLEX_MATRIX)) { if (static_cast<ObjComplexMatrix*>(v.asObj())->mat.getCols() != 1) throw std::runtime_error(f + "() expects Nx1 column vector."); } else throw std::runtime_error(f + "() requires a matrix."); };

    reg("dim", { 1 }, [assertVec](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST))
            return Value::fromInt32(static_cast<int32_t>(static_cast<ObjList*>(args[0].asObj())->vec.size()));
        assertVec(args[0], "dim");
        if (args[0].isObjType(ObjType::REAL_MATRIX))
            return Value::fromInt32(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.getRows());
        return Value::fromInt32(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.getRows());
        });    
    reg("dot", { 2 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "dot"); assertVec(args[1], "dot"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); if (a.getRows() != b.getRows()) throw std::runtime_error("Math Error: Dimension mismatch."); return Value((a.conjugateTranspose() * b)(0, 0)); });
    reg("vnorm", { 1 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "vnorm"); ComplexMatrix v = args[0].asComplexMatrix(); return Value(std::sqrt((v.conjugateTranspose() * v)(0, 0).real)); });
    reg("normalize", { 1 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "normalize"); ComplexMatrix v = args[0].asComplexMatrix(); double len = std::sqrt((v.conjugateTranspose() * v)(0, 0).real); if (len == 0.0) throw std::runtime_error("Math Error: Cannot normalize a zero vector."); return Value(v / Complex(len)); });
    reg("cross", { 2 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "cross"); assertVec(args[1], "cross"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); if (a.getRows() != 3 || b.getRows() != 3) throw std::runtime_error("Math Error: Cross product is 3D only."); std::vector<Complex> r = { a(1,0)*b(2,0)-a(2,0)*b(1,0), a(2,0)*b(0,0)-a(0,0)*b(2,0), a(0,0)*b(1,0)-a(1,0)*b(0,0) }; return Value(ComplexMatrix(3, 1, r)); });
    reg("angle", { 2 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "angle"); assertVec(args[1], "angle"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); double nA = std::sqrt((a.conjugateTranspose()*a)(0,0).real), nB = std::sqrt((b.conjugateTranspose()*b)(0,0).real); if (nA == 0.0 || nB == 0.0) throw std::runtime_error("Math Error: Zero vector."); double ct = (a.conjugateTranspose()*b)(0,0).real/(nA*nB); ct = std::max(-1.0, std::min(1.0, ct)); return Value(std::acos(ct)); });
    reg("sproj", { 2 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "sproj"); assertVec(args[1], "sproj"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); double nB = std::sqrt((b.conjugateTranspose()*b)(0,0).real); if (nB == 0.0) throw std::runtime_error("Math Error: Zero vector."); return Value((a.conjugateTranspose()*b)(0,0).real/nB); });
    reg("vproj", { 2 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "vproj"); assertVec(args[1], "vproj"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); Complex dBB = (b.conjugateTranspose()*b)(0,0); if (dBB.real == 0.0 && dBB.imag == 0.0) throw std::runtime_error("Math Error: Zero vector."); return Value(b * ((a.conjugateTranspose()*b)(0,0)/dBB)); });
    reg("triple", { 3 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "triple"); assertVec(args[1], "triple"); assertVec(args[2], "triple"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(), c = args[2].asComplexMatrix(); if (a.getRows()!=3||b.getRows()!=3||c.getRows()!=3) throw std::runtime_error("Math Error: 3D only."); std::vector<Complex> bc = { b(1,0)*c(2,0)-b(2,0)*c(1,0), b(2,0)*c(0,0)-b(0,0)*c(2,0), b(0,0)*c(1,0)-b(1,0)*c(0,0) }; return Value(a(0,0)*bc[0]+a(1,0)*bc[1]+a(2,0)*bc[2]); });
    reg("isperp", { 2 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "isperp"); assertVec(args[1], "isperp"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); double innerScale = a.norm()*b.norm(); return Value(Tol::clean((a.conjugateTranspose()*b)(0,0).modulus(), innerScale)==0.0); });
    reg("isparallel", { 2 }, [assertVec](const std::vector<Value>& args) -> Value { assertVec(args[0], "isparallel"); assertVec(args[1], "isparallel"); ComplexMatrix a = args[0].asComplexMatrix(), b = args[1].asComplexMatrix(); return Value(a.integR(b).rank()<=1); });
}

// =================================================================
// [9] 数论
// =================================================================
void BuiltinRegistry::registerNumberTheory() {
    auto toBigInt = [](const Value& v) -> BigInt {
        if (v.isInt32()) return BigInt(v.asInt32());
        if (v.isBigInt()) return static_cast<ObjBigInt*>(v.asObj())->num;
        return BigInt(static_cast<int64_t>(std::round(v.asDouble())));
    };

    auto toInt64 = [](const Value& v) -> int64_t {
        if (v.isInt32()) return v.asInt32();
        if (v.isBigInt()) return static_cast<ObjBigInt*>(v.asObj())->num.toInt64();
        return static_cast<int64_t>(std::round(v.asDouble()));
    };

    reg("factorial", { 1 }, [toInt64](const std::vector<Value>& args) -> Value { return Value(BigInt::factorial(toInt64(args[0]))); });
    reg("fib", { 1 }, [toInt64](const std::vector<Value>& args) -> Value { return Value(BigInt::fibonacci(toInt64(args[0]))); });
    reg("gcd", { 2 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(BigInt::gcd(toBigInt(args[0]), toBigInt(args[1]))); });
    reg("lcm", { 2 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(BigInt::lcm(toBigInt(args[0]), toBigInt(args[1]))); });
    reg("digits", { 1 }, [](const std::vector<Value>& args) -> Value { 
        if (args[0].isInt32()) return Value::fromInt32(args[0].asInt32() == 0 ? 0 : static_cast<int32_t>(std::to_string(args[0].asInt32()).size() - (args[0].asInt32() < 0 ? 1 : 0)));
        if (args[0].isBigInt()) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjBigInt*>(args[0].asObj())->num.digitCount()));
        if (args[0].isObjType(ObjType::BASENUM)) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjBaseNum*>(args[0].asObj())->base.digitCount()));
        throw std::runtime_error("Type Error: expects an integer or basenum."); 
    });
    reg("isPrime", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(toBigInt(args[0]).isPrime()); });
    reg("nextPrime", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(toBigInt(args[0]).nextPrime()); });
    reg("nthPrime", { 1 }, [toInt64](const std::vector<Value>& args) -> Value { return Value(BigInt::nthPrime(toInt64(args[0]))); });
    reg("primePi", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(BigInt(toBigInt(args[0]).primePi())); });
    reg("phi", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(toBigInt(args[0]).eulerPhi()); });
    reg("divisors", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(toBigInt(args[0]).divisorCount()); });
    reg("sigma", { 1, 2 }, [toBigInt, toInt64](const std::vector<Value>& args) -> Value { int64_t k = (args.size()==2) ? toInt64(args[1]) : 1; return Value(toBigInt(args[0]).divisorSum(k)); });
    reg("omega", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(BigInt(toBigInt(args[0]).omega())); });
    reg("bigOmega", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(BigInt(toBigInt(args[0]).bigOmega())); });
    reg("mobius", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(BigInt(toBigInt(args[0]).mobius())); });
    reg("isPerfect", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(toBigInt(args[0]).isPerfect()); });
    reg("mod", { 2 }, [toBigInt](const std::vector<Value>& args) -> Value {
        if ((args[0].isBigInt() || args[0].isInt32()) && (args[1].isBigInt() || args[1].isInt32())) return Value(BigInt::mathMod(toBigInt(args[0]), toBigInt(args[1])));
        if (args[0].isObjType(ObjType::FRACTION)) { const auto& f = static_cast<ObjFraction*>(args[0].asObj())->frac; if (f.getDen() == BigInt(1)) return Value(BigInt::mathMod(f.getNum(), toBigInt(args[1]))); }
        double a = args[0].asDouble(), b = args[1].asDouble();
        if (b == 0.0) throw std::runtime_error("Math Error: Modulo by zero.");
        double r = std::fmod(a, b); if (r < 0) r += std::abs(b); return Value(r);
    });
    reg("modpow", { 3 }, [toBigInt](const std::vector<Value>& args) -> Value { return Value(BigInt::modPow(toBigInt(args[0]), toBigInt(args[1]), toBigInt(args[2]))); });
    reg("C", { 2 }, [toInt64](const std::vector<Value>& args) -> Value { int64_t n = toInt64(args[0]), k = toInt64(args[1]); if (n<0||k<0) throw std::runtime_error("Math Error: C(n,k) requires non-negative integers."); if (k>n) return Value(BigInt(0)); if (k>n-k) k = n-k; BigInt result(1); for (int64_t i = 0; i < k; ++i) { jc::checkInterrupt(); result = result*BigInt(n-i); result = result/BigInt(i+1); } return Value(result); });
    reg("A", { 2 }, [toInt64](const std::vector<Value>& args) -> Value { int64_t n = toInt64(args[0]), k = toInt64(args[1]); if (n<0||k<0) throw std::runtime_error("Math Error: A(n,k) requires non-negative integers."); if (k>n) return Value(BigInt(0)); BigInt result(1); for (int64_t i = 0; i < k; ++i) { jc::checkInterrupt(); result = result*BigInt(n-i); } return Value(result); });
    reg("catalan", { 1 }, [toInt64](const std::vector<Value>& args) -> Value { int64_t n = toInt64(args[0]); if (n<0) throw std::runtime_error("Math Error: catalan(n) requires non-negative integer."); BigInt result(1); for (int64_t i = 0; i < n; ++i) { jc::checkInterrupt(); result = result*BigInt(2*n-i); result = result/BigInt(i+1); } result = result/BigInt(n+1); return Value(result); });
}

// =================================================================
// [10] 多进制与位运算
// =================================================================
void BuiltinRegistry::registerBase() {
    reg("base", { 2 }, [](const std::vector<Value>& args) -> Value { return Value(BaseNum(args[0].asBigInt(), static_cast<int>(std::round(args[1].asDouble())))); });
    reg("bnum", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: First arg must be a string."); return Value(BaseNum::fromString(args[0].asString(), static_cast<int>(std::round(args[1].asDouble())))); });
    reg("changeBase", { 2 }, [](const std::vector<Value>& args) -> Value { return Value(BaseNum(args[0].asBigInt(), static_cast<int>(std::round(args[1].asDouble())))); });
    reg("data", { 1 }, [](const std::vector<Value>& args) -> Value { return Value(args[0].asBigInt()); });

    reg("bitand", { 2 }, [](const std::vector<Value>& args) -> Value { 
        if (args[0].isObjType(ObjType::SET) || args[1].isObjType(ObjType::SET)) throw std::runtime_error("Type Error: bitand() does not support Sets.");
        return args[0] & args[1]; 
    });
    reg("bitor", { 2 }, [](const std::vector<Value>& args) -> Value { 
        if (args[0].isObjType(ObjType::SET) || args[1].isObjType(ObjType::SET)) throw std::runtime_error("Type Error: bitor() does not support Sets.");
        return args[0] | args[1]; 
    });
    reg("bitxor", { 2 }, [](const std::vector<Value>& args) -> Value { 
        if (args[0].isObjType(ObjType::SET) || args[1].isObjType(ObjType::SET)) throw std::runtime_error("Type Error: bitxor() does not support Sets.");
        return bitXor(args[0], args[1]); 
    });
    
    reg("bitnot", { 1, 2 }, [](const std::vector<Value>& args) -> Value { 
        int width = -1;
        if (args.size() == 2) {
            width = static_cast<int>(std::round(args[1].asDouble()));
            if (width <= 0) throw std::runtime_error("Math Error: Bit width must be positive.");
        }
        const Value& a = args[0];
        if (width > 0) {
            if (a.isObjType(ObjType::BASENUM)) {
                auto& base = static_cast<ObjBaseNum*>(a.asObj())->base;
                return Value(base.bitNot(width));
            }
            BaseNum base(a.asBigInt(), 2);
            return Value(base.bitNot(width).getValue());
        } else {
            if (a.isInt32()) return Value::fromInt32(~a.asInt32());
            if (a.isObjType(ObjType::BASENUM)) {
                auto& base = static_cast<ObjBaseNum*>(a.asObj())->base;
                return Value(BaseNum(-base.getValue() - BigInt(1), base.getRadix()));
            }
            return Value(-a.asBigInt() - BigInt(1));
        }
    });
    
    reg("bitshift", { 2 }, [](const std::vector<Value>& args) -> Value { 
        int shift = static_cast<int>(std::round(args[1].asDouble())); 
        const Value& a = args[0];
        if (a.isObjType(ObjType::BASENUM)) {
            auto& base = static_cast<ObjBaseNum*>(a.asObj())->base;
            return Value(shift > 0 ? base.shiftLeft(shift) : base.shiftRight(-shift));
        }
        if (a.isInt32()) {
            int32_t v = a.asInt32();
            if (shift > 0) {
                if (v == 0) return Value::fromInt32(0);
                if (shift < 31) {
                    int64_t res = static_cast<int64_t>(v) << shift;
                    if (res >= -2147483648LL && res <= 2147483647LL) {
                        return Value::fromInt32(static_cast<int32_t>(res));
                    }
                }
            } else {
                int rshift = -shift;
                if (rshift < 31) return Value::fromInt32(v >> rshift);
                return Value::fromInt32(v < 0 ? -1 : 0);
            }
        }
        bool lhsIsInt = a.isInt32() || a.isBigInt() || a.isBool();
        if (lhsIsInt) {
            BigInt lVal = a.isBool() ? BigInt(a.asBool() ? 1 : 0) : a.asBigInt();
            if (shift > 0) {
                return Value(lVal * BigInt(2).pow(shift));
            } else {
                int rshift = -shift;
                BigInt divisor = BigInt(2).pow(rshift);
                BigInt res = lVal / divisor;
                if (lVal.isNegative() && !(lVal % divisor).isZero()) {
                    res = res - BigInt(1);
                }
                return Value(res);
            }
        }
        throw std::runtime_error("Type Error: bitshift() not supported for these types.");
    });
}

// =================================================================
// [12] 统计（用静态 helper，无需 this）
// =================================================================
void BuiltinRegistry::registerStatistics() {
    reg("mean", { 1 }, [](const std::vector<Value>& args) -> Value { auto d = extractDS(args[0], "mean"); return Value(computeMean(d)); });
    reg("var", { 1 }, [](const std::vector<Value>& args) -> Value { auto d = extractDS(args[0], "var"); return Value(computeVar(d)); });
    reg("svar", { 1 }, [](const std::vector<Value>& args) -> Value { auto d = extractDS(args[0], "svar"); if (d.size()<2) throw std::runtime_error("Math Error: Sample variance requires at least 2 data points."); return Value(computeSvar(d)); });
    reg("std", { 1 }, [](const std::vector<Value>& args) -> Value { auto d = extractDS(args[0], "std"); return Value(computeStd(d)); });
    reg("sstd", { 1 }, [](const std::vector<Value>& args) -> Value { auto d = extractDS(args[0], "sstd"); if (d.size()<2) throw std::runtime_error("Math Error: Sample std requires at least 2 data points."); return Value(std::sqrt(computeSvar(d))); });
    reg("max", { 1 }, [this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        Value mx;
        GcValueGuard mxGuard(mx);
        bool first = true;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (first) { mx = nextVal; first = false; }
            else if (helpers::checkGreater(nextVal, mx)) mx = nextVal;
            return true;
        })) {
            if (first) throw std::runtime_error("Math Error: Cannot compute max of empty iterable.");
            return mx;
        }
        if (arg.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(arg.asObj())->vec;
            if (L.empty()) throw std::runtime_error("Math Error: Cannot compute max of empty list.");
            Value listMx = L[0];
            GcValueGuard listMxGuard(listMx);
            for (size_t i = 1; i < L.size(); ++i) {
                Value v = L[i];
                // ★ 正确的 C++ 比较
                if (helpers::checkGreater(v, listMx)) listMx = v;
            }
            return listMx;
        }
        auto d = extractDS(arg, "max");
        double mx_d = d[0]; for (double v : d) if (v > mx_d) mx_d = v; return Value(mx_d);
        });

    reg("min", { 1 }, [this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        Value mn;
        GcValueGuard mnGuard(mn);
        bool first = true;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (first) { mn = nextVal; first = false; }
            else if (helpers::checkLess(nextVal, mn)) mn = nextVal;
            return true;
        })) {
            if (first) throw std::runtime_error("Math Error: Cannot compute min of empty iterable.");
            return mn;
        }
        if (arg.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(arg.asObj())->vec;
            if (L.empty()) throw std::runtime_error("Math Error: Cannot compute min of empty list.");
            Value listMn = L[0];
            GcValueGuard listMnGuard(listMn);
            for (size_t i = 1; i < L.size(); ++i) {
                Value v = L[i];
                // ★ 正确的 C++ 比较
                if (helpers::checkLess(v, listMn)) listMn = v;
            }
            return listMn;
        }
        auto d = extractDS(arg, "min");
        double mn_d = d[0]; for (double v : d) if (v < mn_d) mn_d = v; return Value(mn_d);
        });

    reg("span", { 1 }, [this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        Value mn, mx;
        GcValueGuard mnGuard(mn);
        GcValueGuard mxGuard(mx);
        bool first = true;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (first) { mn = nextVal; mx = nextVal; first = false; }
            else {
                if (helpers::checkGreater(nextVal, mx)) mx = nextVal;
                if (helpers::checkLess(nextVal, mn)) mn = nextVal;
            }
            return true;
        })) {
            if (first) throw std::runtime_error("Math Error: Cannot compute span of empty iterable.");
            return mx - mn;
        }
        if (arg.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(arg.asObj())->vec;
            if (L.empty()) throw std::runtime_error("Math Error: Cannot compute span of empty list.");
            Value listMn = L[0];
            Value listMx = L[0];
            GcValueGuard listMnGuard(listMn);
            GcValueGuard listMxGuard(listMx);
            for (size_t i = 1; i < L.size(); ++i) {
                Value v = L[i];
                // ★ 正确的 C++ 比较
                if (helpers::checkGreater(v, listMx)) listMx = v;
                if (helpers::checkLess(v, listMn)) listMn = v;
            }
            // 利用 Value 的重载减法返回 span
            return listMx - listMn;
        }
        auto d = extractDS(arg, "span");
        double mx_d = d[0], mn_d = d[0];
        for (double v : d) { if (v > mx_d) mx_d = v; if (v < mn_d) mn_d = v; }
        return Value(mx_d - mn_d);
        });

    reg("perc", { 2 }, [](const std::vector<Value>& args) -> Value {
        auto d = extractDS(args[0], "perc");
        if (d.empty()) throw std::runtime_error("Math Error: Cannot compute percentile of empty dataset.");
        double p = args[1].asDouble();
        if (p<0||p>100) throw std::runtime_error("Math Error: Percentile must be [0,100].");
        std::sort(d.begin(), d.end());
        int n = static_cast<int>(d.size());
        double pos = (p/100.0) * n;
        int kk = static_cast<int>(std::ceil(pos));
        if (kk <= 0) return Value(d[0]);
        if (kk >= n) return Value(d[n-1]);
        int m = kk - 1;
        double f = pos - kk;
        if (std::abs(f) > 1e-9) return Value(d[m]);
        if (kk < n) return Value((d[m] + d[kk]) / 2.0);
        return Value(d[m]);
    });
    reg("median", { 1 }, [this](const std::vector<Value>& args) -> Value { return builtins["perc"]({ args[0], Value(50.0) }); });

    reg("mode", { 1 }, [](const std::vector<Value>& args) -> Value {
        auto d = extractDS(args[0], "mode");
        if (d.empty()) throw std::runtime_error("Math Error: Cannot compute mode of empty dataset.");
        struct Bucket { double representative; int count; };
        std::vector<Bucket> buckets;
        for (double v : d) { bool found = false; for (auto& bkt : buckets) { if (v == bkt.representative) { bkt.count++; found = true; break; } } if (!found) buckets.push_back({ v, 1 }); }
        int mx = 0; for (const auto& bkt : buckets) if (bkt.count > mx) mx = bkt.count;
        std::vector<double> modes; for (const auto& bkt : buckets) if (bkt.count == mx) modes.push_back(bkt.representative);
        std::sort(modes.begin(), modes.end());
        if (modes.size() == 1) return Value(modes[0]);
        return Value(RealMatrix(1, static_cast<int>(modes.size()), modes));
    });

    reg("cov", { 2 }, [](const std::vector<Value>& args) -> Value { auto X = extractDS(args[0], "cov"), Y = extractDS(args[1], "cov"); if (X.size()!=Y.size()) throw std::runtime_error("Math Error: Size mismatch."); return Value(computeCov(X, Y)); });
    reg("corr", { 2 }, [](const std::vector<Value>& args) -> Value { auto X = extractDS(args[0], "corr"), Y = extractDS(args[1], "corr"); if (X.size()!=Y.size()) throw std::runtime_error("Math Error: Size mismatch."); return Value(computeCorr(X, Y)); });
    reg("rsq", { 2 }, [](const std::vector<Value>& args) -> Value { auto X = extractDS(args[0], "rsq"), Y = extractDS(args[1], "rsq"); if (X.size()!=Y.size()) throw std::runtime_error("Math Error: Size mismatch."); double r = computeCorr(X, Y); return Value(r * r); });
    reg("regress", { 2 }, [](const std::vector<Value>& args) -> Value {
        auto X = extractDS(args[0], "regress"), Y = extractDS(args[1], "regress");
        if (X.size()!=Y.size()) throw std::runtime_error("Math Error: Size mismatch.");
        double vX = computeVar(X);
        if (vX == 0.0) throw std::runtime_error("Math Error: Zero variance in X.");
        double c = computeCov(X, Y);
        double b = c / vX, a = computeMean(Y) - b * computeMean(X);
        std::cout << "Linear Model: Y = " << a << " + " << b << " * X" << std::endl;
        std::cout << "Correlation r: " << computeCorr(X, Y) << std::endl;
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        L->vec.push_back(Value(a)); L->vec.push_back(Value(b));
        L->is_frozen = true; return Value(L);
    });
}

// =================================================================
// [13] 随机数
// =================================================================
void BuiltinRegistry::registerRandom() {
    reg("rand", { 0, 2 }, [](const std::vector<Value>& args) -> Value { static std::mt19937 gen(std::random_device{}()); if (args.size()==0) return Value(std::uniform_real_distribution<double>(0,1)(gen)); double lo = args[0].asDouble(), hi = args[1].asDouble(); return Value(std::uniform_real_distribution<double>(lo, hi)(gen)); });
    reg("randint", { 2 }, [](const std::vector<Value>& args) -> Value { static std::mt19937 gen(std::random_device{}()); return Value::fromInt32(std::uniform_int_distribution<int>(static_cast<int>(std::round(args[0].asDouble())), static_cast<int>(std::round(args[1].asDouble())))(gen)); });
    reg("randc", { 0, 2 }, [](const std::vector<Value>& args) -> Value { static std::mt19937 gen(std::random_device{}()); double lo=0,hi=1; if (args.size()==2){lo=args[0].asDouble();hi=args[1].asDouble();} std::uniform_real_distribution<double> dist(lo,hi); return Value(Complex(dist(gen),dist(gen))); });
    reg("randmat", { 2, 4 }, [](const std::vector<Value>& args) -> Value { static std::mt19937 gen(std::random_device{}()); int r,c; double lo=0,hi=1; if (args.size()==2){r=static_cast<int>(std::round(args[0].asDouble()));c=static_cast<int>(std::round(args[1].asDouble()));} else {r=static_cast<int>(std::round(args[0].asDouble()));c=static_cast<int>(std::round(args[1].asDouble()));lo=args[2].asDouble();hi=args[3].asDouble();} std::uniform_real_distribution<double> dist(lo,hi); std::vector<double> d(r*c); for (auto& v:d) v=dist(gen); return Value(RealMatrix(r,c,d)); });
    reg("randimat", { 2, 4 }, [](const std::vector<Value>& args) -> Value { static std::mt19937 gen(std::random_device{}()); int r,c,lo=0,hi=10; if (args.size()==2){r=static_cast<int>(std::round(args[0].asDouble()));c=static_cast<int>(std::round(args[1].asDouble()));} else {r=static_cast<int>(std::round(args[0].asDouble()));c=static_cast<int>(std::round(args[1].asDouble()));lo=static_cast<int>(std::round(args[2].asDouble()));hi=static_cast<int>(std::round(args[3].asDouble()));} std::uniform_int_distribution<int> dist(lo,hi); std::vector<double> d(r*c); for (auto& v:d) v=static_cast<double>(dist(gen)); return Value(RealMatrix(r,c,d)); });
    reg("randcmat", { 2, 4 }, [](const std::vector<Value>& args) -> Value { static std::mt19937 gen(std::random_device{}()); int r,c; double lo=0,hi=1; if (args.size()==2){r=static_cast<int>(std::round(args[0].asDouble()));c=static_cast<int>(std::round(args[1].asDouble()));} else {r=static_cast<int>(std::round(args[0].asDouble()));c=static_cast<int>(std::round(args[1].asDouble()));lo=args[2].asDouble();hi=args[3].asDouble();} std::uniform_real_distribution<double> dist(lo,hi); std::vector<Complex> d(r*c); for (auto& v:d) v=Complex(dist(gen),dist(gen)); return Value(ComplexMatrix(r,c,d)); });
    reg("magic", { 1 }, [](const std::vector<Value>& args) -> Value { return Value(RealMatrix::magic(static_cast<int>(std::round(args[0].asDouble())))); });
}

// =================================================================
// [14] 系统工具（无状态部分）
// =================================================================
void BuiltinRegistry::registerSystemUtils() {
    reg("buildIndex", { 0 }, [](const std::vector<Value>&) -> Value { BigInt::buildFileIndex(); return Value::none(); });
    reg("loadPrimes", builtinArity["buildIndex"], builtins["buildIndex"]);
    reg("mountPrimes", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Runtime Error: mountPrimes(\"path\") expects a string."); BigInt::setPrimeFilePath(args[0].asString()); return Value::none(); });
    reg("extendPrimes", { 1 }, [](const std::vector<Value>& args) -> Value { int64_t count = static_cast<int64_t>(std::round(args[0].asDouble())); if (count <= 0) throw std::runtime_error("Runtime Error: count must be positive."); BigInt::extendPrimeTable(count); return Value::none(); });
    reg("convertPrimes", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString() || !args[1].isString()) throw std::runtime_error("Type Error: convertPrimes() expects two strings (txtPath, binPath)."); BigInt::convertTxtToJCP1(helpers::safeResolvePath(args[0].asString()), helpers::safeResolvePath(args[1].asString())); return Value::none(); });
    reg("verifyPrimes", { 0 }, [](const std::vector<Value>&) -> Value { return Value(BigInt::verifyPrimeTable()); });
    reg("sysinfo", { 0 }, [](const std::vector<Value>&) -> Value { std::cout << "--- Junk Calculator System Info ---\n" << "Prime DB: " << (BigInt::getPrimeFilePath().empty() ? "(Dynamic Computation)" : BigInt::getPrimeFilePath()) << "\n" << "Format:   " << (BigInt::getPrimeFilePath().empty() ? "None" : "JCP1 (Block-Differential)") << "\n" << "Mounted:  " << BigInt::totalPrimesInFile << " primes\n"; if (BigInt::totalPrimesInFile > 0) std::cout << "Max:      " << BigInt::largestPrimeInFile << "\n"; std::cout << "-----------------------------------" << std::endl; return Value::none(); });

    reg("gensym", { 0, 1 }, [](const std::vector<Value>& args) -> Value {
        static uint64_t counter = 0;
        std::string prefix = "gensym";
        if (args.size() == 1 && args[0].isString()) {
            prefix = args[0].asString();
        }
        // ★ 使用尖括号包裹，生成词法上“非法”但 VM 合法的绝对安全标识符
        std::string uniqueName = "<" + prefix + "_" + std::to_string(counter++) + ">";
        
        auto clsVal = VM::activeVM->getBuiltinValue("ASTNode");
        if (!clsVal.isClass()) throw std::runtime_error("ASTNode class not found");
        
        ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
        GcObjGuard instGuard(inst);
        inst->classDef = static_cast<ObjClass*>(clsVal.asObj());
        inst->fields = GcHeap::get().allocate<ObjDict>();
        
        inst->fields->set(Value("type"), Value("Variable"));
        inst->fields->set(Value("line"), Value::fromInt32(0));
        inst->fields->set(Value("name"), Value(uniqueName));
        
        return Value(inst);
    });
    reg("gc", { 0, 1 }, [](const std::vector<Value>& args) -> Value {
        // 1. 清理符号表达式的弱引用池
        jc::SymExpr::cleanupPool();

        if (!VM::activeVM) return Value(0.0);

        // ★ gc(true) = 激进模式：先清掉 ANS 避免它充当隐形保护伞
        bool aggressive = (args.size() == 1 && args[0].truthy());
        if (aggressive) {
            VM::activeVM->setGlobal("ANS", Value::none());
        }

        if (GcHeap::get().markCallback) GcHeap::get().markCallback();
        if (GcHeap::get().sweepCallback) GcHeap::get().sweepCallback();
        int freed = GcHeap::get().sweep();

        std::cout << "[GC] Collected " << freed << " unreachable object(s). "
            << "Tracked: " << GcHeap::get().trackedCount() << std::endl;
        return Value::fromInt32(freed);
        });

    reg("gcinfo", { 0 }, [](const std::vector<Value>&) -> Value {
        auto& heap = GcHeap::get();
        std::cout << "--- GC Status ---\n"
            << "  Tracked objects:     " << heap.trackedCount() << "\n"
            << "  Allocs since GC:     " << heap.allocsSinceGc() << "\n"
            << "  Next GC threshold:   " << heap.threshold() << "\n"
            << "-----------------" << std::endl;
        return Value::none();
        });

    reg("freeze", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            static_cast<ObjList*>(args[0].asObj())->is_frozen = true;
        } else if (args[0].isObjType(ObjType::DICT)) {
            static_cast<ObjDict*>(args[0].asObj())->is_frozen = true;
        } else if (args[0].isObjType(ObjType::SET)) {
            static_cast<ObjSet*>(args[0].asObj())->is_frozen = true;
        } else if (args[0].isInstance()) {
            args[0].asInstance()->is_frozen = true;
            if (args[0].asInstance()->fields) args[0].asInstance()->fields->is_frozen = true;
        } else if (args[0].isObjType(ObjType::NAMESPACE)) {
            static_cast<ObjNamespace*>(args[0].asObj())->is_frozen = true;
        }
        return args[0];
        });

    reg("isFrozen", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            return Value(static_cast<ObjList*>(args[0].asObj())->is_frozen);
        } else if (args[0].isObjType(ObjType::DICT)) {
            return Value(static_cast<ObjDict*>(args[0].asObj())->is_frozen);
        } else if (args[0].isObjType(ObjType::SET)) {
            return Value(static_cast<ObjSet*>(args[0].asObj())->is_frozen);
        } else if (args[0].isInstance()) {
            return Value(args[0].asInstance()->is_frozen);
        } else if (args[0].isObjType(ObjType::NAMESPACE)) {
            return Value(static_cast<ObjNamespace*>(args[0].asObj())->is_frozen);
        }
        return Value(false);
        });

    reg("hash", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isHashable()) throw std::runtime_error("TypeError: unhashable type.");
        size_t h = jc::ValueHasher{}(args[0]);
        return Value(BigInt(static_cast<int64_t>(h)));
        });

    reg("clone", { 1 }, [](const std::vector<Value>& args) -> Value {
        std::map<const void*, Value> visited;
        std::function<Value(const Value&)> deepCopy = [&](const Value& v) -> Value {
            if (v.isObjType(ObjType::LIST)) {
                auto l = static_cast<ObjList*>(v.asObj());
                if (visited.count(l)) return visited[l];
                ObjList* newList = GcHeap::get().allocate<ObjList>();
                GcObjGuard guard(newList);
                Value newVal(newList);
                visited[l] = newVal;
                for (const auto& e : l->vec) {
                    newList->vec.push_back(deepCopy(e));
                }
                return newVal;
            }
            if (v.isObjType(ObjType::DICT)) {
                auto d = static_cast<ObjDict*>(v.asObj());
                if (visited.count(d)) return visited[d];
                ObjDict* newDict = GcHeap::get().allocate<ObjDict>();
                GcObjGuard guard(newDict);
                Value newVal(newDict);
                visited[d] = newVal;
                for (const auto& [k, val] : d->elements) {
                    Value newK = deepCopy(k);
                    Value newV = deepCopy(val);
                    newDict->keyMap[newK] = newDict->elements.size();
                    newDict->elements.push_back({newK, newV});
                }
                return newVal;
            }
            if (v.isObjType(ObjType::SET)) {
                auto s = static_cast<ObjSet*>(v.asObj());
                if (visited.count(s)) return visited[s];
                ObjSet* newSet = GcHeap::get().allocate<ObjSet>();
                GcObjGuard guard(newSet);
                Value newVal(newSet);
                visited[s] = newVal;
                for (const auto& val : s->elements) {
                    Value newV = deepCopy(val);
                    newSet->keys.insert(newV);
                    newSet->elements.push_back(newV);
                }
                return newVal;
            }
            if (v.isInstance()) {
                auto inst = v.asInstance();
                if (visited.count(inst)) return visited[inst];
                ObjInstance* newInst = GcHeap::get().allocate<ObjInstance>();
                GcObjGuard guard(newInst);
                newInst->classDef = inst->classDef;
                newInst->nativeData = inst->nativeData;
                Value newVal(newInst);
                visited[inst] = newVal;
                if (inst->fields) {
                    newInst->fields = static_cast<ObjDict*>(deepCopy(Value(inst->fields)).asObj());
                }
                return newVal;
            }
            if (v.isObjType(ObjType::NAMESPACE)) {
                auto ns = static_cast<ObjNamespace*>(v.asObj());
                if (visited.count(ns)) return visited[ns];
                ObjNamespace* newNs = GcHeap::get().allocate<ObjNamespace>();
                GcObjGuard guard(newNs);
                newNs->name = ns->name;
                Value newVal(newNs);
                visited[ns] = newVal;
                for (const auto& [k, field] : ns->fields) {
                    auto uv = GcHeap::get().allocate<ObjUpVal>();
                    uv->closed = deepCopy(*(field.upval->location));
                    uv->location = &uv->closed;
                    newNs->fields[k] = { uv, field.isConst };
                }
                return newVal;
            }
            return v;
        };
        return deepCopy(args[0]);
        });

    reg("copy", { 1 }, [](const std::vector<Value>& args) -> Value {
        std::map<const void*, Value> visited;
        std::function<Value(const Value&)> deepCopyExact = [&](const Value& v) -> Value {
            if (v.isObjType(ObjType::LIST)) {
                auto l = static_cast<ObjList*>(v.asObj());
                if (visited.count(l)) return visited[l];
                ObjList* newList = GcHeap::get().allocate<ObjList>();
                GcObjGuard guard(newList);
                Value newVal(newList);
                visited[l] = newVal;
                for (const auto& e : l->vec) {
                    newList->vec.push_back(deepCopyExact(e));
                }
                newList->is_frozen = l->is_frozen;
                newList->is_hashable_cached = l->is_hashable_cached;
                return newVal;
            }
            if (v.isObjType(ObjType::DICT)) {
                auto d = static_cast<ObjDict*>(v.asObj());
                if (visited.count(d)) return visited[d];
                ObjDict* newDict = GcHeap::get().allocate<ObjDict>();
                GcObjGuard guard(newDict);
                Value newVal(newDict);
                visited[d] = newVal;
                for (const auto& [k, val] : d->elements) {
                    Value newK = deepCopyExact(k);
                    Value newV = deepCopyExact(val);
                    newDict->keyMap[newK] = newDict->elements.size();
                    newDict->elements.push_back({newK, newV});
                }
                newDict->is_frozen = d->is_frozen;
                newDict->is_hashable_cached = d->is_hashable_cached;
                return newVal;
            }
            if (v.isObjType(ObjType::SET)) {
                auto s = static_cast<ObjSet*>(v.asObj());
                if (visited.count(s)) return visited[s];
                ObjSet* newSet = GcHeap::get().allocate<ObjSet>();
                GcObjGuard guard(newSet);
                Value newVal(newSet);
                visited[s] = newVal;
                for (const auto& val : s->elements) {
                    Value newV = deepCopyExact(val);
                    newSet->keys.insert(newV);
                    newSet->elements.push_back(newV);
                }
                newSet->is_frozen = s->is_frozen;
                newSet->is_hashable_cached = s->is_hashable_cached;
                return newVal;
            }
            if (v.isInstance()) {
                auto inst = v.asInstance();
                if (visited.count(inst)) return visited[inst];
                ObjInstance* newInst = GcHeap::get().allocate<ObjInstance>();
                GcObjGuard guard(newInst);
                newInst->classDef = inst->classDef;
                newInst->nativeData = inst->nativeData;
                Value newVal(newInst);
                visited[inst] = newVal;
                if (inst->fields) {
                    newInst->fields = static_cast<ObjDict*>(deepCopyExact(Value(inst->fields)).asObj());
                }
                newInst->is_frozen = inst->is_frozen;
                newInst->is_hashable_cached = inst->is_hashable_cached;
                return newVal;
            }
            if (v.isObjType(ObjType::NAMESPACE)) {
                auto ns = static_cast<ObjNamespace*>(v.asObj());
                if (visited.count(ns)) return visited[ns];
                ObjNamespace* newNs = GcHeap::get().allocate<ObjNamespace>();
                GcObjGuard guard(newNs);
                newNs->name = ns->name;
                Value newVal(newNs);
                visited[ns] = newVal;
                for (const auto& [k, field] : ns->fields) {
                    auto uv = GcHeap::get().allocate<ObjUpVal>();
                    uv->closed = deepCopyExact(*(field.upval->location));
                    uv->location = &uv->closed;
                    newNs->fields[k] = { uv, field.isConst };
                }
                newNs->is_frozen = ns->is_frozen;
                return newVal;
            }
            return v;
        };
        return deepCopyExact(args[0]);
        });

    reg("val", { 1 }, [](const std::vector<Value>& args) -> Value {
        std::map<const void*, Value> visited;
        std::function<Value(const Value&)> deepCopyAndFreeze = [&](const Value& v) -> Value {
            if (v.isObjType(ObjType::LIST)) {
                auto l = static_cast<ObjList*>(v.asObj());
                if (visited.count(l)) return visited[l];
                ObjList* newList = GcHeap::get().allocate<ObjList>();
                GcObjGuard guard(newList);
                Value newVal(newList);
                visited[l] = newVal;
                for (const auto& e : l->vec) {
                    newList->vec.push_back(deepCopyAndFreeze(e));
                }
                newList->is_frozen = true;
                newList->is_hashable_cached = true;
                return newVal;
            }
            if (v.isObjType(ObjType::DICT)) {
                auto d = static_cast<ObjDict*>(v.asObj());
                if (visited.count(d)) return visited[d];
                ObjDict* newDict = GcHeap::get().allocate<ObjDict>();
                GcObjGuard guard(newDict);
                Value newVal(newDict);
                visited[d] = newVal;
                for (const auto& [k, val] : d->elements) {
                    Value newK = deepCopyAndFreeze(k);
                    Value newV = deepCopyAndFreeze(val);
                    newDict->keyMap[newK] = newDict->elements.size();
                    newDict->elements.push_back({newK, newV});
                }
                newDict->is_frozen = true;
                newDict->is_hashable_cached = true;
                return newVal;
            }
            if (v.isObjType(ObjType::SET)) {
                auto s = static_cast<ObjSet*>(v.asObj());
                if (visited.count(s)) return visited[s];
                ObjSet* newSet = GcHeap::get().allocate<ObjSet>();
                GcObjGuard guard(newSet);
                Value newVal(newSet);
                visited[s] = newVal;
                for (const auto& val : s->elements) {
                    Value newV = deepCopyAndFreeze(val);
                    newSet->keys.insert(newV);
                    newSet->elements.push_back(newV);
                }
                newSet->is_frozen = true;
                newSet->is_hashable_cached = true;
                return newVal;
            }
            if (v.isInstance()) {
                auto inst = v.asInstance();
                if (visited.count(inst)) return visited[inst];
                ObjInstance* newInst = GcHeap::get().allocate<ObjInstance>();
                GcObjGuard guard(newInst);
                newInst->classDef = inst->classDef;
                newInst->nativeData = inst->nativeData;
                Value newVal(newInst);
                visited[inst] = newVal;
                if (inst->fields) {
                    newInst->fields = static_cast<ObjDict*>(deepCopyAndFreeze(Value(inst->fields)).asObj());
                }
                newInst->is_frozen = true;
                newInst->is_hashable_cached = true;
                return newVal;
            }
            if (v.isObjType(ObjType::NAMESPACE)) {
                auto ns = static_cast<ObjNamespace*>(v.asObj());
                if (visited.count(ns)) return visited[ns];
                ObjNamespace* newNs = GcHeap::get().allocate<ObjNamespace>();
                GcObjGuard guard(newNs);
                newNs->name = ns->name;
                Value newVal(newNs);
                visited[ns] = newVal;
                for (const auto& [k, field] : ns->fields) {
                    auto uv = GcHeap::get().allocate<ObjUpVal>();
                    uv->closed = deepCopyAndFreeze(*(field.upval->location));
                    uv->location = &uv->closed;
                    newNs->fields[k] = { uv, field.isConst };
                }
                newNs->is_frozen = true;
                return newVal;
            }
            return v;
        };
        return deepCopyAndFreeze(args[0]);
        });

    reg("symconfig", { 0, 1 }, [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            ObjDict* d = GcHeap::get().allocate<ObjDict>();
            GcObjGuard guard(d);
            auto setField = [&](const std::string& k, Value v) {
                Value key(k);
                d->keyMap[key] = d->elements.size();
                d->elements.push_back({key, v});
            };
            setField("maxExpandTerms", Value(BigInt(SymConfig::maxExpandTerms)));
            setField("maxAstNodes", Value::fromInt32(SymConfig::maxAstNodes));
            setField("maxIterations", Value::fromInt32(SymConfig::maxIterations));
            setField("maxDepth", Value::fromInt32(SymConfig::maxDepth));
            setField("debugIntegration", Value(SymConfig::debugIntegration));
            return Value(d);
        }
        if (args[0].isString() && args[0].asString() == "default") {
            SymConfig::maxExpandTerms = 2000;
            SymConfig::maxAstNodes = 30000;
            SymConfig::maxIterations = 200;
            SymConfig::maxDepth = 6;
            SymConfig::debugIntegration = false;
            return Value::none();
        }
        if (!args[0].isObjType(ObjType::DICT)) {
            throw std::runtime_error("Type Error: symconfig() expects a Dict or \"default\".");
        }
        auto d = static_cast<ObjDict*>(args[0].asObj());
        auto getField = [&](const std::string& k) -> Value* {
            auto it = d->keyMap.find(Value(k));
            if (it != d->keyMap.end()) return &d->elements[it->second].second;
            return nullptr;
        };
        if (auto v = getField("maxExpandTerms")) SymConfig::maxExpandTerms = static_cast<int64_t>(v->asDouble());
        if (auto v = getField("maxAstNodes")) SymConfig::maxAstNodes = static_cast<int>(v->asDouble());
        if (auto v = getField("maxIterations")) SymConfig::maxIterations = static_cast<int>(v->asDouble());
        if (auto v = getField("maxDepth")) SymConfig::maxDepth = static_cast<int>(v->asDouble());
        if (auto v = getField("debugIntegration")) SymConfig::debugIntegration = v->truthy();
        return Value::none();
        });

    reg("setSymLimit", { 1, 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: setSymLimit() expects a string key.");
        std::string key = args[0].asString();
        
        if (args.size() == 1) {
            if (key == "default") {
                SymConfig::maxExpandTerms = 2000;
                SymConfig::maxAstNodes = 30000;
                SymConfig::maxIterations = 200;
                SymConfig::maxDepth = 6;
                SymConfig::debugIntegration = false;
                return Value::none();
            }
            throw std::runtime_error("Runtime Error: setSymLimit() expects 2 arguments unless resetting with \"default\".");
        }

        if (args[1].isString() && args[1].asString() == "default") {
            if (key == "maxExpandTerms") SymConfig::maxExpandTerms = 5000;
            else if (key == "maxAstNodes") SymConfig::maxAstNodes = 50000;
            else if (key == "maxIterations") SymConfig::maxIterations = 1000;
            else if (key == "maxDepth") SymConfig::maxDepth = 20;
            else if (key == "debugIntegration") SymConfig::debugIntegration = false;
            else throw std::runtime_error("Runtime Error: Unknown SymConfig key '" + key + "'.");
            return Value::none();
        }

        if (key == "debugIntegration") {
            SymConfig::debugIntegration = args[1].truthy();
            return Value::none();
        }

        double val = args[1].asDouble();
        if (key == "maxExpandTerms") SymConfig::maxExpandTerms = static_cast<int64_t>(val);
        else if (key == "maxAstNodes") SymConfig::maxAstNodes = static_cast<int>(val);
        else if (key == "maxIterations") SymConfig::maxIterations = static_cast<int>(val);
        else if (key == "maxDepth") SymConfig::maxDepth = static_cast<int>(val);
        else throw std::runtime_error("Runtime Error: Unknown SymConfig key '" + key + "'.");
        return Value::none();
        });

    reg("__register_help", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString() || !args[1].isString()) {
            throw std::runtime_error("System Error: __register_help expects two strings.");
        }
        std::string topic = args[0].asString();
        std::string text = args[1].asString();
        jc::DynamicHelp[topic] = text; // 存入 C++ 内存池
        return Value::none();
        });
    // ★ 暴露给用户的原生 help() 内置函数
    reg("help", { 0, 1 }, [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            jc::HelpRouter::printMainHelp();
            return Value::none();
        }

        if (!args[0].isString())
            throw std::runtime_error("Type Error: help() expects a string topic.");

        std::string topic = args[0].asString();
        jc::HelpRouter::printHelpTopic(topic);
        return Value::none();
        });
}

// =================================================================
// [15] 控制流辅助（无状态部分）
// =================================================================
void BuiltinRegistry::registerControlFlow() {
    reg("print", {}, [](const std::vector<Value>& args) -> Value {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) std::cout << " ";
            // ★ Dunder 钩子: __str__
            if (args[i].isInstance()) {
                auto inst = args[i].asInstance();
                auto [found, result] = invokeDunder(inst, DUNDER_STR, {});
                if (found) { std::cout << result; continue; }
            }
            std::cout << args[i];
        }
        std::cout << std::flush; return Value::none();
        });
    reg("println", {}, [](const std::vector<Value>& args) -> Value {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) std::cout << " ";
            // ★ Dunder 钩子: __str__
            if (args[i].isInstance()) {
                auto inst = args[i].asInstance();
                auto [found, result] = invokeDunder(inst, DUNDER_STR, {});
                if (found) { std::cout << result; continue; }
            }
            std::cout << args[i];
        }
        std::cout << std::endl; return Value::none();
        });
    reg("bool", { 1 }, [](const std::vector<Value>& args) -> Value {
        // ★ Dunder 钩子: __bool__
        if (args[0].isInstance()) {
            auto inst = args[0].asInstance();
            auto [found, result] = invokeDunder(inst, DUNDER_BOOL, {});
            if (found) return Value(result.truthy());
        }
        return Value(args[0].truthy());
        });
    reg("not", { 1 }, [](const std::vector<Value>& args) -> Value { return Value(!args[0].truthy()); });
    reg("and", { 2 }, [](const std::vector<Value>& args) -> Value { return Value(args[0].truthy() && args[1].truthy()); });
    reg("or", { 2 }, [](const std::vector<Value>& args) -> Value { return Value(args[0].truthy() || args[1].truthy()); });

    reg("seq", { 2, 3 }, [](const std::vector<Value>& args) -> Value {
        double start, step, end;
        if (args.size()==2) { start=args[0].asDouble(); end=args[1].asDouble(); step=(start<=end)?1.0:-1.0; }
        else { start=args[0].asDouble(); step=args[1].asDouble(); end=args[2].asDouble(); }
        if (step == 0.0) throw std::runtime_error("Math Error: Step cannot be zero.");
        std::vector<double> vals;
        if (step>0) { for (double v=start; v<=end+Tol::EPS*100; v+=step) { jc::checkInterrupt(); vals.push_back(v); } }
        else { for (double v=start; v>=end-Tol::EPS*100; v+=step) { jc::checkInterrupt(); vals.push_back(v); } }
        if (vals.empty()) throw std::runtime_error("Math Error: seq() produced empty sequence.");
        return Value(RealMatrix(static_cast<int>(vals.size()), 1, vals));
    });

    reg("error", { 1 }, [](const std::vector<Value>& args) -> Value {
        std::string msg;
        if (args[0].isString()) msg = args[0].asString();
        else { std::ostringstream oss; oss << args[0]; msg = oss.str(); }
        throw ErrorSignal{ msg };
    });
    reg("input", { 0, 1 }, [](const std::vector<Value>& args) -> Value {
        if (args.size()==1) { if (args[0].isString()) std::cout << args[0].asString(); else std::cout << args[0]; std::cout << std::flush; }
        std::string line;
        if (!std::getline(std::cin, line)) throw std::runtime_error("IO Error: Failed to read input.");
        return Value(line);
    });
    reg("clock", { 0 }, [](const std::vector<Value>&) -> Value {
        auto now = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        return Value(static_cast<double>(ms) / 1e6);
    });
    reg("sleep", { 1 }, [](const std::vector<Value>& args) -> Value {
        int ms = static_cast<int>(std::round(args[0].asDouble() * 1000));
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Value::none();
    });
    reg("highlight", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: highlight() expects a string.");
        return Value(jc::highlightCode(args[0].asString()));
    });
    reg("color", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: color() expects \"on\" or \"off\".");
        std::string arg = args[0].asString();
        if (arg=="on") jc::colorsEnabled = true; else if (arg=="off") jc::colorsEnabled = false;
        else throw std::runtime_error("Runtime Error: color() expects \"on\" or \"off\".");
        return Value::none();
    });
    reg("debugInteg", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: debugInteg() expects \"on\" or \"off\".");
        std::string arg = args[0].asString();
        if (arg=="on") jc::SymConfig::debugIntegration = true; else if (arg=="off") jc::SymConfig::debugIntegration = false;
        else throw std::runtime_error("Runtime Error: debugInteg() expects \"on\" or \"off\".");
        return Value::none();
    });

    // =================================================================
    // [大一统泛型 API] add / remove / discard / clear
    // =================================================================

    reg("add", { 2, 3 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::SET)) {
            if (args.size() != 2) throw std::runtime_error("Runtime Error: add() on Set takes 2 args (set, val).");
            auto s = static_cast<ObjSet*>(args[0].asObj());
            s->add(args[1]);
            return args[0];
        }
        else if (args[0].isObjType(ObjType::LIST)) {
            if (args.size() != 2) throw std::runtime_error("Runtime Error: add() on List takes 2 args (list, val).");
            auto l = static_cast<ObjList*>(args[0].asObj());
            l->mut().push_back(args[1]);
            return args[0];
        }
        else if (args[0].isObjType(ObjType::DICT) || args[0].isInstance()) {
            if (args.size() != 3) throw std::runtime_error("Runtime Error: add() on Dict/Instance takes 3 args (obj, key, val).");
            if (args[0].isInstance()) args[0].asInstance()->checkModify();
            auto d = args[0].isInstance() ? args[0].asInstance()->fields : static_cast<ObjDict*>(args[0].asObj());
            if (!d && args[0].isInstance()) {
                d = GcHeap::get().allocate<ObjDict>();
                args[0].asInstance()->fields = d;
            }
            d->set(args[1], args[2]);
            return args[0]; // 返回原对象
        }
        else if (args[0].isObjType(ObjType::NAMESPACE)) {
            if (args.size() != 3) throw std::runtime_error("Runtime Error: add() on Namespace takes 3 args (obj, key, val).");
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ns->checkModify();
            if (!args[1].isString()) throw std::runtime_error("Type Error: Namespace keys must be strings.");
            std::string key = args[1].asString();
            auto it = ns->fields.find(key);
            if (it != ns->fields.end()) {
                if (it->second.isConst) throw std::runtime_error("Runtime Error: Cannot modify const property '" + key + "'.");
                *(it->second.upval->location) = args[2];
            } else {
                auto uv = GcHeap::get().allocate<ObjUpVal>();
                uv->closed = args[2];
                uv->location = &uv->closed;
                ns->fields[key] = { uv, false };
            }
            return args[0];
        }
        throw std::runtime_error("Type Error: add() expects a Set, List, Dict, Instance, or Namespace.");
        });

    reg("remove", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::SET)) {
            auto s = static_cast<ObjSet*>(args[0].asObj());
            s->remove(args[1]);
            return args[0];
        }
        else if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            int idx = static_cast<int>(std::round(args[1].asDouble()));
            if (idx < 0) idx += static_cast<int>(l->vec.size());
            if (idx < 0 || idx >= static_cast<int>(l->vec.size())) throw std::runtime_error("Runtime Error: Index out of bounds.");
            l->mut().erase(l->mut().begin() + idx);
            return args[0];
        }
        else if (args[0].isObjType(ObjType::DICT) || args[0].isInstance()) {
            if (args[0].isInstance()) args[0].asInstance()->checkModify();
            auto d = args[0].isInstance() ? args[0].asInstance()->fields : static_cast<ObjDict*>(args[0].asObj());
            if (d) d->remove(args[1]);
            else throw std::runtime_error("Runtime Error: Key not found.");
            return args[0];
        }
        else if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ns->checkModify();
            if (!args[1].isString()) throw std::runtime_error("Type Error: Namespace keys must be strings.");
            auto it = ns->fields.find(args[1].asString());
            if (it == ns->fields.end()) throw std::runtime_error("Runtime Error: Key not found.");
            ns->fields.erase(it);
            return args[0];
        }
        throw std::runtime_error("Type Error: remove() expects a Set, List, Dict, Instance, or Namespace.");
        });

    reg("discard", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::SET)) {
            auto s = static_cast<ObjSet*>(args[0].asObj());
            s->discard(args[1]);
            return args[0];
        }
        else if (args[0].isObjType(ObjType::DICT) || args[0].isInstance()) {
            if (args[0].isInstance()) args[0].asInstance()->checkModify();
            auto d = args[0].isInstance() ? args[0].asInstance()->fields : static_cast<ObjDict*>(args[0].asObj());
            if (d) d->discard(args[1]);
            return args[0];
        }
        else if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ns->checkModify();
            if (args[1].isString()) ns->fields.erase(args[1].asString());
            return args[0];
        }
        throw std::runtime_error("Type Error: discard() expects a Set, Dict, Instance, or Namespace.");
        });

    reg("clear", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::SET)) {
            auto s = static_cast<ObjSet*>(args[0].asObj());
            s->clear();
            return args[0];
        }
        else if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            l->clear();
            return args[0];
        }
        else if (args[0].isObjType(ObjType::DICT) || args[0].isInstance()) {
            if (args[0].isInstance()) args[0].asInstance()->checkModify();
            auto d = args[0].isInstance() ? args[0].asInstance()->fields : static_cast<ObjDict*>(args[0].asObj());
            if (d) d->clear();
            return args[0];
        }
        else if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ns->clear();
            return args[0];
        }
        throw std::runtime_error("Type Error: clear() expects a Set, List, Dict, Instance, or Namespace.");
        });
}

// =================================================================
// [16] 字符串
// =================================================================
void BuiltinRegistry::registerStringFunctions() {
    reg("str", { 1 }, [](const std::vector<Value>& args) -> Value {
        // ★ Dunder 钩子: __str__
        if (args[0].isInstance()) {
            auto inst = args[0].asInstance();
            auto [found, result] = invokeDunder(inst, DUNDER_STR, {});
            if (found) return result;
        }
        if (args[0].isString()) return args[0];
        std::ostringstream oss; oss << args[0]; return Value(oss.str());
        });
    reg("string", builtinArity["str"], builtins["str"]);

    reg("len", { 1 }, [](const std::vector<Value>& args) -> Value {
        // ★ Dunder 钩子: __len__
        if (args[0].isInstance()) {
            auto inst = args[0].asInstance();
            auto [found, result] = invokeDunder(inst, DUNDER_LEN, {});
            if (found) return result;
            return Value::fromInt32(static_cast<int32_t>(inst->fields->elements.size()));
        }
        if (args[0].isString()) return Value::fromInt32(static_cast<int32_t>(args[0].asObjString()->charLength));
        if (args[0].isObjType(ObjType::REAL_MATRIX)) { const auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat; if (m.getCols() == 1) return Value::fromInt32(m.getRows()); if (m.getRows() == 1) return Value::fromInt32(m.getCols()); return Value::fromInt32(m.getRows() * m.getCols()); }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { const auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; if (m.getCols() == 1) return Value::fromInt32(m.getRows()); if (m.getRows() == 1) return Value::fromInt32(m.getCols()); return Value::fromInt32(m.getRows() * m.getCols()); }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) { const auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat; return Value::fromInt32(m.getRows() * m.getCols()); }
        if (args[0].isObjType(ObjType::DICT)) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjDict*>(args[0].asObj())->elements.size()));
        if (args[0].isObjType(ObjType::LIST)) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjList*>(args[0].asObj())->vec.size()));
        if (args[0].isObjType(ObjType::SET)) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjSet*>(args[0].asObj())->elements.size()));
        if (args[0].isObjType(ObjType::NAMESPACE)) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjNamespace*>(args[0].asObj())->fields.size()));
        if (args[0].isBigInt()) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjBigInt*>(args[0].asObj())->num.digitCount()));
        if (args[0].isInt32()) return Value::fromInt32(args[0].asInt32() == 0 ? 0 : static_cast<int32_t>(std::to_string(args[0].asInt32()).size() - (args[0].asInt32() < 0 ? 1 : 0)));
        if (args[0].isObjType(ObjType::BASENUM)) return Value::fromInt32(static_cast<int32_t>(static_cast<ObjBaseNum*>(args[0].asObj())->base.digitCount()));
        throw std::runtime_error("Type Error: len() expects a string, vector, matrix, dict, list, set, namespace, integer, or basenum.");
        });
    reg("length", builtinArity["len"], builtins["len"]);
    reg("size", builtinArity["len"], builtins["len"]);

    reg("eval", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: eval() expects a string.");
        if (!helpers::evalCallback)
            throw std::runtime_error("Runtime Error: eval() not available in this context.");
        return helpers::evalCallback(args[0].asString());
        });

    reg("substr", { 2, 3 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: substr() expects a string."); ObjString* objStr = args[0].asObjString(); const std::string& s = objStr->str; int n=static_cast<int>(objStr->charLength); int start=static_cast<int>(std::round(args[1].asDouble())); if (start<0) start=n+start; if (start<0||start>n) throw std::runtime_error("Runtime Error: substr() start index out of range."); if (args.size()==2) return Value(utf8::substring(s, start, n - start, objStr->isAscii)); int length=static_cast<int>(std::round(args[2].asDouble())); if (length<0) throw std::runtime_error("Runtime Error: substr() length must be non-negative."); return Value(utf8::substring(s, start, length, objStr->isAscii)); });
    reg("charAt", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: charAt() expects a string."); ObjString* objStr = args[0].asObjString(); const std::string& s = objStr->str; int n=static_cast<int>(objStr->charLength); int idx=static_cast<int>(std::round(args[1].asDouble())); if (idx<0) idx=n+idx; if (idx<0||idx>=n) throw std::runtime_error("Runtime Error: charAt() index out of range."); return Value(utf8::substring(s, idx, 1, objStr->isAscii)); });
    reg("upper", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: upper() expects a string."); std::string s = args[0].asString(); std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) -> char { return static_cast<char>(std::toupper(c)); }); return Value(s); });
    reg("lower", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: lower() expects a string."); std::string s = args[0].asString(); std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); }); return Value(s); });
    reg("trim", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: trim() expects a string."); std::string s = args[0].asString(); size_t a=s.find_first_not_of(" \t\r\n"); size_t b=s.find_last_not_of(" \t\r\n"); if (a==std::string::npos) return Value(std::string("")); return Value(s.substr(a, b-a+1)); });
    reg("find", { 2, 3 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()||!args[1].isString()) throw std::runtime_error("Type Error: find() expects two strings."); ObjString* objStr = args[0].asObjString(); const std::string& s=objStr->str; const std::string& sub=args[1].asString(); size_t startChar=0; if (args.size()==3) startChar=static_cast<size_t>(std::round(args[2].asDouble())); size_t startByte = utf8::byteOffset(s, startChar, objStr->isAscii); if (startByte == std::string::npos) return Value::fromInt32(-1); size_t pos=s.find(sub, startByte); return pos==std::string::npos ? Value::fromInt32(-1) : Value::fromInt32(static_cast<int32_t>(utf8::charIndex(s, pos, objStr->isAscii))); });
    reg("contains", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()||!args[1].isString()) throw std::runtime_error("Type Error: contains() expects two strings."); return Value(args[0].asString().find(args[1].asString())!=std::string::npos); });
    reg("replace", { 3 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()||!args[1].isString()||!args[2].isString()) throw std::runtime_error("Type Error: replace() expects three strings."); std::string s=args[0].asString(); const std::string& from=args[1].asString(); const std::string& to=args[2].asString(); if (from.empty()) return Value(s); size_t pos=0; while ((pos=s.find(from, pos))!=std::string::npos) { s.replace(pos, from.size(), to); pos+=to.size(); } return Value(s); });
    reg("repeat", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: repeat() expects a string."); const std::string& s = args[0].asString(); int n=static_cast<int>(std::round(args[1].asDouble())); if (n<0) throw std::runtime_error("Runtime Error: repeat() count must be non-negative."); std::string result; result.reserve(s.size()*n); for (int i=0;i<n;++i) result+=s; return Value(result); });
    reg("concat", {}, [](const std::vector<Value>& args) -> Value {
        bool allStrings = true;
        size_t totalLen = 0;
        for (const auto& a : args) {
            if (!a.isString()) { allStrings = false; break; }
            totalLen += a.asString().size();
        }
        if (allStrings) {
            std::string result;
            result.reserve(totalLen);
            for (const auto& a : args) result.append(a.asString());
            return Value(result);
        }
        std::ostringstream oss;
        for (const auto& a : args) oss << a;
        return Value(oss.str());
    });
    reg("startsWith", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()||!args[1].isString()) throw std::runtime_error("Type Error: startsWith() expects two strings."); const std::string& s=args[0].asString(); const std::string& prefix=args[1].asString(); return Value(s.size()>=prefix.size()&&s.compare(0,prefix.size(),prefix)==0); });
    reg("endsWith", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()||!args[1].isString()) throw std::runtime_error("Type Error: endsWith() expects two strings."); const std::string& s=args[0].asString(); const std::string& suffix=args[1].asString(); return Value(s.size()>=suffix.size()&&s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0); });
    reg("split", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()||!args[1].isString()) throw std::runtime_error("Type Error: split() expects two strings."); const std::string& s=args[0].asString(); const std::string& delim=args[1].asString(); if (delim.empty()) throw std::runtime_error("Runtime Error: split() delimiter cannot be empty."); ObjList* result = GcHeap::get().allocate<ObjList>(); GcObjGuard guard(result); size_t start=0,pos; while ((pos=s.find(delim,start))!=std::string::npos) { result->vec.push_back(Value(s.substr(start,pos-start))); start=pos+delim.size(); } result->vec.push_back(Value(s.substr(start))); return Value(result); });
    reg("ord", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: ord() expects a string."); const std::string& s=args[0].asString(); if (s.empty()) throw std::runtime_error("Runtime Error: ord() requires a non-empty string."); return Value::fromInt32(utf8::codepoint(s, 0)); });
    reg("chr", { 1 }, [](const std::vector<Value>& args) -> Value { int code=static_cast<int>(std::round(args[0].asDouble())); if (code<0||code>0x10FFFF) throw std::runtime_error("Runtime Error: chr() code out of Unicode range."); return Value(utf8::fromCodepoint(code)); });
    reg("parseNum", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: parseNum() expects a string."); const std::string& s=args[0].asString(); size_t a=s.find_first_not_of(" \t\r\n"); if (a==std::string::npos) throw std::runtime_error("Math Error: Cannot parse empty string as number."); size_t b=s.find_last_not_of(" \t\r\n"); std::string trimmed=s.substr(a,b-a+1); try { if (trimmed.find('.')!=std::string::npos||trimmed.find('e')!=std::string::npos||trimmed.find('E')!=std::string::npos) return Value(std::stod(trimmed)); return Value(BigInt(trimmed)); } catch (...) { throw std::runtime_error("Math Error: Cannot parse '"+trimmed+"' as a number."); } });
}

// =================================================================
// [17] 数组引擎
// =================================================================
void BuiltinRegistry::registerArrayFunctions() {
    auto expectContainer = [](const std::string& name) -> Value {
        throw std::runtime_error("Type Error: " + name + "() expects a List or a Matrix (Real/Complex/String).");
        };

    reg("first", { 1 }, [expectContainer](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            if (l->vec.empty()) throw std::runtime_error("Runtime Error: first() on empty list.");
            return l->vec[0];
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            if (m.getRows() * m.getCols() == 0) throw std::runtime_error("Runtime Error: first() on empty vector.");
            return Value(m.rawData()[0]);
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            if (m.getRows() * m.getCols() == 0) throw std::runtime_error("Runtime Error: first() on empty vector.");
            return Value(m.rawData()[0]);
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            if (m.getRows() * m.getCols() == 0) throw std::runtime_error("Runtime Error: first() on empty vector.");
            return Value(m.rawData()[0]);
        }
        return expectContainer("first");
        });

    reg("last", { 1 }, [expectContainer](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            if (l->vec.empty()) throw std::runtime_error("Runtime Error: last() on empty list.");
            return l->vec.back();
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: last() on empty vector.");
            return Value(m.rawData()[n - 1]);
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: last() on empty vector.");
            return Value(m.rawData()[n - 1]);
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: last() on empty vector.");
            return Value(m.rawData()[n - 1]);
        }
        return expectContainer("last");
        });

    reg("pop", { 1 }, [expectContainer](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            if (l->vec.empty()) throw std::runtime_error("Runtime Error: pop() on empty list.");
            Value val = l->vec.back();
            l->mut().pop_back();
            return val;
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: pop() on empty vector.");
            return Value(m.rawData()[n - 1]);
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: pop() on empty vector.");
            return Value(m.rawData()[n - 1]);
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: pop() on empty vector.");
            return Value(m.rawData()[n - 1]);
        }
        return expectContainer("pop");
        });

    reg("shift", { 1 }, [expectContainer](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            if (l->vec.empty()) throw std::runtime_error("Runtime Error: shift() on empty list.");
            Value val = l->vec.front();
            l->mut().erase(l->mut().begin());
            return val;
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: shift() on empty vector.");
            return Value(m.rawData()[0]);
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: shift() on empty vector.");
            return Value(m.rawData()[0]);
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            int n = m.getRows() * m.getCols();
            if (n == 0) throw std::runtime_error("Runtime Error: shift() on empty vector.");
            return Value(m.rawData()[0]);
        }
        return expectContainer("shift");
        });

    reg("push", { 2 }, [expectContainer](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            l->mut().push_back(args[1]);
            return args[0];
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            v.push_back(args[1].asDouble());
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(RealMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            v.push_back(args[1].asComplex());
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(ComplexMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            if (args[1].isString()) v.push_back(args[1].asString());
            else { std::ostringstream oss; oss << args[1]; v.push_back(oss.str()); }
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(StringMatrix(r, c, v));
        }
        return expectContainer("push");
        });
    reg("append", builtinArity["push"], builtins["push"]);

    reg("prepend", { 2 }, [expectContainer](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            l->mut().insert(l->mut().begin(), args[1]);
            return args[0];
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            v.insert(v.begin(), args[1].asDouble());
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(RealMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            v.insert(v.begin(), args[1].asComplex());
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(ComplexMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            if (args[1].isString()) v.insert(v.begin(), args[1].asString());
            else { std::ostringstream oss; oss << args[1]; v.insert(v.begin(), oss.str()); }
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(StringMatrix(r, c, v));
        }
        return expectContainer("prepend");
        });

    reg("insert", { 3 }, [expectContainer](const std::vector<Value>& args) -> Value {
        int idx = static_cast<int>(std::round(args[1].asDouble()));
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            int i = idx < 0 ? static_cast<int>(l->vec.size()) + idx : idx;
            if (i < 0 || i > static_cast<int>(l->vec.size())) throw std::runtime_error("Runtime Error: insert() index out of range.");
            l->mut().insert(l->mut().begin() + i, args[2]);
            return args[0];
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            int i = idx < 0 ? static_cast<int>(v.size()) + idx : idx;
            if (i < 0 || i > static_cast<int>(v.size())) throw std::runtime_error("Runtime Error: insert() index out of range.");
            v.insert(v.begin() + i, args[2].asDouble());
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(RealMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            int i = idx < 0 ? static_cast<int>(v.size()) + idx : idx;
            if (i < 0 || i > static_cast<int>(v.size())) throw std::runtime_error("Runtime Error: insert() index out of range.");
            v.insert(v.begin() + i, args[2].asComplex());
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(ComplexMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            int i = idx < 0 ? static_cast<int>(v.size()) + idx : idx;
            if (i < 0 || i > static_cast<int>(v.size())) throw std::runtime_error("Runtime Error: insert() index out of range.");
            if (args[2].isString()) v.insert(v.begin() + i, args[2].asString());
            else { std::ostringstream oss; oss << args[2]; v.insert(v.begin() + i, oss.str()); }
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            return Value(StringMatrix(r, c, v));
        }
        return expectContainer("insert");
        });

    reg("removeAt", { 2 }, [expectContainer](const std::vector<Value>& args) -> Value {
        int idx = static_cast<int>(std::round(args[1].asDouble()));
        if (args[0].isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(args[0].asObj());
            int i = idx < 0 ? static_cast<int>(l->vec.size()) + idx : idx;
            if (i < 0 || i >= static_cast<int>(l->vec.size())) throw std::runtime_error("Runtime Error: removeAt() index out of range.");
            l->mut().erase(l->mut().begin() + i);
            return args[0];
        }
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            if (v.empty()) throw std::runtime_error("Runtime Error: removeAt() on empty vector.");
            int i = idx < 0 ? static_cast<int>(v.size()) + idx : idx;
            if (i < 0 || i >= static_cast<int>(v.size())) throw std::runtime_error("Runtime Error: removeAt() index out of range.");
            v.erase(v.begin() + i);
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            if (r * c == 0) return Value(RealMatrix(1, 0));
            return Value(RealMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            if (v.empty()) throw std::runtime_error("Runtime Error: removeAt() on empty vector.");
            int i = idx < 0 ? static_cast<int>(v.size()) + idx : idx;
            if (i < 0 || i >= static_cast<int>(v.size())) throw std::runtime_error("Runtime Error: removeAt() index out of range.");
            v.erase(v.begin() + i);
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            if (r * c == 0) return Value(ComplexMatrix(1, 0));
            return Value(ComplexMatrix(r, c, v));
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            auto v = m.rawData();
            if (v.empty()) throw std::runtime_error("Runtime Error: removeAt() on empty vector.");
            int i = idx < 0 ? static_cast<int>(v.size()) + idx : idx;
            if (i < 0 || i >= static_cast<int>(v.size())) throw std::runtime_error("Runtime Error: removeAt() index out of range.");
            v.erase(v.begin() + i);
            int r = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(v.size()) : 1;
            int c = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(v.size());
            if (r * c == 0) return Value(StringMatrix(1, 0));
            return Value(StringMatrix(r, c, v));
        }
        return expectContainer("removeAt");
        });

    reg("slice", { 2, 3 }, [expectContainer](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        auto getBounds = [&](int n, int& start, int& end) {
            start = static_cast<int>(std::round(args[1].asDouble()));
            if (start < 0) start = n + start;
            start = std::max(0, std::min(start, n));
            end = n;
            if (args.size() == 3) {
                end = static_cast<int>(std::round(args[2].asDouble()));
                if (end < 0) end = n + end;
                end = std::max(0, std::min(end, n));
            }
        };

        if (arg.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(arg.asObj());
            int start, end; getBounds(static_cast<int>(l->vec.size()), start, end);
            ObjList* result = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(result);
            for (int i = start; i < end; ++i) result->vec.push_back(l->vec[i]);
            return Value(result);
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            auto v = m.rawData();
            int start, end; getBounds(static_cast<int>(v.size()), start, end);
            std::vector<double> retV;
            if (start < end) retV.assign(v.begin() + start, v.begin() + end);
            int cr = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(retV.size()) : 1;
            int cc = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(retV.size());
            if (cr * cc == 0) return Value(RealMatrix(1, 0));
            return Value(RealMatrix(cr, cc, retV));
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            auto v = m.rawData();
            int start, end; getBounds(static_cast<int>(v.size()), start, end);
            std::vector<Complex> retV;
            if (start < end) retV.assign(v.begin() + start, v.begin() + end);
            int cr = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(retV.size()) : 1;
            int cc = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(retV.size());
            if (cr * cc == 0) return Value(ComplexMatrix(1, 0));
            return Value(ComplexMatrix(cr, cc, retV));
        } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            auto v = m.rawData();
            int start, end; getBounds(static_cast<int>(v.size()), start, end);
            std::vector<std::string> retV;
            if (start < end) retV.assign(v.begin() + start, v.begin() + end);
            int cr = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(retV.size()) : 1;
            int cc = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(retV.size());
            if (cr * cc == 0) return Value(StringMatrix(1, 0));
            return Value(StringMatrix(cr, cc, retV));
        }
        return expectContainer("slice");
        });

    reg("reverse", { 1 }, [expectContainer](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        if (arg.isString()) {
            ObjString* objStr = arg.asObjString();
            if (objStr->isAscii) {
                std::string s = objStr->str;
                std::reverse(s.begin(), s.end());
                return Value(s);
            }
            std::string s = objStr->str;
            std::string res;
            size_t len = objStr->charLength;
            for (size_t i = 0; i < len; ++i) {
                res += utf8::substring(s, len - 1 - i, 1, false);
            }
            return Value(res);
        } else if (arg.isObjType(ObjType::LIST)) {
            ObjList* L = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(L);
            L->vec = static_cast<ObjList*>(arg.asObj())->vec;
            std::reverse(L->vec.begin(), L->vec.end());
            return Value(L);
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            auto v = m.rawData(); std::reverse(v.begin(), v.end());
            return Value(RealMatrix(m.getRows(), m.getCols(), v));
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            auto v = m.rawData(); std::reverse(v.begin(), v.end());
            return Value(ComplexMatrix(m.getRows(), m.getCols(), v));
        } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            auto v = m.rawData(); std::reverse(v.begin(), v.end());
            return Value(StringMatrix(m.getRows(), m.getCols(), v));
        }
        return expectContainer("reverse");
        });

    reg("flatten", { 1 }, [expectContainer](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        if (arg.isObjType(ObjType::LIST)) {
            ObjList* result = GcHeap::get().allocate<ObjList>();
            std::function<void(ObjList*)> flattenList = [&](ObjList* L) {
                for (const auto& e : L->vec) {
                    if (e.isObjType(ObjType::LIST)) flattenList(static_cast<ObjList*>(e.asObj()));
                    else result->vec.push_back(e);
                }
            };
            flattenList(static_cast<ObjList*>(arg.asObj()));
            return Value(result);
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            return Value(RealMatrix(1, m.getRows() * m.getCols(), m.rawData()));
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            return Value(ComplexMatrix(1, m.getRows() * m.getCols(), m.rawData()));
        } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            return Value(StringMatrix(1, m.getRows() * m.getCols(), m.rawData()));
        }
        return expectContainer("flatten");
        });

    reg("unique", { 1 }, [expectContainer](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        if (arg.isObjType(ObjType::LIST)) {
            ObjList* result = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(result);
            for (const auto& e : static_cast<ObjList*>(arg.asObj())->vec) {
                bool found = false;
                for (const auto& r : result->vec) {
                    if (Value::equals(e, r)) { found = true; break; }
                }
                if (!found) result->vec.push_back(e);
            }
            return Value(result);
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            std::vector<double> result;
            for (const auto& x : m.rawData()) {
                bool found = false;
                for (const auto& y : result) { if (x == y) { found = true; break; } }
                if (!found) result.push_back(x);
            }
            int cr = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(result.size()) : 1;
            int cc = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(result.size());
            if (cr * cc == 0) return Value(RealMatrix(1, 0));
            return Value(RealMatrix(cr, cc, result));
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            std::vector<Complex> result;
            for (const auto& x : m.rawData()) {
                bool found = false;
                for (const auto& y : result) { if (x == y) { found = true; break; } }
                if (!found) result.push_back(x);
            }
            int cr = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(result.size()) : 1;
            int cc = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(result.size());
            if (cr * cc == 0) return Value(ComplexMatrix(1, 0));
            return Value(ComplexMatrix(cr, cc, result));
        } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            std::vector<std::string> result;
            for (const auto& x : m.rawData()) {
                bool found = false;
                for (const auto& y : result) { if (x == y) { found = true; break; } }
                if (!found) result.push_back(x);
            }
            int cr = (m.getCols() == 1 && m.getRows() > 1) ? static_cast<int>(result.size()) : 1;
            int cc = (m.getCols() == 1 && m.getRows() > 1) ? 1 : static_cast<int>(result.size());
            if (cr * cc == 0) return Value(StringMatrix(1, 0));
            return Value(StringMatrix(cr, cc, result));
        }
        return expectContainer("unique");
        });

    reg("indexOf", { 2 }, [expectContainer, this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        int idx = 0;
        int foundIdx = -1;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (Value::equals(nextVal, args[1])) {
                foundIdx = idx;
                return false; // break
            }
            idx++;
            return true;
        })) {
            return Value::fromInt32(foundIdx);
        }
        if (arg.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(arg.asObj());
            for (size_t i = 0; i < l->vec.size(); ++i) {
                if (Value::equals(l->vec[i], args[1])) return Value::fromInt32(static_cast<int32_t>(i));
            }
            return Value::fromInt32(-1);
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            try {
                double target = args[1].asDouble();
                auto v = m.rawData();
                for (size_t i = 0; i < v.size(); ++i) if (v[i] == target) return Value::fromInt32(static_cast<int32_t>(i));
            } catch (...) {}
            return Value::fromInt32(-1);
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            try {
                Complex target = args[1].asComplex();
                auto v = m.rawData();
                for (size_t i = 0; i < v.size(); ++i) if (v[i] == target) return Value::fromInt32(static_cast<int32_t>(i));
            } catch (...) {}
            return Value::fromInt32(-1);
        } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            std::string target = args[1].isString() ? args[1].asString() : args[1].toString();
            auto v = m.rawData();
            for (size_t i = 0; i < v.size(); ++i) if (v[i] == target) return Value::fromInt32(static_cast<int32_t>(i));
            return Value::fromInt32(-1);
        }
        return expectContainer("indexOf");
        });

    reg("count", { 2 }, [expectContainer, this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        int c = 0;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (Value::equals(nextVal, args[1])) c++;
            return true;
        })) {
            return Value::fromInt32(c);
        }
        if (arg.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(arg.asObj());
            int countVal = 0;
            for (const auto& e : l->vec) { if (Value::equals(e, args[1])) countVal++; }
            return Value::fromInt32(countVal);
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            int countVal = 0;
            try {
                double target = args[1].asDouble();
                for (const auto& x : m.rawData()) if (x == target) countVal++;
            } catch (...) {}
            return Value::fromInt32(countVal);
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            int countVal = 0;
            try {
                Complex target = args[1].asComplex();
                for (const auto& x : m.rawData()) if (x == target) countVal++;
            } catch (...) {}
            return Value::fromInt32(countVal);
        } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            int countVal = 0;
            std::string target = args[1].isString() ? args[1].asString() : args[1].toString();
            for (const auto& x : m.rawData()) if (x == target) countVal++;
            return Value::fromInt32(countVal);
        }
        return expectContainer("count");
        });

    reg("join", { 2 }, [expectContainer, this](const std::vector<Value>& args) -> Value {
        if (!args[1].isString()) throw std::runtime_error("Type Error: delimiter must be a string.");
        const std::string& delim = args[1].asString();
        Value arg = args[0];
        std::ostringstream oss;
        bool first = true;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (!first) oss << delim;
            if (nextVal.isString()) oss << nextVal.asString();
            else oss << nextVal;
            first = false;
            return true;
        })) {
            return Value(oss.str());
        }
        if (arg.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(arg.asObj());
            if (l->vec.empty()) return Value(std::string(""));
            bool allStrings = true; size_t totalLen = 0;
            for (const auto& e : l->vec) {
                if (!e.isString()) { allStrings = false; break; }
                totalLen += e.asString().size();
            }
            if (allStrings) {
                std::string result;
                result.reserve(totalLen + delim.size() * (l->vec.size() - 1));
                result.append(l->vec[0].asString());
                for (size_t i = 1; i < l->vec.size(); ++i) { result.append(delim); result.append(l->vec[i].asString()); }
                return Value(result);
            }
            std::ostringstream listOss;
            for (size_t i = 0; i < l->vec.size(); ++i) { if (i > 0) listOss << delim; listOss << l->vec[i]; }
            return Value(listOss.str());
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            std::ostringstream matOss; auto v = m.rawData();
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) matOss << delim;
                matOss << Value(v[i]);
            }
            return Value(matOss.str());
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            std::ostringstream cmatOss; auto v = m.rawData();
            for (size_t i = 0; i < v.size(); ++i) { if (i > 0) cmatOss << delim; cmatOss << Value(v[i]); }
            return Value(cmatOss.str());
        } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            auto v = m.rawData();
            if (v.empty()) return Value(std::string(""));
            size_t totalLen = 0;
            for (const auto& s : v) totalLen += s.size();
            std::string result;
            result.reserve(totalLen + delim.size() * (v.size() - 1));
            result.append(v[0]);
            for (size_t i = 1; i < v.size(); ++i) { result.append(delim); result.append(v[i]); }
            return Value(result);
        }
        return expectContainer("join");
        });

    auto applyMathVectorOp = [expectContainer](const Value& val, auto opBody) -> Value {
        if (val.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(val.asObj());
            ObjList* result = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(result);
            if (l->vec.empty()) return Value(result);
            Value acc = l->vec[0]; 
            GcValueGuard accGuard(acc);
            result->vec.push_back(acc);
            for (size_t i = 1; i < l->vec.size(); ++i) { acc = opBody(acc, l->vec[i]); result->vec.push_back(acc); }
            return Value(result);
        } else if (val.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(val.asObj())->mat;
            auto v = m.rawData();
            if (v.empty()) return Value(RealMatrix(m.getRows(), m.getCols(), v));
            for (size_t i = 1; i < v.size(); ++i) {
                Value res = opBody(Value(v[i - 1]), Value(v[i]));
                v[i] = res.asDouble();
            }
            return Value(RealMatrix(m.getRows(), m.getCols(), v));
        } else if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(val.asObj())->mat;
            auto v = m.rawData();
            if (v.empty()) return Value(ComplexMatrix(m.getRows(), m.getCols(), v));
            for (size_t i = 1; i < v.size(); ++i) {
                Value res = opBody(Value(v[i - 1]), Value(v[i]));
                v[i] = res.asComplex();
            }
            return Value(ComplexMatrix(m.getRows(), m.getCols(), v));
        }
        throw std::runtime_error("Type Error: cumsum/cumprod expects a numeric vector or list.");
        };

    reg("cumsum", { 1 }, [applyMathVectorOp](const std::vector<Value>& args) -> Value {
        return applyMathVectorOp(args[0], [](const Value& a, const Value& b) { return a + b; });
        });
    reg("cumprod", { 1 }, [applyMathVectorOp](const std::vector<Value>& args) -> Value {
        return applyMathVectorOp(args[0], [](const Value& a, const Value& b) { return a * b; });
        });

    reg("diffs", { 1 }, [](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        if (arg.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(arg.asObj());
            if (l->vec.size() < 2) throw std::runtime_error("Runtime Error: diffs() requires at least 2 elements.");
            ObjList* result = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(result);
            for (size_t i = 0; i < l->vec.size() - 1; ++i) result->vec.push_back(l->vec[i + 1] - l->vec[i]);
            return Value(result);
        } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            auto v = m.rawData();
            if (v.size() < 2) throw std::runtime_error("Runtime Error: diffs() requires at least 2 elements.");
            std::vector<double> d(v.size() - 1);
            for (size_t i = 0; i < d.size(); ++i) d[i] = v[i + 1] - v[i];
            return Value(RealMatrix(1, static_cast<int>(d.size()), d));
        } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            auto v = m.rawData();
            if (v.size() < 2) throw std::runtime_error("Runtime Error: diffs() requires at least 2 elements.");
            std::vector<Complex> d(v.size() - 1);
            for (size_t i = 0; i < d.size(); ++i) d[i] = v[i + 1] - v[i];
            return Value(ComplexMatrix(1, static_cast<int>(d.size()), d));
        }
        throw std::runtime_error("Type Error: diffs() expects a numeric vector or list.");
        });

    // fill, linspace
    reg("fill", { 2 }, [](const std::vector<Value>& args) -> Value { int n = static_cast<int>(std::round(args[1].asDouble())); if (n < 0) throw std::runtime_error("Runtime Error: count must be non-negative."); return Value(RealMatrix(1, n, std::vector<double>(n, args[0].asDouble()))); });
    reg("linspace", { 3 }, [](const std::vector<Value>& args) -> Value { double a = args[0].asDouble(), b = args[1].asDouble(); int n = static_cast<int>(std::round(args[2].asDouble())); if (n < 1) throw std::runtime_error("Runtime Error: requires n >= 1."); std::vector<double> v(n); if (n == 1) v[0] = a; else { for (int i = 0; i < n; ++i) v[i] = a + (b - a) * i / (n - 1); } return Value(RealMatrix(1, n, v)); });
}

// =================================================================
// [18] StringMatrix
// =================================================================
void BuiltinRegistry::registerStringMatrix() {
    reg("strmat", {}, [](const std::vector<Value>& args) -> Value { if (args.size()<2) throw std::runtime_error("Runtime Error: strmat() expects at least 2 arguments."); int r=static_cast<int>(std::round(args[0].asDouble())),c=static_cast<int>(std::round(args[1].asDouble())); if (r<0||c<0) throw std::runtime_error("Runtime Error: strmat() dimensions must be non-negative."); if (args.size()==2) return Value(StringMatrix(r,c)); if (static_cast<int>(args.size())-2!=r*c) throw std::runtime_error("Runtime Error: strmat() element count does not match dimensions."); std::vector<std::string> flat; flat.reserve(r*c); for (size_t i=2;i<args.size();++i) { if (args[i].isString()) flat.push_back(args[i].asString()); else { std::ostringstream oss; oss<<args[i]; flat.push_back(oss.str()); } } return Value(StringMatrix(r,c,flat)); });
    reg("strvec", {}, [](const std::vector<Value>& args) -> Value { std::vector<std::string> flat; for (const auto& a:args) { if (a.isString()) flat.push_back(a.asString()); else { std::ostringstream oss; oss<<a; flat.push_back(oss.str()); } } return Value(StringMatrix(static_cast<int>(flat.size()),1,flat)); });
    reg("strrow", {}, [](const std::vector<Value>& args) -> Value { std::vector<std::string> flat; for (const auto& a:args) { if (a.isString()) flat.push_back(a.asString()); else { std::ostringstream oss; oss<<a; flat.push_back(oss.str()); } } return Value(StringMatrix(1,static_cast<int>(flat.size()),flat)); });
    reg("strfill", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isString()) throw std::runtime_error("Type Error: strfill() expects a string and count."); int n=static_cast<int>(std::round(args[1].asDouble())); if (n<0) throw std::runtime_error("Runtime Error: strfill() count must be non-negative."); return Value(StringMatrix(1,n,std::vector<std::string>(n,args[0].asString()))); });
    reg("strfind", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isObjType(ObjType::STRING_MATRIX)||!args[1].isString()) throw std::runtime_error("Type Error: strfind() expects StringMatrix and string."); const auto& m=static_cast<ObjStringMatrix*>(args[0].asObj())->mat; const std::string& target=args[1].asString(); for (int i=0;i<m.getRows();++i) for (int j=0;j<m.getCols();++j) if (m(i,j)==target) return Value(RealMatrix(1,2,{static_cast<double>(i),static_cast<double>(j)})); return Value::fromInt32(-1); });
    reg("strjoin", { 2 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isObjType(ObjType::STRING_MATRIX)||!args[1].isString()) throw std::runtime_error("Type Error: strjoin() expects StringMatrix and string."); const auto& m=static_cast<ObjStringMatrix*>(args[0].asObj())->mat; const std::string& delim=args[1].asString(); std::string result; bool first=true; for (int i=0;i<m.getRows();++i) for (int j=0;j<m.getCols();++j) { if (!first) result+=delim; result+=m(i,j); first=false; } return Value(result); });
    reg("strsort", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isObjType(ObjType::STRING_MATRIX)) throw std::runtime_error("Type Error: strsort() expects a StringMatrix."); const auto& m=static_cast<ObjStringMatrix*>(args[0].asObj())->mat; std::vector<std::string> flat=m.rawData(); std::sort(flat.begin(),flat.end()); return Value(StringMatrix(m.getRows(),m.getCols(),flat)); });
    reg("toStrMat", { 1 }, [](const std::vector<Value>& args) -> Value { if (args[0].isObjType(ObjType::STRING_MATRIX)) return args[0]; if (args[0].isObjType(ObjType::REAL_MATRIX)) { const auto& m=static_cast<ObjRealMatrix*>(args[0].asObj())->mat; std::vector<std::string> flat; for (int i=0;i<m.getRows();++i) for (int j=0;j<m.getCols();++j) { std::ostringstream oss; oss<<Value(m(i,j)); flat.push_back(oss.str()); } return Value(StringMatrix(m.getRows(),m.getCols(),flat)); } if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) { const auto& m=static_cast<ObjComplexMatrix*>(args[0].asObj())->mat; std::vector<std::string> flat; for (int i=0;i<m.getRows();++i) for (int j=0;j<m.getCols();++j) { std::ostringstream oss; oss<<Value(m(i,j)); flat.push_back(oss.str()); } return Value(StringMatrix(m.getRows(),m.getCols(),flat)); } throw std::runtime_error("Type Error: toStrMat() expects a matrix."); });
}

// =================================================================
// [19] Dict / Instance 属性大一统透视 API
// =================================================================
void BuiltinRegistry::registerDictFunctions() {
    reg("dict", {}, [](const std::vector<Value>& args) -> Value { 
        if (args.size() % 2 != 0) throw std::runtime_error("Runtime Error: dict() expects even number of arguments."); 
        ObjDict* d = GcHeap::get().allocate<ObjDict>();
        GcObjGuard guard(d);
        for (size_t i = 0; i < args.size(); i += 2) { 
            d->keyMap[args[i]] = d->elements.size(); 
            d->elements.push_back({args[i], args[i + 1]}); 
        } 
        return Value(d); 
    });

    reg("keys", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ObjList* L = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(L);
            for (const auto& [k, v] : ns->fields) L->vec.push_back(Value(k));
            return Value(L);
        }
        ObjDict* d = helpers::getDictMap(args[0], "keys");
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        for (const auto& [k, v] : d->elements) L->vec.push_back(k);
        return Value(L);
        });
    // ★ 设定属性拾取别名！完美融合 Instance
    reg("getFields", builtinArity["keys"], builtins["keys"]);

    reg("values", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ObjList* L = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(L);
            for (const auto& [k, v] : ns->fields) L->vec.push_back(*(v.upval->location));
            return Value(L);
        }
        ObjDict* d = helpers::getDictMap(args[0], "values");
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        for (const auto& [k, v] : d->elements) L->vec.push_back(v);
        return Value(L);
        });

    reg("hasKey", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            if (!args[1].isString()) return Value(false);
            return Value(ns->fields.find(args[1].asString()) != ns->fields.end());
        }
        ObjDict* d = helpers::getDictMap(args[0], "hasKey");
        return Value(d->keyMap.find(args[1]) != d->keyMap.end());
        });
    // ★ 设定查询别名
    reg("hasField", builtinArity["hasKey"], builtins["hasKey"]);
    reg("has", builtinArity["hasKey"], builtins["hasKey"]);

    reg("removeKey", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ns->checkModify();
            if (!args[1].isString()) throw std::runtime_error("Type Error: Namespace keys must be strings.");
            auto it = ns->fields.find(args[1].asString());
            if (it == ns->fields.end()) throw std::runtime_error("Runtime Error: Key not found.");
            ns->fields.erase(it);
            return args[0];
        }
        ObjDict* d = helpers::getDictMap(args[0], "removeKey");
        d->remove(args[1]);
        return args[0];
        });

    reg("dictSize", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            return Value::fromInt32(static_cast<int32_t>(ns->fields.size()));
        }
        ObjDict* d = helpers::getDictMap(args[0], "dictSize"); return Value::fromInt32(static_cast<int32_t>(d->elements.size()));
        });

    reg("dictMerge", { 2 }, [](const std::vector<Value>& args) -> Value {
        auto getPairs = [](const Value& v) -> std::vector<std::pair<Value, Value>> {
            if (v.isObjType(ObjType::NAMESPACE)) {
                std::vector<std::pair<Value, Value>> res;
                for (const auto& [k, field] : static_cast<ObjNamespace*>(v.asObj())->fields) {
                    res.push_back({Value(k), *(field.upval->location)});
                }
                return res;
            }
            ObjDict* d = helpers::getDictMap(v, "dictMerge");
            return d->elements;
        };
        
        if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ns->checkModify();
            auto pairs2 = getPairs(args[1]);
            for (const auto& [k, v] : pairs2) {
                if (!k.isString()) throw std::runtime_error("Type Error: Namespace keys must be strings.");
                std::string key = k.asString();
                auto it = ns->fields.find(key);
                if (it != ns->fields.end()) {
                    if (it->second.isConst) throw std::runtime_error("Runtime Error: Cannot modify const property '" + key + "'.");
                    *(it->second.upval->location) = v;
                } else {
                    auto uv = GcHeap::get().allocate<ObjUpVal>();
                    uv->closed = v;
                    uv->location = &uv->closed;
                    ns->fields[key] = { uv, false };
                }
            }
            return args[0];
        }
        
        ObjDict* d1 = helpers::getDictMap(args[0], "dictMerge");
        auto pairs2 = getPairs(args[1]);
        for (const auto& [k, v] : pairs2) {
            d1->set(k, v);
        }
        return args[0];
        });

    reg("dictPairs", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(args[0].asObj());
            ObjList* L = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(L);
            for (const auto& [k, v] : ns->fields) {
                ObjList* pair = GcHeap::get().allocate<ObjList>();
                pair->vec.push_back(Value(k));
                pair->vec.push_back(*(v.upval->location));
                pair->is_frozen = true;
                L->vec.push_back(Value(pair));
            }
            return Value(L);
        }
        ObjDict* d = helpers::getDictMap(args[0], "dictPairs");
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        for (const auto& [k, v] : d->elements) {
            ObjList* pair = GcHeap::get().allocate<ObjList>();
            pair->vec.push_back(k);
            pair->vec.push_back(v);
            pair->is_frozen = true;
            L->vec.push_back(Value(pair));
        }
        return Value(L);
        });
}

// =================================================================
// [20] List & Conversion
// =================================================================
void BuiltinRegistry::registerListConversion() {
    reg("list", {}, [](const std::vector<Value>& args) -> Value { ObjList* L = GcHeap::get().allocate<ObjList>(); GcObjGuard guard(L); for (const auto& a : args) L->vec.push_back(a); return Value(L); });

    reg("toList", { 1 }, [](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        if (arg.isObjType(ObjType::LIST)) return arg;
        
        if (arg.isInstance() && helpers::hasDunder(arg, DUNDER_ITER)) {
            auto inst = arg.asInstance();
            auto [hasIter, iterObj] = invokeDunder(inst, DUNDER_ITER, {});
            if (hasIter) {
                GcValueGuard iterGuard(iterObj);
                ObjList* L = GcHeap::get().allocate<ObjList>();
                GcObjGuard guard(L);
                auto iterInst = iterObj.asInstance();
                if (iterInst->c_nativeNext) {
                    while (true) {
                        Value nextVal = iterInst->c_nativeNext(iterInst);
                        if (nextVal.isUninit()) break;
                        L->vec.push_back(nextVal);
                    }
                } else {
                    while (true) {
                        auto [hasNext, nextVal] = invokeDunder(iterInst, DUNDER_NEXT, {});
                        if (!hasNext || nextVal.isNone()) break;
                        L->vec.push_back(nextVal);
                    }
                }
                return Value(L);
            }
        }

        if (arg.isObjType(ObjType::REAL_MATRIX)) {
            const auto& m = static_cast<ObjRealMatrix*>(arg.asObj())->mat;
            if (m.getRows() == 1 || m.getCols() == 1) {
                ObjList* L = GcHeap::get().allocate<ObjList>();
                for (double d : m.rawData()) L->vec.push_back(Value(d));
                return Value(L);
            }
            ObjList* rows = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(rows);
            for (int i = 0; i < m.getRows(); ++i) {
                ObjList* row = GcHeap::get().allocate<ObjList>();
                for (int j = 0; j < m.getCols(); ++j) row->vec.push_back(Value(m(i, j)));
                row->is_frozen = true;
                rows->vec.push_back(Value(row));
            }
            return Value(rows);
        }
        if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
            const auto& m = static_cast<ObjComplexMatrix*>(arg.asObj())->mat;
            if (m.getRows() == 1 || m.getCols() == 1) {
                ObjList* L = GcHeap::get().allocate<ObjList>();
                for (const auto& c : m.rawData()) L->vec.push_back(Value(c));
                return Value(L);
            }
            ObjList* rows = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(rows);
            for (int i = 0; i < m.getRows(); ++i) {
                ObjList* row = GcHeap::get().allocate<ObjList>();
                for (int j = 0; j < m.getCols(); ++j) row->vec.push_back(Value(m(i, j)));
                row->is_frozen = true;
                rows->vec.push_back(Value(row));
            }
            return Value(rows);
        }
        if (arg.isObjType(ObjType::STRING_MATRIX)) {
            const auto& m = static_cast<ObjStringMatrix*>(arg.asObj())->mat;
            if (m.getRows() == 1 || m.getCols() == 1) {
                ObjList* L = GcHeap::get().allocate<ObjList>();
                for (const auto& s : m.rawData()) L->vec.push_back(Value(s));
                return Value(L);
            }
            ObjList* rows = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(rows);
            for (int i = 0; i < m.getRows(); ++i) {
                ObjList* row = GcHeap::get().allocate<ObjList>();
                for (int j = 0; j < m.getCols(); ++j) row->vec.push_back(Value(m(i, j)));
                row->is_frozen = true;
                rows->vec.push_back(Value(row));
            }
            return Value(rows);
        }
        if (arg.isString()) {
            ObjList* L = GcHeap::get().allocate<ObjList>();
            ObjString* objStr = arg.asObjString();
            const std::string& str = objStr->str;
            if (objStr->isAscii) {
                for (char c : str) L->vec.push_back(Value(std::string(1, c)));
            } else {
                size_t len = objStr->charLength;
                for (size_t i = 0; i < len; ++i) L->vec.push_back(Value(utf8::substring(str, i, 1, false)));
            }
            return Value(L);
        }
        if (arg.isObjType(ObjType::SET)) {
            ObjList* L = GcHeap::get().allocate<ObjList>();
            for (const auto& val : static_cast<ObjSet*>(arg.asObj())->elements) L->vec.push_back(val);
            return Value(L);
        }
        ObjList* L = GcHeap::get().allocate<ObjList>();
        L->vec.push_back(arg);
        return Value(L);
        });

    reg("toStrVec", { 1 }, [this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        std::vector<std::string> flat;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            if (nextVal.isString()) flat.push_back(nextVal.asString());
            else { std::ostringstream oss; oss << nextVal; flat.push_back(oss.str()); }
            return true;
        })) {
            return Value(StringMatrix(static_cast<int>(flat.size()), 1, flat));
        }
        if (arg.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(arg.asObj())->vec;
            std::vector<std::string> listFlat;
            for (const auto& v : L) {
                if (v.isString()) listFlat.push_back(v.asString());
                else { std::ostringstream oss; oss << v; listFlat.push_back(oss.str()); }
            }
            return Value(StringMatrix(static_cast<int>(listFlat.size()), 1, listFlat));
        }
        if (arg.isObjType(ObjType::STRING_MATRIX)) return arg;
        throw std::runtime_error("Type Error: toStrVec() expects a List, StringMatrix, or Iterable.");
        });

    reg("toArray", { 1 }, [this](const std::vector<Value>& args) -> Value {
        Value arg = args[0];
        std::vector<double> flat;
        if (helpers::iterateIterable(arg, [&](const Value& nextVal) {
            flat.push_back(nextVal.asDouble());
            return true;
        })) {
            return Value(RealMatrix(1, static_cast<int>(flat.size()), flat));
        }
        if (!arg.isObjType(ObjType::LIST)) throw std::runtime_error("Type Error: expects a List or Iterable.");
        const auto& L = static_cast<ObjList*>(arg.asObj())->vec;
        std::vector<double> listFlat;
        for (const auto& v : L) listFlat.push_back(v.asDouble());
        return Value(RealMatrix(1, static_cast<int>(listFlat.size()), listFlat));
        });

    reg("toMatrix", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX) || args[0].isObjType(ObjType::COMPLEX_MATRIX) || args[0].isObjType(ObjType::STRING_MATRIX)) return args[0];
        if (!args[0].isObjType(ObjType::LIST)) throw std::runtime_error("Type Error: expects a List or matrix.");
        const auto& L = static_cast<ObjList*>(args[0].asObj())->vec;
        if (L.empty()) return Value(RealMatrix(0, 0));
        Value first = L[0];
        bool isNested = first.isObjType(ObjType::LIST);
        auto isReal = [](const Value& v) { return v.isNumber() || v.isBigInt() || v.isObjType(ObjType::FRACTION); };
        auto isNumeric = [](const Value& v) { return v.isNumber() || v.isBigInt() || v.isObjType(ObjType::FRACTION) || v.isComplex(); };
        auto isStr = [](const Value& v) { return v.isString(); };
        auto valToStr = [](const Value& v) -> std::string { if (v.isString()) return v.asString(); std::ostringstream oss; oss << v; return oss.str(); };

        if (!isNested) {
            int n = static_cast<int>(L.size()); bool allReal = true, allNum = true, allStr = true;
            for (const auto& v : L) { if (!isReal(v)) allReal = false; if (!isNumeric(v)) allNum = false; if (!isStr(v)) allStr = false; }
            if (allStr) { std::vector<std::string> flat; for (const auto& v : L) flat.push_back(v.asString()); return Value(StringMatrix(1, n, flat)); }
            if (allReal) { std::vector<double> flat; for (const auto& v : L) flat.push_back(v.asDouble()); return Value(RealMatrix(1, n, flat)); }
            if (allNum) { std::vector<Complex> flat; for (const auto& v : L) flat.push_back(v.asComplex()); return Value(ComplexMatrix(1, n, flat)); }
            std::vector<std::string> flat; for (const auto& v : L) flat.push_back(valToStr(v)); return Value(StringMatrix(1, n, flat));
        }

        int rows = static_cast<int>(L.size()), cols = -1; bool allReal = true, allNum = true, allStr = true;
        std::vector<std::vector<Value>> grid;
        for (const auto& rowVal : L) {
            if (!rowVal.isObjType(ObjType::LIST)) throw std::runtime_error("Type Error: expects uniform List of Lists.");
            const auto& rowList = static_cast<ObjList*>(rowVal.asObj())->vec;
            if (cols == -1) cols = static_cast<int>(rowList.size()); else if (static_cast<int>(rowList.size()) != cols) throw std::runtime_error("Type Error: rows must have equal length.");
            std::vector<Value> rowVec;
            for (const auto& v : rowList) { if (!isReal(v)) allReal = false; if (!isNumeric(v)) allNum = false; if (!isStr(v)) allStr = false; rowVec.push_back(v); }
            grid.push_back(std::move(rowVec));
        }
        if (cols <= 0) return Value(RealMatrix(0, 0));
        if (allStr) { std::vector<std::string> flat; for (const auto& row : grid) for (const auto& v : row) flat.push_back(v.asString()); return Value(StringMatrix(rows, cols, flat)); }
        if (allReal) { std::vector<double> flat; for (const auto& row : grid) for (const auto& v : row) flat.push_back(v.asDouble()); return Value(RealMatrix(rows, cols, flat)); }
        if (allNum) { std::vector<Complex> flat; for (const auto& row : grid) for (const auto& v : row) flat.push_back(v.asComplex()); return Value(ComplexMatrix(rows, cols, flat)); }
        std::vector<std::string> flat; for (const auto& row : grid) for (const auto& v : row) flat.push_back(valToStr(v)); return Value(StringMatrix(rows, cols, flat));
        });

    reg("zip", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::LIST) || args[1].isObjType(ObjType::LIST)) {
            auto extractL = [](const Value& v) -> std::vector<Value> {
                if (v.isObjType(ObjType::LIST)) return static_cast<ObjList*>(v.asObj())->vec;
                std::vector<Value> L;
                if (v.isObjType(ObjType::REAL_MATRIX)) { for (double d : static_cast<ObjRealMatrix*>(v.asObj())->mat.rawData()) L.push_back(Value(d)); }
                else if (v.isObjType(ObjType::COMPLEX_MATRIX)) { for (const auto& c : static_cast<ObjComplexMatrix*>(v.asObj())->mat.rawData()) L.push_back(Value(c)); }
                else if (v.isObjType(ObjType::STRING_MATRIX)) { for (const auto& s : static_cast<ObjStringMatrix*>(v.asObj())->mat.rawData()) L.push_back(Value(s)); }
                else L.push_back(v);
                return L;
                };
            auto a = extractL(args[0]), b = extractL(args[1]);
            if (a.size() != b.size()) throw std::runtime_error("Math Error: zip() requires same length.");
            ObjList* result = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(result);
            for (size_t i = 0; i < a.size(); ++i) {
                ObjList* pair = GcHeap::get().allocate<ObjList>();
                pair->vec.push_back(a[i]); pair->vec.push_back(b[i]); pair->is_frozen = true;
                result->vec.push_back(Value(pair));
            }
            return Value(result);
        }

        bool hasString = args[0].isObjType(ObjType::STRING_MATRIX) || args[1].isObjType(ObjType::STRING_MATRIX);
        bool hasComplex = args[0].isObjType(ObjType::COMPLEX_MATRIX) || args[1].isObjType(ObjType::COMPLEX_MATRIX);

        auto getLenAndFetch = [](const Value& v, int i, bool toString, bool toComplex) -> Value {
            if (v.isObjType(ObjType::STRING_MATRIX)) return Value(static_cast<ObjStringMatrix*>(v.asObj())->mat.rawData()[i]);
            if (v.isObjType(ObjType::COMPLEX_MATRIX)) {
                if (toString) { std::ostringstream oss; oss << Value(static_cast<ObjComplexMatrix*>(v.asObj())->mat.rawData()[i]); return Value(oss.str()); }
                return Value(static_cast<ObjComplexMatrix*>(v.asObj())->mat.rawData()[i]);
            }
            if (v.isObjType(ObjType::REAL_MATRIX)) {
                if (toString) { std::ostringstream oss; oss << Value(static_cast<ObjRealMatrix*>(v.asObj())->mat.rawData()[i]); return Value(oss.str()); }
                if (toComplex) return Value(Complex(static_cast<ObjRealMatrix*>(v.asObj())->mat.rawData()[i]));
                return Value(static_cast<ObjRealMatrix*>(v.asObj())->mat.rawData()[i]);
            }
            return v;
            };

        auto getLen = [](const Value& v) {
            if (v.isObjType(ObjType::STRING_MATRIX)) return static_cast<ObjStringMatrix*>(v.asObj())->mat.rawData().size();
            if (v.isObjType(ObjType::COMPLEX_MATRIX)) return static_cast<ObjComplexMatrix*>(v.asObj())->mat.rawData().size();
            if (v.isObjType(ObjType::REAL_MATRIX)) return static_cast<ObjRealMatrix*>(v.asObj())->mat.rawData().size();
            return size_t(1);
            };

        int nA = static_cast<int>(getLen(args[0])), nB = static_cast<int>(getLen(args[1]));
        if (nA != nB) throw std::runtime_error("Math Error: zip() vectors must have same length.");
        int n = nA;

        if (hasString) {
            std::vector<std::string> flat(n * 2);
            for (int i = 0; i < n; ++i) { flat[i * 2] = getLenAndFetch(args[0], i, true, false).asString(); flat[i * 2 + 1] = getLenAndFetch(args[1], i, true, false).asString(); }
            return Value(StringMatrix(n, 2, flat));
        }
        if (hasComplex) {
            std::vector<Complex> flat(n * 2);
            for (int i = 0; i < n; ++i) { flat[i * 2] = getLenAndFetch(args[0], i, false, true).asComplex(); flat[i * 2 + 1] = getLenAndFetch(args[1], i, false, true).asComplex(); }
            return Value(ComplexMatrix(n, 2, flat));
        }
        std::vector<double> flat(n * 2);
        for (int i = 0; i < n; ++i) { flat[i * 2] = getLenAndFetch(args[0], i, false, false).asDouble(); flat[i * 2 + 1] = getLenAndFetch(args[1], i, false, false).asDouble(); }
        return Value(RealMatrix(n, 2, flat));
        });

    reg("cat", {}, [](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("Runtime Error: cat() expects at least 1 argument.");
        bool hasList = false, hasStringMat = false, hasComplexMat = false;
        for (const auto& a : args) {
            if (a.isObjType(ObjType::LIST)) hasList = true;
            else if (a.isObjType(ObjType::STRING_MATRIX) || a.isString()) hasStringMat = true;
            else if (a.isObjType(ObjType::COMPLEX_MATRIX) || a.isComplex()) hasComplexMat = true;
            else if (!a.isObjType(ObjType::REAL_MATRIX) && !a.isNumber() && !a.isBigInt() && !a.isObjType(ObjType::FRACTION)) {
                hasList = true;
            }
        }
        if (hasList) {
            ObjList* result = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(result);
            for (const auto& a : args) {
                if (a.isObjType(ObjType::LIST)) { for (const auto& e : static_cast<ObjList*>(a.asObj())->vec) result->vec.push_back(e); }
                else result->vec.push_back(a);
            }
            return Value(result);
        }
        if (hasStringMat) {
            std::vector<std::string> flat;
            for (const auto& a : args) {
                if (a.isObjType(ObjType::STRING_MATRIX)) { auto d = static_cast<ObjStringMatrix*>(a.asObj())->mat.rawData(); flat.insert(flat.end(), d.begin(), d.end()); }
                else if (a.isObjType(ObjType::REAL_MATRIX)) { for (auto d : static_cast<ObjRealMatrix*>(a.asObj())->mat.rawData()) { std::ostringstream oss; oss << Value(d); flat.push_back(oss.str()); } }
                else if (a.isObjType(ObjType::COMPLEX_MATRIX)) { for (auto d : static_cast<ObjComplexMatrix*>(a.asObj())->mat.rawData()) { std::ostringstream oss; oss << Value(d); flat.push_back(oss.str()); } }
                else { std::ostringstream oss; oss << a; flat.push_back(oss.str()); }
            }
            return Value(StringMatrix(1, static_cast<int>(flat.size()), flat));
        }
        if (hasComplexMat) {
            std::vector<Complex> flat;
            for (const auto& a : args) {
                if (a.isObjType(ObjType::COMPLEX_MATRIX)) { auto d = static_cast<ObjComplexMatrix*>(a.asObj())->mat.rawData(); flat.insert(flat.end(), d.begin(), d.end()); }
                else if (a.isObjType(ObjType::REAL_MATRIX)) { for (auto d : static_cast<ObjRealMatrix*>(a.asObj())->mat.rawData()) flat.push_back(Complex(d)); }
                else { flat.push_back(a.asComplex()); }
            }
            return Value(ComplexMatrix(1, static_cast<int>(flat.size()), flat));
        }
        std::vector<double> flat;
        for (const auto& a : args) {
            if (a.isObjType(ObjType::REAL_MATRIX)) { auto d = static_cast<ObjRealMatrix*>(a.asObj())->mat.rawData(); flat.insert(flat.end(), d.begin(), d.end()); }
            else { flat.push_back(a.asDouble()); }
        }
        return Value(RealMatrix(1, static_cast<int>(flat.size()), flat));
        });
}

// =================================================================
// [24] Class 内省
// =================================================================
void BuiltinRegistry::registerIntrospection() {
    reg("isinstance", { 1, 2 }, [](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            // 单参数：检测是否为任意类的实例
            return Value(args[0].isInstance());
        }
        // 双参数：检测是否为指定类（含继承链）的实例
        if (!args[0].isInstance()) return Value(false);
        if (!args[1].isClass())
            throw std::runtime_error("Type Error: isinstance() second argument must be a class.");
        auto inst = args[0].asInstance();
        auto cls = static_cast<ObjClass*>(args[1].asObj());
        auto c = inst->classDef;
        while (c) { if (c == cls) return Value(true); c = c->parent; }
        return Value(false);
        });    
    reg("getClass", { 1 }, [](const std::vector<Value>& args) -> Value { if (!args[0].isInstance()) throw std::runtime_error("Type Error: getClass() expects an instance."); return Value(args[0].asInstance()->classDef); });
    reg("getParent", { 1 }, [](const std::vector<Value>& args) -> Value { ObjClass* cls = nullptr; if (args[0].isClass()) cls=static_cast<ObjClass*>(args[0].asObj()); else if (args[0].isInstance()) cls=args[0].asInstance()->classDef; else throw std::runtime_error("Type Error: getParent() expects a class or instance."); if (!cls->parent) return Value::none(); return Value(cls->parent); });
}

// =================================================================
// format() + type()
// =================================================================
void BuiltinRegistry::registerFormatType() {
    reg("format", {}, [](const std::vector<Value>& args) -> Value {
        if (args.size() < 1) throw std::runtime_error("Runtime Error: format() expects at least 1 argument.");
        if (!args[0].isString()) throw std::runtime_error("Type Error: format() first argument must be a format string.");
        ObjList* paList = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(paList);
        paList->vec = args;
        for (size_t i = 1; i < paList->vec.size(); ++i) {
            if (paList->vec[i].isInstance()) {
                auto inst = paList->vec[i].asInstance();
                auto [found, result] = invokeDunder(inst, DUNDER_STR, {});
                if (found) paList->vec[i] = result;
            }
        }
        std::vector<Value>& pa = paList->vec;
        std::string fmt = pa[0].asString(); std::string result; size_t argIdx = 1;
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] == '{' && i + 1 < fmt.size() && fmt[i + 1] == '}') { if (argIdx >= pa.size()) throw std::runtime_error("Runtime Error: format() too few arguments."); std::ostringstream oss; oss << pa[argIdx++]; result += oss.str(); i += 1; }
            else if (fmt[i] == '{' && i + 1 < fmt.size() && fmt[i + 1] == ':') { size_t close = fmt.find('}', i); if (close == std::string::npos) throw std::runtime_error("Runtime Error: format() unclosed '{'."); std::string spec = fmt.substr(i + 2, close - i - 2); if (argIdx >= pa.size()) throw std::runtime_error("Runtime Error: format() too few arguments."); char align = '\0'; int width = 0; int precision = -1; char type = '\0'; size_t si = 0; if (si < spec.size() && (spec[si] == '<' || spec[si] == '>' || spec[si] == '^')) align = spec[si++]; while (si < spec.size() && spec[si] >= '0' && spec[si] <= '9') width = width * 10 + (spec[si++] - '0'); if (si < spec.size() && spec[si] == '.') { si++; precision = 0; while (si < spec.size() && spec[si] >= '0' && spec[si] <= '9')precision = precision * 10 + (spec[si++] - '0'); } if (si < spec.size()) type = spec[si++]; std::ostringstream oss; if (type == 'f' || type == 'e') { double v = pa[argIdx].asDouble(); if (precision >= 0)oss << std::fixed << std::setprecision(precision); if (type == 'e')oss << std::scientific; oss << v; } else if (type == 'd')oss << static_cast<int64_t>(std::round(pa[argIdx].asDouble())); else if (type == 'x')oss << std::hex << static_cast<int64_t>(std::round(pa[argIdx].asDouble())); else { if (precision >= 0)oss << std::fixed << std::setprecision(precision); oss << pa[argIdx]; } std::string valStr = oss.str(); if (width > 0 && static_cast<int>(valStr.size()) < width) { int pad = width - static_cast<int>(valStr.size()); if (align == '<')valStr += std::string(pad, ' '); else if (align == '^') { int l = pad / 2, r = pad - l; valStr = std::string(l, ' ') + valStr + std::string(r, ' '); } else valStr = std::string(pad, ' ') + valStr; } result += valStr; argIdx++; i = close; }
            else { result += fmt[i]; }
        }
        return Value(result);
        });

    reg("type", { 1 }, [](const std::vector<Value>& args) -> Value {
        Value v = args[0];
        ObjTypeDef* td = GcHeap::get().allocate<ObjTypeDef>();
        if (v.isType()) {
            td->types.push_back(BuiltinType::TYPE_DEF);
        } else if (v.isClass()) {
            td->types.push_back(BuiltinType::CLASS);
        } else if (v.isInstance()) {
            td->types.push_back(v.asInstance()->classDef);
        } else {
            BuiltinType bt = BuiltinType::ANY;
            if (v.isInt32() || v.isBigInt()) bt = BuiltinType::INT;
            else if (v.isDouble()) bt = BuiltinType::FLOAT;
            else if (v.isString()) bt = BuiltinType::STRING;
            else if (v.isBool()) bt = BuiltinType::BOOL;
            else if (v.isNone()) bt = BuiltinType::NONE_TYPE;
            else if (v.isObjType(ObjType::LIST)) bt = BuiltinType::LIST;
            else if (v.isObjType(ObjType::DICT)) bt = BuiltinType::DICT;
            else if (v.isObjType(ObjType::SET)) bt = BuiltinType::SET;
            else if (v.isObjType(ObjType::FRACTION)) bt = BuiltinType::FRACTION;
            else if (v.isObjType(ObjType::COMPLEX)) bt = BuiltinType::COMPLEX;
            else if (v.isObjType(ObjType::BASENUM)) bt = BuiltinType::BASENUM;
            else if (v.isObjType(ObjType::SYMBOLIC)) bt = BuiltinType::SYMBOLIC;
            else if (v.isObjType(ObjType::REAL_MATRIX)) bt = BuiltinType::REALMAT;
            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) bt = BuiltinType::COMPLEXMAT;
            else if (v.isObjType(ObjType::STRING_MATRIX)) bt = BuiltinType::STRINGMAT;
            else if (v.isFunctionClosure()) bt = BuiltinType::FUNC;
            else if (v.isObjType(ObjType::NAMESPACE)) bt = BuiltinType::NAMESPACE;
            td->types.push_back(bt);
        }
        td->normalize();
        return Value(td);
        });
}

// =================================================================
// [HOF] 高阶函数（使用通用 callClosure）
// =================================================================

void BuiltinRegistry::registerHigherOrder() {

    auto applyCore = [this](const Value& argList, ObjClosure* cl) -> Value {
        ObjList* unpackedList = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(unpackedList);
        
        Value iterable = argList;
        if (helpers::iterateIterable(iterable, [&](const Value& nextVal) {
            unpackedList->vec.push_back(nextVal);
            return true;
        })) {
            return safeCallFunction(cl, unpackedList->vec);
        }

        if (iterable.isObjType(ObjType::LIST)) {
            for (const auto& e : static_cast<ObjList*>(iterable.asObj())->vec) unpackedList->vec.push_back(e);
        } else if (iterable.isObjType(ObjType::REAL_MATRIX)) {
            auto& m = static_cast<ObjRealMatrix*>(iterable.asObj())->mat;
            if (m.getRows() != 1 && m.getCols() != 1) throw std::runtime_error("Type Error: apply() expects 1D vector.");
            for (const auto& d : m.rawData()) unpackedList->vec.push_back(Value(d));
        } else if (iterable.isObjType(ObjType::COMPLEX_MATRIX)) {
            auto& m = static_cast<ObjComplexMatrix*>(iterable.asObj())->mat;
            if (m.getRows() != 1 && m.getCols() != 1) throw std::runtime_error("Type Error: apply() expects 1D vector.");
            for (const auto& d : m.rawData()) unpackedList->vec.push_back(Value(d));
        } else if (iterable.isObjType(ObjType::STRING_MATRIX)) {
            auto& m = static_cast<ObjStringMatrix*>(iterable.asObj())->mat;
            if (m.getRows() != 1 && m.getCols() != 1) throw std::runtime_error("Type Error: apply() expects 1D vector.");
            for (const auto& d : m.rawData()) unpackedList->vec.push_back(Value(d));
        } else if (iterable.isString()) {
            ObjString* objStr = iterable.asObjString();
            const std::string& str = objStr->str;
            if (objStr->isAscii) {
                for (char c : str) unpackedList->vec.push_back(Value(std::string(1, c)));
            } else {
                size_t len = objStr->charLength;
                for (size_t i = 0; i < len; ++i) unpackedList->vec.push_back(Value(utf8::substring(str, i, 1, false)));
            }
        } else {
            throw std::runtime_error("Type Error: apply() expects a function and an iterable argument list/vector.");
        }
        
        return safeCallFunction(cl, unpackedList->vec);
    };

    reg("apply", { 1, 2 }, [applyCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            Value capturedFn = args[0];
            if (!capturedFn.isFunctionClosure()) throw std::runtime_error("Type Error: apply() currying expects a function.");
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "apply_curried", nullptr);
            bound->boundSelf = capturedFn; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([applyCore](const std::vector<Value>& innerArgs) -> Value {
                return applyCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction());
            });
            return Value(bound);
        }
        return applyCore(args[0], args[1].asFunction());
        });

    auto mapCore = [this](const Value& argList, ObjClosure* cl) -> Value {
        if (!cl->acceptsArgCount(1)) throw std::runtime_error("Runtime Error: map() requires a single-parameter function.");

        Value iterable = argList;
        ObjList* result = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(result);
        if (helpers::iterateIterable(iterable, [&](const Value& nextVal) {
            jc::checkInterrupt();
            result->vec.push_back(safeCallFunction(cl, { nextVal }));
            return true;
        })) {
            return Value(result);
        }

        if (iterable.isObjType(ObjType::LIST)) {
            ObjList* listResult = GcHeap::get().allocate<ObjList>();
            GcObjGuard listGuard(listResult);
            for (const auto& e : static_cast<ObjList*>(iterable.asObj())->vec) {
                jc::checkInterrupt();
                listResult->vec.push_back(safeCallFunction(cl, { e }));
            }
            return Value(listResult);
        } else if (iterable.isObjType(ObjType::REAL_MATRIX) || iterable.isObjType(ObjType::COMPLEX_MATRIX) || iterable.isObjType(ObjType::STRING_MATRIX)) {
            int rows = 1, cols = 1;
            std::vector<Value> flatVals;
            if (iterable.isObjType(ObjType::REAL_MATRIX)) {
                auto& m = static_cast<ObjRealMatrix*>(iterable.asObj())->mat;
                rows = m.getRows(); cols = m.getCols();
                for (auto d : m.rawData()) flatVals.push_back(Value(d));
            } else if (iterable.isObjType(ObjType::COMPLEX_MATRIX)) {
                auto& m = static_cast<ObjComplexMatrix*>(iterable.asObj())->mat;
                rows = m.getRows(); cols = m.getCols();
                for (auto c : m.rawData()) flatVals.push_back(Value(c));
            } else {
                auto& m = static_cast<ObjStringMatrix*>(iterable.asObj())->mat;
                rows = m.getRows(); cols = m.getCols();
                for (auto s : m.rawData()) flatVals.push_back(Value(s));
            }

            ObjList* fallback = GcHeap::get().allocate<ObjList>();
            GcObjGuard fallbackGuard(fallback);
            bool typeConflict = false;
            bool hasString = false, hasComp = false;
            std::vector<double> rd; std::vector<Complex> rc; std::vector<std::string> rs;

            for (size_t i = 0; i < flatVals.size(); ++i) {
                jc::checkInterrupt();
                Value y = safeCallFunction(cl, { flatVals[i] });
                if (i == 0) {
                    if (y.isString()) hasString = true;
                    else if (y.isComplex()) hasComp = true;
                    else if (!y.isNumber() && !y.isBigInt() && !y.isObjType(ObjType::FRACTION)) typeConflict = true;
                }
                if (typeConflict) { fallback->vec.push_back(y); }
                else if (hasString) {
                    if (y.isString()) rs.push_back(y.asString());
                    else { std::ostringstream oss; oss << y; rs.push_back(oss.str()); }
                }
                else if (hasComp) {
                    try { rc.push_back(y.asComplex()); }
                    catch (...) { typeConflict = true; fallback->vec.clear(); for (auto r : rc) fallback->vec.push_back(Value(r)); fallback->vec.push_back(y); }
                }
                else {
                    try { rd.push_back(y.asDouble()); }
                    catch (...) {
                        try { rc.clear(); for (auto d : rd) rc.push_back(Complex(d)); rc.push_back(y.asComplex()); hasComp = true; }
                        catch (...) { typeConflict = true; fallback->vec.clear(); for (auto d : rd) fallback->vec.push_back(Value(d)); fallback->vec.push_back(y); }
                    }
                }
            }

            if (typeConflict) {
                for (size_t i = fallback->vec.size(); i < flatVals.size(); ++i) {
                    jc::checkInterrupt();
                    fallback->vec.push_back(safeCallFunction(cl, { flatVals[i] }));
                }
                return Value(fallback);
            }
            if (hasString) return Value(StringMatrix(rows, cols, rs));
            if (hasComp) return Value(ComplexMatrix(rows, cols, rc));
            return Value(RealMatrix(rows, cols, rd));
        }
        throw std::runtime_error("Type Error: map() expects a vector/matrix/list.");
    };

    reg("map", { 1, 2 }, [mapCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            Value capturedFn = args[0];
            if (!capturedFn.isFunctionClosure()) throw std::runtime_error("Type Error: map() currying expects a function.");
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "map_curried", nullptr);
            bound->boundSelf = capturedFn; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([mapCore](const std::vector<Value>& innerArgs) -> Value {
                return mapCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction());
            });
            return Value(bound);
        }
        return mapCore(args[0], args[1].asFunction());
        });

    auto filterCore = [this](const Value& argList, ObjClosure* cl) -> Value {
        if (!cl->acceptsArgCount(1)) throw std::runtime_error("Runtime Error: filter() requires a single-parameter function.");

        Value iterable = argList;
        ObjList* result = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(result);
        if (helpers::iterateIterable(iterable, [&](const Value& nextVal) {
            jc::checkInterrupt();
            if (safeCallFunction(cl, { nextVal }).truthy()) result->vec.push_back(nextVal);
            return true;
        })) {
            return Value(result);
        }

        if (iterable.isObjType(ObjType::LIST)) {
            ObjList* listResult = GcHeap::get().allocate<ObjList>();
            GcObjGuard listGuard(listResult);
            for (const auto& e : static_cast<ObjList*>(iterable.asObj())->vec) {
                jc::checkInterrupt();
                if (safeCallFunction(cl, { e }).truthy()) listResult->vec.push_back(e);
            }
            return Value(listResult);
        } else if (iterable.isObjType(ObjType::REAL_MATRIX)) {
            std::vector<double> matResult;
            for (const auto& x : static_cast<ObjRealMatrix*>(iterable.asObj())->mat.rawData()) {
                jc::checkInterrupt();
                if (safeCallFunction(cl, { Value(x) }).truthy()) matResult.push_back(x);
            }
            int n = static_cast<int>(matResult.size());
            if (n == 0) return Value(RealMatrix(1, 0));
            return Value(RealMatrix(1, n, matResult));
        } else if (iterable.isObjType(ObjType::COMPLEX_MATRIX)) {
            std::vector<Complex> matResult;
            for (const auto& x : static_cast<ObjComplexMatrix*>(iterable.asObj())->mat.rawData()) {
                jc::checkInterrupt();
                if (safeCallFunction(cl, { Value(x) }).truthy()) matResult.push_back(x);
            }
            int n = static_cast<int>(matResult.size());
            if (n == 0) return Value(ComplexMatrix(1, 0));
            return Value(ComplexMatrix(1, n, matResult));
        } else if (iterable.isObjType(ObjType::STRING_MATRIX)) {
            std::vector<std::string> matResult;
            for (const auto& x : static_cast<ObjStringMatrix*>(iterable.asObj())->mat.rawData()) {
                jc::checkInterrupt();
                if (safeCallFunction(cl, { Value(x) }).truthy()) matResult.push_back(x);
            }
            int n = static_cast<int>(matResult.size());
            if (n == 0) return Value(StringMatrix(1, 0));
            return Value(StringMatrix(1, n, matResult));
        }
        throw std::runtime_error("Type Error: filter() expects a vector/matrix/list.");
    };

    reg("filter", { 1, 2 }, [filterCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            Value capturedFn = args[0];
            if (!capturedFn.isFunctionClosure()) throw std::runtime_error("Type Error: filter() currying expects a function.");
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "filter_curried", nullptr);
            bound->boundSelf = capturedFn; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([filterCore](const std::vector<Value>& innerArgs) -> Value {
                return filterCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction());
            });
            return Value(bound);
        }
        return filterCore(args[0], args[1].asFunction());
        });

    auto reduceCore = [this](const Value& argList, ObjClosure* cl, const Value& initVal) -> Value {
        if (!cl->acceptsArgCount(2)) throw std::runtime_error("Runtime Error: reduce() requires a two-parameter function.");

        Value iterable = argList;
        Value acc;
        bool first = true;
        if (!initVal.isNone()) { acc = initVal; first = false; }
        GcValueGuard guard(acc);
        
        if (helpers::iterateIterable(iterable, [&](const Value& nextVal) {
            jc::checkInterrupt();
            if (first) { acc = nextVal; first = false; }
            else acc = safeCallFunction(cl, { acc, nextVal });
            return true;
        })) {
            if (first) throw std::runtime_error("Runtime Error: reduce() on empty.");
            return acc;
        }

        if (iterable.isObjType(ObjType::LIST)) {
            auto l = static_cast<ObjList*>(iterable.asObj());
            Value listAcc; size_t startIdx = 0;
            if (!initVal.isNone()) { listAcc = initVal; }
            else { if (l->vec.empty()) throw std::runtime_error("Runtime Error: reduce() on empty."); listAcc = l->vec[0]; startIdx = 1; }
            GcValueGuard listGuard(listAcc);
            for (size_t i = startIdx; i < l->vec.size(); ++i) { 
                jc::checkInterrupt(); 
                listAcc = safeCallFunction(cl, { listAcc, l->vec[i] }); 
            }
            return listAcc;
        } else if (iterable.isObjType(ObjType::REAL_MATRIX) || iterable.isObjType(ObjType::COMPLEX_MATRIX) || iterable.isObjType(ObjType::STRING_MATRIX)) {
            std::vector<Value> flatVals;
            if (iterable.isObjType(ObjType::REAL_MATRIX)) {
                for (auto d : static_cast<ObjRealMatrix*>(iterable.asObj())->mat.rawData()) flatVals.push_back(Value(d));
            } else if (iterable.isObjType(ObjType::COMPLEX_MATRIX)) {
                for (auto c : static_cast<ObjComplexMatrix*>(iterable.asObj())->mat.rawData()) flatVals.push_back(Value(c));
            } else {
                for (auto s : static_cast<ObjStringMatrix*>(iterable.asObj())->mat.rawData()) flatVals.push_back(Value(s));
            }
            Value matAcc; size_t startIdx = 0;
            if (!initVal.isNone()) { matAcc = initVal; }
            else { if (flatVals.empty()) throw std::runtime_error("Runtime Error: reduce() on empty."); matAcc = flatVals[0]; startIdx = 1; }
            GcValueGuard matGuard(matAcc);
            for (size_t i = startIdx; i < flatVals.size(); ++i) { 
                jc::checkInterrupt(); 
                matAcc = safeCallFunction(cl, { matAcc, flatVals[i] }); 
            }
            return matAcc;
        }
        throw std::runtime_error("Type Error: reduce() expects a vector/matrix/list.");
    };

    reg("reduce", { 1, 2, 3 }, [reduceCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1 || (args.size() == 2 && args[0].isFunctionClosure())) {
            Value capturedFn = args[0];
            if (!capturedFn.isFunctionClosure()) throw std::runtime_error("Type Error: reduce() currying expects a function.");
            Value capturedInit = args.size() == 2 ? args[1] : Value::none();
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "reduce_curried", nullptr);
            bound->boundSelf = capturedFn;   // ★ 让 GC 追踪
            bound->boundClass = capturedInit; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([reduceCore](const std::vector<Value>& innerArgs) -> Value {
                return reduceCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction(), helpers::nativeClassStack.back());
            });
            return Value(bound);
        }
        return reduceCore(args[0], args[1].asFunction(), args.size() == 3 ? args[2] : Value::none());
        });

    auto iterateAndCheck = [this](const Value& argList, ObjClosure* cl, auto checkFn) -> Value {
        Value iterable = argList;
        bool found = false;
        if (helpers::iterateIterable(iterable, [&](const Value& nextVal) {
            jc::checkInterrupt();
            if (checkFn(safeCallFunction(cl, { nextVal }).truthy())) {
                found = true;
                return false; // break
            }
            return true;
        })) {
            return Value(found);
        }
        if (iterable.isObjType(ObjType::LIST)) {
            for (const auto& e : static_cast<ObjList*>(iterable.asObj())->vec) {
                jc::checkInterrupt();
                if (checkFn(safeCallFunction(cl, { e }).truthy())) return Value(true);
            }
        } else if (iterable.isObjType(ObjType::REAL_MATRIX)) {
            for (const auto& x : static_cast<ObjRealMatrix*>(iterable.asObj())->mat.rawData()) {
                jc::checkInterrupt();
                if (checkFn(safeCallFunction(cl, { Value(x) }).truthy())) return Value(true);
            }
        } else if (iterable.isObjType(ObjType::COMPLEX_MATRIX)) {
            for (const auto& x : static_cast<ObjComplexMatrix*>(iterable.asObj())->mat.rawData()) {
                jc::checkInterrupt();
                if (checkFn(safeCallFunction(cl, { Value(x) }).truthy())) return Value(true);
            }
        } else if (iterable.isObjType(ObjType::STRING_MATRIX)) {
            for (const auto& x : static_cast<ObjStringMatrix*>(iterable.asObj())->mat.rawData()) {
                jc::checkInterrupt();
                if (checkFn(safeCallFunction(cl, { Value(x) }).truthy())) return Value(true);
            }
        } else {
            throw std::runtime_error("Type Error: expects a vector/list.");
        }
        return Value(false);
    };

    auto anyCore = [iterateAndCheck](const Value& argList, ObjClosure* cl) -> Value {
        if (!cl->acceptsArgCount(1)) throw std::runtime_error("Runtime Error: any() requires a single-parameter function.");
        return iterateAndCheck(argList, cl, [](bool res) { return res; });
    };

    reg("any", { 1, 2 }, [anyCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            Value capturedFn = args[0];
            if (!capturedFn.isFunctionClosure()) throw std::runtime_error("Type Error: any() currying expects a function.");
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "any_curried", nullptr);
            bound->boundSelf = capturedFn; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([anyCore](const std::vector<Value>& innerArgs) -> Value {
                return anyCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction());
            });
            return Value(bound);
        }
        return anyCore(args[0], args[1].asFunction());
        });

    auto allCore = [iterateAndCheck](const Value& argList, ObjClosure* cl) -> Value {
        if (!cl->acceptsArgCount(1)) throw std::runtime_error("Runtime Error: all() requires a single-parameter function.");
        Value res = iterateAndCheck(argList, cl, [](bool res) { return !res; });
        return Value(!res.asBool());
    };

    reg("all", { 1, 2 }, [allCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            Value capturedFn = args[0];
            if (!capturedFn.isFunctionClosure()) throw std::runtime_error("Type Error: all() currying expects a function.");
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "all_curried", nullptr);
            bound->boundSelf = capturedFn; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([allCore](const std::vector<Value>& innerArgs) -> Value {
                return allCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction());
            });
            return Value(bound);
        }
        return allCore(args[0], args[1].asFunction());
        });

    auto countIfCore = [](const Value& argList, ObjClosure* cl) -> Value {
        if (!cl->acceptsArgCount(1)) throw std::runtime_error("Runtime Error: countIf() requires a single-parameter function.");
        int c = 0;
        if (helpers::iterateIterable(argList, [&](const Value& nextVal) {
            jc::checkInterrupt();
            if (safeCallFunction(cl, { nextVal }).truthy()) c++;
            return true;
        })) {
            return Value::fromInt32(c);
        }
        if (argList.isObjType(ObjType::LIST)) {
            for (const auto& e : static_cast<ObjList*>(argList.asObj())->vec) { jc::checkInterrupt(); if (safeCallFunction(cl, { e }).truthy()) c++; }
        } else if (argList.isObjType(ObjType::REAL_MATRIX)) {
            for (const auto& x : static_cast<ObjRealMatrix*>(argList.asObj())->mat.rawData()) { jc::checkInterrupt(); if (safeCallFunction(cl, { Value(x) }).truthy()) c++; }
        } else if (argList.isObjType(ObjType::COMPLEX_MATRIX)) {
            for (const auto& x : static_cast<ObjComplexMatrix*>(argList.asObj())->mat.rawData()) { jc::checkInterrupt(); if (safeCallFunction(cl, { Value(x) }).truthy()) c++; }
        } else if (argList.isObjType(ObjType::STRING_MATRIX)) {
            for (const auto& x : static_cast<ObjStringMatrix*>(argList.asObj())->mat.rawData()) { jc::checkInterrupt(); if (safeCallFunction(cl, { Value(x) }).truthy()) c++; }
        } else {
            throw std::runtime_error("Type Error: countIf() expects a vector/list.");
        }
        return Value::fromInt32(c);
    };

    reg("countIf", { 1, 2 }, [countIfCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            Value capturedFn = args[0];
            if (!capturedFn.isFunctionClosure()) throw std::runtime_error("Type Error: countIf() currying expects a function.");
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "countIf_curried", nullptr);
            bound->boundSelf = capturedFn; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([countIfCore](const std::vector<Value>& innerArgs) -> Value {
                return countIfCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction());
            });
            return Value(bound);
        }
        return countIfCore(args[0], args[1].asFunction());
        });

    auto sortCore = [this](const Value& argList, ObjClosure* cmp) -> Value {
        Value arg = argList;
        if (arg.isInstance() && helpers::hasDunder(arg, DUNDER_ITER)) {
            ObjList* L = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(L);
            helpers::iterateIterable(arg, [&](const Value& nextVal) {
                L->vec.push_back(nextVal);
                return true;
            });
            arg = Value(L);
        }
        if (cmp) {
            if (!cmp->acceptsArgCount(2)) throw std::runtime_error("Runtime Error: sort() comparator must be a 2-parameter function.");
            if (arg.isObjType(ObjType::LIST)) {
                ObjList* L = GcHeap::get().allocate<ObjList>();
                GcObjGuard guard(L);
                L->vec = static_cast<ObjList*>(arg.asObj())->vec;
                std::stable_sort(L->vec.begin(), L->vec.end(), [&](const Value& a, const Value& b) {
                    return safeCallFunction(cmp, { a, b }).truthy();
                });
                return Value(L);
            } else if (arg.isObjType(ObjType::REAL_MATRIX)) {
                auto f = static_cast<ObjRealMatrix*>(arg.asObj())->mat.rawData();
                std::stable_sort(f.begin(), f.end(), [&](const auto& a, const auto& b) { return safeCallFunction(cmp, { Value(a), Value(b) }).truthy(); });
                return Value(RealMatrix(1, static_cast<int>(f.size()), f));
            } else if (arg.isObjType(ObjType::COMPLEX_MATRIX)) {
                auto f = static_cast<ObjComplexMatrix*>(arg.asObj())->mat.rawData();
                std::stable_sort(f.begin(), f.end(), [&](const auto& a, const auto& b) { return safeCallFunction(cmp, { Value(a), Value(b) }).truthy(); });
                return Value(ComplexMatrix(1, static_cast<int>(f.size()), f));
            } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
                auto f = static_cast<ObjStringMatrix*>(arg.asObj())->mat.rawData();
                std::stable_sort(f.begin(), f.end(), [&](const auto& a, const auto& b) { return safeCallFunction(cmp, { Value(a), Value(b) }).truthy(); });
                return Value(StringMatrix(1, static_cast<int>(f.size()), f));
            }
            throw std::runtime_error("Type Error: sort() expects a vector or list.");
        } else {
            if (arg.isObjType(ObjType::REAL_MATRIX)) {
                auto f = static_cast<ObjRealMatrix*>(arg.asObj())->mat.rawData();
                std::sort(f.begin(), f.end()); return Value(RealMatrix(1, static_cast<int>(f.size()), f));
            } else if (arg.isObjType(ObjType::STRING_MATRIX)) {
                auto f = static_cast<ObjStringMatrix*>(arg.asObj())->mat.rawData();
                std::sort(f.begin(), f.end()); return Value(StringMatrix(1, static_cast<int>(f.size()), f));
            } else if (arg.isObjType(ObjType::LIST)) {
                ObjList* L = GcHeap::get().allocate<ObjList>();
                GcObjGuard guard(L);
                L->vec = static_cast<ObjList*>(arg.asObj())->vec;
                std::stable_sort(L->vec.begin(), L->vec.end(), [](const Value& a, const Value& b) {
                    return helpers::checkLess(a, b);
                });
                return Value(L);
            }
            throw std::runtime_error("Type Error: sort() without comparator expects an array or list.");
        }
    };

    reg("sort", { 1, 2 }, [sortCore](const std::vector<Value>& args) -> Value {
        if (args.size() == 1 && args[0].isFunctionClosure()) {
            Value capturedFn = args[0];
            auto bound = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"v"}, std::vector<bool>{false}, "sort_curried", nullptr);
            bound->boundSelf = capturedFn; // ★ 让 GC 追踪
            bound->nativeFn = std::make_any<NativeCallable>([sortCore](const std::vector<Value>& innerArgs) -> Value {
                return sortCore(innerArgs[0], helpers::nativeSelfStack.back().asFunction());
            });
            return Value(bound);
        }
        if (args.size() == 1) return sortCore(args[0], nullptr);
        return sortCore(args[0], args[1].asFunction());
        });

}

// =================================================================
// [Phase 2] 微积分引擎
// =================================================================
void BuiltinRegistry::registerCalculus() {

    // 通用 eval 辅助：调用单参数函数 f(x)
    auto evalFunc = [](ObjClosure* cl, double x) -> double {
        return safeCallFunction(cl, { Value(x) }).asDouble();
        };

    reg("solveE", { 2 }, [evalFunc](const std::vector<Value>& args) -> Value {
        auto cl = args[0].asFunction(); double x = args[1].asDouble(); double h = 1e-5;
        for (int i = 0; i < 1000; ++i) {
            jc::checkInterrupt();
            double y = evalFunc(cl, x);
            if (Tol::clean(y, std::max(1.0, std::abs(x)), 1e7) == 0.0) return Value(x);
            double df = (evalFunc(cl, x + h) - evalFunc(cl, x - h)) / (2 * h);
            if (df == 0.0) x += 1e-4; else x -= y / df;
        }
        throw std::runtime_error("Math Error: Equation solver did not converge.");
        });

    reg("table", {}, [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) throw std::runtime_error("Runtime Error: table() expects at least 2 arguments.");
        auto cl = args[0].asFunction();
        int k = static_cast<int>(cl->paramNames.size());
        // ★ 如果参数名为空（VM 闭包可能如此），从 arity 推断
        if (k == 0 && cl->isNative()) {
            // 尝试从 maxArgs 推断
            k = cl->maxArgs();
            if (k == 0) k = 1; // 默认单参数
        }

        auto evalRow = [&](const std::vector<Value>& rowArgs,
            std::vector<double>& res_d, std::vector<Complex>& res_c, bool& hasComplex) {
                jc::checkInterrupt();
                Value y_val = safeCallFunction(cl, rowArgs);
                if (!hasComplex) {
                    try { res_d.push_back(y_val.asDouble()); }
                    catch (...) { hasComplex = true; for (double d : res_d) res_c.push_back(Complex(d)); res_c.push_back(y_val.asComplex()); }
                }
                else { res_c.push_back(y_val.asComplex()); }
            };

        if (args.size() == 4 && k == 1 &&
            !args[1].isObjType(ObjType::REAL_MATRIX) &&
            !args[1].isObjType(ObjType::COMPLEX_MATRIX)) {
            double start = args[1].asDouble(), step = args[2].asDouble();
            int count = static_cast<int>(std::round(args[3].asDouble()));
            if (count <= 0) throw std::runtime_error("Math Error: count must be positive.");
            std::vector<double> rd; std::vector<Complex> rc; bool hc = false;
            for (int i = 0; i < count; ++i) evalRow({ Value(start + i * step) }, rd, rc, hc);
            if (hc) return Value(ComplexMatrix(count, 1, rc));
            return Value(RealMatrix(count, 1, rd));
        }
        if (args.size() == 2) {
            int N = 0; std::vector<double> rd; std::vector<Complex> rc; bool hc = false;
            if (args[1].isObjType(ObjType::REAL_MATRIX)) {
                const auto& M = static_cast<ObjRealMatrix*>(args[1].asObj())->mat;
                if (M.getCols() != k) throw std::runtime_error("Math Error: Matrix columns must match function parameter count.");
                N = M.getRows();
                for (int i = 0; i < N; ++i) { std::vector<Value> row; for (int j = 0; j < k; ++j) row.push_back(Value(M(i, j))); evalRow(row, rd, rc, hc); }
            }
            else if (args[1].isObjType(ObjType::COMPLEX_MATRIX)) {
                const auto& M = static_cast<ObjComplexMatrix*>(args[1].asObj())->mat;
                if (M.getCols() != k) throw std::runtime_error("Math Error: Matrix columns must match function parameter count.");
                N = M.getRows();
                for (int i = 0; i < N; ++i) { std::vector<Value> row; for (int j = 0; j < k; ++j) row.push_back(Value(M(i, j))); evalRow(row, rd, rc, hc); }
            }
            else throw std::runtime_error("Type Error: Expected a matrix.");
            if (N == 0) return Value(RealMatrix(0, 0));
            if (hc) return Value(ComplexMatrix(N, 1, rc));
            return Value(RealMatrix(N, 1, rd));
        }
        if (args.size() == static_cast<size_t>(k + 1)) {
            int N = -1;
            for (int i = 1; i <= k; ++i) {
                if (args[i].isObjType(ObjType::REAL_MATRIX)) { if (static_cast<ObjRealMatrix*>(args[i].asObj())->mat.getCols() != 1) throw std::runtime_error("Math Error: Arguments must be column vectors."); if (N == -1) N = static_cast<ObjRealMatrix*>(args[i].asObj())->mat.getRows(); else if (N != static_cast<ObjRealMatrix*>(args[i].asObj())->mat.getRows()) throw std::runtime_error("Math Error: Vectors must have same length."); }
                else if (args[i].isObjType(ObjType::COMPLEX_MATRIX)) { if (static_cast<ObjComplexMatrix*>(args[i].asObj())->mat.getCols() != 1) throw std::runtime_error("Math Error: Arguments must be column vectors."); if (N == -1) N = static_cast<ObjComplexMatrix*>(args[i].asObj())->mat.getRows(); else if (N != static_cast<ObjComplexMatrix*>(args[i].asObj())->mat.getRows()) throw std::runtime_error("Math Error: Vectors must have same length."); }
                else throw std::runtime_error("Type Error: Expected column vectors.");
            }
            if (N <= 0) return Value(RealMatrix(0, 0));
            std::vector<double> rd; std::vector<Complex> rc; bool hc = false;
            for (int r = 0; r < N; ++r) { std::vector<Value> row; for (int c = 1; c <= k; ++c) { if (args[c].isObjType(ObjType::REAL_MATRIX)) row.push_back(Value(static_cast<ObjRealMatrix*>(args[c].asObj())->mat(r, 0))); else row.push_back(Value(static_cast<ObjComplexMatrix*>(args[c].asObj())->mat(r, 0))); } evalRow(row, rd, rc, hc); }
            if (hc) return Value(ComplexMatrix(N, 1, rc));
            return Value(RealMatrix(N, 1, rd));
        }
        throw std::runtime_error("Runtime Error: Argument count mismatch.");
        });
}

// =================================================================
// [Phase 2] 文件 I/O 引擎
// =================================================================
void BuiltinRegistry::registerFileIO() {

    reg("readFile", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: readFile() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::ifstream file(path);
        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot open file '" + path + "'.");
        std::ostringstream oss; oss << file.rdbuf(); file.close();
        return Value(oss.str());
        });

    reg("writeFile", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: writeFile() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::string content;
        if (args[1].isString()) content = args[1].asString();
        else { std::ostringstream oss; oss << args[1]; content = oss.str(); }
        std::ofstream file(path);
        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot write to file '" + path + "'.");
        file << content; file.close();
        return Value::none();
        });

    reg("appendFile", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: appendFile() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::string content;
        if (args[1].isString()) content = args[1].asString();
        else { std::ostringstream oss; oss << args[1]; content = oss.str(); }
        std::ofstream file(path, std::ios::app);
        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot append to file '" + path + "'.");
        file << content; file.close();
        return Value::none();
        });

    reg("readLines", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: readLines() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::ifstream file(path);
        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot open file '" + path + "'.");
        ObjList* L = GcHeap::get().allocate<ObjList>(); std::string line;
        GcObjGuard guard(L);
        while (std::getline(file, line)) {
            jc::checkInterrupt();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            L->vec.push_back(Value(line));
        }
        file.close(); return Value(L);
        });

    reg("writeLines", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: writeLines() expects a string path.");
        if (!args[1].isObjType(ObjType::LIST))
            throw std::runtime_error("Type Error: writeLines() expects a List.");
        std::string path = safeResolvePath(args[0].asString());
        const auto& L = static_cast<ObjList*>(args[1].asObj())->vec;
        std::ofstream file(path);
        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot write to file '" + path + "'.");
        for (const auto& v : L) {
            if (v.isString()) file << v.asString() << "\n";
            else file << v << "\n";
        }
        file.close(); return Value::none();
        });

    reg("fileExists", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: fileExists() expects a string path.");
        return Value(std::filesystem::exists(safeResolvePath(args[0].asString())));
        });

    reg("deleteFile", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: deleteFile() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        if (!std::filesystem::exists(path))
            throw std::runtime_error("IO Error: File '" + path + "' does not exist.");
        std::filesystem::remove(path);
        return Value::none();
        });

    reg("fileSize", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw std::runtime_error("Type Error: fileSize() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        if (!std::filesystem::exists(path))
            throw std::runtime_error("IO Error: File '" + path + "' does not exist.");
        return Value(BigInt(static_cast<int64_t>(std::filesystem::file_size(path))));
        });

    reg("listDir", { 0, 1 }, [](const std::vector<Value>& args) -> Value {
        std::string dir;
        if (args.size() == 1) {
            if (!args[0].isString())
                throw std::runtime_error("Type Error: listDir() expects a string path.");
            dir = safeResolvePath(args[0].asString());
        }
        else {
            dir = std::filesystem::current_path().string();
        }
        if (!std::filesystem::exists(dir))
            throw std::runtime_error("IO Error: Directory '" + dir + "' does not exist.");
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        for (const auto& entry : std::filesystem::directory_iterator(dir))
            L->vec.push_back(Value(entry.path().filename().string()));
        return Value(L);
        });

    // --- CSV ---
    reg("readCSV", { 1, 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: readCSV() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::string delim = ",";
        if (args.size() == 2) { if (!args[1].isString()) throw std::runtime_error("Type Error: readCSV() delimiter must be a string."); delim = args[1].asString(); }
        std::ifstream file(path); if (!file.is_open()) throw std::runtime_error("IO Error: Cannot open file '" + path + "'.");
        ObjList* rows = GcHeap::get().allocate<ObjList>(); std::string line;
        GcObjGuard guard(rows);
        while (std::getline(file, line)) { jc::checkInterrupt(); if (!line.empty() && line.back() == '\r') line.pop_back(); ObjList* row = GcHeap::get().allocate<ObjList>(); GcObjGuard rowGuard(row); size_t pos = 0, found; while ((found = line.find(delim, pos)) != std::string::npos) { row->vec.push_back(Value(line.substr(pos, found - pos))); pos = found + delim.size(); } row->vec.push_back(Value(line.substr(pos))); row->is_frozen = true; rows->vec.push_back(Value(row)); }
        file.close(); return Value(rows);
        });

    reg("readCSVMat", { 1, 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: readCSVMat() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::string delim = ",";
        if (args.size() == 2) { if (!args[1].isString()) throw std::runtime_error("Type Error: readCSVMat() delimiter must be a string."); delim = args[1].asString(); }
        std::ifstream file(path); if (!file.is_open()) throw std::runtime_error("IO Error: Cannot open file '" + path + "'.");
        std::vector<std::vector<std::string>> rowsData; std::string line; size_t maxCols = 0;
        while (std::getline(file, line)) { jc::checkInterrupt(); if (!line.empty() && line.back() == '\r') line.pop_back(); std::vector<std::string> row; size_t pos = 0, found; while ((found = line.find(delim, pos)) != std::string::npos) { row.push_back(line.substr(pos, found - pos)); pos = found + delim.size(); } row.push_back(line.substr(pos)); if (row.size() > maxCols) maxCols = row.size(); rowsData.push_back(row); }
        file.close(); std::vector<std::string> flat; for (auto& row : rowsData) { row.resize(maxCols, ""); flat.insert(flat.end(), row.begin(), row.end()); }
        return Value(StringMatrix(static_cast<int>(rowsData.size()), static_cast<int>(maxCols), flat));
        });

    reg("parseCSVNum", { 1, 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: parseCSVNum() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::string delim = ",";
        if (args.size() == 2) { if (!args[1].isString()) throw std::runtime_error("Type Error: parseCSVNum() delimiter must be a string."); delim = args[1].asString(); }
        std::ifstream file(path); if (!file.is_open()) throw std::runtime_error("IO Error: Cannot open file '" + path + "'.");
        std::vector<std::vector<double>> rowsData; std::string line; size_t maxCols = 0;
        while (std::getline(file, line)) { jc::checkInterrupt(); if (!line.empty() && line.back() == '\r') line.pop_back(); if (line.empty()) continue; std::vector<double> row; size_t pos = 0, found; while ((found = line.find(delim, pos)) != std::string::npos) { try { row.push_back(std::stod(line.substr(pos, found - pos))); } catch (...) { row.push_back(0.0); } pos = found + delim.size(); } try { row.push_back(std::stod(line.substr(pos))); } catch (...) { row.push_back(0.0); } if (row.size() > maxCols) maxCols = row.size(); rowsData.push_back(row); }
        file.close(); if (rowsData.empty()) return Value(RealMatrix(0, 0));
        std::vector<double> flat; for (auto& row : rowsData) { row.resize(maxCols, 0.0); flat.insert(flat.end(), row.begin(), row.end()); }
        return Value(RealMatrix(static_cast<int>(rowsData.size()), static_cast<int>(maxCols), flat));
        });

    reg("writeCSV", { 2, 3 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: writeCSV() expects a string path.");
        std::string path = safeResolvePath(args[0].asString());
        std::string delim = ",";
        if (args.size() == 3) { if (!args[2].isString()) throw std::runtime_error("Type Error: writeCSV() delimiter must be a string."); delim = args[2].asString(); }
        std::ofstream file(path); if (!file.is_open()) throw std::runtime_error("IO Error: Cannot write to file '" + path + "'.");
        if (args[1].isObjType(ObjType::REAL_MATRIX)) { const auto& m = static_cast<ObjRealMatrix*>(args[1].asObj())->mat; for (int i = 0; i < m.getRows(); ++i) { for (int j = 0; j < m.getCols(); ++j) { if (j > 0) file << delim; file << Value(m(i, j)); } file << "\n"; } }
        else if (args[1].isObjType(ObjType::STRING_MATRIX)) { const auto& m = static_cast<ObjStringMatrix*>(args[1].asObj())->mat; for (int i = 0; i < m.getRows(); ++i) { for (int j = 0; j < m.getCols(); ++j) { if (j > 0) file << delim; file << m(i, j); } file << "\n"; } }
        else if (args[1].isObjType(ObjType::COMPLEX_MATRIX)) { const auto& m = static_cast<ObjComplexMatrix*>(args[1].asObj())->mat; for (int i = 0; i < m.getRows(); ++i) { for (int j = 0; j < m.getCols(); ++j) { if (j > 0) file << delim; file << m(i, j); } file << "\n"; } }
        else if (args[1].isObjType(ObjType::LIST)) { for (const auto& e : static_cast<ObjList*>(args[1].asObj())->vec) { file << e << "\n"; } }
        else throw std::runtime_error("Type Error: writeCSV() expects a matrix or list.");
        file.close(); return Value::none();
        });
}

// =================================================================
// [Phase 2] 错误处理 (pcall, isError, assert)
// =================================================================
void BuiltinRegistry::registerErrorHandling() {

    reg("pcall", { 1 }, [](const std::vector<Value>& args) -> Value {
        auto cl = args[0].asFunction();
        if (!cl->acceptsArgCount(0))
            throw std::runtime_error("Runtime Error: pcall() expects a zero-parameter function.");

        try {
            Value result = safeCallFunction(cl, {});
            GcValueGuard resGuard(result);
            ObjList* L = GcHeap::get().allocate<ObjList>();
            L->vec.push_back(Value(true)); L->vec.push_back(result);
            L->is_frozen = true; return Value(L);
        }
        catch (const StackTracedException& ex) {
            // ★ 完美拿到纯净的出错理由字符串！无视底下挂着的多行追踪栈
            ObjList* L = GcHeap::get().allocate<ObjList>();
            L->vec.push_back(Value(false)); L->vec.push_back(Value(ex.rawMessage));
            L->is_frozen = true; L->is_hashable_cached = true; return Value(L);
        }
        catch (const ErrorSignal& sig) {
            ObjList* L = GcHeap::get().allocate<ObjList>();
            L->vec.push_back(Value(false)); L->vec.push_back(Value(sig.message));
            L->is_frozen = true; L->is_hashable_cached = true; return Value(L);
        }
        catch (const jc::EngineInterruptError&) {
            throw; // 强行中断，无视 pcall 拦截
        }
        catch (const std::exception& ex) {
            ObjList* L = GcHeap::get().allocate<ObjList>();
            L->vec.push_back(Value(false)); L->vec.push_back(Value(std::string(ex.what())));
            L->is_frozen = true; L->is_hashable_cached = true; return Value(L);
        }
        });

    reg("isError", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::LIST)) return Value(false);
        const auto& L = static_cast<ObjList*>(args[0].asObj())->vec;
        if (L.size() != 2) return Value(false);
        Value first = L[0];
        if (first.isBool())
            return Value(!first.asBool());
        if (first.isNumber())
            return Value(first.asDouble() == 0.0);
        return Value(false);
        });

    reg("assert", { 1, 2, 3 }, [](const std::vector<Value>& args) -> Value {
        if (args.size() == 1) {
            if (!args[0].truthy()) throw std::runtime_error("Assertion Failed.");
            return Value(true);
        }
        if (args.size() == 2) {
            if (!args[0].truthy()) {
                std::string msg = "Assertion Failed";
                if (args[1].isString())
                    msg += ": " + args[1].asString();
                else { std::ostringstream oss; oss << args[1]; msg += ": " + oss.str(); }
                throw std::runtime_error(msg);
            }
            return Value(true);
        }
        // assert(name, got, expected)
        std::string name;
        if (args[0].isString()) name = args[0].asString();
        else { std::ostringstream oss; oss << args[0]; name = oss.str(); }
        Value got = args[1], expected = args[2];
        
        if (!Value::equals(got, expected)) {
            std::ostringstream oss;
            oss << "Assertion Failed: [" << name << "]\n"
                << "       Expected: " << expected << "\n"
                << "       Got:      " << got;
            throw std::runtime_error(oss.str());
        }
        return Value(true);
        });
}

// =================================================================
// [Phase 3] 终极系统 Shell 与绘图 (取代 Evaluator 专属函数)
// =================================================================
void BuiltinRegistry::registerSystemShell() {

    reg("resetConst", { 0 }, [](const std::vector<Value>&) -> Value {
        if (helpers::setGlobalCallback) {
            helpers::setGlobalCallback("PI", Value(3.14159265358979323846));
            helpers::setGlobalCallback("E", Value(2.71828182845904523536));
            helpers::setGlobalCallback("i", Value(Complex(0.0, 1.0)));
            helpers::setGlobalCallback("I", Value(Complex(0.0, 1.0)));
        }
        std::cout << "System constants restored: PI, E, i, I" << std::endl;
        return Value::none();
        });

    reg("resetType", { 0 }, [](const std::vector<Value>&) -> Value {
        if (VM::activeVM) {
            auto makeType = [](BuiltinType bt) {
                ObjTypeDef* td = GcHeap::get().allocate<ObjTypeDef>();
                td->types.push_back(bt);
                return Value(td);
            };
            VM::activeVM->setGlobal("any_type", makeType(BuiltinType::ANY));
            VM::activeVM->setGlobal("int", makeType(BuiltinType::INT));
            VM::activeVM->setGlobal("double", makeType(BuiltinType::FLOAT));
            VM::activeVM->setGlobal("real", makeType(BuiltinType::REAL));
            VM::activeVM->setGlobal("number", makeType(BuiltinType::NUMBER));
            VM::activeVM->setGlobal("whole", makeType(BuiltinType::WHOLE));
            VM::activeVM->setGlobal("exact", makeType(BuiltinType::EXACT));
            VM::activeVM->setGlobal("string", makeType(BuiltinType::STRING));
            VM::activeVM->setGlobal("bool", makeType(BuiltinType::BOOL));
            VM::activeVM->setGlobal("binary", makeType(BuiltinType::BINARY));
            VM::activeVM->setGlobal("none_type", makeType(BuiltinType::NONE_TYPE));
            VM::activeVM->setGlobal("list", makeType(BuiltinType::LIST));
            VM::activeVM->setGlobal("dict", makeType(BuiltinType::DICT));
            VM::activeVM->setGlobal("set", makeType(BuiltinType::SET));
            VM::activeVM->setGlobal("fraction", makeType(BuiltinType::FRACTION));
            VM::activeVM->setGlobal("complex", makeType(BuiltinType::COMPLEX));
            VM::activeVM->setGlobal("basenum", makeType(BuiltinType::BASENUM));
            VM::activeVM->setGlobal("symbolic", makeType(BuiltinType::SYMBOLIC));
            VM::activeVM->setGlobal("realmatrix", makeType(BuiltinType::REALMAT));
            VM::activeVM->setGlobal("complexmatrix", makeType(BuiltinType::COMPLEXMAT));
            VM::activeVM->setGlobal("stringmatrix", makeType(BuiltinType::STRINGMAT));
            VM::activeVM->setGlobal("matrix", makeType(BuiltinType::MATRIX));
            VM::activeVM->setGlobal("function", makeType(BuiltinType::FUNC));
            VM::activeVM->setGlobal("class_type", makeType(BuiltinType::CLASS));
            VM::activeVM->setGlobal("instance", makeType(BuiltinType::INSTANCE));
            VM::activeVM->setGlobal("namespace_type", makeType(BuiltinType::NAMESPACE));
            VM::activeVM->setGlobal("iterable", makeType(BuiltinType::ITERABLE));
            VM::activeVM->setGlobal("callable", makeType(BuiltinType::CALLABLE));
            VM::activeVM->setGlobal("indexable", makeType(BuiltinType::INDEXABLE));
            VM::activeVM->setGlobal("hashable", makeType(BuiltinType::HASHABLE));
            VM::activeVM->setGlobal("numeric", makeType(BuiltinType::NUMERIC));
            VM::activeVM->setGlobal("type", makeType(BuiltinType::TYPE_DEF));
        }
        std::cout << "System types restored." << std::endl;
        return Value::none();
        });

    reg("setWorkspace", { 1 }, [](const std::vector<Value>& args) -> Value {
        std::string p = args[0].asString();
        if (p == "default") {
            g_workspacePath = "";
        }
        else {
            namespace fs = std::filesystem;
            fs::path dir(p);
            if (!dir.is_absolute()) dir = fs::path(g_cwd()) / dir;
            if (!fs::exists(dir)) fs::create_directories(dir);
            g_workspacePath = fs::weakly_canonical(dir).string();
        }
        std::cout << "[System] Workspace set to: " << (g_workspacePath.empty() ? "./data" : g_workspacePath) << std::endl;
        return Value::none();
        });

    reg("getWorkspace", { 0 }, [](const std::vector<Value>&) -> Value {
        return Value(g_workspacePath.empty() ? (std::filesystem::current_path() / "data").string() : g_workspacePath);
        });

    reg("pwd", { 0 }, [](const std::vector<Value>&) -> Value {
        std::cout << "  Script dir:    " << g_cwd() << std::endl;
        std::cout << "  Workspace dir: " << (g_workspacePath.empty() ? (std::filesystem::current_path() / "data").string() : g_workspacePath) << std::endl;
        return Value::none();
        });

    reg("cls", { 0 }, [](const std::vector<Value>&) -> Value {
#ifdef _WIN32
        std::system("cls");
#else
        std::system("clear");
#endif
        return Value::none();
        });

    reg("run", { 1 }, [](const std::vector<Value>& args) -> Value {
        std::string filepath = args[0].asString();
        std::string resolved = helpers::safeResolvePath(filepath);
        if (!std::filesystem::exists(resolved)) resolved = helpers::safeResolvePath(filepath + ".jc2");
        if (!std::filesystem::exists(resolved)) throw std::runtime_error("IO Error: Cannot open script '" + filepath + "'.");

        std::ifstream file(resolved);
        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot read script.");
        std::string code, line;
        while (std::getline(file, line)) code += line + "\n";
        file.close();

        helpers::g_scriptDirStack.push_back(std::filesystem::path(resolved).parent_path().string());
        Value result = Value::none();
        if (helpers::evalCallback) result = helpers::evalCallback(code);
        helpers::g_scriptDirStack.pop_back();

        return result;
        });

    reg("compileCode", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: compileCode() expects a string.");
        std::string code = args[0].asString();
        
        jc::Lexer lexer(code, "<compileCode>");
        auto tokens = lexer.tokenize();
        jc::Parser parser(tokens);
        auto ast = parser.parse();
        
        auto mainFn = std::make_shared<CompiledFunction>();
        mainFn->name = "<compiled_code>";
        mainFn->sourceFile = "<compileCode>";
        mainFn->arity = 0;
        mainFn->maxArity = 0;
        mainFn->hasRestParam = false;
        
        auto fns = VM::activeVM->getCompiledFunctions();
        
        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, &fns, nullptr, mainFn.get());
        fnBuilder.build(ast.get());
        
        IROptimizer::optimize(&fnGraph);
        RegisterAllocator::allocate(&fnGraph);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                IRNode* localNode = target.localNode;
                int upvalIdx = target.index;
                CompiledFunction* childFn = mainFn.get();
                fnGraph.postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                    childFn->upvalues[upvalIdx].index = localNode->getResolved()->physicalReg;
                });
            }
        }
        
        mainFn->localCount = Emitter::emit(&fnGraph, mainFn->chunk);
        
        fns.push_back(mainFn);
        int mainFnIdx = static_cast<int>(fns.size()) - 1;
        VM::activeVM->setCompiledFunctions(fns);
        
        ObjClosure* cls = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{}, std::vector<bool>{}, "<compiled_code>", nullptr
        );
        cls->compiledFnIndex = mainFnIdx;
        
        return Value(cls);
        });

    reg("compileFile", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) throw std::runtime_error("Type Error: compileFile() expects a string path.");
        std::string filepath = args[0].asString();
        std::string resolved = helpers::safeResolvePath(filepath);
        if (!std::filesystem::exists(resolved)) resolved = helpers::safeResolvePath(filepath + ".jc2");
        if (!std::filesystem::exists(resolved)) throw std::runtime_error("IO Error: Cannot open script '" + filepath + "'.");

        std::ifstream file(resolved);
        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot read script.");
        std::string code, line;
        while (std::getline(file, line)) code += line + "\n";
        file.close();

        jc::Lexer lexer(code, resolved);
        auto tokens = lexer.tokenize();
        jc::Parser parser(tokens);
        auto ast = parser.parse();
        
        auto mainFn = std::make_shared<CompiledFunction>();
        mainFn->name = "<compiled_file>";
        mainFn->sourceFile = resolved;
        mainFn->arity = 0;
        mainFn->maxArity = 0;
        mainFn->hasRestParam = false;
        
        auto fns = VM::activeVM->getCompiledFunctions();
        
        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, &fns, nullptr, mainFn.get());
        fnBuilder.build(ast.get());
        
        IROptimizer::optimize(&fnGraph);
        RegisterAllocator::allocate(&fnGraph);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                IRNode* localNode = target.localNode;
                int upvalIdx = target.index;
                CompiledFunction* childFn = mainFn.get();
                fnGraph.postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                    childFn->upvalues[upvalIdx].index = localNode->getResolved()->physicalReg;
                });
            }
        }
        
        mainFn->localCount = Emitter::emit(&fnGraph, mainFn->chunk);
        
        fns.push_back(mainFn);
        int mainFnIdx = static_cast<int>(fns.size()) - 1;
        VM::activeVM->setCompiledFunctions(fns);

        // ★ 核心：使用原生闭包代理，保护相对路径上下文！
        std::string scriptDir = std::filesystem::path(resolved).parent_path().string();
        VM* vm = VM::activeVM;
        
        ObjClosure* proxy = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{}, std::vector<bool>{}, "<compiled_file_proxy>", nullptr
        );
        
        proxy->nativeFn = std::make_any<NativeCallable>([vm, mainFnIdx, scriptDir](const std::vector<Value>& callArgs) -> Value {
            helpers::g_scriptDirStack.push_back(scriptDir);
            Value result;
            try {
                result = vm->callVMFunction(mainFnIdx, callArgs);
            } catch (...) {
                helpers::g_scriptDirStack.pop_back();
                throw;
            }
            helpers::g_scriptDirStack.pop_back();
            return result;
        });
        
        return Value(proxy);
        });

    reg("imgPlot", { 7, 8 }, [](const std::vector<Value>& args) -> Value {
        auto inst = args[0].asInstance();
        auto& im = std::any_cast<std::shared_ptr<Image>&>(inst->nativeData);
        auto fn_actual = args[1].asFunction();
        double xMin = args[2].asDouble(), xMax = args[3].asDouble();
        double yMin = args[4].asDouble(), yMax = args[5].asDouble();
        Color c = Color::parse(args[6].asString());
        int thick = (args.size() == 8) ? static_cast<int>(std::round(args[7].asDouble())) : 2;
        int plotW = im->width() - 50;
        int prevPx = -1, prevPy = -1;

        for (int px = 0; px <= plotW; ++px) {
            jc::checkInterrupt();
            double x = xMin + (static_cast<double>(px) / plotW) * (xMax - xMin);
            double y = 0;
            try { y = helpers::safeCallFunction(fn_actual, { Value(x) }).asDouble(); }
            catch (const jc::EngineInterruptError&) { throw; }
            catch (...) { prevPx = -1; prevPy = -1; continue; }
            int screenX = im->mapPlotX(x, xMin, xMax);
            int screenY = im->mapPlotY(y, yMin, yMax);
            if (prevPx >= 0 && std::abs(screenY - prevPy) < im->height())
                im->line(prevPx, prevPy, screenX, screenY, c, thick);
            prevPx = screenX; prevPy = screenY;
        }
        return args[0];
        });

    reg("breakpoint", { 0 }, [](const std::vector<Value>&) -> Value {
        if (VM::activeVM) {
            VM::activeVM->triggerDebugger();
        }
        return Value::none();
        });
    // 做个兼容别名
    reg("debugger", builtinArity["breakpoint"], builtins["breakpoint"]);

    reg("disassemble", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isFunctionClosure()) {
            throw std::runtime_error("Type Error: disassemble() expects a function.");
        }
        auto cl = args[0].asFunction();
        if (cl->compiledFnIndex >= 0) {
            if (VM::activeVM) {
                auto fns = VM::activeVM->getCompiledFunctions();
                if (cl->compiledFnIndex < static_cast<int>(fns.size())) {
                    auto fn = fns[cl->compiledFnIndex];
                    fn->chunk.disassemble(fn->name.empty() ? "Function" : fn->name);
                } else {
                    std::cout << "Invalid compiled function index." << std::endl;
                }
            }
        } else {
            std::cout << "Cannot disassemble native function: " << cl->rawBody << std::endl;
        }
        return Value::none();
        });
    reg("disasm", builtinArity["disassemble"], builtins["disassemble"]);
}

// =================================================================
// [Phase 3] 类型与字符串谓词函数
// =================================================================
void BuiltinRegistry::registerTypeChecks() {

    // ═══ 数值类型谓词 ═══

    reg("isbool", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isBool());
        });

    reg("isint", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isInt32() || args[0].isBigInt() || args[0].isBool()) return Value(true);
        if (args[0].isObjType(ObjType::FRACTION))
            return Value(static_cast<ObjFraction*>(args[0].asObj())->frac.getDen() == BigInt(1));
        return Value(false);
        });

    reg("iswhole", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isInt32() || args[0].isBigInt() || args[0].isBool()) return Value(true);
        if (args[0].isDouble()) {
            double v = args[0].asDoubleRaw();
            return Value(std::isfinite(v) && v == std::floor(v));
        }
        if (args[0].isObjType(ObjType::FRACTION))
            return Value(static_cast<ObjFraction*>(args[0].asObj())->frac.getDen() == BigInt(1));
        if (args[0].isComplex()) {
            const auto& c = args[0].asComplex();
            return Value(c.imag == 0.0 && std::isfinite(c.real) && c.real == std::floor(c.real));
        }
        return Value(false);
        });

    reg("isdouble", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isDouble());
        });

    reg("isnumeric", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& val = args[0];
        if (val.isNumber() || val.isBigInt() || val.isObjType(ObjType::FRACTION) ||
            val.isComplex() || val.isObjType(ObjType::BASENUM)) return Value(true);
        if (val.isInstance()) {
            auto inst = val.asInstance();
            return Value(invokeDunder(inst, DUNDER_ADD).first || invokeDunder(inst, DUNDER_MUL).first ||
                         invokeDunder(inst, DUNDER_SUB).first || invokeDunder(inst, DUNDER_DIV).first || invokeDunder(inst, DUNDER_LDIV).first);
        }
        return Value(false);
        });

    reg("iscomplex", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isComplex()) return Value(true);
        // double/BigInt/Fraction 在数学意义上也是复数（虚部为 0）
        return Value(false);
        });

    reg("isreal", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isNumber() ||
            args[0].isBigInt() ||
            args[0].isObjType(ObjType::FRACTION) ||
            args[0].isObjType(ObjType::BASENUM))
            return Value(true);
        if (args[0].isComplex())
            return Value(args[0].asComplex().imag == 0.0);
        return Value(false);
        });

    reg("isfrac", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::FRACTION));
        });

    reg("isbase", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::BASENUM));
        });

    reg("isexact", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isInt32() || args[0].isBigInt() || args[0].isBool() || args[0].isObjType(ObjType::FRACTION) || args[0].isObjType(ObjType::BASENUM) || args[0].isObjType(ObjType::SYMBOLIC));
        });

    reg("isbinary", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isBool()) return Value(true);
        try {
            double d = args[0].asDouble();
            if (d == 0.0 || d == 1.0) return Value(true);
        } catch (...) {}
        return Value(false);
        });

    // ═══ 容器类型谓词 ═══

    reg("ismatrix", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::REAL_MATRIX) ||
            args[0].isObjType(ObjType::COMPLEX_MATRIX) ||
            args[0].isObjType(ObjType::STRING_MATRIX));
        });

    reg("isrealmatrix", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::REAL_MATRIX));
        });

    reg("iscomplexmatrix", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::COMPLEX_MATRIX));
        });

    reg("isstringmatrix", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::STRING_MATRIX));
        });

    reg("isvector", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            const auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            return Value(m.getRows() == 1 || m.getCols() == 1);
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            const auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            return Value(m.getRows() == 1 || m.getCols() == 1);
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            const auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            return Value(m.getRows() == 1 || m.getCols() == 1);
        }
        return Value(false);
        });

    reg("issquare", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::REAL_MATRIX))
            return Value(static_cast<ObjRealMatrix*>(args[0].asObj())->mat.getRows() == static_cast<ObjRealMatrix*>(args[0].asObj())->mat.getCols());
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX))
            return Value(static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.getRows() == static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.getCols());
        if (args[0].isObjType(ObjType::STRING_MATRIX))
            return Value(static_cast<ObjStringMatrix*>(args[0].asObj())->mat.getRows() == static_cast<ObjStringMatrix*>(args[0].asObj())->mat.getCols());
        return Value(false);
        });

    reg("islist", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::LIST));
        });

    reg("isdict", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::DICT));
        });

    reg("isiterable", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& val = args[0];
        if (val.isObjType(ObjType::LIST) || val.isObjType(ObjType::DICT) || val.isObjType(ObjType::SET) ||
            val.isString() || val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) ||
            val.isObjType(ObjType::STRING_MATRIX)) return Value(true);
        if (val.isInstance()) {
            auto inst = val.asInstance();
            return Value(invokeDunder(inst, DUNDER_ITER).first || invokeDunder(inst, DUNDER_NEXT).first);
        }
        return Value(false);
        });

    reg("iscallable", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& val = args[0];
        if (val.isFunctionClosure() || val.isClass() || val.isString()) return Value(true);
        if (val.isInstance()) return Value(invokeDunder(val.asInstance(), DUNDER_CALL).first);
        return Value(false);
        });

    reg("isindexable", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& val = args[0];
        if (val.isObjType(ObjType::LIST) || val.isObjType(ObjType::DICT) || val.isString() ||
            val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) ||
            val.isObjType(ObjType::STRING_MATRIX)) return Value(true);
        if (val.isInstance()) return Value(invokeDunder(val.asInstance(), DUNDER_GETITEM).first);
        return Value(false);
        });

    reg("ishashable", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isHashable());
        });

    // ═══ 字符串谓词 ═══

    reg("isstring", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isString());
        });

    reg("isalpha", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) return Value(false);
        const auto& s = args[0].asString();
        if (s.empty()) return Value(false);
        for (char c : s) if (!std::isalpha(static_cast<unsigned char>(c))) return Value(false);
        return Value(true);
        });

    reg("isdigit", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) return Value(false);
        const auto& s = args[0].asString();
        if (s.empty()) return Value(false);
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return Value(false);
        return Value(true);
        });

    reg("isalnum", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) return Value(false);
        const auto& s = args[0].asString();
        if (s.empty()) return Value(false);
        for (char c : s) if (!std::isalnum(static_cast<unsigned char>(c))) return Value(false);
        return Value(true);
        });

    reg("isspace", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) return Value(false);
        const auto& s = args[0].asString();
        if (s.empty()) return Value(false);
        for (char c : s) if (!std::isspace(static_cast<unsigned char>(c))) return Value(false);
        return Value(true);
        });

    reg("isupper", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) return Value(false);
        const auto& s = args[0].asString();
        if (s.empty()) return Value(false);
        for (char c : s) if (std::isalpha(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c))) return Value(false);
        return Value(true);
        });

    reg("islower", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString()) return Value(false);
        const auto& s = args[0].asString();
        if (s.empty()) return Value(false);
        for (char c : s) if (std::isalpha(static_cast<unsigned char>(c)) && !std::islower(static_cast<unsigned char>(c))) return Value(false);
        return Value(true);
        });

    reg("isempty", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isString())
            return Value(args[0].asString().empty());
        if (args[0].isObjType(ObjType::LIST))
            return Value(static_cast<ObjList*>(args[0].asObj())->vec.empty());
        if (args[0].isObjType(ObjType::DICT))
            return Value(static_cast<ObjDict*>(args[0].asObj())->elements.empty());
        if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            const auto& m = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            return Value(m.getRows() == 0 || m.getCols() == 0);
        }
        if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            const auto& m = static_cast<ObjComplexMatrix*>(args[0].asObj())->mat;
            return Value(m.getRows() == 0 || m.getCols() == 0);
        }
        if (args[0].isObjType(ObjType::STRING_MATRIX)) {
            const auto& m = static_cast<ObjStringMatrix*>(args[0].asObj())->mat;
            return Value(m.getRows() == 0 || m.getCols() == 0);
        }
        if (args[0].isObjType(ObjType::SET))
            return Value(static_cast<ObjSet*>(args[0].asObj())->elements.empty());
        return Value(false);
        });

    // ═══ 特殊谓词 ═══

    reg("isnone", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isNone());
        });

    reg("isfunction", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isFunctionClosure());
        });

    reg("isclass", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isClass());
        });

    reg("isnamespace", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::NAMESPACE));
        });

    reg("issymbolic", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isSymbolic());
        });

    reg("isnan", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isDouble())
            return Value(std::isnan(args[0].asDoubleRaw()));
        return Value(false);
        });

    reg("isinf", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isDouble())
            return Value(std::isinf(args[0].asDoubleRaw()));
        return Value(false);
        });

    reg("isfinite", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& v = args[0];
        if (v.isDouble()) return Value(std::isfinite(v.asDoubleRaw()));
        if (v.isInt32() || v.isBigInt() || v.isObjType(ObjType::FRACTION)) return Value(true);
        return Value(false);
        });

    reg("isprime", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& v = args[0];
        if (v.isInt32()) return Value(BigInt(v.asInt32()).isPrime());
        if (v.isBigInt()) return Value(static_cast<ObjBigInt*>(v.asObj())->num.isPrime());
        if (v.isDouble()) return Value(BigInt(static_cast<int64_t>(std::round(v.asDoubleRaw()))).isPrime());
        return Value(false);
        });

    reg("iseven", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& v = args[0];
        if (v.isInt32()) return Value((v.asInt32() & 1) == 0);
        if (v.isBigInt()) return Value((static_cast<ObjBigInt*>(v.asObj())->num % BigInt(2)).isZero());
        if (v.isDouble()) {
            double d = v.asDoubleRaw();
            return Value(std::isfinite(d) && d == std::floor(d) && std::fmod(d, 2.0) == 0.0);
        }
        return Value(false);
        });

    reg("isodd", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& v = args[0];
        if (v.isInt32()) return Value((v.asInt32() & 1) != 0);
        if (v.isBigInt()) return Value(!(static_cast<ObjBigInt*>(v.asObj())->num % BigInt(2)).isZero());
        if (v.isDouble()) {
            double d = v.asDoubleRaw();
            return Value(std::isfinite(d) && d == std::floor(d) && std::fmod(d, 2.0) != 0.0);
        }
        return Value(false);
        });

    reg("ispositive", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& v = args[0];
        if (v.isInt32()) return Value(v.asInt32() > 0);
        if (v.isDouble()) return Value(v.asDoubleRaw() > 0.0);
        if (v.isBigInt()) return Value(!static_cast<ObjBigInt*>(v.asObj())->num.isZero() && !static_cast<ObjBigInt*>(v.asObj())->num.isNegative());
        if (v.isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(v.asObj())->frac.toDouble() > 0.0);
        return Value(false);
        });

    reg("isnegative", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& v = args[0];
        if (v.isInt32()) return Value(v.asInt32() < 0);
        if (v.isDouble()) return Value(v.asDoubleRaw() < 0.0);
        if (v.isBigInt()) return Value(static_cast<ObjBigInt*>(v.asObj())->num.isNegative());
        if (v.isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(v.asObj())->frac.toDouble() < 0.0);
        return Value(false);
        });

    reg("iszero", { 1 }, [](const std::vector<Value>& args) -> Value {
        const Value& v = args[0];
        if (v.isInt32()) return Value(v.asInt32() == 0);
        if (v.isDouble()) return Value(v.asDoubleRaw() == 0.0);
        if (v.isBigInt()) return Value(static_cast<ObjBigInt*>(v.asObj())->num.isZero());
        if (v.isComplex()) {
            const auto& c = static_cast<ObjComplex*>(v.asObj())->comp;
            return Value(c.real == 0.0 && c.imag == 0.0);
        }
        if (v.isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(v.asObj())->frac.getNum().isZero());
        return Value(false);
        });

    reg("isapprox", { 2, 3, 4 }, [](const std::vector<Value>& args) -> Value {
        double rtol = args.size() >= 3 ? args[2].asDouble() : 1e-9;
        double atol = args.size() == 4 ? args[3].asDouble() : 0.0;

        bool isComp = false;
        if (args[0].isComplex() || args[1].isComplex()) isComp = true;

        if (isComp) {
            Complex a = args[0].asComplex();
            Complex b = args[1].asComplex();
            if (std::isnan(a.real) || std::isnan(a.imag) || std::isnan(b.real) || std::isnan(b.imag)) return Value(false);
            if (a.real == b.real && a.imag == b.imag) return Value(true);
            if (std::isinf(a.real) || std::isinf(a.imag) || std::isinf(b.real) || std::isinf(b.imag)) return Value(false);
            double diff = (a - b).modulus();
            double tol = std::max(atol, rtol * std::max(a.modulus(), b.modulus()));
            return Value(diff <= tol);
        } else {
            double a = args[0].asDouble();
            double b = args[1].asDouble();
            if (std::isnan(a) || std::isnan(b)) return Value(false);
            if (a == b) return Value(true);
            if (std::isinf(a) || std::isinf(b)) return Value(false);
            double diff = std::abs(a - b);
            double tol = std::max(atol, rtol * std::max(std::abs(a), std::abs(b)));
            return Value(diff <= tol);
        }
        });

    reg("isset", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(args[0].isObjType(ObjType::SET));
        });
}

// =================================================================
// [Set] 无序去重集合
// =================================================================
void BuiltinRegistry::registerSetFunctions() {

    // ═══ 构造 ═══
    reg("set", {}, [](const std::vector<Value>& args) -> Value {
        ObjSet* s = GcHeap::get().allocate<ObjSet>();
        GcObjGuard guard(s);
        for (const auto& a : args) {
            if (s->keys.find(a) == s->keys.end()) {
                s->keys.insert(a);
                s->elements.push_back(a);
            }
        }
        return Value(s);
        });

    reg("toSet", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (args[0].isObjType(ObjType::SET)) return args[0];
        ObjSet* s = GcHeap::get().allocate<ObjSet>();
        GcObjGuard guard(s);
        if (args[0].isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(args[0].asObj())->vec) {
                if (s->keys.find(v) == s->keys.end()) { s->keys.insert(v); s->elements.push_back(v); }
            }
        }
        else if (args[0].isObjType(ObjType::REAL_MATRIX)) {
            for (double d : static_cast<ObjRealMatrix*>(args[0].asObj())->mat.rawData()) {
                Value v(d);
                if (s->keys.find(v) == s->keys.end()) { s->keys.insert(v); s->elements.push_back(v); }
            }
        }
        else if (args[0].isObjType(ObjType::COMPLEX_MATRIX)) {
            for (const auto& c : static_cast<ObjComplexMatrix*>(args[0].asObj())->mat.rawData()) {
                Value v(c);
                if (s->keys.find(v) == s->keys.end()) { s->keys.insert(v); s->elements.push_back(v); }
            }
        }
        else if (args[0].isString()) {
            ObjString* objStr = args[0].asObjString();
            const std::string& str = objStr->str;
            if (objStr->isAscii) {
                for (char c : str) {
                    Value v(std::string(1, c));
                    if (s->keys.find(v) == s->keys.end()) { s->keys.insert(v); s->elements.push_back(v); }
                }
            } else {
                size_t len = objStr->charLength;
                for (size_t i = 0; i < len; ++i) {
                    Value v(utf8::substring(str, i, 1, false));
                    if (s->keys.find(v) == s->keys.end()) { s->keys.insert(v); s->elements.push_back(v); }
                }
            }
        }
        else if (args[0].isInstance() && helpers::hasDunder(args[0], DUNDER_ITER)) {
            auto inst = args[0].asInstance();
            auto [hasIter, iterObj] = invokeDunder(inst, DUNDER_ITER, {});
            if (hasIter) {
                GcValueGuard iterGuard(iterObj);
                auto iterInst = iterObj.asInstance();
                if (iterInst->c_nativeNext) {
                    while (true) {
                        Value nextVal = iterInst->c_nativeNext(iterInst);
                        if (nextVal.isUninit()) break;
                        if (s->keys.find(nextVal) == s->keys.end()) { s->keys.insert(nextVal); s->elements.push_back(nextVal); }
                    }
                } else {
                    while (true) {
                        auto [hasNext, nextVal] = invokeDunder(iterInst, DUNDER_NEXT, {});
                        if (!hasNext || nextVal.isNone()) break;
                        if (s->keys.find(nextVal) == s->keys.end()) { s->keys.insert(nextVal); s->elements.push_back(nextVal); }
                    }
                }
                return Value(s);
            }
            throw std::runtime_error("Type Error: toSet() expects an iterable.");
        }
        else {
            throw std::runtime_error("Type Error: toSet() expects a list, array, string, set, or iterable.");
        }
        return Value(s);
        });

    // ═══ 元素操作（引用语义，原地修改）═══
    reg("setAdd", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setAdd() expects a Set.");
        auto s = static_cast<ObjSet*>(args[0].asObj());
        s->add(args[1]);
        return args[0];
        });

    reg("setRemove", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setRemove() expects a Set.");
        auto s = static_cast<ObjSet*>(args[0].asObj());
        s->remove(args[1]);
        return args[0];
        });

    reg("setDiscard", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setDiscard() expects a Set.");
        auto s = static_cast<ObjSet*>(args[0].asObj());
        s->discard(args[1]);
        return args[0];
        });

    reg("setClear", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setClear() expects a Set.");
        auto s = static_cast<ObjSet*>(args[0].asObj());
        s->clear();
        return args[0];
        });

    reg("setPop", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setPop() expects a Set.");
        auto s = static_cast<ObjSet*>(args[0].asObj());
        return s->pop();
        });

    // ═══ 集合运算（返回新 Set）═══
    reg("setUnion", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setUnion() expects two Sets.");
        return args[0] | args[1];
        });

    reg("setIntersect", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setIntersect() expects two Sets.");
        return args[0] & args[1];
        });

    reg("setDiff", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setDiff() expects two Sets.");
        auto a = static_cast<ObjSet*>(args[0].asObj());
        auto b = static_cast<ObjSet*>(args[1].asObj());
        ObjSet* result = GcHeap::get().allocate<ObjSet>();
        GcObjGuard guard(result);
        for (const auto& val : a->elements) {
            if (b->keys.find(val) == b->keys.end()) {
                result->keys.insert(val);
                result->elements.push_back(val);
            }
        }
        return Value(result);
        });

    reg("setSymDiff", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setSymDiff() expects two Sets.");
        return bitXor(args[0], args[1]);
        });

    // ═══ 集合关系谓词 ═══
    reg("isSubset", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: isSubset() expects two Sets.");
        auto a = static_cast<ObjSet*>(args[0].asObj());
        auto b = static_cast<ObjSet*>(args[1].asObj());
        for (const auto& val : a->elements) {
            if (b->keys.find(val) == b->keys.end()) return Value(false);
        }
        return Value(true);
        });

    reg("isSuperset", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: isSuperset() expects two Sets.");
        auto a = static_cast<ObjSet*>(args[0].asObj());
        auto b = static_cast<ObjSet*>(args[1].asObj());
        for (const auto& val : b->elements) {
            if (a->keys.find(val) == a->keys.end()) return Value(false);
        }
        return Value(true);
        });

    reg("isDisjoint", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: isDisjoint() expects two Sets.");
        auto a = static_cast<ObjSet*>(args[0].asObj());
        auto b = static_cast<ObjSet*>(args[1].asObj());
        for (const auto& val : a->elements) {
            if (b->keys.find(val) != b->keys.end()) return Value(false);
        }
        return Value(true);
        });

    // ═══ 笛卡尔积 ═══
    reg("setProduct", { 2 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET) || !args[1].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setProduct() expects two Sets.");
        // 直接触发刚写好的重载 *
        return args[0] * args[1];
        });

    // ═══ 集合幂集 ═══
    reg("setPow", { 1 }, [](const std::vector<Value>& args) -> Value {
        if (!args[0].isObjType(ObjType::SET))
            throw std::runtime_error("Type Error: setPow() expects a Set.");

        auto s = static_cast<ObjSet*>(args[0].asObj());
        int n = static_cast<int>(s->elements.size());
        if (n > 20)
            throw std::runtime_error("Math Error: Set size too large for powerset (max 20 elements).");

        ObjSet* result = GcHeap::get().allocate<ObjSet>();
        GcObjGuard guard(result);
        int limit = 1 << n;  // 2^n
        const auto& raw = s->elements;

        for (int mask = 0; mask < limit; ++mask) {
            jc::checkInterrupt();
            ObjSet* sub = GcHeap::get().allocate<ObjSet>();
            for (int i = 0; i < n; ++i) {
                if (mask & (1 << i)) {
                    sub->keys.insert(raw[i]);
                    sub->elements.push_back(raw[i]);
                }
            }
            sub->is_frozen = true;
            sub->is_hashable_cached = true;
            Value subVal(sub);
            result->keys.insert(subVal);
            result->elements.push_back(subVal);
        }
        return Value(result);
        });
}

// =================================================================
// [CAS] 符号计算与计算机代数系统
// =================================================================
void BuiltinRegistry::registerCAS() {
    auto* fnsPtr = &builtins;

    auto toBigInt = [](const Value& v) -> BigInt {
        return v.isBigInt() ? v.asBigInt() : BigInt(static_cast<int64_t>(std::round(v.asDouble())));
    };

    auto evalFunc = [](ObjClosure* cl, double x) -> double {
        return safeCallFunction(cl, { Value(x) }).asDouble();
    };

    auto getVarName = [](const Value& v, const std::string& funcName) -> std::string {
        if (v.isString()) return v.asString();
        if (v.isSymbolic() && v.asSymbolic().ptr->getType() == SymType::VAR) return std::static_pointer_cast<SymVar>(v.asSymbolic().ptr)->name;
        throw std::runtime_error("TypeError: " + funcName + "() expects a variable name (string or symbol).");
    };

    reg("sym", { 1 }, [getVarName](const std::vector<Value>& args) -> Value {
        return Value(SymExpr::makeVar(getVarName(args[0], "sym")));
        });

    reg("RootOf", { 3 }, [getVarName](const std::vector<Value>& args) -> Value {
        SymExpr poly = args[0].asSymbolic();
        std::string var = getVarName(args[1], "RootOf");
        SymExpr k = args[2].asSymbolic();
        return Value(SymExpr(std::make_shared<SymFunc>("RootOf", std::vector<std::shared_ptr<SymNode>>{
            poly.ptr, SymExpr::makeVar(var).ptr, k.ptr
        })));
        });

    reg("RootSum", { 3 }, [getVarName](const std::vector<Value>& args) -> Value {
        SymExpr expr = args[0].asSymbolic();
        std::string var = getVarName(args[1], "RootSum");
        SymExpr poly = args[2].asSymbolic();
        return Value(SymExpr(std::make_shared<SymFunc>("RootSum", std::vector<std::shared_ptr<SymNode>>{
            expr.ptr, SymExpr::makeVar(var).ptr, poly.ptr
        })));
        });

    reg("expand", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(expand(args[0].asSymbolic()));
        });

    reg("simplify", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(full_simplify(args[0].asSymbolic()));
        });

    reg("contract", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(contract(args[0].asSymbolic()));
        });

    reg("trigsimp", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(trigsimp(args[0].asSymbolic()));
        });

    reg("subs", { 3 }, [fnsPtr, this](const std::vector<Value>& args) -> Value {
        SymExpr result = args[0].asSymbolic();

        std::vector<std::string> vars;
        std::vector<SymExpr> vals;

        if (args[1].isString()) {
            vars.push_back(args[1].asString());
        }
        else if (args[1].isSymbolic() && args[1].asSymbolic().ptr->getType() == SymType::VAR) {
            vars.push_back(std::static_pointer_cast<SymVar>(args[1].asSymbolic().ptr)->name);
        }
        else if (args[1].isObjType(ObjType::STRING_MATRIX)) {
            const auto& sm = static_cast<ObjStringMatrix*>(args[1].asObj())->mat;
            for (int i = 0; i < sm.getRows(); ++i)
                for (int j = 0; j < sm.getCols(); ++j)
                    vars.push_back(sm(i, j));
        }
        else if (args[1].isObjType(ObjType::LIST)) {
            const auto& lst = static_cast<ObjList*>(args[1].asObj())->vec;
            for (size_t i = 0; i < lst.size(); ++i) {
                Value v = lst[i];
                if (v.isString()) {
                    vars.push_back(v.asString());
                } else if (v.isSymbolic() && v.asSymbolic().ptr->getType() == SymType::VAR) {
                    vars.push_back(std::static_pointer_cast<SymVar>(v.asSymbolic().ptr)->name);
                } else {
                    throw std::runtime_error("TypeError: subs() variable list must contain strings or symbols.");
                }
            }
        }
        else {
            throw std::runtime_error("TypeError: subs() second argument must be a string, symbol, string matrix, or list.");
        }

        if (vars.size() == 1) {
            vals.push_back(args[2].asSymbolic());
        }
        else if (args[2].isObjType(ObjType::REAL_MATRIX)) {
            const auto& rm = static_cast<ObjRealMatrix*>(args[2].asObj())->mat;
            for (int i = 0; i < rm.getRows(); ++i)
                for (int j = 0; j < rm.getCols(); ++j)
                    vals.push_back(SymExpr(rm(i, j)));
        }
        else if (args[2].isObjType(ObjType::COMPLEX_MATRIX)) {
            const auto& cm = static_cast<ObjComplexMatrix*>(args[2].asObj())->mat;
            for (int i = 0; i < cm.getRows(); ++i)
                for (int j = 0; j < cm.getCols(); ++j)
                    vals.push_back(SymExpr(cm(i, j)));
        }
        else if (args[2].isObjType(ObjType::LIST)) {
            const auto& lst = static_cast<ObjList*>(args[2].asObj())->vec;
            for (size_t i = 0; i < lst.size(); ++i) {
                Value v = lst[i];
                vals.push_back(v.asSymbolic());
            }
        }
        else {
            throw std::runtime_error("TypeError: subs() third argument must be a value, matrix, or list.");
        }

        if (vars.size() != vals.size())
            throw std::runtime_error("TypeError: subs() variable count (" + std::to_string(vars.size()) +
                ") and value count (" + std::to_string(vals.size()) + ") must match.");

        for (size_t i = 0; i < vars.size(); ++i)
            result = subs(result, vars[i], vals[i]);

        result = simplify(collapseSymFuncs(result, *fnsPtr, this->builtinArity));

        if (result.ptr->getType() == SymType::NUM) {
            auto num = std::static_pointer_cast<SymNum>(result.ptr);
            return casValToValue(num->value);
        }
        return Value(result);
        });

    reg("toFunc", { 2 }, [this](const std::vector<Value>& args) -> Value {
        jc::SymExpr ast = args[0].asSymbolic();
        std::vector<std::string> varNames;
        if (args[1].isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(args[1].asObj())->vec) {
                if (v.isString()) {
                    varNames.push_back(v.asString());
                } else if (v.isSymbolic() && v.asSymbolic().ptr->getType() == SymType::VAR) {
                    varNames.push_back(std::static_pointer_cast<SymVar>(v.asSymbolic().ptr)->name);
                } else {
                    throw std::runtime_error("toFunc: Variable names must be strings or symbols.");
                }
            }
        }
        else if (args[1].isObjType(ObjType::STRING_MATRIX)) {
            for (const auto& s : static_cast<ObjStringMatrix*>(args[1].asObj())->mat.rawData()) {
                varNames.push_back(s);
            }
        }
        else if (args[1].isString()) {
            varNames.push_back(args[1].asString());
        }
        else if (args[1].isSymbolic() && args[1].asSymbolic().ptr->getType() == SymType::VAR) {
            varNames.push_back(std::static_pointer_cast<SymVar>(args[1].asSymbolic().ptr)->name);
        }
        else {
            throw std::runtime_error("toFunc(): 2nd argument must be a string, symbol, List, or StringMatrix of variable names.");
        }
        int argCount = static_cast<int>(varNames.size());
        std::vector<bool> pRefs(argCount, false);

        ObjClosure* cls = GcHeap::get().allocate<ObjClosure>(
            varNames, pRefs, "<sym_to_func>", nullptr
        );
        cls->defaultValues.resize(argCount, jc::Value::none());
        auto arities = this->builtinArity;
        jc::SymbolicFuncResolver resolver = [arities](const std::string& name, const std::vector<jc::Value>& fnArgs) -> jc::Value {
            if (!jc::VM::activeVM) throw std::runtime_error("toFunc error: VM context lost.");

            const auto& builtins = jc::VM::activeVM->getNativeBuiltins();
            auto it = builtins.find(name);
            if (it != builtins.end()) {
                auto ait = arities.find(name);
                if (ait != arities.end() && !ait->second.empty()) {
                    if (ait->second.find(static_cast<int>(fnArgs.size())) == ait->second.end()) {
                        throw std::runtime_error("Runtime Error: Function '" + name + "' expects wrong number of arguments.");
                    }
                }
                return it->second(fnArgs);
            }
            throw std::runtime_error("toFunc error: Math function '" + name + "' not found in BuiltinRegistry.");
            };
        auto jc_caller = [ast, varNames, resolver](const std::vector<jc::Value>& call_args) -> jc::Value {
            if (call_args.size() != varNames.size()) {
                throw std::runtime_error("Compiled function expects " + std::to_string(varNames.size()) + " arguments.");
            }
            bool isPureReal = true;
            for (const auto& arg : call_args) {
                if (arg.isComplex() || arg.isObjType(ObjType::REAL_MATRIX) ||
                    arg.isObjType(ObjType::COMPLEX_MATRIX) ||
                    arg.isObjType(ObjType::STRING_MATRIX)) {
                    isPureReal = false;
                    break;
                }
            }
            if (isPureReal) {
                std::map<std::string, double> env;
                for (size_t i = 0; i < varNames.size(); ++i) env[varNames[i]] = call_args[i].asDouble();
                return jc::Value(jc::fastEval(ast.ptr, env, resolver));
            }

            std::map<std::string, jc::Value> valEnv;
            for (size_t i = 0; i < varNames.size(); ++i) valEnv[varNames[i]] = call_args[i];

            return jc::evalUniversal(ast.ptr, valEnv, resolver);
            };
        cls->nativeFn = std::make_any<NativeCallable>(std::move(jc_caller));
        return jc::Value(cls);
        });

    reg("evalf", { 1 }, [fnsPtr, this](const std::vector<Value>& args) -> Value {
        SymExpr expr = args[0].asSymbolic();
        expr = evalFloat(expr);
        expr = collapseSymFuncs(expr, *fnsPtr, this->builtinArity);

        if (isConstantExpr(expr)) {
            try {
                std::map<std::string, Value> emptyEnv;
                auto arities = this->builtinArity;
                SymbolicFuncResolver resolver = [fnsPtr, arities](const std::string& name, const std::vector<Value>& fnArgs) -> Value {
                    auto it = fnsPtr->find(name);
                    if (it != fnsPtr->end()) {
                        auto ait = arities.find(name);
                        if (ait != arities.end() && !ait->second.empty()) {
                            if (ait->second.find(static_cast<int>(fnArgs.size())) == ait->second.end()) {
                                throw std::runtime_error("Runtime Error: Function '" + name + "' expects wrong number of arguments.");
                            }
                        }
                        return it->second(fnArgs);
                    }
                    throw std::runtime_error("Function not found");
                };
                return evalUniversal(expr.ptr, emptyEnv, resolver);
            } catch (const jc::EngineInterruptError&) {
                throw;
            } catch (...) {}
        }
        return Value(expr);
        });

    reg("evalv", { 1 }, [fnsPtr, this](const std::vector<Value>& args) -> Value {
        SymExpr expr = args[0].asSymbolic();
        expr = evalValue(expr);
        expr = simplify(collapseSymFuncs(expr, *fnsPtr, this->builtinArity));

        if (isConstantExpr(expr)) {
            try {
                std::map<std::string, Value> emptyEnv;
                auto arities = this->builtinArity;
                SymbolicFuncResolver resolver = [fnsPtr, arities](const std::string& name, const std::vector<Value>& fnArgs) -> Value {
                    auto it = fnsPtr->find(name);
                    if (it != fnsPtr->end()) {
                        auto ait = arities.find(name);
                        if (ait != arities.end() && !ait->second.empty()) {
                            if (ait->second.find(static_cast<int>(fnArgs.size())) == ait->second.end()) {
                                throw std::runtime_error("Runtime Error: Function '" + name + "' expects wrong number of arguments.");
                            }
                        }
                        return it->second(fnArgs);
                    }
                    throw std::runtime_error("Function not found");
                };
                return evalUniversal(expr.ptr, emptyEnv, resolver);
            } catch (const jc::EngineInterruptError&) {
                throw;
            } catch (...) {}
        }
        return Value(expr);
        });

    reg("replaceRule", { 3 }, [](const std::vector<Value>& args) -> Value {
        SymExpr expr = args[0].asSymbolic();
        SymExpr pattern = args[1].asSymbolic();
        SymExpr target = args[2].asSymbolic();
        return Value(jc::simplify(jc::applyRule(expr, pattern, target)));
    });

    reg("solveEq", { 2 }, [getVarName](const std::vector<Value>& args) -> Value {
        auto roots = jc::solveEq(args[0].asSymbolic(), getVarName(args[1], "solveEq"));
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        for (const auto& r : roots) {
            if (r.ptr->getType() == SymType::FUNC) {
                auto func = std::static_pointer_cast<SymFunc>(r.ptr);
                if (func->name == "RootOf" && func->args.size() == 3) {
                    SymExpr P(func->args[0]);
                    std::string dummy = std::static_pointer_cast<SymVar>(func->args[1])->name;
                    int k = 1;
                    if (func->args[2]->getType() == SymType::NUM) {
                        auto [isInt, val] = jc::extractExactInt(std::static_pointer_cast<SymNum>(func->args[2])->value);
                        if (isInt) k = static_cast<int>(val);
                    }
                    
                    auto coeffs = jc::extractCoeffs(P, dummy);
                    int deg = static_cast<int>(coeffs.size()) - 1;
                    if (deg >= 1 && deg <= 4) {
                        auto exactRoots = jc::getExactRoots(coeffs);
                        if (!exactRoots.empty() && k >= 1 && k <= deg) {
                            L->vec.push_back(Value(exactRoots[k - 1]));
                            continue;
                        }
                    }
                }
            }
            L->vec.push_back(Value(r));
        }
        return Value(L);
    });

    reg("polyDiv", { 3 }, [getVarName](const std::vector<Value>& args) -> Value {
        auto [q, r] = jc::polyDiv(args[0].asSymbolic(), args[1].asSymbolic(), getVarName(args[2], "polyDiv"));
        ObjList* L = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(L);
        L->vec.push_back(Value(q));
        L->vec.push_back(Value(r));
        L->is_frozen = true;
        L->is_hashable_cached = true;
        return Value(L);
    });

    reg("polyGCD", { 3 }, [getVarName](const std::vector<Value>& args) -> Value {
        return Value(jc::polyGCD(args[0].asSymbolic(), args[1].asSymbolic(), getVarName(args[2], "polyGCD")));
    });

    reg("resultant", { 3 }, [getVarName](const std::vector<Value>& args) -> Value {
        return Value(jc::polyResultant(args[0].asSymbolic(), args[1].asSymbolic(), getVarName(args[2], "resultant")));
    });

    reg("factor", { 1 }, [toBigInt](const std::vector<Value>& args) -> Value {
        if (args[0].isSymbolic()) {
            return Value(jc::factor(args[0].asSymbolic()));
        }
        auto factors = toBigInt(args[0]).factorize();
        ObjDict* d = GcHeap::get().allocate<ObjDict>();
        GcObjGuard guard(d);
        for (const auto& f : factors) {
            Value k(f.first);
            Value v = Value::fromInt32(f.second);
            d->keyMap[k] = d->elements.size();
            d->elements.push_back({k, v});
        }
        return Value(d);
        });

    reg("factorReal", { 1 }, [](const std::vector<Value>& args) -> Value {
        return Value(jc::factorReal(args[0].asSymbolic()));
        });

    reg("taylor", { 3, 4 }, [getVarName](const std::vector<Value>& args) -> Value {
        int order = 5;
        if (args.size() == 4) order = static_cast<int>(std::round(args[3].asDouble()));
        return Value(jc::taylor(args[0].asSymbolic(), getVarName(args[1], "taylor"), args[2].asSymbolic(), order));
    });

    reg("limit", { 2, 3, 4 }, [evalFunc, getVarName](const std::vector<Value>& args) -> Value {
        bool isSymLimit = args[0].isSymbolic();
        if (!isSymLimit && args.size() >= 3) {
            if (args[1].isString() || (args[1].isSymbolic() && args[1].asSymbolic().ptr->getType() == SymType::VAR)) {
                isSymLimit = true;
            }
        }
        if (isSymLimit) {
            if (args.size() < 3 || args.size() > 4) throw std::runtime_error("TypeError: Symbolic limit expects 3 or 4 arguments: expr, var, val, [dir].");
            std::string dir = "";
            if (args.size() == 4) {
                if (!args[3].isString()) throw std::runtime_error("TypeError: Symbolic limit direction must be a string ('+' or '-').");
                dir = args[3].asString();
            }
            SymExpr valExpr;
            if (args[2].isString()) valExpr = SymExpr::makeVar(args[2].asString());
            else valExpr = args[2].asSymbolic();
            return Value(jc::limit(args[0].asSymbolic(), getVarName(args[1], "limit"), valExpr, dir));
        }

        if (args.size() > 3) {
            throw std::runtime_error("TypeError: Numeric limit expects 2 or 3 arguments: func, x0, [dir].");
        }

        auto cl = args[0].asFunction();
        double x0 = args[1].asDouble();
        double h = 1e-7;
        
        auto safeEval = [&](double x) -> double {
            try { return evalFunc(cl, x); }
            catch (const jc::EngineInterruptError&) { throw; }
            catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
        };

        std::string dir = "";
        if (args.size() == 3) {
            if (args[2].isString()) dir = args[2].asString();
            else if (args[2].isNumber()) {
                double d = args[2].asDouble();
                dir = d > 0 ? "+" : (d < 0 ? "-" : "");
            }
        }
        
        if (dir == "+") {
            double v = safeEval(x0 + h);
            if (std::isnan(v)) throw std::runtime_error("Math Error: Right limit does not exist.");
            return Value(v);
        }
        if (dir == "-") {
            double v = safeEval(x0 - h);
            if (std::isnan(v)) throw std::runtime_error("Math Error: Left limit does not exist.");
            return Value(v);
        }
        
        double left = safeEval(x0 - h);
        double right = safeEval(x0 + h);
        
        if (!std::isnan(left) && !std::isnan(right) && std::abs(left - right) < 1e-4) {
            return Value((left + right) / 2.0);
        }
        
        throw std::runtime_error("Math Error: Limit does not exist (left and right limits differ significantly or are undefined).");
    });

    reg("verifyInteg", { 2 }, [getVarName, this](const std::vector<Value>& args) -> Value {
        SymExpr expr = args[0].asSymbolic();
        std::string var = getVarName(args[1], "verifyInteg");
        SymExpr integral = jc::integrate(expr, var);
        SymExpr derivative = jc::diff(integral, var);
        SymExpr diff_expr = jc::simplify(derivative - expr);
        
        if (diff_expr.isZero()) return Value(true);

        std::set<std::string> vars;
        jc::collectAllVars(diff_expr.ptr, vars);
        
        // 使用复数测试点，完美避开实数域的定义域陷阱 (如 log(-x), sqrt(-x))
        // 选择模长较小的测试点，防止高次幂导致浮点误差放大
        std::vector<Complex> test_vals = {
            Complex(0.271828, 0.314159),
            Complex(0.141421, -0.173205),
            Complex(-0.223606, 0.264575),
            Complex(0.331662, 0.316227),
            Complex(-0.123456, -0.654321)
        };
        
        int pass_count = 0;
        int valid_tests = 0;

        auto evalfIt = builtins.find("evalf");

        for (const auto& tv : test_vals) {
            SymExpr subbed = diff_expr;
            for (const auto& v : vars) {
                if (v != "i" && v != "I" && v != "PI" && v != "E") {
                    subbed = jc::subs(subbed, v, SymExpr(tv));
                }
            }
            
            try {
                Value res = evalfIt->second({Value(subbed)});
                if (res.isSymbolic()) continue; // 如果 evalf 没能完全化简为数值，则跳过该测试点
                valid_tests++;
                double err = res.isComplex() ? res.asComplex().modulus() : std::abs(res.asDouble());
                if (err < 1e-4) pass_count++;
            } catch (const jc::EngineInterruptError&) {
                throw;
            } catch (...) {}
        }
        
        if (valid_tests > 0 && pass_count == valid_tests) return Value(true);
        return Value(false);
    });

    reg("diff", { 2 }, [evalFunc, getVarName](const std::vector<Value>& args) -> Value {
        bool isSymDiff = args[0].isSymbolic();
        if (!isSymDiff) {
            if (args[1].isString() || (args[1].isSymbolic() && args[1].asSymbolic().ptr->getType() == SymType::VAR)) {
                isSymDiff = true;
            }
        }
        if (isSymDiff) {
            SymExpr expr = args[0].asSymbolic();
            std::string var = getVarName(args[1], "diff");
            return Value(simplify(jc::diff(expr, var)));
        }

        auto cl = args[0].asFunction();
        double x = args[1].asDouble();
        double h = 1e-4;

        double d = (-evalFunc(cl, x + 2 * h) + 8 * evalFunc(cl, x + h)
            - 8 * evalFunc(cl, x - h) + evalFunc(cl, x - 2 * h)) / (12 * h);
        return Value(d);
        });

    reg("integ", { 2, 3, 4 }, [evalFunc, getVarName, this](const std::vector<Value>& args) -> Value {
        bool isSymInteg = args[0].isSymbolic();
        if (!isSymInteg && args.size() >= 2) {
            if (args[1].isString() || (args[1].isSymbolic() && args[1].asSymbolic().ptr->getType() == SymType::VAR)) {
                isSymInteg = true;
            }
        }
        if (isSymInteg) {
            SymExpr expr = args[0].asSymbolic();
            std::string var = getVarName(args[1], "integ");
            
            if (args.size() == 4) {
                SymExpr a = args[2].asSymbolic();
                SymExpr b = args[3].asSymbolic();
                return Value(jc::defint(expr, var, a, b));
            } else if (args.size() == 3) {
                throw std::runtime_error("TypeError: Symbolic definite integration expects 4 arguments: expr, var, a, b.");
            }
            return Value(simplify(jc::integrate(expr, var)));
        }

        if (args.size() < 3) {
            throw std::runtime_error("TypeError: Numeric integration expects at least 3 arguments: func, a, b.");
        }

        auto cl = args[0].asFunction();
        double a = args[1].asDouble(), b = args[2].asDouble();
        int n = (args.size() == 4) ? static_cast<int>(std::round(args[3].asDouble())) : 100000;
        if (n <= 0 || n % 2 != 0) n = 100000;
        double h = (b - a) / n, s = evalFunc(cl, a) + evalFunc(cl, b);
        for (int i = 1; i < n; i += 2) { jc::checkInterrupt(); s += 4 * evalFunc(cl, a + i * h); }
        for (int i = 2; i < n - 1; i += 2) { jc::checkInterrupt(); s += 2 * evalFunc(cl, a + i * h); }
        return Value(s * h / 3.0);
        });
}

} // namespace jc
