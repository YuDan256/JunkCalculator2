#include "VM.h"
#include "../../memory/GcHeap.h"
#include "../../frontend/Utf8.h"
#include "../../frontend/Lexer.h"
#include "../../frontend/Parser.h"
#include "../../frontend/Compiler.h"
#include "../../vm/VM.h" // For ValueException
#include "../../vm/BuiltinRegistry.h"
#include "../../modules/ExtensionBridge.h"
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#undef IN
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace jc {
namespace regvm {

ObjUpVal* VM::captureUpvalue(int regIndex) {
    ObjUpVal* prevUpval = nullptr;
    ObjUpVal* upval = openUpvalues;
    while (upval != nullptr && upval->stackIndex > regIndex) {
        prevUpval = upval;
        upval = upval->nextOpen;
    }

    if (upval != nullptr && upval->stackIndex == regIndex) {
        return upval;
    }

    ObjUpVal* createdUpval = GcHeap::get().allocate<ObjUpVal>();
    createdUpval->location = &registers[regIndex];
    createdUpval->stackIndex = regIndex;
    createdUpval->nextOpen = upval;

    if (prevUpval == nullptr) {
        openUpvalues = createdUpval;
    } else {
        prevUpval->nextOpen = createdUpval;
    }

    return createdUpval;
}

void VM::closeUpvalues(int lastRegIndex) {
    while (openUpvalues != nullptr && openUpvalues->stackIndex >= lastRegIndex) {
        ObjUpVal* upval = openUpvalues;
        upval->closed = *upval->location;
        upval->location = &upval->closed;
        openUpvalues = upval->nextOpen;
    }
}

void VM::populateRefParams(CallFrame& newFrame, const CompiledFunction* fn) {
    if (fn->refCount == 0) {
        newFrame.refParamsBase = -1;
        pendingCallRefs.clear();
        return;
    }

    // 在寄存器窗口末尾分配引用参数槽
    newFrame.refParamsBase = newFrame.registerBase + fn->localCount;
    
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
                registers[newFrame.refParamsBase + refIdx] = Value(providedRef);
            } else {
                ObjUpVal* dummy = GcHeap::get().allocate<ObjUpVal>();
                dummy->location = &registers[newFrame.registerBase + i];
                registers[newFrame.refParamsBase + refIdx] = Value(dummy);
            }
            refIdx++;
        }
    }
    pendingCallRefs.clear();
}

void VM::execCall(int a, int b, bool isTailCall) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    Value callee = registers[currentFrame->registerBase + a];
    int argc = b;
    
    if (callee.isString()) {
        const std::string& tag = callee.asString();
        auto nIt = jc::VM::activeVM->getNativeBuiltins().find(tag);
        if (nIt != jc::VM::activeVM->getNativeBuiltins().end()) {
            std::vector<Value> args;
            args.reserve(argc);
            for (int i = 0; i < argc; ++i) {
                args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
            }
            registers[currentFrame->registerBase + a] = nIt->second(args);
            return;
        }
        auto it = globalNames.find(tag);
        if (it != globalNames.end()) {
            callee = globals[it->second];
            registers[currentFrame->registerBase + a] = callee;
        } else {
            throw std::runtime_error("RegVM Error: Unknown function or not callable '" + tag + "()'.");
        }
    }

    if (callee.isFunctionClosure()) {
        auto closure = callee.asFunction();
        if (closure->isBytecode()) {
            auto& fnDef = compiledFunctions[closure->compiledFnIndex];
            
            std::vector<Value> actualArgs;
            if (closure->isUFCS) {
                actualArgs.push_back(closure->boundSelf);
                for (auto& pr : pendingCallRefs) {
                    pr.first += 1;
                }
            }
            for (int i = 0; i < argc; ++i) {
                actualArgs.push_back(registers[currentFrame->registerBase + a + 1 + i]);
            }
            int totalArgc = static_cast<int>(actualArgs.size());

            if (fnDef->hasRestParam) {
                int fixedMax = fnDef->maxArity - 1;
                if (totalArgc < fnDef->arity) {
                    throw std::runtime_error("RegVM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                }
                ObjList* restList = GcHeap::get().allocate<ObjList>();
                if (totalArgc > fixedMax) {
                    int restCount = totalArgc - fixedMax;
                    restList->vec.resize(restCount);
                    for (int j = 0; j < restCount; j++) {
                        restList->vec[j] = actualArgs[fixedMax + j];
                    }
                    actualArgs.resize(fixedMax);
                }
                while (actualArgs.size() < static_cast<size_t>(fixedMax)) actualArgs.push_back(Value::uninit());
                actualArgs.push_back(Value(restList));
            } else {
                if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                    throw std::runtime_error("RegVM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
                }
                while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
            }

            if (isTailCall) {
                while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                    exceptionHandlers.pop_back();
                }
                closeUpvalues(currentFrame->registerBase);
                currentFrame->function = fnDef.get();
                currentFrame->chunk = &fnDef->chunk;
                currentFrame->ip = 0;
                currentFrame->closure = closure;
                currentFrame->selfContext = closure->boundSelf;
                currentFrame->classContext = closure->boundClass;
                
                for (size_t i = 0; i < actualArgs.size(); ++i) {
                    registers[currentFrame->registerBase + i] = actualArgs[i];
                }
                for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                    registers[currentFrame->registerBase + i] = Value::none();
                }
                
                populateRefParams(*currentFrame, fnDef.get());
                return;
            }
            
            CallFrame newFrame;
            newFrame.function = fnDef.get();
            newFrame.chunk = &fnDef->chunk;
            newFrame.ip = 0;
            newFrame.registerBase = currentFrame->registerBase + a + 1;
            newFrame.returnRegister = a;
            newFrame.closure = closure;
            newFrame.selfContext = closure->boundSelf;
            newFrame.classContext = closure->boundClass;
            
            for (size_t i = 0; i < actualArgs.size(); ++i) {
                registers[newFrame.registerBase + i] = actualArgs[i];
            }
            for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                registers[newFrame.registerBase + i] = Value::none();
            }
            
            populateRefParams(newFrame, fnDef.get());
            
            if (frameCount >= MAX_FRAMES) throw std::runtime_error("RegVM Error: CallFrame stack overflow.");
            frames[frameCount++] = newFrame;
        } else if (closure->isNative()) {
            auto ait = jc::VM::activeVM->getBuiltinArity().find(closure->rawBody);
            if (ait != jc::VM::activeVM->getBuiltinArity().end() && !ait->second.empty()) {
                if (ait->second.find(argc) == ait->second.end()) {
                    std::string expected;
                    for (auto aIt = ait->second.begin(); aIt != ait->second.end(); ++aIt) {
                        if (aIt != ait->second.begin()) expected += " or ";
                        expected += std::to_string(*aIt);
                    }
                    throw std::runtime_error("Runtime Error: Function '" + closure->rawBody + 
                        "' expects " + expected + " arguments, got " + std::to_string(argc) + ".");
                }
            } else if (static_cast<int>(closure->maxArgs()) > 0 && !closure->hasRestParam) {
                if (argc < static_cast<int>(closure->minArgs()) || argc > static_cast<int>(closure->maxArgs())) {
                    throw std::runtime_error("Runtime Error: Function '" + closure->rawBody + 
                        "' expects " + std::to_string(closure->minArgs()) + " to " + 
                        std::to_string(closure->maxArgs()) + " arguments, got " + 
                        std::to_string(argc) + ".");
                }
            }

            helpers::nativeSelfStack.push_back(closure->boundSelf);
            helpers::nativeClassStack.push_back(closure->boundClass);
            std::vector<Value> args;
            args.reserve(argc);
            for (int i = 0; i < argc; ++i) {
                args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
            }
            try {
                auto& fn = std::any_cast<NativeCallable&>(closure->nativeFn);
                registers[currentFrame->registerBase + a] = fn(args);
            } catch (...) {
                helpers::nativeSelfStack.pop_back();
                helpers::nativeClassStack.pop_back();
                throw;
            }
            helpers::nativeSelfStack.pop_back();
            helpers::nativeClassStack.pop_back();
        }
    } else if (callee.isClass()) {
        auto cls = static_cast<ObjClass*>(callee.asObj());
        auto instance = GcHeap::get().allocate<ObjInstance>();
        instance->classDef = cls;
        
        ObjClosure* initMethod = nullptr;
        auto c = cls;
        while (c) {
            auto it = c->methods.find("init");
            if (it != c->methods.end()) {
                initMethod = it->second;
                break;
            }
            c = c->parent;
        }
        
        if (initMethod) {
            if (initMethod->isBytecode()) {
                auto& fnDef = compiledFunctions[initMethod->compiledFnIndex];
                
                std::vector<Value> actualArgs;
                for (int i = 0; i < argc; ++i) {
                    actualArgs.push_back(registers[currentFrame->registerBase + a + 1 + i]);
                }
                int totalArgc = static_cast<int>(actualArgs.size());

                if (fnDef->hasRestParam) {
                    int fixedMax = fnDef->maxArity - 1;
                    if (totalArgc < fnDef->arity) {
                        throw std::runtime_error("RegVM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                    }
                    ObjList* restList = GcHeap::get().allocate<ObjList>();
                    if (totalArgc > fixedMax) {
                        int restCount = totalArgc - fixedMax;
                        restList->vec.resize(restCount);
                        for (int j = 0; j < restCount; j++) {
                            restList->vec[j] = actualArgs[fixedMax + j];
                        }
                        actualArgs.resize(fixedMax);
                    }
                    while (actualArgs.size() < static_cast<size_t>(fixedMax)) actualArgs.push_back(Value::uninit());
                    actualArgs.push_back(Value(restList));
                } else {
                    if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                        throw std::runtime_error("RegVM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
                    }
                    while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
                }

                if (isTailCall) {
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(currentFrame->registerBase);
                    currentFrame->function = fnDef.get();
                    currentFrame->chunk = &fnDef->chunk;
                    currentFrame->ip = 0;
                    currentFrame->closure = initMethod;
                    currentFrame->selfContext = Value(instance);
                    currentFrame->classContext = Value(cls);
                    
                    for (size_t i = 0; i < actualArgs.size(); ++i) {
                        registers[currentFrame->registerBase + i] = actualArgs[i];
                    }
                    for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                        registers[currentFrame->registerBase + i] = Value::none();
                    }
                    
                    populateRefParams(*currentFrame, fnDef.get());
                    return;
                }

                CallFrame newFrame;
                newFrame.function = fnDef.get();
                newFrame.chunk = &fnDef->chunk;
                newFrame.ip = 0;
                newFrame.registerBase = currentFrame->registerBase + a + 1;
                newFrame.returnRegister = a;
                newFrame.closure = initMethod;
                newFrame.selfContext = Value(instance);
                newFrame.classContext = Value(cls);
                
                for (size_t i = 0; i < actualArgs.size(); ++i) {
                    registers[newFrame.registerBase + i] = actualArgs[i];
                }
                for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                    registers[newFrame.registerBase + i] = Value::none();
                }
                
                populateRefParams(newFrame, fnDef.get());
                
                if (frameCount >= MAX_FRAMES) throw std::runtime_error("RegVM Error: CallFrame stack overflow.");
                frames[frameCount++] = newFrame;
            } else if (initMethod->isNative()) {
                helpers::nativeSelfStack.push_back(Value(instance));
                helpers::nativeClassStack.push_back(Value(cls));
                std::vector<Value> args;
                args.reserve(argc);
                for (int i = 0; i < argc; ++i) args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
                try {
                    auto& fn = std::any_cast<NativeCallable&>(initMethod->nativeFn);
                    fn(args);
                } catch (...) {
                    helpers::nativeSelfStack.pop_back();
                    helpers::nativeClassStack.pop_back();
                    throw;
                }
                helpers::nativeSelfStack.pop_back();
                helpers::nativeClassStack.pop_back();
                registers[currentFrame->registerBase + a] = Value(instance);
            }
        } else {
            if (argc > 0) throw std::runtime_error("TypeError: Class takes no arguments directly.");
            registers[currentFrame->registerBase + a] = Value(instance);
        }
    } else if (callee.isInstance()) {
        auto inst = callee.asInstance();
        ObjClosure* method = nullptr;
        ObjClass* owningClass = nullptr;
        auto c = inst->classDef;
        while (c) {
            auto it = c->methods.find("__call__");
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
                
                std::vector<Value> actualArgs;
                for (int i = 0; i < argc; ++i) {
                    actualArgs.push_back(registers[currentFrame->registerBase + a + 1 + i]);
                }
                int totalArgc = static_cast<int>(actualArgs.size());

                if (fnDef->hasRestParam) {
                    int fixedMax = fnDef->maxArity - 1;
                    if (totalArgc < fnDef->arity) {
                        throw std::runtime_error("RegVM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                    }
                    ObjList* restList = GcHeap::get().allocate<ObjList>();
                    if (totalArgc > fixedMax) {
                        int restCount = totalArgc - fixedMax;
                        restList->vec.resize(restCount);
                        for (int j = 0; j < restCount; j++) {
                            restList->vec[j] = actualArgs[fixedMax + j];
                        }
                        actualArgs.resize(fixedMax);
                    }
                    while (actualArgs.size() < static_cast<size_t>(fixedMax)) actualArgs.push_back(Value::uninit());
                    actualArgs.push_back(Value(restList));
                } else {
                    if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                        throw std::runtime_error("RegVM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
                    }
                    while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
                }

                if (isTailCall) {
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(currentFrame->registerBase);
                    currentFrame->function = fnDef.get();
                    currentFrame->chunk = &fnDef->chunk;
                    currentFrame->ip = 0;
                    currentFrame->closure = method;
                    currentFrame->selfContext = callee;
                    currentFrame->classContext = Value(owningClass);
                    
                    for (size_t i = 0; i < actualArgs.size(); ++i) {
                        registers[currentFrame->registerBase + i] = actualArgs[i];
                    }
                    for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                        registers[currentFrame->registerBase + i] = Value::none();
                    }
                    
                    populateRefParams(*currentFrame, fnDef.get());
                    return;
                }
                
                CallFrame newFrame;
                newFrame.function = fnDef.get();
                newFrame.chunk = &fnDef->chunk;
                newFrame.ip = 0;
                newFrame.registerBase = currentFrame->registerBase + a + 1;
                newFrame.returnRegister = a;
                newFrame.closure = method;
                newFrame.selfContext = callee;
                newFrame.classContext = Value(owningClass);
                
                for (size_t i = 0; i < actualArgs.size(); ++i) {
                    registers[newFrame.registerBase + i] = actualArgs[i];
                }
                for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                    registers[newFrame.registerBase + i] = Value::none();
                }
                
                populateRefParams(newFrame, fnDef.get());
                
                if (frameCount >= MAX_FRAMES) throw std::runtime_error("RegVM Error: CallFrame stack overflow.");
                frames[frameCount++] = newFrame;
            } else if (method->isNative()) {
                helpers::nativeSelfStack.push_back(callee);
                helpers::nativeClassStack.push_back(Value(owningClass));
                std::vector<Value> args;
                args.reserve(argc);
                for (int i = 0; i < argc; ++i) {
                    args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
                }
                try {
                    auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
                    registers[currentFrame->registerBase + a] = fn(args);
                } catch (...) {
                    helpers::nativeSelfStack.pop_back();
                    helpers::nativeClassStack.pop_back();
                    throw;
                }
                helpers::nativeSelfStack.pop_back();
                helpers::nativeClassStack.pop_back();
            }
        } else {
            throw std::runtime_error("RegVM Error: Target is not callable.");
        }
    } else {
        throw std::runtime_error("RegVM Error: Target is not callable.");
    }
}

BuiltinType parseBuiltinType(const std::string& typeStr) {
    if (typeStr == "any" || typeStr.empty()) return BuiltinType::ANY;
    if (typeStr == "int") return BuiltinType::INT;
    if (typeStr == "float" || typeStr == "double") return BuiltinType::FLOAT;
    if (typeStr == "real") return BuiltinType::REAL;
    if (typeStr == "number") return BuiltinType::NUMBER;
    if (typeStr == "whole") return BuiltinType::WHOLE;
    if (typeStr == "exact") return BuiltinType::EXACT;
    if (typeStr == "string" || typeStr == "str") return BuiltinType::STRING;
    if (typeStr == "bool") return BuiltinType::BOOL;
    if (typeStr == "binary" || typeStr == "bool_like") return BuiltinType::BINARY;
    if (typeStr == "none") return BuiltinType::NONE_TYPE;
    if (typeStr == "list") return BuiltinType::LIST;
    if (typeStr == "dict") return BuiltinType::DICT;
    if (typeStr == "set") return BuiltinType::SET;
    if (typeStr == "fraction") return BuiltinType::FRACTION;
    if (typeStr == "complex") return BuiltinType::COMPLEX;
    if (typeStr == "basenum") return BuiltinType::BASENUM;
    if (typeStr == "symbolic" || typeStr == "symbol" || typeStr == "expr") return BuiltinType::SYMBOLIC;
    if (typeStr == "realmat" || typeStr == "realmatrix") return BuiltinType::REALMAT;
    if (typeStr == "complexmat" || typeStr == "complexmatrix") return BuiltinType::COMPLEXMAT;
    if (typeStr == "stringmat" || typeStr == "stringmatrix") return BuiltinType::STRINGMAT;
    if (typeStr == "matrix") return BuiltinType::MATRIX;
    if (typeStr == "func" || typeStr == "function") return BuiltinType::FUNC;
    if (typeStr == "class") return BuiltinType::CLASS;
    if (typeStr == "instance") return BuiltinType::INSTANCE;
    if (typeStr == "namespace") return BuiltinType::NAMESPACE;
    if (typeStr == "iterable") return BuiltinType::ITERABLE;
    if (typeStr == "callable") return BuiltinType::CALLABLE;
    if (typeStr == "indexable") return BuiltinType::INDEXABLE;
    if (typeStr == "hashable") return BuiltinType::HASHABLE;
    if (typeStr == "numeric") return BuiltinType::NUMERIC;
    return BuiltinType::CUSTOM_CLASS;
}

std::string VM::getTypeName(const Value& val) {
    if (val.isUninit()) return "Uninitialized";
    return val.typeName();
}

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

ObjClosure* VM::findDunder(const Value& val, const std::string& name) {
    if (!val.isInstance()) return nullptr;
    auto inst = val.asInstance();
    auto c = inst->classDef;
    while (c) {
        auto it = c->methods.find(name);
        if (it != c->methods.end()) return it->second;
        c = c->parent;
    }
    return nullptr;
}

bool VM::evaluateTruthiness(const Value& val) {
    if (val.isInstance()) {
        auto method = findDunder(val, DUNDER_BOOL);
        if (method) {
            return callDunder(val, method, {}).truthy();
        }
    }
    return val.truthy();
}

Value VM::callDunder(const Value& obj, ObjClosure* method, const std::vector<Value>& args) {
    auto inst = obj.asInstance();
    if (method->isNative() && !method->isBytecode()) {
        helpers::nativeSelfStack.push_back(Value(inst));
        helpers::nativeClassStack.push_back(Value(inst->classDef));
        Value result;
        try {
            auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
            result = fn(args);
        } catch (...) {
            helpers::nativeSelfStack.pop_back();
            helpers::nativeClassStack.pop_back();
            throw;
        }
        helpers::nativeSelfStack.pop_back();
        helpers::nativeClassStack.pop_back();
        return result;
    } else if (method->isBytecode()) {
        auto& fnDef = compiledFunctions[method->compiledFnIndex];
        CallFrame newFrame;
        newFrame.function = fnDef.get();
        newFrame.chunk = &fnDef->chunk;
        newFrame.ip = 0;
        
        CallFrame* currentFrame = &frames[frameCount - 1];
        int newBase = currentFrame->registerBase + currentFrame->function->localCount;
        newFrame.registerBase = newBase;
        newFrame.returnRegister = 0;
        newFrame.closure = method;
        newFrame.selfContext = Value(inst);
        newFrame.classContext = Value(inst->classDef);
        
        std::vector<Value> actualArgs = args;
        int totalArgc = static_cast<int>(actualArgs.size());

        if (fnDef->hasRestParam) {
            int fixedMax = fnDef->maxArity - 1;
            if (totalArgc < fnDef->arity) {
                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
            }
            ObjList* restList = GcHeap::get().allocate<ObjList>();
            if (totalArgc > fixedMax) {
                int restCount = totalArgc - fixedMax;
                restList->vec.resize(restCount);
                for (int j = 0; j < restCount; j++) {
                    restList->vec[j] = actualArgs[fixedMax + j];
                }
                actualArgs.resize(fixedMax);
            }
            while (actualArgs.size() < static_cast<size_t>(fixedMax)) actualArgs.push_back(Value::uninit());
            actualArgs.push_back(Value(restList));
        } else {
            if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
            }
            while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
        }

        for (size_t i = 0; i < actualArgs.size(); ++i) {
            registers[newBase + i] = actualArgs[i];
        }
        for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
            registers[newBase + i] = Value::none();
        }
        
        populateRefParams(newFrame, fnDef.get());
        
        frames[frameCount++] = newFrame;
        
        int targetDepth = frameCount - 1;
        return run(targetDepth);
    }
    throw std::runtime_error("RegVM Error: Dunder method is not callable.");
}

bool VM::checkValueType(const Value& val, BuiltinType btype, const std::string& typeStr) {
    switch (btype) {
        case BuiltinType::ANY: return true;
        case BuiltinType::INT: return val.isInt32() || val.isObjType(ObjType::BIGINT) || val.isBool();
        case BuiltinType::FLOAT: return val.isDouble();
        case BuiltinType::REAL: return val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION) || val.isObjType(ObjType::BASENUM) || (val.isComplex() && val.asComplex().imag == 0.0);
        case BuiltinType::NUMBER: return val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION) || val.isObjType(ObjType::BASENUM) || val.isComplex();
        case BuiltinType::WHOLE: return val.isInt32() || val.isObjType(ObjType::BIGINT) || val.isBool() || (val.isDouble() && std::isfinite(val.asDoubleRaw()) && val.asDoubleRaw() == std::floor(val.asDoubleRaw())) || (val.isObjType(ObjType::FRACTION) && static_cast<ObjFraction*>(val.asObj())->frac.getDen() == BigInt(1)) || (val.isComplex() && val.asComplex().imag == 0.0 && std::isfinite(val.asComplex().real) && val.asComplex().real == std::floor(val.asComplex().real));
        case BuiltinType::EXACT: return val.isInt32() || val.isObjType(ObjType::BIGINT) || val.isBool() || val.isObjType(ObjType::FRACTION) || val.isObjType(ObjType::BASENUM) || val.isObjType(ObjType::SYMBOLIC);
        case BuiltinType::STRING: return val.isString();
        case BuiltinType::BOOL: return val.isBool();
        case BuiltinType::BINARY: {
            if (val.isBool()) return true;
            try { double d = val.asDouble(); if (d == 0.0 || d == 1.0) return true; } catch (...) {}
            return false;
        }
        case BuiltinType::NONE_TYPE: return val.isNone();
        case BuiltinType::LIST: return val.isObjType(ObjType::LIST);
        case BuiltinType::DICT: return val.isObjType(ObjType::DICT);
        case BuiltinType::SET: return val.isObjType(ObjType::SET);
        case BuiltinType::FRACTION: return val.isObjType(ObjType::FRACTION);
        case BuiltinType::COMPLEX: return val.isObjType(ObjType::COMPLEX);
        case BuiltinType::BASENUM: return val.isObjType(ObjType::BASENUM);
        case BuiltinType::SYMBOLIC: return val.isObjType(ObjType::SYMBOLIC);
        case BuiltinType::REALMAT: return val.isObjType(ObjType::REAL_MATRIX);
        case BuiltinType::COMPLEXMAT: return val.isObjType(ObjType::COMPLEX_MATRIX);
        case BuiltinType::STRINGMAT: return val.isObjType(ObjType::STRING_MATRIX);
        case BuiltinType::MATRIX: return val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) || val.isObjType(ObjType::STRING_MATRIX);
        case BuiltinType::FUNC: return val.isFunctionClosure();
        case BuiltinType::CLASS: return val.isClass();
        case BuiltinType::INSTANCE: return val.isInstance();
        case BuiltinType::NAMESPACE: return val.isObjType(ObjType::NAMESPACE);
        case BuiltinType::ITERABLE: {
            if (val.isObjType(ObjType::LIST) || val.isObjType(ObjType::DICT) || val.isObjType(ObjType::SET) ||
                val.isString() || val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) ||
                val.isObjType(ObjType::STRING_MATRIX)) return true;
            if (val.isInstance()) return findDunder(val, "__iter__") || findDunder(val, "__next__");
            return false;
        }
        case BuiltinType::CALLABLE: {
            if (val.isFunctionClosure() || val.isClass() || val.isString()) return true;
            if (val.isInstance()) return findDunder(val, "__call__") != nullptr;
            return false;
        }
        case BuiltinType::INDEXABLE: {
            if (val.isObjType(ObjType::LIST) || val.isObjType(ObjType::DICT) || val.isString() ||
                val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) ||
                val.isObjType(ObjType::STRING_MATRIX)) return true;
            if (val.isInstance()) return findDunder(val, "__getitem__") != nullptr;
            return false;
        }
        case BuiltinType::HASHABLE: return val.isHashable();
        case BuiltinType::NUMERIC: {
            if (val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION) ||
                val.isObjType(ObjType::COMPLEX) || val.isObjType(ObjType::BASENUM)) return true;
            if (val.isInstance()) {
                return findDunder(val, "__add__") || findDunder(val, "__mul__") || findDunder(val, "__sub__") || findDunder(val, "__div__") || findDunder(val, "__ldiv__");
            }
            return false;
        }
        case BuiltinType::CUSTOM_CLASS:
        default:
            break;
    }

    if (val.isInstance()) {
        auto inst = val.asInstance();
        auto c = inst->classDef;
        
        Value typeVal = Value::none();
        size_t dotPos = typeStr.find('.');
        if (dotPos != std::string::npos) {
            std::string currentName = typeStr.substr(0, dotPos);
            auto it = globalNames.find(currentName);
            if (it != globalNames.end()) {
                Value currentVal = globals[it->second];
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
            auto it = globalNames.find(typeStr);
            if (it != globalNames.end()) typeVal = globals[it->second];
        }

        if (typeVal.isClass()) {
            ObjClass* expectedClass = static_cast<ObjClass*>(typeVal.asObj());
            while (c) {
                if (c == expectedClass) return true;
                c = c->parent;
            }
            return false;
        }

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

void VM::execAssertParamType(const Value& val, uint32_t icIdx, uint32_t nameIdx) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    InlineCache& ic = const_cast<InlineCache&>(currentFrame->function->chunk.inlineCaches[icIdx]);
    if (ic.cachedBuiltinType == BuiltinType::UNKNOWN) {
        ic.cachedBuiltinType = parseBuiltinType(currentFrame->function->chunk.constants[ic.nameIdx].asString());
    }
    const std::string& expectedType = currentFrame->function->chunk.constants[ic.nameIdx].asString();

    if (!checkValueType(val, ic.cachedBuiltinType, expectedType)) {
        const std::string& paramName = currentFrame->function->chunk.constants[nameIdx].asString();
        throw std::runtime_error("TypeError: Parameter '" + paramName +
            "' expected type '" + expectedType +
            "', got '" + getTypeName(val) + "'.");
    }
}

void VM::execAssertReturnType(const Value& val, uint32_t icIdx) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    InlineCache& ic = const_cast<InlineCache&>(currentFrame->function->chunk.inlineCaches[icIdx]);
    if (ic.cachedBuiltinType == BuiltinType::UNKNOWN) {
        ic.cachedBuiltinType = parseBuiltinType(currentFrame->function->chunk.constants[ic.nameIdx].asString());
    }
    const std::string& expectedType = currentFrame->function->chunk.constants[ic.nameIdx].asString();

    if (!checkValueType(val, ic.cachedBuiltinType, expectedType)) {
        throw std::runtime_error("TypeError: Function '" + currentFrame->function->name +
            "' expected to return '" + expectedType +
            "', but returned '" + getTypeName(val) + "'.");
    }
}

void VM::execInvoke(int a, int b, uint32_t icIdx, bool isTailCall, int fbType, uint32_t fbIdx) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    InlineCache& ic = const_cast<InlineCache&>(currentFrame->function->chunk.inlineCaches[icIdx]);
    uint32_t nameIdx = ic.nameIdx;
    const std::string& methodName = currentFrame->function->chunk.constants[nameIdx].asString();
    
    int argc = b;
    Value obj = registers[currentFrame->registerBase + a];

    ObjClosure* method = nullptr;
    ObjClass* owningClass = nullptr;

    if (obj.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(obj.asObj());
        auto it = d->keyMap.find(currentFrame->function->chunk.constants[nameIdx]);
        if (it != d->keyMap.end()) {
            Value fv = d->elements[it->second].second;
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, isTailCall);
                return;
            }
        }
    } else if (obj.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(obj.asObj());
        auto it = ns->fields.find(methodName);
        if (it != ns->fields.end()) {
            Value fv = *(it->second.upval->location);
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, isTailCall);
                return;
            }
        }
    } else if (obj.isInstance()) {
        auto inst = obj.asInstance();
        bool foundInField = false;

        if (ic.cachedClass == inst->classDef && ic.cachedMethod) {
            if (!inst->fields || inst->fields->keyMap.find(currentFrame->function->chunk.constants[nameIdx]) == inst->fields->keyMap.end()) {
                method = ic.cachedMethod;
                owningClass = ic.cachedClass;
                goto invoke_method;
            }
        }

        if (inst->fields) {
            auto it = inst->fields->keyMap.find(currentFrame->function->chunk.constants[nameIdx]);
            if (it != inst->fields->keyMap.end()) {
                Value fv = inst->fields->elements[it->second].second;
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                    owningClass = inst->classDef;
                    foundInField = true;
                } else {
                    registers[currentFrame->registerBase + a] = fv;
                    execCall(a, b, isTailCall);
                    return;
                }
            }
        }

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

            if (!method) {
                auto getattrMethod = findDunder(obj, "__getattr__");
                if (getattrMethod) {
                    std::vector<Value> args = { Value(methodName) };
                    Value fv = callDunder(obj, getattrMethod, args);
                    if (fv.isFunctionClosure()) {
                        method = fv.asFunction();
                        owningClass = inst->classDef;
                    } else {
                        registers[currentFrame->registerBase + a] = fv;
                        execCall(a, b, isTailCall);
                        return;
                    }
                }
            }
        }
    }

invoke_method:
    if (!method) {
        if (fbType != -1) {
            Value fallbackVal;
            if (fbType == 0) {
                fallbackVal = registers[currentFrame->registerBase + fbIdx];
            } else if (fbType == 1) {
                fallbackVal = *(currentFrame->closure->upvalues[fbIdx]->location);
            } else if (fbType == 2) {
                fallbackVal = *(static_cast<ObjUpVal*>(registers[currentFrame->refParamsBase + fbIdx].asObj())->location);
            }
            
            for (int i = argc - 1; i >= 0; --i) {
                registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
            }
            registers[currentFrame->registerBase + a + 1] = obj;
            registers[currentFrame->registerBase + a] = fallbackVal;
            for (auto& pr : pendingCallRefs) {
                pr.first += 1;
            }
            execCall(a, argc + 1, isTailCall);
            return;
        }
        
        auto nIt = jc::VM::activeVM->getNativeBuiltins().find(methodName);
        if (nIt != jc::VM::activeVM->getNativeBuiltins().end()) {
            auto ait = jc::VM::activeVM->getBuiltinArity().find(methodName);
            int totalArgs = argc + 1;
            if (ait != jc::VM::activeVM->getBuiltinArity().end() && !ait->second.empty() && ait->second.find(totalArgs) == ait->second.end()) {
                std::string expected;
                for (auto aIt = ait->second.begin(); aIt != ait->second.end(); ++aIt) {
                    if (aIt != ait->second.begin()) expected += " or ";
                    expected += std::to_string(*aIt - 1);
                }
                throw std::runtime_error("Runtime Error: Method '" + methodName + "' expects " + expected + " arguments, got " + std::to_string(argc) + ".");
            }

            std::vector<Value> argsVec;
            argsVec.reserve(totalArgs);
            argsVec.push_back(obj);
            for (int i = 0; i < argc; ++i) {
                argsVec.push_back(registers[currentFrame->registerBase + a + 1 + i]);
            }
            registers[currentFrame->registerBase + a] = nIt->second(argsVec);
            return;
        }
        
        auto gIt = globalNames.find(methodName);
        if (gIt != globalNames.end() && globals[gIt->second].isFunctionClosure()) {
            for (int i = argc - 1; i >= 0; --i) {
                registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
            }
            registers[currentFrame->registerBase + a + 1] = obj;
            registers[currentFrame->registerBase + a] = globals[gIt->second];
            for (auto& pr : pendingCallRefs) {
                pr.first += 1;
            }
            execCall(a, argc + 1, isTailCall);
            return;
        }
        
        throw std::runtime_error("RegVM Error: Cannot invoke method '" + methodName + "' on this type.");
    }

    if (method->isBytecode()) {
        auto& fnDef = compiledFunctions[method->compiledFnIndex];
        
        std::vector<Value> actualArgs;
        for (int i = 0; i < argc; ++i) {
            actualArgs.push_back(registers[currentFrame->registerBase + a + 1 + i]);
        }
        int totalArgc = static_cast<int>(actualArgs.size());

        if (fnDef->hasRestParam) {
            int fixedMax = fnDef->maxArity - 1;
            if (totalArgc < fnDef->arity) {
                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
            }
            ObjList* restList = GcHeap::get().allocate<ObjList>();
            if (totalArgc > fixedMax) {
                int restCount = totalArgc - fixedMax;
                restList->vec.resize(restCount);
                for (int j = 0; j < restCount; j++) {
                    restList->vec[j] = actualArgs[fixedMax + j];
                }
                actualArgs.resize(fixedMax);
            }
            while (actualArgs.size() < static_cast<size_t>(fixedMax)) actualArgs.push_back(Value::uninit());
            actualArgs.push_back(Value(restList));
        } else {
            if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
            }
            while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
        }

        if (isTailCall) {
            while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                exceptionHandlers.pop_back();
            }
            closeUpvalues(currentFrame->registerBase);
            currentFrame->function = fnDef.get();
            currentFrame->chunk = &fnDef->chunk;
            currentFrame->ip = 0;
            currentFrame->closure = method;
            currentFrame->selfContext = obj;
            currentFrame->classContext = owningClass ? Value(owningClass) : Value::none();
            
            for (size_t i = 0; i < actualArgs.size(); ++i) {
                registers[currentFrame->registerBase + i] = actualArgs[i];
            }
            for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                registers[currentFrame->registerBase + i] = Value::none();
            }
            
            populateRefParams(*currentFrame, fnDef.get());
            return;
        }
        
        CallFrame newFrame;
        newFrame.function = fnDef.get();
        newFrame.chunk = &fnDef->chunk;
        newFrame.ip = 0;
        newFrame.registerBase = currentFrame->registerBase + a + 1;
        newFrame.returnRegister = a;
        newFrame.closure = method;
        newFrame.selfContext = obj;
        newFrame.classContext = owningClass ? Value(owningClass) : Value::none();
        
        for (size_t i = 0; i < actualArgs.size(); ++i) {
            registers[newFrame.registerBase + i] = actualArgs[i];
        }
        for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
            registers[newFrame.registerBase + i] = Value::none();
        }
        
        populateRefParams(newFrame, fnDef.get());
        
        if (frameCount >= MAX_FRAMES) throw std::runtime_error("RegVM Error: CallFrame stack overflow.");
        frames[frameCount++] = newFrame;
    } else if (method->isNative()) {
        if (static_cast<int>(method->maxArgs()) > 0 && !method->hasRestParam) {
            if (argc < static_cast<int>(method->minArgs()) || argc > static_cast<int>(method->maxArgs())) {
                throw std::runtime_error("Runtime Error: Method '" + methodName + 
                    "' expects " + std::to_string(method->minArgs()) + " to " + 
                    std::to_string(method->maxArgs()) + " arguments, got " + 
                    std::to_string(argc) + ".");
            }
        }

        helpers::nativeSelfStack.push_back(obj);
        helpers::nativeClassStack.push_back(owningClass ? Value(owningClass) : Value::none());
        std::vector<Value> args;
        args.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
        }
        try {
            auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
            registers[currentFrame->registerBase + a] = fn(args);
        } catch (...) {
            helpers::nativeSelfStack.pop_back();
            helpers::nativeClassStack.pop_back();
            throw;
        }
        helpers::nativeSelfStack.pop_back();
        helpers::nativeClassStack.pop_back();
    }
}

void VM::execSuperInvoke(int a, int b, uint32_t nameIdx, bool isTailCall) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    const std::string& methodName = currentFrame->function->chunk.constants[nameIdx].asString();
    Value selfVal = registers[currentFrame->registerBase + a];
    int argc = b;
    
    if (!selfVal.isInstance()) throw std::runtime_error("RegVM Error: 'super' requires an instance context.");
    auto inst = selfVal.asInstance();
    
    Value classVal = currentFrame->classContext;
    if (!classVal.isClass()) throw std::runtime_error("RegVM Error: 'super' requires class context.");
    auto currentClass = static_cast<ObjClass*>(classVal.asObj());
    auto parentClass = currentClass->parent;
    if (!parentClass) throw std::runtime_error("RegVM Error: No parent class.");
    
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
    if (!method) throw std::runtime_error("RegVM Error: Parent class has no method '" + methodName + "'.");
    
    if (method->isBytecode()) {
        auto& fnDef = compiledFunctions[method->compiledFnIndex];
        
        std::vector<Value> actualArgs;
        for (int i = 0; i < argc; ++i) {
            actualArgs.push_back(registers[currentFrame->registerBase + a + 1 + i]);
        }
        int totalArgc = static_cast<int>(actualArgs.size());

        if (fnDef->hasRestParam) {
            int fixedMax = fnDef->maxArity - 1;
            if (totalArgc < fnDef->arity) {
                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
            }
            ObjList* restList = GcHeap::get().allocate<ObjList>();
            if (totalArgc > fixedMax) {
                int restCount = totalArgc - fixedMax;
                restList->vec.resize(restCount);
                for (int j = 0; j < restCount; j++) {
                    restList->vec[j] = actualArgs[fixedMax + j];
                }
                actualArgs.resize(fixedMax);
            }
            while (actualArgs.size() < static_cast<size_t>(fixedMax)) actualArgs.push_back(Value::uninit());
            actualArgs.push_back(Value(restList));
        } else {
            if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
            }
            while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
        }

        if (isTailCall) {
            while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                exceptionHandlers.pop_back();
            }
            closeUpvalues(currentFrame->registerBase);
            currentFrame->function = fnDef.get();
            currentFrame->chunk = &fnDef->chunk;
            currentFrame->ip = 0;
            currentFrame->closure = method;
            currentFrame->selfContext = Value(inst);
            currentFrame->classContext = Value(owningClass);
            
            for (size_t i = 0; i < actualArgs.size(); ++i) {
                registers[currentFrame->registerBase + i] = actualArgs[i];
            }
            for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                registers[currentFrame->registerBase + i] = Value::none();
            }
            
            populateRefParams(*currentFrame, fnDef.get());
            return;
        }
        
        CallFrame newFrame;
        newFrame.function = fnDef.get();
        newFrame.chunk = &fnDef->chunk;
        newFrame.ip = 0;
        newFrame.registerBase = currentFrame->registerBase + a + 1;
        newFrame.returnRegister = a;
        newFrame.closure = method;
        newFrame.selfContext = Value(inst);
        newFrame.classContext = Value(owningClass);
        
        for (size_t i = 0; i < actualArgs.size(); ++i) {
            registers[newFrame.registerBase + i] = actualArgs[i];
        }
        for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
            registers[newFrame.registerBase + i] = Value::none();
        }
        
        populateRefParams(newFrame, fnDef.get());
        
        if (frameCount >= MAX_FRAMES) throw std::runtime_error("RegVM Error: CallFrame stack overflow.");
        frames[frameCount++] = newFrame;
    } else if (method->isNative()) {
        if (static_cast<int>(method->maxArgs()) > 0 && !method->hasRestParam) {
            if (argc < static_cast<int>(method->minArgs()) || argc > static_cast<int>(method->maxArgs())) {
                throw std::runtime_error("Runtime Error: Super method '" + methodName + 
                    "' expects " + std::to_string(method->minArgs()) + " to " + 
                    std::to_string(method->maxArgs()) + " arguments, got " + 
                    std::to_string(argc) + ".");
            }
        }

        helpers::nativeSelfStack.push_back(Value(inst));
        helpers::nativeClassStack.push_back(Value(owningClass));
        std::vector<Value> args;
        args.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
        }
        try {
            auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
            registers[currentFrame->registerBase + a] = fn(args);
        } catch (...) {
            helpers::nativeSelfStack.pop_back();
            helpers::nativeClassStack.pop_back();
            throw;
        }
        helpers::nativeSelfStack.pop_back();
        helpers::nativeClassStack.pop_back();
    }
}

void VM::execSliceGet(int a, int b, uint8_t dims) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    Value obj = registers[currentFrame->registerBase + b];
    
    auto readOptionalInt = [&](int idx) -> std::pair<bool, int> {
        Value v = registers[currentFrame->registerBase + b + 1 + idx];
        if (v.isNone()) return { false, 0 };
        if (v.isInt32()) return { true, v.asInt32() };
        if (v.isDouble()) return { true, static_cast<int>(std::round(v.asDoubleRaw())) };
        return { true, static_cast<int>(std::round(v.asDouble())) };
    };

    struct SliceInfo { int start; int step; int count; };
    auto buildSliceInfo = [](int dimSize, std::pair<bool, int> start, std::pair<bool, int> end, std::pair<bool, int> step) -> SliceInfo {
        int sp = step.first ? step.second : 1;
        if (step.first && sp == 0) {
            int idx = start.first ? start.second : 0;
            if (idx < 0) idx = dimSize + idx;
            if (idx < 0 || idx >= dimSize) throw std::out_of_range("RegVM Error: Index out of bounds.");
            return { idx, 0, 1 };
        }
        int st, en;
        if (sp > 0) {
            st = start.first ? start.second : 0;
            en = end.first ? end.second : dimSize;
        } else {
            st = start.first ? start.second : dimSize - 1;
            en = end.first ? end.second : -1;
        }
        if (st < 0) st = dimSize + st;
        if (en < 0 && end.first) en = dimSize + en;
        if (sp > 0) {
            st = std::max(0, std::min(dimSize, st));
            en = std::max(0, std::min(dimSize, en));
        } else {
            st = std::max(-1, std::min(dimSize - 1, st));
            en = std::max(-1, std::min(dimSize - 1, en));
        }
        int count = 0;
        if (sp > 0) {
            if (en > st) count = (en - st + sp - 1) / sp;
        } else {
            if (en < st) count = (st - en - sp - 1) / (-sp);
        }
        return { st, sp, count };
    };

    if (dims == 1) {
        auto step = readOptionalInt(0);
        auto end = readOptionalInt(1);
        auto start = readOptionalInt(2);
        
        if (obj.isString()) {
            ObjString* objStr = obj.asObjString();
            const auto& s = objStr->str;
            auto info = buildSliceInfo(static_cast<int>(objStr->charLength), start, end, step);
            std::string result;
            if (objStr->isAscii) {
                result.reserve(info.count);
                for (int i = 0; i < info.count; ++i) result += s[info.start + i * info.step];
            } else {
                for (int i = 0; i < info.count; ++i) result += utf8::substring(s, info.start + i * info.step, 1, false);
            }
            registers[currentFrame->registerBase + a] = Value(result);
            return;
        }
        
        if (obj.isObjType(ObjType::LIST)) {
            const auto& L = static_cast<ObjList*>(obj.asObj())->vec;
            auto info = buildSliceInfo(static_cast<int>(L.size()), start, end, step);
            ObjList* result = GcHeap::get().allocate<ObjList>();
            result->vec.reserve(info.count);
            for (int i = 0; i < info.count; ++i) result->vec.push_back(L[info.start + i * info.step]);
            registers[currentFrame->registerBase + a] = Value(result);
            return;
        }
        
        if (obj.isObjType(ObjType::REAL_MATRIX)) {
            const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
            int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
            auto info = buildSliceInfo(n, start, end, step);
            std::vector<double> result;
            result.reserve(info.count);
            if (m.getRows() == 1) {
                for (int i = 0; i < info.count; ++i) result.push_back(m(0, info.start + i * info.step));
                registers[currentFrame->registerBase + a] = Value(RealMatrix(1, info.count, result));
            } else if (m.getCols() == 1) {
                for (int i = 0; i < info.count; ++i) result.push_back(m(info.start + i * info.step, 0));
                registers[currentFrame->registerBase + a] = Value(RealMatrix(info.count, 1, result));
            } else {
                std::vector<double> flat;
                flat.reserve(info.count * m.getCols());
                for (int i = 0; i < info.count; ++i) {
                    int id = info.start + i * info.step;
                    for (int j = 0; j < m.getCols(); ++j) flat.push_back(m(id, j));
                }
                registers[currentFrame->registerBase + a] = Value(RealMatrix(info.count, m.getCols(), flat));
            }
            return;
        }
        
        throw std::runtime_error("RegVM Error: Cannot slice a value of type '" + getTypeName(obj) + "'.");
    } else if (dims == 2) {
        auto cStep = readOptionalInt(0);
        auto cEnd = readOptionalInt(1);
        auto cStart = readOptionalInt(2);
        auto rStep = readOptionalInt(3);
        auto rEnd = readOptionalInt(4);
        auto rStart = readOptionalInt(5);
        
        auto processMatSlice = [&](const auto& m) {
            auto rInfo = buildSliceInfo(m.getRows(), rStart, rEnd, rStep);
            auto cInfo = buildSliceInfo(m.getCols(), cStart, cEnd, cStep);
            using MatType = std::decay_t<decltype(m)>;
            using ElemType = std::decay_t<decltype(m(0, 0))>;
            std::vector<ElemType> flat;
            flat.reserve(rInfo.count * cInfo.count);
            for (int i = 0; i < rInfo.count; ++i) {
                int ri = rInfo.start + i * rInfo.step;
                for (int j = 0; j < cInfo.count; ++j) {
                    int ci = cInfo.start + j * cInfo.step;
                    flat.push_back(m(ri, ci));
                }
            }
            registers[currentFrame->registerBase + a] = Value(MatType(rInfo.count, cInfo.count, flat));
        };
        
        if (obj.isObjType(ObjType::REAL_MATRIX)) {
            processMatSlice(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
        } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
            processMatSlice(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
        } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
            processMatSlice(static_cast<ObjStringMatrix*>(obj.asObj())->mat);
        } else {
            throw std::runtime_error("RegVM Error: 2D slicing requires a matrix.");
        }
    } else {
        throw std::runtime_error("RegVM Error: Unsupported slice dimensionality.");
    }
}

void VM::execSliceSet(int a, int c, uint8_t dims) {
    (void)c;
    CallFrame* currentFrame = &frames[frameCount - 1];
    Value obj = registers[currentFrame->registerBase + a];
    Value val = registers[currentFrame->registerBase + a + 3 * dims + 1];
    
    auto readOptionalInt = [&](int idx) -> std::pair<bool, int> {
        Value v = registers[currentFrame->registerBase + a + 1 + idx];
        if (v.isNone()) return { false, 0 };
        if (v.isInt32()) return { true, v.asInt32() };
        if (v.isDouble()) return { true, static_cast<int>(std::round(v.asDoubleRaw())) };
        return { true, static_cast<int>(std::round(v.asDouble())) };
    };

    struct SliceInfo { int start; int step; int count; };
    auto buildSliceInfo = [](int dimSize, std::pair<bool, int> start, std::pair<bool, int> end, std::pair<bool, int> step) -> SliceInfo {
        int sp = step.first ? step.second : 1;
        if (step.first && sp == 0) {
            int idx = start.first ? start.second : 0;
            if (idx < 0) idx = dimSize + idx;
            if (idx < 0 || idx >= dimSize) throw std::out_of_range("RegVM Error: Index out of bounds.");
            return { idx, 0, 1 };
        }
        int st, en;
        if (sp > 0) {
            st = start.first ? start.second : 0;
            en = end.first ? end.second : dimSize;
        } else {
            st = start.first ? start.second : dimSize - 1;
            en = end.first ? end.second : -1;
        }
        if (st < 0) st = dimSize + st;
        if (en < 0 && end.first) en = dimSize + en;
        if (sp > 0) {
            st = std::max(0, std::min(dimSize, st));
            en = std::max(0, std::min(dimSize, en));
        } else {
            st = std::max(-1, std::min(dimSize - 1, st));
            en = std::max(-1, std::min(dimSize - 1, en));
        }
        int count = 0;
        if (sp > 0) {
            if (en > st) count = (en - st + sp - 1) / sp;
        } else {
            if (en < st) count = (st - en - sp - 1) / (-sp);
        }
        return { st, sp, count };
    };

    if (dims == 1) {
        auto step = readOptionalInt(0);
        auto end = readOptionalInt(1);
        auto start = readOptionalInt(2);
        
        if (obj.isObjType(ObjType::LIST)) {
            auto list = static_cast<ObjList*>(obj.asObj());
            auto info = buildSliceInfo(static_cast<int>(list->vec.size()), start, end, step);
            if (val.isObjType(ObjType::LIST)) {
                const auto& srcL = static_cast<ObjList*>(val.asObj())->vec;
                if (static_cast<int>(srcL.size()) != info.count) throw std::runtime_error("RegVM Error: Slice assignment size mismatch.");
                for (int k = 0; k < info.count; ++k) list->mut()[info.start + k * info.step] = srcL[k];
            } else {
                for (int i = 0; i < info.count; ++i) list->mut()[info.start + i * info.step] = val;
            }
        } else if (obj.isObjType(ObjType::REAL_MATRIX)) {
            if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
            auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
            int n = (m.getRows() == 1) ? m.getCols() : m.getRows();
            auto info = buildSliceInfo(n, start, end, step);
            
            if (val.isNumber() || val.isObjType(ObjType::BIGINT) || val.isObjType(ObjType::FRACTION)) {
                double v = val.asDouble();
                if (m.getRows() == 1) {
                    for (int i = 0; i < info.count; ++i) m(0, info.start + i * info.step) = v;
                } else if (m.getCols() == 1) {
                    for (int i = 0; i < info.count; ++i) m(info.start + i * info.step, 0) = v;
                } else {
                    for (int i = 0; i < info.count; ++i) {
                        int id = info.start + i * info.step;
                        for (int j = 0; j < m.getCols(); ++j) m(id, j) = v;
                    }
                }
            } else if (val.isObjType(ObjType::REAL_MATRIX)) {
                const auto& src = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                auto srcFlat = src.rawData();
                if (m.getRows() == 1 || m.getCols() == 1) {
                    if (static_cast<int>(srcFlat.size()) != info.count) throw std::runtime_error("RegVM Error: Slice assignment size mismatch.");
                    if (m.getRows() == 1) {
                        for (int k = 0; k < info.count; ++k) m(0, info.start + k * info.step) = srcFlat[k];
                    } else {
                        for (int k = 0; k < info.count; ++k) m(info.start + k * info.step, 0) = srcFlat[k];
                    }
                } else {
                    if (static_cast<int>(srcFlat.size()) != info.count * m.getCols()) throw std::runtime_error("RegVM Error: Slice assignment size mismatch for matrix row.");
                    for (int k = 0; k < info.count; ++k) {
                        int id = info.start + k * info.step;
                        for (int j = 0; j < m.getCols(); ++j) m(id, j) = srcFlat[k * m.getCols() + j];
                    }
                }
            } else {
                throw std::runtime_error("RegVM Error: Cannot assign this type to slice.");
            }
            registers[currentFrame->registerBase + a] = obj;
        } else {
            throw std::runtime_error("RegVM Error: Cannot slice-assign a value of type '" + getTypeName(obj) + "'.");
        }
    } else if (dims == 2) {
        auto cStep = readOptionalInt(0);
        auto cEnd = readOptionalInt(1);
        auto cStart = readOptionalInt(2);
        auto rStep = readOptionalInt(3);
        auto rEnd = readOptionalInt(4);
        auto rStart = readOptionalInt(5);
        
        auto processMatSliceSet = [&](auto& m) {
            auto rInfo = buildSliceInfo(m.getRows(), rStart, rEnd, rStep);
            auto cInfo = buildSliceInfo(m.getCols(), cStart, cEnd, cStep);
            int dstR = rInfo.count;
            int dstC = cInfo.count;
            using ElemType = std::decay_t<decltype(m(0, 0))>;
            
            bool isRhsMat = val.isObjType(ObjType::REAL_MATRIX) || val.isObjType(ObjType::COMPLEX_MATRIX) || val.isObjType(ObjType::STRING_MATRIX);
            if (isRhsMat) {
                int srcR = 0, srcC = 0;
                if (val.isObjType(ObjType::REAL_MATRIX)) {
                    srcR = static_cast<ObjRealMatrix*>(val.asObj())->mat.getRows();
                    srcC = static_cast<ObjRealMatrix*>(val.asObj())->mat.getCols();
                } else if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                    srcR = static_cast<ObjComplexMatrix*>(val.asObj())->mat.getRows();
                    srcC = static_cast<ObjComplexMatrix*>(val.asObj())->mat.getCols();
                } else {
                    srcR = static_cast<ObjStringMatrix*>(val.asObj())->mat.getRows();
                    srcC = static_cast<ObjStringMatrix*>(val.asObj())->mat.getCols();
                }
                if (srcR != dstR || srcC != dstC) throw std::runtime_error("RegVM Error: Slice assignment size mismatch.");
                
                for (int i = 0; i < dstR; ++i) {
                    int ri = rInfo.start + i * rInfo.step;
                    for (int j = 0; j < dstC; ++j) {
                        int ci = cInfo.start + j * cInfo.step;
                        if constexpr (std::is_same_v<ElemType, double>) {
                            if (val.isObjType(ObjType::REAL_MATRIX)) m(ri, ci) = static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j);
                            else throw std::runtime_error("RegVM Error: Cannot assign complex/string matrix to real matrix slice.");
                        } else if constexpr (std::is_same_v<ElemType, Complex>) {
                            if (val.isObjType(ObjType::COMPLEX_MATRIX)) m(ri, ci) = static_cast<ObjComplexMatrix*>(val.asObj())->mat(i, j);
                            else if (val.isObjType(ObjType::REAL_MATRIX)) m(ri, ci) = Complex(static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j));
                            else throw std::runtime_error("RegVM Error: Cannot assign string matrix to complex matrix slice.");
                        } else if constexpr (std::is_same_v<ElemType, std::string>) {
                            std::ostringstream oss;
                            if (val.isObjType(ObjType::STRING_MATRIX)) oss << static_cast<ObjStringMatrix*>(val.asObj())->mat(i, j);
                            else if (val.isObjType(ObjType::COMPLEX_MATRIX)) oss << Value(static_cast<ObjComplexMatrix*>(val.asObj())->mat(i, j));
                            else oss << Value(static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j));
                            m(ri, ci) = oss.str();
                        }
                    }
                }
            } else {
                ElemType scalarVal{};
                if constexpr (std::is_same_v<ElemType, double>) scalarVal = val.asDouble();
                else if constexpr (std::is_same_v<ElemType, Complex>) scalarVal = val.asComplex();
                else if constexpr (std::is_same_v<ElemType, std::string>) {
                    if (val.isString()) scalarVal = val.asString();
                    else { std::ostringstream oss; if (val.isUninit()) oss << "Uninitialized"; else oss << val; scalarVal = oss.str(); }
                }
                for (int i = 0; i < rInfo.count; ++i) {
                    int ri = rInfo.start + i * rInfo.step;
                    for (int j = 0; j < cInfo.count; ++j) {
                        int ci = cInfo.start + j * cInfo.step;
                        m(ri, ci) = scalarVal;
                    }
                }
            }
        };
        
        if (obj.isObjType(ObjType::REAL_MATRIX)) {
            if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
            processMatSliceSet(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
            registers[currentFrame->registerBase + a] = obj;
        } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
            if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
            processMatSliceSet(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
            registers[currentFrame->registerBase + a] = obj;
        } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
            if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
            processMatSliceSet(static_cast<ObjStringMatrix*>(obj.asObj())->mat);
            registers[currentFrame->registerBase + a] = obj;
        } else {
            throw std::runtime_error("RegVM Error: 2D slice assignment requires a matrix.");
        }
    } else {
        throw std::runtime_error("RegVM Error: Unsupported slice assignment dimensionality.");
    }
}

Value VM::execImport(const std::string& name) {
    (void)name;
    throw std::runtime_error("RegVM Error: execImport not fully implemented.");
}

bool VM::handleExceptionUnwind(Value errVal) {
    if (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= currentTargetFrameDepth) {
        auto handler = exceptionHandlers.back();
        exceptionHandlers.pop_back();
        
        while (frameCount > handler.frameIndex + 1) {
            frames[frameCount - 1].selfContext = Value::none();
            frames[frameCount - 1].classContext = Value::none();
            frames[frameCount - 1].closure = nullptr;
            frames[frameCount - 1].refParamsBase = -1;
            frameCount--;
        }
        
        pendingCallRefs.clear();
        
        CallFrame* frame = &frames[frameCount - 1];
        frame->ip = handler.ip;
        frame->registerBase = handler.registerBase;
        
        closeUpvalues(frame->registerBase + frame->function->localCount);

        if (errVal.isString()) {
            std::string s = errVal.asString();
            if (s.find("[Line ") == 0) {
                size_t c = s.find("] ");
                if (c != std::string::npos) errVal = Value(s.substr(c + 2));
            }
        }
        
        registers[frame->registerBase + handler.errReg] = errVal;
        return true;
    }
    return false;
}

VM::VM() {
    registers = new Value[MAX_REGISTERS];
    frames = new CallFrame[MAX_FRAMES];
}

VM::~VM() {
    delete[] registers;
    delete[] frames;
}

Value VM::execute(const Chunk& mainChunk, int localCount) {
    auto mainFn = std::make_shared<CompiledFunction>();
    mainFn->name = "<script>";
    mainFn->chunk = mainChunk;
    mainFn->localCount = localCount;
    
    CallFrame mainFrame;
    mainFrame.function = mainFn.get();
    mainFrame.chunk = &mainChunk;
    mainFrame.ip = 0;
    
    if (frameCount > 0) {
        CallFrame* prev = &frames[frameCount - 1];
        mainFrame.registerBase = prev->registerBase + prev->function->localCount;
    } else {
        mainFrame.registerBase = 0;
    }
    mainFrame.returnRegister = 0;
    
    if (frameCount >= MAX_FRAMES) throw std::runtime_error("RegVM Error: CallFrame stack overflow.");
    
    int targetDepth = frameCount;
    frames[frameCount++] = mainFrame;

    try {
        Value res = run(targetDepth);
        frames[frameCount].selfContext = Value::none();
        frames[frameCount].classContext = Value::none();
        frames[frameCount].closure = nullptr;
        frames[frameCount].refParamsBase = -1;
        return res;
    } catch (...) {
        while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= targetDepth) {
            exceptionHandlers.pop_back();
        }
        while (frameCount > targetDepth) {
            frames[frameCount - 1].selfContext = Value::none();
            frames[frameCount - 1].classContext = Value::none();
            frames[frameCount - 1].closure = nullptr;
            frames[frameCount - 1].refParamsBase = -1;
            frameCount--;
        }
        pendingCallRefs.clear();
        throw;
    }
}

Value VM::run(int targetFrameDepth) {
    struct DepthRestorer {
        VM* vm;
        int prev;
        DepthRestorer(VM* v, int p) : vm(v), prev(p) {}
        ~DepthRestorer() { vm->currentTargetFrameDepth = prev; }
    } restorer(this, currentTargetFrameDepth);
    currentTargetFrameDepth = targetFrameDepth;

    CallFrame* frame = &frames[frameCount - 1];
    const Chunk* chunk = frame->chunk;
    const Instruction* code = chunk->code.data();
    
    // 提取 EXTRAARG 扩展操作数 (24-bit)
    auto fetchExtra = [&]() -> int {
        Instruction ext = code[frame->ip++];
        return GET_Ax(ext);
    };

    // 获取物理寄存器或溢出槽 (Unified Address Space)
    auto getReg = [&](int idx) -> Value& {
        return registers[frame->registerBase + idx];
    };

    // K-Bit 机制：解析寄存器或常量池索引
    auto getRK = [&](int rk) -> Value {
        if (ISK(rk)) {
            int idx = INDEXK(rk);
            if (idx == ESCAPE_KBIT_CONST) idx = fetchExtra();
            return chunk->constants[idx];
        } else {
            if (rk == ESCAPE_KBIT_REG) rk = fetchExtra();
            return getReg(rk);
        }
    };

    while (true) {
        try {
        Instruction instruction = code[frame->ip++];
        OpCode op = GET_OPCODE(instruction);
        
        int a = GET_A(instruction);
        int b = GET_B(instruction);
        int c = GET_C(instruction);
        int bx = GET_Bx(instruction);
        int sbx = GET_sBx(instruction);
        int ax = GET_Ax(instruction);
        (void)ax;
        int sax = GET_sAx(instruction);

        switch (op) {
            case OpCode::MOVE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                getReg(a) = getReg(b);
                break;
            }
            case OpCode::LOADK: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                getReg(a) = chunk->constants[bx];
                break;
            }
            case OpCode::LOAD_NIL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                getReg(a) = Value::none();
                break;
            }
            case OpCode::LOAD_BOOL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                getReg(a) = Value(b != 0);
                break;
            }
            case OpCode::GET_GLOBAL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[bx]);
                if (ic.cachedGlobalSlot != -1) {
                    getReg(a) = globals[ic.cachedGlobalSlot];
                } else {
                    const std::string& name = chunk->constants[ic.nameIdx].asString();
                    if (name == "__class__") {
                        if (frame->classContext.isNone()) throw std::runtime_error("RegVM Error: '__class__' accessed outside of context.");
                        getReg(a) = frame->classContext;
                        break;
                    }
                    auto it = globalNames.find(name);
                    if (it != globalNames.end()) {
                        ic.cachedGlobalSlot = it->second;
                        getReg(a) = globals[it->second];
                    } else {
                        Value builtinVal = jc::VM::activeVM->getBuiltinClosure(name);
                        if (!builtinVal.isNone()) {
                            getReg(a) = builtinVal;
                        } else {
                            throw std::runtime_error("RegVM Error: Undefined global variable '" + name + "'.");
                        }
                    }
                }
                break;
            }
            case OpCode::SET_GLOBAL:
            case OpCode::SET_GLOBAL_REF:
            case OpCode::DEFINE_CONST_GLOBAL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[bx]);
                const std::string& name = chunk->constants[ic.nameIdx].asString();
                if (name == "__class__") throw std::runtime_error("Syntax Error: cannot override context keyword '" + name + "'.");
                
                if (constGlobals.count(name) && op != OpCode::DEFINE_CONST_GLOBAL) {
                    throw std::runtime_error("Runtime Error: Cannot modify const variable '" + name + "'.");
                }
                if (op == OpCode::DEFINE_CONST_GLOBAL && constGlobals.count(name)) {
                    throw std::runtime_error("Runtime Error: Cannot redefine const variable '" + name + "'.");
                }
                if (op == OpCode::SET_GLOBAL_REF) {
                    if (globalNames.find(name) == globalNames.end() && jc::VM::activeVM->getNativeBuiltins().find(name) == jc::VM::activeVM->getNativeBuiltins().end()) {
                        throw std::runtime_error("Runtime Error: Undefined variable '" + name + "'.");
                    }
                }

                Value val = getReg(a);
                if (val.isFunctionClosure()) {
                    auto nit = jc::VM::activeVM->getNativeBuiltins().find(name);
                    if (nit != jc::VM::activeVM->getNativeBuiltins().end()) {
                        auto ait = jc::VM::activeVM->getBuiltinArity().find(name);
                        auto closure = val.asFunction();
                        if (ait == jc::VM::activeVM->getBuiltinArity().end() || ait->second.empty()) {
                            throw std::runtime_error("Runtime Error: Cannot redefine '" + name + "' — it is a variadic built-in function.");
                        }
                        for (int argC = closure->minArgs(); argC <= closure->maxArgs(); ++argC) {
                            if (ait->second.count(argC)) {
                                throw std::runtime_error("Runtime Error: Cannot redefine '" + name + "' with " + std::to_string(argC) + " parameter(s) — conflicts with built-in function. Use a different parameter count to create an overload.");
                            }
                        }
                    }
                }

                if (ic.cachedGlobalSlot != -1) {
                    globals[ic.cachedGlobalSlot] = val;
                } else {
                    auto it = globalNames.find(name);
                    if (it != globalNames.end()) {
                        ic.cachedGlobalSlot = it->second;
                        globals[it->second] = val;
                    } else {
                        ic.cachedGlobalSlot = static_cast<int>(globals.size());
                        globalNames[name] = ic.cachedGlobalSlot;
                        globals.push_back(val);
                    }
                }
                
                if (op == OpCode::DEFINE_CONST_GLOBAL) {
                    constGlobals.insert(name);
                }
                break;
            }
            case OpCode::DELETE_GLOBAL: {
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                const std::string& name = chunk->constants[bx].asString();
                if (constGlobals.count(name)) {
                    throw std::runtime_error("Runtime Error: Cannot delete const variable '" + name + "'.");
                }
                auto it = globalNames.find(name);
                if (it != globalNames.end()) {
                    globals[it->second] = Value::none();
                    globalNames.erase(it);
                    clearAllGlobalICs();
                } else {
                    throw std::runtime_error("RegVM Error: Undefined global variable '" + name + "'.");
                }
                break;
            }
            case OpCode::IS_UNINIT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                getReg(a) = Value(getReg(b).isUninit());
                break;
            }
            case OpCode::CLOSURE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                        
                int fnIdx = static_cast<int>(std::round(chunk->constants[bx].asDouble()));
                if (fnIdx < 0 || fnIdx >= static_cast<int>(compiledFunctions.size()))
                    throw std::runtime_error("RegVM Error: Invalid function index.");

                auto& fn = compiledFunctions[fnIdx];
                auto closure = GcHeap::get().allocate<ObjClosure>(
                    std::vector<std::string>{}, std::vector<bool>{}, fn->name, nullptr
                );
                globals.push_back(Value(closure)); // ★ 临时 Root 防止 GC 误杀
                closure->compiledFnIndex = fnIdx;

                if (!fn->upvalues.empty()) {
                    closure->upvalueCount = static_cast<int>(fn->upvalues.size());
                    closure->upvalues = new ObjUpVal*[closure->upvalueCount];
                    for (int i = 0; i < closure->upvalueCount; ++i) {
                        auto& uv = fn->upvalues[i];
                        if (uv.isRef) {
                            if (uv.isLocal) {
                                if (uv.isRefParam) {
                                    closure->upvalues[i] = static_cast<ObjUpVal*>(getReg(frame->refParamsBase + uv.index).asObj());
                                } else {
                                    closure->upvalues[i] = captureUpvalue(frame->registerBase + uv.index);
                                }
                            } else {
                                if (frame->closure && uv.index < frame->closure->upvalueCount)
                                    closure->upvalues[i] = frame->closure->upvalues[uv.index];
                                else {
                                    auto dummy = GcHeap::get().allocate<ObjUpVal>();
                                    if (uv.isGlobal) {
                                        auto it = globalNames.find(uv.name);
                                        if (it != globalNames.end()) {
                                            dummy->location = &globals[it->second];
                                        } else {
                                            globalNames[uv.name] = static_cast<uint32_t>(globals.size());
                                            globals.push_back(Value::uninit());
                                            dummy->location = &globals.back();
                                        }
                                    } else {
                                        dummy->closed = Value::none();
                                        dummy->location = &dummy->closed;
                                    }
                                    closure->upvalues[i] = dummy;
                                }
                            }
                        } else {
                            auto dummy = GcHeap::get().allocate<ObjUpVal>();
                            if (uv.isExplicitState) {
                                dummy->closed = Value::uninit();
                            } else if (uv.isGlobal) {
                                auto it = globalNames.find(uv.name);
                                if (it != globalNames.end()) {
                                    dummy->closed = globals[it->second];
                                } else {
                                    Value builtinVal = jc::VM::activeVM->getBuiltinClosure(uv.name);
                                    if (!builtinVal.isNone()) {
                                        dummy->closed = builtinVal;
                                    } else {
                                        throw std::runtime_error("RegVM Error: Undefined variable '" + uv.name + "'.");
                                    }
                                }
                            } else if (uv.isLocal) {
                                if (uv.isRefParam) {
                                    dummy->closed = *(static_cast<ObjUpVal*>(getReg(frame->refParamsBase + uv.index).asObj())->location);
                                } else {
                                    dummy->closed = getReg(uv.index);
                                }
                            } else {
                                if (frame->closure && uv.index < frame->closure->upvalueCount) {
                                    dummy->closed = *(frame->closure->upvalues[uv.index]->location);
                                } else {
                                    dummy->closed = Value::none();
                                }
                            }
                            dummy->location = &dummy->closed;
                            closure->upvalues[i] = dummy;
                        }
                    }
                }
                        
                getReg(a) = globals.back();
                globals.pop_back();
                        
                int capturedFnIdx = fnIdx;
                VM* vm = this;
                Value currentSelf = frame->selfContext;
                Value currentClass = frame->classContext;
                closure->nativeFn = std::make_any<NativeCallable>(
                    [vm, capturedFnIdx, closure, currentSelf, currentClass](const std::vector<Value>& args) -> Value {
                        Value s = !helpers::nativeSelfStack.empty() ? helpers::nativeSelfStack.back() : currentSelf;
                        Value c = !helpers::nativeClassStack.empty() ? helpers::nativeClassStack.back() : currentClass;
                        
                        auto& fnDef = vm->compiledFunctions[capturedFnIdx];
                        
                        std::vector<Value> actualArgs = args;
                        int totalArgc = static_cast<int>(actualArgs.size());

                        if (fnDef->hasRestParam) {
                            int fixedMax = fnDef->maxArity - 1;
                            if (totalArgc < fnDef->arity) {
                                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                            }
                            ObjList* restList = GcHeap::get().allocate<ObjList>();
                            if (totalArgc > fixedMax) {
                                int restCount = totalArgc - fixedMax;
                                restList->vec.resize(restCount);
                                for (int j = 0; j < restCount; j++) {
                                    restList->vec[j] = actualArgs[fixedMax + j];
                                }
                                actualArgs.resize(fixedMax);
                            }
                            while (actualArgs.size() < static_cast<size_t>(fixedMax)) actualArgs.push_back(Value::uninit());
                            actualArgs.push_back(Value(restList));
                        } else {
                            if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                                throw std::runtime_error("RegVM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
                            }
                            while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
                        }

                        CallFrame newFrame;
                        newFrame.function = fnDef.get();
                        newFrame.chunk = &fnDef->chunk;
                        newFrame.ip = 0;
                        newFrame.registerBase = vm->frames[vm->frameCount - 1].registerBase + vm->frames[vm->frameCount - 1].function->localCount;
                        newFrame.returnRegister = 0;
                        newFrame.closure = closure;
                        newFrame.selfContext = s;
                        newFrame.classContext = c;
                        
                        for (size_t i = 0; i < actualArgs.size(); ++i) {
                            vm->registers[newFrame.registerBase + i] = actualArgs[i];
                        }
                        for (int i = static_cast<int>(actualArgs.size()); i < fnDef->localCount; ++i) {
                            vm->registers[newFrame.registerBase + i] = Value::none();
                        }
                        
                        vm->populateRefParams(newFrame, fnDef.get());
                        vm->frames[vm->frameCount++] = newFrame;
                        
                        int targetDepth = vm->frameCount - 1;
                        return vm->run(targetDepth);
                    }
                );

                for (int j = 0; j < fn->arity; ++j) {
                    closure->paramNames.push_back("_" + std::to_string(j));
                    closure->isRef.push_back(false);
                }
                int defaultLimit = fn->hasRestParam ? (fn->maxArity - 1) : fn->maxArity;
                for (int j = fn->arity; j < defaultLimit; ++j) {
                    closure->paramNames.push_back("_" + std::to_string(j));
                    closure->isRef.push_back(false);
                    closure->defaultValues.push_back(Value::none());
                }
                if (fn->hasRestParam) {
                    closure->paramNames.push_back("...rest");
                    closure->isRef.push_back(false);
                }
                closure->hasRestParam = fn->hasRestParam;
                closure->boundSelf = frame->selfContext;
                closure->boundClass = frame->classContext;
                break;
            }
            case OpCode::GET_UPVAL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (!frame->closure || b >= frame->closure->upvalueCount)
                    throw std::runtime_error("RegVM Error: Invalid upvalue index.");
                getReg(a) = *(frame->closure->upvalues[b]->location);
                break;
            }
            case OpCode::SET_UPVAL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (!frame->closure || b >= frame->closure->upvalueCount)
                    throw std::runtime_error("RegVM Error: Invalid upvalue index.");
                *(frame->closure->upvalues[b]->location) = getReg(a);
                break;
            }
            case OpCode::GET_REF_PARAM: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                if (frame->refParamsBase == -1) throw std::runtime_error("RegVM Error: Invalid ref param index.");
                getReg(a) = *(static_cast<ObjUpVal*>(registers[frame->refParamsBase + bx].asObj())->location);
                break;
            }
            case OpCode::SET_REF_PARAM: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                if (frame->refParamsBase == -1) throw std::runtime_error("RegVM Error: Invalid ref param index.");
                *(static_cast<ObjUpVal*>(registers[frame->refParamsBase + bx].asObj())->location) = getReg(a);
                break;
            }
            case OpCode::PASS_REFS: {
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                const auto& sig = chunk->callSignatures[bx];
                pendingCallRefs.clear();
                for (const auto& ref : sig.refs) {
                    uint8_t argIndex = ref.argIndex;
                    uint8_t sourceType = ref.sourceType;
                    uint32_t sourceRef = ref.sourceRef;

                    ObjUpVal* upval = nullptr;
                    switch (sourceType) {
                        case 1: {
                            std::string name = chunk->constants[sourceRef].asString();
                            upval = GcHeap::get().allocate<ObjUpVal>();
                            auto it = globalNames.find(name);
                            if (it == globalNames.end()) {
                                globalNames[name] = static_cast<uint32_t>(globals.size());
                                globals.push_back(Value::none());
                            }
                            upval->location = &globals[globalNames[name]];
                            break;
                        }
                        case 2: {
                            upval = captureUpvalue(frame->registerBase + sourceRef);
                            break;
                        }
                        case 3: {
                            if (frame->closure && sourceRef < static_cast<uint32_t>(frame->closure->upvalueCount)) {
                                upval = frame->closure->upvalues[sourceRef];
                            }
                            break;
                        }
                        case 4: {
                            if (frame->refParamsBase != -1) {
                                upval = static_cast<ObjUpVal*>(registers[frame->refParamsBase + sourceRef].asObj());
                            }
                            break;
                        }
                    }
                    if (upval) pendingCallRefs.push_back({ argIndex, upval });
                }
                break;
            }
            case OpCode::ADD: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() + vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    int64_t res = static_cast<int64_t>(v1) + v2;
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value(static_cast<int32_t>(res)); break; }
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_ADD)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RADD)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                try { getReg(a) = vb + vc; } catch (const std::exception& e) {
                    std::string msg = e.what();
                    if (msg.find("Type Error") != std::string::npos || msg.find("Cannot add") != std::string::npos) {
                        throw std::runtime_error("Type Error: Cannot add '" + getTypeName(vb) + "' and '" + getTypeName(vc) + "'.");
                    }
                    throw;
                }
                break;
            }
            case OpCode::SUB: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() - vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    int64_t res = static_cast<int64_t>(v1) - v2;
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value(static_cast<int32_t>(res)); break; }
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_SUB)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RSUB)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                try { getReg(a) = vb - vc; } catch (const std::exception& e) {
                    std::string msg = e.what();
                    if (msg.find("Type Error") != std::string::npos || msg.find("Cannot subtract") != std::string::npos) {
                        throw std::runtime_error("Type Error: Cannot subtract '" + getTypeName(vc) + "' from '" + getTypeName(vb) + "'.");
                    }
                    throw;
                }
                break;
            }
            case OpCode::MUL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() * vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    int64_t res = static_cast<int64_t>(v1) * v2;
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value(static_cast<int32_t>(res)); break; }
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_MUL)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RMUL)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                try { getReg(a) = vb * vc; } catch (const std::exception& e) {
                    std::string msg = e.what();
                    if (msg.find("Type Error") != std::string::npos || msg.find("Cannot multiply") != std::string::npos) {
                        throw std::runtime_error("Type Error: Cannot multiply '" + getTypeName(vb) + "' and '" + getTypeName(vc) + "'.");
                    }
                    throw;
                }
                break;
            }
            case OpCode::DIV: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { 
                    if (vc.asDoubleRaw() == 0.0) throw std::runtime_error("Math Error: Division by zero.");
                    getReg(a) = Value(vb.asDoubleRaw() / vc.asDoubleRaw()); break; 
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_DIV)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RDIV)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                try { getReg(a) = vb / vc; } catch (const std::exception& e) {
                    std::string msg = e.what();
                    if (msg.find("Type Error") != std::string::npos || msg.find("Cannot divide") != std::string::npos) {
                        throw std::runtime_error("Type Error: Cannot divide '" + getTypeName(vb) + "' by '" + getTypeName(vc) + "'.");
                    }
                    throw;
                }
                break;
            }
            case OpCode::MOD: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_MOD)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RMOD)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb % vc;
                break;
            }
            case OpCode::POW: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_POW)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RPOW)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb ^ vc;
                break;
            }
            case OpCode::LDIV: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_LDIV)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RLDIV)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = ldivide(vb, vc);
                break;
            }
            case OpCode::BAND: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITAND)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RBITAND)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb & vc;
                break;
            }
            case OpCode::BOR: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITOR)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RBITOR)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb | vc;
                break;
            }
            case OpCode::BXOR: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITXOR)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RBITXOR)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = bitXor(vb, vc);
                break;
            }
            case OpCode::SHL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_LSHIFT)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RLSHIFT)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb << vc;
                break;
            }
            case OpCode::SHR: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_RSHIFT)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RRSHIFT)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb >> vc;
                break;
            }
            case OpCode::UNM: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                Value vb = getReg(b);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_NEG)) { getReg(a) = callDunder(vb, meth, {}); break; } }
                getReg(a) = -vb;
                break;
            }
            case OpCode::NOT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                getReg(a) = Value(!evaluateTruthiness(getReg(b)));
                break;
            }
            case OpCode::BNOT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                Value vb = getReg(b);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITNOT)) { getReg(a) = callDunder(vb, meth, {}); break; } }
                getReg(a) = ~vb;
                break;
            }
            case OpCode::TO_BOOL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                getReg(a) = Value(evaluateTruthiness(getReg(b)));
                break;
            }
            case OpCode::EQ: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() == vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    getReg(a) = Value(v1 == v2); break;
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_EQ)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                getReg(a) = Value(Value::equals(vb, vc));
                break;
            }
            case OpCode::NEQ: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() != vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    getReg(a) = Value(v1 != v2); break;
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_NEQ)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_EQ)) { getReg(a) = Value(!evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                getReg(a) = Value(!Value::equals(vb, vc));
                break;
            }
            case OpCode::LT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() < vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    getReg(a) = Value(v1 < v2); break;
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_LT)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                getReg(a) = Value(vb < vc);
                break;
            }
            case OpCode::LE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() <= vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    getReg(a) = Value(v1 <= v2); break;
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_LE)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                getReg(a) = Value(vb <= vc);
                break;
            }
            case OpCode::GT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() > vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    getReg(a) = Value(v1 > v2); break;
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_GT)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                getReg(a) = Value(vb > vc);
                break;
            }
            case OpCode::GE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b); Value vc = getRK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() >= vc.asDoubleRaw()); break; }
                bool bIsInt = vb.isInt32() || vb.isBool();
                bool cIsInt = vc.isInt32() || vc.isBool();
                if (bIsInt && cIsInt) {
                    int32_t v1 = vb.isInt32() ? vb.asInt32() : (vb.asBool() ? 1 : 0);
                    int32_t v2 = vc.isInt32() ? vc.asInt32() : (vc.asBool() ? 1 : 0);
                    getReg(a) = Value(v1 >= v2); break;
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_GE)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                getReg(a) = Value(vb >= vc);
                break;
            }
            case OpCode::JMP: {
                frame->ip += sax;
                break;
            }
            case OpCode::JMP_TRUE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (evaluateTruthiness(getReg(a))) frame->ip += sbx;
                break;
            }
            case OpCode::JMP_FALSE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (!evaluateTruthiness(getReg(a))) frame->ip += sbx;
                break;
            }
            case OpCode::BUILD_LIST: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                ObjList* list = GcHeap::get().allocate<ObjList>();
                getReg(a) = Value(list); // ★ 立即 Root 防止 GC 误杀
                list->vec.reserve(c);
                for (int i = 0; i < c; ++i) {
                    list->vec.push_back(getReg(b + i));
                }
                break;
            }
            case OpCode::BUILD_DICT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                ObjDict* dict = GcHeap::get().allocate<ObjDict>();
                getReg(a) = Value(dict); // ★ 立即 Root 防止 GC 误杀
                dict->elements.reserve(c);
                dict->keyMap.reserve(c);
                for (int i = 0; i < c; ++i) {
                    dict->set(getReg(b + i * 2), getReg(b + i * 2 + 1));
                }
                break;
            }
            case OpCode::BUILD_SET: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                ObjSet* set = GcHeap::get().allocate<ObjSet>();
                getReg(a) = Value(set); // ★ 立即 Root 防止 GC 误杀
                set->elements.reserve(c);
                set->keys.reserve(c);
                for (int i = 0; i < c; ++i) {
                    set->add(getReg(b + i));
                }
                break;
            }
            case OpCode::BUILD_MATRIX: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                const auto& shape = chunk->matrixShapes[c];
                uint16_t rows = shape.rows;
                const std::vector<uint16_t>& rowCols = shape.rowCols;

                int total = 0;
                for (uint16_t cols : rowCols) total += cols;

                bool hasComplex = false;
                bool hasString = false;
                bool hasOther = false;

                auto canBeMatrixElement = [](const Value& v) -> bool {
                    return v.isNumber() || v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION) ||
                        v.isObjType(ObjType::BASENUM) || v.isObjType(ObjType::COMPLEX) || v.isString() ||
                        v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || v.isObjType(ObjType::STRING_MATRIX);
                };

                for (int ii = 0; ii < total; ++ii) {
                    const Value& v = getReg(b + ii);
                    if (v.isObjType(ObjType::COMPLEX) || v.isObjType(ObjType::COMPLEX_MATRIX)) hasComplex = true;
                    if (v.isString() || v.isObjType(ObjType::STRING_MATRIX)) hasString = true;
                    if (!canBeMatrixElement(v)) hasOther = true;
                }

                Value result;

                if (hasOther) {
                    if (rows == 1) {
                        ObjList* L = GcHeap::get().allocate<ObjList>();
                        getReg(a) = Value(L); // ★ 立即 Root 防止 GC 误杀
                        for (int ii = 0; ii < total; ++ii) L->vec.push_back(getReg(b + ii));
                        result = Value(L);
                    } else {
                        ObjList* outer = GcHeap::get().allocate<ObjList>();
                        getReg(a) = Value(outer); // ★ 立即 Root 防止 GC 误杀
                        int idx = 0;
                        for (int i = 0; i < rows; ++i) {
                            ObjList* inner = GcHeap::get().allocate<ObjList>();
                            int cols = rowCols[i];
                            for (int j = 0; j < cols; ++j) inner->vec.push_back(getReg(b + idx++));
                            inner->is_frozen = true;
                            outer->vec.push_back(Value(inner));
                        }
                        result = Value(outer);
                    }
                } else {
                    bool hasSubMatrix = false;
                    for (int ii = 0; ii < total; ++ii) {
                        const Value& v = getReg(b + ii);
                        if (v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || v.isObjType(ObjType::STRING_MATRIX)) hasSubMatrix = true;
                    }

                    if (hasSubMatrix) {
                        auto extractCell = [&](Value& cell) {
                            if (!cell.isObjType(ObjType::REAL_MATRIX) && !cell.isObjType(ObjType::COMPLEX_MATRIX) && !cell.isObjType(ObjType::STRING_MATRIX)) {
                                if (hasString) {
                                    std::ostringstream oss; oss << cell;
                                    cell = Value(StringMatrix(1, 1, { oss.str() }));
                                } else if (hasComplex) {
                                    cell = Value(ComplexMatrix(1, 1, { cell.asComplex() }));
                                } else {
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
                                } else if (cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                                    const auto& m = static_cast<ObjComplexMatrix*>(cell.asObj())->mat;
                                    std::vector<std::string> flat;
                                    for (int i = 0; i < m.getRows(); ++i)
                                        for (int j = 0; j < m.getCols(); ++j) {
                                            std::ostringstream oss; oss << Value(m(i, j));
                                            flat.push_back(oss.str());
                                        }
                                    cell = Value(StringMatrix(m.getRows(), m.getCols(), flat));
                                }
                            } else if (hasComplex && cell.isObjType(ObjType::REAL_MATRIX)) {
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
                                    Value cell = getReg(b + idx++);
                                    extractCell(cell);
                                    if (rowResult.isNone()) {
                                        rowResult = cell;
                                    } else {
                                        if (hasString) rowResult = Value(static_cast<ObjStringMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjStringMatrix*>(cell.asObj())->mat));
                                        else if (hasComplex) rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                                        else rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                                    }
                                }
                                if (matResult.isNone()) {
                                    matResult = rowResult;
                                } else {
                                    if (hasString) matResult = Value(static_cast<ObjStringMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjStringMatrix*>(rowResult.asObj())->mat));
                                    else if (hasComplex) matResult = Value(static_cast<ObjComplexMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat));
                                    else matResult = Value(static_cast<ObjRealMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat));
                                }
                            }
                            result = matResult;
                        } catch (...) {
                            throw std::runtime_error("RegVM Error: Dimension mismatch during block matrix concatenation.");
                        }
                    } else {
                        int expectedCols = rows > 0 ? rowCols[0] : 0;
                        bool uniformCols = true;
                        for (int i = 1; i < rows; ++i) {
                            if (rowCols[i] != expectedCols) { uniformCols = false; break; }
                        }
                        if (!uniformCols) throw std::runtime_error("RegVM Error: Matrix rows must have the same number of columns.");

                        if (hasString) {
                            std::vector<std::string> flat(total);
                            for (int ii = 0; ii < total; ++ii) {
                                const Value& v = getReg(b + ii);
                                if (v.isString()) flat[ii] = v.asString();
                                else { std::ostringstream oss; if (v.isUninit()) oss << "Uninitialized"; else oss << v; flat[ii] = oss.str(); }
                            }
                            result = Value(StringMatrix(rows, expectedCols, flat));
                        } else if (hasComplex) {
                            std::vector<Complex> flat(total);
                            for (int ii = 0; ii < total; ++ii) flat[ii] = getReg(b + ii).asComplex();
                            result = Value(ComplexMatrix(rows, expectedCols, flat));
                        } else {
                            std::vector<double> flat(total);
                            for (int ii = 0; ii < total; ++ii) flat[ii] = getReg(b + ii).asDouble();
                            result = Value(RealMatrix(rows, expectedCols, flat));
                        }
                    }
                }
                getReg(a) = result;
                break;
            }
            case OpCode::LIST_INIT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                getReg(a) = Value(GcHeap::get().allocate<ObjList>());
                break;
            }
            case OpCode::LIST_APPEND: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                Value& listVal = getReg(a);
                if (listVal.isObjType(ObjType::LIST)) {
                    static_cast<ObjList*>(listVal.asObj())->mut().push_back(getReg(b));
                } else {
                    throw std::runtime_error("RegVM Error: LIST_APPEND target is not a list.");
                }
                break;
            }
            case OpCode::STRINGIFY: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                Value v = getReg(b);
                if (v.isString()) {
                    getReg(a) = v;
                } else {
                    auto d = findDunder(v, DUNDER_STR);
                    if (d) {
                        getReg(a) = callDunder(v, d, {});
                    } else {
                        std::ostringstream oss;
                        if (v.isUninit()) oss << "Uninitialized";
                        else oss << v;
                        getReg(a) = Value(oss.str());
                    }
                }
                break;
            }
            case OpCode::CONCAT_STRINGS: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                bool allStrings = true;
                size_t totalLen = 0;
                for (int i = 0; i < c; ++i) {
                    Value& v = getReg(b + i);
                    if (v.isString()) {
                        totalLen += v.asString().size();
                    } else {
                        allStrings = false;
                        break;
                    }
                }
                
                std::string result;
                if (allStrings) {
                    result.reserve(totalLen);
                    for (int i = 0; i < c; ++i) {
                        result += getReg(b + i).asString();
                    }
                } else {
                    for (int i = 0; i < c; ++i) {
                        Value& v = getReg(b + i);
                        if (v.isString()) {
                            result += v.asString();
                        } else {
                            std::ostringstream oss;
                            if (v.isUninit()) oss << "Uninitialized";
                            else oss << v;
                            result += oss.str();
                        }
                    }
                }
                getReg(a) = Value(result);
                break;
            }
            case OpCode::FORMAT_STRING: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                const std::string& spec = chunk->constants[c].asString();
                Value val = getReg(b);

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
                getReg(a) = Value(result);
                break;
            }
            case OpCode::INDEX_GET: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                Value obj = getReg(b);
                if (c == 1) {
                    Value idx = getReg(b + 1);
                    Value result;
                    if (obj.isObjType(ObjType::DICT)) {
                        auto dict = static_cast<ObjDict*>(obj.asObj());
                        auto it = dict->keyMap.find(idx);
                        if (it == dict->keyMap.end()) {
                            throw std::runtime_error("RegVM Error: Key not found.");
                        }
                        result = dict->elements[it->second].second;
                    } else if (obj.isObjType(ObjType::LIST)) {
                        auto list = static_cast<ObjList*>(obj.asObj());
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = static_cast<int>(list->vec.size());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: List index out of bounds.");
                        result = list->vec[i];
                    } else if (obj.isString()) {
                        ObjString* objStr = obj.asObjString();
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int len = static_cast<int>(objStr->charLength);
                        if (i < 0) i = len + i;
                        if (i < 0 || i >= len) throw std::out_of_range("RegVM Error: String index out of bounds.");
                        result = Value(utf8::substring(objStr->str, i, 1, objStr->isAscii));
                    } else if (obj.isInstance()) {
                        auto inst = obj.asInstance();
                        auto cls = inst->classDef;
                        ObjClosure* getitemMethod = nullptr;
                        while (cls) {
                            auto it = cls->methods.find(DUNDER_GETITEM);
                            if (it != cls->methods.end()) {
                                getitemMethod = it->second;
                                break;
                            }
                            cls = cls->parent;
                        }
                        if (getitemMethod) {
                            result = callDunder(obj, getitemMethod, {idx});
                        } else {
                            throw std::runtime_error("RegVM Error: Cannot index this instance (no __getitem__).");
                        }
                    } else if (obj.isObjType(ObjType::REAL_MATRIX)) {
                        const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        if (m.getRows() == 1) result = Value(m(0, i));
                        else if (m.getCols() == 1) result = Value(m(i, 0));
                        else {
                            std::vector<double> row(m.getCols());
                            for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                            result = Value(RealMatrix(1, m.getCols(), row));
                        }
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        const auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        if (m.getRows() == 1) result = Value(m(0, i));
                        else if (m.getCols() == 1) result = Value(m(i, 0));
                        else {
                            std::vector<Complex> row(m.getCols());
                            for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                            result = Value(ComplexMatrix(1, m.getCols(), row));
                        }
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        const auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        if (m.getRows() == 1) result = Value(m(0, i));
                        else if (m.getCols() == 1) result = Value(m(i, 0));
                        else {
                            std::vector<std::string> row(m.getCols());
                            for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                            result = Value(StringMatrix(1, m.getCols(), row));
                        }
                    } else {
                        throw std::runtime_error("RegVM Error: Unsupported 1D index get.");
                    }
                    getReg(a) = result;
                } else if (c == 2) {
                    Value row = getReg(b + 1);
                    Value col = getReg(b + 2);
                    int r = static_cast<int>(std::round(row.asDouble()));
                    int c_idx = static_cast<int>(std::round(col.asDouble()));
                    Value result;
                    if (obj.isObjType(ObjType::REAL_MATRIX)) {
                        const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        result = Value(m(r, c_idx));
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        const auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        result = Value(m(r, c_idx));
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        const auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        result = Value(m(r, c_idx));
                    } else {
                        throw std::runtime_error("RegVM Error: Unsupported 2D index get.");
                    }
                    getReg(a) = result;
                } else {
                    throw std::runtime_error("RegVM Error: Unsupported index dimensionality.");
                }
                break;
            }
            case OpCode::INDEX_SET: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                Value obj = getReg(a);
                Value val = getReg(a + c + 1);
                
                if (c == 1) {
                    Value idx = getReg(a + 1);
                    if (obj.isObjType(ObjType::DICT)) {
                        auto dict = static_cast<ObjDict*>(obj.asObj());
                        dict->set(idx, val);
                    } else if (obj.isObjType(ObjType::LIST)) {
                        auto list = static_cast<ObjList*>(obj.asObj());
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = static_cast<int>(list->vec.size());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: List index out of bounds.");
                        list->mut()[i] = val;
                    } else if (obj.isInstance()) {
                        auto inst = obj.asInstance();
                        inst->checkModify();
                        auto cls = inst->classDef;
                        ObjClosure* setitemMethod = nullptr;
                        while (cls) {
                            auto it = cls->methods.find(DUNDER_SETITEM);
                            if (it != cls->methods.end()) {
                                setitemMethod = it->second;
                                break;
                            }
                            cls = cls->parent;
                        }
                        if (setitemMethod) {
                            callDunder(obj, setitemMethod, {idx, val});
                        } else {
                            throw std::runtime_error("RegVM Error: Cannot assign index on this instance (no __setitem__).");
                        }
                    } else if (obj.isObjType(ObjType::REAL_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        
                        if (m.getRows() == 1) m(0, i) = val.asDouble();
                        else if (m.getCols() == 1) m(i, 0) = val.asDouble();
                        else {
                            if (val.isObjType(ObjType::REAL_MATRIX)) {
                                const auto& src = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = src(0, j);
                                } else throw std::runtime_error("RegVM Error: Matrix row assignment dimension mismatch.");
                            } else throw std::runtime_error("RegVM Error: Matrix row assignment requires a row vector.");
                        }
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        
                        if (m.getRows() == 1) m(0, i) = val.asComplex();
                        else if (m.getCols() == 1) m(i, 0) = val.asComplex();
                        else {
                            if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                                const auto& src = static_cast<ObjComplexMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = src(0, j);
                                } else throw std::runtime_error("RegVM Error: Matrix row assignment dimension mismatch.");
                            } else if (val.isObjType(ObjType::REAL_MATRIX)) {
                                const auto& src = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = Complex(src(0, j));
                                } else throw std::runtime_error("RegVM Error: Matrix row assignment dimension mismatch.");
                            } else throw std::runtime_error("RegVM Error: Matrix row assignment requires a row vector.");
                        }
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        
                        if (m.getRows() == 1) m(0, i) = val.asString();
                        else if (m.getCols() == 1) m(i, 0) = val.asString();
                        else {
                            if (val.isObjType(ObjType::STRING_MATRIX)) {
                                const auto& src = static_cast<ObjStringMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = src(0, j);
                                } else throw std::runtime_error("RegVM Error: Matrix row assignment dimension mismatch.");
                            } else throw std::runtime_error("RegVM Error: Matrix row assignment requires a row vector.");
                        }
                        getReg(a) = obj;
                    } else {
                        throw std::runtime_error("RegVM Error: Unsupported 1D index set.");
                    }
                } else if (c == 2) {
                    Value row = getReg(a + 1);
                    Value col = getReg(a + 2);
                    int r = static_cast<int>(std::round(row.asDouble()));
                    int c_idx = static_cast<int>(std::round(col.asDouble()));
                    if (obj.isObjType(ObjType::REAL_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        m(r, c_idx) = val.asDouble();
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        m(r, c_idx) = val.asComplex();
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("RegVM Error: Matrix index out of bounds.");
                        if (val.isString()) m(r, c_idx) = val.asString();
                        else { std::ostringstream oss; oss << val; m(r, c_idx) = oss.str(); }
                        getReg(a) = obj;
                    } else {
                        throw std::runtime_error("RegVM Error: Unsupported 2D index set.");
                    }
                } else {
                    throw std::runtime_error("RegVM Error: Unsupported index dimensionality.");
                }
                break;
            }
            case OpCode::ITER_INIT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                uint8_t destructFlag = static_cast<uint8_t>(c);
                Value iterable = getReg(b);
                
                if (iterable.isInstance()) {
                    auto method = findDunder(iterable, DUNDER_ITER);
                    if (method) {
                        Value iterObj = callDunder(iterable, method, {});
                        getReg(a) = iterObj; // Custom iterator
                        break;
                    }
                }
                
                if (iterable.isObjType(ObjType::LIST)) {
                    ObjList* state = GcHeap::get().allocate<ObjList>();
                    state->vec.push_back(iterable);
                    state->vec.push_back(Value::fromInt32(0));
                    getReg(a) = Value(state);
                    break;
                }
                
                ObjList* elements = GcHeap::get().allocate<ObjList>();
                getReg(a) = Value(elements); // ★ 立即 Root 防止 GC 误杀
                
                if (iterable.isString()) {
                    ObjString* objStr = iterable.asObjString();
                    const std::string& s = objStr->str;
                    if (objStr->isAscii) {
                        for (char ch : s) elements->vec.push_back(Value(std::string(1, ch)));
                    } else {
                        size_t len = objStr->charLength;
                        for (size_t i = 0; i < len; ++i)
                            elements->vec.push_back(Value(utf8::substring(s, i, 1, false)));
                    }
                } else if (iterable.isObjType(ObjType::DICT)) {
                    const auto* d = static_cast<ObjDict*>(iterable.asObj());
                    if (destructFlag) {
                        for (const auto& [key, val] : d->elements) {
                            ObjList* pair = GcHeap::get().allocate<ObjList>();
                            pair->vec.push_back(key);
                            pair->vec.push_back(val);
                            pair->is_frozen = true;
                            elements->vec.push_back(Value(pair));
                        }
                    } else {
                        for (const auto& [key, val] : d->elements) {
                            elements->vec.push_back(key);
                        }
                    }
                } else if (iterable.isObjType(ObjType::SET)) {
                    const auto* s = static_cast<ObjSet*>(iterable.asObj());
                    for (const auto& val : s->elements) {
                        elements->vec.push_back(val);
                    }
                } else if (iterable.isObjType(ObjType::REAL_MATRIX)) {
                    const auto& m = static_cast<ObjRealMatrix*>(iterable.asObj())->mat;
                    if (m.getRows() == 1) {
                        for (int j = 0; j < m.getCols(); ++j) elements->vec.push_back(Value(m(0, j)));
                    } else if (m.getCols() == 1) {
                        for (int i = 0; i < m.getRows(); ++i) elements->vec.push_back(Value(m(i, 0)));
                    } else {
                        for (int i = 0; i < m.getRows(); ++i) {
                            std::vector<double> row(m.getCols());
                            for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                            elements->vec.push_back(Value(RealMatrix(1, m.getCols(), row)));
                        }
                    }
                } else if (iterable.isObjType(ObjType::COMPLEX_MATRIX)) {
                    const auto& m = static_cast<ObjComplexMatrix*>(iterable.asObj())->mat;
                    if (m.getRows() == 1) {
                        for (int j = 0; j < m.getCols(); ++j) elements->vec.push_back(Value(m(0, j)));
                    } else if (m.getCols() == 1) {
                        for (int i = 0; i < m.getRows(); ++i) elements->vec.push_back(Value(m(i, 0)));
                    } else {
                        for (int i = 0; i < m.getRows(); ++i) {
                            std::vector<Complex> row(m.getCols());
                            for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                            elements->vec.push_back(Value(ComplexMatrix(1, m.getCols(), row)));
                        }
                    }
                } else if (iterable.isObjType(ObjType::STRING_MATRIX)) {
                    const auto& m = static_cast<ObjStringMatrix*>(iterable.asObj())->mat;
                    if (m.getRows() == 1) {
                        for (int j = 0; j < m.getCols(); ++j) elements->vec.push_back(Value(m(0, j)));
                    } else if (m.getCols() == 1) {
                        for (int i = 0; i < m.getRows(); ++i) elements->vec.push_back(Value(m(i, 0)));
                    } else {
                        for (int i = 0; i < m.getRows(); ++i) {
                            std::vector<std::string> row(m.getCols());
                            for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                            elements->vec.push_back(Value(StringMatrix(1, m.getCols(), row)));
                        }
                    }
                } else if (iterable.isInstance()) {
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
                        } else {
                            for (const auto& [key, val] : inst->fields->elements) {
                                elements->vec.push_back(key);
                            }
                        }
                    }
                } else {
                    throw std::runtime_error("RegVM Error: Cannot iterate over this type.");
                }
                
                ObjList* state = GcHeap::get().allocate<ObjList>();
                state->vec.push_back(Value(elements));
                state->vec.push_back(Value::fromInt32(0));
                getReg(a) = Value(state);
                break;
            }
            case OpCode::ITER_NEXT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                
                Value& stateVal = getReg(b);
                
                if (stateVal.isObjType(ObjType::LIST)) {
                    auto state = static_cast<ObjList*>(stateVal.asObj());
                    if (state->vec.size() == 2 && state->vec[1].isInt32()) {
                        const auto& elems = static_cast<ObjList*>(state->vec[0].asObj())->vec; // ★ 修复 O(N^2) 性能 Bug：使用引用避免拷贝
                        int i = state->vec[1].asInt32();
                        if (i >= static_cast<int>(elems.size())) {
                            getReg(a) = Value::uninit();
                        } else {
                            getReg(a) = elems[i];
                            state->vec[1] = Value::fromInt32(i + 1);
                        }
                        break;
                    }
                }
                
                auto method = findDunder(stateVal, DUNDER_NEXT);
                if (!method) throw std::runtime_error("RegVM Error: Iterator missing __next__ method.");
                
                Value nextVal = callDunder(stateVal, method, {});
                if (nextVal.isNone()) {
                    getReg(a) = Value::uninit();
                } else {
                    getReg(a) = nextVal;
                }
                break;
            }
            case OpCode::IN: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                Value needle = getReg(b);
                Value haystack = getReg(c);
                bool found = false;
                
                if (needle.isString() && haystack.isString()) {
                    found = haystack.asString().find(needle.asString()) != std::string::npos;
                } else if (haystack.isObjType(ObjType::LIST)) {
                    const auto& L = static_cast<ObjList*>(haystack.asObj())->vec;
                    for (const auto& e : L) {
                        try {
                            if (Value::equals(needle, e)) {
                                found = true;
                                break;
                            }
                        } catch (...) {}
                    }
                } else if (haystack.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(haystack.asObj());
                    found = d->keyMap.find(needle) != d->keyMap.end();
                } else if (haystack.isObjType(ObjType::SET)) {
                    auto s = static_cast<ObjSet*>(haystack.asObj());
                    found = s->keys.find(needle) != s->keys.end();
                } else if (haystack.isObjType(ObjType::REAL_MATRIX)) {
                    const auto& m = static_cast<ObjRealMatrix*>(haystack.asObj())->mat;
                    if (needle.isNumber() || needle.isObjType(ObjType::BIGINT) || needle.isObjType(ObjType::FRACTION)) {
                        double nv = needle.asDouble();
                        for (int i = 0; i < m.getRows(); ++i) {
                            for (int j = 0; j < m.getCols(); ++j) {
                                if (m(i, j) == nv) { found = true; break; }
                            }
                            if (found) break;
                        }
                    }
                } else if (haystack.isObjType(ObjType::COMPLEX_MATRIX)) {
                    const auto& m = static_cast<ObjComplexMatrix*>(haystack.asObj())->mat;
                    if (needle.isNumber() || needle.isObjType(ObjType::BIGINT) || needle.isObjType(ObjType::FRACTION) || needle.isObjType(ObjType::COMPLEX)) {
                        Complex nv = needle.asComplex();
                        for (int i = 0; i < m.getRows(); ++i) {
                            for (int j = 0; j < m.getCols(); ++j) {
                                if (m(i, j) == nv) { found = true; break; }
                            }
                            if (found) break;
                        }
                    }
                } else if (haystack.isObjType(ObjType::STRING_MATRIX)) {
                    const auto& m = static_cast<ObjStringMatrix*>(haystack.asObj())->mat;
                    std::string nv;
                    if (needle.isString()) nv = needle.asString();
                    else { std::ostringstream oss; oss << needle; nv = oss.str(); }
                    for (int i = 0; i < m.getRows(); ++i) {
                        for (int j = 0; j < m.getCols(); ++j) {
                            if (m(i, j) == nv) { found = true; break; }
                        }
                        if (found) break;
                    }
                } else if (haystack.isInstance()) {
                    auto method = findDunder(haystack, DUNDER_CONTAINS);
                    if (method) {
                        found = evaluateTruthiness(callDunder(haystack, method, {needle}));
                    } else {
                        auto inst = haystack.asInstance();
                        if (inst->fields && inst->fields->keyMap.find(needle) != inst->fields->keyMap.end()) {
                            found = true;
                        } else if (needle.isString()) {
                            auto cls = inst->classDef;
                            std::string key = needle.asString();
                            while (cls) {
                                if (cls->methods.find(key) != cls->methods.end()) {
                                    found = true;
                                    break;
                                }
                                cls = cls->parent;
                            }
                            if (!found) {
                                auto getattrMethod = findDunder(haystack, DUNDER_GETATTR);
                                if (getattrMethod) {
                                    try {
                                        callDunder(haystack, getattrMethod, {needle});
                                        found = true;
                                    } catch (...) {
                                        // Fall through to false
                                    }
                                }
                            }
                        }
                    }
                } else {
                    throw std::runtime_error("RegVM Error: 'in' requires a string, list, dict, set, matrix, or instance.");
                }
                
                getReg(a) = Value(found);
                break;
            }
            case OpCode::TRY_BEGIN: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                ExceptionHandler handler;
                handler.frameIndex = frameCount - 1;
                handler.ip = frame->ip + sbx;
                handler.registerBase = frame->registerBase;
                handler.errReg = a;
                exceptionHandlers.push_back(handler);
                break;
            }
            case OpCode::TRY_END: {
                if (!exceptionHandlers.empty()) {
                    exceptionHandlers.pop_back();
                }
                break;
            }
            case OpCode::THROW: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value errVal = getReg(a);
                throw ValueException(errVal);
            }
            case OpCode::CLASS: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                const std::string& name = chunk->constants[bx].asString();
                auto cls = GcHeap::get().allocate<ObjClass>();
                cls->name = name;
                getReg(a) = Value(cls);
                break;
            }
            case OpCode::METHOD: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                const std::string& methodName = chunk->constants[b].asString();
                Value classVal = getReg(a);
                Value closureVal = getReg(c);
                
                if (!classVal.isClass()) throw std::runtime_error("RegVM Error: METHOD requires a class.");
                auto cls = static_cast<ObjClass*>(classVal.asObj());
                
                if (closureVal.isFunctionClosure()) {
                    cls->methods[methodName] = closureVal.asFunction();
                } else {
                    throw std::runtime_error("RegVM Error: Invalid closure type for method.");
                }
                break;
            }
            case OpCode::INHERIT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                
                Value subClass = getReg(a);
                Value superClass = getReg(b);
                
                if (!subClass.isClass() || !superClass.isClass()) throw std::runtime_error("RegVM Error: Inheritance requires two classes.");
                auto sub = static_cast<ObjClass*>(subClass.asObj());
                auto sup = static_cast<ObjClass*>(superClass.asObj());
                
                sub->parent = sup;
                for (auto& [name, method] : sup->methods) {
                    if (sub->methods.find(name) == sub->methods.end()) {
                        sub->methods[name] = method;
                    }
                }
                break;
            }
            case OpCode::GET_PROP: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[c]);
                const std::string& field = chunk->constants[ic.nameIdx].asString();
                Value obj = getReg(b);
                bool found = false;
                Value result;
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    if (inst->fields) {
                        auto it = inst->fields->keyMap.find(chunk->constants[ic.nameIdx]);
                        if (it != inst->fields->keyMap.end()) {
                            result = inst->fields->elements[it->second].second;
                            found = true;
                        }
                    }
                    if (!found) {
                        auto cls = inst->classDef;
                        while (cls) {
                            auto it = cls->methods.find(field);
                            if (it != cls->methods.end()) {
                                auto rawMethod = it->second;
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->paramNames = rawMethod->paramNames;
                                bound->isRef = rawMethod->isRef;
                                bound->defaultValues = rawMethod->defaultValues;
                                bound->hasRestParam = rawMethod->hasRestParam;
                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                bound->nativeFn = rawMethod->nativeFn;
                                bound->boundSelf = Value(inst);
                                bound->boundClass = Value(cls);
                                result = Value(bound);
                                found = true;
                                break;
                            }
                            cls = cls->parent;
                        }
                        if (!found) {
                            auto getattrMethod = findDunder(obj, DUNDER_GETATTR);
                            if (getattrMethod) {
                                result = callDunder(obj, getattrMethod, {Value(field)});
                                found = true;
                            }
                        }
                    }
                } else if (obj.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(obj.asObj());
                    auto it = d->keyMap.find(chunk->constants[ic.nameIdx]);
                    if (it != d->keyMap.end()) {
                        result = d->elements[it->second].second;
                        found = true;
                    }
                } else if (obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    auto it = ns->fields.find(field);
                    if (it != ns->fields.end()) {
                        result = *(it->second.upval->location);
                        found = true;
                    }
                }
                
                if (!found) {
                    auto nIt = jc::VM::activeVM->getNativeBuiltins().find(field);
                    if (nIt != jc::VM::activeVM->getNativeBuiltins().end()) {
                        auto bound = GcHeap::get().allocate<ObjClosure>(
                            std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                        );
                        bound->boundSelf = obj;
                        NativeCallable nativeFn = nIt->second;
                        
                        auto ait = jc::VM::activeVM->getBuiltinArity().find(field);
                        std::set<int> allowedArities;
                        if (ait != jc::VM::activeVM->getBuiltinArity().end()) allowedArities = ait->second;

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
                        auto gIt = globalNames.find(field);
                        if (gIt != globalNames.end() && globals[gIt->second].isFunctionClosure()) {
                            auto bound = GcHeap::get().allocate<ObjClosure>(
                                std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                            );
                            bound->boundSelf = obj;
                            ObjClosure* targetFn = globals[gIt->second].asFunction();
                        
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
                                bound->isUFCS = true;
                                bound->nativeFn = targetFn->nativeFn;
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
                                bound->boundClass = Value(targetFn);
                            }
                            result = Value(bound);
                            found = true;
                        }
                    }
                }

                if (!found) throw std::runtime_error("RegVM Error: Property '" + field + "' not found.");
                getReg(a) = result;
                break;
            }
            case OpCode::TRY_GET_PROP: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[c]);
                const std::string& field = chunk->constants[ic.nameIdx].asString();
                Value obj = getReg(b);
                bool found = false;
                Value result;
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    if (ic.cachedClass == inst->classDef && ic.cachedFieldIndex != -1 && inst->fields && ic.cachedFieldIndex < static_cast<int>(inst->fields->elements.size())) {
                        if (inst->fields->elements[ic.cachedFieldIndex].first.asString() == field) {
                            result = inst->fields->elements[ic.cachedFieldIndex].second;
                            found = true;
                        }
                    }
                    if (!found) {
                        if (inst->fields) {
                            auto it = inst->fields->keyMap.find(chunk->constants[ic.nameIdx]);
                            if (it != inst->fields->keyMap.end()) {
                                result = inst->fields->elements[it->second].second;
                                found = true;
                                ic.cachedClass = inst->classDef;
                                ic.cachedFieldIndex = static_cast<int>(it->second);
                            }
                        }
                        if (!found) {
                            auto getattrMethod = findDunder(obj, "__getattr__");
                            if (getattrMethod) {
                                try {
                                    result = callDunder(obj, getattrMethod, {Value(field)});
                                    found = true;
                                } catch (...) {
                                    found = false;
                                }
                            }
                        }
                    }
                } else if (obj.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(obj.asObj());
                    auto it = d->keyMap.find(chunk->constants[ic.nameIdx]);
                    if (it != d->keyMap.end()) {
                        result = d->elements[it->second].second;
                        found = true;
                    }
                } else if (obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    auto it = ns->fields.find(field);
                    if (it != ns->fields.end()) {
                        result = *(it->second.upval->location);
                        found = true;
                    }
                }
                
                if (found) {
                    getReg(a) = result;
                    getReg(a + 1) = Value(true);
                } else {
                    getReg(a) = Value::none();
                    getReg(a + 1) = Value(false);
                }
                break;
            }
            case OpCode::SET_PROP: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[b]);
                const std::string& field = chunk->constants[ic.nameIdx].asString();
                Value obj = getReg(a);
                Value val = getReg(c);
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    inst->checkModify();
                    auto setattrMethod = findDunder(obj, DUNDER_SETATTR);
                    if (setattrMethod) {
                        callDunder(obj, setattrMethod, {Value(field), val});
                    } else {
                        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
                        Value key = chunk->constants[ic.nameIdx];
                        auto it = inst->fields->keyMap.find(key);
                        if (it != inst->fields->keyMap.end()) {
                            inst->fields->elements[it->second].second = val;
                        } else {
                            inst->fields->keyMap[key] = inst->fields->elements.size();
                            inst->fields->elements.push_back({key, val});
                        }
                    }
                } else if (obj.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(obj.asObj());
                    d->set(chunk->constants[ic.nameIdx], val);
                } else {
                    throw std::runtime_error("RegVM Error: Cannot set property on this type.");
                }
                break;
            }
            case OpCode::DICT_REST: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                Value obj = getReg(b);
                Value excludeKeysVal = getReg(c);
                std::unordered_set<std::string> excludeKeys;
                if (excludeKeysVal.isObjType(ObjType::LIST)) {
                    for (const auto& k : static_cast<ObjList*>(excludeKeysVal.asObj())->vec) {
                        if (k.isString()) excludeKeys.insert(k.asString());
                    }
                }
                
                ObjDict* restDict = GcHeap::get().allocate<ObjDict>();
                getReg(a) = Value(restDict); // ★ 立即 Root 防止 GC 误杀
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
                break;
            }
            case OpCode::BUILD_NAMESPACE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                const std::string& nsName = chunk->constants[b].asString();
                ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
                getReg(a) = Value(ns); // ★ 立即 Root 防止 GC 误杀
                ns->name = nsName;
                
                for (int i = 0; i < c; ++i) {
                    Value keyVal = getReg(a + 1 + i * 3);
                    Value slotVal = getReg(a + 1 + i * 3 + 1);
                    Value isConstVal = getReg(a + 1 + i * 3 + 2);
                    
                    std::string key = keyVal.asString();
                    int slot = static_cast<int>(slotVal.asDouble());
                    bool isConst = isConstVal.truthy();
                    
                    ObjUpVal* upval = captureUpvalue(frame->registerBase + slot);
                    ns->fields[key] = { upval, isConst };
                }
                break;
            }
            case OpCode::LIST_COMP_END: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value arg = getReg(a);
                if (!arg.isObjType(ObjType::LIST)) break;
                
                auto l = static_cast<ObjList*>(arg.asObj());
                if (l->vec.empty()) {
                    getReg(a) = Value(RealMatrix(1, 0));
                    break;
                }
                
                bool hasComplex = false;
                bool hasString = false;
                bool hasOther = false;
                bool hasSubMatrix = false;

                auto canBeMatrixElement = [](const Value& v) -> bool {
                    return v.isNumber() || v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION) ||
                           v.isObjType(ObjType::BASENUM) || v.isObjType(ObjType::COMPLEX) || v.isString() ||
                           v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || v.isObjType(ObjType::STRING_MATRIX);
                };

                for (const auto& v : l->vec) {
                    if (v.isObjType(ObjType::COMPLEX) || v.isObjType(ObjType::COMPLEX_MATRIX)) hasComplex = true;
                    if (v.isString() || v.isObjType(ObjType::STRING_MATRIX)) hasString = true;
                    if (v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || v.isObjType(ObjType::STRING_MATRIX)) hasSubMatrix = true;
                    if (!canBeMatrixElement(v)) hasOther = true;
                }

                if (hasOther) break;

                int total = static_cast<int>(l->vec.size());

                if (hasSubMatrix) {
                    auto extractCell = [&](Value& cell) {
                        if (!cell.isObjType(ObjType::REAL_MATRIX) && !cell.isObjType(ObjType::COMPLEX_MATRIX) && !cell.isObjType(ObjType::STRING_MATRIX)) {
                            if (hasString) {
                                std::ostringstream oss; oss << cell;
                                cell = Value(StringMatrix(1, 1, { oss.str() }));
                            } else if (hasComplex) {
                                cell = Value(ComplexMatrix(1, 1, { cell.asComplex() }));
                            } else {
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
                            } else if (cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                                const auto& m = static_cast<ObjComplexMatrix*>(cell.asObj())->mat;
                                std::vector<std::string> flat;
                                for (int i = 0; i < m.getRows(); ++i)
                                    for (int j = 0; j < m.getCols(); ++j) {
                                        std::ostringstream oss; oss << Value(m(i, j));
                                        flat.push_back(oss.str());
                                    }
                                cell = Value(StringMatrix(m.getRows(), m.getCols(), flat));
                            }
                        } else if (hasComplex && cell.isObjType(ObjType::REAL_MATRIX)) {
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
                            } else {
                                if (hasString)
                                    rowResult = Value(static_cast<ObjStringMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjStringMatrix*>(cell.asObj())->mat));
                                else if (hasComplex)
                                    rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                                else
                                    rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                            }
                        }
                        getReg(a) = rowResult;
                    } catch (...) {
                        throw std::runtime_error("RegVM Error: Dimension mismatch during list comprehension matrix concatenation.");
                    }
                } else if (hasString) {
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
                    getReg(a) = Value(StringMatrix(1, total, flat));
                } else if (hasComplex) {
                    std::vector<Complex> flat(total);
                    for (int ii = 0; ii < total; ++ii) flat[ii] = l->vec[ii].asComplex();
                    getReg(a) = Value(ComplexMatrix(1, total, flat));
                } else {
                    std::vector<double> flat(total);
                    for (int ii = 0; ii < total; ++ii) flat[ii] = l->vec[ii].asDouble();
                    getReg(a) = Value(RealMatrix(1, total, flat));
                }
                break;
            }
            case OpCode::SLICE_GET: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                execSliceGet(a, b, static_cast<uint8_t>(c));
                break;
            }
            case OpCode::SLICE_SET: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                execSliceSet(a, c, static_cast<uint8_t>(c));
                break;
            }
            case OpCode::IMPORT: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                Value pathVal = getReg(b);
                if (!pathVal.isString()) throw std::runtime_error("RegVM Error: import requires a string path.");
                getReg(a) = execImport(pathVal.asString());
                break;
            }
            case OpCode::ASSERT_PARAM_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                execAssertParamType(getReg(a), b, c);
                break;
            }
            case OpCode::ASSERT_RETURN_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                execAssertReturnType(getReg(a), b);
                break;
            }
            case OpCode::MATCH_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[c]);
                if (ic.cachedBuiltinType == BuiltinType::UNKNOWN) {
                    ic.cachedBuiltinType = parseBuiltinType(chunk->constants[ic.nameIdx].asString());
                }
                const std::string& typeStr = chunk->constants[ic.nameIdx].asString();
                getReg(a) = Value(checkValueType(getReg(b), ic.cachedBuiltinType, typeStr));
                break;
            }
            case OpCode::MATCH_SHAPE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                const auto& sp = chunk->shapePatterns[c];
                uint32_t minRows = sp.minRows;
                uint32_t maxRows = sp.maxRows;
                uint32_t minCols = sp.minCols;
                uint32_t maxCols = sp.maxCols;
                uint8_t exactMask = sp.exactMask;
                Value val = getReg(b);
                bool matched = false;
                
                bool is1DPattern = (exactMask & 2) != 0;

                if (val.isObjType(ObjType::LIST)) {
                    if (is1DPattern) {
                        uint32_t len = static_cast<uint32_t>(static_cast<ObjList*>(val.asObj())->vec.size());
                        matched = (len >= minCols && (maxCols == 0xFFFFFFFF || len <= maxCols));
                    }
                } else if (val.isString()) {
                    if (is1DPattern) {
                        uint32_t len = static_cast<uint32_t>(val.asObjString()->charLength);
                        matched = (len >= minCols && (maxCols == 0xFFFFFFFF || len <= maxCols));
                    }
                } else if (val.isObjType(ObjType::REAL_MATRIX)) {
                    const auto& m = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                    if (is1DPattern) {
                        if (m.getRows() == 0 && m.getCols() == 0) matched = (0U >= minCols && (maxCols == 0xFFFFFFFF || 0U <= maxCols));
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
                        if (m.getRows() == 0 && m.getCols() == 0) matched = (0U >= minCols && (maxCols == 0xFFFFFFFF || 0U <= maxCols));
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
                        if (m.getRows() == 0 && m.getCols() == 0) matched = (0U >= minCols && (maxCols == 0xFFFFFFFF || 0U <= maxCols));
                        else matched = (m.getRows() == 1) && (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                    } else {
                        bool rMatch = (static_cast<uint32_t>(m.getRows()) >= minRows && (maxRows == 0xFFFFFFFF || static_cast<uint32_t>(m.getRows()) <= maxRows));
                        bool cMatch = (static_cast<uint32_t>(m.getCols()) >= minCols && (maxCols == 0xFFFFFFFF || static_cast<uint32_t>(m.getCols()) <= maxCols));
                        if (minRows == 1 && minCols == 0 && m.getRows() == 0 && m.getCols() == 0) matched = true;
                        else matched = rMatch && cMatch;
                    }
                }
                getReg(a) = Value(matched);
                break;
            }
            case OpCode::INVOKE:
            case OpCode::TAIL_INVOKE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                bool isTailCall = (op == OpCode::TAIL_INVOKE);
                int prevIp = frame->ip;
                execInvoke(a, b, c, isTailCall, -1, 0);
                if (isTailCall && frame->ip == prevIp) {
                    Value res = getReg(a);
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                }
                break;
            }
            case OpCode::INVOKE_FALLBACK:
            case OpCode::TAIL_INVOKE_FALLBACK: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                const auto& fbInfo = chunk->fallbackInfos[c];
                bool isTailCall = (op == OpCode::TAIL_INVOKE_FALLBACK);
                int prevIp = frame->ip;
                execInvoke(a, b, fbInfo.icIdx, isTailCall, fbInfo.fbType, fbInfo.fbIdx);
                if (isTailCall && frame->ip == prevIp) {
                    Value res = getReg(a);
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                }
                break;
            }
            case OpCode::GET_SUPER: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                
                const std::string& field = chunk->constants[b].asString();
                Value selfVal = getReg(a);
                if (!selfVal.isInstance()) throw std::runtime_error("RegVM Error: 'super' requires an instance context.");
                auto inst = selfVal.asInstance();
                
                Value classVal = frame->classContext;
                if (!classVal.isClass()) throw std::runtime_error("RegVM Error: 'super' requires class context.");
                auto currentClass = static_cast<ObjClass*>(classVal.asObj());
                auto parentClass = currentClass->parent;
                if (!parentClass) throw std::runtime_error("RegVM Error: No parent class.");
                
                ObjClosure* rawMethod = nullptr;
                ObjClass* ownerClass = nullptr;
                auto cls = parentClass;
                while (cls) {
                    auto it = cls->methods.find(field);
                    if (it != cls->methods.end()) {
                        rawMethod = it->second;
                        ownerClass = cls;
                        break;
                    }
                    cls = cls->parent;
                }
                if (!rawMethod) throw std::runtime_error("RegVM Error: Parent class has no method '" + field + "'.");
                
                auto bound = GcHeap::get().allocate<ObjClosure>(
                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
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
                
                getReg(a) = Value(bound);
                break;
            }
            case OpCode::SUPER_INVOKE:
            case OpCode::TAIL_SUPER_INVOKE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                
                bool isTailCall = (op == OpCode::TAIL_SUPER_INVOKE);
                int prevIp = frame->ip;
                execSuperInvoke(a, b, c, isTailCall);
                if (isTailCall && frame->ip == prevIp) {
                    Value res = getReg(a);
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                }
                break;
            }
            case OpCode::GET_SELF: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (frame->selfContext.isNone()) throw std::runtime_error("RegVM Error: 'self' accessed outside of context.");
                getReg(a) = frame->selfContext;
                break;
            }
            case OpCode::CALL:
            case OpCode::TAIL_CALL: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                
                bool isTailCall = (op == OpCode::TAIL_CALL);
                int prevIp = frame->ip;
                execCall(a, b, isTailCall);
                if (isTailCall && frame->ip == prevIp) {
                    Value res = getReg(a);
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                }
                break;
            }
            case OpCode::RETURN: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value res = getReg(a);
                
                while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                    exceptionHandlers.pop_back();
                }
                
                closeUpvalues(frame->registerBase);
                
                int targetReg = frame->returnRegister;
                bool isInit = (frame->function && frame->function->name == "init");
                Value selfCtx = frame->selfContext;

                frameCount--;
                if (frameCount <= targetFrameDepth) {
                    return res;
                }
                
                // 恢复调用方帧状态
                frame = &frames[frameCount - 1];
                chunk = frame->chunk;
                code = chunk->code.data();
                
                if (isInit) {
                    getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                } else {
                    getReg(targetReg) = res;
                }
                break;
            }
            default:
                throw std::runtime_error("RegVM Error: Unimplemented opcode " + std::to_string(static_cast<int>(op)));
        }
        } catch (const ValueException& ex) {
            if (!handleExceptionUnwind(ex.val)) {
                int line = (frame->ip > 0 && frame->ip <= static_cast<int>(chunk->lines.size())) ? chunk->lines[frame->ip - 1] : 0;
                std::string msg = ex.val.isString() ? ex.val.asString() : "ValueException";
                throw std::runtime_error("[Line " + std::to_string(line) + "] " + msg);
            }
            frame = &frames[frameCount - 1];
            chunk = frame->chunk;
            code = chunk->code.data();
        } catch (const std::exception& ex) {
            if (!handleExceptionUnwind(Value(ex.what()))) {
                int line = (frame->ip > 0 && frame->ip <= static_cast<int>(chunk->lines.size())) ? chunk->lines[frame->ip - 1] : 0;
                std::string msg = ex.what();
                if (msg.find("[Line ") != 0) {
                    msg = "[Line " + std::to_string(line) + "] " + msg;
                }
                throw std::runtime_error(msg);
            }
            frame = &frames[frameCount - 1];
            chunk = frame->chunk;
            code = chunk->code.data();
        } catch (...) {
            if (!handleExceptionUnwind(Value("Unknown VM Error"))) {
                int line = (frame->ip > 0 && frame->ip <= static_cast<int>(chunk->lines.size())) ? chunk->lines[frame->ip - 1] : 0;
                throw std::runtime_error("[Line " + std::to_string(line) + "] Unknown VM Error");
            }
            frame = &frames[frameCount - 1];
            chunk = frame->chunk;
            code = chunk->code.data();
        }
    }
}

} // namespace regvm
} // namespace jc
