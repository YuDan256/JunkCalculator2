//Vm.cpp
#include "VM.h"
#include "../modules/Module.h"
#include "BuiltinRegistry.h"
#include "../frontend/Highlight.h"
#include "../frontend/Lexer.h"
#include "../frontend/Parser.h"
#include "../frontend/Compiler.h"
#include "../memory/GcHeap.h"
#include "EngineInterrupt.h"
#include <iostream>
#include <cmath>

// ★ 开启 VM 级指令与栈追踪日志
#define JC2_DEBUG_VM_TRACE false
#include <stdexcept>
#include <filesystem>
#include <sstream>
namespace jc {

    // =======================================================
    // ★ 字符串驻留池 (String Interning)
    // =======================================================
    std::unordered_map<std::string, ObjString*> g_internedStrings;

    ObjString* internString(const std::string& str) {
        auto it = g_internedStrings.find(str);
        if (it != g_internedStrings.end()) {
            return it->second;
        }
        ObjString* obj = GcHeap::get().allocate<ObjString>(str);
        g_internedStrings[str] = obj;
        return obj;
    }

    // =======================================================
    // ★ 统一拦截与展开 Try-Catch 栈
    // =======================================================
    bool VM::handleExceptionUnwind(std::string& msg) {
#if JC2_DEBUG_VM_TRACE
        std::cout << "[VM TRACE] Exception Thrown: " << msg << "\n";
#endif
        if (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= currentTargetFrameDepth) {
            auto handler = exceptionHandlers.back();
            exceptionHandlers.pop_back();
#if JC2_DEBUG_VM_TRACE
            std::cout << "[VM TRACE] Caught by handler at IP " << handler.ip << ", restoring stack to " << handler.stackSize << "\n";
#endif

            // 剥除所有比 catch 更深的函数堆栈
            while (frameCount > handler.frameIndex + 1) {
                frames[frameCount - 1].selfContext = Value::none();
                frames[frameCount - 1].classContext = Value::none();
                frames[frameCount - 1].closure = nullptr;
                frames[frameCount - 1].refParamsBase = -1;
                frameCount--;
            }

            // 回复当时的变量栈大小
            closeUpvalues(handler.stackSize);
            setStackSize(handler.stackSize);

            // 向前兼容清洗（万一由内部某处带上了 [Line，强行剥离保证纯净赋给 e 变量）
            if (msg.find("[Line ") == 0) {
                size_t c = msg.find("] ");
                if (c != std::string::npos) msg = msg.substr(c + 2);
            }

            push(Value(msg));
            frame().ip = handler.ip;
            return true; // 代表已经成功捕获，指示外层继续 run()
        }
        return false; // 无人捕获，通知外层构建 StackTrace 熔断！
    }
    // =======================================================
    // ★ 构建绚丽的 Traceback 调用栈回溯日志
    // =======================================================
    std::string VM::buildStackTrace(const std::string& errorMsg) {
        std::ostringstream oss;
        oss << errorMsg << "\n";

        // ★ 核心修复 1：不要使用 RESET 把外层的红洗没，使用专属于后续文本的颜色控制！
        oss << col(Ansi::GRAY) << "Traceback (most recent call last):\n";

        for (int i = frameCount - 1; i >= 0; --i) {
            const CallFrame& f = frames[i];

            int ip = f.ip - 1;
            if (ip < 0) ip = 0;

            const auto& lines = f.function->chunk.lines;
            int errLine = 0;
            if (!lines.empty()) {
                if (ip >= static_cast<int>(lines.size())) ip = static_cast<int>(lines.size()) - 1;
                errLine = lines[ip];
            }

            std::string fnName = f.function->name;
            if (fnName == "<script>" || fnName == "<eval>") {
                std::string sfile = f.function->sourceFile;
                if (sfile.empty()) sfile = "REPL";
                else {
                    try { sfile = std::filesystem::path(sfile).filename().string(); }
                    catch (...) {}
                }
                oss << "  at [Line " << errLine << "] in " << sfile << "\n";
            }
            else {
                oss << "  at [Line " << errLine << "] in " << fnName << "()\n";
            }
        }

        // ★ 核心修复 2：在所有堆栈打印完毕后，最后加一个 RESET 以确保后续无污染
        oss << col(Ansi::RESET);

        return oss.str();
    }

    std::string VM::getTypeName(const Value& val) {
        if (val.isUninit()) return "Uninitialized";
        return val.typeName();
    }

    void VM::triggerParamTypeError(const Value& val, uint16_t typeIdx, uint16_t nameIdx) {
        const std::string& expectedType = currentChunk().constants[typeIdx].asString();
        const std::string& paramName = currentChunk().constants[nameIdx].asString();
        throw std::runtime_error("TypeError: Parameter '" + paramName +
            "' expected type '" + expectedType +
            "', got '" + getTypeName(val) + "'.");
    }
    void VM::triggerReturnTypeError(const Value& val, uint16_t typeIdx) {
        const std::string& expectedType = currentChunk().constants[typeIdx].asString();
        throw std::runtime_error("TypeError: Function '" + frame().function->name +
            "' expected to return '" + expectedType +
            "', but returned '" + getTypeName(val) + "'.");
    }

    static ObjClosure* findDunder(const Value& val, const std::string& name);

    static const std::string DUNDER_ADD = "__add__";
    static const std::string DUNDER_RADD = "__radd__";
    static const std::string DUNDER_SUB = "__sub__";
    static const std::string DUNDER_RSUB = "__rsub__";
    static const std::string DUNDER_MUL = "__mul__";
    static const std::string DUNDER_RMUL = "__rmul__";
    static const std::string DUNDER_DIV = "__div__";
    static const std::string DUNDER_RDIV = "__rdiv__";
    static const std::string DUNDER_LDIV = "__ldiv__";
    static const std::string DUNDER_RLDIV = "__rldiv__";
    static const std::string DUNDER_MOD = "__mod__";
    static const std::string DUNDER_RMOD = "__rmod__";
    static const std::string DUNDER_POW = "__pow__";
    static const std::string DUNDER_RPOW = "__rpow__";
    static const std::string DUNDER_NEG = "__neg__";
    static const std::string DUNDER_BITNOT = "__bitnot__";
    static const std::string DUNDER_BITAND = "__bitand__";
    static const std::string DUNDER_RBITAND = "__rbitand__";
    static const std::string DUNDER_BITOR = "__bitor__";
    static const std::string DUNDER_RBITOR = "__rbitor__";
    static const std::string DUNDER_BITXOR = "__bitxor__";
    static const std::string DUNDER_RBITXOR = "__rbitxor__";
    static const std::string DUNDER_LSHIFT = "__lshift__";
    static const std::string DUNDER_RLSHIFT = "__rlshift__";
    static const std::string DUNDER_RSHIFT = "__rshift__";
    static const std::string DUNDER_RRSHIFT = "__rrshift__";
    static const std::string DUNDER_EQ = "__eq__";
    static const std::string DUNDER_NEQ = "__neq__";
    static const std::string DUNDER_LT = "__lt__";
    static const std::string DUNDER_LE = "__le__";
    static const std::string DUNDER_GT = "__gt__";
    static const std::string DUNDER_GE = "__ge__";
    static const std::string DUNDER_GETITEM = "__getitem__";
    static const std::string DUNDER_SETITEM = "__setitem__";
    static const std::string DUNDER_GETATTR = "__getattr__";
    static const std::string DUNDER_SETATTR = "__setattr__";
    static const std::string DUNDER_CALL = "__call__";
    static const std::string DUNDER_ITER = "__iter__";
    static const std::string DUNDER_NEXT = "__next__";
    static const std::string DUNDER_STR = "__str__";
    static const std::string DUNDER_BOOL = "__bool__";
    static const std::string DUNDER_CONTAINS = "__contains__";

    bool VM::checkValueType(const Value& val, const std::string& typeStr) {
        if (typeStr == "any" || typeStr.empty()) return true;

        // 1. 精确类型 & 联合类型
        if (typeStr == "int") return val.isInt32() || val.isObjType(ObjType::BIGINT);
        if (typeStr == "float" || typeStr == "double") return val.isDouble();
        if (typeStr == "real") return val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION) || val.isObjType(ObjType::BASENUM) || (val.isComplex() && val.asComplex().imag == 0.0);
        if (typeStr == "number") return val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION) || val.isObjType(ObjType::BASENUM) || val.isComplex();
        if (typeStr == "whole") return val.isInt32() || val.isObjType(ObjType::BIGINT) || (val.isDouble() && std::isfinite(val.asDoubleRaw()) && val.asDoubleRaw() == std::floor(val.asDoubleRaw())) || (val.isObjType(ObjType::FRACTION) && static_cast<ObjFraction*>(val.asObj())->frac.getDen() == BigInt(1)) || (val.isComplex() && val.asComplex().imag == 0.0 && std::isfinite(val.asComplex().real) && val.asComplex().real == std::floor(val.asComplex().real));
        if (typeStr == "exact") return val.isInt32() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION) || val.isObjType(ObjType::BASENUM) || val.isObjType(ObjType::SYMBOLIC);
        if (typeStr == "string" || typeStr == "str") return val.isString();
        if (typeStr == "bool") return val.isBool();
        if (typeStr == "binary" || typeStr == "bool_like") {
            if (val.isBool()) return true;
            try {
                double d = val.asDouble();
                if (d == 0.0 || d == 1.0) return true;
            } catch (...) {}
            return false;
        }
        if (typeStr == "none") return val.isNone();
        if (typeStr == "list") return val.isObjType(ObjType::LIST);
        if (typeStr == "dict") return val.isObjType(ObjType::DICT);
        if (typeStr == "set") return val.isObjType(ObjType::SET);
        if (typeStr == "fraction") return val.isObjType(ObjType::FRACTION);
        if (typeStr == "complex") return val.isObjType(ObjType::COMPLEX);
        if (typeStr == "basenum") return val.isObjType(ObjType::BASENUM);
        if (typeStr == "symbolic" || typeStr == "symbol" || typeStr == "expr") return val.isObjType(ObjType::SYMBOLIC);
        if (typeStr == "realmat") return val.isObjType(ObjType::REAL_MATRIX);
        if (typeStr == "complexmat") return val.isObjType(ObjType::COMPLEX_MATRIX);
        if (typeStr == "stringmat") return val.isObjType(ObjType::STRING_MATRIX);
        if (typeStr == "matrix") return val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) || val.isObjType(ObjType::STRING_MATRIX);
        if (typeStr == "func" || typeStr == "function") return val.isFunctionClosure();
        if (typeStr == "class") return val.isClass();
        if (typeStr == "instance") return val.isInstance();
        if (typeStr == "namespace") return val.isObjType(ObjType::NAMESPACE);

        // 2. 鸭子类型 / 接口类型
        if (typeStr == "iterable") {
            if (val.isObjType(ObjType::LIST) || val.isObjType(ObjType::DICT) || val.isObjType(ObjType::SET) ||
                val.isString() || val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) ||
                val.isObjType(ObjType::STRING_MATRIX)) return true;
            if (val.isInstance()) return findDunder(val, "__iter__") || findDunder(val, "__next__");
            return false;
        }
        if (typeStr == "callable") {
            if (val.isFunctionClosure() || val.isClass() || val.isString()) return true;
            if (val.isInstance()) return findDunder(val, "__call__") != nullptr;
            return false;
        }
        if (typeStr == "indexable") {
            if (val.isObjType(ObjType::LIST) || val.isObjType(ObjType::DICT) || val.isString() ||
                val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) ||
                val.isObjType(ObjType::STRING_MATRIX)) return true;
            if (val.isInstance()) return findDunder(val, "__getitem__") != nullptr;
            return false;
        }
        if (typeStr == "hashable") return val.isHashable();
        if (typeStr == "numeric") {
            if (val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION) ||
                val.isObjType(ObjType::COMPLEX) || val.isObjType(ObjType::BASENUM)) return true;
            if (val.isInstance()) {
                return findDunder(val, "__add__") || findDunder(val, "__mul__") || findDunder(val, "__sub__") || findDunder(val, "__div__") || findDunder(val, "__ldiv__");
            }
            return false;
        }

        // 3. 面向对象标称类型
        if (val.isInstance()) {
            auto inst = val.asInstance();
            auto c = inst->classDef;
            
            // 尝试解析可能带有模块前缀的 typeStr (如 "engine.GameEngine")
            Value typeVal = Value::none();
            size_t dotPos = typeStr.find('.');
            if (dotPos != std::string::npos) {
                std::string currentName = typeStr.substr(0, dotPos);
                auto it = globalNamesToSlots.find(currentName);
                if (it != globalNamesToSlots.end()) {
                    Value currentVal = globalValues[it->second];
                    size_t start = dotPos + 1;
                    while (start < typeStr.size()) {
                        size_t nextDot = typeStr.find('.', start);
                        std::string part = typeStr.substr(start, nextDot == std::string::npos ? std::string::npos : nextDot - start);
                        
                        if (currentVal.isObjType(ObjType::NAMESPACE)) {
                            auto ns = static_cast<ObjNamespace*>(currentVal.asObj());
                            auto fIt = ns->fields.find(part);
                            if (fIt != ns->fields.end()) {
                                currentVal = *(fIt->second.upval->location);
                            } else {
                                currentVal = Value::none();
                                break;
                            }
                        } else {
                            currentVal = Value::none();
                            break;
                        }
                        
                        if (nextDot == std::string::npos) break;
                        start = nextDot + 1;
                    }
                    typeVal = currentVal;
                }
            } else {
                auto it = globalNamesToSlots.find(typeStr);
                if (it != globalNamesToSlots.end()) typeVal = globalValues[it->second];
            }

            // 如果找到了真实的类对象，进行严格的指针比对！
            if (typeVal.isClass()) {
                ObjClass* expectedClass = static_cast<ObjClass*>(typeVal.asObj());
                while (c) {
                    if (c == expectedClass) return true;
                    c = c->parent;
                }
                return false; // 名字可能一样，但指针不同，严格拒绝！
            }

            // 如果没找到真实的类对象（可能是内置类型，或者是没导入的局部类），退化为字符串匹配
            std::string shortName = typeStr;
            size_t lastDot = typeStr.find_last_of('.');
            if (lastDot != std::string::npos) shortName = typeStr.substr(lastDot + 1);
            
            while (c) {
                if (c->name == shortName) return true;
                c = c->parent;
            }
        }
        return false;
    }

    static ObjClosure* findDunder(
        const Value& val, const std::string& name)
    {
        if (!val.isInstance())
            return nullptr;
        auto inst = val.asInstance();
        auto c = inst->classDef;
        while (c) {
            auto it = c->methods.find(name);
            if (it != c->methods.end()) return it->second;
            c = c->parent;
        }
        return nullptr;
    }

    Value VM::callDunder(const Value& obj, const std::string& name,
        const std::vector<Value>& args)
    {
        auto inst = obj.asInstance();
        auto method = findDunder(obj, name);
        if (!method) throw std::runtime_error(std::string("VM Error: No callable dunder '") + name + "'.");

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
            return result;
        }
        else if (method->isBytecode()) {
            // ★ 无污染传参：直接送入 VM CallFrame 的 boundSelf
            return callVMFunction(method->compiledFnIndex, args, method, Value(inst), Value(inst->classDef));
        }
        else {
            throw std::runtime_error(std::string("VM Error: No callable dunder '") + name + "'.");
        }
    }

    ObjUpVal* VM::captureUpvalue(Value* local) {
        ObjUpVal* prevUpval = nullptr;
        ObjUpVal* upval = openUpvalues;
        int index = static_cast<int>(local - stack);
        while (upval != nullptr && upval->stackIndex > index) {
            prevUpval = upval;
            upval = upval->nextOpen;
        }

        if (upval != nullptr && upval->stackIndex == index) {
            return upval;
        }

        ObjUpVal* createdUpval = GcHeap::get().allocate<ObjUpVal>();
        createdUpval->location = local;
        createdUpval->stackIndex = index;
        createdUpval->nextOpen = upval;

        if (prevUpval == nullptr) {
            openUpvalues = createdUpval;
        } else {
            prevUpval->nextOpen = createdUpval;
        }

        return createdUpval;
    }

    void VM::closeUpvalues(int lastStackIndex) {
        while (openUpvalues != nullptr && openUpvalues->stackIndex >= lastStackIndex) {
            ObjUpVal* upval = openUpvalues;
            upval->closed = *upval->location;
            upval->location = &upval->closed;
            openUpvalues = upval->nextOpen;
        }
    }

    VM::VM() {
        activeVM = this;
        stack = new Value[MAX_STACK + 1024]; // ★ 彻底抛弃 vector，使用原生数组
        stackTop = stack;
        stackLimit = stack + MAX_STACK;
        frames = new CallFrame[MAX_FRAMES];

        // ★ 核心重定向器：C++ 层索要 "self" 时，直接打劫当前虚拟机的寄存器！
        helpers::getGlobalCallback = [this](const std::string& name) -> Value {
            // 1. 优先满足正在运行的 C++ 原生方法栈 (如 isArray 等内置方法内部调用)
            if (name == "self" && !helpers::nativeSelfStack.empty()) return helpers::nativeSelfStack.back();
            if (name == "__class__" && !helpers::nativeClassStack.empty()) return helpers::nativeClassStack.back();

            // 2. 然后满足 VM 字节码的 CallFrame 寄存器
            if (name == "self") {
                if (frameCount == 0 || frames[frameCount - 1].selfContext.isNone()) return Value::none();
                return frames[frameCount - 1].selfContext;
            }
            if (name == "__class__") {
                if (frameCount == 0 || frames[frameCount - 1].classContext.isNone()) return Value::none();
                return frames[frameCount - 1].classContext;
            }
            // 3. 最后才是普通的全局变量
            auto it = globalNamesToSlots.find(name);
            return it != globalNamesToSlots.end() ? globalValues[it->second] : Value::none();
            };

        globalValues.reserve(65536);

        setGlobal("PI", Value(3.14159265358979323846));
        setGlobal("E", Value(2.71828182845904523536));
        setGlobal("i", Value(Complex(0.0, 1.0)));
        setGlobal("I", Value(Complex(0.0, 1.0)));
    }

    VM::~VM() {
        delete[] stack;
        delete[] frames;
    }

    std::any VM::makeNativeFn(NativeCallable fn) {
        return std::make_any<NativeCallable>(std::move(fn));
    }

    void VM::registerBuiltin(const std::string& name, NativeCallable fn, std::set<int> arity) {
        nativeBuiltins[name] = fn;
        builtinArity[name] = arity;
    }

    Value VM::getBuiltinClosure(const std::string& name) {
        auto it = builtinClosures.find(name);
        if (it != builtinClosures.end()) {
            return it->second;
        }
        auto nit = nativeBuiltins.find(name);
        if (nit != nativeBuiltins.end()) {
            auto closure = GcHeap::get().allocate<ObjClosure>(
                std::vector<std::string>{},
                std::vector<bool>{},
                name,
                nullptr
            );
            closure->nativeFn = std::make_any<NativeCallable>(nit->second);
            auto ait = builtinArity.find(name);
            if (ait != builtinArity.end() && !ait->second.empty()) {
                int maxA = *ait->second.rbegin();
                int minA = *ait->second.begin();
                for (int j = 0; j < maxA; ++j) {
                    closure->paramNames.push_back("_" + std::to_string(j));
                    closure->isRef.push_back(false);
                }
                for (int j = minA; j < maxA; ++j) {
                    closure->defaultValues.push_back(Value::none());
                }
            }
            Value val(closure);
            builtinClosures[name] = val;
            return val;
        }
        return Value::none();
    }

    void VM::setGlobal(const std::string& name, const Value& val) {
        auto it = globalNamesToSlots.find(name);
        if (it != globalNamesToSlots.end()) {
            globalValues[it->second] = val;
        } else {
            if (globalValues.size() >= 65535) throw std::runtime_error("VM Error: Too many global variables.");
            globalNamesToSlots[name] = static_cast<uint16_t>(globalValues.size());
            globalValues.push_back(val);
        }
    }

    Value VM::execute(const Chunk& c) {
        activeVM = this;
        auto mainFn = std::make_shared<CompiledFunction>();
        mainFn->name = "<script>";
        mainFn->chunk = c;
        closeUpvalues(0);
        setStackSize(0);
        while (frameCount > 0) {
            frames[frameCount - 1].selfContext = Value::none();
            frames[frameCount - 1].classContext = Value::none();
            frames[frameCount - 1].closure = nullptr;
            frames[frameCount - 1].refParamsBase = -1;
            frameCount--;
        }
        exceptionHandlers.clear();
        CallFrame mainFrame;
        mainFrame.function = mainFn.get();
        mainFrame.ip = 0;
        mainFrame.stackBase = 0;
        frames[frameCount++] = mainFrame;
        return run(0);
    }

    Value VM::callVMFunction(int fnIdx, const std::vector<Value>& args,
        ObjClosure* closure,
        Value boundSelf, Value boundClass) {
        if (fnIdx < 0 || fnIdx >= static_cast<int>(compiledFunctions.size()))
            throw std::runtime_error("VM Error: Invalid function index in callback.");
        auto fn = compiledFunctions[fnIdx]; // ★ 拷贝 shared_ptr，防止 run 期间 compiledFunctions 重新分配导致悬空引用
        int savedTargetFrameDepth = currentTargetFrameDepth;
        auto savedCallRefs = pendingCallRefs;
        pendingCallRefs.clear();

        // ★ 临时保护 boundSelf 和 boundClass，防止在打包变长参数触发 GC 时被回收
        helpers::nativeSelfStack.push_back(boundSelf);
        helpers::nativeClassStack.push_back(boundClass);

        for (const auto& arg : args)
            push(arg);

        uint8_t argc = static_cast<uint8_t>(args.size());
        if (fn->hasRestParam) {
            int fixedMax = fn->maxArity - 1;
            if (static_cast<int>(argc) < fn->arity) {
                throw std::runtime_error("VM Error: '" + fn->name + "' requires at least " + std::to_string(fn->arity) + " arguments.");
            }

            ObjList* restList = GcHeap::get().allocate<ObjList>();
            if (static_cast<int>(argc) > fixedMax) {
                int restCount = static_cast<int>(argc) - fixedMax;
                std::vector<Value> tempValues(restCount);
                for (int j = 0; j < restCount; j++) {
                    tempValues[restCount - 1 - j] = pop();
                }
                for (int j = 0; j < restCount; j++) {
                    restList->vec.push_back(tempValues[j]);
                }
                argc = static_cast<uint8_t>(fixedMax);
            }


            int padCount = fixedMax - static_cast<int>(argc);
            for (int j = 0; j < padCount; ++j) push(Value::none());
            push(Value(restList));
            argc = static_cast<uint8_t>(fn->maxArity);
        }
        else {
            if (static_cast<int>(argc) < fn->arity || static_cast<int>(argc) > fn->maxArity) {
                throw std::runtime_error("VM Error: '" + fn->name + "' expects " + std::to_string(fn->arity) + " to " + std::to_string(fn->maxArity) + " arguments, got " + std::to_string(argc) + ".");
            }
            int padCount = fn->maxArity - static_cast<int>(argc);
            for (int j = 0; j < padCount; ++j) push(Value::none());
            argc = static_cast<uint8_t>(fn->maxArity);
        }

        int reserveCount = fn->localCount - fn->maxArity;
        for (int j = 0; j < reserveCount; ++j) push(Value::none());

        CallFrame newFrame;
        newFrame.function = fn.get();
        newFrame.ip = 0;
        newFrame.stackBase = static_cast<int>(getStackSize()) - fn->localCount;
        newFrame.closure = closure;
        // ★ 清爽下发！寄存器已就位：
        newFrame.selfContext = boundSelf;
        newFrame.classContext = boundClass;
        populateRefParams(newFrame, fn.get());
        if (frameCount >= MAX_FRAMES) {
            helpers::nativeSelfStack.pop_back();
            helpers::nativeClassStack.pop_back();
            throw std::runtime_error("VM Error: CallFrame stack overflow.");
        }
        frames[frameCount++] = newFrame;

        helpers::nativeSelfStack.pop_back();
        helpers::nativeClassStack.pop_back();

        int boundary = frameCount - 1;

        Value result;
        std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
        if (profileMode) {
            start_time = std::chrono::high_resolution_clock::now();
        }
        try {
            result = run(boundary);
        }
        catch (const StackTracedException&) {
            currentTargetFrameDepth = savedTargetFrameDepth;
            pendingCallRefs = savedCallRefs;
            while (frameCount > boundary) {
                frames[frameCount - 1].selfContext = Value::none();
                frames[frameCount - 1].classContext = Value::none();
                frames[frameCount - 1].closure = nullptr;
                frames[frameCount - 1].refParamsBase = -1;
                frameCount--;
            }
            closeUpvalues(newFrame.stackBase);
            setStackSize(newFrame.stackBase);
            throw;
        }
        catch (...) {
            currentTargetFrameDepth = savedTargetFrameDepth;
            pendingCallRefs = savedCallRefs;
            while (frameCount > boundary) {
                frames[frameCount - 1].selfContext = Value::none();
                frames[frameCount - 1].classContext = Value::none();
                frames[frameCount - 1].closure = nullptr;
                frames[frameCount - 1].refParamsBase = -1;
                frameCount--;
            }
            closeUpvalues(newFrame.stackBase);
            setStackSize(newFrame.stackBase);
            throw;
        }
        if (profileMode) {
            auto end_time = std::chrono::high_resolution_clock::now();
            double duration = std::chrono::duration<double, std::milli>(end_time - start_time).count();
            auto& prof = funcProfiles[fn->name];
            prof.callCount++;
            prof.totalTimeMs += duration;
        }

        currentTargetFrameDepth = savedTargetFrameDepth;
        pendingCallRefs = savedCallRefs;

        return result;
    }

    // =======================================================
    // 返回当前执行帧指令对应的源码行号（仅用于断点调试显示）
    // =======================================================
    int VM::currentLine() {
        if (frameCount == 0) return 0;
        int errorIp = frame().ip - 1;
        if (errorIp < 0) errorIp = 0;
        const auto& lines = currentChunk().lines;
        for (int i = std::min(errorIp, static_cast<int>(lines.size()) - 1); i >= 0; --i) {
            if (lines[i] > 0) return lines[i];
        }
        return 0;
    }

    Value VM::run(int targetFrameDepth) {
        currentTargetFrameDepth = targetFrameDepth;

        CallFrame* currentFrame = &frames[frameCount - 1];
        const Chunk* chunk = &currentFrame->function->chunk;
        const uint8_t* codeData = chunk->code.data();
        int codeSize = static_cast<int>(chunk->code.size());

        #define UPDATE_FRAME() \
            do { \
                currentFrame = &frames[frameCount - 1]; \
                chunk = &currentFrame->function->chunk; \
                codeData = chunk->code.data(); \
                codeSize = static_cast<int>(chunk->code.size()); \
            } while(false)

        while (true) {
            // ═══ GC 自动触发探针与中断探针 ═══
            if (++gcInstructionCounter_ >= 2048) {
                gcInstructionCounter_ = 0;
                jc::checkInterrupt();
                if (GcHeap::get().shouldCollect()) {
                    collectGarbage();
                }
            }

            // =======================================================
            // ★ 调试器拦截探针 (Debugger Interceptor)
            // =======================================================
            if (debugMode) {
                int currentL = currentLine();
                if (currentL > 0) {
                    bool hitBreak = breakpoints.count(currentL) && currentL != lastDebugLine;
                    bool hitStep = stepNextLine && currentL != lastDebugLine;
                    if (hitBreak || hitStep) {
                        lastDebugLine = currentL;
                        stepNextLine = false;
                        debugPrompt();  // 挂起虚拟机，进入时间停止的四次元空间！
                    }
                }
            }

            if (currentFrame->ip >= codeSize) {
                return getStackSize() == 0 ? Value::none() : pop();
            }
            
            OpCode op;
            try {
                op = static_cast<OpCode>(codeData[currentFrame->ip++]);
            }
            catch (...) {
                return getStackSize() == 0 ? Value::none() : pop();
            }

#if JC2_DEBUG_VM_TRACE
            std::cout << "[VM TRACE] IP: " << currentFrame->ip - 1 
                      << " | OP: " << opCodeToString(op) 
                      << " | STACK (" << getStackSize() << "): [";
            for (size_t i = 0; i < getStackSize(); ++i) {
                if (stack[i].isString()) std::cout << "\"" << stack[i].asString() << "\"";
                else std::cout << stack[i];
                if (i < getStackSize() - 1) std::cout << ", ";
            }
            std::cout << "]\n";
#endif

			// =======================================================
			// ★ Profiler 探针: 记录微观指令 (Instruction Tick)
			// =======================================================
			if (profileMode) {
				opCounts[op]++;
			}

            #define readByte() (codeData[currentFrame->ip++])
            #define readShort() (currentFrame->ip += 2, static_cast<uint16_t>((codeData[currentFrame->ip - 2] << 8) | codeData[currentFrame->ip - 1]))

            try {

                switch (op) {

                case OpCode::OP_CONSTANT: {
                    uint16_t idx = readShort();
                    push(chunk->constants[idx]);
                    break;
                }
                case OpCode::OP_NONE:  push(Value::none()); break;
                case OpCode::OP_TRUE:  push(Value(true)); break;
                case OpCode::OP_FALSE: push(Value(false)); break;
                case OpCode::OP_POP:   pop(); stack[getStackSize()] = Value::none(); break;

                case OpCode::OP_IS_UNINIT: {
                    Value res = Value(peek(0).isUninit());
                    peek(0) = res;
                    break;
                }

                case OpCode::OP_GET_SELF: {
                    if (currentFrame->selfContext.isNone()) throw std::runtime_error("VM Error: 'self' accessed outside of context.");
                    push(currentFrame->selfContext);
                    break;
                }

                case OpCode::OP_ADD: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { double res = a.asDoubleRaw() + b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) {
                        int64_t res = static_cast<int64_t>(a.asInt32()) + b.asInt32();
                        if (res >= INT32_MIN && res <= INT32_MAX) { pop(); peek(0) = Value(static_cast<int32_t>(res)); break; }
                    }
                    if (a.isInstance() && findDunder(a, DUNDER_ADD)) { Value res = callDunder(a, DUNDER_ADD, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RADD)) { Value res = callDunder(b, DUNDER_RADD, { a }); pop(); peek(0) = res; break; }
                    Value res = a + b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_SUBTRACT: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { double res = a.asDoubleRaw() - b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) {
                        int64_t res = static_cast<int64_t>(a.asInt32()) - b.asInt32();
                        if (res >= INT32_MIN && res <= INT32_MAX) { pop(); peek(0) = Value(static_cast<int32_t>(res)); break; }
                    }
                    if (a.isInstance() && findDunder(a, DUNDER_SUB)) { Value res = callDunder(a, DUNDER_SUB, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RSUB)) { Value res = callDunder(b, DUNDER_RSUB, { a }); pop(); peek(0) = res; break; }
                    Value res = a - b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_MULTIPLY: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { double res = a.asDoubleRaw() * b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) {
                        int64_t res = static_cast<int64_t>(a.asInt32()) * b.asInt32();
                        if (res >= INT32_MIN && res <= INT32_MAX) { pop(); peek(0) = Value(static_cast<int32_t>(res)); break; }
                    }
                    if (a.isInstance() && findDunder(a, DUNDER_MUL)) { Value res = callDunder(a, DUNDER_MUL, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RMUL)) { Value res = callDunder(b, DUNDER_RMUL, { a }); pop(); peek(0) = res; break; }
                    Value res = a * b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_DIVIDE: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { double res = a.asDoubleRaw() / b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInstance() && findDunder(a, DUNDER_DIV)) { Value res = callDunder(a, DUNDER_DIV, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RDIV)) { Value res = callDunder(b, DUNDER_RDIV, { a }); pop(); peek(0) = res; break; }
                    Value res = a / b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_LEFT_DIVIDE: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_LDIV)) { Value res = callDunder(a, DUNDER_LDIV, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RLDIV)) { Value res = callDunder(b, DUNDER_RLDIV, { a }); pop(); peek(0) = res; break; }
                    Value res = ldivide(a, b); pop(); peek(0) = res; break;
                }
                case OpCode::OP_MODULO: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_MOD)) { Value res = callDunder(a, DUNDER_MOD, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RMOD)) { Value res = callDunder(b, DUNDER_RMOD, { a }); pop(); peek(0) = res; break; }
                    Value res = a % b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_POWER: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_POW)) { Value res = callDunder(a, DUNDER_POW, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RPOW)) { Value res = callDunder(b, DUNDER_RPOW, { a }); pop(); peek(0) = res; break; }
                    Value res = a ^ b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_NEGATE: {
                    Value& a = peek(0);
                    if (a.isInstance() && findDunder(a, DUNDER_NEG)) { Value res = callDunder(a, DUNDER_NEG, {}); peek(0) = res; break; }
                    Value res = -a; peek(0) = res; break;
                }
                case OpCode::OP_NOT: { 
                    Value res = Value(!peek(0).truthy()); peek(0) = res; break; 
                }
                case OpCode::OP_BIT_NOT: {
                    Value& a = peek(0);
                    if (a.isInstance() && findDunder(a, DUNDER_BITNOT)) { Value res = callDunder(a, DUNDER_BITNOT, {}); peek(0) = res; break; }
                    Value res = ~a; peek(0) = res; break;
                }

                case OpCode::OP_BIT_AND: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_BITAND)) { Value res = callDunder(a, DUNDER_BITAND, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RBITAND)) { Value res = callDunder(b, DUNDER_RBITAND, { a }); pop(); peek(0) = res; break; }
                    Value res = a & b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_BIT_OR: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_BITOR)) { Value res = callDunder(a, DUNDER_BITOR, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RBITOR)) { Value res = callDunder(b, DUNDER_RBITOR, { a }); pop(); peek(0) = res; break; }
                    Value res = a | b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_BIT_XOR: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_BITXOR)) { Value res = callDunder(a, DUNDER_BITXOR, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RBITXOR)) { Value res = callDunder(b, DUNDER_RBITXOR, { a }); pop(); peek(0) = res; break; }
                    Value res = bitXor(a, b); pop(); peek(0) = res; break;
                }
                case OpCode::OP_BIT_SHIFT_LEFT: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_LSHIFT)) { Value res = callDunder(a, DUNDER_LSHIFT, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RLSHIFT)) { Value res = callDunder(b, DUNDER_RLSHIFT, { a }); pop(); peek(0) = res; break; }
                    Value res = a << b; pop(); peek(0) = res; break;
                }
                case OpCode::OP_BIT_SHIFT_RIGHT: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isInstance() && findDunder(a, DUNDER_RSHIFT)) { Value res = callDunder(a, DUNDER_RSHIFT, { b }); pop(); peek(0) = res; break; }
                    if (b.isInstance() && findDunder(b, DUNDER_RRSHIFT)) { Value res = callDunder(b, DUNDER_RRSHIFT, { a }); pop(); peek(0) = res; break; }
                    Value res = a >> b; pop(); peek(0) = res; break;
                }

                case OpCode::OP_EQUAL: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { bool res = a.asDoubleRaw() == b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) { bool res = a.asInt32() == b.asInt32(); pop(); peek(0) = Value(res); break; }
                    if (a.isInstance() && findDunder(a, DUNDER_EQ)) { Value res = Value(callDunder(a, DUNDER_EQ, { b }).truthy()); pop(); peek(0) = res; break; }
                    Value res = Value(Value::equals(a, b)); pop(); peek(0) = res; break;
                }
                case OpCode::OP_NOT_EQUAL: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { bool res = a.asDoubleRaw() != b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) { bool res = a.asInt32() != b.asInt32(); pop(); peek(0) = Value(res); break; }
                    if (a.isInstance() && findDunder(a, DUNDER_NEQ)) { Value res = Value(callDunder(a, DUNDER_NEQ, { b }).truthy()); pop(); peek(0) = res; break; }
                    if (a.isInstance() && findDunder(a, DUNDER_EQ)) { Value res = Value(!callDunder(a, DUNDER_EQ, { b }).truthy()); pop(); peek(0) = res; break; }
                    Value res = Value(!Value::equals(a, b)); pop(); peek(0) = res; break;
                }
                case OpCode::OP_LESS: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { bool res = a.asDoubleRaw() < b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) { bool res = a.asInt32() < b.asInt32(); pop(); peek(0) = Value(res); break; }
                    if (a.isInstance() && findDunder(a, DUNDER_LT)) { Value res = Value(callDunder(a, DUNDER_LT, { b }).truthy()); pop(); peek(0) = res; break; }
                    Value res;
                    if ((a.isBigInt() || a.isInt32()) && (b.isBigInt() || b.isInt32())) res = Value(a.asBigInt() < b.asBigInt());
                    else if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) res = Value(static_cast<ObjFraction*>(a.asObj())->frac < static_cast<ObjFraction*>(b.asObj())->frac);
                    else if (a.isString() && b.isString()) res = Value(a.asString() < b.asString());
                    else { double da = a.asDouble(), db = b.asDouble(); res = Value(da < db); }
                    pop(); peek(0) = res; break;
                }
                case OpCode::OP_LESS_EQUAL: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { bool res = a.asDoubleRaw() <= b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) { bool res = a.asInt32() <= b.asInt32(); pop(); peek(0) = Value(res); break; }
                    if (a.isInstance() && findDunder(a, DUNDER_LE)) { Value res = Value(callDunder(a, DUNDER_LE, { b }).truthy()); pop(); peek(0) = res; break; }
                    Value res;
                    if ((a.isBigInt() || a.isInt32()) && (b.isBigInt() || b.isInt32())) res = Value(a.asBigInt() <= b.asBigInt());
                    else if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) res = Value(static_cast<ObjFraction*>(a.asObj())->frac <= static_cast<ObjFraction*>(b.asObj())->frac);
                    else if (a.isString() && b.isString()) res = Value(a.asString() <= b.asString());
                    else { double da = a.asDouble(), db = b.asDouble(); res = Value(da <= db); }
                    pop(); peek(0) = res; break;
                }
                case OpCode::OP_GREATER: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { bool res = a.asDoubleRaw() > b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) { bool res = a.asInt32() > b.asInt32(); pop(); peek(0) = Value(res); break; }
                    if (a.isInstance() && findDunder(a, DUNDER_GT)) { Value res = Value(callDunder(a, DUNDER_GT, { b }).truthy()); pop(); peek(0) = res; break; }
                    Value res;
                    if ((a.isBigInt() || a.isInt32()) && (b.isBigInt() || b.isInt32())) res = Value(a.asBigInt() > b.asBigInt());
                    else if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) res = Value(static_cast<ObjFraction*>(a.asObj())->frac > static_cast<ObjFraction*>(b.asObj())->frac);
                    else if (a.isString() && b.isString()) res = Value(a.asString() > b.asString());
                    else { double da = a.asDouble(), db = b.asDouble(); res = Value(da > db); }
                    pop(); peek(0) = res; break;
                }
                case OpCode::OP_GREATER_EQUAL: {
                    Value& b = peek(0); Value& a = peek(1);
                    if (a.isDouble() && b.isDouble()) { bool res = a.asDoubleRaw() >= b.asDoubleRaw(); pop(); peek(0) = Value(res); break; }
                    if (a.isInt32() && b.isInt32()) { bool res = a.asInt32() >= b.asInt32(); pop(); peek(0) = Value(res); break; }
                    if (a.isInstance() && findDunder(a, DUNDER_GE)) { Value res = Value(callDunder(a, DUNDER_GE, { b }).truthy()); pop(); peek(0) = res; break; }
                    Value res;
                    if ((a.isBigInt() || a.isInt32()) && (b.isBigInt() || b.isInt32())) res = Value(a.asBigInt() >= b.asBigInt());
                    else if (a.isObjType(ObjType::FRACTION) && b.isObjType(ObjType::FRACTION)) res = Value(static_cast<ObjFraction*>(a.asObj())->frac >= static_cast<ObjFraction*>(b.asObj())->frac);
                    else if (a.isString() && b.isString()) res = Value(a.asString() >= b.asString());
                    else { double da = a.asDouble(), db = b.asDouble(); res = Value(da >= db); }
                    pop(); peek(0) = res; break;
                }

                case OpCode::OP_ASSERT_PARAM_TYPE: {
                    uint16_t typeIdx = readShort();
                    uint16_t nameIdx = readShort();
                    Value val = pop();
                    execAssertParamType(val, typeIdx, nameIdx);
                    break;
                }

                case OpCode::OP_ASSERT_RETURN_TYPE: {
                    uint16_t typeIdx = readShort();
                    execAssertReturnType(peek(0), typeIdx);
                    break;
                }

                case OpCode::OP_MATCH_TYPE: {
                    uint16_t typeIdx = readShort();
                    const std::string& typeStr = chunk->constants[typeIdx].asString();
                    Value val = pop();
                    push(Value(checkValueType(val, typeStr)));
                    break;
                }

                case OpCode::OP_MATCH_SHAPE: {
                    uint16_t shapeIdx = readShort();
                    const auto& sp = chunk->shapePatterns[shapeIdx];
                    uint32_t minRows = sp.minRows;
                    uint32_t maxRows = sp.maxRows;
                    uint32_t minCols = sp.minCols;
                    uint32_t maxCols = sp.maxCols;
                    uint8_t exactMask = sp.exactMask;
                    Value val = pop();
                    bool matched = false;
                    
                    bool is1DPattern = (exactMask & 2) != 0;

                    if (val.isObjType(ObjType::LIST)) {
                        if (is1DPattern) {
                            uint32_t len = static_cast<uint32_t>(static_cast<ObjList*>(val.asObj())->vec.size());
                            matched = (len >= minCols && (maxCols == 0xFFFFFFFF || len <= maxCols));
                        }
                    } else if (val.isString()) {
                        if (is1DPattern) {
                            uint32_t len = static_cast<uint32_t>(val.asString().size());
                            matched = (len >= minCols && (maxCols == 0xFFFFFFFF || len <= maxCols));
                        }
                    } else if (val.isObjType(ObjType::REAL_MATRIX)) {
                        const auto& m = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                        if (is1DPattern) {
                            if (m.getRows() == 0 && m.getCols() == 0) matched = (0 >= minCols && (maxCols == 0xFFFFFFFF || 0 <= maxCols));
                            else matched = (m.getRows() == 1) && (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                        } else {
                            bool rMatch = (static_cast<uint32_t>(m.getRows()) >= minRows && (maxRows == 0xFFFFFFFF || static_cast<uint32_t>(m.getRows()) <= maxRows));
                            bool cMatch = (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                            if (minRows == 1 && minCols == 0 && m.getRows() == 0 && m.getCols() == 0) matched = true;
                            else matched = rMatch && cMatch;
                        }
                    } else if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                        const auto& m = static_cast<ObjComplexMatrix*>(val.asObj())->mat;
                        if (is1DPattern) {
                            if (m.getRows() == 0 && m.getCols() == 0) matched = (0 >= minCols && (maxCols == 0xFFFFFFFF || 0 <= maxCols));
                            else matched = (m.getRows() == 1) && (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                        } else {
                            bool rMatch = (static_cast<uint32_t>(m.getRows()) >= minRows && (maxRows == 0xFFFFFFFF || static_cast<uint32_t>(m.getRows()) <= maxRows));
                            bool cMatch = (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                            if (minRows == 1 && minCols == 0 && m.getRows() == 0 && m.getCols() == 0) matched = true;
                            else matched = rMatch && cMatch;
                        }
                    } else if (val.isObjType(ObjType::STRING_MATRIX)) {
                        const auto& m = static_cast<ObjStringMatrix*>(val.asObj())->mat;
                        if (is1DPattern) {
                            if (m.getRows() == 0 && m.getCols() == 0) matched = (0 >= minCols && (maxCols == 0xFFFFFFFF || 0 <= maxCols));
                            else matched = (m.getRows() == 1) && (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                        } else {
                            bool rMatch = (static_cast<uint32_t>(m.getRows()) >= minRows && (maxRows == 0xFFFFFFFF || static_cast<uint32_t>(m.getRows()) <= maxRows));
                            bool cMatch = (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                            if (minRows == 1 && minCols == 0 && m.getRows() == 0 && m.getCols() == 0) matched = true;
                            else matched = rMatch && cMatch;
                        }
                    }
#if JC2_DEBUG_VM_TRACE
                    std::cout << "[VM TRACE] OP_MATCH_SHAPE: val=" << val << " minR=" << minRows << " maxR=" << maxRows << " minC=" << minCols << " maxC=" << maxCols << " 1D=" << is1DPattern << " -> matched=" << matched << "\n";
#endif
                    
                    push(Value(matched));
                    break;
                }

                case OpCode::OP_GET_GLOBAL: {
                    uint16_t icIdx = readShort();
                    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
                    if (ic.cachedGlobalSlot != -1) {
                        push(globalValues[ic.cachedGlobalSlot]);
                        break;
                    }
                    uint16_t nameIdx = ic.nameIdx;
                    const std::string& name = chunk->constants[nameIdx].asString();

                    // ★ 虚拟机级别拦截：遇到 '__class__'，直接去它该在的物理寄存器里拿！
                    if (name == "__class__") {
                        if (currentFrame->classContext.isNone()) throw std::runtime_error("VM Error: '__class__' accessed outside of context.");
                        push(currentFrame->classContext);
                        break;
                    }

                    auto it = globalNamesToSlots.find(name);
                    if (it != globalNamesToSlots.end()) {
                        ic.cachedGlobalSlot = it->second;
                        push(globalValues[it->second]);
                    }
                    else {
                        Value builtinVal = getBuiltinClosure(name);
                        if (!builtinVal.isNone()) {
                            push(builtinVal);
                        }
                        else {
                            throw std::runtime_error("VM Error: Undefined variable '" + name + "'.");
                        }
                    }
                    break;
                }
                case OpCode::OP_SET_GLOBAL:
                case OpCode::OP_SET_GLOBAL_REF: {
                    uint16_t icIdx = readShort();
                    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
                    uint16_t nameIdx = ic.nameIdx;
                    const std::string& name = chunk->constants[nameIdx].asString();

                    // ★ 关键字保护：绝不许改写上下文关键字 !
                    if (name == "__class__")
                        throw std::runtime_error("Syntax Error: cannot override context keyword '" + name + "'.");

                    if (constGlobals.count(name))
                        throw std::runtime_error("Runtime Error: Cannot modify const variable '" + name + "'.");

                    if (op == OpCode::OP_SET_GLOBAL_REF) {
                        if (globalNamesToSlots.find(name) == globalNamesToSlots.end() && nativeBuiltins.find(name) == nativeBuiltins.end()) {
                            throw std::runtime_error("Runtime Error: Undefined variable '" + name + "'.");
                        }
                    }

                    // ★ 检查是否与内建函数 arity 冲突
                    Value& val = peek(0);
                    if (val.isFunctionClosure()) {
                        auto nit = nativeBuiltins.find(name);
                        if (nit != nativeBuiltins.end()) {
                            auto ait = builtinArity.find(name);
                            auto closure = val.asFunction();

                            if (ait == builtinArity.end() || ait->second.empty()) {
                                // 原生函数接受任意参数数量 → 完全禁止同名函数
                                throw std::runtime_error(
                                    "Runtime Error: Cannot redefine '" + name +
                                    "' — it is a variadic built-in function.");
                            }

                            // 检查用户函数的每个可接受参数数量是否与原生冲突
                            for (int a = closure->minArgs(); a <= closure->maxArgs(); ++a) {
                                if (ait->second.count(a)) {
                                    throw std::runtime_error(
                                        "Runtime Error: Cannot redefine '" + name + "' with " +
                                        std::to_string(a) + " parameter(s) — conflicts with built-in function. "
                                        "Use a different parameter count to create an overload.");
                                }
                            }
                        }
                    }

                    if (ic.cachedGlobalSlot != -1) {
                        globalValues[ic.cachedGlobalSlot] = val;
                    } else {
                        auto it = globalNamesToSlots.find(name);
                        if (it != globalNamesToSlots.end()) {
                            ic.cachedGlobalSlot = it->second;
                            globalValues[it->second] = val;
                        } else {
                            if (globalValues.size() >= 65535) throw std::runtime_error("VM Error: Too many global variables.");
                            ic.cachedGlobalSlot = static_cast<int>(globalValues.size());
                            globalNamesToSlots[name] = static_cast<uint16_t>(ic.cachedGlobalSlot);
                            globalValues.push_back(val);
                        }
                    }
                    break;
                }
                case OpCode::OP_DEFINE_CONST_GLOBAL: {
                    uint16_t icIdx = readShort();
                    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
                    uint16_t nameIdx = ic.nameIdx;
                    const std::string& name = chunk->constants[nameIdx].asString();
                    if (constGlobals.count(name)) {
                        throw std::runtime_error("Runtime Error: Cannot redefine const variable '" + name + "'.");
                    }
                    
                    if (ic.cachedGlobalSlot != -1) {
                        globalValues[ic.cachedGlobalSlot] = peek(0);
                    } else {
                        auto it = globalNamesToSlots.find(name);
                        if (it != globalNamesToSlots.end()) {
                            ic.cachedGlobalSlot = it->second;
                            globalValues[it->second] = peek(0);
                        } else {
                            if (globalValues.size() >= 65535) throw std::runtime_error("VM Error: Too many global variables.");
                            ic.cachedGlobalSlot = static_cast<int>(globalValues.size());
                            globalNamesToSlots[name] = static_cast<uint16_t>(ic.cachedGlobalSlot);
                            globalValues.push_back(peek(0));
                        }
                    }
                    constGlobals.insert(name);
                    break;
                }
                case OpCode::OP_DELETE_GLOBAL: {
                    uint16_t nameIdx = readShort();
                    const std::string& name = chunk->constants[nameIdx].asString();
                    if (constGlobals.count(name))
                        throw std::runtime_error("Runtime Error: Cannot delete const variable '" + name + "'.");
                    auto it = globalNamesToSlots.find(name);
                    if (it == globalNamesToSlots.end())
                        throw std::runtime_error("Runtime Error: Undefined variable '" + name + "'.");
                    globalValues[it->second] = Value::none();
                    globalNamesToSlots.erase(it);
                    clearAllGlobalICs();
                    break;
                }

                case OpCode::OP_GET_LOCAL: {
                    uint16_t slot = readShort();
                    push(stack[currentFrame->stackBase + slot]);
                    break;
                }
                case OpCode::OP_SET_LOCAL: {
                    uint16_t slot = readShort();
                    stack[currentFrame->stackBase + slot] = peek(0);
                    break;
                }

                case OpCode::OP_JUMP: {
                    uint16_t offset = readShort();
                    currentFrame->ip += offset;
                    break;
                }
                case OpCode::OP_JUMP_IF_FALSE: {
                    uint16_t offset = readShort();
                    if (!peek(0).truthy()) currentFrame->ip += offset;
                    break;
                }
                case OpCode::OP_JUMP_IF_TRUE: {
                    uint16_t offset = readShort();
                    if (peek(0).truthy()) currentFrame->ip += offset;
                    break;
                }
                case OpCode::OP_LOOP: {
                    uint16_t offset = readShort();
                    currentFrame->ip -= offset;
                    break;
                }

                case OpCode::OP_CLOSURE: {
                    uint16_t fnConstIdx = readShort();
                    int idx = static_cast<int>(std::round(
                        chunk->constants[fnConstIdx].asDouble()));
                    if (idx < 0 || idx >= static_cast<int>(compiledFunctions.size()))
                        throw std::runtime_error("VM Error: Invalid function index.");

                    auto& fn = compiledFunctions[idx];

                    auto closure = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{},
                        std::vector<bool>{},
                        fn->name,
                        nullptr
                    );

                    closure->compiledFnIndex = idx;

                    if (!fn->upvalues.empty()) {
                        closure->upvalueCount = static_cast<int>(fn->upvalues.size());
                        closure->upvalues = new ObjUpVal*[closure->upvalueCount];
                        for (int i = 0; i < closure->upvalueCount; ++i) {
                            auto& uv = fn->upvalues[i];
                            if (uv.isRef) {
                                // ★ 按引用捕获 (Open Upvalue)
                                if (uv.isLocal) {
                                    if (uv.isRefParam) {
                                        closure->upvalues[i] = static_cast<ObjUpVal*>(stack[currentFrame->refParamsBase + uv.index].asObj());
                                    } else {
                                        int captureIdx = currentFrame->stackBase + uv.index;
                                        closure->upvalues[i] = captureUpvalue(&stack[captureIdx]);
                                    }
                                }
                                else {
                                    if (currentFrame->closure && uv.index < currentFrame->closure->upvalueCount)
                                        closure->upvalues[i] = currentFrame->closure->upvalues[uv.index];
                                    else {
                                        auto dummy = GcHeap::get().allocate<ObjUpVal>();
                                        if (uv.isGlobal) {
                                            auto it = globalNamesToSlots.find(uv.name);
                                            if (it != globalNamesToSlots.end()) {
                                                dummy->location = &globalValues[it->second];
                                            } else {
                                                if (globalValues.size() >= 65535) throw std::runtime_error("VM Error: Too many global variables.");
                                                globalNamesToSlots[uv.name] = static_cast<uint16_t>(globalValues.size());
                                                globalValues.push_back(Value::uninit());
                                                dummy->location = &globalValues.back();
                                            }
                                        } else {
                                            dummy->closed = Value::none();
                                            dummy->location = &dummy->closed;
                                        }
                                        closure->upvalues[i] = dummy;
                                    }
                                }
                            } else {
                                // ★ 默认按值捕获 (立即 Closed Upvalue)
                                auto dummy = GcHeap::get().allocate<ObjUpVal>();
                                if (uv.isGlobal) {
                                    if (!uv.isExplicitState) {
                                        auto it = globalNamesToSlots.find(uv.name);
                                        if (it != globalNamesToSlots.end()) {
                                            dummy->closed = globalValues[it->second];
                                        } else {
                                            Value builtinVal = getBuiltinClosure(uv.name);
                                            if (!builtinVal.isNone()) {
                                                dummy->closed = builtinVal;
                                            } else {
                                                throw std::runtime_error("Runtime Error: Undefined variable '" + uv.name + "'.");
                                            }
                                        }
                                    } else {
                                        dummy->closed = Value::uninit();
                                    }
                                } else if (uv.isLocal) {
                                    if (uv.isRefParam) {
                                        dummy->closed = *(static_cast<ObjUpVal*>(stack[currentFrame->refParamsBase + uv.index].asObj())->location);
                                    } else {
                                        int captureIdx = currentFrame->stackBase + uv.index;
                                        dummy->closed = stack[captureIdx];
                                    }
                                } else {
                                    if (currentFrame->closure && uv.index < currentFrame->closure->upvalueCount) {
                                        dummy->closed = *(currentFrame->closure->upvalues[uv.index]->location);
                                        if (!uv.isExplicitState && dummy->closed.isUninit()) {
                                            throw std::runtime_error("Runtime Error: Undefined variable '" + uv.name + "'.");
                                        }
                                    }
                                    else
                                        dummy->closed = Value::none();
                                }
                                dummy->location = &dummy->closed;
                                closure->upvalues[i] = dummy;
                            }
                        }
                    }

                    // ★ （为了保证从外界通过 C++ 获取到这个闭包也能强制执行，我们依然做一层薄薄的回调包装）
                    int capturedFnIdx = idx;
                    VM* vm = this;

                    Value currentSelf = currentFrame->selfContext;
                    Value currentClass = currentFrame->classContext;
                    closure->nativeFn = std::make_any<NativeCallable>(
                        [vm, capturedFnIdx, closure, currentSelf, currentClass](const std::vector<Value>& args) -> Value {
                            // ★ 智能窃取：如果有 Dunder 方法等触发的原生调用，优先使用隔离栈里的运行态 Target
                            Value s = !helpers::nativeSelfStack.empty() ? helpers::nativeSelfStack.back() : currentSelf;
                            Value c = !helpers::nativeClassStack.empty() ? helpers::nativeClassStack.back() : currentClass;
                            return vm->callVMFunction(capturedFnIdx, args, closure, s, c);
                        }
                    );

                    for (int j = 0; j < fn->maxArity; ++j) {
                        closure->paramNames.push_back("_" + std::to_string(j));
                        closure->isRef.push_back(false);
                    }
                    closure->defaultValues.resize(fn->maxArity, Value::none());
                    closure->paramNames.clear();
                    closure->isRef.clear();
                    closure->defaultValues.clear();
                    // ========================================================
                    // ★ 修复：在给闭包构建占位数据时，严格划清“必填”、“默认值”和“变长”的界限
                    // ========================================================

                    // 1. 先把基础坑位全部挖好
                    for (int j = 0; j < fn->maxArity; ++j) {
                        closure->paramNames.push_back("_" + std::to_string(j));
                        closure->isRef.push_back(false);
                    }
                    closure->defaultValues.resize(fn->maxArity, Value::none());
                    closure->paramNames.clear();
                    closure->isRef.clear();
                    closure->defaultValues.clear();

                    // 2. 灌入必填参数
                    for (int j = 0; j < fn->arity; ++j) {
                        closure->paramNames.push_back("_" + std::to_string(j));
                        closure->isRef.push_back(false);
                    }

                    // 3. 灌入真正的带默认值的参数（如果是变长，那最后一项就不是默认参数！）
                    int defaultLimit = fn->hasRestParam ? (fn->maxArity - 1) : fn->maxArity;
                    for (int j = fn->arity; j < defaultLimit; ++j) {
                        closure->paramNames.push_back("_" + std::to_string(j));
                        closure->isRef.push_back(false);
                        closure->defaultValues.push_back(Value::none()); // 真正的默认值占位
                    }

                    // 4. 灌入变长参数标识
                    if (fn->hasRestParam) {
                        closure->paramNames.push_back("...rest");
                        closure->isRef.push_back(false);
                        // 变长参数自身不需要压入 defaultValues 中！这保证了 C++ 反射获取的干净度。
                    }

                    // ★ 必须保留这个标志供 C++ 层 API 重用识别
                    closure->hasRestParam = fn->hasRestParam;
                    closure->boundSelf = currentFrame->selfContext;
                    closure->boundClass = currentFrame->classContext;
                    push(Value(closure));
                    break;
                }

                case OpCode::OP_CALL: {
                    uint8_t argc = readByte();
                    execCall(argc);
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_TAIL_CALL: {
                    uint8_t argc = readByte();
                    int prevIp = currentFrame->ip;
                    execCall(argc, true);
                    if (currentFrame->ip == prevIp) {
                        bool shouldExit = false;
                        Value result = execReturn(shouldExit);
                        if (shouldExit) return result;
                    }
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_GET_UPVALUE: {
                    uint16_t idx = readShort();
                    if (!currentFrame->closure || idx >= currentFrame->closure->upvalueCount)
                        throw std::runtime_error("VM Error: Invalid upvalue index " +
                            std::to_string(idx) + ".");
                    push(*(currentFrame->closure->upvalues[idx]->location));
                    break;
                }

                case OpCode::OP_SET_UPVALUE: {
                    uint16_t idx = readShort();
                    if (!currentFrame->closure || idx >= currentFrame->closure->upvalueCount)
                        throw std::runtime_error("VM Error: Invalid upvalue index " +
                            std::to_string(idx) + ".");
                    *(currentFrame->closure->upvalues[idx]->location) = peek(0);
                    break;
                }

                case OpCode::OP_SUPER_INVOKE: {
                    uint16_t nameIdx = readShort();
                    uint8_t argc = readByte();
                    execSuperInvoke(nameIdx, argc);
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_TAIL_SUPER_INVOKE: {
                    uint16_t nameIdx = readShort();
                    uint8_t argc = readByte();
                    int prevIp = currentFrame->ip;
                    execSuperInvoke(nameIdx, argc, true);
                    if (currentFrame->ip == prevIp) {
                        bool shouldExit = false;
                        Value result = execReturn(shouldExit);
                        if (shouldExit) return result;
                    }
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_GET_SUPER: {
                    uint16_t nameIdx = readShort();
                    const std::string& field = chunk->constants[nameIdx].asString();

                    Value selfVal = peek(0);

                    if (!selfVal.isInstance())
                        throw std::runtime_error("VM Error: 'super' requires an instance context.");

                    auto inst = selfVal.asInstance();

                    Value classVal = currentFrame->classContext;
                    if (!classVal.isClass())
                        throw std::runtime_error("VM Error: 'super' requires class context.");

                    auto currentClass = static_cast<ObjClass*>(classVal.asObj());
                    auto parentClass = currentClass->parent;
                    if (!parentClass)
                        throw std::runtime_error("VM Error: No parent class.");

                    ObjClosure* rawMethod = nullptr;
                    ObjClass* ownerClass = nullptr;
                    auto c = parentClass;
                    while (c) {
                        auto it = c->methods.find(field);
                        if (it != c->methods.end()) {
                            rawMethod = it->second;
                            ownerClass = c;
                            break;
                        }
                        c = c->parent;
                    }
                    if (!rawMethod)
                        throw std::runtime_error("VM Error: Parent class has no method '" + field + "'.");

                    // ★ FIX: 像 OP_GET_PROPERTY 一样，打包一个携带严格上下文的绑定方法（Bound Method）！
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{},
                        field, nullptr
                    );

                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->hasRestParam = rawMethod->hasRestParam;

                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    bound->nativeFn = rawMethod->nativeFn;
                    
                    bound->boundSelf = Value(inst);
                    bound->boundClass = Value(ownerClass);

                    pop();
                    push(Value(bound));
                    break;
                }

                case OpCode::OP_PASS_REFS: {
                    uint16_t sigIdx = readShort();
                    const auto& sig = chunk->callSignatures[sigIdx];
                    pendingCallRefs.clear();
                    for (const auto& ref : sig.refs) {
                        uint8_t argIndex = ref.argIndex;
                        uint8_t sourceType = ref.sourceType;
                        uint16_t sourceRef = ref.sourceRef;

                        ObjUpVal* upval = nullptr;
                        switch (sourceType) {
                        case 1: {
                            std::string name = currentChunk().constants[sourceRef].asString();
                            upval = GcHeap::get().allocate<ObjUpVal>();
                            auto it = globalNamesToSlots.find(name);
                            if (it == globalNamesToSlots.end()) {
                                if (globalValues.size() >= 65535) throw std::runtime_error("VM Error: Too many global variables.");
                                globalNamesToSlots[name] = static_cast<uint16_t>(globalValues.size());
                                globalValues.push_back(Value::none());
                            }
                            upval->location = &globalValues[globalNamesToSlots[name]];
                            break;
                        }
                        case 2: {
                            int captureIdx = currentFrame->stackBase + sourceRef;
                            upval = captureUpvalue(&stack[captureIdx]);
                            break;
                        }
                        case 3: {
                            if (currentFrame->closure && sourceRef < currentFrame->closure->upvalueCount) {
                                upval = currentFrame->closure->upvalues[sourceRef];
                            }
                            break;
                        }
                        case 4: {
                            if (currentFrame->refParamsBase != -1) {
                                upval = static_cast<ObjUpVal*>(stack[currentFrame->refParamsBase + sourceRef].asObj());
                            }
                            break;
                        }
                        }
                        if (upval) {
                            pendingCallRefs.push_back({ argIndex, upval });
                        }
                    }
                    break;
                }

                case OpCode::OP_GET_REF_PARAM: {
                    uint16_t idx = readShort();
                    if (currentFrame->refParamsBase == -1)
                        throw std::runtime_error("VM Error: Invalid ref param index.");
                    push(*(static_cast<ObjUpVal*>(stack[currentFrame->refParamsBase + idx].asObj())->location));
                    break;
                }

                case OpCode::OP_SET_REF_PARAM: {
                    uint16_t idx = readShort();
                    if (currentFrame->refParamsBase == -1)
                        throw std::runtime_error("VM Error: Invalid ref param index.");
                    *(static_cast<ObjUpVal*>(stack[currentFrame->refParamsBase + idx].asObj())->location) = peek(0);
                    break;
                }

                case OpCode::OP_RETURN: {
                    bool shouldExit = false;
                    Value result = execReturn(shouldExit);
                    if (shouldExit) return result;
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_STRINGIFY: {
                    Value v = peek(0);
                    if (v.isString()) {
                        // do nothing, already on stack
                    }
                    else {
                        auto d = findDunder(v, DUNDER_STR);
                        if (d) {
                            Value res = callDunder(v, DUNDER_STR, {});
                            pop();
                            push(res);
                        }
                        else {
                            std::ostringstream oss;
                            if (v.isUninit()) oss << "Uninitialized";
                            else oss << v;
                            pop();
                            push(Value(oss.str()));
                        }
                    }
                    break;
                }

                case OpCode::OP_CONCAT_STRINGS: {
                    uint16_t count = readShort();
                    std::vector<std::string> parts(count);
                    size_t totalLen = 0;
                    stackTop -= count;
                    for (int i = 0; i < count; ++i) {
                        Value& v = stackTop[i];
                        if (v.isString()) {
                            parts[i] = v.asString();
                        } else {
                            std::ostringstream oss;
                            if (v.isUninit()) oss << "Uninitialized";
                            else oss << v;
                            parts[i] = oss.str();
                        }
                        totalLen += parts[i].size();
                    }
                    std::string result;
                    result.reserve(totalLen);
                    for (const auto& p : parts) result += p;
                    push(Value(result));
                    break;
                }

                case OpCode::OP_SLICE_GET: {
                    uint8_t dims = readByte();
                    execSliceGet(dims);
                    break;
                }

                case OpCode::OP_TRY_BEGIN: {
                    uint16_t catchRelOffset = readShort();
                    uint16_t catchNameIdx = readShort();
                    (void)catchNameIdx;

                    ExceptionHandler handler;
                    handler.frameIndex = frameCount - 1;
                    handler.ip = currentFrame->ip + catchRelOffset;
                    handler.stackSize = static_cast<int>(getStackSize());
                    exceptionHandlers.push_back(handler);
                    break;
                }

                case OpCode::OP_TRY_END: {
                    if (!exceptionHandlers.empty())
                        exceptionHandlers.pop_back();
                    break;
                }

                case OpCode::OP_THROW: {
                    Value errVal = pop();
                    std::string msg;
                    if (errVal.isString())
                        msg = errVal.asString();
                    else {
                        std::ostringstream oss;
                        if (errVal.isUninit()) oss << "Uninitialized";
                        else oss << errVal;
                        msg = oss.str();
                    }
                    throw ErrorSignal(msg);
                } 

                case OpCode::OP_BUILD_NAMESPACE: {
                    uint16_t nameIdx = readShort();
                    uint16_t count = readShort();
                    std::string nsName = chunk->constants[nameIdx].asString();
                    ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
                    ns->name = nsName;
                    for (int j = 0; j < count; ++j) {
                        bool isConst = pop().truthy();
                        int slot = static_cast<int>(pop().asDouble());
                        std::string key = pop().asString();
                        
                        int captureIdx = currentFrame->stackBase + slot;
                        ObjUpVal* upval = captureUpvalue(&stack[captureIdx]);
                        ns->fields[key] = { upval, isConst };
                    }
                    push(Value(ns));
                    break;
                }

                case OpCode::OP_BUILD_DICT: {
                    uint16_t count = readShort();
                    ObjDict* d = GcHeap::get().allocate<ObjDict>();
                    d->elements.reserve(count);
                    d->keyMap.reserve(count);
                    stackTop -= (count * 2);
                    for (int i = 0; i < count; ++i) {
                        d->set(std::move(stackTop[i * 2]), std::move(stackTop[i * 2 + 1]));
                    }
                    push(Value(d));
                    break;
                }

                case OpCode::OP_BUILD_SET: {
                    uint16_t count = readShort();
                    ObjSet* s = GcHeap::get().allocate<ObjSet>();
                    s->elements.reserve(count);
                    s->keys.reserve(count);
                    stackTop -= count;
                    for (int i = 0; i < count; ++i) {
                        s->add(std::move(stackTop[i]));
                    }
                    push(Value(s));
                    break;
                }

                case OpCode::OP_DICT_REST: {
                    uint16_t count = readShort();
                    std::unordered_set<std::string> excludeKeys;
                    for (int i = 0; i < count; ++i) {
                        excludeKeys.insert(pop().asString());
                    }
                    Value obj = peek(0);
                    ObjDict* restDict = GcHeap::get().allocate<ObjDict>();

                    if (obj.isObjType(ObjType::DICT)) {
                        auto d = static_cast<ObjDict*>(obj.asObj());
                        for (const auto& [k, v] : d->elements) {
                            if (k.isString() && excludeKeys.count(k.asString())) continue;
                            restDict->set(k, v);
                        }
                    } else if (obj.isInstance()) {
                        auto inst = obj.asInstance();
                        if (inst->fields) {
                            for (const auto& [k, v] : inst->fields->elements) {
                                if (k.isString() && excludeKeys.count(k.asString())) continue;
                                restDict->set(k, v);
                            }
                        }
                    } else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        for (const auto& [k, field] : ns->fields) {
                            if (excludeKeys.count(k)) continue;
                            restDict->set(Value(k), *(field.upval->location));
                        }
                    }
                    pop();
                    push(Value(restDict));
                    break;
                }

                case OpCode::OP_DUP: {
                    push(peek(0));
                    break;
                }

                case OpCode::OP_ITER_INIT: {
                    uint8_t destructFlag = readByte();
                    Value iterable = peek(0);
                    ObjList* elements = GcHeap::get().allocate<ObjList>();
                    if (iterable.isObjType(ObjType::REAL_MATRIX)) {
                        const auto& m = static_cast<ObjRealMatrix*>(iterable.asObj())->mat;
                        if (m.getRows() == 1) {
                            for (int j = 0; j < m.getCols(); ++j)
                                elements->vec.push_back(Value(m(0, j)));
                        }
                        else if (m.getCols() == 1) {
                            for (int ii = 0; ii < m.getRows(); ++ii)
                                elements->vec.push_back(Value(m(ii, 0)));
                        }
                        else {
                            for (int ii = 0; ii < m.getRows(); ++ii)
                                elements->vec.push_back(Value(m.getRow(ii)));
                        }
                    }
                    else if (iterable.isObjType(ObjType::STRING_MATRIX)) {
                        const auto& m = static_cast<ObjStringMatrix*>(iterable.asObj())->mat;
                        if (m.getRows() == 1) {
                            for (int j = 0; j < m.getCols(); ++j)
                                elements->vec.push_back(Value(m(0, j)));
                        }
                        else if (m.getCols() == 1) {
                            for (int ii = 0; ii < m.getRows(); ++ii)
                                elements->vec.push_back(Value(m(ii, 0)));
                        }
                        else {
                            for (int ii = 0; ii < m.getRows(); ++ii)
                                elements->vec.push_back(Value(m.getRow(ii)));
                        }
                    }
                    else if (iterable.isObjType(ObjType::COMPLEX_MATRIX)) {
                        const auto& m = static_cast<ObjComplexMatrix*>(iterable.asObj())->mat;
                        if (m.getRows() == 1) {
                            for (int j = 0; j < m.getCols(); ++j)
                                elements->vec.push_back(Value(m(0, j)));
                        }
                        else if (m.getCols() == 1) {
                            for (int ii = 0; ii < m.getRows(); ++ii)
                                elements->vec.push_back(Value(m(ii, 0)));
                        }
                        else {
                            for (int ii = 0; ii < m.getRows(); ++ii)
                                elements->vec.push_back(Value(m.getRow(ii)));
                        }
                    }
                    else if (iterable.isObjType(ObjType::LIST)) {
                        elements->vec = static_cast<ObjList*>(iterable.asObj())->vec;
                    }
                    else if (iterable.isString()) {
                        for (char c : iterable.asString())
                            elements->vec.push_back(Value(std::string(1, c)));
                    }
                    else if (iterable.isObjType(ObjType::DICT)) {
                        const auto* d = static_cast<ObjDict*>(iterable.asObj());
                        if (destructFlag) {
                            for (const auto& [key, val] : d->elements) {
                                ObjList* pair = GcHeap::get().allocate<ObjList>();
                                pair->vec.push_back(key);
                                pair->vec.push_back(val);
                                pair->is_frozen = true;
                                elements->vec.push_back(Value(pair));
                            }
                        }
                        else {
                            for (const auto& [key, val] : d->elements) {
                                elements->vec.push_back(key);
                            }
                        }
                    }
                    else if (iterable.isObjType(ObjType::NAMESPACE)) {
                        const auto* ns = static_cast<ObjNamespace*>(iterable.asObj());
                        if (destructFlag) {
                            for (const auto& [key, field] : ns->fields) {
                                ObjList* pair = GcHeap::get().allocate<ObjList>();
                                pair->vec.push_back(Value(key));
                                pair->vec.push_back(*(field.upval->location));
                                pair->is_frozen = true;
                                elements->vec.push_back(Value(pair));
                            }
                        }
                        else {
                            for (const auto& [key, field] : ns->fields) {
                                elements->vec.push_back(Value(key));
                            }
                        }
                    }
                    else if (iterable.isObjType(ObjType::SET)) {
                        const auto* s = static_cast<ObjSet*>(iterable.asObj());
                        for (const auto& val : s->elements) {
                            elements->vec.push_back(val);
                        }
                    }
                    else if (iterable.isInstance()) {
                        auto method = findDunder(iterable, DUNDER_ITER);
                        if (method) {
                            Value iterObj = callDunder(iterable, DUNDER_ITER, {});
                            pop();
                            push(iterObj);
                            push(Value::none()); // 使用 none 作为自定义迭代器的索引标记
                            break;
                        }
                        auto inst = iterable.asInstance();
                        if (inst->fields) {
                            if (destructFlag) {
                                for (const auto& [key, val] : inst->fields->elements) {
                                    ObjList* pair = GcHeap::get().allocate<ObjList>();
                                    pair->vec.push_back(key);
                                    pair->vec.push_back(val);
                                    pair->is_frozen = true;
                                    elements->vec.push_back(Value(pair));
                                }
                            }
                            else {
                                for (const auto& [key, val] : inst->fields->elements) {
                                    elements->vec.push_back(key);
                                }
                            }
                        }
                    }
                    else {
                        throw std::runtime_error("VM Error: Cannot iterate over this type.");
                    }
                    pop();
                    push(Value(elements));
                    push(Value(0.0));
                    break;
                }

                case OpCode::OP_ITER_NEXT: {
                    uint16_t offset = readShort();
                    Value idxVal = peek(0);
                    
                    if (idxVal.isNone()) {
                        // 自定义迭代器分支
                        Value iterObj = peek(1);
                        auto method = findDunder(iterObj, DUNDER_NEXT);
                        if (!method) throw std::runtime_error("VM Error: Iterator missing __next__ method.");
                        
                        Value nextVal = callDunder(iterObj, DUNDER_NEXT, {});
                        if (nextVal.isNone()) {
                            currentFrame->ip += offset; // 迭代结束
                        } else {
                            push(nextVal);
                        }
                    } else {
                        // 原生 List 迭代分支
                        double idx = idxVal.asDouble();
                        const auto& elems = static_cast<ObjList*>(peek(1).asObj())->vec;
                        int i = static_cast<int>(idx);
                        if (i >= static_cast<int>(elems.size())) {
                            currentFrame->ip += offset;
                        }
                        else {
                            Value elem = elems[i];
                            stack[getStackSize() - 1] = Value(idx + 1);
                            push(elem);
                        }
                    }
                    break;
                }

                case OpCode::OP_BUILD_LIST: {
                    uint16_t count = readShort();
                    ObjList* list = GcHeap::get().allocate<ObjList>();
                    list->vec.resize(count);
                    stackTop -= count;
                    for (int i = 0; i < count; ++i) {
                        list->vec[i] = std::move(stackTop[i]);
                    }
                    push(Value(list));
                    break;
                }

                case OpCode::OP_BUILD_MATRIX: {
                    uint16_t shapeIdx = readShort();
                    execBuildMatrix(shapeIdx);
                    break;
                }

                case OpCode::OP_IN: {
                    execIn();
                    break;
                }

                case OpCode::OP_SLICE_SET: {
                    uint8_t dims = readByte();
                    execSliceSet(dims);
                    break;
                }

                case OpCode::OP_INDEX_GET: {
                    uint8_t dims = readByte();
                    execIndexGet(dims);
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_INDEX_SET: {
                    uint8_t dims = readByte();
                    execIndexSet(dims);
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_FORMAT_STRING: {
                    uint16_t specIdx = readShort();
                    const std::string& spec = chunk->constants[specIdx].asString();
                    Value val = pop();

                    char align = '\0';
                    int width = 0;
                    int precision = -1;
                    char type = '\0';
                    size_t si = 0;
                    if (si < spec.size() && (spec[si] == '<' || spec[si] == '>' || spec[si] == '^'))
                        align = spec[si++];
                    while (si < spec.size() && spec[si] >= '0' && spec[si] <= '9')
                        width = width * 10 + (spec[si++] - '0');
                    if (si < spec.size() && spec[si] == '.') {
                        si++; precision = 0;
                        while (si < spec.size() && spec[si] >= '0' && spec[si] <= '9')
                            precision = precision * 10 + (spec[si++] - '0');
                    }
                    if (si < spec.size()) type = spec[si++];

                    std::ostringstream oss;
                    if (type == 'f' || type == 'e') {
                        if (precision >= 0) oss << std::fixed << std::setprecision(precision);
                        if (type == 'e') oss << std::scientific;
                        oss << val.asDouble();
                    }
                    else if (type == 'd') { oss << static_cast<int64_t>(std::round(val.asDouble())); }
                    else if (type == 'x') { oss << std::hex << static_cast<int64_t>(std::round(val.asDouble())); }
                    else { oss << val; }

                    std::string result = oss.str();
                    if (width > 0 && static_cast<int>(result.size()) < width) {
                        int pad = width - static_cast<int>(result.size());
                        if (align == '<') result += std::string(pad, ' ');
                        else if (align == '^') {
                            int l = pad / 2, r = pad - l;
                            result = std::string(l, ' ') + result + std::string(r, ' ');
                        }
                        else result = std::string(pad, ' ') + result;
                    }
                    push(Value(result));
                    break;
                }

                case OpCode::OP_LIST_INIT: {
                    push(Value(GcHeap::get().allocate<ObjList>()));
                    break;
                }

                case OpCode::OP_LIST_APPEND: {
                    uint16_t depth = readShort();
                    Value elem = pop();
                    int listIdx = static_cast<int>(getStackSize()) - 1 - static_cast<int>(depth);
                    if (listIdx >= 0 && stack[listIdx].isObjType(ObjType::LIST)) {
                        static_cast<ObjList*>(stack[listIdx].asObj())->mut().push_back(elem);
                    }
                    else {
                        throw std::runtime_error("VM Error: LIST_APPEND target not found at depth " +
                            std::to_string(depth));
                    }
                    break;
                }

                case OpCode::OP_LIST_COMP_END: {
                    Value arg = peek(0);
                    if (!arg.isObjType(ObjType::LIST)) {
                        break;
                    }
                    auto l = static_cast<ObjList*>(arg.asObj());
                    if (l->vec.empty()) {
                        pop();
                        push(Value(RealMatrix(1, 0)));
                        break;
                    }
                    
                    bool hasComplex = false;
                    bool hasString = false;
                    bool hasOther = false;
                    bool hasSubMatrix = false;

                    auto canBeMatrixElement = [](const Value& v) -> bool {
                        return v.isNumber() ||
                            v.isObjType(ObjType::BIGINT) ||
                            v.isObjType(ObjType::FRACTION) ||
                            v.isObjType(ObjType::BASENUM) ||
                            v.isObjType(ObjType::COMPLEX) ||
                            v.isString() ||
                            v.isObjType(ObjType::REAL_MATRIX) ||
                            v.isObjType(ObjType::COMPLEX_MATRIX) ||
                            v.isObjType(ObjType::STRING_MATRIX);
                    };

                    for (const auto& v : l->vec) {
                        if (v.isObjType(ObjType::COMPLEX) || v.isObjType(ObjType::COMPLEX_MATRIX)) hasComplex = true;
                        if (v.isString() || v.isObjType(ObjType::STRING_MATRIX)) hasString = true;
                        if (v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || v.isObjType(ObjType::STRING_MATRIX)) hasSubMatrix = true;
                        if (!canBeMatrixElement(v)) hasOther = true;
                    }

                    if (hasOther) {
                        break;
                    }

                    int total = static_cast<int>(l->vec.size());

                    if (hasSubMatrix) {
                        auto extractCell = [&](Value& cell) {
                            if (!cell.isObjType(ObjType::REAL_MATRIX) &&
                                !cell.isObjType(ObjType::COMPLEX_MATRIX) &&
                                !cell.isObjType(ObjType::STRING_MATRIX)) {
                                if (hasString) {
                                    std::ostringstream oss; oss << cell;
                                    cell = Value(StringMatrix(1, 1, { oss.str() }));
                                }
                                else if (hasComplex) {
                                    cell = Value(ComplexMatrix(1, 1, { cell.asComplex() }));
                                }
                                else {
                                    cell = Value(RealMatrix(1, 1, { cell.asDouble() }));
                                }
                            }
                            if (hasString) {
                                if (cell.isObjType(ObjType::REAL_MATRIX)) {
                                    const auto& m = static_cast<ObjRealMatrix*>(cell.asObj())->mat;
                                    std::vector<std::string> flat;
                                    for (int i = 0; i < m.getRows(); ++i)
                                        for (int j = 0; j < m.getCols(); ++j) {
                                            std::ostringstream oss; oss << Value(m(i, j));
                                            flat.push_back(oss.str());
                                        }
                                    cell = Value(StringMatrix(m.getRows(), m.getCols(), flat));
                                }
                                else if (cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                                    const auto& m = static_cast<ObjComplexMatrix*>(cell.asObj())->mat;
                                    std::vector<std::string> flat;
                                    for (int i = 0; i < m.getRows(); ++i)
                                        for (int j = 0; j < m.getCols(); ++j) {
                                            std::ostringstream oss; oss << Value(m(i, j));
                                            flat.push_back(oss.str());
                                        }
                                    cell = Value(StringMatrix(m.getRows(), m.getCols(), flat));
                                }
                            }
                            else if (hasComplex && cell.isObjType(ObjType::REAL_MATRIX)) {
                                cell = Value(cell.asComplexMatrix());
                            }
                        };

                        try {
                            Value rowResult = Value::none();
                            for (int j = 0; j < total; ++j) {
                                Value cell = l->vec[j];
                                extractCell(cell);
                                if (rowResult.isNone()) {
                                    rowResult = cell;
                                }
                                else {
                                    if (hasString)
                                        rowResult = Value(static_cast<ObjStringMatrix*>(rowResult.asObj())->mat
                                            .integR(static_cast<ObjStringMatrix*>(cell.asObj())->mat));
                                    else if (hasComplex)
                                        rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat
                                            .integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                                    else
                                        rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat
                                            .integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                                }
                            }
                            pop();
                            push(rowResult);
                        }
                        catch (...) {
                            throw std::runtime_error("VM Error: Dimension mismatch during list comprehension matrix concatenation.");
                        }
                    }
                    else if (hasString) {
                        std::vector<std::string> flat(total);
                        for (int ii = 0; ii < total; ++ii) {
                            const Value& v = l->vec[ii];
                            if (v.isString()) flat[ii] = v.asString();
                            else {
                                std::ostringstream oss;
                                if (v.isUninit()) oss << "Uninitialized";
                                else oss << v;
                                flat[ii] = oss.str();
                            }
                        }
                        pop();
                        push(Value(StringMatrix(1, total, flat)));
                    }
                    else if (hasComplex) {
                        std::vector<Complex> flat(total);
                        for (int ii = 0; ii < total; ++ii) flat[ii] = l->vec[ii].asComplex();
                        pop();
                        push(Value(ComplexMatrix(1, total, flat)));
                    }
                    else {
                        std::vector<double> flat(total);
                        for (int ii = 0; ii < total; ++ii) flat[ii] = l->vec[ii].asDouble();
                        pop();
                        push(Value(RealMatrix(1, total, flat)));
                    }
                    break;
                }

                case OpCode::OP_IMPORT: {
                    Value pathVal = pop();
                    if (!pathVal.isString())
                        throw std::runtime_error("VM Error: import requires a string path.");
                    std::string name = pathVal.asString();
                    std::string baseName = std::filesystem::path(name).stem().string();

                    if (loadedModules.count(name)) {
                        push(loadedModules[name]);
                        break;
                    }

                    ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
                    ns->name = name;

                    auto& modules = getNativeModules();
                    auto it = modules.find(name);
                    if (it != modules.end()) {
                        // ★ 原生模块：使用临时字典，完全隔离，不污染全局！
                        std::unordered_map<std::string, Value> tempGlobals;
                        std::unordered_map<std::string, NativeCallable> tempNatives;
                        std::unordered_map<std::string, std::set<int>> tempArity;

                        it->second.loader(tempGlobals, tempNatives, tempArity);
                        importedModules.insert(name);

                        for (const auto& kv : tempGlobals) {
                            auto uv = GcHeap::get().allocate<ObjUpVal>();
                            uv->closed = kv.second;
                            uv->location = &uv->closed;
                            ns->fields[kv.first] = { uv, true };
                        }

                        for (const auto& kv : tempNatives) {
                            auto closure = GcHeap::get().allocate<ObjClosure>(
                                std::vector<std::string>{}, std::vector<bool>{}, kv.first, nullptr
                            );
                            closure->nativeFn = std::make_any<NativeCallable>(kv.second);
                            
                            auto ait = tempArity.find(kv.first);
                            if (ait != tempArity.end() && !ait->second.empty()) {
                                int maxA = *ait->second.rbegin();
                                int minA = *ait->second.begin();
                                for (int j = 0; j < maxA; ++j) {
                                    closure->paramNames.push_back("_" + std::to_string(j));
                                    closure->isRef.push_back(false);
                                }
                                for (int j = minA; j < maxA; ++j) {
                                    closure->defaultValues.push_back(Value::none());
                                }
                            }

                            auto uv = GcHeap::get().allocate<ObjUpVal>();
                            uv->closed = Value(closure);
                            uv->location = &uv->closed;
                            ns->fields[kv.first] = { uv, true };
                        }
                    } else {
                        // ★ 脚本模块：使用真正的 Namespace 编译机制，彻底告别老掉牙的 diff！
                        std::string resolved = helpers::safeResolvePath(name);
                        if (!std::filesystem::exists(resolved)) {
                            resolved = helpers::safeResolvePath(name + ".jc2");
                        }

                        if (!std::filesystem::exists(resolved)) {
                            throw std::runtime_error("VM Error: Cannot find module '" + name + "'.");
                        }

                        importedModules.insert(name);

                        std::ifstream file(resolved);
                        if (!file.is_open()) throw std::runtime_error("IO Error: Cannot read module script.");
                        std::string code, line;
                        while (std::getline(file, line)) code += line + "\n";
                        file.close();

                        jc::Lexer lexer(code, resolved);
                        auto tokens = lexer.tokenize();
                        jc::Parser parser(tokens);
                        auto ast = parser.parse();

                        jc::Compiler compiler;
                        compiler.setCompiledFunctions(compiledFunctions);
                        compiler.setFunctionIndexOffset(0);

                        Chunk modChunk = compiler.compileModule(ast.get(), resolved, baseName);

                        auto modFn = std::make_shared<CompiledFunction>();
                        modFn->name = "<module " + baseName + ">";
                        modFn->sourceFile = resolved;
                        modFn->chunk = std::move(modChunk);
                        modFn->arity = 0;
                        modFn->maxArity = 0;
                        modFn->localCount = compiler.getTopLevelLocalCount();

                        auto fns = compiler.getCompiledFunctions();
                        fns.push_back(modFn);
                        int modFnIdx = static_cast<int>(fns.size()) - 1;
                        compiledFunctions = fns;

                        std::string scriptDir = std::filesystem::path(resolved).parent_path().string();
                        helpers::g_scriptDirStack.push_back(scriptDir);
                        Value nsVal;
                        try {
                            nsVal = callVMFunction(modFnIdx, {});
                        } catch (...) {
                            helpers::g_scriptDirStack.pop_back();
                            throw;
                        }
                        helpers::g_scriptDirStack.pop_back();

                        if (!nsVal.isObjType(ObjType::NAMESPACE)) {
                            throw std::runtime_error("VM Error: Module script must not use top-level 'return'.");
                        }
                        ns = static_cast<ObjNamespace*>(nsVal.asObj());
                    }

                    loadedModules[name] = Value(ns);
                    push(Value(ns));
                    break;
                }

                case OpCode::OP_CLASS: {
                    uint16_t nameIdx = readShort();
                    const std::string& name = chunk->constants[nameIdx].asString();
                    auto cls = GcHeap::get().allocate<ObjClass>();
                    cls->name = name;
                    push(Value(cls));
                    break;
                }

                case OpCode::OP_METHOD: {
                    uint16_t nameIdx = readShort();
                    const std::string& methodName = chunk->constants[nameIdx].asString();
                    Value closureVal = peek(0);
                    Value& classVal = peek(1);

                    if (!classVal.isClass())
                        throw std::runtime_error("VM Error: OP_METHOD requires a class on stack.");

                    auto cls = static_cast<ObjClass*>(classVal.asObj());

                    if (closureVal.isFunctionClosure()) {
                        auto fc = closureVal.asFunction();
                        cls->methods[methodName] = fc;
                        pop();
                        break;
                    }

                    if (closureVal.isString()) {
                        const std::string& tag = closureVal.asString();
                        auto fc = GcHeap::get().allocate<ObjClosure>(
                            std::vector<std::string>{},
                            std::vector<bool>{},
                            methodName,
                            nullptr
                        );
                        fc->nativeFn = std::make_any<std::string>(tag);
                        cls->methods[methodName] = fc;
                        pop();
                        break;
                    }

                    throw std::runtime_error("VM Error: Invalid closure type for method '" +
                        methodName + "'.");
                }

                case OpCode::OP_INHERIT: {
                    Value superClass = pop();
                    Value& subClass = peek(0);
                    if (!superClass.isClass() || !subClass.isClass())
                        throw std::runtime_error("VM Error: Inheritance requires two classes.");
                    auto sub = static_cast<ObjClass*>(subClass.asObj());
                    auto sup = static_cast<ObjClass*>(superClass.asObj());
                    sub->parent = sup;
                    for (auto& [name, method] : sup->methods) {
                        if (sub->methods.find(name) == sub->methods.end())
                            sub->methods[name] = method;
                    }
                    pop();
                    break;
                }

                case OpCode::OP_GET_PROPERTY: {
                    uint16_t icIdx = readShort();
                    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
                    uint16_t nameIdx = ic.nameIdx;
                    const std::string& field = chunk->constants[nameIdx].asString();
                    Value obj = peek(0);
                    bool found = false;
                    Value result;

                    if (obj.isInstance()) {
                        auto inst = obj.asInstance();

                        // ★ IC 命中检查
                        if (ic.cachedClass == inst->classDef) {
                            if (ic.cachedFieldIndex != -1 && inst->fields && ic.cachedFieldIndex < static_cast<int>(inst->fields->elements.size())) {
                                if (inst->fields->elements[ic.cachedFieldIndex].first.asString() == field) {
                                    result = inst->fields->elements[ic.cachedFieldIndex].second;
                                    found = true;
                                }
                            } else if (ic.cachedMethod) {
                                if (!inst->fields || inst->fields->keyMap.find(chunk->constants[nameIdx]) == inst->fields->keyMap.end()) {
                                    auto bound = GcHeap::get().allocate<ObjClosure>(
                                        std::vector<std::string>{}, std::vector<bool>{},
                                        field, nullptr
                                    );
                                    bound->paramNames = ic.cachedMethod->paramNames;
                                    bound->isRef = ic.cachedMethod->isRef;
                                    bound->defaultValues = ic.cachedMethod->defaultValues;
                                    bound->hasRestParam = ic.cachedMethod->hasRestParam;
                                    
                                    bound->compiledFnIndex = ic.cachedMethod->compiledFnIndex;
                                    if (ic.cachedMethod->upvalueCount > 0) {
                                        bound->upvalueCount = ic.cachedMethod->upvalueCount;
                                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                        for (int i = 0; i < bound->upvalueCount; ++i) {
                                            bound->upvalues[i] = ic.cachedMethod->upvalues[i];
                                        }
                                    }
                                    bound->nativeFn = ic.cachedMethod->nativeFn;
                                    
                                    bound->boundSelf = Value(inst);
                                    bound->boundClass = Value(ic.cachedClass);

                                    result = Value(bound);
                                    found = true;
                                }
                            }
                        }

                        if (!found) {
                            // 1. 字段查找
                            if (inst->fields) {
                                auto it = inst->fields->keyMap.find(chunk->constants[nameIdx]);
                                if (it != inst->fields->keyMap.end()) {
                                    result = inst->fields->elements[it->second].second;
                                    found = true;
                                    ic.cachedClass = inst->classDef;
                                    ic.cachedFieldIndex = static_cast<int>(it->second);
                                    ic.cachedMethod = nullptr;
                                }
                            }

                            // 2. 方法查找
                            if (!found) {
                                ObjClosure* rawMethod = nullptr;
                                ObjClass* ownerClass = nullptr;
                                auto c = inst->classDef;
                                while (c) {
                                    auto it = c->methods.find(field);
                                    if (it != c->methods.end()) {
                                        rawMethod = it->second;
                                        ownerClass = c;
                                        break;
                                    }
                                    c = c->parent;
                                }
                                if (rawMethod) {
                                    auto bound = GcHeap::get().allocate<ObjClosure>(
                                        std::vector<std::string>{}, std::vector<bool>{},
                                        field, nullptr
                                    );
                                    bound->paramNames = rawMethod->paramNames;
                                    bound->isRef = rawMethod->isRef;
                                    bound->defaultValues = rawMethod->defaultValues;
                                    bound->hasRestParam = rawMethod->hasRestParam;
                                    
                                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                    if (rawMethod->upvalueCount > 0) {
                                        bound->upvalueCount = rawMethod->upvalueCount;
                                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                        for (int i = 0; i < bound->upvalueCount; ++i) {
                                            bound->upvalues[i] = rawMethod->upvalues[i];
                                        }
                                    }
                                    bound->nativeFn = rawMethod->nativeFn;
                                    
                                    bound->boundSelf = Value(inst);
                                    bound->boundClass = Value(ownerClass);

                                    result = Value(bound);
                                    found = true;
                                    
                                    ic.cachedClass = inst->classDef;
                                    ic.cachedMethod = rawMethod;
                                    ic.cachedFieldIndex = -1;
                                } else {
                                    auto getattrMethod = findDunder(obj, DUNDER_GETATTR);
                                    if (getattrMethod) {
                                        result = callDunder(obj, DUNDER_GETATTR, { Value(field) });
                                        found = true;
                                    }
                                }
                            }
                        }
                    }
                    else if (obj.isObjType(ObjType::DICT)) {
                        auto d = static_cast<ObjDict*>(obj.asObj());
                        auto it = d->keyMap.find(chunk->constants[nameIdx]);
                        if (it != d->keyMap.end()) {
                            result = d->elements[it->second].second;
                            found = true;
                        }
                    }
                    else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        auto it = ns->fields.find(field);
                        if (it != ns->fields.end()) {
                            result = *(it->second.upval->location);
                            found = true;
                        }
                    }

                    if (!found) {
                        // ★ UFCS Fallback: 允许内置类型像对象一样调用全局函数
                        auto nIt = nativeBuiltins.find(field);
                        if (nIt != nativeBuiltins.end()) {
                            auto bound = GcHeap::get().allocate<ObjClosure>(
                                std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                            );
                            bound->boundSelf = obj;
                            NativeCallable nativeFn = nIt->second;
                            
                            auto ait = builtinArity.find(field);
                            std::set<int> allowedArities;
                            if (ait != builtinArity.end()) allowedArities = ait->second;

                            bound->nativeFn = std::make_any<NativeCallable>(
                                [nativeFn, allowedArities, field](const std::vector<Value>& args) -> Value {
                                    Value capturedObj = helpers::nativeSelfStack.back();
                                    int totalArgs = static_cast<int>(args.size()) + 1;
                                    if (!allowedArities.empty() && allowedArities.find(totalArgs) == allowedArities.end()) {
                                        std::string expected;
                                        for (auto aIt = allowedArities.begin(); aIt != allowedArities.end(); ++aIt) {
                                            if (aIt != allowedArities.begin()) expected += " or ";
                                            expected += std::to_string(*aIt - 1);
                                        }
                                        throw std::runtime_error("Runtime Error: Method '" + field + "' expects " + expected + " arguments, got " + std::to_string(args.size()) + ".");
                                    }
                                    std::vector<Value> fullArgs;
                                    fullArgs.reserve(totalArgs);
                                    fullArgs.push_back(capturedObj);
                                    fullArgs.insert(fullArgs.end(), args.begin(), args.end());
                                    return nativeFn(fullArgs);
                                }
                            );
                            result = Value(bound);
                            found = true;
                        } else {
                            auto gIt = globalNamesToSlots.find(field);
                            if (gIt != globalNamesToSlots.end() && globalValues[gIt->second].isFunctionClosure()) {
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->boundSelf = obj;
                                ObjClosure* targetFn = globalValues[gIt->second].asFunction();
                            
                                if (targetFn->isBytecode()) {
                                    bound->compiledFnIndex = targetFn->compiledFnIndex;
                                    if (targetFn->upvalueCount > 0) {
                                        bound->upvalueCount = targetFn->upvalueCount;
                                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                        for (int i = 0; i < bound->upvalueCount; ++i) {
                                            bound->upvalues[i] = targetFn->upvalues[i];
                                        }
                                    }
                                    bound->hasRestParam = targetFn->hasRestParam;
                                    bound->paramNames = targetFn->paramNames;
                                    bound->isRef = targetFn->isRef;
                                    bound->defaultValues = targetFn->defaultValues;
                                    bound->isUFCS = true; // ★ 标记为 UFCS 绑定，让 execCall 自动插入 boundSelf
                                } else {
                                    bound->nativeFn = std::make_any<NativeCallable>(
                                        [](const std::vector<Value>& args) -> Value {
                                            Value capturedObj = helpers::nativeSelfStack.back();
                                            ObjClosure* fn = helpers::nativeClassStack.back().asFunction();
                                            std::vector<Value> fullArgs;
                                            fullArgs.reserve(args.size() + 1);
                                            fullArgs.push_back(capturedObj);
                                            fullArgs.insert(fullArgs.end(), args.begin(), args.end());
                                            return helpers::safeCallFunction(fn, fullArgs);
                                        }
                                    );
                                    bound->boundClass = Value(targetFn); // 借用 boundClass 传递 targetFn 以防被 GC 回收
                                }
                                result = Value(bound);
                                found = true;
                            }
                        }
                    }

                    if (!found) {
                        if (obj.isInstance()) throw std::runtime_error("VM Error: No field/method '" + field + "'.");
                        if (obj.isObjType(ObjType::DICT)) throw std::runtime_error("VM Error: Key '" + field + "' not found.");
                        if (obj.isObjType(ObjType::NAMESPACE)) throw std::runtime_error("VM Error: Field '" + field + "' not found in namespace.");
                        throw std::runtime_error("VM Error: Cannot access property '" + field + "' on this type.");
                    }
                    pop();
                    push(result);
                    break;
                }

                case OpCode::OP_TRY_GET_PROPERTY: {
                    uint16_t icIdx = readShort();
                    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
                    uint16_t nameIdx = ic.nameIdx;
                    const std::string& field = chunk->constants[nameIdx].asString();
                    Value obj = peek(0);
                    bool found = false;
                    Value result;

                    if (obj.isInstance()) {
                        auto inst = obj.asInstance();
                        
                        // ★ IC 命中检查
                        if (ic.cachedClass == inst->classDef && ic.cachedFieldIndex != -1 && inst->fields && ic.cachedFieldIndex < static_cast<int>(inst->fields->elements.size())) {
                            if (inst->fields->elements[ic.cachedFieldIndex].first.asString() == field) {
                                result = inst->fields->elements[ic.cachedFieldIndex].second;
                                found = true;
                            }
                        }

                        if (!found) {
                            if (inst->fields) {
                                auto it = inst->fields->keyMap.find(chunk->constants[nameIdx]);
                                if (it != inst->fields->keyMap.end()) {
                                    result = inst->fields->elements[it->second].second;
                                    found = true;
                                    ic.cachedClass = inst->classDef;
                                    ic.cachedFieldIndex = static_cast<int>(it->second);
                                }
                            }
                            if (!found) {
                                auto getattrMethod = findDunder(obj, DUNDER_GETATTR);
                                if (getattrMethod) {
                                    try {
                                        result = callDunder(obj, DUNDER_GETATTR, { Value(field) });
                                        found = true;
                                    } catch (...) {
                                        found = false;
                                    }
                                }
                            }
                        }
                    }
                    else if (obj.isObjType(ObjType::DICT)) {
                        auto d = static_cast<ObjDict*>(obj.asObj());
                        auto it = d->keyMap.find(chunk->constants[nameIdx]);
                        if (it != d->keyMap.end()) {
                            result = d->elements[it->second].second;
                            found = true;
                        }
                    }
                    else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        auto it = ns->fields.find(field);
                        if (it != ns->fields.end()) {
                            result = *(it->second.upval->location);
                            found = true;
                        }
                    }

                    pop();
                    if (found) {
                        push(result);
                        push(Value(true));
                    } else {
                        push(Value::none());
                        push(Value(false));
                    }
                    break;
                }

                case OpCode::OP_SET_PROPERTY: {
                    uint16_t icIdx = readShort();
                    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
                    uint16_t nameIdx = ic.nameIdx;
                    const std::string& field = chunk->constants[nameIdx].asString();
                    Value val = peek(0);
                    Value obj = peek(1);

                    if (obj.isInstance()) {
                        auto inst = obj.asInstance();
                        inst->checkModify();
                        auto setattrMethod = findDunder(obj, DUNDER_SETATTR);
                        if (setattrMethod) {
                            callDunder(obj, DUNDER_SETATTR, { Value(field), val });
                        } else {
                            if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
                            
                            // ★ IC 命中检查
                            if (ic.cachedClass == inst->classDef && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(inst->fields->elements.size())) {
                                if (inst->fields->elements[ic.cachedFieldIndex].first.asString() == field) {
                                    inst->fields->elements[ic.cachedFieldIndex].second = val;
                                    pop(); pop();
                                    push(val);
                                    break;
                                }
                            }

                            Value key = chunk->constants[nameIdx];
                            auto it = inst->fields->keyMap.find(key);
                            if (it != inst->fields->keyMap.end()) {
                                inst->fields->elements[it->second].second = val;
                                ic.cachedClass = inst->classDef;
                                ic.cachedFieldIndex = static_cast<int>(it->second);
                            } else {
                                ic.cachedClass = inst->classDef;
                                ic.cachedFieldIndex = static_cast<int>(inst->fields->elements.size());
                                inst->fields->keyMap[key] = inst->fields->elements.size();
                                inst->fields->elements.push_back({key, val});
                            }
                        }
                    }
                    else if (obj.isObjType(ObjType::DICT)) {
                        auto d = static_cast<ObjDict*>(obj.asObj());
                        d->set(chunk->constants[nameIdx], val);
                    }
                    else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        ns->checkModify();
                        auto it = ns->fields.find(field);
                        if (it != ns->fields.end()) {
                            if (it->second.isConst) throw std::runtime_error("Runtime Error: Cannot modify const property '" + field + "' in namespace '" + ns->name + "'.");
                            *(it->second.upval->location) = val;
                        } else {
                            auto uv = GcHeap::get().allocate<ObjUpVal>();
                            uv->closed = val;
                            uv->location = &uv->closed;
                            ns->fields[field] = { uv, false };
                        }
                    }
                    else {
                        throw std::runtime_error("VM Error: Cannot set property on this type.");
                    }
                    pop(); pop();
                    push(val);
                    break;
                }

                case OpCode::OP_INVOKE: {
                    uint8_t argc = readByte();
                    uint16_t icIdx = readShort();
                    execInvoke(argc, icIdx);
                    UPDATE_FRAME();
                    break;
                }

                case OpCode::OP_TAIL_INVOKE: {
                    uint8_t argc = readByte();
                    uint16_t icIdx = readShort();
                    int prevIp = currentFrame->ip;
                    execInvoke(argc, icIdx, true);
                    if (currentFrame->ip == prevIp) {
                        bool shouldExit = false;
                        Value result = execReturn(shouldExit);
                        if (shouldExit) return result;
                    }
                    UPDATE_FRAME();
                    break;
                }

                default:
                    throw std::runtime_error("VM Error: Unknown opcode " +
                        std::to_string(static_cast<int>(op)));
                }

            }
            // =======================================================
            // ★ 异常捕获与 Traceback 调用栈回溯 (Stack Tracing)
            // =======================================================
            catch (const EngineInterruptError&) {
                throw; // 强行中断，无视 try-catch 拦截，不生成 Traceback
            }
            catch (const StackTracedException& ex) {
                std::string msg = ex.rawMessage;
                if (handleExceptionUnwind(msg)) { UPDATE_FRAME(); continue; }
                throw;
            }
            catch (const ErrorSignal& sig) {
                std::string msg = sig.message;
                if (handleExceptionUnwind(msg)) { UPDATE_FRAME(); continue; }
                throw StackTracedException(msg, buildStackTrace(msg));
            }
            catch (const std::exception& ex) {
                std::string msg = ex.what();
                if (handleExceptionUnwind(msg)) { UPDATE_FRAME(); continue; }
                throw StackTracedException(msg, buildStackTrace(msg));
            }
            catch (...) {
                std::string msg = "Unknown VM Error";
                if (handleExceptionUnwind(msg)) { UPDATE_FRAME(); continue; }
                throw StackTracedException(msg, buildStackTrace(msg));
            }
        }
        #undef UPDATE_FRAME
        #undef readByte
        #undef readShort
    }

    void VM::debugPrompt() {
        std::cout << "\n" << col(Ansi::BRIGHT_YELLOW)
            << ">>> [Debugger] Paused at Line " << currentLine()
            << " in " << frame().function->name
            << col(Ansi::RESET) << "\n";

        while (true) {
            std::cout << col(Ansi::BRIGHT_MAGENTA) << "(jc2-dbg) " << col(Ansi::RESET);
            std::string cmd;
            if (!std::getline(std::cin, cmd)) break;

            // 去除头尾空格
            size_t s = cmd.find_first_not_of(" \t");
            if (s != std::string::npos) cmd = cmd.substr(s);
            else continue;

            if (cmd == "c" || cmd == "continue") {
                break; // 恢复执行
            }
            else if (cmd == "s" || cmd == "step") {
                stepNextLine = true;
                break; // 走一步（即步入下一个不同的行号）
            }
            else if (cmd == "stack") {
                std::cout << "--- VM Stack (" << getStackSize() << " elements) ---\n";
                // 打印栈内容（即局部变量与中间计算状态）
                for (size_t i = 0; i < getStackSize(); i++) {
                    std::cout << " [" << i << "]  " << stack[i];
                    if (static_cast<int>(i) == frame().stackBase) std::cout << "  <-- Frame Base";
                    std::cout << "\n";
                }
                std::cout << "-----------------------------------\n";
            }
            else if (cmd.substr(0, 2) == "p ") {
                std::string varName = cmd.substr(2);
                size_t vs = varName.find_first_not_of(" \t");
                if (vs != std::string::npos) varName = varName.substr(vs);
                // 探查全局变量
                auto it = globalNamesToSlots.find(varName);
                if (it != globalNamesToSlots.end()) {
                    std::cout << varName << " = " << globalValues[it->second] << "\n";
                }
                else {
                    std::cout << "Variable '" << varName << "' not found in global scope.\n";
                }
            }
            else if (cmd.substr(0, 2) == "b ") {
                int l = std::stoi(cmd.substr(2));
                breakpoints.insert(l);
                std::cout << "Breakpoint set at Line " << l << "\n";
            }
            else if (cmd.substr(0, 3) == "rb ") {
                int l = std::stoi(cmd.substr(3));
                breakpoints.erase(l);
                std::cout << "Breakpoint removed at Line " << l << "\n";
            }
            else if (cmd == "q" || cmd == "quit") {
                throw std::runtime_error("Execution aborted by debugger.");
            }
            else {
                std::cout << "Available Commands:\n"
                    << "  c / continue  : Resume execution until next breakpoint\n"
                    << "  s / step      : Step to the next line of code\n"
                    << "  b <line>      : Set breakpoint at line\n"
                    << "  rb <line>     : Remove breakpoint at line\n"
                    << "  p <global>    : Print a global variable's value\n"
                    << "  stack         : View raw VM memory stack (inspect auto-locals)\n"
                    << "  q / quit      : Abort program\n";
            }
        }
    }

    void VM::printProfileReport() {
        std::cout << "\n" << col(Ansi::BRIGHT_CYAN)
            << "==================================================\n"
            << "               JC2 PROFILER REPORT                \n"
            << "=================================================="
            << col(Ansi::RESET) << "\n";

        // --- 1. 函数耗时排行榜 ---
        std::cout << col(Ansi::BRIGHT_YELLOW) << "\n[Top Functions by Time]\n" << col(Ansi::RESET);

        std::vector<std::pair<std::string, FuncProfile>> funcList(funcProfiles.begin(), funcProfiles.end());
        std::sort(funcList.begin(), funcList.end(), [](const auto& a, const auto& b) {
            return a.second.totalTimeMs > b.second.totalTimeMs;
            });

        int count = 1;
        for (const auto& f : funcList) {
            if (count > 10) break;
            std::cout << "  " << count << ". " << std::left << std::setw(20) << f.first
                << " | " << std::setw(10) << std::fixed << std::setprecision(4) << f.second.totalTimeMs << " ms"
                << " | " << f.second.callCount << " calls\n";
            count++;
        }

        // --- 2. 虚拟机操作码热点榜 ---
        std::cout << col(Ansi::BRIGHT_YELLOW) << "\n[Top 15 VM OpCodes Frequency]\n" << col(Ansi::RESET);

        uint64_t totalOps = 0;
        std::vector<std::pair<OpCode, uint64_t>> opList(opCounts.begin(), opCounts.end());
        for (const auto& op : opList) totalOps += op.second;

        std::sort(opList.begin(), opList.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
            });

        if (totalOps == 0) totalOps = 1;
        count = 1;
        for (const auto& op : opList) {
            if (count > 15) break;  // ★ 设为 15，因为有全量字典了，可以多看几个大头

            // ★ 调用公共字典！去掉前三个字符 "OP_" 让显示更好看
            std::string opName = opCodeToString(op.first).substr(3);

            double perm = (static_cast<double>(op.second) / totalOps) * 100.0;
            std::cout << "  " << std::right << std::setw(2) << count << ". "
                << std::left << std::setw(16) << opName
                << " | " << std::setw(8) << op.second << " times ("
                << std::fixed << std::setprecision(2) << perm << "%)\n";
            count++;
        }
        std::cout << "\n  Total Instructions Executed: " << totalOps << "\n";
        std::cout << col(Ansi::BRIGHT_CYAN) << "==================================================" << col(Ansi::RESET) << "\n\n";

        funcProfiles.clear();
        opCounts.clear();
    }

// =================================================================
// ★ 垃圾回收器实现 (Mark-and-Sweep Garbage Collector)
// =================================================================

    void VM::markValue(const Value& val) {
        if (!val.isObj()) return;
        markObject(val.asObj());
    }

    void VM::markObject(Obj* obj) {
        if (obj == nullptr || obj->isMarked) return;
        obj->isMarked = true;
        grayStack.push_back(obj);
    }

    void VM::traceReferences() {
        while (!grayStack.empty()) {
            Obj* obj = grayStack.back();
            grayStack.pop_back();

            switch (obj->type) {
                case ObjType::LIST: {
                    for (const auto& elem : static_cast<ObjList*>(obj)->vec) markValue(elem);
                    break;
                }
                case ObjType::DICT: {
                    for (const auto& [key, v] : static_cast<ObjDict*>(obj)->elements) {
                        markValue(key); markValue(v);
                    }
                    break;
                }
                case ObjType::SET: {
                    for (const auto& elem : static_cast<ObjSet*>(obj)->elements) markValue(elem);
                    break;
                }
                case ObjType::INSTANCE: {
                    auto inst = static_cast<ObjInstance*>(obj);
                    if (inst->fields) markObject(inst->fields);
                    if (inst->classDef) markObject(inst->classDef);
                    break;
                }
                case ObjType::CLOSURE: {
                    auto cl = static_cast<ObjClosure*>(obj);
                    markValue(cl->boundSelf);
                    markValue(cl->boundClass);
                    for (int i = 0; i < cl->upvalueCount; ++i) {
                        if (cl->upvalues[i]) markObject(cl->upvalues[i]);
                    }
                    break;
                }
                case ObjType::UPVALUE: {
                    auto uv = static_cast<ObjUpVal*>(obj);
                    markValue(uv->closed);
                    if (uv->location && uv->location != &uv->closed) {
                        markValue(*(uv->location));
                    }
                    break;
                }
                case ObjType::CLASS: {
                    auto cls = static_cast<ObjClass*>(obj);
                    for (const auto& [name, method] : cls->methods) {
                        if (method) markObject(method);
                    }
                    if (cls->parent) markObject(cls->parent);
                    break;
                }
                case ObjType::SUPER_PROXY: {
                    auto sp = static_cast<ObjSuper*>(obj);
                    if (sp->instance) markObject(sp->instance);
                    if (sp->parentClass) markObject(sp->parentClass);
                    break;
                }
                case ObjType::NAMESPACE: {
                    auto ns = static_cast<ObjNamespace*>(obj);
                    for (const auto& [k, field] : ns->fields) {
                        if (field.upval) {
                            markObject(field.upval);
                        }
                    }
                    break;
                }
                default: break;
            }
        }
    }

    void VM::collectGarbage() {
        // ═══ Phase 1: MARK ═══

        // 根集合 1: 全局变量
        for (const auto& val : globalValues)
            markValue(val);

        // 根集合 1.5: 已加载的模块缓存
        for (const auto& [name, val] : loadedModules)
            markValue(val);

        // 根集合 1.6: 内置函数闭包缓存
        for (const auto& [name, val] : builtinClosures)
            markValue(val);

        // 根集合 2: 虚拟机求值栈
        for (Value* p = stack; p < stackTop; ++p)
            markValue(*p);

        // 根集合 3: 所有调用帧的闭包上值，以及存活帧的上下文引擎！
        for (int i = 0; i < frameCount; ++i) {
            const auto& f = frames[i];
            if (f.closure) {
                markObject(f.closure);
            }
            // ★ 世纪补漏：必须追踪目前存活函数的上下文环境！
            markValue(f.selfContext);
            markValue(f.classContext);
            
            // ★ 终极补漏：主脚本的常量池不在 compiledFunctions 中，必须通过活跃帧扫描！
            if (f.function) {
                for (const auto& c : f.function->chunk.constants)
                    markValue(c);
            }
        }

        // 根集合 4: 常量池 (编译后的函数里缓存的字面量) 和 内联缓存
        for (const auto& fn : compiledFunctions) {
            for (const auto& c : fn->chunk.constants)
                markValue(c);
            for (auto& ic : fn->chunk.inlineCaches) {
                if (ic.cachedClass) markObject(ic.cachedClass);
                if (ic.cachedMethod) markObject(ic.cachedMethod);
            }
        }

        // 根集合 5: C++ 层当前正在执行跨界调用的原生对象栈！
        for (const auto& val : helpers::nativeSelfStack) markValue(val);
        for (const auto& val : helpers::nativeClassStack) markValue(val);

        // 根集合 6: C++ 层 RAII 临时保护的 GC 根
        for (Obj* obj : GcHeap::get().getTempObjRoots()) markObject(obj);
        for (Value* val : GcHeap::get().getTempValueRoots()) markValue(*val);

        // 根集合 7: 开放上值链表
        ObjUpVal* uv = openUpvalues;
        while (uv) {
            markObject(uv);
            uv = uv->nextOpen;
        }

        // 根集合 8: 挂起的引用参数
        for (const auto& pr : pendingCallRefs) {
            if (pr.second) markObject(pr.second);
        }

        traceReferences();

        // ★ 在 sweep 之前，清理驻留池中未被标记的字符串
        for (auto it = g_internedStrings.begin(); it != g_internedStrings.end(); ) {
            if (!it->second->isMarked) {
                it = g_internedStrings.erase(it);
            } else {
                ++it;
            }
        }

        // ═══ Phase 2: SWEEP ═══
        GcHeap::get().sweep();
    }

    int VM::runGC() {
        for (const auto& val : globalValues)  markValue(val);
        for (const auto& [name, val] : loadedModules) markValue(val);
        for (const auto& [name, val] : builtinClosures) markValue(val);
        for (Value* p = stack; p < stackTop; ++p) markValue(*p);
        for (int i = 0; i < frameCount; ++i) {
            const auto& f = frames[i];
            if (f.closure) {
                markObject(f.closure);
            }
            // ★ 防止手动 gc() 触发对象丢失
            markValue(f.selfContext);
            markValue(f.classContext);
            
            if (f.function) {
                for (const auto& c : f.function->chunk.constants)
                    markValue(c);
            }
        }
        for (const auto& fn : compiledFunctions) {
            for (const auto& c : fn->chunk.constants) markValue(c);
            for (auto& ic : fn->chunk.inlineCaches) {
                if (ic.cachedClass) markObject(ic.cachedClass);
                if (ic.cachedMethod) markObject(ic.cachedMethod);
            }
        }

        // ★ C++ 原生堆栈手动同步
        for (const auto& val : helpers::nativeSelfStack) markValue(val);
        for (const auto& val : helpers::nativeClassStack) markValue(val);

        for (Obj* obj : GcHeap::get().getTempObjRoots()) markObject(obj);
        for (Value* val : GcHeap::get().getTempValueRoots()) markValue(*val);

        ObjUpVal* uv = openUpvalues;
        while (uv) {
            markObject(uv);
            uv = uv->nextOpen;
        }

        for (const auto& pr : pendingCallRefs) {
            if (pr.second) markObject(pr.second);
        }

        traceReferences();

        // ★ 在 sweep 之前，清理驻留池中未被标记的字符串
        for (auto it = g_internedStrings.begin(); it != g_internedStrings.end(); ) {
            if (!it->second->isMarked) {
                it = g_internedStrings.erase(it);
            } else {
                ++it;
            }
        }

        return GcHeap::get().sweep();
    }

    void VM::populateRefParams(CallFrame& newFrame, const CompiledFunction* fn) {
        int refCount = 0;
        for (int i = 0; i < fn->maxArity; ++i) {
            if (i < static_cast<int>(fn->paramIsRef.size()) && fn->paramIsRef[i]) {
                refCount++;
            }
        }
        if (refCount == 0) {
            newFrame.refParamsBase = -1;
            pendingCallRefs.clear();
            return;
        }

        newFrame.refParamsBase = static_cast<int>(getStackSize());
        for (int i = 0; i < refCount; ++i) {
            push(Value::none()); // 占位
        }

        int refIdx = 0;
        for (int i = 0; i < fn->maxArity; ++i) {
            if (i < static_cast<int>(fn->paramIsRef.size()) && fn->paramIsRef[i]) {
                ObjUpVal* providedRef = nullptr;
                for (auto& pr : pendingCallRefs) {
                    if (pr.first == i) {
                        providedRef = pr.second;
                        break;
                    }
                }
                if (providedRef) {
                    stack[newFrame.refParamsBase + refIdx] = Value(providedRef);
                } else {
                    ObjUpVal* dummy = GcHeap::get().allocate<ObjUpVal>();
                    dummy->location = &stack[newFrame.stackBase + i];
                    stack[newFrame.refParamsBase + refIdx] = Value(dummy);
                }
                refIdx++;
            }
        }
        pendingCallRefs.clear();
    }

    void VM::execCall(uint8_t argc, bool isTailCall) {
        struct CallRefGuard { VM* vm; ~CallRefGuard() { vm->pendingCallRefs.clear(); } } guard{this};
        Value callee = stack[getStackSize() - 1 - argc];

        // ======== [1] 字符串动态调用 (晚绑定) ========
        if (callee.isString()) {
            const std::string& tag = callee.asString();
            if (tag.size() >= 5 && tag.substr(0, 5) == "__fn:") {
                int fnIdx = std::stoi(tag.substr(5));
                auto& fn = compiledFunctions[fnIdx];
                if (static_cast<int>(argc) < fn->arity || static_cast<int>(argc) > fn->maxArity)
                    throw std::runtime_error("VM Error: '" + fn->name + "' expects args mismatch.");
                
                int padCount = fn->maxArity - static_cast<int>(argc);
                for (int j = 0; j < padCount; ++j) push(Value::none());
                int reserveCount = fn->localCount - fn->maxArity;
                for (int j = 0; j < reserveCount; ++j) push(Value::none());
                
                eraseStack(fn->localCount); // ★ FIX: 延迟移除 callee，保护其在可能触发的 GC 中存活
                
                if (isTailCall) {
                    int base = frame().stackBase;
                    closeUpvalues(base);
                    int newLocalCount = fn->localCount;
                    int argStart = static_cast<int>(getStackSize()) - newLocalCount;
                    if (base != argStart) {
                        for (int i = 0; i < newLocalCount; ++i) {
                            stack[base + i] = std::move(stack[argStart + i]);
                        }
                    }
                    setStackSize(base + newLocalCount);
                    frame().function = fn.get();
                    frame().ip = 0;
                    frame().closure = nullptr;
                    frame().selfContext = Value::none();
                    frame().classContext = Value::none();
                    populateRefParams(frame(), fn.get());
                    return;
                }

                CallFrame newFrame; newFrame.function = fn.get(); newFrame.ip = 0;
                newFrame.stackBase = static_cast<int>(getStackSize()) - fn->localCount;
                populateRefParams(newFrame, fn.get());
                if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                frames[frameCount++] = newFrame; return;
            }
            if (tag.size() >= 10 && tag.substr(0, 10) == "__builtin:") {
                std::string fnName = tag.substr(10); std::vector<Value> args(argc);
                for (int j = 0; j < argc; ++j) args[j] = peek(argc - 1 - j);
                Value result = nativeBuiltins.find(fnName)->second(args);
                for (int j = 0; j <= argc; ++j) pop();
                push(result); return;
            }
            auto nIt = nativeBuiltins.find(tag);
            if (nIt != nativeBuiltins.end()) {
                auto arityIt = builtinArity.find(tag);
                bool arityMatched = false;
                if (arityIt != builtinArity.end() && !arityIt->second.empty()) {
                    if (arityIt->second.count(argc)) arityMatched = true;
                }
                else {
                    arityMatched = true;
                }

                if (arityMatched) {
                    std::vector<Value> args(argc);
                    for (int j = 0; j < argc; ++j) args[j] = peek(argc - 1 - j);
                    Value result = nIt->second(args);
                    for (int j = 0; j <= argc; ++j) pop();
                    push(result);
                    return;
                }
            }

            auto it = globalNamesToSlots.find(tag);
            if (it != globalNamesToSlots.end()) {
                callee = globalValues[it->second];
                stack[getStackSize() - 1 - argc] = callee;
            }
            else {
                if (nIt != nativeBuiltins.end()) {
                    auto arityIt = builtinArity.find(tag);
                    std::string expected;
                    for (auto aIt = arityIt->second.begin(); aIt != arityIt->second.end(); ++aIt) {
                        if (aIt != arityIt->second.begin()) expected += " or ";
                        expected += std::to_string(*aIt);
                    }
                    throw std::runtime_error("Runtime Error: " + tag + "() expects " +
                        expected + " arguments, got " + std::to_string(argc) + ".");
                }
                throw std::runtime_error("Runtime Error: Unknown function or not callable '" + tag + "()'.");
            }
        } // 结束 if (holds_string)

        // ======== [2] 类实例化 ========
        if (callee.isClass()) {
            auto cls = static_cast<ObjClass*>(callee.asObj());
            auto instance = GcHeap::get().allocate<ObjInstance>();
            instance->classDef = cls;

            ObjClosure* initMethod = nullptr;
            ObjClass* initOwner = nullptr;
            auto c = cls;
            while (c) {
                auto it = c->methods.find("init");
                if (it != c->methods.end()) {
                    initMethod = it->second;
                    initOwner = c;
                    break;
                }
                c = c->parent;
            }

            if (initMethod) {
                if (initMethod->isBytecode()) {
                    auto& fnDef = compiledFunctions[initMethod->compiledFnIndex];

                    int padCount = fnDef->maxArity - static_cast<int>(argc);
                    for (int j = 0; j < padCount; ++j) push(Value::none());
                    int reserveCount = fnDef->localCount - fnDef->maxArity;
                    for (int j = 0; j < reserveCount; ++j) push(Value::none());

                    eraseStack(fnDef->localCount); // ★ FIX: 延迟移除 callee，保护其在可能触发的 GC 中存活

                    if (isTailCall) {
                        int base = frame().stackBase;
                        closeUpvalues(base);
                        int newLocalCount = fnDef->localCount;
                        int argStart = static_cast<int>(getStackSize()) - newLocalCount;
                        if (base != argStart) {
                            for (int i = 0; i < newLocalCount; ++i) {
                                stack[base + i] = std::move(stack[argStart + i]);
                            }
                        }
                        setStackSize(base + newLocalCount);
                        frame().function = fnDef.get();
                        frame().ip = 0;
                        frame().closure = initMethod;
                        frame().selfContext = Value(instance);
                        frame().classContext = Value(initOwner);
                        populateRefParams(frame(), fnDef.get());
                        return;
                    }

                    CallFrame newFrame;
                    // ★ NEW: 直接将新建的 instance 注入帧寄存器！绝不弄脏 globals
                    newFrame.selfContext = Value(instance);
                    newFrame.classContext = Value(initOwner);

                    newFrame.function = fnDef.get();
                    newFrame.ip = 0;
                    newFrame.stackBase = static_cast<int>(getStackSize()) - fnDef->localCount;
                    newFrame.closure = initMethod;
                    populateRefParams(newFrame, fnDef.get());
                    if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                    frames[frameCount++] = newFrame;
                    return;
                }
                else if (initMethod->isNative()) {
                    // ★ NEW: C++ 原生构造器，直接压入专属隔离栈！
                    helpers::nativeSelfStack.push_back(Value(instance));
                    helpers::nativeClassStack.push_back(Value(initOwner));

                    std::vector<Value> args(argc);
                    for (int j = 0; j < argc; ++j) args[j] = peek(argc - 1 - j);

                    try {
                        auto& fn = std::any_cast<NativeCallable&>(initMethod->nativeFn);
                        fn(args);
                    }
                    catch (...) {
                        helpers::nativeSelfStack.pop_back();
                        helpers::nativeClassStack.pop_back();
                        throw;
                    }
                    helpers::nativeSelfStack.pop_back();
                    helpers::nativeClassStack.pop_back();

                    for (int j = 0; j <= argc; ++j) pop();
                    push(Value(instance));
                }
            }
            else if (!initMethod) {
                if (argc > 0) {
                    throw std::runtime_error(
                        "TypeError: Class '" + cls->name +
                        "' takes no arguments directly (no 'init' method defined).");
                }

                // 如果是无参调用（合法），则弹出 Callee 并推入空壳 Instance
                for (int j = 0; j < argc; ++j) pop();
                pop();
                push(Value(instance));
            }
            else {
                throw std::runtime_error("VM Error: init has no callable implementation.");
            }
            return;
        } // 结束 if (holds_class)

        // ======== [3] 闭包执行 ========
        if (callee.isFunctionClosure()) {
            auto closure = callee.asFunction();

            if (closure->isBytecode()) {
                auto& fnDef = compiledFunctions[closure->compiledFnIndex];

                if (closure->isUFCS) {
                    // ★ UFCS 绑定闭包：将 boundSelf 插入到参数列表的最前面
                    if (static_cast<int>(getStackSize()) >= MAX_STACK) throw std::runtime_error("VM Error: Stack overflow.");
                    insertStack(argc, closure->boundSelf);
                    for (auto& pr : pendingCallRefs) pr.first += 1; // ★ UFCS 引用参数索引右移
                    argc++;
                }

                if (fnDef->hasRestParam) {
                    int fixedMax = fnDef->maxArity - 1;
                    if (static_cast<int>(argc) < fnDef->arity) {
                        throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                    }

                    ObjList* restList = GcHeap::get().allocate<ObjList>();
                    if (static_cast<int>(argc) > fixedMax) {
                        int restCount = static_cast<int>(argc) - fixedMax;
                        std::vector<Value> tempValues(restCount);
                        for (int j = 0; j < restCount; j++) {
                            tempValues[restCount - 1 - j] = pop();
                        }
                        for (int j = 0; j < restCount; j++) {
                            restList->vec.push_back(tempValues[j]);
                        }
                        argc = static_cast<uint8_t>(fixedMax);
                    }

                    int padCount = fixedMax - static_cast<int>(argc);
                    for (int j = 0; j < padCount; ++j) push(Value::none());
                    push(Value(restList));
                }
                else {
                    if (static_cast<int>(argc) < fnDef->arity || static_cast<int>(argc) > fnDef->maxArity)
                        throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(argc) + ".");
                    int padCount = fnDef->maxArity - static_cast<int>(argc);
                    for (int j = 0; j < padCount; ++j) push(Value::none());
                }

                int reserveCount = fnDef->localCount - fnDef->maxArity;
                for (int j = 0; j < reserveCount; ++j) push(Value::none());

                eraseStack(fnDef->localCount); // ★ FIX: 延迟移除 callee，保护其在可能触发的 GC 中存活

                if (isTailCall) {
                    int base = frame().stackBase;
                    closeUpvalues(base);
                    int newLocalCount = fnDef->localCount;
                    int argStart = static_cast<int>(getStackSize()) - newLocalCount;
                    if (base != argStart) {
                        for (int i = 0; i < newLocalCount; ++i) {
                            stack[base + i] = std::move(stack[argStart + i]);
                        }
                    }
                    setStackSize(base + newLocalCount);
                    frame().function = fnDef.get();
                    frame().ip = 0;
                    frame().closure = closure;
                    frame().selfContext = closure->boundSelf;
                    frame().classContext = closure->boundClass;
                    populateRefParams(frame(), fnDef.get());
                    return;
                }

                CallFrame newFrame;
                newFrame.function = fnDef.get();
                newFrame.ip = 0;
                newFrame.stackBase = static_cast<int>(getStackSize()) - fnDef->localCount;
                newFrame.closure = closure;

                // ★ NEW：将该闭包出生时带的 self 塞进新帧的心房！
                newFrame.selfContext = closure->boundSelf;
                newFrame.classContext = closure->boundClass;
                populateRefParams(newFrame, fnDef.get());

                if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                frames[frameCount++] = newFrame;
                return;
            }
            else if (closure->isNative()) {
                // ★ 修复：检查原生闭包的参数数量，防止 C++ 越界崩溃
                auto ait = builtinArity.find(closure->rawBody);
                if (ait != builtinArity.end() && !ait->second.empty()) {
                    if (ait->second.find(argc) == ait->second.end()) {
                        std::string expected;
                        for (auto aIt = ait->second.begin(); aIt != ait->second.end(); ++aIt) {
                            if (aIt != ait->second.begin()) expected += " or ";
                            expected += std::to_string(*aIt);
                        }
                        throw std::runtime_error("Runtime Error: Function '" + closure->rawBody + 
                            "' expects " + expected + " arguments, got " + std::to_string(argc) + ".");
                    }
                } else if (closure->maxArgs() > 0 && !closure->hasRestParam) {
                    if (static_cast<int>(argc) < closure->minArgs() || static_cast<int>(argc) > closure->maxArgs()) {
                        throw std::runtime_error("Runtime Error: Function '" + closure->rawBody + 
                            "' expects " + std::to_string(closure->minArgs()) + " to " + 
                            std::to_string(closure->maxArgs()) + " arguments, got " + 
                            std::to_string(argc) + ".");
                    }
                }

                std::vector<Value> args(argc);
                for (int j = 0; j < argc; ++j) args[j] = peek(argc - 1 - j);

                // ★ NEW：C++ 原生闭包也进隔离池
                helpers::nativeSelfStack.push_back(closure->boundSelf);
                helpers::nativeClassStack.push_back(closure->boundClass);

                auto& fn = std::any_cast<NativeCallable&>(closure->nativeFn);
                Value result;
                try { result = fn(args); }
                catch (...) {
                    helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
                    throw;
                }
                helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();

                for (int j = 0; j <= argc; ++j) pop();
                push(result);
                return;
            }
            throw std::runtime_error("VM Error: Invalid closure.");
        } // 结束 if (holds_function)

        // ======== [4] String Tag fallback ========
        if (callee.isString()) {
            const std::string& tag = callee.asString();

            if (tag.size() >= 5 && tag.substr(0, 5) == "__fn:") {
                int fnIdx = std::stoi(tag.substr(5));
                auto& fn = compiledFunctions[fnIdx];

                eraseStack(argc); // ★ FIX: 先安全移除 callee

                if (fn->hasRestParam) {
                    int fixedMax = fn->maxArity - 1;
                    if (static_cast<int>(argc) < fn->arity) {
                        throw std::runtime_error("VM Error: '" + fn->name + "' requires at least " + std::to_string(fn->arity) + " arguments.");
                    }

                    ObjList* restList = GcHeap::get().allocate<ObjList>();
                    if (static_cast<int>(argc) > fixedMax) {
                        int restCount = static_cast<int>(argc) - fixedMax;
                        std::vector<Value> tempValues(restCount);
                        for (int j = 0; j < restCount; j++) {
                            tempValues[restCount - 1 - j] = pop();
                        }
                        for (int j = 0; j < restCount; j++) {
                            restList->vec.push_back(tempValues[j]);
                        }
                        argc = static_cast<uint8_t>(fixedMax);
                    }

                    int padCount = fixedMax - static_cast<int>(argc);
                    for (int j = 0; j < padCount; ++j) push(Value::none());
                    push(Value(restList));
                }
                else {
                    if (static_cast<int>(argc) < fn->arity || static_cast<int>(argc) > fn->maxArity)
                        throw std::runtime_error("VM Error: '" + fn->name + "' expects " + std::to_string(fn->arity) + " to " + std::to_string(fn->maxArity) + " arguments, got " + std::to_string(argc) + ".");
                    int padCount = fn->maxArity - static_cast<int>(argc);
                    for (int j = 0; j < padCount; ++j) push(Value::none());
                }

                int reserveCount = fn->localCount - fn->maxArity;
                for (int j = 0; j < reserveCount; ++j) push(Value::none());

                if (isTailCall) {
                    int base = frame().stackBase;
                    closeUpvalues(base);
                    int newLocalCount = fn->localCount;
                    int argStart = static_cast<int>(getStackSize()) - newLocalCount;
                    if (base != argStart) {
                        for (int i = 0; i < newLocalCount; ++i) {
                            stack[base + i] = std::move(stack[argStart + i]);
                        }
                    }
                    setStackSize(base + newLocalCount);
                    frame().function = fn.get();
                    frame().ip = 0;
                    frame().closure = nullptr;
                    frame().selfContext = Value::none();
                    frame().classContext = Value::none();
                    populateRefParams(frame(), fn.get());
                    return;
                }

                CallFrame newFrame; newFrame.function = fn.get(); newFrame.ip = 0;
                newFrame.stackBase = static_cast<int>(getStackSize()) - fn->localCount;
                populateRefParams(newFrame, fn.get());
                if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                frames[frameCount++] = newFrame; return;
            }

            if (tag.size() >= 10 && tag.substr(0, 10) == "__builtin:") {
                std::string fnName = tag.substr(10);
                std::vector<Value> argsVec(argc);
                for (int j = 0; j < argc; ++j) argsVec[j] = peek(argc - 1 - j);
                auto nit = nativeBuiltins.find(fnName);
                if (nit == nativeBuiltins.end())
                    throw std::runtime_error("VM Error: Unknown builtin '" + fnName + "'.");
                Value result = nit->second(argsVec);
                for (int j = 0; j <= argc; ++j) pop();
                push(result);
                return;
            }
        } // 结束 if (fallback string tag)

        // ======== [5] 实例的 __call__ 魔术方法 ========
        if (callee.isInstance()) {
            auto inst = callee.asInstance();
            ObjClosure* method = nullptr;
            ObjClass* owningClass = nullptr;
            auto c = inst->classDef;
            while (c) {
                auto it = c->methods.find(DUNDER_CALL);
                if (it != c->methods.end()) {
                    method = it->second;
                    owningClass = c;
                    break;
                }
                c = c->parent;
            }

            if (method) {
                if (method->isBytecode()) {
                    auto& fnDef = compiledFunctions[method->compiledFnIndex];

                    if (fnDef->hasRestParam) {
                        int fixedMax = fnDef->maxArity - 1;
                        if (static_cast<int>(argc) < fnDef->arity) {
                            throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                        }
                        ObjList* restList = GcHeap::get().allocate<ObjList>();
                        if (static_cast<int>(argc) > fixedMax) {
                            int restCount = static_cast<int>(argc) - fixedMax;
                            std::vector<Value> tempValues(restCount);
                            for (int j = 0; j < restCount; j++) tempValues[restCount - 1 - j] = pop();
                            for (int j = 0; j < restCount; j++) restList->vec.push_back(tempValues[j]);
                            argc = static_cast<uint8_t>(fixedMax);
                        }
                        int padCount = fixedMax - static_cast<int>(argc);
                        for (int j = 0; j < padCount; ++j) push(Value::none());
                        push(Value(restList));
                    } else {
                        if (static_cast<int>(argc) < fnDef->arity || static_cast<int>(argc) > fnDef->maxArity)
                            throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(argc) + ".");
                        int padCount = fnDef->maxArity - static_cast<int>(argc);
                        for (int j = 0; j < padCount; ++j) push(Value::none());
                    }

                    int reserveCount = fnDef->localCount - fnDef->maxArity;
                    for (int j = 0; j < reserveCount; ++j) push(Value::none());

                    eraseStack(fnDef->localCount); // ★ FIX: 延迟移除 callee，保护其在可能触发的 GC 中存活

                    if (isTailCall) {
                        int base = frame().stackBase;
                        closeUpvalues(base);
                        int newLocalCount = fnDef->localCount;
                        int argStart = static_cast<int>(getStackSize()) - newLocalCount;
                        if (base != argStart) {
                            for (int i = 0; i < newLocalCount; ++i) {
                                stack[base + i] = std::move(stack[argStart + i]);
                            }
                        }
                        setStackSize(base + newLocalCount);
                        frame().function = fnDef.get();
                        frame().ip = 0;
                        frame().closure = method;
                        frame().selfContext = callee;
                        frame().classContext = Value(owningClass);
                        populateRefParams(frame(), fnDef.get());
                        return;
                    }

                    CallFrame newFrame;
                    newFrame.function = fnDef.get();
                    newFrame.ip = 0;
                    newFrame.stackBase = static_cast<int>(getStackSize()) - fnDef->localCount;
                    newFrame.closure = method;
                    
                    // ★ 核心：将实例自身作为 self 注入
                    newFrame.selfContext = callee;
                    newFrame.classContext = Value(owningClass);
                    populateRefParams(newFrame, fnDef.get());

                    if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                    frames[frameCount++] = newFrame;
                    return;
                } else if (method->isNative()) {
                    std::vector<Value> argsVec(argc);
                    for (int j = 0; j < argc; ++j) argsVec[j] = peek(argc - 1 - j);

                    helpers::nativeSelfStack.push_back(callee);
                    helpers::nativeClassStack.push_back(Value(owningClass));

                    auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
                    Value result;
                    try { result = fn(argsVec); }
                    catch (...) {
                        helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
                        throw;
                    }
                    helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();

                    for (int j = 0; j <= argc; ++j) pop();
                    push(result);
                    return;
                }
            }
        }

        {
            std::vector<Value> args(argc);
            for (int j = argc - 1; j >= 0; --j) args[j] = pop();
            Value calleeVal = pop();
            std::string desc;
            if (calleeVal.isString())
                desc = calleeVal.asString();
            else {
                std::ostringstream oss;
                if (calleeVal.isUninit()) oss << "Uninitialized";
                else oss << calleeVal;
                desc = oss.str();
            }
            throw std::runtime_error("VM Error: '" + desc + "' is not callable.");
        }
    } // 结束 execCall

    void VM::execIndexGet(uint8_t dims) {
        if (dims == 1) {
            Value idx = peek(0);
            Value obj = peek(1);
            Value result;

            if (obj.isObjType(ObjType::DICT)) {
                auto dict = static_cast<ObjDict*>(obj.asObj());
                auto it = dict->keyMap.find(idx);
                if (it == dict->keyMap.end()) {
                    std::string keyStr;
                    if (idx.isString()) keyStr = idx.asString();
                    else {
                        std::ostringstream oss;
                        if (idx.isUninit()) oss << "Uninitialized";
                        else oss << idx;
                        keyStr = oss.str();
                    }
                    throw std::runtime_error("VM Error: Key '" + keyStr + "' not found.");
                }
                result = dict->elements[it->second].second;
            }
            else if (obj.isObjType(ObjType::NAMESPACE)) {
                auto ns = static_cast<ObjNamespace*>(obj.asObj());
                if (!idx.isString()) throw std::runtime_error("Type Error: Namespace keys must be strings.");
                auto it = ns->fields.find(idx.asString());
                if (it == ns->fields.end()) throw std::runtime_error("VM Error: Field '" + idx.asString() + "' not found in namespace.");
                result = *(it->second.upval->location);
            }
            else if (obj.isInstance()) {
                auto inst = obj.asInstance();
                auto c = inst->classDef;
                ObjClosure* getitemMethod = nullptr;
                while (c) {
                    auto it = c->methods.find(DUNDER_GETITEM);
                    if (it != c->methods.end()) {
                        getitemMethod = it->second;
                        break;
                    }
                    c = c->parent;
                }
                if (getitemMethod) {
                    if (getitemMethod->isBytecode()) {
                        auto& fnDef = compiledFunctions[getitemMethod->compiledFnIndex];
                        
                        int padCount = fnDef->maxArity - 1;
                        for (int j = 0; j < padCount; ++j) push(Value::none());
                        int reserveCount = fnDef->localCount - fnDef->maxArity;
                        for (int j = 0; j < reserveCount; ++j) push(Value::none());

                        eraseStack(fnDef->localCount); // ★ FIX: 延迟移除 obj，保护其在可能触发的 GC 中存活

                        CallFrame newFrame;
                        newFrame.function = fnDef.get();
                        newFrame.ip = 0;
                        newFrame.stackBase = static_cast<int>(getStackSize()) - fnDef->localCount;
                        newFrame.closure = getitemMethod;

                        newFrame.selfContext = Value(inst);
                        newFrame.classContext = Value(inst->classDef);
                        if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                        frames[frameCount++] = newFrame;
                        return; // ★ 绝对返回防线
                    }
                    else if (getitemMethod->isNative()) {
                        helpers::nativeSelfStack.push_back(Value(inst));
                        helpers::nativeClassStack.push_back(Value(inst->classDef));
                        try {
                            auto& fn = std::any_cast<NativeCallable&>(getitemMethod->nativeFn);
                            result = fn({ idx });
                        }
                        catch (...) {
                            helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
                            throw;
                        }
                        helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
                    }
                    else {
                        throw std::runtime_error("VM Error: __getitem__ has no callable implementation.");
                    }
                }
                else {
                    throw std::runtime_error("VM Error: Cannot index this instance (no __getitem__).");
                }
            }
            else {
                // ==========================================================
                // ★ 高级容器阻断防线 (放在 Instance 检查下面！)
                // ==========================================================
                if (!obj.isObjType(ObjType::REAL_MATRIX) &&
                    !obj.isObjType(ObjType::COMPLEX_MATRIX) &&
                    !obj.isObjType(ObjType::STRING_MATRIX) &&
                    !obj.isObjType(ObjType::LIST) &&
                    !obj.isString()) {
                    throw std::runtime_error("TypeError: Cannot index into a value of type '" + getTypeName(obj) + "'.");
                }

                int i = 0;
                try {
                    i = static_cast<int>(std::round(idx.asDouble()));
                }
                catch (...) {
                    throw std::runtime_error("TypeError: Array or List index must be a number, got '" + getTypeName(idx) + "'.");
                }

                if (obj.isObjType(ObjType::REAL_MATRIX)) {
                    const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                    if (m.getRows() == 1) {
                        if (i < 0) i = m.getCols() + i;
                        if (i < 0 || i >= m.getCols()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m(0, i));
                    }
                    else if (m.getCols() == 1) {
                        if (i < 0) i = m.getRows() + i;
                        if (i < 0 || i >= m.getRows()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m(i, 0));
                    }
                    else {
                        if (i < 0) i = m.getRows() + i;
                        if (i < 0 || i >= m.getRows()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m.getRow(i));
                    }
                }
                else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                    const auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                    if (m.getRows() == 1) {
                        if (i < 0) i = m.getCols() + i;
                        if (i < 0 || i >= m.getCols()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m(0, i));
                    }
                    else if (m.getCols() == 1) {
                        if (i < 0) i = m.getRows() + i;
                        if (i < 0 || i >= m.getRows()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m(i, 0));
                    }
                    else {
                        if (i < 0) i = m.getRows() + i;
                        if (i < 0 || i >= m.getRows()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m.getRow(i));
                    }
                }
                else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                    const auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                    if (m.getRows() == 1) {
                        if (i < 0) i = m.getCols() + i;
                        if (i < 0 || i >= m.getCols()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m(0, i));
                    }
                    else if (m.getCols() == 1) {
                        if (i < 0) i = m.getRows() + i;
                        if (i < 0 || i >= m.getRows()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m(i, 0));
                    }
                    else {
                        if (i < 0) i = m.getRows() + i;
                        if (i < 0 || i >= m.getRows()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                        result = Value(m.getRow(i));
                    }
                }
                else if (obj.isObjType(ObjType::LIST)) {
                    auto list = static_cast<ObjList*>(obj.asObj());
                    int n = static_cast<int>(list->vec.size());
                    if (i < 0) i = n + i;
                    if (i < 0 || i >= n) throw std::out_of_range("List Error: Index out of bounds.");
                    result = list->vec[i];
                }
                else if (obj.isString()) {
                    const auto& s = obj.asString();
                    if (i < 0) i = static_cast<int>(s.size()) + i;
                    if (i < 0 || i >= static_cast<int>(s.size()))
                        throw std::runtime_error("VM Error: String index out of bounds.");
                    result = Value(std::string(1, s[i]));
                }
            }

            pop(); pop();
            push(result);
        }
        else if (dims == 2) {
            Value col = peek(0);
            Value row = peek(1);
            Value obj = peek(2);
            Value result;

            int r = static_cast<int>(std::round(row.asDouble()));
            int c = static_cast<int>(std::round(col.asDouble()));

            if (obj.isObjType(ObjType::REAL_MATRIX)) {
                const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                if (r < 0) r = m.getRows() + r;
                if (c < 0) c = m.getCols() + c;
                if (r < 0 || r >= m.getRows() || c < 0 || c >= m.getCols()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                result = Value(m(r, c));
            }
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                const auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                if (r < 0) r = m.getRows() + r;
                if (c < 0) c = m.getCols() + c;
                if (r < 0 || r >= m.getRows() || c < 0 || c >= m.getCols()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                result = Value(m(r, c));
            }
            else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                const auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                if (r < 0) r = m.getRows() + r;
                if (c < 0) c = m.getCols() + c;
                if (r < 0 || r >= m.getRows() || c < 0 || c >= m.getCols()) throw std::out_of_range("Matrix Error: Index out of bounds.");
                result = Value(m(r, c));
            }
            else {
                throw std::runtime_error("VM Error: 2D indexing requires a matrix.");
            }

            pop(); pop(); pop();
            push(result);
        }
        else {
            throw std::runtime_error("VM Error: Unsupported index dimensionality.");
        }
    }

    void VM::execIndexSet(uint8_t dims) {
        if (dims == 1) {
            Value val = peek(0);
            Value idx = peek(1);
            Value obj = peek(2);

            if (obj.isObjType(ObjType::DICT)) {
                auto d = static_cast<ObjDict*>(obj.asObj());
                d->set(idx, val);
                pop(); pop(); pop();
                push(val); push(obj); return;
            }

            if (obj.isObjType(ObjType::NAMESPACE)) {
                auto ns = static_cast<ObjNamespace*>(obj.asObj());
                ns->checkModify();
                if (!idx.isString()) throw std::runtime_error("Type Error: Namespace keys must be strings.");
                std::string key = idx.asString();
                auto it = ns->fields.find(key);
                if (it != ns->fields.end()) {
                    if (it->second.isConst) throw std::runtime_error("Runtime Error: Cannot modify const property '" + key + "' in namespace '" + ns->name + "'.");
                    *(it->second.upval->location) = val;
                } else {
                    auto uv = GcHeap::get().allocate<ObjUpVal>();
                    uv->closed = val;
                    uv->location = &uv->closed;
                    ns->fields[key] = { uv, false };
                }
                pop(); pop(); pop();
                push(val); push(obj); return;
            }

            // ── Instance (__setitem__) ──
            if (obj.isInstance()) {
                auto inst = obj.asInstance();
                inst->checkModify();
                auto c = inst->classDef;
                ObjClosure* setitemMethod = nullptr;
                while (c) {
                    auto it = c->methods.find(DUNDER_SETITEM);
                    if (it != c->methods.end()) {
                        setitemMethod = it->second;
                        break;
                    }
                    c = c->parent;
                }
                if (setitemMethod) {
                    if (setitemMethod->isBytecode()) {
                        eraseStack(2); // remove obj, idx. val shifts to peek(0)
                        pop(); // remove val
                        
                        callVMFunction(setitemMethod->compiledFnIndex, { idx, val }, setitemMethod, Value(inst), Value(inst->classDef));
                        push(val); push(obj); return; // ★ 绝对返回防线！
                    }
                    else if (setitemMethod->isNative()) {
                        helpers::nativeSelfStack.push_back(Value(inst));
                        helpers::nativeClassStack.push_back(Value(inst->classDef));
                        try {
                            auto& fn = std::any_cast<NativeCallable&>(setitemMethod->nativeFn);
                            fn({ idx, val });
                        }
                        catch (...) {
                            helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
                            throw;
                        }
                        helpers::nativeSelfStack.pop_back(); helpers::nativeClassStack.pop_back();
                        pop(); pop(); pop();
                        push(val); push(obj); return; // ★ 绝对返回防线！
                    }
                    else {
                        throw std::runtime_error("VM Error: __setitem__ has no callable implementation.");
                    }
                }
                else {
                    throw std::runtime_error("VM Error: Cannot assign index on this instance (no __setitem__).");
                }
            }

            // ==========================================================
            // ★ 高级容器阻断防线 (放在 Instance 检查下面！)
            // ==========================================================
            if (!obj.isObjType(ObjType::REAL_MATRIX) &&
                !obj.isObjType(ObjType::COMPLEX_MATRIX) &&
                !obj.isObjType(ObjType::STRING_MATRIX) &&
                !obj.isObjType(ObjType::LIST) &&
                !obj.isString()) {
                throw std::runtime_error("TypeError: Cannot index into a value of type '" + getTypeName(obj) + "'.");
            }

            int i = 0;
            try {
                i = static_cast<int>(std::round(idx.asDouble()));
            }
            catch (...) {
                throw std::runtime_error("TypeError: Array or List index must be a number, got '" + getTypeName(idx) + "'.");
            }

            if (obj.isObjType(ObjType::REAL_MATRIX)) {
                if (val.isComplex() || val.isObjType(ObjType::COMPLEX_MATRIX)) {
                    ComplexMatrix cm = static_cast<ObjRealMatrix*>(obj.asObj())->mat.toComplexMatrix();
                    if (cm.getRows() == 1) {
                        if (i < 0) i = cm.getCols() + i;
                        cm(0, i) = val.asComplex();
                    }
                    else if (cm.getCols() == 1) {
                        if (i < 0) i = cm.getRows() + i;
                        cm(i, 0) = val.asComplex();
                    }
                    else {
                        if (i < 0) i = cm.getRows() + i;
                        if (i < 0 || i >= cm.getRows()) throw std::out_of_range("VM Error: Row index out of bounds.");
                        if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                            auto srcFlat = static_cast<ObjComplexMatrix*>(val.asObj())->mat.rawData();
                            if (static_cast<int>(srcFlat.size()) != cm.getCols()) throw std::runtime_error("VM Error: Row assignment size mismatch.");
                            for (int j = 0; j < cm.getCols(); ++j) cm(i, j) = srcFlat[j];
                        }
                        else {
                            Complex cv = val.asComplex();
                            for (int j = 0; j < cm.getCols(); ++j) cm(i, j) = cv;
                        }
                    }
                    obj = Value(cm);
                }
                else {
                    if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
                    auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                    if (m.getRows() == 1) {
                        if (i < 0) i = m.getCols() + i;
                        m(0, i) = val.asDouble();
                    }
                    else if (m.getCols() == 1) {
                        if (i < 0) i = m.getRows() + i;
                        m(i, 0) = val.asDouble();
                    }
                    else {
                        // 2D 矩阵整行赋值 / 广播
                        if (i < 0) i = m.getRows() + i;
                        if (i < 0 || i >= m.getRows()) throw std::out_of_range("VM Error: Row index out of bounds.");
                        if (val.isObjType(ObjType::REAL_MATRIX)) {
                            const auto& src = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                            auto srcFlat = src.rawData();
                            if (static_cast<int>(srcFlat.size()) != m.getCols())
                                throw std::runtime_error("VM Error: Row assignment size mismatch.");
                            for (int j = 0; j < m.getCols(); ++j) m(i, j) = srcFlat[j];
                        }
                        else {
                            double dVal = val.asDouble();
                            for (int j = 0; j < m.getCols(); ++j) m(i, j) = dVal;
                        }
                    }
                }
            }
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                if (m.getRows() == 1) {
                    if (i < 0) i = m.getCols() + i;
                    m(0, i) = val.asComplex();
                }
                else if (m.getCols() == 1) {
                    if (i < 0) i = m.getRows() + i;
                    m(i, 0) = val.asComplex();
                }
                else {
                    if (i < 0) i = m.getRows() + i;
                    if (i < 0 || i >= m.getRows()) throw std::out_of_range("VM Error: Row index out of bounds.");
                    if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                        auto srcFlat = static_cast<ObjComplexMatrix*>(val.asObj())->mat.rawData();
                        if (static_cast<int>(srcFlat.size()) != m.getCols()) throw std::runtime_error("VM Error: Row assignment size mismatch.");
                        for (int j = 0; j < m.getCols(); ++j) m(i, j) = srcFlat[j];
                    }
                    else if (val.isObjType(ObjType::REAL_MATRIX)) {
                        auto srcFlat = static_cast<ObjRealMatrix*>(val.asObj())->mat.rawData();
                        if (static_cast<int>(srcFlat.size()) != m.getCols()) throw std::runtime_error("VM Error: Row assignment size mismatch.");
                        for (int j = 0; j < m.getCols(); ++j) m(i, j) = Complex(srcFlat[j], 0.0);
                    }
                    else {
                        Complex cv = val.asComplex();
                        for (int j = 0; j < m.getCols(); ++j) m(i, j) = cv;
                    }
                }
            }
            else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                if (m.getRows() == 1) {
                    if (i < 0) i = m.getCols() + i;
                    if (val.isString()) m(0, i) = val.asString();
                    else { std::ostringstream oss; oss << val; m(0, i) = oss.str(); }
                }
                else if (m.getCols() == 1) {
                    if (i < 0) i = m.getRows() + i;
                    if (val.isString()) m(i, 0) = val.asString();
                    else { std::ostringstream oss; oss << val; m(i, 0) = oss.str(); }
                }
                else {
                    if (i < 0) i = m.getRows() + i;
                    if (i < 0 || i >= m.getRows()) throw std::out_of_range("VM Error: Row index out of bounds.");
                    if (val.isObjType(ObjType::STRING_MATRIX)) {
                        auto srcFlat = static_cast<ObjStringMatrix*>(val.asObj())->mat.rawData();
                        if (static_cast<int>(srcFlat.size()) != m.getCols()) throw std::runtime_error("VM Error: Row assignment size mismatch.");
                        for (int j = 0; j < m.getCols(); ++j) m(i, j) = srcFlat[j];
                    }
                    else if (val.isObjType(ObjType::REAL_MATRIX)) {
                        auto srcFlat = static_cast<ObjRealMatrix*>(val.asObj())->mat.rawData();
                        if (static_cast<int>(srcFlat.size()) != m.getCols()) throw std::runtime_error("VM Error: Row assignment size mismatch.");
                        for (int j = 0; j < m.getCols(); ++j) { std::ostringstream oss; oss << Value(srcFlat[j]); m(i, j) = oss.str(); }
                    }
                    else if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                        auto srcFlat = static_cast<ObjComplexMatrix*>(val.asObj())->mat.rawData();
                        if (static_cast<int>(srcFlat.size()) != m.getCols()) throw std::runtime_error("VM Error: Row assignment size mismatch.");
                        for (int j = 0; j < m.getCols(); ++j) { std::ostringstream oss; oss << Value(srcFlat[j]); m(i, j) = oss.str(); }
                    }
                    else {
                        std::string s;
                        if (val.isString()) s = val.asString();
                        else { std::ostringstream oss; oss << val; s = oss.str(); }
                        for (int j = 0; j < m.getCols(); ++j) m(i, j) = s;
                    }
                }
            }
            else if (obj.isObjType(ObjType::LIST)) {
                auto list = static_cast<ObjList*>(obj.asObj());
                int n = static_cast<int>(list->vec.size());
                if (i < 0) i = n + i;
                if (i < 0 || i >= n) throw std::out_of_range("List Error: Index out of bounds.");
                list->mut()[i] = val;
            }
            else if (obj.isString()) {
                std::string s = obj.asString();
                if (i < 0) i = static_cast<int>(s.size()) + i;
                if (i < 0 || i >= static_cast<int>(s.size()))
                    throw std::runtime_error("VM Error: String index out of bounds.");
                if (!val.isString() || val.asString().size() != 1)
                    throw std::runtime_error("VM Error: String element assignment requires a single character.");
                s[i] = val.asString()[0];
                obj = Value(s);
            }
            pop(); pop(); pop();
            push(val);
            push(obj);
        }
        else if (dims == 2) {
            Value val = peek(0);
            Value col = peek(1);
            Value row = peek(2);
            Value obj = peek(3);
            int r = static_cast<int>(std::round(row.asDouble()));
            int c = static_cast<int>(std::round(col.asDouble()));

            if (obj.isObjType(ObjType::REAL_MATRIX)) {
                if (val.isComplex()) {
                    ComplexMatrix cm = static_cast<ObjRealMatrix*>(obj.asObj())->mat.toComplexMatrix();
                    if (r < 0) r = cm.getRows() + r;
                    if (c < 0) c = cm.getCols() + c;
                    cm(r, c) = val.asComplex();
                    obj = Value(cm);
                }
                else {
                    if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
                    auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                    if (r < 0) r = m.getRows() + r;
                    if (c < 0) c = m.getCols() + c;
                    m(r, c) = val.asDouble();
                }
            }
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                if (r < 0) r = m.getRows() + r;
                if (c < 0) c = m.getCols() + c;
                m(r, c) = val.asComplex();
            }
            else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                if (r < 0) r = m.getRows() + r;
                if (c < 0) c = m.getCols() + c;
                if (val.isString())
                    m(r, c) = val.asString();
                else {
                    std::ostringstream oss;
                    if (val.isUninit()) oss << "Uninitialized";
                    else oss << val;
                    m(r, c) = oss.str();
                }
            }
            else {
                throw std::runtime_error("VM Error: 2D index assignment requires a matrix.");
            }
            pop(); pop(); pop(); pop();
            push(val);
            push(obj);
        }
    }

    void VM::execSliceGet(uint8_t dims) {
        int popCount = 0;
        auto readOptionalInt = [this, &popCount]() -> std::pair<bool, int> {
            Value v = peek(popCount++);
            if (v.isNone()) return { false, 0 };
            return { true, static_cast<int>(std::round(v.asDouble())) };
            };

        auto buildSliceIndices = [](int dimSize, std::pair<bool, int> start,
            std::pair<bool, int> end,
            std::pair<bool, int> step) -> std::vector<int> {
                int sp = step.first ? step.second : 1;

                // ★ 点索引标记：step 被显式设置为 0
                if (step.first && sp == 0) {
                    int idx = start.first ? start.second : 0;
                    if (idx < 0) idx = dimSize + idx;
                    if (idx < 0 || idx >= dimSize)
                        throw std::out_of_range("VM Error: Index out of bounds.");
                    return { idx };
                }

                int st, en;
                if (sp > 0) {
                    st = start.first ? start.second : 0;
                    en = end.first ? end.second : dimSize;
                }
                else {
                    st = start.first ? start.second : dimSize - 1;
                    en = end.first ? end.second : -1;
                }

                if (st < 0) st = dimSize + st;
                if (en < 0 && end.first) en = dimSize + en;

                if (sp > 0) {
                    st = std::max(0, std::min(dimSize, st));
                    en = std::max(0, std::min(dimSize, en));
                }
                else {
                    st = std::max(-1, std::min(dimSize - 1, st));
                    en = std::max(-1, std::min(dimSize - 1, en));
                }

                std::vector<int> ids;
                if (sp > 0) {
                    for (int i = st; i < en; i += sp) ids.push_back(i);
                }
                else {
                    for (int i = st; i > en; i += sp) ids.push_back(i);
                }
                return ids;
            };

        if (dims == 1) {
            auto step = readOptionalInt();
            auto end = readOptionalInt();
            auto start = readOptionalInt();
            Value obj = peek(popCount++);

            if (obj.isString()) {
                const auto& s = obj.asString();
                auto ids = buildSliceIndices(static_cast<int>(s.size()), start, end, step);
                std::string result;
                for (int id : ids) result += s[id];
                for (int i = 0; i < popCount; ++i) pop();
                push(Value(result));
                return;
            }

            if (obj.isObjType(ObjType::REAL_MATRIX)) {
                const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
                auto ids = buildSliceIndices(n, start, end, step);
                std::vector<double> result;
                if (m.getRows() == 1) {
                    for (int id : ids) result.push_back(m(0, id));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(RealMatrix(1, static_cast<int>(result.size()), result)));
                }
                else if (m.getCols() == 1) {
                    for (int id : ids) result.push_back(m(id, 0));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(RealMatrix(static_cast<int>(result.size()), 1, result)));
                }
                else {
                    int rc = static_cast<int>(ids.size());
                    std::vector<double> flat;
                    for (int id : ids)
                        for (int j = 0; j < m.getCols(); ++j)
                            flat.push_back(m(id, j));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(RealMatrix(rc, m.getCols(), flat)));
                }
                return;
            }

            if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                const auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
                auto ids = buildSliceIndices(n, start, end, step);
                if (m.getRows() == 1) {
                    std::vector<Complex> result;
                    for (int id : ids) result.push_back(m(0, id));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(ComplexMatrix(1, static_cast<int>(result.size()), result)));
                }
                else if (m.getCols() == 1) {
                    std::vector<Complex> result;
                    for (int id : ids) result.push_back(m(id, 0));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(ComplexMatrix(static_cast<int>(result.size()), 1, result)));
                }
                else {
                    int rc = static_cast<int>(ids.size());
                    std::vector<Complex> flat;
                    for (int id : ids)
                        for (int j = 0; j < m.getCols(); ++j)
                            flat.push_back(m(id, j));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(ComplexMatrix(rc, m.getCols(), flat)));
                }
                return;
            }

            if (obj.isObjType(ObjType::STRING_MATRIX)) {
                const auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
                auto ids = buildSliceIndices(n, start, end, step);
                if (m.getRows() == 1) {
                    std::vector<std::string> result;
                    for (int id : ids) result.push_back(m(0, id));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(StringMatrix(1, static_cast<int>(result.size()), result)));
                }
                else if (m.getCols() == 1) {
                    std::vector<std::string> result;
                    for (int id : ids) result.push_back(m(id, 0));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(StringMatrix(static_cast<int>(result.size()), 1, result)));
                }
                else {
                    int rc = static_cast<int>(ids.size());
                    std::vector<std::string> flat;
                    for (int id : ids)
                        for (int j = 0; j < m.getCols(); ++j)
                            flat.push_back(m(id, j));
                    for (int i = 0; i < popCount; ++i) pop();
                    push(Value(StringMatrix(rc, m.getCols(), flat)));
                }
                return;
            }

            if (obj.isObjType(ObjType::LIST)) {
                const auto& L = static_cast<ObjList*>(obj.asObj())->vec;
                auto ids = buildSliceIndices(static_cast<int>(L.size()), start, end, step);
                ObjList* result = GcHeap::get().allocate<ObjList>();
                for (int id : ids) result->vec.push_back(L[id]);
                for (int i = 0; i < popCount; ++i) pop();
                push(Value(result));
                return;
            }

            throw std::runtime_error("VM Error: Cannot slice a value of type '" + getTypeName(obj) + "'.");
        }
        else if (dims == 2) {
            auto cStep = readOptionalInt();
            auto cEnd = readOptionalInt();
            auto cStart = readOptionalInt();
            auto rStep = readOptionalInt();
            auto rEnd = readOptionalInt();
            auto rStart = readOptionalInt();
            Value obj = peek(popCount++);

            auto processMatSlice = [&](const auto& m) {
                auto rIds = buildSliceIndices(m.getRows(), rStart, rEnd, rStep);
                auto cIds = buildSliceIndices(m.getCols(), cStart, cEnd, cStep);

                using MatType = std::decay_t<decltype(m)>;
                using ElemType = std::decay_t<decltype(m(0, 0))>;
                std::vector<ElemType> flat;
                for (int ri : rIds)
                    for (int ci : cIds)
                        flat.push_back(m(ri, ci));
                for (int i = 0; i < popCount; ++i) pop();
                push(Value(MatType(static_cast<int>(rIds.size()),
                    static_cast<int>(cIds.size()), flat)));
                };

            if (obj.isObjType(ObjType::REAL_MATRIX)) {
                processMatSlice(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
            }
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                processMatSlice(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
            }
            else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                processMatSlice(static_cast<ObjStringMatrix*>(obj.asObj())->mat);
            }
            else {
                throw std::runtime_error("VM Error: 2D slicing requires a matrix, got '" + getTypeName(obj) + "'.");
            }
        }
        else {
            throw std::runtime_error("VM Error: Unsupported slice dimensionality.");
        }
        return;
    }

    void VM::execSliceSet(uint8_t dims) {
        int popCount = 0;
        auto readOptionalInt = [this, &popCount]() -> std::pair<bool, int> {
            Value v = peek(popCount++);
            if (v.isNone()) return { false, 0 };
            return { true, static_cast<int>(std::round(v.asDouble())) };
            };

        auto buildSliceIndices = [](int dimSize, std::pair<bool, int> start,
            std::pair<bool, int> end,
            std::pair<bool, int> step) -> std::vector<int> {
                int sp = step.first ? step.second : 1;

                // ★ 点索引标记：step 被显式设置为 0
                if (step.first && sp == 0) {
                    int idx = start.first ? start.second : 0;
                    if (idx < 0) idx = dimSize + idx;
                    if (idx < 0 || idx >= dimSize)
                        throw std::out_of_range("VM Error: Index out of bounds.");
                    return { idx };
                }

                int st, en;
                if (sp > 0) {
                    st = start.first ? start.second : 0;
                    en = end.first ? end.second : dimSize;
                }
                else {
                    st = start.first ? start.second : dimSize - 1;
                    en = end.first ? end.second : -1;
                }

                if (st < 0) st = dimSize + st;
                if (en < 0 && end.first) en = dimSize + en;

                if (sp > 0) {
                    st = std::max(0, std::min(dimSize, st));
                    en = std::max(0, std::min(dimSize, en));
                }
                else {
                    st = std::max(-1, std::min(dimSize - 1, st));
                    en = std::max(-1, std::min(dimSize - 1, en));
                }

                std::vector<int> ids;
                if (sp > 0) {
                    for (int i = st; i < en; i += sp) ids.push_back(i);
                }
                else {
                    for (int i = st; i > en; i += sp) ids.push_back(i);
                }
                return ids;
            };

        if (dims == 1) {
            Value val = peek(popCount++);
            auto step = readOptionalInt();
            auto end = readOptionalInt();
            auto start = readOptionalInt();
            Value obj = peek(popCount++);

            if (obj.isObjType(ObjType::REAL_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
                auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
                auto ids = buildSliceIndices(n, start, end, step);

                if (val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION)) {
                    double v = val.asDouble();
                    if (m.getRows() == 1) {
                        for (int id : ids) m(0, id) = v;
                    }
                    else if (m.getCols() == 1) {
                        for (int id : ids) m(id, 0) = v;
                    }
                    else {
                        // ★ 广播到这几行的所有列！
                        for (int id : ids)
                            for (int j = 0; j < m.getCols(); ++j) m(id, j) = v;
                    }
                }
                else if (val.isObjType(ObjType::REAL_MATRIX)) {
                    const auto& src = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                    auto srcFlat = src.rawData();

                    if (m.getRows() == 1 || m.getCols() == 1) {
                        // 纯向量赋值
                        if (static_cast<int>(srcFlat.size()) != static_cast<int>(ids.size()))
                            throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                        if (m.getRows() == 1) {
                            for (size_t k = 0; k < ids.size(); ++k) m(0, ids[k]) = srcFlat[k];
                        }
                        else {
                            for (size_t k = 0; k < ids.size(); ++k) m(ids[k], 0) = srcFlat[k];
                        }
                    }
                    else {
                        // 2D 矩阵的单维整行赋值（M[0:1] 意味着替换第0行的所有列）
                        if (static_cast<int>(srcFlat.size()) != static_cast<int>(ids.size()) * m.getCols())
                            throw std::runtime_error("VM Error: Slice assignment size mismatch for matrix row.");
                        for (size_t k = 0; k < ids.size(); ++k) {
                            for (int j = 0; j < m.getCols(); ++j) {
                                m(ids[k], j) = srcFlat[k * m.getCols() + j];
                            }
                        }
                    }
                }
                else {
                    throw std::runtime_error("VM Error: Cannot assign this type to slice.");
                }
            }
            else if (obj.isObjType(ObjType::LIST)) {
                auto list = static_cast<ObjList*>(obj.asObj());
                auto ids = buildSliceIndices(static_cast<int>(list->vec.size()), start, end, step);
                if (val.isObjType(ObjType::LIST)) {
                    const auto& srcL = static_cast<ObjList*>(val.asObj())->vec;
                    if (srcL.size() != ids.size())
                        throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                    for (size_t k = 0; k < ids.size(); ++k)
                        list->mut()[ids[k]] = srcL[k];
                }
                else {
                    for (int id : ids)
                        list->mut()[id] = val;
                }
            }
            else if (obj.isString()) {
                std::string s = obj.asString();
                auto ids = buildSliceIndices(static_cast<int>(s.size()), start, end, step);
                if (!val.isString())
                    throw std::runtime_error("VM Error: String slice assignment requires a string.");
                const auto& src = val.asString();
                if (static_cast<int>(src.size()) != static_cast<int>(ids.size()))
                    throw std::runtime_error("VM Error: String slice assignment size mismatch.");
                for (size_t k = 0; k < ids.size(); ++k) s[ids[k]] = src[k];
                obj = Value(s);
            }
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
                auto ids = buildSliceIndices(n, start, end, step);
                if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                    auto srcFlat = static_cast<ObjComplexMatrix*>(val.asObj())->mat.rawData();

                    if (m.getRows() == 1 || m.getCols() == 1) {
                        if (static_cast<int>(srcFlat.size()) != static_cast<int>(ids.size()))
                            throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                        if (m.getRows() == 1) {
                            for (size_t k = 0; k < ids.size(); ++k) m(0, ids[k]) = srcFlat[k];
                        }
                        else {
                            for (size_t k = 0; k < ids.size(); ++k) m(ids[k], 0) = srcFlat[k];
                        }
                    }
                    else {
                        if (static_cast<int>(srcFlat.size()) != static_cast<int>(ids.size()) * m.getCols())
                            throw std::runtime_error("VM Error: Slice assignment size mismatch for matrix row.");
                        for (size_t k = 0; k < ids.size(); ++k) {
                            for (int j = 0; j < m.getCols(); ++j) {
                                m(ids[k], j) = srcFlat[k * m.getCols() + j];
                            }
                        }
                    }
                }
                else {
                    Complex cv = val.asComplex();
                    if (m.getRows() == 1) {
                        for (int id : ids) m(0, id) = cv;
                    }
                    else if (m.getCols() == 1) {
                        for (int id : ids) m(id, 0) = cv;
                    }
                    else {
                        for (int id : ids)
                            for (int j = 0; j < m.getCols(); ++j) m(id, j) = cv;
                    }
                }
            }
            else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
                auto ids = buildSliceIndices(n, start, end, step);
                if (val.isObjType(ObjType::STRING_MATRIX)) {
                    auto srcFlat = static_cast<ObjStringMatrix*>(val.asObj())->mat.rawData();

                    if (m.getRows() == 1 || m.getCols() == 1) {
                        if (static_cast<int>(srcFlat.size()) != static_cast<int>(ids.size()))
                            throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                        if (m.getRows() == 1) {
                            for (size_t k = 0; k < ids.size(); ++k) m(0, ids[k]) = srcFlat[k];
                        }
                        else {
                            for (size_t k = 0; k < ids.size(); ++k) m(ids[k], 0) = srcFlat[k];
                        }
                    }
                    else {
                        if (static_cast<int>(srcFlat.size()) != static_cast<int>(ids.size()) * m.getCols())
                            throw std::runtime_error("VM Error: Slice assignment size mismatch for matrix row.");
                        for (size_t k = 0; k < ids.size(); ++k) {
                            for (int j = 0; j < m.getCols(); ++j) {
                                m(ids[k], j) = srcFlat[k * m.getCols() + j];
                            }
                        }
                    }
                }
                else {
                    std::string sv;
                    if (val.isString()) sv = val.asString();
                    else {
                        std::ostringstream oss;
                        if (val.isUninit()) oss << "Uninitialized";
                        else oss << val;
                        sv = oss.str();
                    }

                    if (m.getRows() == 1) {
                        for (int id : ids) m(0, id) = sv;
                    }
                    else if (m.getCols() == 1) {
                        for (int id : ids) m(id, 0) = sv;
                    }
                    else {
                        for (int id : ids)
                            for (int j = 0; j < m.getCols(); ++j) m(id, j) = sv;
                    }
                }
            }
            else {
                throw std::runtime_error("VM Error: Cannot slice-assign a value of type '" + getTypeName(obj) + "'.");
            }
            for (int i = 0; i < popCount; ++i) pop();
            push(val);
            push(obj);
        }
        else if (dims == 2) {
            Value val = peek(popCount++);
            auto cStep = readOptionalInt();
            auto cEnd = readOptionalInt();
            auto cStart = readOptionalInt();
            auto rStep = readOptionalInt();
            auto rEnd = readOptionalInt();
            auto rStart = readOptionalInt();
            Value obj = peek(popCount++);

            auto processMatSliceSet = [&](auto& m) {
                auto rIds = buildSliceIndices(m.getRows(), rStart, rEnd, rStep);
                auto cIds = buildSliceIndices(m.getCols(), cStart, cEnd, cStep);
                int dstR = static_cast<int>(rIds.size());
                int dstC = static_cast<int>(cIds.size());

                using ElemType = std::decay_t<decltype(m(0, 0))>;

                // 检测右值是否为一个矩阵
                bool isRhsMat = val.isObjType(ObjType::REAL_MATRIX) ||
                    val.isObjType(ObjType::COMPLEX_MATRIX) ||
                    val.isObjType(ObjType::STRING_MATRIX);

                if (isRhsMat) {
                    int srcR = 0, srcC = 0;
                    if (val.isObjType(ObjType::REAL_MATRIX)) {
                        srcR = static_cast<ObjRealMatrix*>(val.asObj())->mat.getRows();
                        srcC = static_cast<ObjRealMatrix*>(val.asObj())->mat.getCols();
                    }
                    else if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                        srcR = static_cast<ObjComplexMatrix*>(val.asObj())->mat.getRows();
                        srcC = static_cast<ObjComplexMatrix*>(val.asObj())->mat.getCols();
                    }
                    else {
                        srcR = static_cast<ObjStringMatrix*>(val.asObj())->mat.getRows();
                        srcC = static_cast<ObjStringMatrix*>(val.asObj())->mat.getCols();
                    }

                    if (srcR != dstR || srcC != dstC)
                        throw std::runtime_error("VM Error: Slice assignment size mismatch.");

                    for (int i = 0; i < dstR; ++i) {
                        for (int j = 0; j < dstC; ++j) {
                            if constexpr (std::is_same_v<ElemType, double>) {
                                if (val.isObjType(ObjType::REAL_MATRIX))
                                    m(rIds[i], cIds[j]) = static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j);
                                else
                                    throw std::runtime_error("VM Error: Cannot assign complex/string matrix to real matrix slice.");
                            }
                            else if constexpr (std::is_same_v<ElemType, Complex>) {
                                if (val.isObjType(ObjType::COMPLEX_MATRIX))
                                    m(rIds[i], cIds[j]) = static_cast<ObjComplexMatrix*>(val.asObj())->mat(i, j);
                                else if (val.isObjType(ObjType::REAL_MATRIX))
                                    m(rIds[i], cIds[j]) = Complex(static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j));
                                else
                                    throw std::runtime_error("VM Error: Cannot assign string matrix to complex matrix slice.");
                            }
                            else if constexpr (std::is_same_v<ElemType, std::string>) {
                                std::ostringstream oss;
                                if (val.isObjType(ObjType::STRING_MATRIX))
                                    oss << static_cast<ObjStringMatrix*>(val.asObj())->mat(i, j);
                                else if (val.isObjType(ObjType::COMPLEX_MATRIX))
                                    oss << Value(static_cast<ObjComplexMatrix*>(val.asObj())->mat(i, j));
                                else
                                    oss << Value(static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j));
                                m(rIds[i], cIds[j]) = oss.str();
                            }
                        }
                    }
                }
                else {
                    // 万能标量广播 (Scalar Broadcast)
                    ElemType scalarVal{};
                    if constexpr (std::is_same_v<ElemType, double>) {
                        scalarVal = val.asDouble(); // 可承接 int, double, fraction 等
                    }
                    else if constexpr (std::is_same_v<ElemType, Complex>) {
                        scalarVal = val.asComplex();
                    }
                    else if constexpr (std::is_same_v<ElemType, std::string>) {
                        if (val.isString())
                            scalarVal = val.asString();
                        else {
                            std::ostringstream oss;
                            if (val.isUninit()) oss << "Uninitialized";
                            else oss << val;
                            scalarVal = oss.str();
                        }
                    }

                    for (int ri : rIds)
                        for (int ci : cIds)
                            m(ri, ci) = scalarVal;
                }
                };

            if (obj.isObjType(ObjType::REAL_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
                processMatSliceSet(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
            }
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                processMatSliceSet(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
            }
            else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                processMatSliceSet(static_cast<ObjStringMatrix*>(obj.asObj())->mat);
            }
            else {
                throw std::runtime_error("VM Error: 2D slice assignment requires a matrix, got '" + getTypeName(obj) + "'.");
            }
            for (int i = 0; i < popCount; ++i) pop();
            push(val);
            push(obj);
        }
        else {
            throw std::runtime_error("VM Error: Unsupported slice assignment dimensionality.");
        }
        return;
    }

    void VM::execBuildMatrix(uint16_t shapeIdx) {
        const auto& shape = frame().function->chunk.matrixShapes[shapeIdx];
        uint16_t rows = shape.rows;
        const std::vector<uint16_t>& rowCols = shape.rowCols;

        int total = 0;
        for (uint16_t c : rowCols) total += c;

        bool hasComplex = false;
        bool hasString = false;
        bool hasOther = false;

        auto canBeMatrixElement = [](const Value& v) -> bool {
            return v.isNumber() ||
                v.isObjType(ObjType::BIGINT) ||
                v.isObjType(ObjType::FRACTION) ||
                v.isObjType(ObjType::BASENUM) ||
                v.isObjType(ObjType::COMPLEX) ||
                v.isString() ||
                v.isObjType(ObjType::REAL_MATRIX) ||
                v.isObjType(ObjType::COMPLEX_MATRIX) ||
                v.isObjType(ObjType::STRING_MATRIX);
            };

        for (int ii = 0; ii < total; ++ii) {
            const Value& v = stack[getStackSize() - total + ii];
            if (v.isObjType(ObjType::COMPLEX) ||
                v.isObjType(ObjType::COMPLEX_MATRIX))
                hasComplex = true;
            if (v.isString() ||
                v.isObjType(ObjType::STRING_MATRIX))
                hasString = true;
            if (!canBeMatrixElement(v))
                hasOther = true;
        }

        Value result;

        if (hasOther) {
            if (rows == 1) {
                ObjList* L = GcHeap::get().allocate<ObjList>();
                for (int ii = 0; ii < total; ++ii)
                    L->vec.push_back(stack[getStackSize() - total + ii]);
                result = Value(L);
            }
            else {
                ObjList* outer = GcHeap::get().allocate<ObjList>();
                int idx = 0;
                for (int i = 0; i < rows; ++i) {
                    ObjList* inner = GcHeap::get().allocate<ObjList>();
                    int cols = rowCols[i];
                    for (int j = 0; j < cols; ++j)
                        inner->vec.push_back(stack[getStackSize() - total + idx++]);
                    inner->is_frozen = true;
                    outer->vec.push_back(Value(inner));
                }
                result = Value(outer);
            }
        }
        else {
            bool hasSubMatrix = false;
            for (int ii = 0; ii < total; ++ii) {
                const Value& v = stack[getStackSize() - total + ii];
                if (v.isObjType(ObjType::REAL_MATRIX) ||
                    v.isObjType(ObjType::COMPLEX_MATRIX) ||
                    v.isObjType(ObjType::STRING_MATRIX))
                    hasSubMatrix = true;
            }

            if (hasSubMatrix) {
                auto extractCell = [&](Value& cell) {
                    if (!cell.isObjType(ObjType::REAL_MATRIX) &&
                        !cell.isObjType(ObjType::COMPLEX_MATRIX) &&
                        !cell.isObjType(ObjType::STRING_MATRIX)) {
                        if (hasString) {
                            std::ostringstream oss; oss << cell;
                            cell = Value(StringMatrix(1, 1, { oss.str() }));
                        }
                        else if (hasComplex) {
                            cell = Value(ComplexMatrix(1, 1, { cell.asComplex() }));
                        }
                        else {
                            cell = Value(RealMatrix(1, 1, { cell.asDouble() }));
                        }
                    }
                    if (hasString) {
                        if (cell.isObjType(ObjType::REAL_MATRIX)) {
                            const auto& m = static_cast<ObjRealMatrix*>(cell.asObj())->mat;
                            std::vector<std::string> flat;
                            for (int i = 0; i < m.getRows(); ++i)
                                for (int j = 0; j < m.getCols(); ++j) {
                                    std::ostringstream oss; oss << Value(m(i, j));
                                    flat.push_back(oss.str());
                                }
                            cell = Value(StringMatrix(m.getRows(), m.getCols(), flat));
                        }
                        else if (cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                            const auto& m = static_cast<ObjComplexMatrix*>(cell.asObj())->mat;
                            std::vector<std::string> flat;
                            for (int i = 0; i < m.getRows(); ++i)
                                for (int j = 0; j < m.getCols(); ++j) {
                                    std::ostringstream oss; oss << Value(m(i, j));
                                    flat.push_back(oss.str());
                                }
                            cell = Value(StringMatrix(m.getRows(), m.getCols(), flat));
                        }
                    }
                    else if (hasComplex && cell.isObjType(ObjType::REAL_MATRIX)) {
                        cell = Value(cell.asComplexMatrix());
                    }
                    };

                try {
                    int idx = 0;
                    Value matResult = Value::none();
                    for (int i = 0; i < rows; ++i) {
                        Value rowResult = Value::none();
                        int cols = rowCols[i];
                        for (int j = 0; j < cols; ++j) {
                            Value cell = stack[getStackSize() - total + idx++];
                            extractCell(cell);
                            if (rowResult.isNone()) {
                                rowResult = cell;
                            }
                            else {
                                if (hasString)
                                    rowResult = Value(static_cast<ObjStringMatrix*>(rowResult.asObj())->mat
                                        .integR(static_cast<ObjStringMatrix*>(cell.asObj())->mat));
                                else if (hasComplex)
                                    rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat
                                        .integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                                else
                                    rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat
                                        .integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                            }
                        }
                        if (matResult.isNone()) {
                            matResult = rowResult;
                        }
                        else {
                            if (hasString)
                                matResult = Value(static_cast<ObjStringMatrix*>(matResult.asObj())->mat
                                    .integC(static_cast<ObjStringMatrix*>(rowResult.asObj())->mat));
                            else if (hasComplex)
                                matResult = Value(static_cast<ObjComplexMatrix*>(matResult.asObj())->mat
                                    .integC(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat));
                            else
                                matResult = Value(static_cast<ObjRealMatrix*>(matResult.asObj())->mat
                                    .integC(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat));
                        }
                    }
                    result = matResult;
                }
                catch (...) {
                    throw std::runtime_error(
                        "VM Error: Dimension mismatch during block matrix concatenation.");
                }
            }
            else {
                int expectedCols = rows > 0 ? rowCols[0] : 0;
                bool uniformCols = true;
                for (int i = 1; i < rows; ++i) {
                    if (rowCols[i] != expectedCols) {
                        uniformCols = false;
                        break;
                    }
                }
                if (!uniformCols) {
                    throw std::runtime_error("VM Error: Matrix rows must have the same number of columns.");
                }

                if (hasString) {
                    std::vector<std::string> flat(total);
                    for (int ii = 0; ii < total; ++ii) {
                        const Value& v = stack[getStackSize() - total + ii];
                        if (v.isString())
                            flat[ii] = v.asString();
                        else {
                            std::ostringstream oss;
                            if (v.isUninit()) oss << "Uninitialized";
                            else oss << v;
                            flat[ii] = oss.str();
                        }
                    }
                    result = Value(StringMatrix(rows, expectedCols, flat));
                }
                else if (hasComplex) {
                    std::vector<Complex> flat(total);
                    for (int ii = 0; ii < total; ++ii)
                        flat[ii] = stack[getStackSize() - total + ii].asComplex();
                    result = Value(ComplexMatrix(rows, expectedCols, flat));
                }
                else {
                    std::vector<double> flat(total);
                    for (int ii = 0; ii < total; ++ii)
                        flat[ii] = stack[getStackSize() - total + ii].asDouble();
                    result = Value(RealMatrix(rows, expectedCols, flat));
                }
            }
        }

        for (int ii = 0; ii < total; ++ii) pop();
        push(result);
        return;
    }

    void VM::execIn() {
        Value haystack = peek(0);
        Value needle = peek(1);
        bool found = false;

        if (needle.isString() && haystack.isString()) {
            found = haystack.asString().find(needle.asString()) != std::string::npos;
        }
        else if (haystack.isString()) {
            throw std::runtime_error(
                "VM Error: 'in' on string requires a string on the left side.");
        }
        else if (haystack.isObjType(ObjType::REAL_MATRIX)) {
            const auto& m = static_cast<ObjRealMatrix*>(haystack.asObj())->mat;
            double target;
            try { 
                target = needle.asDouble(); 
                for (const auto& v : m.rawData()) {
                    if (v == target) { found = true; break; }
                }
            }
            catch (...) { /* found remains false */ }
        }
        else if (haystack.isObjType(ObjType::COMPLEX_MATRIX)) {
            const auto& m = static_cast<ObjComplexMatrix*>(haystack.asObj())->mat;
            Complex target;
            try { 
                target = needle.asComplex(); 
                for (const auto& v : m.rawData()) {
                    if (v == target) { found = true; break; }
                }
            }
            catch (...) { /* found remains false */ }
        }
        else if (haystack.isObjType(ObjType::STRING_MATRIX)) {
            if (!needle.isString())
                throw std::runtime_error(
                    "VM Error: 'in' on StringMatrix requires a string needle.");
            const auto& m = static_cast<ObjStringMatrix*>(haystack.asObj())->mat;
            const auto& target = needle.asString();
            for (const auto& v : m.rawData()) {
                if (v == target) { found = true; break; }
            }
        }
        else if (haystack.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(haystack.asObj())->vec;
            for (const auto& e : L) {
                try {
                    if (Value::equals(needle, e)) {
                        found = true;
                        break;
                    }
                }
                catch (...) {}
            }
        }
        else if (haystack.isObjType(ObjType::DICT)) {
            auto d = static_cast<ObjDict*>(haystack.asObj());
            found = d->keyMap.find(needle) != d->keyMap.end();
        }
        else if (haystack.isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(haystack.asObj());
            if (needle.isString()) {
                found = ns->fields.find(needle.asString()) != ns->fields.end();
            }
        }
        else if (haystack.isObjType(ObjType::SET)) {
            auto s = static_cast<ObjSet*>(haystack.asObj());
            found = s->keys.find(needle) != s->keys.end();
        }
        else if (haystack.isInstance()) {
            auto method = findDunder(haystack, DUNDER_CONTAINS);
            if (method) {
                found = callDunder(haystack, DUNDER_CONTAINS, { needle }).truthy();
            } else {
                auto inst = haystack.asInstance();
                if (inst->fields && inst->fields->keyMap.find(needle) != inst->fields->keyMap.end()) {
                    found = true;
                } else if (needle.isString()) {
                    auto c = inst->classDef;
                    std::string key = needle.asString();
                    while (c) {
                        if (c->methods.find(key) != c->methods.end()) {
                            found = true;
                            break;
                        }
                        c = c->parent;
                    }
                    if (!found) {
                        auto getattrMethod = findDunder(haystack, DUNDER_GETATTR);
                        if (getattrMethod) {
                            try {
                                callDunder(haystack, DUNDER_GETATTR, { needle });
                                found = true;
                            } catch (...) {
                                // Fall through to false
                            }
                        }
                    }
                }
            }
        }
        else {
            throw std::runtime_error(
                "VM Error: 'in' requires an array, vector, matrix, string, list, dict, or instance, got '" + getTypeName(haystack) + "'.");
        }

        pop(); pop();
        push(Value(found));
    }

    // VM.cpp 中的实现：
    Value VM::execReturn(bool& shouldExit) {
        shouldExit = false;
        Value result = pop();
        int base = frame().stackBase;
        std::string fnName = frame().function->name;

        // ★ 核心：记录下属于当前自身心跳的上下文
        Value activeSelf = frame().selfContext;

        while (!exceptionHandlers.empty() &&
            exceptionHandlers.back().frameIndex == frameCount - 1) {
            exceptionHandlers.pop_back();
        }

        frames[frameCount - 1].selfContext = Value::none();
        frames[frameCount - 1].classContext = Value::none();
        frames[frameCount - 1].closure = nullptr;
        frames[frameCount - 1].refParamsBase = -1;
        frameCount--;

        // ★ 退出判定
        if (frameCount <= currentTargetFrameDepth) {
            if (currentTargetFrameDepth == 0) {
                closeUpvalues(0);
                setStackSize(0);
            }
            else {
                closeUpvalues(base);
                setStackSize(base);
            }
            shouldExit = true;  // 通知 run() 退出
            return result;
        }

        closeUpvalues(base);
        setStackSize(base);

        // ★ 唯独构造函数返回时做个特判：如果你调用了 init()，VM 会默默返回正在创建的对象
        if (fnName == "init") {
            push(activeSelf.isNone() ? result : activeSelf);
        }
        else {
            push(result);
        }
        return Value::none();
    }

    void VM::execInvoke(uint8_t argc, uint16_t icIdx, bool isTailCall) {
        struct CallRefGuard { VM* vm; ~CallRefGuard() { vm->pendingCallRefs.clear(); } } guard{this};
        InlineCache& ic = const_cast<InlineCache&>(frame().function->chunk.inlineCaches[icIdx]);
        uint16_t nameIdx = ic.nameIdx;
        const std::string& methodName = frame().function->chunk.constants[nameIdx].asString();
        Value obj = stack[getStackSize() - 1 - argc];

        ObjClosure* method = nullptr;
        ObjClass* owningClass = nullptr;

        // ==============================================================
        // 1. 如果它是原生 Dict！我们要像对待对象一样去调用它内部的闭包
        // ==============================================================
        if (obj.isObjType(ObjType::DICT)) {
            auto d = static_cast<ObjDict*>(obj.asObj());
            auto it = d->keyMap.find(frame().function->chunk.constants[nameIdx]);
            if (it != d->keyMap.end()) {
                Value fv = d->elements[it->second].second;
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                } else {
                    // ★ 如果是类、实例或其他可调用对象，直接替换栈底的 obj，转交 execCall 处理！
                    stack[getStackSize() - 1 - argc] = fv;
                    execCall(argc);
                    return;
                }
            }
        }
        // ==============================================================
        // 1.5 如果它是 Namespace！
        // ==============================================================
        else if (obj.isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(obj.asObj());
            auto it = ns->fields.find(methodName);
            if (it != ns->fields.end()) {
                Value fv = *(it->second.upval->location);
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                } else {
                    stack[getStackSize() - 1 - argc] = fv;
                    execCall(argc);
                    return;
                }
            }
        }
        // ==============================================================
        // 2. 经典面向对象 Instance 的方法查询（优先实例字段，后查类模板）
        // ==============================================================
        else if (obj.isInstance()) {
            auto inst = obj.asInstance();
            bool foundInField = false;

            // ★ IC 命中检查
            if (ic.cachedClass == inst->classDef && ic.cachedMethod) {
                if (!inst->fields || inst->fields->keyMap.find(frame().function->chunk.constants[nameIdx]) == inst->fields->keyMap.end()) {
                    method = ic.cachedMethod;
                    owningClass = ic.cachedClass;
                    goto invoke_method;
                }
            }

            // 2.1 优先查找实例自身的字段 (Fields)
            if (inst->fields) {
                auto it = inst->fields->keyMap.find(frame().function->chunk.constants[nameIdx]);
                if (it != inst->fields->keyMap.end()) {
                    Value fv = inst->fields->elements[it->second].second;
                    if (fv.isFunctionClosure()) {
                        method = fv.asFunction();
                        owningClass = inst->classDef;
                        foundInField = true;
                    } else {
                        // ★ 如果实例字段里存的是类或其他可调用对象
                        stack[getStackSize() - 1 - argc] = fv;
                        execCall(argc);
                        return;
                    }
                }
            }

            // 2.2 如果字段里没找到，再顺着类继承链查找方法 (Class Methods)
            if (!foundInField) {
                auto c = inst->classDef;
                while (c) {
                    auto it = c->methods.find(methodName);
                    if (it != c->methods.end()) {
                        method = it->second;
                        owningClass = c;
                        break;
                    }
                    c = c->parent;
                }
                
                if (method) {
                    ic.cachedClass = inst->classDef;
                    ic.cachedMethod = method;
                }

                // 2.3 如果类方法也没找到，尝试 __getattr__
                if (!method) {
                    auto getattrMethod = findDunder(obj, DUNDER_GETATTR);
                    if (getattrMethod) {
                        Value fv = callDunder(obj, DUNDER_GETATTR, { Value(methodName) });
                        if (fv.isFunctionClosure()) {
                            method = fv.asFunction();
                            owningClass = inst->classDef;
                        } else {
                            stack[getStackSize() - 1 - argc] = fv;
                            execCall(argc);
                            return;
                        }
                    }
                }
            }
        }

    invoke_method:
        // ==============================================================
        // ★ UFCS Fallback: 允许内置类型像对象一样调用全局函数
        // ==============================================================
        if (!method) {
            auto nIt = nativeBuiltins.find(methodName);
            if (nIt != nativeBuiltins.end()) {
                auto ait = builtinArity.find(methodName);
                int totalArgs = argc + 1;
                if (ait != builtinArity.end() && !ait->second.empty() && ait->second.find(totalArgs) == ait->second.end()) {
                    std::string expected;
                    for (auto aIt = ait->second.begin(); aIt != ait->second.end(); ++aIt) {
                        if (aIt != ait->second.begin()) expected += " or ";
                        expected += std::to_string(*aIt - 1);
                    }
                    throw std::runtime_error("Runtime Error: Method '" + methodName + "' expects " + expected + " arguments, got " + std::to_string(argc) + ".");
                }

                std::vector<Value> argsVec(totalArgs);
                for (int j = 0; j < argc; ++j) argsVec[j + 1] = peek(argc - 1 - j);
                argsVec[0] = peek(argc); // obj
                Value result = nIt->second(argsVec);
                for (int j = 0; j <= argc; ++j) pop();
                push(result);
                return;
            }
            auto gIt = globalNamesToSlots.find(methodName);
            if (gIt != globalNamesToSlots.end() && globalValues[gIt->second].isFunctionClosure()) {
                if (static_cast<int>(getStackSize()) >= MAX_STACK) throw std::runtime_error("VM Error: Stack overflow.");
                insertStack(argc + 1, globalValues[gIt->second]); // ★ FIX: 插入点上方有 argc + 1 个元素 (obj + args)
                for (auto& pr : pendingCallRefs) pr.first += 1; // ★ UFCS 引用参数索引右移
                execCall(argc + 1);
                return;
            }

            if (obj.isInstance()) throw std::runtime_error("VM Error: No method '" + methodName + "' on instances of class '" + obj.asInstance()->classDef->name + "'.");
            if (obj.isObjType(ObjType::DICT)) throw std::runtime_error("VM Error: No callable field '" + methodName + "' in Dict.");
            if (obj.isObjType(ObjType::NAMESPACE)) throw std::runtime_error("VM Error: No callable field '" + methodName + "' in namespace.");
            throw std::runtime_error("VM Error: Cannot invoke method '" + methodName + "' on this type.");
        }

        // ==============================================================
        // ★ 核心方法执行引擎：此时的 obj 不论是 Dict 还是 Instance，
        // 都会被公平地当做 `self` 注入环境！
        // ==============================================================
        if (method->isBytecode()) {
            CallFrame newFrame;
            // ★ Magic: 跨过 globals 的直接帧级注入！
            newFrame.selfContext = obj;
            newFrame.classContext = owningClass ? Value(owningClass) : Value::none();
            auto& fnDef = compiledFunctions[method->compiledFnIndex];

            if (fnDef->hasRestParam) {
                int fixedMax = fnDef->maxArity - 1;
                if (static_cast<int>(argc) < fnDef->arity) {
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                }

                ObjList* restList = GcHeap::get().allocate<ObjList>();
                if (static_cast<int>(argc) > fixedMax) {
                    int restCount = static_cast<int>(argc) - fixedMax;
                    std::vector<Value> tempValues(restCount);
                    for (int j = 0; j < restCount; j++) tempValues[restCount - 1 - j] = pop();
                    for (int j = 0; j < restCount; j++) restList->vec.push_back(tempValues[j]);
                    argc = static_cast<uint8_t>(fixedMax);
                }

                int padCount = fixedMax - static_cast<int>(argc);
                for (int j = 0; j < padCount; ++j) push(Value::none());
                push(Value(restList));
            }
            else {
                if (static_cast<int>(argc) < fnDef->arity || static_cast<int>(argc) > fnDef->maxArity)
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(argc) + ".");
                int padCount = fnDef->maxArity - static_cast<int>(argc);
                for (int j = 0; j < padCount; ++j) push(Value::none());
            }

            int reserveCount = fnDef->localCount - fnDef->maxArity;
            for (int j = 0; j < reserveCount; ++j) push(Value::none());
            
            eraseStack(fnDef->localCount); // ★ FIX: 延迟移除 obj，保护其在可能触发的 GC 中存活
            
            if (isTailCall) {
                int base = frame().stackBase;
                closeUpvalues(base);
                int newLocalCount = fnDef->localCount;
                int argStart = static_cast<int>(getStackSize()) - newLocalCount;
                if (base != argStart) {
                    for (int i = 0; i < newLocalCount; ++i) {
                        stack[base + i] = std::move(stack[argStart + i]);
                    }
                }
                setStackSize(base + newLocalCount);
                frame().function = fnDef.get();
                frame().ip = 0;
                frame().closure = method;
                frame().selfContext = obj;
                frame().classContext = owningClass ? Value(owningClass) : Value::none();
                populateRefParams(frame(), fnDef.get());
                return;
            }

            newFrame.function = fnDef.get();
            newFrame.ip = 0;
            newFrame.stackBase = static_cast<int>(getStackSize()) - fnDef->localCount;
            newFrame.closure = method;
            populateRefParams(newFrame, fnDef.get());
            if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
            frames[frameCount++] = newFrame;
            return;
        }
        else if (method->isNative()) {
            // ★ 修复：检查 nativeFn 是否为晚绑定字符串标签
            if (method->nativeFn.type() == typeid(std::string)) {
                const std::string& tag = std::any_cast<std::string>(method->nativeFn);
                if (tag.size() >= 5 && tag.substr(0, 5) == "__fn:") {
                    int fnIdx = std::stoi(tag.substr(5));
                    auto& fnDef = compiledFunctions[fnIdx];

                    if (fnDef->hasRestParam) {
                        int fixedMax = fnDef->maxArity - 1;
                        if (static_cast<int>(argc) < fnDef->arity) {
                            throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                        }
                        ObjList* restList = GcHeap::get().allocate<ObjList>();
                        if (static_cast<int>(argc) > fixedMax) {
                            int restCount = static_cast<int>(argc) - fixedMax;
                            std::vector<Value> tempValues(restCount);
                            for (int j = 0; j < restCount; j++) tempValues[restCount - 1 - j] = pop();
                            for (int j = 0; j < restCount; j++) restList->vec.push_back(tempValues[j]);
                            argc = static_cast<uint8_t>(fixedMax);
                        }
                        int padCount = fixedMax - static_cast<int>(argc);
                        for (int j = 0; j < padCount; ++j) push(Value::none());
                        push(Value(restList));
                    }
                    else {
                        if (static_cast<int>(argc) < fnDef->arity || static_cast<int>(argc) > fnDef->maxArity)
                            throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(argc) + ".");
                        int padCount = fnDef->maxArity - static_cast<int>(argc);
                        for (int j = 0; j < padCount; ++j) push(Value::none());
                    }

                    int reserveCount = fnDef->localCount - fnDef->maxArity;
                    for (int j = 0; j < reserveCount; ++j) push(Value::none());

                    eraseStack(fnDef->localCount); // ★ FIX: 延迟移除 obj，保护其在可能触发的 GC 中存活

                    CallFrame newFrame;
                    newFrame.function = fnDef.get();
                    newFrame.ip = 0;
                    newFrame.stackBase = static_cast<int>(getStackSize()) - fnDef->localCount;
                    newFrame.selfContext = obj;
                    newFrame.classContext = owningClass ? Value(owningClass) : Value::none();
                    populateRefParams(newFrame, fnDef.get());
                    if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                    frames[frameCount++] = newFrame;
                    return;
                }
                if (tag.size() >= 10 && tag.substr(0, 10) == "__builtin:") {
                    std::string fnName = tag.substr(10);
                    std::vector<Value> argsVec(argc);
                    for (int j = 0; j < argc; ++j) argsVec[j] = peek(argc - 1 - j);
                    auto nit = nativeBuiltins.find(fnName);
                    if (nit == nativeBuiltins.end()) throw std::runtime_error("VM Error: Unknown builtin '" + fnName + "'.");
                    Value result = nit->second(argsVec);
                    for (int j = 0; j <= argc; ++j) pop();
                    push(result);
                    return;
                }
                throw std::runtime_error("VM Error: Invalid string tag in method.");
            }

            // ★ 修复：检查原生方法的参数数量
            if (method->maxArgs() > 0 && !method->hasRestParam) {
                if (static_cast<int>(argc) < method->minArgs() || static_cast<int>(argc) > method->maxArgs()) {
                    throw std::runtime_error("Runtime Error: Method '" + methodName + 
                        "' expects " + std::to_string(method->minArgs()) + " to " + 
                        std::to_string(method->maxArgs()) + " arguments, got " + 
                        std::to_string(argc) + ".");
                }
            }

            // ★ C++ 原生函数直接进隔离池
            helpers::nativeSelfStack.push_back(obj);
            helpers::nativeClassStack.push_back(owningClass ? Value(owningClass) : Value::none());

            std::vector<Value> args(argc);
            for (int j = 0; j < argc; ++j) args[j] = peek(argc - 1 - j);
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
            for (int j = 0; j <= argc; ++j) pop();
            push(result);
            return;
        }

        throw std::runtime_error("VM Error: Method '" + methodName +
            "' has no callable implementation.");
    }

    void VM::execSuperInvoke(uint16_t nameIdx, uint8_t argc, bool isTailCall) {
        struct CallRefGuard { VM* vm; ~CallRefGuard() { vm->pendingCallRefs.clear(); } } guard{this};
        const std::string& methodName = frame().function->chunk.constants[nameIdx].asString();
        Value selfVal = stack[getStackSize() - 1 - argc];
        if (!selfVal.isInstance())
            throw std::runtime_error("VM Error: 'super' requires an instance context.");
        auto inst = selfVal.asInstance();
        // ★ FIX: 直接从当前函数的帧寄存器提取！
        Value classVal = frame().classContext;
        if (!classVal.isClass())
            throw std::runtime_error("VM Error: 'super' requires class context (__class__).");
        auto currentClass = static_cast<ObjClass*>(classVal.asObj());
        auto parentClass = currentClass->parent;
        if (!parentClass)
            throw std::runtime_error("VM Error: Class '" + currentClass->name +
                "' has no parent class.");

        ObjClosure* method = nullptr;
        ObjClass* owningClass = nullptr;
        auto c = parentClass;
        while (c) {
            auto it = c->methods.find(methodName);
            if (it != c->methods.end()) {
                method = it->second;
                owningClass = c;
                break;
            }
            c = c->parent;
        }
        if (!method)
            throw std::runtime_error("VM Error: Parent class has no method '" +
                methodName + "'.");

        // ★ 不在这里赋值！

        if (method->isBytecode()) {
            CallFrame newFrame;
            newFrame.selfContext = Value(inst);         
            newFrame.classContext = Value(owningClass); 

            auto& fnDef = compiledFunctions[method->compiledFnIndex];

            // =============================================================
            // ★ 核心变长参数打包引擎 (OOP SuperInvoke 端)
            // =============================================================
            if (fnDef->hasRestParam) {
                int fixedMax = fnDef->maxArity - 1;
                if (static_cast<int>(argc) < fnDef->arity) {
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                }

                ObjList* restList = GcHeap::get().allocate<ObjList>();
                if (static_cast<int>(argc) > fixedMax) {
                    int restCount = static_cast<int>(argc) - fixedMax;
                    std::vector<Value> tempValues(restCount);
                    for (int j = 0; j < restCount; j++) {
                        tempValues[restCount - 1 - j] = pop();
                    }
                    for (int j = 0; j < restCount; j++) {
                        restList->vec.push_back(tempValues[j]);
                    }
                    argc = static_cast<uint8_t>(fixedMax);
                }

                int padCount = fixedMax - static_cast<int>(argc);
                for (int j = 0; j < padCount; ++j) push(Value::none());
                push(Value(restList));
            }
            else {
                if (static_cast<int>(argc) < fnDef->arity || static_cast<int>(argc) > fnDef->maxArity)
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(argc) + ".");
                int padCount = fnDef->maxArity - static_cast<int>(argc);
                for (int j = 0; j < padCount; ++j) push(Value::none());
            }

            int reserveCount = fnDef->localCount - fnDef->maxArity;
            for (int j = 0; j < reserveCount; ++j) push(Value::none());

            eraseStack(fnDef->localCount); // ★ FIX: 延迟移除 selfVal，保护其在可能触发的 GC 中存活

            if (isTailCall) {
                int base = frame().stackBase;
                closeUpvalues(base);
                int newLocalCount = fnDef->localCount;
                int argStart = static_cast<int>(getStackSize()) - newLocalCount;
                if (base != argStart) {
                    for (int i = 0; i < newLocalCount; ++i) {
                        stack[base + i] = std::move(stack[argStart + i]);
                    }
                }
                setStackSize(base + newLocalCount);
                frame().function = fnDef.get();
                frame().ip = 0;
                frame().closure = method;
                frame().selfContext = Value(inst);
                frame().classContext = Value(owningClass);
                populateRefParams(frame(), fnDef.get());
                return;
            }

            newFrame.function = fnDef.get();
            newFrame.ip = 0;
            newFrame.stackBase = static_cast<int>(getStackSize()) - fnDef->localCount;
            newFrame.closure = method;
            populateRefParams(newFrame, fnDef.get());
            if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
            frames[frameCount++] = newFrame;
            return;
        }
        else if (method->isNative()) {
            // ★ 修复：检查原生方法的参数数量
            if (method->maxArgs() > 0 && !method->hasRestParam) {
                if (static_cast<int>(argc) < method->minArgs() || static_cast<int>(argc) > method->maxArgs()) {
                    throw std::runtime_error("Runtime Error: Super method '" + methodName + 
                        "' expects " + std::to_string(method->minArgs()) + " to " + 
                        std::to_string(method->maxArgs()) + " arguments, got " + 
                        std::to_string(argc) + ".");
                }
            }

            // ★ 压入原生方法隔离池
            helpers::nativeSelfStack.push_back(Value(inst));
            helpers::nativeClassStack.push_back(Value(owningClass));

            std::vector<Value> args(argc);
            for (int j = 0; j < argc; ++j) args[j] = peek(argc - 1 - j);
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
            for (int j = 0; j <= argc; ++j) pop();
            push(result);
            return;
        }

        throw std::runtime_error("VM Error: Parent method '" + methodName +
            "' has no callable implementation.");
    }

    void VM::execAssertParamType(const Value& val, uint16_t typeIdx, uint16_t nameIdx) {
        const std::string& expectedType = frame().function->chunk.constants[typeIdx].asString();

        if (!checkValueType(val, expectedType)) {
            const std::string& paramName = frame().function->chunk.constants[nameIdx].asString();
            throw std::runtime_error("TypeError: Parameter '" + paramName +
                "' expected type '" + expectedType +
                "', got '" + getTypeName(val) + "'.");
        }
    }

    void VM::execAssertReturnType(const Value& val, uint16_t typeIdx) {
        const std::string& expectedType = frame().function->chunk.constants[typeIdx].asString();

        if (!checkValueType(val, expectedType)) {
            throw std::runtime_error("TypeError: Function '" + frame().function->name +
                "' expected to return '" + expectedType +
                "', but returned '" + getTypeName(val) + "'.");
        }
    }

} // namespace jc
