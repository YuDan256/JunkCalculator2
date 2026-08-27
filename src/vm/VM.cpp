#define _CRT_SECURE_NO_WARNINGS
#include "VM.h"
#include "../memory/GcHeap.h"
#include "../frontend/Utf8.h"
#include "../frontend/Lexer.h"
#include "../frontend/Parser.h"
#include "../lib/ExtensionBridge.h"
#include "../frontend/Highlight.h"
#include "../compiler/IRBuilder.h"
#include "../compiler/IROptimizer.h"
#include "../compiler/RegisterAllocator.h"
#include "../compiler/Emitter.h"
#include "EngineInterrupt.h"
#include "BytecodeSerializer.h"
#include "../jit/runtime/Deoptimization.h"
#include "../jit/frontend/BytecodeCFG.h"
#include "../jit/frontend/BytecodeToHIR.h"
#include "../jit/pass/DeadPhiElimination.h"
#include "../jit/pass/ConstantFolding.h"
#include "../jit/pass/AlgebraicSimplification.h"
#include "../jit/pass/CSE.h"
#include "../jit/pass/DCE.h"
#include "../jit/pass/GCM.h"
#include "../jit/pass/InstructionSelector.h"
#include "../jit/pass/LivenessAnalysis.h"
#include "../jit/pass/LinearScan.h"
#include "../jit/backend/CodeEmitter.h"
#include "../jit/backend/Disassembler.h"
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace jc {
    inline std::filesystem::path to_path(const std::string& utf8_str) {
        return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8_str.c_str()));
    }
    inline std::string from_path(const std::filesystem::path& p) {
        auto u8str = p.u8string();
        return std::string(u8str.begin(), u8str.end());
    }
}

extern bool g_showIR;
extern bool g_showHIR;
extern bool g_showMachineCode;
extern bool g_autoDebug;
extern bool g_profile;
extern bool g_enableJit;

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#undef IN
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#define JIT_CALLOUT_TRY try {
#define JIT_CALLOUT_CATCH \
    } catch (const ValueException& e) { \
        VM::activeVM->jit_exception_value = e.val; \
        jit::g_jit_pending_exception = 1; \
        return 0; \
    } catch (const RuntimeError& e) { \
        VM::activeVM->jit_exception_value = VM::activeVM->wrapException(e.type, e.message); \
        jit::g_jit_pending_exception = 1; \
        return 0; \
    } catch (const std::exception& e) { \
        VM::activeVM->jit_exception_value = Value(std::string(e.what())); \
        jit::g_jit_pending_exception = 1; \
        return 0; \
    } catch (...) { \
        VM::activeVM->jit_exception_value = Value("Unknown JIT Exception"); \
        jit::g_jit_pending_exception = 1; \
        return 0; \
    }

#define JIT_CALLOUT_CATCH_VOID \
    } catch (const ValueException& e) { \
        VM::activeVM->jit_exception_value = e.val; \
        jit::g_jit_pending_exception = 1; \
        return; \
    } catch (const RuntimeError& e) { \
        VM::activeVM->jit_exception_value = VM::activeVM->wrapException(e.type, e.message); \
        jit::g_jit_pending_exception = 1; \
        return; \
    } catch (const std::exception& e) { \
        VM::activeVM->jit_exception_value = Value(std::string(e.what())); \
        jit::g_jit_pending_exception = 1; \
        return; \
    } catch (...) { \
        VM::activeVM->jit_exception_value = Value("Unknown JIT Exception"); \
        jit::g_jit_pending_exception = 1; \
        return; \
    }

static std::string manglePrivate(uint64_t classId, const std::string& name) {
    return "<" + std::to_string(classId) + "::" + name + ">";
}

namespace jc {

// 核心索引赋值（定义在下方 JIT callout 区域；解释器 INDEX_SET 也复用）
static Value vmIndexSetCore(VM* vm, Value obj, std::vector<Value>& args, Value val);

uint64_t jc2_jit_call_helper(uint64_t callee_bits, Value* current_regs, uint64_t* arg_bits, uint32_t argc) {
    JIT_CALLOUT_TRY
    (void)current_regs;
    Value callee = Value::fromRawBits(callee_bits);
    std::vector<Value> args(argc);
    for (uint32_t i = 0; i < argc; ++i) args[i] = Value::fromRawBits(arg_bits[i]);
    
    while (true) {
        if (callee.isString()) {
            const std::string& tag = callee.asString();
            Value gVal = VM::activeVM->getGlobal(tag);
            if (!gVal.isNone()) {
                callee = gVal;
                continue;
            } else {
                auto nIt = VM::activeVM->getNativeBuiltins().find(tag);
                if (nIt != VM::activeVM->getNativeBuiltins().end()) {
                    callee = VM::activeVM->getBuiltinClosure(tag);
                    continue;
                } else {
                    throw std::runtime_error("VM Error: Unknown function or not callable '" + tag + "'.");
                }
            }
        }

        if (callee.isType()) {
            ObjTypeDef* td = static_cast<ObjTypeDef*>(callee.asObj());
            if (td->converter) {
                Value res = td->converter(args);
                VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
                return res.as_bits;
            }
            if (td->types.size() == 1 && std::holds_alternative<BuiltinType>(td->types[0])) {
                BuiltinType bt = std::get<BuiltinType>(td->types[0]);
                if (bt == BuiltinType::TYPE_DEF) {
                    if (argc != 1) throw std::runtime_error("TypeError: type() expects 1 argument.");
                    Value v = args[0];
                    std::vector<std::variant<BuiltinType, ObjClass*>> newTypes;
                    if (v.isType()) {
                        newTypes.push_back(BuiltinType::TYPE_DEF);
                    } else if (v.isClass()) {
                        newTypes.push_back(BuiltinType::CLASS);
                    } else if (v.isInstance()) {
                        newTypes.push_back(v.asInstance()->classDef);
                    } else {
                        BuiltinType vbt = BuiltinType::ANY;
                        if (v.isInt32() || v.isBigInt()) vbt = BuiltinType::INT;
                        else if (v.isDouble()) vbt = BuiltinType::FLOAT;
                        else if (v.isString()) vbt = BuiltinType::STRING;
                        else if (v.isBool()) vbt = BuiltinType::BOOL;
                        else if (v.isNone()) vbt = BuiltinType::NONE_TYPE;
                        else if (v.isObjType(ObjType::LIST)) vbt = BuiltinType::LIST;
                        else if (v.isObjType(ObjType::DICT)) vbt = BuiltinType::DICT;
                        else if (v.isObjType(ObjType::SET)) vbt = BuiltinType::SET;
                        else if (v.isObjType(ObjType::FRACTION)) vbt = BuiltinType::FRACTION;
                        else if (v.isObjType(ObjType::COMPLEX)) vbt = BuiltinType::COMPLEX;
                        else if (v.isObjType(ObjType::SYMBOLIC)) vbt = BuiltinType::SYMBOLIC;
                        else if (v.isObjType(ObjType::REAL_MATRIX)) vbt = BuiltinType::REALMAT;
                        else if (v.isObjType(ObjType::COMPLEX_MATRIX)) vbt = BuiltinType::COMPLEXMAT;
                        else if (v.isObjType(ObjType::SYM_MATRIX)) vbt = BuiltinType::SYMMAT;
                        else if (v.isFunctionClosure()) vbt = BuiltinType::FUNC;
                        else if (v.isObjType(ObjType::NAMESPACE)) vbt = BuiltinType::NAMESPACE;
                        else if (v.isObjType(ObjType::SLICE)) vbt = BuiltinType::SLICE;
                        newTypes.push_back(vbt);
                    }
                    Value res(internType(std::move(newTypes)));
                    VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
                    return res.as_bits;
                }
            }
            throw std::runtime_error("TypeError: This type object is not callable.");
        }

        if (callee.isFunctionClosure()) {
            ObjClosure* cl = callee.asFunction();
            if (cl->isBytecode()) {
                int fnIdx = cl->compiledFnIndex;
                Value res = VM::activeVM->callVMFunction(fnIdx, args, cl, cl->boundSelf, cl->boundClass);
                VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
                return res.as_bits;
            } else if (cl->isNative()) {
                helpers::nativeSelfStack.push_back(cl->boundSelf);
                helpers::nativeClassStack.push_back(cl->boundClass);
                Value res;
                try {
                    auto& fn = std::any_cast<NativeCallable&>(cl->nativeFn);
                    res = fn(args);
                } catch (...) {
                    helpers::nativeSelfStack.pop_back();
                    helpers::nativeClassStack.pop_back();
                    throw;
                }
                helpers::nativeSelfStack.pop_back();
                helpers::nativeClassStack.pop_back();
                VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
                return res.as_bits;
            }
        } else if (callee.isClass()) {
            auto cls = static_cast<ObjClass*>(callee.asObj());
            if (cls->native_allocator) {
                Value res = cls->native_allocator(args);
                VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
                return res.as_bits;
            }
            auto instance = GcHeap::get().allocate<ObjInstance>();
            Value res(instance);
            GcValueGuard guard(res);
            instance->classDef = cls;
            
            ObjClosure* initMethod = nullptr;
            auto c = cls;
            while (c) {
                auto it = c->properties.find("<init>");
                if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                    initMethod = it->second.val.asFunction();
                    break;
                }
                c = c->parent;
            }
            
            if (initMethod) {
                if (initMethod->isBytecode()) {
                    VM::activeVM->callVMFunction(initMethod->compiledFnIndex, args, initMethod, res, Value(cls));
                } else if (initMethod->isNative()) {
                    helpers::nativeSelfStack.push_back(res);
                    helpers::nativeClassStack.push_back(Value(cls));
                    auto& fn = std::any_cast<NativeCallable&>(initMethod->nativeFn);
                    fn(args);
                    helpers::nativeSelfStack.pop_back();
                    helpers::nativeClassStack.pop_back();
                }
            }
            VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
            return res.as_bits;
        } else if (callee.isInstance()) {
            auto inst = callee.asInstance();
            ObjClosure* method = nullptr;
            ObjClass* owningClass = nullptr;
            auto c = inst->classDef;
            while (c) {
                auto it = c->properties.find("__call__");
                if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                    method = it->second.val.asFunction();
                    owningClass = c;
                    break;
                }
                c = c->parent;
            }
            if (method) {
                if (method->isBytecode()) {
                    Value res = VM::activeVM->callVMFunction(method->compiledFnIndex, args, method, callee, Value(owningClass));
                    VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
                    return res.as_bits;
                } else if (method->isNative()) {
                    helpers::nativeSelfStack.push_back(callee);
                    helpers::nativeClassStack.push_back(Value(owningClass));
                    Value res;
                    try {
                        auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
                        res = fn(args);
                    } catch (...) {
                        helpers::nativeSelfStack.pop_back();
                        helpers::nativeClassStack.pop_back();
                        throw;
                    }
                    helpers::nativeSelfStack.pop_back();
                    helpers::nativeClassStack.pop_back();
                    VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
                    return res.as_bits;
                }
            }
        }
        throw std::runtime_error("JIT Error: Target is not callable.");
    }
    JIT_CALLOUT_CATCH
}

Value VM::makeTokenInstance(const Token& t) {
    Value tokenClassVal = getBuiltinValue("Token");
    if (!tokenClassVal.isClass()) return Value::none();
    
    ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
    inst->classDef = static_cast<ObjClass*>(tokenClassVal.asObj());
    std::string typeStr = tokenTypeToString(t.type);
    size_t paren = typeStr.find('(');
    if (paren != std::string::npos) {
        typeStr = typeStr.substr(0, paren);
    }
    
    inst->properties["type"] = {Value(typeStr), false, false};
    inst->properties["lexeme"] = {Value(t.lexeme), false, false};
    inst->properties["line"] = {Value::fromInt32(t.line), false, false};
    inst->properties["position"] = {Value::fromInt32(t.position), false, false};
    
    return Value(inst);
}

void VM::registerBuiltin(const std::string& name, NativeCallable fn, std::set<int> arity, std::vector<std::string> paramNames, std::string restName, std::vector<std::string> kwargNames, std::string kwargsName, int kwargDefaultCount, std::vector<std::string> kwargDefaultValueTexts) {
    nativeBuiltins[name] = fn;
    builtinArity[name] = arity;
    builtinParamNames[name] = paramNames;
    builtinRestName[name] = std::move(restName);
    builtinKwargNames[name] = std::move(kwargNames);
    builtinKwargsName[name] = std::move(kwargsName);
    builtinKwargDefaultCount[name] = kwargDefaultCount;
    builtinKwargDefaultValueTexts[name] = std::move(kwargDefaultValueTexts);
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
        auto pit = builtinParamNames.find(name);
        auto rit = builtinRestName.find(name);
        auto kwit = builtinKwargNames.find(name);
        auto kwnit = builtinKwargsName.find(name);
        auto kwdcit = builtinKwargDefaultCount.find(name);
        auto kwdtit = builtinKwargDefaultValueTexts.find(name);
        if (pit != builtinParamNames.end()) {
            closure->paramNames = pit->second;
            for (size_t j = 0; j < closure->paramNames.size(); ++j) {
                closure->isRef.push_back(false);
            }
        }
        if (rit != builtinRestName.end()) closure->restName = rit->second;
        if (kwit != builtinKwargNames.end()) closure->kwargNames = kwit->second;
        if (kwnit != builtinKwargsName.end()) closure->kwargsName = kwnit->second;
        if (kwdcit != builtinKwargDefaultCount.end()) closure->setKwargDefaultsFromCount(kwdcit->second);
        if (kwdtit != builtinKwargDefaultValueTexts.end()) closure->kwargDefaultValueTexts = kwdtit->second;
        if (ait != builtinArity.end() && !ait->second.empty()) {
            int minA = *ait->second.begin();
            int maxA = *ait->second.rbegin();
            if (closure->paramNames.empty()) {
                for (int j = 0; j < maxA; ++j) {
                    closure->paramNames.push_back("_" + std::to_string(j));
                    closure->isRef.push_back(false);
                }
            }
            for (int j = minA; j < maxA; ++j) {
                closure->defaultValues.push_back(Value::uninit());
            }
        }
        Value val(closure);
        builtinClosures[name] = val;
        return val;
    }
    return Value::none();
}

Value VM::getGlobalChecked(const std::string& name) {
    auto it = globalNames.find(name);
    if (it != globalNames.end()) return globals[it->second];
    Value builtinVal = getBuiltinValue(name);
    if (builtinVal.isNone()) builtinVal = getBuiltinClosure(name);
    if (!builtinVal.isNone()) return builtinVal;
    throw std::runtime_error("VM Error: Undefined global variable '" + name + "'.");
}

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

void VM::runDefersDownTo(int targetBase, Value* currentException) {
    while (static_cast<int>(deferStack.size()) > targetBase) {
        ObjClosure* closure = deferStack.back();
        deferStack.pop_back();
        try {
            callVMFunction(closure->compiledFnIndex, {}, closure, closure->boundSelf, closure->boundClass);
        } catch (const ValueException& ex) {
            if (currentException) {
                Value deferEx = wrapException("Exception", ex.val);
                auto inst = currentException->asInstance();
                if (inst) {
                    auto it = inst->properties.find("suppressed");
                    if (it != inst->properties.end()) {
                        Value suppList = it->second.val;
                        if (suppList.isObjType(ObjType::LIST)) {
                            static_cast<ObjList*>(suppList.asObj())->vec.push_back(deferEx);
                        }
                    }
                }
            } else {
                throw;
            }
        } catch (const RuntimeError& ex) {
            if (currentException) {
                Value deferEx = wrapException(ex.type, ex.message);
                auto inst = currentException->asInstance();
                if (inst) {
                    auto it = inst->properties.find("suppressed");
                    if (it != inst->properties.end()) {
                        Value suppList = it->second.val;
                        if (suppList.isObjType(ObjType::LIST)) {
                            static_cast<ObjList*>(suppList.asObj())->vec.push_back(deferEx);
                        }
                    }
                }
            } else {
                throw;
            }
        } catch (const std::exception& ex) {
            if (currentException) {
                std::string msg = ex.what();
                std::string type = "Exception";
                size_t colonPos = msg.find(": ");
                if (colonPos != std::string::npos) {
                    std::string prefix = msg.substr(0, colonPos);
                    if (prefix.find(' ') == std::string::npos) {
                        type = prefix;
                        msg = msg.substr(colonPos + 2);
                    } else if (prefix == "VM Error" || prefix == "Runtime Error" || prefix == "Type Error" || prefix == "Math Error" || prefix == "IO Error" || prefix == "Syntax Error") {
                        type = prefix;
                        type.erase(std::remove(type.begin(), type.end(), ' '), type.end());
                        msg = msg.substr(colonPos + 2);
                    }
                }
                Value deferEx = wrapException(type, Value(msg));
                auto inst = currentException->asInstance();
                if (inst) {
                    auto it = inst->properties.find("suppressed");
                    if (it != inst->properties.end()) {
                        Value suppList = it->second.val;
                        if (suppList.isObjType(ObjType::LIST)) {
                            static_cast<ObjList*>(suppList.asObj())->vec.push_back(deferEx);
                        }
                    }
                }
            } else {
                throw;
            }
        } catch (...) {
            if (currentException) {
                Value deferEx = wrapException("Exception", Value("Unknown Error in defer"));
                auto inst = currentException->asInstance();
                if (inst) {
                    auto it = inst->properties.find("suppressed");
                    if (it != inst->properties.end()) {
                        Value suppList = it->second.val;
                        if (suppList.isObjType(ObjType::LIST)) {
                            static_cast<ObjList*>(suppList.asObj())->vec.push_back(deferEx);
                        }
                    }
                }
            } else {
                throw;
            }
        }
    }
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

std::vector<Value> VM::alignArguments(int posArgc, int kwArgc, Value* argsBase, const std::vector<std::string>& paramNames, const std::string& restName, const std::vector<std::string>& kwargNames, const std::string& kwargsName, Value boundSelf, const std::vector<bool>& kwargHasDefault) {
    // ★ 展开解包（...list 位置解包、...dict 关键字解包）
    std::vector<Value> expandedStorage;
    {
        std::vector<Value> spreadPos, spreadKwNames, spreadKwVals;
        bool anySpread = false;
        for (int i = 0; i < posArgc; ++i) {
            if (argsBase[i].isSpread()) {
                anySpread = true;
                auto* sp = static_cast<ObjSpread*>(argsBase[i].asObj());
                if (sp->isKeyword) throw std::runtime_error("TypeError: keyword spread not allowed in positional position.");
                if (!helpers::spreadPositional(sp->value, spreadPos)) {
                    throw std::runtime_error("TypeError: positional spread expects a list, set, matrix, string, or an instance with __unpack__().");
                }
            } else {
                spreadPos.push_back(argsBase[i]);
            }
        }
        for (int i = 0; i < kwArgc; ++i) {
            Value kwNameVal = argsBase[posArgc + i * 2];
            Value kwVal = argsBase[posArgc + i * 2 + 1];
            if (kwNameVal.isSpread()) {
                anySpread = true;
                auto* sp = static_cast<ObjSpread*>(kwNameVal.asObj());
                if (!sp->isKeyword) throw std::runtime_error("TypeError: positional spread not allowed in keyword position.");
                Value kwDictVal = kwVal;
                std::unique_ptr<GcValueGuard> upGuard;
                if (!kwDictVal.isObjType(ObjType::DICT)) {
                    if (!kwDictVal.isInstance()) throw std::runtime_error("TypeError: keyword spread expects a dict or an instance with __mapping__().");
                    auto [upMethod, upOwner] = findDunder(kwDictVal, "__mapping__");
                    if (!upMethod) throw std::runtime_error("TypeError: keyword spread expects a dict or an instance with __mapping__().");
                    kwDictVal = callDunder(kwDictVal, upMethod, upOwner, {});
                    if (!kwDictVal.isObjType(ObjType::DICT)) throw std::runtime_error("TypeError: __mapping__() must return a dict for keyword spread.");
                    upGuard = std::make_unique<GcValueGuard>(kwDictVal);
                }
                for (auto& [k, v] : static_cast<ObjDict*>(kwDictVal.asObj())->elements) {
                    if (!k.isString()) throw std::runtime_error("TypeError: keyword spread key must be a string.");
                    spreadKwNames.push_back(k);
                    spreadKwVals.push_back(v);
                }
            } else {
                spreadKwNames.push_back(kwNameVal);
                spreadKwVals.push_back(kwVal);
            }
        }
        if (anySpread) {
            expandedStorage.reserve(spreadPos.size() + spreadKwNames.size() * 2);
            for (auto& v : spreadPos) expandedStorage.push_back(v);
            for (size_t i = 0; i < spreadKwNames.size(); ++i) {
                expandedStorage.push_back(spreadKwNames[i]);
                expandedStorage.push_back(spreadKwVals[i]);
            }
            argsBase = expandedStorage.data();
            posArgc = static_cast<int>(spreadPos.size());
            kwArgc = static_cast<int>(spreadKwNames.size());
        }
    }
    
    std::vector<Value> alignedArgs;
    int totalExpected = static_cast<int>(paramNames.size());
    
    int posSlot = totalExpected;
    int restSlot = restName.empty() ? -1 : posSlot;
    int kwStart = posSlot + (restName.empty() ? 0 : 1);
    int kwargSlot = kwStart + static_cast<int>(kwargNames.size());
    
    alignedArgs.resize(kwargSlot + (kwargsName.empty() ? 0 : 1), Value::uninit());
    
    int dstIdx = 0;
    if (!boundSelf.isNone()) {
        if (totalExpected > 0) {
            alignedArgs[0] = boundSelf;
            dstIdx = 1;
        }
    }
    
    int fillCount = std::min(posArgc, totalExpected - dstIdx);
    for (int i = 0; i < fillCount; ++i) {
        alignedArgs[dstIdx + i] = argsBase[i];
    }
    
    // 多余位置参数（无 rest 时）
    if (restName.empty() && posArgc > fillCount) {
        if (boundSelf.isNone() && totalExpected == 0) {
            for (int i = fillCount; i < posArgc; ++i) alignedArgs.push_back(argsBase[i]);
        }
    }
    
    ObjDict* kwargs = nullptr;
    if (!kwargsName.empty()) {
        kwargs = GcHeap::get().allocate<ObjDict>();
        GcObjGuard kwGuard(kwargs);
        alignedArgs[kwargSlot] = Value(kwargs);
    }
    
    for (int i = 0; i < kwArgc; ++i) {
        Value kwNameVal = argsBase[posArgc + i * 2];
        Value kwVal = argsBase[posArgc + i * 2 + 1];
        std::string kwName = kwNameVal.asString();
        
        bool found = false;
        for (int j = 0; j < totalExpected; ++j) {
            if (paramNames[j] == kwName) {
                if (j < dstIdx || !alignedArgs[j].isUninit()) {
                    throw std::runtime_error("TypeError: Multiple values for argument '" + kwName + "'.");
                }
                alignedArgs[j] = kwVal;
                found = true;
                break;
            }
        }
        if (!found) {
            for (size_t j = 0; j < kwargNames.size(); ++j) {
                if (kwargNames[j] == kwName) {
                    int slot = kwStart + static_cast<int>(j);
                    if (!alignedArgs[slot].isUninit()) {
                        throw std::runtime_error("TypeError: Multiple values for argument '" + kwName + "'.");
                    }
                    alignedArgs[slot] = kwVal;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            if (kwargs) {
                kwargs->set(Value(kwName), kwVal);
            } else {
                throw std::runtime_error("TypeError: Unexpected keyword argument '" + kwName + "'.");
            }
        }
    }
    
    if (!restName.empty()) {
        ObjList* restList = GcHeap::get().allocate<ObjList>();
        if (!boundSelf.isNone() && totalExpected == 0) {
            restList->vec.push_back(boundSelf);
        }
        for (int i = fillCount; i < posArgc; ++i) {
            restList->vec.push_back(argsBase[i]);
        }
        alignedArgs[restSlot] = Value(restList);
    }
    
    // ★ 必填仅关键字检查（无默认值的仅关键字必须显式传）
    for (size_t i = 0; i < kwargNames.size(); ++i) {
        bool hasDefault = i < kwargHasDefault.size() && kwargHasDefault[i];
        if (!hasDefault && alignedArgs[kwStart + i].isUninit()) {
            throw std::runtime_error("Runtime Error: Missing required keyword-only argument '" + kwargNames[i] + "'.");
        }
    }
    
    return alignedArgs;
}

Value VM::callTypeConverter(ObjTypeDef* td, int posArgc, int kwArgc, Value* argsBase) {
    std::vector<Value> args;
    // ★ 统一调用约定：有 rest/仅关键字/kwargs 的类型转换器，无论有无关键字一律走 alignArguments。
    bool hasRest = !td->converterRestName.empty() || !td->converterKwargNames.empty() || !td->converterKwargsName.empty();
    if (kwArgc > 0 || hasRest) {
        if (td->converterParamNames.empty() && td->converterRestName.empty() && td->converterKwargNames.empty() && td->converterKwargsName.empty()) {
            throw std::runtime_error("TypeError: This type object does not support keyword arguments.");
        }
        std::vector<bool> kwargHasDefault(td->converterKwargNames.size(), false);
        int n = static_cast<int>(td->converterKwargNames.size());
        for (int i = 0; i < td->converterKwargDefaultCount && i < n; ++i) kwargHasDefault[n - 1 - i] = true;
        args = alignArguments(posArgc, kwArgc, argsBase, td->converterParamNames, td->converterRestName, td->converterKwargNames, td->converterKwargsName, Value::none(), kwargHasDefault);
    } else {
        args.reserve(posArgc);
        for (int i = 0; i < posArgc; ++i) args.push_back(argsBase[i]);
    }
    if (!td->converterArity.empty()) {
        int actual = 0;
        for (const auto& a : args) if (!a.isUninit()) actual++;
        if (td->converterArity.find(actual) == td->converterArity.end()) {
            std::string expected;
            for (auto aIt = td->converterArity.begin(); aIt != td->converterArity.end(); ++aIt) {
                if (aIt != td->converterArity.begin()) expected += " or ";
                expected += std::to_string(*aIt);
            }
            throw std::runtime_error("Runtime Error: Function '" + td->name() + "' expects " + expected + " arguments, got " + std::to_string(actual) + ".");
        }
    }
    return td->converter(args);
}

void VM::execCall(int calleeReg, int argc, int kwArgc, int dstReg, bool isTailCall) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    Value callee = registers[currentFrame->registerBase + calleeReg];
    
    if (callee.isString()) {
        const std::string& tag = callee.asString();
        auto it = globalNames.find(tag);
        if (it != globalNames.end()) {
            callee = globals[it->second];
        } else {
            auto nIt = nativeBuiltins.find(tag);
            if (nIt != nativeBuiltins.end()) {
                if (kwArgc > 0) {
                    callee = getBuiltinClosure(tag);
                } else {
                    auto ait = builtinArity.find(tag);
                    if (ait != builtinArity.end() && !ait->second.empty()) {
                        if (ait->second.find(argc) == ait->second.end()) {
                            std::string expected;
                            for (auto aIt = ait->second.begin(); aIt != ait->second.end(); ++aIt) {
                                if (aIt != ait->second.begin()) expected += " or ";
                                expected += std::to_string(*aIt);
                            }
                            throw std::runtime_error("Runtime Error: Function '" + tag + 
                                "' expects " + expected + " arguments, got " + std::to_string(argc) + ".");
                        }
                    }
                    std::vector<Value> args;
                    args.reserve(argc);
                    for (int i = 0; i < argc; ++i) {
                        args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
                    }
                    pendingCallRefs.clear();
                    registers[currentFrame->registerBase + dstReg] = nIt->second(args);
                    return;
                }
            } else {
                throw std::runtime_error("VM Error: Unknown function or not callable '" + tag + "'.");
            }
        }
    }

    if (callee.isType()) {
        ObjTypeDef* td = static_cast<ObjTypeDef*>(callee.asObj());
        if (td->converter) {
            pendingCallRefs.clear();
            registers[currentFrame->registerBase + dstReg] = 
                callTypeConverter(td, argc - 2 * kwArgc, kwArgc, &registers[currentFrame->registerBase + calleeReg + 1]);
            return;
        }
        if (td->types.size() == 1 && std::holds_alternative<BuiltinType>(td->types[0])) {
            BuiltinType bt = std::get<BuiltinType>(td->types[0]);
            if (bt == BuiltinType::TYPE_DEF) {
                if (kwArgc > 0) throw std::runtime_error("TypeError: type() does not accept keyword arguments.");
                if (argc != 1) throw std::runtime_error("TypeError: type() expects 1 argument.");
                Value v = registers[currentFrame->registerBase + calleeReg + 1];
                std::vector<std::variant<BuiltinType, ObjClass*>> newTypes;
                if (v.isType()) {
                    newTypes.push_back(BuiltinType::TYPE_DEF);
                } else if (v.isClass()) {
                    newTypes.push_back(BuiltinType::CLASS);
                } else if (v.isInstance()) {
                    newTypes.push_back(v.asInstance()->classDef);
                } else {
                    BuiltinType vbt = BuiltinType::ANY;
                    if (v.isInt32() || v.isBigInt()) vbt = BuiltinType::INT;
                    else if (v.isDouble()) vbt = BuiltinType::FLOAT;
                    else if (v.isString()) vbt = BuiltinType::STRING;
                    else if (v.isBool()) vbt = BuiltinType::BOOL;
                    else if (v.isNone()) vbt = BuiltinType::NONE_TYPE;
                    else if (v.isObjType(ObjType::LIST)) vbt = BuiltinType::LIST;
                    else if (v.isObjType(ObjType::DICT)) vbt = BuiltinType::DICT;
                    else if (v.isObjType(ObjType::SET)) vbt = BuiltinType::SET;
                    else if (v.isObjType(ObjType::FRACTION)) vbt = BuiltinType::FRACTION;
                    else if (v.isObjType(ObjType::COMPLEX)) vbt = BuiltinType::COMPLEX;
                    else if (v.isObjType(ObjType::SYMBOLIC)) vbt = BuiltinType::SYMBOLIC;
                    else if (v.isObjType(ObjType::REAL_MATRIX)) vbt = BuiltinType::REALMAT;
                    else if (v.isObjType(ObjType::COMPLEX_MATRIX)) vbt = BuiltinType::COMPLEXMAT;
                    else if (v.isObjType(ObjType::SYM_MATRIX)) vbt = BuiltinType::SYMMAT;
                    else if (v.isFunctionClosure()) vbt = BuiltinType::FUNC;
                    else if (v.isObjType(ObjType::NAMESPACE)) vbt = BuiltinType::NAMESPACE;
                    else if (v.isObjType(ObjType::SLICE)) vbt = BuiltinType::SLICE;
                    newTypes.push_back(vbt);
                }
                registers[currentFrame->registerBase + dstReg] = Value(internType(std::move(newTypes)));
                return;
            }
        }
        throw std::runtime_error("TypeError: This type object is not callable.");
    }

    if (callee.isFunctionClosure()) {
        auto closure = callee.asFunction();
        if (closure->isBytecode()) {
            auto& fnDef = compiledFunctions[closure->compiledFnIndex];
            
            int posArgc = argc - 2 * kwArgc;
            
            if (closure->isUFCS) {
                for (auto& pr : pendingCallRefs) pr.first += 1;
            }

            int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + calleeReg + 1;
            int newTotalCount = fnDef->localCount + fnDef->refCount;
            PendingFrameGuard pfg(this, newBase, newTotalCount);

            std::vector<Value> alignedArgs = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + calleeReg + 1], closure->paramNames, closure->restName, closure->kwargNames, closure->kwargsName, closure->isUFCS ? closure->boundSelf : Value::none(), closure->kwargHasDefault);
            for (int i = 0; i < fnDef->arity; ++i) {
                if (alignedArgs[i].isUninit()) {
                    int expected = closure->isUFCS ? fnDef->arity - 1 : fnDef->arity;
                    if (expected < 0) expected = 0;
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(expected) + " arguments.");
                }
            }
            if (fnDef->restName.empty() && static_cast<size_t>(posArgc) > static_cast<size_t>(fnDef->maxArity)) {
                int expected = closure->isUFCS ? fnDef->maxArity - 1 : fnDef->maxArity;
                if (expected < 0) expected = 0;
                throw std::runtime_error("VM Error: '" + fnDef->name + "' expects at most " + std::to_string(expected) + " arguments.");
            }
            
            int totalArgc = static_cast<int>(alignedArgs.size());

            for (int i = 0; i < totalArgc; ++i) {
                registers[newBase + i] = alignedArgs[i];
            }
            for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                registers[newBase + i] = Value::uninit();
            }

            int paramSlotCount = fnDef->maxArity + (fnDef->restName.empty() ? 0 : 1) + static_cast<int>(fnDef->kwargNames.size()) + (fnDef->kwargsName.empty() ? 0 : 1);
            for (int i = paramSlotCount; i < fnDef->localCount; ++i) {
                registers[newBase + i] = Value::none();
            }

            if (jitEntryPoints.count(closure->compiledFnIndex) && jitEntryPoints[closure->compiledFnIndex] != nullptr) {
                CallFrame newFrame;
                newFrame.function = fnDef.get();
                newFrame.chunk = &fnDef->chunk;
                newFrame.ip = 0;
                newFrame.registerBase = newBase;
                newFrame.returnRegister = dstReg;
                newFrame.deferBase = static_cast<int>(deferStack.size());
                newFrame.closure = closure;
                newFrame.selfContext = closure->boundSelf;
                newFrame.classContext = closure->boundClass;
                populateRefParams(newFrame, fnDef.get());
                
                if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                profileFrameStart(&newFrame);
                frames[frameCount++] = newFrame;

                typedef uint64_t (*JitFunc)(Value*);
                JitFunc func = reinterpret_cast<JitFunc>(jitEntryPoints[closure->compiledFnIndex]);
                
                jit::g_jc2_jit_deoptimized = false;
                uint64_t retBits = func(&registers[newBase]);
                
                if (jit::g_jc2_jit_deoptimized) {
                    jit::g_jc2_jit_deoptimized = false;
                    // 去优化：状态已由 jc2_jit_deoptimize 恢复，直接继续解释执行
                    // ★ 从 deopt 中学习：把实际 site 标记为 megamorphic，
                    //   重编译时走 callout 通用路径，避免重复去优化。
                    if (newFrame.ip >= 0 && newFrame.ip < (int)fnDef->chunk.typeFeedback.size()) {
                        fnDef->chunk.typeFeedback[newFrame.ip] |= 0x80;
                    }
                    jitEntryPoints[closure->compiledFnIndex] = nullptr;
                    jitCompiledCode.erase(closure->compiledFnIndex);
                    fnDef->callCount = 0;
                    if (jit::g_jit_pending_exception) {
                        jit::g_jit_pending_exception = 0;
                        Value exVal = jit_exception_value;
                        jit_exception_value = Value::none();
                        throw ValueException(exVal);
                    }
                    int targetDepth = frameCount - 1;
                    try {
                        registers[currentFrame->registerBase + dstReg] = run(targetDepth);
                        return;
                    } catch (...) {
                        while (frameCount > targetDepth) {
                            CallFrame* f = &frames[frameCount - 1];
                            profileFrameEnd(f);
                            int clearBase = f->registerBase;
                            int clearCount = f->function->localCount + f->function->refCount;
                            for (int i = 0; i < clearCount; ++i) {
                                registers[clearBase + i] = Value::none();
                            }
                            f->selfContext = Value::none();
                            f->classContext = Value::none();
                            f->jitReturnSlot = Value::none();
                            f->closure = nullptr;
                            f->refParamsBase = -1;
                            frameCount--;
                        }
                        throw;
                    }
                }
            
                Value retVal = Value::fromRawBits(retBits);
                registers[currentFrame->registerBase + dstReg] = retVal;

                CallFrame* f = &frames[frameCount - 1];
                profileFrameEnd(f);
                int clearBase = f->registerBase;
                int clearCount = f->function->localCount + f->function->refCount;
                for (int i = 0; i < clearCount; ++i) {
                    registers[clearBase + i] = Value::none();
                }
                f->selfContext = Value::none();
                f->classContext = Value::none();
                f->jitReturnSlot = Value::none();
                f->closure = nullptr;
                f->refParamsBase = -1;
                frameCount--;
                return;
            }

            if (isTailCall) {
                runDefersDownTo(currentFrame->deferBase);
                int oldTotalCount = currentFrame->function->localCount + currentFrame->function->refCount;
                while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                    exceptionHandlers.pop_back();
                }
                closeUpvalues(currentFrame->registerBase);
                profileFrameEnd(currentFrame);
                currentFrame->function = fnDef.get();
                currentFrame->chunk = &fnDef->chunk;
                currentFrame->ip = 0;
                currentFrame->closure = closure;
                currentFrame->selfContext = closure->boundSelf;
                currentFrame->classContext = closure->boundClass;
                
                populateRefParams(*currentFrame, fnDef.get());
                profileFrameStart(currentFrame);
                
                for (int i = newTotalCount; i < oldTotalCount; ++i) {
                    registers[newBase + i] = Value::none();
                }
                return;
            }
            
            CallFrame newFrame;
            newFrame.function = fnDef.get();
            newFrame.chunk = &fnDef->chunk;
            newFrame.ip = 0;
            newFrame.registerBase = newBase;
            newFrame.returnRegister = dstReg;
            newFrame.deferBase = static_cast<int>(deferStack.size());
            newFrame.closure = closure;
            newFrame.selfContext = closure->boundSelf;
            newFrame.classContext = closure->boundClass;
            
            populateRefParams(newFrame, fnDef.get());
            
            if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
            profileFrameStart(&newFrame);
            frames[frameCount++] = newFrame;
        } else if (closure->isNative()) {
            int posArgc = argc - 2 * kwArgc;
            std::vector<Value> args;
            
            // ★ 统一调用约定：只要 builtin 声明了 rest/仅关键字/kwargs，无论有无关键字
            //   一律走 alignArguments，rest 收集成 list、仅关键字槽位用 uninit 占位，
            //   fn 收到的 args 结构由签名元数据唯一决定。纯固定参数 builtin 才走直接 push。
            bool hasRest = !closure->restName.empty() || !closure->kwargNames.empty() || !closure->kwargsName.empty();
            if (kwArgc > 0 || hasRest) {
                if (closure->paramNames.empty() && closure->restName.empty() && closure->kwargNames.empty() && closure->kwargsName.empty()) {
                    throw std::runtime_error("TypeError: Native function '" + closure->rawBody + "' does not support keyword arguments.");
                }
                args = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + calleeReg + 1], closure->paramNames, closure->restName, closure->kwargNames, closure->kwargsName, closure->isUFCS ? closure->boundSelf : Value::none(), closure->kwargHasDefault);
                
                int expected = closure->isUFCS ? closure->minArgs() + 1 : closure->minArgs();
                for (int i = 0; i < expected; ++i) {
                    if (args[i].isUninit()) {
                        throw std::runtime_error("Runtime Error: Function '" + closure->rawBody + "' requires at least " + std::to_string(closure->minArgs()) + " arguments.");
                    }
                }
            } else {
                args.reserve(argc + (closure->isUFCS ? 1 : 0));
                if (closure->isUFCS) args.push_back(closure->boundSelf);
                for (int i = 0; i < argc; ++i) {
                    args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
                }
            }

            int totalArgc = static_cast<int>(args.size());
            auto ait = builtinArity.find(closure->rawBody);
            if (ait != builtinArity.end() && !ait->second.empty()) {
                // For builtins with specific arities, we need to count non-uninit args if kwArgc > 0
                int actualArgc = totalArgc;
                if (kwArgc > 0) {
                    actualArgc = 0;
                    for (const auto& arg : args) {
                        if (!arg.isUninit()) actualArgc++;
                    }
                }
                if (ait->second.find(actualArgc) == ait->second.end()) {
                    std::string expected;
                    for (auto aIt = ait->second.begin(); aIt != ait->second.end(); ++aIt) {
                        if (aIt != ait->second.begin()) expected += " or ";
                        expected += std::to_string(closure->isUFCS ? *aIt - 1 : *aIt);
                    }
                    throw std::runtime_error("Runtime Error: Function '" + closure->rawBody + 
                        "' expects " + expected + " arguments, got " + std::to_string(closure->isUFCS ? actualArgc - 1 : actualArgc) + ".");
                }
            } else if (static_cast<int>(closure->maxArgs()) > 0 && closure->restName.empty()) {
                int expectedMin = closure->isUFCS ? closure->minArgs() + 1 : closure->minArgs();
                int expectedMax = closure->isUFCS ? closure->maxArgs() + 1 : closure->maxArgs();
                if (totalArgc < expectedMin || totalArgc > expectedMax) {
                    throw std::runtime_error("Runtime Error: Function '" + closure->rawBody + 
                        "' expects " + std::to_string(closure->minArgs()) + " to " + 
                        std::to_string(closure->maxArgs()) + " arguments, got " + 
                        std::to_string(closure->isUFCS ? totalArgc - 1 : totalArgc) + ".");
                }
            }

            helpers::nativeSelfStack.push_back(closure->boundSelf);
            helpers::nativeClassStack.push_back(closure->boundClass);
            pendingCallRefs.clear();
            try {
                auto& fn = std::any_cast<NativeCallable&>(closure->nativeFn);
                registers[currentFrame->registerBase + dstReg] = fn(args);
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
        
        if (cls->native_allocator) {
            if (kwArgc > 0) throw std::runtime_error("TypeError: Native class allocator does not support keyword arguments.");
            std::vector<Value> args;
            args.reserve(argc);
            for (int i = 0; i < argc; ++i) {
                args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
            }
            pendingCallRefs.clear();
            registers[currentFrame->registerBase + dstReg] = cls->native_allocator(args);
            return;
        }

        auto instance = GcHeap::get().allocate<ObjInstance>();
        registers[currentFrame->registerBase + dstReg] = Value(instance); // ★ 立即 Root 防止 GC 误杀
        instance->classDef = cls;
        
        ObjClosure* initMethod = nullptr;
        auto c = cls;
        while (c) {
            auto it = c->properties.find("<init>");
            if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                initMethod = it->second.val.asFunction();
                break;
            }
            c = c->parent;
        }
        
        if (initMethod) {
            if (initMethod->isBytecode()) {
                auto& fnDef = compiledFunctions[initMethod->compiledFnIndex];
                
                int posArgc = argc - 2 * kwArgc;
                int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + calleeReg + 1;
                int newTotalCount = fnDef->localCount + fnDef->refCount;
                PendingFrameGuard pfg(this, newBase, newTotalCount);

                std::vector<Value> alignedArgs = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + calleeReg + 1], initMethod->paramNames, initMethod->restName, initMethod->kwargNames, initMethod->kwargsName, Value::none(), initMethod->kwargHasDefault);
                
                for (int i = 0; i < fnDef->arity; ++i) {
                    if (alignedArgs[i].isUninit()) {
                        throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                    }
                }
                if (fnDef->restName.empty() && static_cast<size_t>(posArgc) > static_cast<size_t>(fnDef->maxArity)) {
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' expects at most " + std::to_string(fnDef->maxArity) + " arguments.");
                }
                
                int totalArgc = static_cast<int>(alignedArgs.size());

                for (int i = 0; i < totalArgc; ++i) {
                    registers[newBase + i] = alignedArgs[i];
                }
                for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                    registers[newBase + i] = Value::uninit();
                }

                int paramSlotCount = fnDef->maxArity + (fnDef->restName.empty() ? 0 : 1) + static_cast<int>(fnDef->kwargNames.size()) + (fnDef->kwargsName.empty() ? 0 : 1);
            for (int i = paramSlotCount; i < fnDef->localCount; ++i) {
                    registers[newBase + i] = Value::none();
                }

                if (isTailCall) {
                    runDefersDownTo(currentFrame->deferBase);
                    int oldTotalCount = currentFrame->function->localCount + currentFrame->function->refCount;
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(currentFrame->registerBase);
                    profileFrameEnd(currentFrame);
                    currentFrame->function = fnDef.get();
                    currentFrame->chunk = &fnDef->chunk;
                    currentFrame->ip = 0;
                    currentFrame->closure = initMethod;
                    currentFrame->selfContext = Value(instance);
                    currentFrame->classContext = Value(cls);
                    
                    populateRefParams(*currentFrame, fnDef.get());
                    profileFrameStart(currentFrame);
                    
                    for (int i = newTotalCount; i < oldTotalCount; ++i) {
                        registers[newBase + i] = Value::none();
                    }
                    return;
                }

                CallFrame newFrame;
                newFrame.function = fnDef.get();
                newFrame.chunk = &fnDef->chunk;
                newFrame.ip = 0;
                newFrame.registerBase = newBase;
                newFrame.returnRegister = dstReg;
                newFrame.deferBase = static_cast<int>(deferStack.size());
                newFrame.closure = initMethod;
                newFrame.selfContext = Value(instance);
                newFrame.classContext = Value(cls);
                
                populateRefParams(newFrame, fnDef.get());
                
                if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                profileFrameStart(&newFrame);
                frames[frameCount++] = newFrame;
            } else if (initMethod->isNative()) {
                int posArgc = argc - 2 * kwArgc;
                std::vector<Value> args;
                
                if (kwArgc > 0) {
                    if (initMethod->paramNames.empty()) {
                        throw std::runtime_error("TypeError: Native method 'init' does not support keyword arguments.");
                    }
                    args = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + calleeReg + 1], initMethod->paramNames, initMethod->restName, initMethod->kwargNames, initMethod->kwargsName, Value::none(), initMethod->kwargHasDefault);
                    
                    for (int i = 0; i < static_cast<int>(initMethod->minArgs()); ++i) {
                        if (args[i].isUninit()) {
                            throw std::runtime_error("Runtime Error: Method 'init' requires at least " + std::to_string(initMethod->minArgs()) + " arguments.");
                        }
                    }
                } else {
                    args.reserve(argc);
                    for (int i = 0; i < argc; ++i) args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
                }

                int totalArgc = static_cast<int>(args.size());
                if (static_cast<int>(initMethod->maxArgs()) > 0 && initMethod->restName.empty()) {
                    if (totalArgc < static_cast<int>(initMethod->minArgs()) || totalArgc > static_cast<int>(initMethod->maxArgs())) {
                        throw std::runtime_error("Runtime Error: Method 'init' expects " + std::to_string(initMethod->minArgs()) + " to " + 
                            std::to_string(initMethod->maxArgs()) + " arguments, got " + 
                            std::to_string(totalArgc) + ".");
                    }
                }
                helpers::nativeSelfStack.push_back(Value(instance));
                helpers::nativeClassStack.push_back(Value(cls));
                pendingCallRefs.clear();
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
                registers[currentFrame->registerBase + dstReg] = Value(instance);
            }
        } else {
            if (cls->is_native) {
                pendingCallRefs.clear();
                throw std::runtime_error("TypeError: Cannot instantiate native class '" + cls->name + "' directly.");
            }
            if (argc > 0) {
                pendingCallRefs.clear();
                throw std::runtime_error("TypeError: Class takes no arguments directly.");
            }
            registers[currentFrame->registerBase + dstReg] = Value(instance);
            pendingCallRefs.clear();
        }
    } else if (callee.isInstance()) {
        auto inst = callee.asInstance();
        ObjClosure* method = nullptr;
        ObjClass* owningClass = nullptr;
        auto c = inst->classDef;
        while (c) {
            auto it = c->properties.find("__call__");
            if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                method = it->second.val.asFunction();
                owningClass = c;
                break;
            }
            c = c->parent;
        }

        if (method) {
            if (method->isBytecode()) {
                auto& fnDef = compiledFunctions[method->compiledFnIndex];
                
                int posArgc = argc - 2 * kwArgc;
                int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + calleeReg + 1;
                int newTotalCount = fnDef->localCount + fnDef->refCount;
                PendingFrameGuard pfg(this, newBase, newTotalCount);

                std::vector<Value> alignedArgs = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + calleeReg + 1], method->paramNames, method->restName, method->kwargNames, method->kwargsName, Value::none(), method->kwargHasDefault);
                
                for (int i = 0; i < fnDef->arity; ++i) {
                    if (alignedArgs[i].isUninit()) {
                        throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                    }
                }
                if (fnDef->restName.empty() && static_cast<size_t>(posArgc) > static_cast<size_t>(fnDef->maxArity)) {
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' expects at most " + std::to_string(fnDef->maxArity) + " arguments.");
                }
                
                int totalArgc = static_cast<int>(alignedArgs.size());

                for (int i = 0; i < totalArgc; ++i) {
                    registers[newBase + i] = alignedArgs[i];
                }
                for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                    registers[newBase + i] = Value::uninit();
                }

                int paramSlotCount = fnDef->maxArity + (fnDef->restName.empty() ? 0 : 1) + static_cast<int>(fnDef->kwargNames.size()) + (fnDef->kwargsName.empty() ? 0 : 1);
            for (int i = paramSlotCount; i < fnDef->localCount; ++i) {
                    registers[newBase + i] = Value::none();
                }

                if (isTailCall) {
                    runDefersDownTo(currentFrame->deferBase);
                    int oldTotalCount = currentFrame->function->localCount + currentFrame->function->refCount;
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(currentFrame->registerBase);
                    profileFrameEnd(currentFrame);
                    currentFrame->function = fnDef.get();
                    currentFrame->chunk = &fnDef->chunk;
                    currentFrame->ip = 0;
                    currentFrame->closure = method;
                    currentFrame->selfContext = callee;
                    currentFrame->classContext = Value(owningClass);
                    
                    populateRefParams(*currentFrame, fnDef.get());
                    profileFrameStart(currentFrame);
                    
                    for (int i = newTotalCount; i < oldTotalCount; ++i) {
                        registers[newBase + i] = Value::none();
                    }
                    return;
                }
                
                CallFrame newFrame;
                newFrame.function = fnDef.get();
                newFrame.chunk = &fnDef->chunk;
                newFrame.ip = 0;
                newFrame.registerBase = newBase;
                newFrame.returnRegister = dstReg;
                newFrame.deferBase = static_cast<int>(deferStack.size());
                newFrame.closure = method;
                newFrame.selfContext = callee;
                newFrame.classContext = Value(owningClass);
                
                populateRefParams(newFrame, fnDef.get());
                
                if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
                profileFrameStart(&newFrame);
                frames[frameCount++] = newFrame;
            } else if (method->isNative()) {
                int posArgc = argc - 2 * kwArgc;
                std::vector<Value> args;
                
                // ★ 统一调用约定（同 execCall 的 nativeFn 路径）
                bool hasRest = !method->restName.empty() || !method->kwargNames.empty() || !method->kwargsName.empty();
                if (kwArgc > 0 || hasRest) {
                    if (method->paramNames.empty() && method->restName.empty() && method->kwargNames.empty() && method->kwargsName.empty()) {
                        throw std::runtime_error("TypeError: Native method '__call__' does not support keyword arguments.");
                    }
                    args = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + calleeReg + 1], method->paramNames, method->restName, method->kwargNames, method->kwargsName, Value::none(), method->kwargHasDefault);
                    
                    for (int i = 0; i < static_cast<int>(method->minArgs()); ++i) {
                        if (args[i].isUninit()) {
                            throw std::runtime_error("Runtime Error: Method '__call__' requires at least " + std::to_string(method->minArgs()) + " arguments.");
                        }
                    }
                } else {
                    args.reserve(argc);
                    for (int i = 0; i < argc; ++i) {
                        args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
                    }
                }

                helpers::nativeSelfStack.push_back(callee);
                helpers::nativeClassStack.push_back(Value(owningClass));
                pendingCallRefs.clear();
                try {
                    auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
                    registers[currentFrame->registerBase + dstReg] = fn(args);
                } catch (...) {
                    helpers::nativeSelfStack.pop_back();
                    helpers::nativeClassStack.pop_back();
                    throw;
                }
                helpers::nativeSelfStack.pop_back();
                helpers::nativeClassStack.pop_back();
            }
        } else {
            throw std::runtime_error("VM Error: Target is not callable.");
        }
    } else {
        throw std::runtime_error("VM Error: Target is not callable.");
    }
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
static const std::string DUNDER_IDIV = "__idiv__";
static const std::string DUNDER_RIDIV = "__ridiv__";
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
static const std::string DUNDER_SUBSET = "__subsets__";
static const std::string DUNDER_UNPACK = "__unpack__";
static const std::string DUNDER_MAPPING = "__mapping__";

std::pair<ObjClosure*, ObjClass*> VM::findDunder(const Value& val, const std::string& name) {
    if (!val.isInstance()) return {nullptr, nullptr};
    auto inst = val.asInstance();
    auto c = inst->classDef;
    while (c) {
        auto it = c->properties.find(name);
        if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
            return {it->second.val.asFunction(), c};
        }
        c = c->parent;
    }
    return {nullptr, nullptr};
}

bool VM::evaluateTruthiness(const Value& val) {
    if (val.isInstance()) {
        auto [method, owner] = findDunder(val, DUNDER_BOOL);
        if (method) {
            return callDunder(val, method, owner, {}).truthy();
        }
    }
    return val.truthy();
}

Value VM::callDunder(const Value& obj, ObjClosure* method, ObjClass* ownerClass, const std::vector<Value>& args) {
    std::vector<Value> rootedArgs = args;
    std::vector<std::unique_ptr<GcValueGuard>> guards;
    for (auto& arg : rootedArgs) guards.push_back(std::make_unique<GcValueGuard>(arg));

    auto inst = obj.asInstance();
    if (method->isNative() && !method->isBytecode()) {
        helpers::nativeSelfStack.push_back(Value(inst));
        helpers::nativeClassStack.push_back(Value(ownerClass));
        pendingCallRefs.clear();
        Value result;
        try {
            auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
            // ★ 统一调用约定：有 rest/仅关键字/kwargs 的 native 方法，把展开的位置参数收集成 list。
            bool hasRest = !method->restName.empty() || !method->kwargNames.empty() || !method->kwargsName.empty();
            if (hasRest) {
                std::vector<Value> aligned = alignArguments(static_cast<int>(rootedArgs.size()), 0, rootedArgs.data(), method->paramNames, method->restName, method->kwargNames, method->kwargsName, Value::none(), method->kwargHasDefault);
                result = fn(aligned);
            } else {
                result = fn(rootedArgs);
            }
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
        
        int newBase = 0;
        if (frameCount > 0) {
            CallFrame* currentFrame = &frames[frameCount - 1];
            int locals = currentFrame->function ? currentFrame->function->localCount : 0;
            int refs = currentFrame->function ? currentFrame->function->refCount : 0;
            newBase = currentFrame->registerBase + locals + refs;
        }
        
        int newTotalCount = fnDef->localCount + fnDef->refCount;
        PendingFrameGuard pfg(this, newBase, newTotalCount);

        newFrame.registerBase = newBase;
        newFrame.returnRegister = 0;
        newFrame.deferBase = static_cast<int>(deferStack.size());
        newFrame.closure = method;
        newFrame.selfContext = Value(inst);
        newFrame.classContext = Value(ownerClass);
        
        int totalArgc = static_cast<int>(args.size());

        if (!fnDef->restName.empty()) {
            int fixedMax = fnDef->maxArity;
            if (totalArgc < fnDef->arity) {
                throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
            }
            ObjList* restList = GcHeap::get().allocate<ObjList>();
            if (totalArgc > fixedMax) {
                int restCount = totalArgc - fixedMax;
                restList->vec.reserve(restCount);
                for (int j = 0; j < restCount; j++) {
                    restList->vec.push_back(rootedArgs[fixedMax + j]);
                }
            }
            
            for (int i = 0; i < std::min(totalArgc, fixedMax); ++i) {
                registers[newBase + i] = rootedArgs[i];
            }
            for (int i = totalArgc; i < fixedMax; ++i) {
                registers[newBase + i] = Value::uninit();
            }
            registers[newBase + fixedMax] = Value(restList);
        } else {
            if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
            }
            for (int i = 0; i < totalArgc; ++i) {
                registers[newBase + i] = rootedArgs[i];
            }
            for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                registers[newBase + i] = Value::uninit();
            }
        }

        int paramSlotCount = fnDef->maxArity + (fnDef->restName.empty() ? 0 : 1) + static_cast<int>(fnDef->kwargNames.size()) + (fnDef->kwargsName.empty() ? 0 : 1);
            for (int i = paramSlotCount; i < fnDef->localCount; ++i) {
            registers[newBase + i] = Value::none();
        }
        
        populateRefParams(newFrame, fnDef.get());
        
        profileFrameStart(&newFrame);
        frames[frameCount++] = newFrame;
        
        int targetDepth = frameCount - 1;
        try {
            return run(targetDepth);
        } catch (...) {
            while (frameCount > targetDepth) {
                CallFrame* f = &frames[frameCount - 1];
                profileFrameEnd(f);
                int clearBase = f->registerBase;
                int clearCount = f->function->localCount + f->function->refCount;
                for (int i = 0; i < clearCount; ++i) {
                    registers[clearBase + i] = Value::none();
                }
                f->selfContext = Value::none();
                f->classContext = Value::none();
                f->jitReturnSlot = Value::none();
                f->closure = nullptr;
                f->refParamsBase = -1;
                frameCount--;
            }
            throw;
        }
    }
    throw std::runtime_error("VM Error: Dunder method is not callable.");
}

bool VM::checkValueType(const Value& val, ObjTypeDef* td) {
    for (const auto& t : td->types) {
        if (std::holds_alternative<BuiltinType>(t)) {
            BuiltinType bt = std::get<BuiltinType>(t);
            switch (bt) {
                case BuiltinType::ANY: return true;
                case BuiltinType::INT: if (val.isInt32() || val.isObjType(ObjType::BIGINT)) return true; break;
                case BuiltinType::FLOAT: if (val.isDouble()) return true; break;
                case BuiltinType::STRING: if (val.isString()) return true; break;
                case BuiltinType::BOOL: if (val.isBool()) return true; break;
                case BuiltinType::NONE_TYPE: if (val.isNone()) return true; break;
                case BuiltinType::LIST: if (val.isObjType(ObjType::LIST)) return true; break;
                case BuiltinType::DICT: if (val.isObjType(ObjType::DICT)) return true; break;
                case BuiltinType::SET: if (val.isObjType(ObjType::SET)) return true; break;
                case BuiltinType::FRACTION: if (val.isObjType(ObjType::FRACTION)) return true; break;
                case BuiltinType::COMPLEX: if (val.isObjType(ObjType::COMPLEX)) return true; break;
                case BuiltinType::SYMBOLIC: if (val.isObjType(ObjType::SYMBOLIC)) return true; break;
                case BuiltinType::REALMAT: if (val.isObjType(ObjType::REAL_MATRIX)) return true; break;
                case BuiltinType::COMPLEXMAT: if (val.isObjType(ObjType::COMPLEX_MATRIX)) return true; break;
                case BuiltinType::SYMMAT: if (val.isObjType(ObjType::SYM_MATRIX)) return true; break;
                case BuiltinType::FUNC: if (val.isFunctionClosure()) return true; break;
                case BuiltinType::CLASS: if (val.isClass()) return true; break;
                case BuiltinType::INSTANCE: if (val.isInstance()) return true; break;
                case BuiltinType::NAMESPACE: if (val.isObjType(ObjType::NAMESPACE)) return true; break;
                case BuiltinType::TYPE_DEF: if (val.isType()) return true; break;
                case BuiltinType::SLICE: if (val.isObjType(ObjType::SLICE)) return true; break;
                case BuiltinType::CUSTOM_CLASS: break;
            }
        } else {
            ObjClass* expectedClass = std::get<ObjClass*>(t);
            if (val.isInstance()) {
                ObjClass* c = val.asInstance()->classDef;
                while (c) {
                    if (c == expectedClass) return true;
                    c = c->parent;
                }
            }
        }
    }
    return false;
}

void VM::assertTypeMatches(const Value& val, const Value& typeObj, AssertContext ctx, const std::string& name) {
    std::string subject;
    bool isReturn = false;
    if (ctx == AssertContext::Param) subject = "Parameter '" + name + "'";
    else if (ctx == AssertContext::Return) { subject = "Function '" + name + "'"; isReturn = true; }
    else subject = name.empty() ? std::string("Expression") : "Variable '" + name + "'";

    if (typeObj.isClass()) {
        ObjClass* expectedClass = static_cast<ObjClass*>(typeObj.asObj());
        bool matched = false;
        if (val.isInstance()) {
            ObjClass* c = val.asInstance()->classDef;
            while (c) {
                if (c == expectedClass) { matched = true; break; }
                c = c->parent;
            }
        }
        if (!matched) {
            if (isReturn) throw std::runtime_error("TypeError: Function '" + name + "' expected to return '" + expectedClass->name + "', but returned '" + getTypeName(val) + "'.");
            throw std::runtime_error("TypeError: " + subject + " expected type '" + expectedClass->name + "', got '" + getTypeName(val) + "'.");
        }
        return;
    }

    if (!typeObj.isType()) throw std::runtime_error("TypeError: Expected a type object for type assertion.");

    if (!checkValueType(val, static_cast<ObjTypeDef*>(typeObj.asObj()))) {
        if (isReturn) throw std::runtime_error("TypeError: Function '" + name + "' expected to return '" + static_cast<ObjTypeDef*>(typeObj.asObj())->name() + "', but returned '" + getTypeName(val) + "'.");
        throw std::runtime_error("TypeError: " + subject + " expected type '" + static_cast<ObjTypeDef*>(typeObj.asObj())->name() + "', got '" + getTypeName(val) + "'.");
    }
}

void VM::execAssertParamType(const Value& val, int paramIdx, uint32_t nameIdx) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    if (!currentFrame->closure || !currentFrame->closure->paramTypes) return;
    
    if (paramIdx >= currentFrame->closure->paramTypesCount) return;
    Value typeObj = currentFrame->closure->paramTypes[paramIdx];
    if (typeObj.isNone()) return;
    const std::string& paramName = currentFrame->function->chunk.constants.data()[nameIdx].asString();
    assertTypeMatches(val, typeObj, AssertContext::Param, paramName);
}

void VM::execAssertReturnType(const Value& val) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    if (!currentFrame->closure || currentFrame->closure->returnType.isNone()) return;
    assertTypeMatches(val, currentFrame->closure->returnType, AssertContext::Return, currentFrame->function->name);
}

void VM::execAssertType(const Value& val, const Value& typeObj, uint32_t nameIdx) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    const std::string& name = currentFrame->function->chunk.constants.data()[nameIdx].asString();
    assertTypeMatches(val, typeObj, AssertContext::Variable, name);
}

void VM::execInvoke(int a, int b, int kwArgc, uint32_t icIdx, bool isTailCall, int fbType, bool isPrivate) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    InlineCache& ic = const_cast<InlineCache&>(currentFrame->function->chunk.inlineCaches.data()[icIdx]);
    uint32_t nameIdx = ic.nameIdx;
    Value keyVal = currentFrame->function->chunk.constants.data()[nameIdx];
    
    int argc = b;
    const Value& obj = registers[currentFrame->registerBase + a];

    ObjClosure* method = nullptr;
    ObjClass* owningClass = nullptr;
    ObjClass* nativeProto = nullptr;
    BuiltinType objBt = BuiltinType::UNKNOWN;

    const std::string& methodName = keyVal.asString();

    if (isPrivate) {
        if (obj.isInstance()) {
            auto inst = obj.asInstance();
            ObjClass* owner = currentFrame->classContext.isClass() ? static_cast<ObjClass*>(currentFrame->classContext.asObj()) : nullptr;
            if (!owner) throw std::runtime_error("VM Error: Cannot access private method outside of class context.");
            
            std::string mangledName = manglePrivate(owner->classId, methodName);
            auto it = inst->properties.find(mangledName);
            if (it != inst->properties.end()) {
                Value fv = it->second.val;
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                    owningClass = owner;
                    goto invoke_method;
                } else {
                    registers[currentFrame->registerBase + a] = fv;
                    execCall(a, b, kwArgc, a, isTailCall);
                    return;
                }
            }
            
            auto cit = owner->properties.find(mangledName);
            if (cit != owner->properties.end()) {
                Value fv = cit->second.val;
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                    owningClass = owner;
                    goto invoke_method;
                } else {
                    registers[currentFrame->registerBase + a] = fv;
                    execCall(a, b, kwArgc, a, isTailCall);
                    return;
                }
            }
            
            throw std::runtime_error("VM Error: Private method '" + methodName + "' not found.");
        } else if (obj.isClass()) {
            ObjClass* owner = currentFrame->classContext.isClass() ? static_cast<ObjClass*>(currentFrame->classContext.asObj()) : nullptr;
            if (!owner) throw std::runtime_error("VM Error: Cannot access private method outside of class context.");
            
            std::string mangledName = manglePrivate(owner->classId, methodName);
            auto it = owner->properties.find(mangledName);
            if (it != owner->properties.end()) {
                Value fv = it->second.val;
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                    owningClass = owner;
                    goto invoke_method;
                } else {
                    registers[currentFrame->registerBase + a] = fv;
                    execCall(a, b, kwArgc, a, isTailCall);
                    return;
                }
            }
            throw std::runtime_error("VM Error: Private static method '" + methodName + "' not found.");
        }
        throw std::runtime_error("VM Error: Cannot invoke private method on this type.");
    }

    if (obj.isObjType(ObjType::LIST)) objBt = BuiltinType::LIST;
    else if (obj.isObjType(ObjType::DICT)) objBt = BuiltinType::DICT;
    else if (obj.isObjType(ObjType::SET)) objBt = BuiltinType::SET;
    else if (obj.isString()) objBt = BuiltinType::STRING;
    else if (obj.isObjType(ObjType::REAL_MATRIX)) objBt = BuiltinType::REALMAT;
    else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) objBt = BuiltinType::COMPLEXMAT;
    else if (obj.isObjType(ObjType::SYM_MATRIX)) objBt = BuiltinType::SYMMAT;

    if (obj.isInstance()) {
        auto inst = obj.asInstance();
        if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
            if (inst->properties.find(methodName) == inst->properties.end()) {
                method = ic.cachedMethod;
                owningClass = ic.cachedClass;
                goto invoke_method;
            }
        }
    } else if (objBt != BuiltinType::UNKNOWN && ic.cachedBuiltinType == objBt && ic.cachedMethod) {
        method = ic.cachedMethod;
        owningClass = ic.cachedClass;
        goto invoke_method;
    }

    if (objBt == BuiltinType::LIST) nativeProto = listProto;
    else if (objBt == BuiltinType::DICT) nativeProto = dictProto;
    else if (objBt == BuiltinType::SET) nativeProto = setProto;
    else if (objBt == BuiltinType::STRING) nativeProto = stringProto;
    else if (objBt == BuiltinType::REALMAT || objBt == BuiltinType::COMPLEXMAT || objBt == BuiltinType::SYMMAT) nativeProto = matrixProto;

    if (nativeProto) {
        auto it = nativeProto->properties.find(methodName);
        if (it != nativeProto->properties.end() && !it->second.is_local) {
            Value fv = it->second.val;
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
                owningClass = nativeProto;
                ic.cachedBuiltinType = objBt;
                ic.cachedMethod = method;
                ic.cachedClass = owningClass;
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, kwArgc, a, isTailCall);
                return;
            }
        }
    }

    if (!method && obj.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(obj.asObj());
        auto it = d->keyMap.find(keyVal);
        if (it != d->keyMap.end()) {
            Value fv = d->elements[it->second].second;
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, kwArgc, a, isTailCall);
                return;
            }
        }
    } else if (!method && obj.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(obj.asObj());
        auto it = ns->fields.find(keyVal.asString());
        if (it != ns->fields.end()) {
            Value fv = *(it->second.upval->location);
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, kwArgc, a, isTailCall);
                return;
            }
        }
    } else if (obj.isClass()) {
        auto cls = static_cast<ObjClass*>(obj.asObj());
        while (cls) {
            auto it = cls->properties.find(methodName);
            if (it != cls->properties.end() && !it->second.is_local) {
                Value fv = it->second.val;
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                    owningClass = cls;
                } else {
                    registers[currentFrame->registerBase + a] = fv;
                    execCall(a, b, kwArgc, a, isTailCall);
                    return;
                }
                break;
            }
            cls = cls->parent;
        }
    } else if (obj.isInstance()) {
        auto inst = obj.asInstance();
        bool foundInField = false;

        if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
            if (inst->properties.find(methodName) == inst->properties.end()) {
                method = ic.cachedMethod;
                owningClass = ic.cachedClass;
                goto invoke_method;
            }
        }

        auto it = inst->properties.find(methodName);
        if (it != inst->properties.end() && !it->second.is_local) {
            Value fv = it->second.val;
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
                owningClass = inst->classDef;
                foundInField = true;
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, kwArgc, a, isTailCall);
                return;
            }
        }

        if (!foundInField) {
            auto c = inst->classDef;
            while (c) {
                auto cit = c->properties.find(methodName);
                if (cit != c->properties.end() && !cit->second.is_local && cit->second.val.isFunctionClosure()) {
                    method = cit->second.val.asFunction();
                    owningClass = c;
                    break;
                }
                c = c->parent;
            }
            
            if (method) {
                ic.cachedClassId = inst->classDef->classId;
                ic.cachedMethod = method;
                ic.cachedClass = owningClass;
            }

            if (!method) {
                auto [getattrMethod, owner] = findDunder(obj, "__getattr__");
                if (getattrMethod) {
                    std::vector<Value> args = { keyVal };
                    Value fv = callDunder(obj, getattrMethod, owner, args);
                    if (fv.isFunctionClosure()) {
                        method = fv.asFunction();
                        owningClass = inst->classDef;
                    } else {
                        registers[currentFrame->registerBase + a] = fv;
                        execCall(a, b, kwArgc, a, isTailCall);
                        return;
                    }
                }
            }
        }
    }

invoke_method:
    if (!method) {
        if (fbType == 1) {
            Value fallbackVal = registers[currentFrame->registerBase + a + 1 + argc];
            for (int i = argc - 1; i >= 0; --i) {
                registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
            }
            registers[currentFrame->registerBase + a + 1] = obj;
            registers[currentFrame->registerBase + a] = fallbackVal;
            for (auto& pr : pendingCallRefs) {
                pr.first += 1;
            }
            execCall(a, argc + 1, kwArgc, a, isTailCall);
            return;
        }
        
        if (ic.cachedGlobalSlot == -4) {
            std::vector<Value> argsVec;
            if (kwArgc > 0) {
                ic.cachedGlobalSlot = -1;
            } else {
                argsVec.reserve(argc + 1);
                argsVec.push_back(obj);
                for (int i = 0; i < argc; ++i) {
                    argsVec.push_back(registers[currentFrame->registerBase + a + 1 + i]);
                }
                pendingCallRefs.clear();
                auto& fn = std::any_cast<NativeCallable&>(ic.cachedNativeFn);
                registers[currentFrame->registerBase + a] = fn(argsVec);
                return;
            }
        }
        
        if (ic.cachedGlobalSlot >= 0) {
            if (globals[ic.cachedGlobalSlot].isFunctionClosure() || globals[ic.cachedGlobalSlot].isType() || globals[ic.cachedGlobalSlot].isClass()) {
                for (int i = argc - 1; i >= 0; --i) {
                    registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
                }
                registers[currentFrame->registerBase + a + 1] = obj;
                registers[currentFrame->registerBase + a] = globals[ic.cachedGlobalSlot];
                for (auto& pr : pendingCallRefs) {
                    pr.first += 1;
                }
                execCall(a, argc + 1, kwArgc, a, isTailCall);
                return;
            }
        } else {
            auto gIt = globalNames.find(methodName);
            if (gIt != globalNames.end() && (globals[gIt->second].isFunctionClosure() || globals[gIt->second].isType() || globals[gIt->second].isClass())) {
                ic.cachedGlobalSlot = gIt->second;
                for (int i = argc - 1; i >= 0; --i) {
                    registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
                }
                registers[currentFrame->registerBase + a + 1] = obj;
                registers[currentFrame->registerBase + a] = globals[gIt->second];
                for (auto& pr : pendingCallRefs) {
                    pr.first += 1;
                }
                execCall(a, argc + 1, kwArgc, a, isTailCall);
                return;
            }
        }

        auto nIt = nativeBuiltins.find(methodName);
        if (nIt != nativeBuiltins.end()) {
            if (kwArgc > 0) {
                Value closureVal = getBuiltinClosure(methodName);
                for (int i = argc - 1; i >= 0; --i) {
                    registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
                }
                registers[currentFrame->registerBase + a + 1] = obj;
                registers[currentFrame->registerBase + a] = closureVal;
                for (auto& pr : pendingCallRefs) {
                    pr.first += 1;
                }
                execCall(a, argc + 1, kwArgc, a, isTailCall);
                return;
            } else {
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

                std::vector<Value> argsVec;
                argsVec.reserve(totalArgs);
                argsVec.push_back(obj);
                for (int i = 0; i < argc; ++i) {
                    argsVec.push_back(registers[currentFrame->registerBase + a + 1 + i]);
                }
                pendingCallRefs.clear();
                
                ic.cachedGlobalSlot = -4;
                ic.cachedNativeFn = std::make_any<NativeCallable>(nIt->second);
                
                registers[currentFrame->registerBase + a] = nIt->second(argsVec);
                return;
            }
        }
        
        throw std::runtime_error("VM Error: Cannot invoke method '" + methodName + "' on this type.");
    }

    if (method->isBytecode()) {
        auto& fnDef = compiledFunctions[method->compiledFnIndex];
        
        int posArgc = argc - 2 * kwArgc;
        int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + a + 1;
        int newTotalCount = fnDef->localCount + fnDef->refCount;
        PendingFrameGuard pfg(this, newBase, newTotalCount);

        std::vector<Value> alignedArgs = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + a + 1], method->paramNames, method->restName, method->kwargNames, method->kwargsName, Value::none(), method->kwargHasDefault);
                
        for (int i = 0; i < fnDef->arity; ++i) {
            if (alignedArgs[i].isUninit()) {
                throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
            }
        }
        if (fnDef->restName.empty() && static_cast<size_t>(posArgc) > static_cast<size_t>(fnDef->maxArity)) {
            throw std::runtime_error("VM Error: '" + fnDef->name + "' expects at most " + std::to_string(fnDef->maxArity) + " arguments.");
        }
                
        int totalArgc = static_cast<int>(alignedArgs.size());

        for (int i = 0; i < totalArgc; ++i) {
            registers[newBase + i] = alignedArgs[i];
        }
        for (int i = totalArgc; i < fnDef->maxArity; ++i) {
            registers[newBase + i] = Value::uninit();
        }

        int paramSlotCount = fnDef->maxArity + (fnDef->restName.empty() ? 0 : 1) + static_cast<int>(fnDef->kwargNames.size()) + (fnDef->kwargsName.empty() ? 0 : 1);
            for (int i = paramSlotCount; i < fnDef->localCount; ++i) {
            registers[newBase + i] = Value::none();
        }

        if (isTailCall) {
            runDefersDownTo(currentFrame->deferBase);
            int oldTotalCount = currentFrame->function->localCount + currentFrame->function->refCount;
            while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                exceptionHandlers.pop_back();
            }
            closeUpvalues(currentFrame->registerBase);
            profileFrameEnd(currentFrame);
            currentFrame->function = fnDef.get();
            currentFrame->chunk = &fnDef->chunk;
            currentFrame->ip = 0;
            currentFrame->closure = method;
            currentFrame->selfContext = obj;
            currentFrame->classContext = owningClass ? Value(owningClass) : Value::none();
            
            populateRefParams(*currentFrame, fnDef.get());
            profileFrameStart(currentFrame);
            
            for (int i = newTotalCount; i < oldTotalCount; ++i) {
                registers[newBase + i] = Value::none();
            }
            return;
        }
        
        CallFrame newFrame;
        newFrame.function = fnDef.get();
        newFrame.chunk = &fnDef->chunk;
        newFrame.ip = 0;
        newFrame.registerBase = newBase;
        newFrame.returnRegister = a;
        newFrame.deferBase = static_cast<int>(deferStack.size());
        newFrame.closure = method;
        newFrame.selfContext = obj;
        newFrame.classContext = owningClass ? Value(owningClass) : Value::none();
        
        populateRefParams(newFrame, fnDef.get());
        
        if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
        profileFrameStart(&newFrame);
        frames[frameCount++] = newFrame;
    } else if (method->isNative()) {
        int posArgc = argc - 2 * kwArgc;
        std::vector<Value> args;
        
        // ★ 统一调用约定
        bool hasRest = !method->restName.empty() || !method->kwargNames.empty() || !method->kwargsName.empty();
        if (kwArgc > 0 || hasRest) {
            if (method->paramNames.empty() && method->restName.empty() && method->kwargNames.empty() && method->kwargsName.empty()) {
                throw std::runtime_error("TypeError: Native method '" + methodName + "' does not support keyword arguments.");
            }
            args = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + a + 1], method->paramNames, method->restName, method->kwargNames, method->kwargsName, Value::none(), method->kwargHasDefault);
            
            for (int i = 0; i < static_cast<int>(method->minArgs()); ++i) {
                if (args[i].isUninit()) {
                    throw std::runtime_error("Runtime Error: Method '" + methodName + "' requires at least " + std::to_string(method->minArgs()) + " arguments.");
                }
            }
        } else {
            args.reserve(argc);
            for (int i = 0; i < argc; ++i) {
                args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
            }
        }

        int totalArgc = static_cast<int>(args.size());
        if (static_cast<int>(method->maxArgs()) > 0 && method->restName.empty()) {
            if (totalArgc < static_cast<int>(method->minArgs()) || totalArgc > static_cast<int>(method->maxArgs())) {
                throw std::runtime_error("Runtime Error: Method '" + methodName + 
                    "' expects " + std::to_string(method->minArgs()) + " to " + 
                    std::to_string(method->maxArgs()) + " arguments, got " + 
                    std::to_string(totalArgc) + ".");
            }
        }

        helpers::nativeSelfStack.push_back(obj);
        helpers::nativeClassStack.push_back(owningClass ? Value(owningClass) : Value::none());
        pendingCallRefs.clear();
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

void VM::execSuperInvoke(int a, int b, int kwArgc, uint32_t nameIdx, bool isTailCall) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    const std::string& methodName = currentFrame->function->chunk.constants.data()[nameIdx].asString();
    const Value& selfVal = registers[currentFrame->registerBase + a];
    int argc = b;
    
    if (!selfVal.isInstance()) throw std::runtime_error("VM Error: 'super' requires an instance context.");
    auto inst = selfVal.asInstance();
    
    Value classVal = currentFrame->classContext;
    if (!classVal.isClass()) throw std::runtime_error("VM Error: 'super' requires class context.");
    auto currentClass = static_cast<ObjClass*>(classVal.asObj());
    auto parentClass = currentClass->parent;
    if (!parentClass) throw std::runtime_error("VM Error: No parent class.");
    
    ObjClosure* method = nullptr;
    ObjClass* owningClass = nullptr;
    auto c = parentClass;
    while (c) {
        auto it = c->properties.find(methodName);
        if (it != c->properties.end() && !it->second.is_local && it->second.val.isFunctionClosure()) {
            method = it->second.val.asFunction();
            owningClass = c;
            break;
        }
        c = c->parent;
    }
    if (!method) throw std::runtime_error("VM Error: Parent class has no method '" + methodName + "'.");
    
    if (method->isBytecode()) {
        auto& fnDef = compiledFunctions[method->compiledFnIndex];
        
        int totalArgc = argc;
        int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + a + 1;
        int newTotalCount = fnDef->localCount + fnDef->refCount;
        PendingFrameGuard pfg(this, newBase, newTotalCount);

        if (!fnDef->restName.empty()) {
            int fixedMax = fnDef->maxArity;
            if (totalArgc < fnDef->arity) {
                throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
            }
            ObjList* restList = GcHeap::get().allocate<ObjList>();
            if (totalArgc > fixedMax) {
                int restCount = totalArgc - fixedMax;
                restList->vec.reserve(restCount);
                for (int j = 0; j < restCount; j++) {
                    restList->vec.push_back(registers[currentFrame->registerBase + a + 1 + fixedMax + j]);
                }
            }
            
            for (int i = 0; i < std::min(totalArgc, fixedMax); ++i) {
                registers[newBase + i] = registers[currentFrame->registerBase + a + 1 + i];
            }
            for (int i = totalArgc; i < fixedMax; ++i) {
                registers[newBase + i] = Value::uninit();
            }
            registers[newBase + fixedMax] = Value(restList);
        } else {
            if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
            }
            for (int i = 0; i < totalArgc; ++i) {
                registers[newBase + i] = registers[currentFrame->registerBase + a + 1 + i];
            }
            for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                registers[newBase + i] = Value::uninit();
            }
        }

        int paramSlotCount = fnDef->maxArity + (fnDef->restName.empty() ? 0 : 1) + static_cast<int>(fnDef->kwargNames.size()) + (fnDef->kwargsName.empty() ? 0 : 1);
            for (int i = paramSlotCount; i < fnDef->localCount; ++i) {
            registers[newBase + i] = Value::none();
        }

        if (isTailCall) {
            runDefersDownTo(currentFrame->deferBase);
            int oldTotalCount = currentFrame->function->localCount + currentFrame->function->refCount;
            while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                exceptionHandlers.pop_back();
            }
            closeUpvalues(currentFrame->registerBase);
            profileFrameEnd(currentFrame);
            currentFrame->function = fnDef.get();
            currentFrame->chunk = &fnDef->chunk;
            currentFrame->ip = 0;
            currentFrame->closure = method;
            currentFrame->selfContext = Value(inst);
            currentFrame->classContext = Value(owningClass);
            
            populateRefParams(*currentFrame, fnDef.get());
            profileFrameStart(currentFrame);
            
            for (int i = newTotalCount; i < oldTotalCount; ++i) {
                registers[newBase + i] = Value::none();
            }
            return;
        }
        
        CallFrame newFrame;
        newFrame.function = fnDef.get();
        newFrame.chunk = &fnDef->chunk;
        newFrame.ip = 0;
        newFrame.registerBase = newBase;
        newFrame.returnRegister = a;
        newFrame.deferBase = static_cast<int>(deferStack.size());
        newFrame.closure = method;
        newFrame.selfContext = Value(inst);
        newFrame.classContext = Value(owningClass);
        
        populateRefParams(newFrame, fnDef.get());
        
        if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
        profileFrameStart(&newFrame);
        frames[frameCount++] = newFrame;
    } else if (method->isNative()) {
        int posArgc = argc - 2 * kwArgc;
        std::vector<Value> args;
        
        // ★ 统一调用约定
        bool hasRest = !method->restName.empty() || !method->kwargNames.empty() || !method->kwargsName.empty();
        if (kwArgc > 0 || hasRest) {
            if (method->paramNames.empty() && method->restName.empty() && method->kwargNames.empty() && method->kwargsName.empty()) {
                throw std::runtime_error("TypeError: Native super method '" + methodName + "' does not support keyword arguments.");
            }
            args = alignArguments(posArgc, kwArgc, &registers[currentFrame->registerBase + a + 1], method->paramNames, method->restName, method->kwargNames, method->kwargsName, Value::none(), method->kwargHasDefault);
            
            for (int i = 0; i < static_cast<int>(method->minArgs()); ++i) {
                if (args[i].isUninit()) {
                    throw std::runtime_error("Runtime Error: Super method '" + methodName + "' requires at least " + std::to_string(method->minArgs()) + " arguments.");
                }
            }
        } else {
            args.reserve(argc);
            for (int i = 0; i < argc; ++i) {
                args.push_back(registers[currentFrame->registerBase + a + 1 + i]);
            }
        }

        int totalArgc = static_cast<int>(args.size());
        if (static_cast<int>(method->maxArgs()) > 0 && method->restName.empty()) {
            if (totalArgc < static_cast<int>(method->minArgs()) || totalArgc > static_cast<int>(method->maxArgs())) {
                throw std::runtime_error("Runtime Error: Super method '" + methodName + 
                    "' expects " + std::to_string(method->minArgs()) + " to " + 
                    std::to_string(method->maxArgs()) + " arguments, got " + 
                    std::to_string(totalArgc) + ".");
            }
        }

        helpers::nativeSelfStack.push_back(Value(inst));
        helpers::nativeClassStack.push_back(Value(owningClass));
        pendingCallRefs.clear();
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

Value VM::execImport(const std::string& name) {
    std::string baseName = from_path(to_path(name).stem());

    if (loadedModules.count(name)) {
        return loadedModules[name];
    }

    ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
    loadedModules[name] = Value(ns);
    ns->name = name;

    std::string resolved = "";
    
#if defined(_WIN32)
    std::string nativeExt = ".dll";
#else
    std::string nativeExt = ".so";
#endif
    std::string nativeName = name;
    if (nativeName.length() < nativeExt.length() || nativeName.substr(nativeName.length() - nativeExt.length()) != nativeExt) {
        nativeName += nativeExt;
    }

    // 1. 优先查找 <exe_dir>/lib/ 下的原生库
#if defined(_WIN32)
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        std::string modPath = from_path(std::filesystem::path(exePath).parent_path() / "lib" / to_path(nativeName));
        if (std::filesystem::is_regular_file(to_path(modPath))) resolved = modPath;
    }
#else
    char exePath[4096];
    ssize_t count = readlink("/proc/self/exe", exePath, 4096);
    if (count != -1) {
        std::string modPath = from_path(std::filesystem::path(std::string(exePath, count)).parent_path() / "lib" / to_path(nativeName));
        if (std::filesystem::is_regular_file(to_path(modPath))) resolved = modPath;
    }
#endif

    // 2. 其次查找当前目录下的原生库
    if (resolved.empty()) {
        std::string localModPath = helpers::safeResolvePath(nativeName);
        if (std::filesystem::is_regular_file(to_path(localModPath))) resolved = localModPath;
    }

    // 3. 查找 .jcb 字节码
    std::string jcbPath = "";
    if (resolved.empty()) {
        std::string p = helpers::safeResolvePath(name);
        if (from_path(to_path(p).extension()) == ".jcb" && std::filesystem::is_regular_file(to_path(p))) {
            jcbPath = p;
        } else {
            p = helpers::safeResolvePath(name + ".jcb");
            if (std::filesystem::is_regular_file(to_path(p))) jcbPath = p;
        }
    }

    // 4. 查找 .jc2 模块脚本
    std::string jc2Path = "";
    if (resolved.empty() && jcbPath.empty()) {
        std::string p = helpers::safeResolvePath(name);
        if (from_path(to_path(p).extension()) == ".jc2" && std::filesystem::is_regular_file(to_path(p))) {
            jc2Path = p;
        } else {
            p = helpers::safeResolvePath(name + ".jc2");
            if (std::filesystem::is_regular_file(to_path(p))) jc2Path = p;
        }
    }

    if (resolved.empty() && jcbPath.empty() && jc2Path.empty()) {
        loadedModules.erase(name);
        throw std::runtime_error("VM Error: Cannot find library or module '" + name + "'.");
    }

    importedModules.insert(name);

    std::shared_ptr<CompiledFunction> modFn;
    std::string executePath;

    if (!resolved.empty()) {
        std::string ext = from_path(to_path(resolved).extension());
#if defined(_WIN32)
        HMODULE handle = LoadLibraryW(to_path(resolved).wstring().c_str());
        if (!handle) { loadedModules.erase(name); throw std::runtime_error("VM Error: Failed to load dynamic library '" + resolved + "'."); }
        auto init_fn = (JC2_ExtensionInitFunc)GetProcAddress(handle, "jc2_extension_init");
#else
        void* handle = dlopen(resolved.c_str(), RTLD_NOW);
        if (!handle) { loadedModules.erase(name); throw std::runtime_error("VM Error: Failed to load dynamic library '" + resolved + "': " + dlerror()); }
        auto init_fn = (JC2_ExtensionInitFunc)dlsym(handle, "jc2_extension_init");
#endif
        if (!init_fn) {
            loadedModules.erase(name);
            throw std::runtime_error("VM Error: Dynamic library '" + resolved + "' does not export 'jc2_extension_init'.");
        }

        std::unordered_map<std::string, Value> tempGlobals;
        std::unordered_map<std::string, NativeCallable> tempNatives;
        std::unordered_map<std::string, std::set<int>> tempArity;
        std::unordered_map<std::string, std::vector<std::string>> tempParamNames;
        std::unordered_map<std::string, std::string> tempRestName;
        std::unordered_map<std::string, std::vector<std::string>> tempKwargNames;
        std::unordered_map<std::string, std::string> tempKwargsName;
        std::unordered_map<std::string, int> tempKwargDefaultCount;

        ModuleLoadContext mctx = { &tempGlobals, &tempNatives, &tempArity, &tempParamNames, &tempRestName, &tempKwargNames, &tempKwargsName, &tempKwargDefaultCount };

        size_t old_size = jc::nativeTempRefs.size();
        int res = init_fn(reinterpret_cast<JC2_VMContext>(this), &mctx, get_host_api());
        jc::nativeTempRefs.resize(old_size);
        if (res != 0) {
            loadedModules.erase(name);
            throw std::runtime_error("VM Error: Extension initialization failed with code " + std::to_string(res));
        }

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
            GcObjGuard closureGuard(closure);
            closure->nativeFn = std::make_any<NativeCallable>(kv.second);
            
            auto ait = tempArity.find(kv.first);
            auto pit = tempParamNames.find(kv.first);
            auto rit = tempRestName.find(kv.first);
            auto kwit = tempKwargNames.find(kv.first);
            auto kwnit = tempKwargsName.find(kv.first);
            auto kwdcit = tempKwargDefaultCount.find(kv.first);
            
            if (pit != tempParamNames.end()) {
                closure->paramNames = pit->second;
                for (size_t j = 0; j < closure->paramNames.size(); ++j) {
                    closure->isRef.push_back(false);
                }
            }
            if (rit != tempRestName.end()) closure->restName = rit->second;
            if (kwit != tempKwargNames.end()) closure->kwargNames = kwit->second;
            if (kwnit != tempKwargsName.end()) closure->kwargsName = kwnit->second;
            if (kwdcit != tempKwargDefaultCount.end()) closure->setKwargDefaultsFromCount(kwdcit->second);
            if (ait != tempArity.end() && !ait->second.empty()) {
                int minA = *ait->second.begin();
                int maxA = *ait->second.rbegin();
                if (closure->paramNames.empty()) {
                    for (int j = 0; j < maxA; ++j) {
                        closure->paramNames.push_back("_" + std::to_string(j));
                        closure->isRef.push_back(false);
                    }
                }
                for (int j = minA; j < maxA; ++j) {
                    closure->defaultValues.push_back(Value::uninit());
                }
            }

            auto uv = GcHeap::get().allocate<ObjUpVal>();
            uv->closed = Value(closure);
            uv->location = &uv->closed;
            ns->fields[kv.first] = { uv, true };
        }
        return Value(ns);
    }

    if (!jcbPath.empty()) {
        try {
            modFn = BytecodeSerializer::loadJCB(jcbPath, this);
            executePath = jcbPath;
        } catch (const std::runtime_error& e) {
            std::string msg = e.what();
            if (msg == "JCB_MAGIC_MISMATCH" || msg == "JCB_VERSION_MISMATCH") {
                if (jc2Path.empty()) {
                    jc2Path = helpers::safeResolvePath(name + ".jc2");
                    if (!std::filesystem::is_regular_file(jc2Path)) {
                        loadedModules.erase(name);
                        throw std::runtime_error("VM Error: Bytecode version mismatch and source file not found for '" + name + "'.");
                    }
                }
            } else {
                loadedModules.erase(name);
                throw;
            }
        }
    }

    if (!modFn && !jc2Path.empty()) {
        std::ifstream file(to_path(jc2Path));
        if (!file.is_open()) { loadedModules.erase(name); throw std::runtime_error("IO Error: Cannot read module script."); }
        std::string code, line;
        while (std::getline(file, line)) code += line + "\n";
        file.close();

        jc::Lexer lexer(code, jc2Path);
        auto tokens = lexer.tokenize();
        jc::Parser parser(tokens);
        auto ast = parser.parse();

        auto nsDecl = std::make_unique<NamespaceDecl>(Token(TokenType::IDENTIFIER, baseName, 0), std::move(ast));

        modFn = std::make_shared<CompiledFunction>();
        modFn->name = "<module " + baseName + ">";
        modFn->sourceFile = jc2Path;
        modFn->arity = 0;
        modFn->maxArity = 0;
        modFn->restName = "";

        Resolver resolver;
        resolver.resolve(nsDecl.get());

        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, &compiledFunctions, nullptr, modFn.get(), &resolver.exprSymbols, &resolver.patternSymbols);
        fnBuilder.build(nsDecl.get());

        if (g_showIR) fnGraph.print("Module '" + baseName + "' Unoptimized");

        IROptimizer::optimize(&fnGraph);
        if (g_showIR) fnGraph.print("Module '" + baseName + "' Optimized");

        RegisterAllocator::allocate(&fnGraph);
        if (g_showIR) fnGraph.print("Module '" + baseName + "' Allocated");

        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                IRNode* localNode = target.localNode;
                int upvalIdx = target.index;
                CompiledFunction* childFn = modFn.get();
                fnGraph.postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                    childFn->upvalues[upvalIdx].index = localNode->physicalReg;
                });
            }
        }

        modFn->localCount = Emitter::emit(&fnGraph, modFn->chunk);
        compiledFunctions.push_back(modFn);
        executePath = jc2Path;
    }

    CallFrame newFrame;
    newFrame.function = modFn.get();
    newFrame.chunk = &modFn->chunk;
    newFrame.ip = 0;
    
    CallFrame* currentFrame = &frames[frameCount - 1];
    int newBase = currentFrame->registerBase + currentFrame->function->localCount + currentFrame->function->refCount;
    int newTotalCount = modFn->localCount + modFn->refCount;
    PendingFrameGuard pfg(this, newBase, newTotalCount);

    newFrame.registerBase = newBase;
    newFrame.returnRegister = 0;
    newFrame.deferBase = static_cast<int>(deferStack.size());
    newFrame.closure = nullptr;
    newFrame.selfContext = Value::none();
    newFrame.classContext = Value::none();
    
    for (int i = 0; i < modFn->localCount; ++i) {
        registers[newFrame.registerBase + i] = Value::none();
    }
    
    populateRefParams(newFrame, modFn.get());
    
    if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
    int targetDepth = frameCount;
    profileFrameStart(&newFrame);
    frames[frameCount++] = newFrame;

    std::string scriptDir = from_path(to_path(executePath).parent_path());
    helpers::g_scriptDirStack.push_back(scriptDir);
    Value nsVal;
    try {
        nsVal = run(targetDepth);
    } catch (...) {
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        helpers::g_scriptDirStack.pop_back();
        loadedModules.erase(name);
        throw;
    }
    helpers::g_scriptDirStack.pop_back();

    if (!nsVal.isObjType(ObjType::NAMESPACE)) {
        loadedModules.erase(name);
        throw std::runtime_error("VM Error: Module script must not use top-level 'return'.");
    }
    ns = static_cast<ObjNamespace*>(nsVal.asObj());
    loadedModules[name] = Value(ns);

    return Value(ns);
}

void VM::execCompileTimeImport(const std::string& name) {
    std::string baseName = from_path(to_path(name).stem());

    if (loadedModules.count(name)) {
        return;
    }

    std::string resolved = "";
    
    resolved = helpers::safeResolvePath(name);
    if (!std::filesystem::is_regular_file(to_path(resolved))) {
        resolved = helpers::safeResolvePath(name + ".jc2");
    }

    if (resolved.empty() || !std::filesystem::is_regular_file(to_path(resolved))) {
        throw std::runtime_error("VM Error: Cannot find compile-time module '" + name + "'.");
    }

    std::ifstream file(to_path(resolved));
    if (!file.is_open()) throw std::runtime_error("IO Error: Cannot read compile-time module script.");
    std::string code, line;
    while (std::getline(file, line)) code += line + "\n";
    file.close();

    jc::Lexer lexer(code, resolved);
    auto tokens = lexer.tokenize();
    jc::Parser parser(tokens, resolved);
    auto ast = parser.parse();

    auto nsDecl = std::make_unique<NamespaceDecl>(Token(TokenType::IDENTIFIER, baseName, 0), std::move(ast));

    auto modFn = std::make_shared<CompiledFunction>();
    modFn->name = "<comptime " + name + ">";
    modFn->sourceFile = resolved;
    modFn->arity = 0;
    modFn->maxArity = 0;
    modFn->restName = "";

    Resolver resolver;
    resolver.resolve(nsDecl.get());

    IRGraph fnGraph;
    IRBuilder fnBuilder(&fnGraph, &compiledFunctions, nullptr, modFn.get(), &resolver.exprSymbols, &resolver.patternSymbols);
    fnBuilder.build(nsDecl.get());

    IROptimizer::optimize(&fnGraph);
    RegisterAllocator::allocate(&fnGraph);

    for (auto& target : fnBuilder.upvalueTargets) {
        if (target.isLocal && target.localNode) {
            IRNode* localNode = target.localNode;
            int upvalIdx = target.index;
            CompiledFunction* childFn = modFn.get();
            fnGraph.postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                childFn->upvalues[upvalIdx].index = localNode->physicalReg;
            });
        }
    }

    modFn->localCount = Emitter::emit(&fnGraph, modFn->chunk);
    compiledFunctions.push_back(modFn);

    CallFrame newFrame;
    newFrame.function = modFn.get();
    newFrame.chunk = &modFn->chunk;
    newFrame.ip = 0;
    
    int newBase = 0;
    if (frameCount > 0) {
        CallFrame* prev = &frames[frameCount - 1];
        newBase = prev->registerBase + prev->function->localCount + prev->function->refCount;
    }
    int newTotalCount = modFn->localCount + modFn->refCount;
    PendingFrameGuard pfg(this, newBase, newTotalCount);

    newFrame.registerBase = newBase;
    newFrame.returnRegister = 0;
    newFrame.deferBase = static_cast<int>(deferStack.size());
    newFrame.closure = nullptr;
    newFrame.selfContext = Value::none();
    newFrame.classContext = Value::none();
    
    for (int i = 0; i < modFn->localCount; ++i) {
        registers[newFrame.registerBase + i] = Value::none();
    }
    
    populateRefParams(newFrame, modFn.get());
    
    if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
    
    int targetDepth = frameCount;
    profileFrameStart(&newFrame);
    frames[frameCount++] = newFrame;

    std::string scriptDir = from_path(to_path(resolved).parent_path());
    helpers::g_scriptDirStack.push_back(scriptDir);
    Value nsVal;
    try {
        nsVal = run(targetDepth);
        frames[frameCount].selfContext = Value::none();
        frames[frameCount].classContext = Value::none();
        frames[frameCount].jitReturnSlot = Value::none();
        frames[frameCount].closure = nullptr;
        frames[frameCount].refParamsBase = -1;
    } catch (ValueException& ex) {
        while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= targetDepth) {
            exceptionHandlers.pop_back();
        }
        Value errVal = wrapException("Exception", ex.val);
        try { runDefersDownTo(frames[targetDepth].deferBase, &errVal); } catch (...) {}
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        pendingCallRefs.clear();
        helpers::g_scriptDirStack.pop_back();
        throw RuntimeError("", errVal);
    } catch (RuntimeError& ex) {
        while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= targetDepth) {
            exceptionHandlers.pop_back();
        }
        Value errVal = wrapException(ex.type, ex.message);
        try { runDefersDownTo(frames[targetDepth].deferBase, &errVal); } catch (...) {}
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        pendingCallRefs.clear();
        helpers::g_scriptDirStack.pop_back();
        throw;
    } catch (...) {
        while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= targetDepth) {
            exceptionHandlers.pop_back();
        }
        try { runDefersDownTo(frames[targetDepth].deferBase); } catch (...) {}
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        pendingCallRefs.clear();
        helpers::g_scriptDirStack.pop_back();
        throw;
    }
    helpers::g_scriptDirStack.pop_back();

    if (!nsVal.isObjType(ObjType::NAMESPACE)) {
        throw std::runtime_error("VM Error: Compile-time module script must not use top-level 'return'.");
    }
    ObjNamespace* ns = static_cast<ObjNamespace*>(nsVal.asObj());
    
    for (const auto& [k, field] : ns->fields) {
        auto it = globalNames.find(k);
        if (it == globalNames.end()) {
            globalNames[k] = static_cast<uint32_t>(globals.size());
            globals.push_back(*(field.upval->location));
            globalsDataPtr = globals.data();
            if (field.isConst) constGlobals.insert(k);
            comptimeGlobals.push_back(k);
        } else {
            globals[it->second] = *(field.upval->location);
        }
    }
    
    loadedModules[name] = Value(ns);
    importedModules.insert(name);
}

bool VM::handleExceptionUnwind(Value* errValPtr) {
    if (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= currentTargetFrameDepth) {
        auto handler = exceptionHandlers.back();
        exceptionHandlers.pop_back();
        
        runDefersDownTo(handler.deferBase, errValPtr);
        
        while (frameCount > handler.frameIndex + 1) {
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        
        pendingCallRefs.clear();
        
        CallFrame* frame = &frames[frameCount - 1];
        frame->ip = handler.ip;
        frame->registerBase = handler.registerBase;
        
        closeUpvalues(frame->registerBase + frame->function->localCount);

        registers[frame->registerBase + handler.errReg] = *errValPtr;
        return true;
    }
    return false;
}

Value VM::wrapException(const std::string& type, Value val) {
    if (val.isInstance() && val.asInstance()->classDef->name == "Exception") {
        auto inst = val.asInstance();
        auto it = inst->properties.find("traceback");
        if (it != inst->properties.end()) {
            Value tbVal = it->second.val;
            if (tbVal.isString() && tbVal.asString().empty()) {
                it->second.val = Value(buildStackTrace());
            }
        }
        return val;
    }
    
    Value classVal = getBuiltinValue("Exception");
    if (!classVal.isClass()) return val;
    
    ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
    inst->classDef = static_cast<ObjClass*>(classVal.asObj());
    
    if (val.isString()) {
        std::string msgStr = val.asString();
        if (msgStr.find("[Line ") == 0) {
            size_t c = msgStr.find("] ");
            if (c != std::string::npos) {
                msgStr = msgStr.substr(c + 2);
                val = Value(msgStr);
            }
        }
    }
    
    inst->properties["type"] = {Value(type), false, false};
    inst->properties["message"] = {val, false, false};
    inst->properties["traceback"] = {Value(buildStackTrace()), false, false};
    inst->properties["suppressed"] = {Value(GcHeap::get().allocate<ObjList>()), false, false};
    
    return Value(inst);
}

std::string VM::formatException(const Value& errVal) {
    if (errVal.isInstance() && errVal.asInstance()->classDef->name == "Exception") {
        auto [dunderStr, owner] = findDunder(errVal, "__str__");
        if (dunderStr) {
            try {
                return callDunder(errVal, dunderStr, owner, {}).asString();
            } catch (...) {}
        }
    }
    return errVal.isString() ? errVal.asString() : errVal.toRepr();
}

std::string VM::buildStackTrace() const {
    std::ostringstream oss;
    oss << jc::col(jc::Ansi::GRAY) << "\nTraceback (most recent call last):\n";
    for (int i = frameCount - 1; i >= 0; --i) {
        const CallFrame& f = frames[i];
        int line = (f.ip > 0 && f.ip <= static_cast<int>(f.chunk->lines.size())) ? f.chunk->lines[f.ip - 1] : 0;
        std::string fnName = f.function ? f.function->name : "<unknown>";
        if (fnName == "<script>" || fnName == "<eval>") {
            std::string srcFile = f.function ? f.function->sourceFile : "";
            if (srcFile.empty()) fnName = "REPL";
            else {
                try { fnName = from_path(to_path(srcFile).filename()); }
                catch (...) { fnName = srcFile; }
            }
            oss << "  at [Line " << line << "] in " << fnName << "\n";
        } else {
            oss << "  at [Line " << line << "] in " << fnName << "()\n";
        }
    }
    oss << jc::col(jc::Ansi::RESET);
    return oss.str();
}

void VM::invalidateJIT() {
    // 只清入口指针，不清 ExecutableMemory（否则正在执行的 JIT 代码的内存会被释放）。
    // 旧内存会在重新编译时被新 shared_ptr 覆盖、延迟释放。
    osrEntryPoints.clear();
    jitEntryPoints.clear();
}

static bool isContainerValue(const Value& v) {
    if (!v.isObj()) return false;
    ObjType t = v.asObj()->type;
    return t == ObjType::LIST || t == ObjType::DICT || t == ObjType::SET ||
           t == ObjType::CLOSURE || t == ObjType::CLASS || t == ObjType::INSTANCE ||
           t == ObjType::SUPER_PROXY || t == ObjType::NAMESPACE ||
           t == ObjType::UPVALUE || t == ObjType::TYPE_DEF;
}

void VM::invalidateJITOnContainerReplace(const Value& oldVal, const Value& newVal) {
    if (isContainerValue(oldVal) || isContainerValue(newVal)) {
        invalidateJIT();
    }
}

void VM::compileForOSR(int fnIdx, int loopHeaderIp) {
    auto& fnDef = compiledFunctions[fnIdx];
    
    bool supported = true;
    for (Instruction inst : fnDef->chunk.code) {
        OpCode op = GET_OPCODE(inst);
        if (op == OpCode::TRY_BEGIN || op == OpCode::DEFER) {
            supported = false;
            break;
        }
    }
    if (!supported) {
        osrEntryPoints[fnIdx][loopHeaderIp] = nullptr;
        return;
    }
    
    try {
        jit::HIRGraph hirGraph;
        jit::HIRBuilder hirBuilder(&hirGraph, fnDef->localCount + fnDef->refCount, jit::DeoptRegistry::get().allocateBailoutIdBase());
        jit::BytecodeToHIR converter(fnDef->chunk, hirBuilder, fnDef->localCount + fnDef->refCount);
        converter.setOSRMode(loopHeaderIp);
        converter.build();
        if (g_showHIR) {
            std::cout << "--- OSR HIR Graph (Unoptimized) ---\n";
            hirGraph.printDOT(std::cout);
        }

        // --- Mid-level Optimizations (Phase 11 & 12) ---
        jit::DeadPhiElimination(hirGraph, hirBuilder).run();
        jit::ConstantFolding(hirGraph, hirBuilder).run();
        jit::AlgebraicSimplification(hirGraph, hirBuilder).run();
        jit::CommonSubexpressionElimination(hirGraph, hirBuilder).run();
        jit::DeadCodeElimination(hirGraph, hirBuilder).run();
        
        if (g_showHIR) {
            std::cout << "--- OSR HIR Graph (Optimized) ---\n";
            hirGraph.printDOT(std::cout);
        }

        jit::LIRGraph lirGraph;
        jit::LIRBuilder lirBuilder(&lirGraph);
        jit::GCM gcm(hirGraph, lirGraph);
        gcm.schedule();

        jit::InstructionSelector selector(gcm, hirGraph, lirGraph, lirBuilder);
        selector.select();

        jit::LivenessAnalyzer liveness(lirGraph);
        liveness.analyze();

        jit::LinearScanAllocator allocator(lirGraph, liveness);
        allocator.allocate();

        jit::MacroAssembler masm;
        jit::CodeEmitter emitter(lirGraph, masm, reinterpret_cast<void*>(jit::jc2_jit_deoptimize), reinterpret_cast<void**>(&globalsDataPtr), reinterpret_cast<void*>(jc2_jit_call_helper));
        
        emitter.emit(allocator.getStackSize(), true); // ★ Step 81: 触发 OSR Prologue
        masm.emitConstantPool();

        auto mem = std::make_shared<jit::ExecutableMemory>();
        masm.finalize(*mem);

        if (g_showMachineCode) {
            std::cout << "--- OSR Machine Code [fn=" << fnDef->name << " osrIp=" << loopHeaderIp << "] (Size: " << mem->size() << " bytes) ---\n";
            const uint8_t* code = mem->get();
            jit::disassemble(code, mem->size(), std::cout);
            std::cout << "\n";
        }

        osrCompiledCode[fnIdx][loopHeaderIp] = mem;
        osrEntryPoints[fnIdx][loopHeaderIp] = mem->get();
        if (g_profile) {
            std::cout << "[JIT] OSR Compilation successful for loop header IP: " << loopHeaderIp << "\n";
        }
    } catch (const std::exception& e) {
        if (g_profile) std::cout << "[JIT] OSR Compilation failed: " << e.what() << "\n";
        // OSR 编译失败，回退到解释器，并标记不再尝试编译
        osrEntryPoints[fnIdx][loopHeaderIp] = nullptr; 
    } catch (...) {
        if (g_profile) std::cout << "[JIT] OSR Compilation failed with unknown error.\n";
        osrEntryPoints[fnIdx][loopHeaderIp] = nullptr; 
    }
}

void VM::profileFrameStart(CallFrame* frame) {
    if (frame->function) {
        CompiledFunction* fn = const_cast<CompiledFunction*>(frame->function);
        fn->callCount++;
        if (fn->callCount == 50) {
            // 延迟收集 (Warm-up Profiling): 清除前 50 次的早期类型污染
            std::memset(fn->chunk.typeFeedback.data(), 0, fn->chunk.typeFeedback.size());
        } else if (fn->callCount == 1000 && frame->closure) {
            int fnIdx = frame->closure->compiledFnIndex;
            // 触发 JIT 编译 (Tier 2)
            if (g_enableJit && jitEntryPoints.find(fnIdx) == jitEntryPoints.end()) {
                bool supported = true;
                for (Instruction inst : fn->chunk.code) {
                    OpCode op = GET_OPCODE(inst);
                    if (op == OpCode::TRY_BEGIN || op == OpCode::DEFER) {
                        supported = false;
                        break;
                    }
                }
                if (!supported) {
                    jitEntryPoints[fnIdx] = nullptr;
                    return;
                }
                try {
                    jit::HIRGraph hirGraph;
                    jit::HIRBuilder hirBuilder(&hirGraph, fn->localCount + fn->refCount, jit::DeoptRegistry::get().allocateBailoutIdBase());
                    jit::BytecodeToHIR converter(fn->chunk, hirBuilder, fn->localCount + fn->refCount);
                    converter.build();

                    if (g_showHIR) {
                        std::cout << "--- Tier 2 HIR Graph (Unoptimized) ---\n";
                        hirGraph.printDOT(std::cout);
                    }

                    // --- Mid-level Optimizations (Phase 11 & 12) ---
                    jit::DeadPhiElimination(hirGraph, hirBuilder).run();
                    jit::ConstantFolding(hirGraph, hirBuilder).run();
                    jit::AlgebraicSimplification(hirGraph, hirBuilder).run();
                    jit::CommonSubexpressionElimination(hirGraph, hirBuilder).run();
                    jit::DeadCodeElimination(hirGraph, hirBuilder).run();

                    if (g_showHIR) {
                        std::cout << "--- Tier 2 HIR Graph (Optimized) ---\n";
                        hirGraph.printDOT(std::cout);
                    }

                    jit::LIRGraph lirGraph;
                    jit::LIRBuilder lirBuilder(&lirGraph);
                    jit::GCM gcm(hirGraph, lirGraph);
                    gcm.schedule();

                    jit::InstructionSelector selector(gcm, hirGraph, lirGraph, lirBuilder);
                    selector.select();

                    jit::LivenessAnalyzer liveness(lirGraph);
                    liveness.analyze();

                    jit::LinearScanAllocator allocator(lirGraph, liveness);
                    allocator.allocate();

                    jit::MacroAssembler masm;
                    jit::CodeEmitter emitter(lirGraph, masm, reinterpret_cast<void*>(jit::jc2_jit_deoptimize), reinterpret_cast<void**>(&globalsDataPtr), reinterpret_cast<void*>(jc2_jit_call_helper));
                    
                    emitter.emit(allocator.getStackSize());
                    masm.emitConstantPool();

                    auto mem = std::make_shared<jit::ExecutableMemory>();
                    masm.finalize(*mem);

                    if (g_showMachineCode) {
                        std::cout << "--- Tier 2 Machine Code (Size: " << mem->size() << " bytes) ---\n";
                        const uint8_t* code = mem->get();
                        jit::disassemble(code, mem->size(), std::cout);
                        std::cout << "\n";
                    }

                    jitCompiledCode[fnIdx] = mem;
                    jitEntryPoints[fnIdx] = mem->get();
                    if (g_profile) std::cout << "[JIT] Tier 2 Compilation successful for function '" << fn->name << "'\n";
                } catch (const std::exception& e) {
                    if (g_profile) std::cout << "[JIT] Tier 2 Compilation failed for function '" << fn->name << "': " << e.what() << "\n";
                    // JIT 编译失败，回退到解释器，并标记不再尝试编译
                    jitEntryPoints[fnIdx] = nullptr; 
                }
            }
        }
    }

    if (!g_profile) return;
    frame->startTime = std::chrono::steady_clock::now();
    frame->childTimeMs = 0.0;
    frame->instructionCount = 0;
}

void VM::profileFrameEnd(CallFrame* frame) {
    if (!g_profile) return;
    auto endTime = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(endTime - frame->startTime).count();
    std::string fnName = frame->function ? frame->function->name : "<unknown>";
    auto& record = profileData[fnName];
    record.callCount++;
    record.totalTimeMs += elapsed;
    record.selfTimeMs += (elapsed - frame->childTimeMs);
    record.instructionCount += frame->instructionCount;
    if (frameCount > 1) {
        frames[frameCount - 2].childTimeMs += elapsed;
    }
}

void VM::printProfileInfo() {
    if (!g_profile || profileData.empty()) return;
    std::cout << "\n" << jc::col(jc::Ansi::BRIGHT_CYAN) << "=== Profiler Results ===" << jc::col(jc::Ansi::RESET) << "\n";
    std::cout << std::left << std::setw(30) << "Function" 
              << std::right << std::setw(10) << "Calls" 
              << std::setw(15) << "Insts" 
              << std::setw(15) << "Total(ms)" 
              << std::setw(15) << "Self(ms)" << "\n";
    std::cout << std::string(85, '-') << "\n";
    
    std::vector<std::pair<std::string, ProfileRecord>> sortedData(profileData.begin(), profileData.end());
    std::sort(sortedData.begin(), sortedData.end(), [](const auto& a, const auto& b) {
        return a.second.selfTimeMs > b.second.selfTimeMs;
    });
    
    for (const auto& [name, record] : sortedData) {
        std::string dispName = name;
        if (dispName.length() > 28) dispName = dispName.substr(0, 25) + "...";
        std::cout << std::left << std::setw(30) << dispName 
                  << std::right << std::setw(10) << record.callCount 
                  << std::setw(15) << record.instructionCount
                  << std::setw(15) << std::fixed << std::setprecision(3) << record.totalTimeMs 
                  << std::setw(15) << std::fixed << std::setprecision(3) << record.selfTimeMs << "\n";
    }
    std::cout << std::string(85, '-') << "\n";
}

VM::VM() {
    activeVM = this;
    GcHeap::get().isInitializing = true;
    registers = new Value[MAX_REGISTERS];
    frames = new CallFrame[MAX_FRAMES];
    globalsDataPtr = globals.data();
    
    GcHeap::get().markCallback = [this]() {
        for (size_t i = 0; i < globals.size(); ++i) {
            GcHeap::get().markValue(globals[i]);
        }
        
        int maxReg = 0;
        for (int i = 0; i < frameCount; ++i) {
            CallFrame& f = frames[i];
            GcHeap::get().markValue(f.selfContext);
            GcHeap::get().markValue(f.classContext);
            GcHeap::get().markValue(f.jitReturnSlot);
            if (f.closure) GcHeap::get().markObj(f.closure);
            if (f.chunk) {
                for (auto& v : f.chunk->constants) GcHeap::get().markValue(v);
                for (auto& ic : f.chunk->inlineCaches) {
                    if (ic.cachedMethod) GcHeap::get().markObj(ic.cachedMethod);
                    if (ic.cachedClass) GcHeap::get().markObj(ic.cachedClass);
                }
            }
            
            int frameEnd = f.registerBase + f.function->localCount + f.function->refCount;
            if (frameEnd > maxReg) maxReg = frameEnd;
        }
        for (int i = 0; i < maxReg; ++i) {
            GcHeap::get().markValue(registers[i]);
        }
        
        if (pendingFrameBase >= 0 && pendingFrameCount > 0) {
            for (int i = 0; i < pendingFrameCount; ++i) {
                GcHeap::get().markValue(registers[pendingFrameBase + i]);
            }
        }
        
        ObjUpVal* uv = openUpvalues;
        while (uv) {
            GcHeap::get().markObj(uv);
            uv = uv->nextOpen;
        }
        
        for (auto& pr : pendingCallRefs) {
            GcHeap::get().markObj(pr.second);
        }
        
        for (auto* closure : deferStack) {
            GcHeap::get().markObj(closure);
        }
        
        for (auto& [k, v] : loadedModules) {
            GcHeap::get().markValue(v);
        }
        
        for (auto& [k, v] : builtinClosures) {
            GcHeap::get().markValue(v);
        }
        
        for (auto& [k, v] : builtinValues) {
            GcHeap::get().markValue(v);
        }
        if (listProto) GcHeap::get().markObj(listProto);
        if (dictProto) GcHeap::get().markObj(dictProto);
        if (setProto) GcHeap::get().markObj(setProto);
        if (stringProto) GcHeap::get().markObj(stringProto);
        if (matrixProto) GcHeap::get().markObj(matrixProto);

        for (auto& v : helpers::nativeSelfStack) GcHeap::get().markValue(v);
        for (auto& v : helpers::nativeClassStack) GcHeap::get().markValue(v);
        for (auto& v : jc::nativeTempRefs) GcHeap::get().markValue(v);
        
        for (auto& fn : compiledFunctions) {
            if (fn) {
                for (auto& v : fn->chunk.constants) GcHeap::get().markValue(v);
                for (auto& ic : fn->chunk.inlineCaches) {
                    if (ic.cachedMethod) GcHeap::get().markObj(ic.cachedMethod);
                    if (ic.cachedClass) GcHeap::get().markObj(ic.cachedClass);
                }
            }
        }
    };

    GcHeap::get().sweepCallback = []() {
        for (auto it = g_internedStrings.begin(); it != g_internedStrings.end(); ) {
            if (!it->second->isMarked) {
                it = g_internedStrings.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = g_internedTypes.begin(); it != g_internedTypes.end(); ) {
            if (!it->second->isMarked) {
                it = g_internedTypes.erase(it);
            } else {
                ++it;
            }
        }
    };

    GcHeap::get().hasFinalizerCallback = [this](Obj* obj) -> bool {
        if (obj->type != ObjType::INSTANCE) return false;
        ObjInstance* inst = static_cast<ObjInstance*>(obj);
        if (inst->is_finalized) return false;
        auto [delMethod, owner] = findDunder(Value(inst), "<finalize>");
        if (delMethod) {
            inst->is_finalized = true;
            return true;
        }
        return false;
    };

    GcHeap::get().executeFinalizerCallback = [this](Obj* obj) {
        if (obj->type != ObjType::INSTANCE) return;
        ObjInstance* inst = static_cast<ObjInstance*>(obj);
        auto [delMethod, owner] = findDunder(Value(inst), "<finalize>");
        if (delMethod) {
            try {
                callDunder(Value(inst), delMethod, owner, {});
            } catch (const std::exception& e) {
                std::cerr << jc::col(jc::Ansi::RED) << "Exception ignored in finalize: " << e.what() << jc::col(jc::Ansi::RESET) << "\n";
            } catch (...) {
                std::cerr << jc::col(jc::Ansi::RED) << "Unknown exception ignored in finalize" << jc::col(jc::Ansi::RESET) << "\n";
            }
        }
    };

    auto makeType = [](BuiltinType bt) {
        return Value(internType({bt}));
    };
    builtinValues["any"] = makeType(BuiltinType::ANY);
    builtinValues["never"] = Value(internType({}));
    builtinValues["int"] = makeType(BuiltinType::INT);
    builtinValues["double"] = makeType(BuiltinType::FLOAT);
    builtinValues["real"] = Value(internType({BuiltinType::INT, BuiltinType::FLOAT, BuiltinType::FRACTION, BuiltinType::BOOL}));
    builtinValues["number"] = Value(internType({BuiltinType::INT, BuiltinType::FLOAT, BuiltinType::FRACTION, BuiltinType::COMPLEX, BuiltinType::BOOL}));
    builtinValues["exact"] = Value(internType({BuiltinType::INT, BuiltinType::FRACTION, BuiltinType::SYMBOLIC, BuiltinType::BOOL}));
    builtinValues["string"] = makeType(BuiltinType::STRING);
    builtinValues["bool"] = makeType(BuiltinType::BOOL);
    builtinValues["none_type"] = makeType(BuiltinType::NONE_TYPE);
    builtinValues["list"] = makeType(BuiltinType::LIST);
    builtinValues["dict"] = makeType(BuiltinType::DICT);
    builtinValues["set"] = makeType(BuiltinType::SET);
    builtinValues["fraction"] = makeType(BuiltinType::FRACTION);
    builtinValues["complex"] = makeType(BuiltinType::COMPLEX);
    builtinValues["symbolic"] = makeType(BuiltinType::SYMBOLIC);
    builtinValues["realmatrix"] = makeType(BuiltinType::REALMAT);
    builtinValues["complexmatrix"] = makeType(BuiltinType::COMPLEXMAT);
    builtinValues["symmatrix"] = makeType(BuiltinType::SYMMAT);
    builtinValues["matrix"] = Value(internType({BuiltinType::REALMAT, BuiltinType::COMPLEXMAT, BuiltinType::SYMMAT}));
    builtinValues["function"] = makeType(BuiltinType::FUNC);
    builtinValues["class_type"] = makeType(BuiltinType::CLASS);
    builtinValues["instance"] = makeType(BuiltinType::INSTANCE);
    builtinValues["namespace_type"] = makeType(BuiltinType::NAMESPACE);
    builtinValues["type"] = makeType(BuiltinType::TYPE_DEF);
    builtinValues["slice"] = makeType(BuiltinType::SLICE);

    // ===== 可调用类型的转换回调（call/invoke 直接调用，不再查同名函数）=====
    {
        auto evalIfSym = [this](Value v) -> Value {
            if (v.isSymbolic()) {
                auto it = nativeBuiltins.find("evalf");
                if (it != nativeBuiltins.end()) return it->second({v});
            }
            return v;
        };
        auto bind = [this](const std::string& name, std::set<int> arity, std::vector<std::string> params,
                           std::function<Value(const std::vector<Value>&)> fn,
                           std::string restName = "", std::vector<std::string> kwargNames = {}, std::string kwargsName = "", int kwargDefaultCount = 0) {
            ObjTypeDef* td = static_cast<ObjTypeDef*>(builtinValues[name].asObj());
            td->converter = std::move(fn);
            td->converterArity = std::move(arity);
            td->converterParamNames = std::move(params);
            td->converterRestName = std::move(restName);
            td->converterKwargNames = std::move(kwargNames);
            td->converterKwargsName = std::move(kwargsName);
            td->converterKwargDefaultCount = kwargDefaultCount;
        };

        bind("int", {1}, {"x"}, [this, evalIfSym](const std::vector<Value>& args) -> Value {
            Value val = evalIfSym(args[0]);
            // 截断取整（向零方向）
            if (val.isObjType(ObjType::BIGINT) || val.isInt32())
                return val;
            if (val.isObjType(ObjType::FRACTION)) {
                const auto& f = static_cast<ObjFraction*>(val.asObj())->frac;
                return Value(f.getNum() / f.getDen());
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

        bind("double", {1}, {"x"}, [this, evalIfSym](const std::vector<Value>& args) -> Value {
            Value val = evalIfSym(args[0]);
            if (val.isComplex()) {
                const auto& c = val.asComplex();
                if (!Tol::isEq(c.imag, 0.0))
                    throw std::runtime_error("Type Error: Cannot convert complex with nonzero imaginary part to double.");
                return Value(c.real);
            }
            return Value(val.asDouble());
        });

        bind("complex", {1, 2}, {"real", "imag"}, [this, evalIfSym](const std::vector<Value>& args) -> Value {
            if (args.size() == 1) {
                Value val = evalIfSym(args[0]);
                if (val.isComplex())
                    return val;
                return Value(Complex(val.asDouble(), 0.0));
            }
            return Value(Complex(evalIfSym(args[0]).asDouble(), evalIfSym(args[1]).asDouble()));
        });

        bind("bool", {1}, {"x"}, [this](const std::vector<Value>& args) -> Value {
            return Value(evaluateTruthiness(args[0]));
        });

        bind("string", {1}, {"x"}, [this](const std::vector<Value>& args) -> Value {
            if (args[0].isInstance()) {
                auto [d, owner] = findDunder(args[0], DUNDER_STR);
                if (d) return callDunder(args[0], d, owner, {});
            }
            if (args[0].isString()) return args[0];
            std::ostringstream oss; oss << args[0]; return Value(oss.str());
        });

        bind("list", {}, {}, [](const std::vector<Value>& args) -> Value {
            // ★ 统一调用约定：args = [rest_list]
            const std::vector<Value>& items = static_cast<ObjList*>(args[0].asObj())->vec;
            ObjList* L = GcHeap::get().allocate<ObjList>(); GcObjGuard guard(L);
            for (const auto& a : items) L->vec.push_back(a);
            return Value(L);
        }, "elements");

        bind("dict", {}, {}, [](const std::vector<Value>& args) -> Value {
            // ★ 统一调用约定：args = [rest_list]
            const std::vector<Value>& items = static_cast<ObjList*>(args[0].asObj())->vec;
            if (items.size() % 2 != 0) throw std::runtime_error("Runtime Error: dict() expects even number of arguments.");
            ObjDict* d = GcHeap::get().allocate<ObjDict>();
            GcObjGuard guard(d);
            for (size_t i = 0; i < items.size(); i += 2) {
                d->keyMap[items[i]] = d->elements.size();
                d->elements.push_back({items[i], items[i + 1]});
            }
            return Value(d);
        }, "pairs");

        bind("set", {}, {}, [](const std::vector<Value>& args) -> Value {
            // ★ 统一调用约定：args = [rest_list]
            const std::vector<Value>& items = static_cast<ObjList*>(args[0].asObj())->vec;
            ObjSet* s = GcHeap::get().allocate<ObjSet>();
            GcObjGuard guard(s);
            for (const auto& a : items) {
                if (s->keys.find(a) == s->keys.end()) {
                    s->keys.insert(a);
                    s->elements.push_back(a);
                }
            }
            return Value(s);
        }, "elements");

        bind("symmatrix", {}, {"rows", "cols"}, [](const std::vector<Value>& args) -> Value {
            // ★ 统一调用约定：args = [rows, cols, rest_list]
            int r = static_cast<int>(std::round(args[0].asDouble()));
            int c = static_cast<int>(std::round(args[1].asDouble()));
            if (r <= 0 || c <= 0)
                throw std::runtime_error("Runtime Error: symmatrix() dimensions must be positive.");
            const std::vector<Value>& items = static_cast<ObjList*>(args[2].asObj())->vec;
            if (items.empty())
                return Value(SymMatrix(r, c));
            int total = r * c;
            if (static_cast<int>(items.size()) != total)
                throw std::runtime_error("Runtime Error: symmatrix() element count mismatch: "
                    "expected " + std::to_string(total) + ", got " +
                    std::to_string(items.size()) + ".");
            std::vector<SymExpr> flat;
            flat.reserve(total);
            for (int i = 0; i < total; ++i) {
                flat.push_back(items[i].asSymbolic());
            }
            return Value(SymMatrix(r, c, flat));
        }, "elements");

        bind("slice", {0, 1, 2, 3}, {"start", "end", "step"}, [](const std::vector<Value>& args) -> Value {
            auto checkArg = [](const Value& v, const std::string& name) -> int {
                if (v.isNone()) return ObjSlice::SLICE_NONE;
                if (!v.isNumber() && !v.isBigInt()) {
                    throw std::runtime_error("Type Error: slice " + name + " must be a number or none.");
                }
                int64_t val64 = 0;
                if (v.isInt32()) {
                    val64 = v.asInt32();
                } else if (v.isDouble()) {
                    val64 = static_cast<int64_t>(std::round(v.asDouble()));
                } else {
                    try {
                        val64 = v.asBigInt().toInt64();
                    } catch (...) {
                        throw std::runtime_error("Value Error: slice " + name + " absolute value exceeds 2^31-1.");
                    }
                }
                if (val64 > 2147483647LL || val64 < -2147483647LL) {
                    throw std::runtime_error("Value Error: slice " + name + " absolute value exceeds 2^31-1.");
                }
                return static_cast<int>(val64);
            };
            int start = args.size() > 0 ? checkArg(args[0], "start") : ObjSlice::SLICE_NONE;
            int end = args.size() > 1 ? checkArg(args[1], "end") : ObjSlice::SLICE_NONE;
            int step = args.size() > 2 ? checkArg(args[2], "step") : ObjSlice::SLICE_NONE;
            ObjSlice* sliceObj = GcHeap::get().allocate<ObjSlice>();
            sliceObj->start = start;
            sliceObj->end = end;
            sliceObj->step = step;
            return Value(sliceObj);
        });

        bind("matrix", {}, {"rows", "cols"}, [](const std::vector<Value>& args) -> Value {
            // ★ 统一调用约定：args = [rows, cols, rest_list]
            int r = static_cast<int>(std::round(args[0].asDouble()));
            int c = static_cast<int>(std::round(args[1].asDouble()));
            if (r <= 0 || c <= 0)
                throw std::runtime_error("Runtime Error: matrix() dimensions must be positive.");
            const std::vector<Value>& items = static_cast<ObjList*>(args[2].asObj())->vec;
            if (items.empty())
                return Value(RealMatrix(r, c));
            int total = r * c;
            if (static_cast<int>(items.size()) != total)
                throw std::runtime_error("Runtime Error: matrix() element count mismatch: "
                    "expected " + std::to_string(total) + ", got " +
                    std::to_string(items.size()) + ".");
            bool hasSymbolic = false;
            bool hasComplex = false;
            for (size_t i = 0; i < items.size(); ++i) {
                if (items[i].isSymbolic()) hasSymbolic = true;
                else if (items[i].isComplex()) hasComplex = true;
            }
            if (hasSymbolic) {
                std::vector<SymExpr> flat;
                flat.reserve(total);
                for (int i = 0; i < total; ++i)
                    flat.push_back(items[i].asSymbolic());
                return Value(SymMatrix(r, c, flat));
            }
            if (hasComplex) {
                std::vector<Complex> flat;
                flat.reserve(total);
                for (int i = 0; i < total; ++i)
                    flat.push_back(items[i].asComplex());
                return Value(ComplexMatrix(r, c, flat));
            }
            std::vector<double> flat;
            flat.reserve(total);
            for (int i = 0; i < total; ++i)
                flat.push_back(items[i].asDouble());
            return Value(RealMatrix(r, c, flat));
        }, "elements");
    }

    listProto = GcHeap::get().allocate<ObjClass>();
    listProto->name = "List";
    dictProto = GcHeap::get().allocate<ObjClass>();
    dictProto->name = "Dict";
    setProto = GcHeap::get().allocate<ObjClass>();
    setProto->name = "Set";
    stringProto = GcHeap::get().allocate<ObjClass>();
    stringProto->name = "String";
    matrixProto = GcHeap::get().allocate<ObjClass>();
    matrixProto->name = "Matrix";

    nativeBuiltins["__dbg_reg"] = [this](const std::vector<Value>& args) -> Value {
        if (!currentDebuggerFrame) throw std::runtime_error("Debugger not active.");
        int reg = static_cast<int>(args[0].asDouble());
        CallFrame* frame = currentDebuggerFrame;
        int maxRegs = frame->function ? (frame->function->localCount + frame->function->refCount) : 0;
        if (reg >= 0 && reg < maxRegs) {
            int locals = frame->function ? frame->function->localCount : 0;
            return (reg < locals) ? registers[frame->registerBase + reg] : registers[frame->refParamsBase + (reg - locals)];
        }
        throw std::runtime_error("Register out of bounds.");
    };
    builtinArity["__dbg_reg"] = {1};

    nativeBuiltins["__dbg_type_feedback"] = [this](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunctionClosure()) throw std::runtime_error("Expected a function.");
        ObjClosure* closure = args[0].asFunction();
        if (!closure->isBytecode()) throw std::runtime_error("Expected a bytecode function.");
        auto& fnDef = compiledFunctions[closure->compiledFnIndex];
        ObjList* list = GcHeap::get().allocate<ObjList>();
        for (uint8_t fb : fnDef->chunk.typeFeedback) {
            list->vec.push_back(Value::fromInt32(fb));
        }
        return Value(list);
    };
    builtinArity["__dbg_type_feedback"] = {1};

    nativeBuiltins["__dbg_is_jitted"] = [this](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunctionClosure()) return Value(false);
        ObjClosure* closure = args[0].asFunction();
        if (!closure->isBytecode()) return Value(false);
        return Value(getJitEntryPoint(closure->compiledFnIndex) != nullptr);
    };
    builtinArity["__dbg_is_jitted"] = {1};
}

VM::~VM() {
    delete[] registers;
    delete[] frames;
}

void VM::triggerDebugger() {
    g_autoDebug = true;
    std::cout << jc::col(jc::Ansi::BRIGHT_YELLOW) << "=== Breakpoint Hit ===" << jc::col(jc::Ansi::RESET) << "\n";
}

Value VM::callVMFunction(int fnIdx, const std::vector<Value>& args, ObjClosure* closure, Value boundSelf, Value boundClass) {
    auto& fnDef = compiledFunctions[fnIdx];
    CallFrame newFrame;
    newFrame.function = fnDef.get();
    newFrame.chunk = &fnDef->chunk;
    newFrame.ip = 0;
    
    int newBase = 0;
    if (frameCount > 0) {
        CallFrame* currentFrame = &frames[frameCount - 1];
        newBase = currentFrame->registerBase + currentFrame->function->localCount + currentFrame->function->refCount;
    }
    
    int newTotalCount = fnDef->localCount + fnDef->refCount;
    PendingFrameGuard pfg(this, newBase, newTotalCount);

    newFrame.registerBase = newBase;
    newFrame.returnRegister = 0;
    newFrame.deferBase = static_cast<int>(deferStack.size());
    newFrame.closure = closure;
    newFrame.selfContext = boundSelf;
    newFrame.classContext = boundClass;
    
    std::vector<Value> actualArgs;
    if (closure && closure->isUFCS) {
        actualArgs.reserve(args.size() + 1);
        actualArgs.push_back(closure->boundSelf);
        actualArgs.insert(actualArgs.end(), args.begin(), args.end());
    } else {
        actualArgs = args;
    }
    
    int totalArgc = static_cast<int>(actualArgs.size());

    if (!fnDef->restName.empty()) {
        int fixedMax = fnDef->maxArity;
        if (totalArgc < fnDef->arity) {
            int expected = closure && closure->isUFCS ? fnDef->arity - 1 : fnDef->arity;
            if (expected < 0) expected = 0;
            throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(expected) + " arguments.");
        }
        ObjList* restList = GcHeap::get().allocate<ObjList>();
        if (totalArgc > fixedMax) {
            int restCount = totalArgc - fixedMax;
            restList->vec.reserve(restCount);
            for (int j = 0; j < restCount; j++) {
                restList->vec.push_back(actualArgs[fixedMax + j]);
            }
        }
        
        for (int i = 0; i < std::min(totalArgc, fixedMax); ++i) {
            registers[newBase + i] = actualArgs[i];
        }
        for (int i = totalArgc; i < fixedMax; ++i) {
            registers[newBase + i] = Value::uninit();
        }
        registers[newBase + fixedMax] = Value(restList);
    } else {
        if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
            int expMin = closure && closure->isUFCS ? fnDef->arity - 1 : fnDef->arity;
            int expMax = closure && closure->isUFCS ? fnDef->maxArity - 1 : fnDef->maxArity;
            if (expMin < 0) expMin = 0;
            if (expMax < 0) expMax = 0;
            int gotArgs = closure && closure->isUFCS ? totalArgc - 1 : totalArgc;
            if (gotArgs < 0) gotArgs = 0;
            throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(expMin) + " to " + std::to_string(expMax) + " arguments, got " + std::to_string(gotArgs) + ".");
        }
        for (int i = 0; i < totalArgc; ++i) {
            registers[newBase + i] = actualArgs[i];
        }
        for (int i = totalArgc; i < fnDef->maxArity; ++i) {
            registers[newBase + i] = Value::uninit();
        }
    }

    int paramSlotCount = fnDef->maxArity + (fnDef->restName.empty() ? 0 : 1) + static_cast<int>(fnDef->kwargNames.size()) + (fnDef->kwargsName.empty() ? 0 : 1);
            for (int i = paramSlotCount; i < fnDef->localCount; ++i) {
        registers[newBase + i] = Value::none();
    }
    
    populateRefParams(newFrame, fnDef.get());
    
    profileFrameStart(&newFrame);
    frames[frameCount++] = newFrame;
    
    if (jitEntryPoints.count(fnIdx) && jitEntryPoints[fnIdx] != nullptr) {
        typedef uint64_t (*JitFunc)(Value*);
        JitFunc func = reinterpret_cast<JitFunc>(jitEntryPoints[fnIdx]);
        jit::g_jc2_jit_deoptimized = false;
        uint64_t retBits = func(&registers[newBase]);
        if (jit::g_jc2_jit_deoptimized) {
            jit::g_jc2_jit_deoptimized = false;
            // 去优化：状态已由 jc2_jit_deoptimize 恢复，直接继续解释执行
            jitEntryPoints[fnIdx] = nullptr;
            jitCompiledCode.erase(fnIdx);
            fnDef->callCount = 0;
            if (jit::g_jit_pending_exception) {
                jit::g_jit_pending_exception = 0;
                Value exVal = jit_exception_value;
                jit_exception_value = Value::none();
                throw ValueException(exVal);
            }
            // Fall through to run() below
        } else {
            Value retVal = Value::fromRawBits(retBits);
                
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
                
            return retVal;
        }
    }

    int targetDepth = frameCount - 1;
    try {
        return run(targetDepth);
    } catch (...) {
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        throw;
    }
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
    
    int newBase = 0;
    if (frameCount > 0) {
        CallFrame* prev = &frames[frameCount - 1];
        newBase = prev->registerBase + prev->function->localCount + prev->function->refCount;
    }
    int newTotalCount = mainFn->localCount + mainFn->refCount;
    PendingFrameGuard pfg(this, newBase, newTotalCount);

    mainFrame.registerBase = newBase;
    mainFrame.returnRegister = 0;
    mainFrame.deferBase = static_cast<int>(deferStack.size());
    
    if (frameCount >= MAX_FRAMES) throw std::runtime_error("VM Error: CallFrame stack overflow.");
    
    int targetDepth = frameCount;
    profileFrameStart(&mainFrame);
    frames[frameCount++] = mainFrame;

    try {
        Value res = run(targetDepth);
        frames[frameCount].selfContext = Value::none();
        frames[frameCount].classContext = Value::none();
        frames[frameCount].jitReturnSlot = Value::none();
        frames[frameCount].closure = nullptr;
        frames[frameCount].refParamsBase = -1;
        return res;
    } catch (ValueException& ex) {
        while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= targetDepth) {
            exceptionHandlers.pop_back();
        }
        Value errVal = wrapException("Exception", ex.val);
        try { runDefersDownTo(frames[targetDepth].deferBase, &errVal); } catch (...) {}
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            profileFrameEnd(f);
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        pendingCallRefs.clear();
        throw RuntimeError("", errVal);
    } catch (RuntimeError& ex) {
        while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= targetDepth) {
            exceptionHandlers.pop_back();
        }
        Value errVal = wrapException(ex.type, ex.message);
        try { runDefersDownTo(frames[targetDepth].deferBase, &errVal); } catch (...) {}
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
            frameCount--;
        }
        pendingCallRefs.clear();
        throw;
    } catch (...) {
        while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= targetDepth) {
            exceptionHandlers.pop_back();
        }
        try { runDefersDownTo(frames[targetDepth].deferBase); } catch (...) {}
        while (frameCount > targetDepth) {
            CallFrame* f = &frames[frameCount - 1];
            int clearBase = f->registerBase;
            int clearCount = f->function->localCount + f->function->refCount;
            for (int i = 0; i < clearCount; ++i) {
                registers[clearBase + i] = Value::none();
            }
            f->selfContext = Value::none();
            f->classContext = Value::none();
            f->jitReturnSlot = Value::none();
            f->closure = nullptr;
            f->refParamsBase = -1;
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
    Value* frameRegs = &registers[frame->registerBase];
    int ip = frame->ip;
    
    // 提取 EXTRAARG 扩展操作数 (24-bit)
    #define FETCH_EXTRA() (code[ip++] >> 8)

    // 懒构建循环头标志：基于 CFG（dominator 回边）识别真正的循环头，
    // 避免把循环体内部的向后跳转（如 zombie 里 ty 循环退出后跳回 GET_SELF）误判为回边。
    #define ENSURE_OSR_LOOP_HEADERS() \
        do { \
            if (!chunk->osrLoopHeadersComputed) { \
                jit::BytecodeCFG _cfg; \
                _cfg.build(*chunk); \
                const_cast<Chunk*>(chunk)->osrLoopHeaderFlags.assign(chunk->code.size(), 0); \
                for (const auto& _blk : _cfg.blocks) { \
                    if (_blk.isLoopHeader && _blk.startIp >= 0 && _blk.startIp < static_cast<int>(chunk->code.size())) { \
                        const_cast<Chunk*>(chunk)->osrLoopHeaderFlags[_blk.startIp] = 1; \
                    } \
                } \
                const_cast<Chunk*>(chunk)->osrLoopHeadersComputed = true; \
            } \
        } while (0)

    // 获取物理寄存器或溢出槽 (Unified Address Space)
    #define getReg(idx) frameRegs[idx]

    // K-Bit 机制：解析寄存器或常量池索引 (按值返回，强制放入寄存器，消除内存间接访问)
    #define GET_RK(rk) (ISK(rk) ? ((rk) == ESCAPE_KBIT_CONST ? chunk->constants.data()[FETCH_EXTRA()] : chunk->constants.data()[INDEXK(rk)]) : ((rk) == ESCAPE_KBIT_REG ? frameRegs[FETCH_EXTRA()] : frameRegs[rk]))

    #define ATTEMPT_OSR(loopHeaderIp) \
        do { \
            void* osrEntry = osrEntryPoints[fnIdx][loopHeaderIp]; \
            if (osrEntry) { \
                if (g_profile) std::cout << "[JIT] Executing OSR machine code from IP: " << loopHeaderIp << "\n"; \
                typedef uint64_t (*JitFunc)(Value*); \
                JitFunc func = reinterpret_cast<JitFunc>(osrEntry); \
                jit::g_jc2_jit_deoptimized = false; \
                uint64_t retBits = func(frameRegs); \
                if (jit::g_jc2_jit_deoptimized) { \
                    if (g_profile) std::cout << "[JIT] OSR Deoptimized! Falling back to interpreter at IP: " << frame->ip << "\n"; \
                    jit::g_jc2_jit_deoptimized = false; \
                    ip = frame->ip; \
                    osrTriggered = true; \
                    if (frame->ip >= 0 && frame->ip < (int)chunk->typeFeedback.size()) { \
                        const_cast<Chunk*>(chunk)->typeFeedback[frame->ip] |= 0x80; \
                    } \
                    osrEntryPoints[fnIdx][loopHeaderIp] = nullptr; \
                    osrCompiledCode[fnIdx].erase(loopHeaderIp); \
                    const_cast<Chunk*>(chunk)->osrCounters[op_ip] = 0; \
                    if (jit::g_jit_pending_exception) { \
                        jit::g_jit_pending_exception = 0; \
                        Value exVal = jit_exception_value; \
                        jit_exception_value = Value::none(); \
                        throw ValueException(exVal); \
                    } \
                } else { \
                    if (g_profile) std::cout << "[JIT] OSR Execution completed successfully.\n"; \
                    Value res = Value::fromRawBits(retBits); \
                    runDefersDownTo(frame->deferBase); \
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) { \
                        exceptionHandlers.pop_back(); \
                    } \
                    closeUpvalues(frame->registerBase); \
                    int targetReg = frame->returnRegister; \
                    bool isInit = (frame->function && frame->function->name == "init"); \
                    Value selfCtx = frame->selfContext; \
                    profileFrameEnd(frame); \
                    int clearBase = frame->registerBase; \
                    int clearCount = frame->function->localCount + frame->function->refCount; \
                    for (int i = 0; i < clearCount; ++i) { \
                        registers[clearBase + i] = Value::none(); \
                    } \
                    frame->jitReturnSlot = Value::none(); \
                    frame->selfContext = Value::none(); \
                    frame->classContext = Value::none(); \
                    frameCount--; \
                    if (frameCount <= targetFrameDepth) return res; \
                    frame = &frames[frameCount - 1]; \
                    chunk = frame->chunk; \
                    code = chunk->code.data(); \
                    frameRegs = &registers[frame->registerBase]; \
                    ip = frame->ip; \
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx; \
                    else getReg(targetReg) = res; \
                    osrTriggered = true; \
                } \
            } \
        } while(0)

    int prevLine = -1;
    int lastBrokenLine = -1;
    int pendingKwArgc = 0;

    while (true) {
        try {
            while (true) {
                int currentLine = (ip < static_cast<int>(chunk->lines.size())) ? chunk->lines[ip] : 0;
                if (currentLine != prevLine) {
                    prevLine = currentLine;
                    lastBrokenLine = -1;
                }

                bool breakHit = g_autoDebug;
                
                if (!breakHit && currentLine > 0 && currentLine != lastBrokenLine) {
                    if (breakpoints.count(currentLine)) {
                        breakHit = true;
                        std::cout << jc::col(jc::Ansi::BRIGHT_YELLOW) << "=== Breakpoint Hit at Line " << currentLine << " ===" << jc::col(jc::Ansi::RESET) << "\n";
                    }
                }
                
                if (!breakHit && !watchpoints.empty()) {
                    int maxRegs = frame->function ? (frame->function->localCount + frame->function->refCount) : 0;
                    int locals = frame->function ? frame->function->localCount : 0;
                    for (const auto& wp : watchpoints) {
                        if (wp.reg >= 0 && wp.reg < maxRegs) {
                            Value v = (wp.reg < locals) ? registers[frame->registerBase + wp.reg] : registers[frame->refParamsBase + (wp.reg - locals)];
                            bool cond = false;
                            if (v.isDouble() || v.isInt32()) {
                                double d = v.isDouble() ? v.asDoubleRaw() : v.asInt32();
                                if (wp.op == "==") cond = (d == wp.val);
                                else if (wp.op == "!=") cond = (d != wp.val);
                                else if (wp.op == ">") cond = (d > wp.val);
                                else if (wp.op == "<") cond = (d < wp.val);
                                else if (wp.op == ">=") cond = (d >= wp.val);
                                else if (wp.op == "<=") cond = (d <= wp.val);
                            }
                            if (cond) {
                                breakHit = true;
                                std::cout << jc::col(jc::Ansi::BRIGHT_YELLOW) << "=== Watchpoint Hit: R(" << wp.reg << ") " << wp.op << " " << wp.val << " ===" << jc::col(jc::Ansi::RESET) << "\n";
                                break;
                            }
                        }
                    }
                }

                if (breakHit) {
                    lastBrokenLine = currentLine;
                    g_autoDebug = true;
                    std::cout << "\n";
                    chunk->disassembleInstruction(ip);
                    while (true) {
                        std::cout << jc::col(jc::Ansi::BRIGHT_CYAN) << "(debug) " << jc::col(jc::Ansi::RESET);
                        std::string line;
                        if (!std::getline(std::cin, line)) std::exit(1);
                        
                        if (line.empty() || line == "s" || line == "step") {
                            break;
                        } else if (line == "c" || line == "continue") {
                            g_autoDebug = false;
                            break;
                        } else if (line == "detach") {
                            g_autoDebug = false;
                            breakpoints.clear();
                            watchpoints.clear();
                            break;
                        } else if (line == "q" || line == "quit") {
                            g_autoDebug = false;
                            breakpoints.clear();
                            watchpoints.clear();
                            throw EngineInterruptError();
                        } else if (line == "h" || line == "help") {
                            std::cout << "Debugger Commands:\n"
                                      << "  h, help                 Show this help message\n"
                                      << "  s, step                 Step to the next instruction\n"
                                      << "  c, continue             Continue execution until next breakpoint\n"
                                      << "  detach                  Disable debugger and continue execution\n"
                                      << "  b <line>, break <line>  Set a breakpoint at <line>\n"
                                      << "  break if R(<r>) <op> <v> Set a conditional breakpoint (e.g., break if R(0) > 5)\n"
                                      << "  clear breaks            Clear all breakpoints and watchpoints\n"
                                      << "  p <expr>                Evaluate and print expression (or R(x))\n"
                                      << "  r, regs                 Show all registers in current frame\n"
                                      << "  g, globals              Show all global variables\n"
                                      << "  bt, backtrace           Show call stack\n"
                                      << "  q, quit                 Terminate debugger and return to REPL\n";
                        } else if (line.substr(0, 8) == "break if") {
                            int reg; char opStr[3]; double val;
                            if (sscanf(line.c_str(), "break if R(%d) %2s %lf", &reg, opStr, &val) == 3) {
                                watchpoints.push_back({reg, opStr, val});
                                std::cout << "Condition breakpoint added: R(" << reg << ") " << opStr << " " << val << "\n";
                            } else {
                                std::cout << "Invalid format. Use: break if R(x) > 5\n";
                            }
                        } else if (line.substr(0, 6) == "break " || line.substr(0, 2) == "b ") {
                            try {
                                int ln = std::stoi(line.substr(line.find(' ') + 1));
                                breakpoints.insert(ln);
                                std::cout << "Breakpoint added at line " << ln << "\n";
                            } catch (...) {
                                std::cout << "Invalid line number.\n";
                            }
                        } else if (line == "clear breaks") {
                            breakpoints.clear();
                            watchpoints.clear();
                            std::cout << "All breakpoints and watchpoints cleared.\n";
                        } else if (line.substr(0, 2) == "p ") {
                            std::string expr = line.substr(2);
                            size_t pos = 0;
                            while ((pos = expr.find("R(", pos)) != std::string::npos) {
                                expr.replace(pos, 2, "__dbg_reg(");
                                pos += 10;
                            }
                            if (helpers::evalCallback) {
                                currentDebuggerFrame = frame;
                                bool prevDebug = g_autoDebug;
                                g_autoDebug = false;
                                try {
                                    Value res = helpers::evalCallback(expr);
                                    std::cout << res << "\n";
                                } catch (const std::exception& e) {
                                    std::cout << "Error: " << e.what() << "\n";
                                }
                                g_autoDebug = prevDebug;
                                currentDebuggerFrame = nullptr;
                            } else {
                                std::cout << "Eval not available.\n";
                            }
                        } else if (line == "r" || line == "regs") {
                            int params = frame->function ? frame->function->maxArity : 0;
                            int locals = frame->function ? frame->function->localCount : 0;
                            int refs = frame->function ? frame->function->refCount : 0;
                            
                            std::cout << "Registers (Total: " << (locals + refs) << "):\n";
                            for (int i = 0; i < locals; ++i) {
                                std::string group = "Local";
                                if (i < params) group = "Param";
                                else if (i >= 128) group = "Spill";
                                
                                Value v = registers[frame->registerBase + i];
                                std::cout << "  [" << std::setw(5) << group << "] R(" << i << ") = " 
                                          << std::left << std::setw(15) << getTypeName(v) << " " << v << "\n";
                            }
                            for (int i = 0; i < refs; ++i) {
                                Value v = registers[frame->refParamsBase + i];
                                std::cout << "  [ Ref  ] R(" << (locals + i) << ") = " 
                                          << std::left << std::setw(15) << getTypeName(v) << " " << v << "\n";
                            }
                        } else if (line == "g" || line == "globals") {
                            for (const auto& [k, v] : globalNames) {
                                if (k.length() >= 2 && k[0] == '_' && k[1] == '_') continue;
                                std::cout << "  " << k << " = " << globals[v] << "\n";
                            }
                        } else if (line == "bt" || line == "backtrace") {
                            std::cout << buildStackTrace();
                        } else {
                            std::cout << "Unknown command. Type 'h' or 'help' for a list of commands.\n";
                        }
                    }
                }
                if (g_profile) frame->instructionCount++;
                int op_ip = ip; // ★ 记录当前指令的真实偏移量，用于 Profiling
                Instruction instruction = code[ip++];
                OpCode op = static_cast<OpCode>(instruction & 0xFF);
                
                int a = (instruction >> 8) & 0xFF;
                int b = (instruction >> 16) & 0xFF;
                int c = instruction >> 24;
                int bx = instruction >> 16;
                int sbx = bx - 0x7FFF;
                int sax = static_cast<int>(instruction >> 8) - 0x7FFFFF;
                int ax = instruction >> 8;

                switch (op) {
            case OpCode::MOVE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                getReg(a) = getReg(b);
                break;
            }
            case OpCode::LOADK: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                getReg(a) = chunk->constants.data()[bx];
                break;
            }
            case OpCode::LOAD_NIL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                getReg(a) = Value::none();
                break;
            }
            case OpCode::LOAD_BOOL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                getReg(a) = Value(b != 0);
                break;
            }
            case OpCode::GET_GLOBAL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[bx]);
                if (ic.cachedGlobalSlot >= 0) {
                    getReg(a) = globals.data()[ic.cachedGlobalSlot];
                } else if (ic.cachedGlobalSlot == -2) {
                    if (frame->classContext.isNone()) throw std::runtime_error("VM Error: 'class' accessed outside of context.");
                    getReg(a) = frame->classContext;
                } else {
                    const std::string& name = chunk->constants.data()[ic.nameIdx].asString();
                    if (name == "<class>") {
                        ic.cachedGlobalSlot = -2;
                        if (frame->classContext.isNone()) throw std::runtime_error("VM Error: 'class' accessed outside of context.");
                        getReg(a) = frame->classContext;
                        break;
                    }
                    auto it = globalNames.find(name);
                    if (it != globalNames.end()) {
                        ic.cachedGlobalSlot = it->second;
                        getReg(a) = globals.data()[it->second];
                    } else {
                        Value builtinVal = getBuiltinValue(name);
                        if (builtinVal.isNone()) builtinVal = getBuiltinClosure(name);
                        if (!builtinVal.isNone()) {
                            int newSlot = static_cast<int>(globals.size());
                            globalNames[name] = newSlot;
                            globals.push_back(builtinVal);
                            globalsDataPtr = globals.data();
                            clearAllGlobalICs();
                            ic.cachedGlobalSlot = newSlot;
                            getReg(a) = builtinVal;
                        } else {
                            if (name == "<namespace>") throw std::runtime_error("VM Error: 'namespace' accessed outside of context.");
                            throw std::runtime_error("VM Error: Undefined global variable '" + name + "'.");
                        }
                    }
                }
                break;
            }
            case OpCode::SET_GLOBAL:
            case OpCode::SET_GLOBAL_REF:
            case OpCode::DEFINE_CONST_GLOBAL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[bx]);
                Value val = getReg(a);

                if (ic.cachedGlobalSlot >= 0 && op == OpCode::SET_GLOBAL) {
                    globals.data()[ic.cachedGlobalSlot] = val;
                    break;
                }

                const std::string& name = chunk->constants.data()[ic.nameIdx].asString();
                if (name == "<class>") throw std::runtime_error("Syntax Error: cannot override context keyword 'class'.");
                if (name == "<namespace>") throw std::runtime_error("Syntax Error: cannot override context keyword 'namespace'.");
                
                if (constGlobals.count(name) && op != OpCode::DEFINE_CONST_GLOBAL) {
                    throw std::runtime_error("Runtime Error: Cannot modify const variable '" + name + "'.");
                }
                if (op == OpCode::DEFINE_CONST_GLOBAL && constGlobals.count(name)) {
                    throw std::runtime_error("Runtime Error: Cannot redefine const variable '" + name + "'.");
                }
                if (op == OpCode::SET_GLOBAL_REF) {
                    if (globalNames.find(name) == globalNames.end() && nativeBuiltins.find(name) == nativeBuiltins.end() && builtinValues.find(name) == builtinValues.end()) {
                        throw std::runtime_error("Runtime Error: Undefined variable '" + name + "'.");
                    }
                }

                if (ic.cachedGlobalSlot != -1) {
                    globals.data()[ic.cachedGlobalSlot] = val;
                } else {
                    auto it = globalNames.find(name);
                    if (it != globalNames.end()) {
                        ic.cachedGlobalSlot = it->second;
                        globals.data()[it->second] = val;
                    } else {
                        int newSlot = static_cast<int>(globals.size());
                        globalNames[name] = newSlot;
                        globals.push_back(val);
                        globalsDataPtr = globals.data();
                        clearAllGlobalICs();
                        ic.cachedGlobalSlot = newSlot;
                    }
                }
                
                if (op == OpCode::DEFINE_CONST_GLOBAL) {
                    constGlobals.insert(name);
                }
                break;
            }
            case OpCode::DELETE_GLOBAL: {
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                const std::string& name = chunk->constants.data()[bx].asString();
                if (name == "<class>") throw std::runtime_error("Syntax Error: cannot delete context keyword 'class'.");
                if (name == "<namespace>") throw std::runtime_error("Syntax Error: cannot delete context keyword 'namespace'.");
                if (constGlobals.count(name)) {
                    throw std::runtime_error("Runtime Error: Cannot delete const variable '" + name + "'.");
                }
                auto it = globalNames.find(name);
                if (it != globalNames.end()) {
                    globals[it->second] = Value::none();
                    globalNames.erase(it);
                    clearAllGlobalICs();
                } else {
                    throw std::runtime_error("VM Error: Undefined global variable '" + name + "'.");
                }
                break;
            }
            case OpCode::IS_UNINIT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                getReg(a) = Value(getReg(b).isUninit());
                break;
            }
            case OpCode::CLOSURE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                        
                int fnIdx = static_cast<int>(std::round(chunk->constants.data()[bx].asDouble()));
                if (fnIdx < 0 || fnIdx >= static_cast<int>(compiledFunctions.size()))
                    throw std::runtime_error("VM Error: Invalid function index.");

                auto& fn = compiledFunctions[fnIdx];
                auto closure = GcHeap::get().allocate<ObjClosure>(
                    std::vector<std::string>{}, std::vector<bool>{}, fn->name, nullptr
                );
                GcObjGuard closureGuard(closure);
                closure->compiledFnIndex = fnIdx;

                if (!fn->upvalues.empty()) {
                    closure->upvalueCount = static_cast<int>(fn->upvalues.size());
                    closure->upvalues = new ObjUpVal*[closure->upvalueCount];
                    for (int i = 0; i < closure->upvalueCount; ++i) closure->upvalues[i] = nullptr; // ★ 初始化为空指针，防止 GC 追踪野指针
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
                                    dummy->closed = Value::none();
                                    dummy->location = &dummy->closed;
                                    closure->upvalues[i] = dummy;
                                }
                            }
                        } else {
                            auto dummy = GcHeap::get().allocate<ObjUpVal>();
                            if (uv.isExplicitState) {
                                dummy->closed = Value::uninit();
                            } else if (uv.isLocal) {
                                if (uv.isRefParam) {
                                    dummy->closed = *(static_cast<ObjUpVal*>(getReg(frame->refParamsBase + uv.index).asObj())->location);
                                } else {
                                    dummy->closed = getReg(uv.index);
                                }
                            } else {
                                if (frame->closure && uv.index < frame->closure->upvalueCount) {
                                    dummy->closed = *(frame->closure->upvalues[uv.index]->location);
                                } else if (uv.isGlobal) {
                                    dummy->closed = getGlobalChecked(uv.name);
                                } else {
                                    dummy->closed = Value::none();
                                }
                            }
                            dummy->location = &dummy->closed;
                            closure->upvalues[i] = dummy;
                        }
                    }
                }
                        
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

                        if (!fnDef->restName.empty()) {
                            int fixedMax = fnDef->maxArity;
                            if (totalArgc < fnDef->arity) {
                                throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
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
                                throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
                            }
                            while (actualArgs.size() < static_cast<size_t>(fnDef->maxArity)) actualArgs.push_back(Value::uninit());
                        }

                        CallFrame newFrame;
                        newFrame.function = fnDef.get();
                        newFrame.chunk = &fnDef->chunk;
                        newFrame.ip = 0;
                        int newBase = vm->frames[vm->frameCount - 1].registerBase + vm->frames[vm->frameCount - 1].function->localCount + vm->frames[vm->frameCount - 1].function->refCount;
                        int newTotalCount = fnDef->localCount + fnDef->refCount;
                        PendingFrameGuard pfg(vm, newBase, newTotalCount);

                        newFrame.registerBase = newBase;
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
                        vm->profileFrameStart(&newFrame);
                        vm->frames[vm->frameCount++] = newFrame;
                        
                        int targetDepth = vm->frameCount - 1;
                        try {
                            return vm->run(targetDepth);
                        } catch (...) {
                            while (vm->frameCount > targetDepth) {
                                CallFrame* f = &vm->frames[vm->frameCount - 1];
                                vm->profileFrameEnd(f);
                                int clearBase = f->registerBase;
                                int clearCount = f->function->localCount + f->function->refCount;
                                for (int i = 0; i < clearCount; ++i) {
                                    vm->registers[clearBase + i] = Value::none();
                                }
                                f->selfContext = Value::none();
                                f->classContext = Value::none();
                                f->jitReturnSlot = Value::none();
                                f->closure = nullptr;
                                f->refParamsBase = -1;
                                vm->frameCount--;
                            }
                            throw;
                        }
                    }
                );

                closure->paramNames = fn->paramNames;
                closure->isRef = fn->paramIsRef;
                closure->isConst = fn->paramIsConst;
                int defaultLimit = fn->maxArity;
                for (int j = fn->arity; j < defaultLimit; ++j) {
                    closure->defaultValues.push_back(Value::uninit());
                }
                closure->restName = fn->restName;
                closure->kwargNames = fn->kwargNames;
                closure->kwargIsRef = fn->kwargIsRef;
                closure->kwargIsConst = fn->kwargIsConst;
                closure->kwargHasDefault = fn->kwargHasDefault;
                closure->kwargsName = fn->kwargsName;
                closure->boundSelf = frame->selfContext;
                closure->boundClass = frame->classContext;
                
                if (!fn->paramTypeRegs.empty()) {
                    closure->paramTypesCount = static_cast<int>(fn->paramTypeRegs.size());
                    closure->paramTypes = new Value[closure->paramTypesCount];
                    for (int i = 0; i < closure->paramTypesCount; ++i) {
                        int reg = fn->paramTypeRegs[i];
                        closure->paramTypes[i] = (reg != -1) ? getReg(reg) : Value::none();
                    }
                }
                if (fn->returnTypeReg != -1) {
                    closure->returnType = getReg(fn->returnTypeReg);
                }
                
                getReg(a) = Value(closure);
                break;
            }
            case OpCode::GET_UPVAL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (!frame->closure || b >= frame->closure->upvalueCount)
                    throw std::runtime_error("VM Error: Invalid upvalue index.");
                getReg(a) = *(frame->closure->upvalues[b]->location);
                break;
            }
            case OpCode::SET_UPVAL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (!frame->closure || b >= frame->closure->upvalueCount)
                    throw std::runtime_error("VM Error: Invalid upvalue index.");
                *(frame->closure->upvalues[b]->location) = getReg(a);
                break;
            }
            case OpCode::GET_REF_PARAM: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                if (frame->refParamsBase == -1) throw std::runtime_error("VM Error: Invalid ref param index.");
                getReg(a) = *(static_cast<ObjUpVal*>(registers[frame->refParamsBase + bx].asObj())->location);
                break;
            }
            case OpCode::SET_REF_PARAM: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                if (frame->refParamsBase == -1) throw std::runtime_error("VM Error: Invalid ref param index.");
                *(static_cast<ObjUpVal*>(registers[frame->refParamsBase + bx].asObj())->location) = getReg(a);
                break;
            }
            case OpCode::PASS_REFS: {
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                const auto& sig = chunk->callSignatures.data()[bx];
                pendingCallRefs.clear();
                for (const auto& ref : sig.refs) {
                    uint8_t argIndex = ref.argIndex;
                    uint8_t sourceType = ref.sourceType;
                    uint32_t sourceRef = ref.sourceRef;

                    ObjUpVal* upval = nullptr;
                    switch (sourceType) {
                        case 1: {
                            std::string name = chunk->constants.data()[sourceRef].asString();
                            upval = GcHeap::get().allocate<ObjUpVal>();
                            auto it = globalNames.find(name);
                            if (it == globalNames.end()) {
                                Value builtinVal = getBuiltinValue(name);
                                if (builtinVal.isNone()) builtinVal = getBuiltinClosure(name);
                                globalNames[name] = static_cast<uint32_t>(globals.size());
                                globals.push_back(builtinVal.isNone() ? Value::none() : builtinVal);
                                globalsDataPtr = globals.data();
                                clearAllGlobalICs();
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; // Monomorphic Int32
                    int64_t res = static_cast<int64_t>(vb.asInt32()) + vc.asInt32();
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value::fromInt32(static_cast<int32_t>(res)); break; }
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Overflow
                } else if (vb.isDouble() && vc.isDouble()) { 
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; // Monomorphic Double
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() + vc.asDoubleRaw()); break; 
                } else if (vb.isString() && vc.isString()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x04; // Monomorphic String
                    getReg(a) = Value(vb.asString() + vc.asString()); break;
                }
                if ((vb.isInt32() && vc.isDouble()) || (vb.isDouble() && vc.isInt32())) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x20; // Numeric Mixed (int↔double)
                } else {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; // Megamorphic / Other
                }
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_ADD); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RADD); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb + vc;
                break;
            }
            case OpCode::SUB: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; // Monomorphic Int32
                    int64_t res = static_cast<int64_t>(vb.asInt32()) - vc.asInt32();
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value::fromInt32(static_cast<int32_t>(res)); break; }
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Overflow
                } else if (vb.isDouble() && vc.isDouble()) { 
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; // Monomorphic Double
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() - vc.asDoubleRaw()); break; 
                }
                if ((vb.isInt32() && vc.isDouble()) || (vb.isDouble() && vc.isInt32())) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x20; // Numeric Mixed (int↔double)
                } else {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; // Megamorphic / Other
                }
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_SUB); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RSUB); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb - vc;
                break;
            }
            case OpCode::MUL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; // Monomorphic Int32
                    int64_t res = static_cast<int64_t>(vb.asInt32()) * vc.asInt32();
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value::fromInt32(static_cast<int32_t>(res)); break; }
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Overflow
                } else if (vb.isDouble() && vc.isDouble()) { 
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; // Monomorphic Double
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() * vc.asDoubleRaw()); break; 
                }
                if ((vb.isInt32() && vc.isDouble()) || (vb.isDouble() && vc.isInt32())) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x20; // Numeric Mixed (int↔double)
                } else {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; // Megamorphic / Other
                }
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_MUL); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RMUL); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb * vc;
                break;
            }
            case OpCode::DIV: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; // Monomorphic Int32
                    int32_t num = vb.asInt32();
                    int32_t den = vc.asInt32();
                    if (den == 0) throw std::runtime_error("Math Error: Division by zero.");
                    if (num % den == 0) {
                        if (num == -2147483648 && den == -1) {
                            const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Overflow
                            getReg(a) = Value(BigInt(2147483648LL));
                        } else {
                            getReg(a) = Value::fromInt32(num / den);
                        }
                    } else {
                        const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Fraction Output
                        getReg(a) = Value(Fraction(BigInt(num), BigInt(den)));
                    }
                    break;
                } else if (vb.isDouble() && vc.isDouble()) { 
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; // Monomorphic Double
                    if (vc.asDoubleRaw() == 0.0) throw std::runtime_error("Math Error: Division by zero.");
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() / vc.asDoubleRaw()); break; 
                }
                if ((vb.isInt32() && vc.isDouble()) || (vb.isDouble() && vc.isInt32())) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x20; // Numeric Mixed (int↔double)
                } else {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; // Megamorphic / Other
                }
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_DIV); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RDIV); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb / vc;
                break;
            }
            case OpCode::IDIV: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; // Monomorphic Int32
                    int32_t num = vb.asInt32();
                    int32_t den = vc.asInt32();
                    if (den == 0) throw std::runtime_error("Math Error: Division by zero.");
                    if (num == -2147483648 && den == -1) { 
                        const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Overflow
                        getReg(a) = Value(BigInt(2147483648LL)); 
                        break; 
                    }
                    getReg(a) = Value::fromInt32(num / den); break;
                } else if (vb.isDouble() && vc.isDouble()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; // Monomorphic Double
                    if (vc.asDoubleRaw() == 0.0) throw std::runtime_error("Math Error: Division by zero.");
                    getReg(a) = Value::fromDouble(std::trunc(vb.asDoubleRaw() / vc.asDoubleRaw())); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; // Megamorphic / Other
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_IDIV); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RIDIV); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = idivide(vb, vc);
                break;
            }
            case OpCode::MOD: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; // Monomorphic Int32
                    int32_t num = vb.asInt32();
                    int32_t den = vc.asInt32();
                    if (den == 0) throw std::runtime_error("Math Error: Modulo by zero.");
                    if (num == -2147483648 && den == -1) { getReg(a) = Value::fromInt32(0); break; }
                    getReg(a) = Value::fromInt32(num % den); break;
                } else if (vb.isDouble() && vc.isDouble()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; // Monomorphic Double
                    if (vc.asDoubleRaw() == 0.0) throw std::runtime_error("Math Error: Modulo by zero.");
                    getReg(a) = Value::fromDouble(std::fmod(vb.asDoubleRaw(), vc.asDoubleRaw())); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; // Megamorphic / Other
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_MOD); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RMOD); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb % vc;
                break;
            }
            case OpCode::POW: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_POW); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RPOW); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb ^ vc;
                break;
            }
            case OpCode::LDIV: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_LDIV); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RLDIV); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = ldivide(vb, vc);
                break;
            }
            case OpCode::BAND: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value::fromInt32(vb.asInt32() & vc.asInt32()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_BITAND); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RBITAND); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb & vc;
                break;
            }
            case OpCode::BOR: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value::fromInt32(vb.asInt32() | vc.asInt32()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_BITOR); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RBITOR); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb | vc;
                break;
            }
            case OpCode::BXOR: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value::fromInt32(vb.asInt32() ^ vc.asInt32()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_BITXOR); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RBITXOR); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = bitXor(vb, vc);
                break;
            }
            case OpCode::SHL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    int32_t v = vb.asInt32();
                    int32_t shift = vc.asInt32();
                    if (shift >= 0 && shift < 31) {
                        int64_t res = static_cast<int64_t>(v) << shift;
                        if (res >= INT32_MIN && res <= INT32_MAX) {
                            getReg(a) = Value::fromInt32(static_cast<int32_t>(res)); break;
                        }
                    }
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Overflow
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_LSHIFT); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RLSHIFT); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb << vc;
                break;
            }
            case OpCode::SHR: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    int32_t v = vb.asInt32();
                    int32_t shift = vc.asInt32();
                    if (shift >= 0) {
                        if (shift < 31) { getReg(a) = Value::fromInt32(v >> shift); break; }
                        else { getReg(a) = Value::fromInt32(v < 0 ? -1 : 0); break; }
                    }
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_RSHIFT); if (meth) { getReg(a) = callDunder(vb, meth, owner, {vc}); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_RRSHIFT); if (meth) { getReg(a) = callDunder(vc, meth, owner, {vb}); break; } }
                getReg(a) = vb >> vc;
                break;
            }
            case OpCode::UNM: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value vb = getReg(b);
                if (vb.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    int32_t v = vb.asInt32();
                    if (v == -2147483648) {
                        const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x10; // Overflow
                        getReg(a) = Value(BigInt(2147483648LL));
                    } else {
                        getReg(a) = Value::fromInt32(-v);
                    }
                    break;
                } else if (vb.isDouble()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                    getReg(a) = Value(-vb.asDoubleRaw()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_NEG); if (meth) { getReg(a) = callDunder(vb, meth, owner, {}); break; } }
                getReg(a) = -vb;
                break;
            }
            case OpCode::NOT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value& val = getReg(b);
                bool cond;
                if (val.isBool()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x08; cond = val.asBool(); }
                else if (val.isInt32()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; cond = val.asInt32() != 0; }
                else if (val.isDouble()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw()); }
                else { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy(); }
                getReg(a) = Value(!cond);
                break;
            }
            case OpCode::BNOT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value vb = getReg(b);
                if (vb.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value::fromInt32(~vb.asInt32()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_BITNOT); if (meth) { getReg(a) = callDunder(vb, meth, owner, {}); break; } }
                getReg(a) = ~vb;
                break;
            }
            case OpCode::TO_BOOL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value& val = getReg(b);
                bool cond;
                if (val.isBool()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x08; cond = val.asBool(); }
                else if (val.isInt32()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; cond = val.asInt32() != 0; }
                else if (val.isDouble()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw()); }
                else { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy(); }
                getReg(a) = Value(cond);
                break;
            }
            case OpCode::EQ: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                else if (vb.isDouble() && vc.isDouble()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                else if (vb.isString() && vc.isString()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x04;
                else const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                
                if (vb.as_bits == vc.as_bits) {
                    getReg(a) = Value(!vb.isDouble() || !std::isnan(vb.asDoubleRaw()));
                    break;
                }
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() == vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(false); break; }
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_EQ); if (meth) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, owner, {vc}))); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_EQ); if (meth) { getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, owner, {vb}))); break; } }
                getReg(a) = Value(Value::equals(vb, vc));
                break;
            }
            case OpCode::NEQ: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                else if (vb.isDouble() && vc.isDouble()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                else if (vb.isString() && vc.isString()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x04;
                else const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;

                if (vb.as_bits == vc.as_bits) {
                    getReg(a) = Value(vb.isDouble() && std::isnan(vb.asDoubleRaw()));
                    break;
                }
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() != vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(true); break; }
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_NEQ); if (meth) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, owner, {vc}))); break; } }
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_EQ); if (meth) { getReg(a) = Value(!evaluateTruthiness(callDunder(vb, meth, owner, {vc}))); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_NEQ); if (meth) { getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, owner, {vb}))); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_EQ); if (meth) { getReg(a) = Value(!evaluateTruthiness(callDunder(vc, meth, owner, {vb}))); break; } }
                getReg(a) = Value(!Value::equals(vb, vc));
                break;
            }
            case OpCode::LT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value(vb.asInt32() < vc.asInt32()); break;
                } else if (vb.isDouble() && vc.isDouble()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                    getReg(a) = Value(vb.asDoubleRaw() < vc.asDoubleRaw()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { auto [meth, owner] = findDunder(vb, DUNDER_LT); if (meth) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, owner, {vc}))); break; } }
                if (vc.isInstance()) { auto [meth, owner] = findDunder(vc, DUNDER_GT); if (meth) { getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, owner, {vb}))); break; } }
                getReg(a) = Value(vb < vc);
                break;
            }
            case OpCode::LE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value(vb.asInt32() <= vc.asInt32()); break;
                } else if (vb.isDouble() && vc.isDouble()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                    getReg(a) = Value(vb.asDoubleRaw() <= vc.asDoubleRaw()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { 
                    auto [meth, owner] = findDunder(vb, DUNDER_LE);
                    if (meth) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, owner, {vc}))); 
                        break; 
                    }
                    auto [methLt, ownerLt] = findDunder(vb, DUNDER_LT);
                    if (methLt) {
                        if (evaluateTruthiness(callDunder(vb, methLt, ownerLt, {vc}))) {
                            getReg(a) = Value(true);
                            break;
                        }
                        auto [methEq, ownerEq] = findDunder(vb, DUNDER_EQ);
                        if (methEq) {
                            getReg(a) = Value(evaluateTruthiness(callDunder(vb, methEq, ownerEq, {vc})));
                            break;
                        }
                    }
                }
                if (vc.isInstance()) { 
                    auto [meth, owner] = findDunder(vc, DUNDER_GE);
                    if (meth) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, owner, {vb}))); 
                        break; 
                    }
                    auto [methGt, ownerGt] = findDunder(vc, DUNDER_GT);
                    if (methGt) {
                        if (evaluateTruthiness(callDunder(vc, methGt, ownerGt, {vb}))) {
                            getReg(a) = Value(true);
                            break;
                        }
                        auto [methEq, ownerEq] = findDunder(vc, DUNDER_EQ);
                        if (methEq) {
                            getReg(a) = Value(evaluateTruthiness(callDunder(vc, methEq, ownerEq, {vb})));
                            break;
                        }
                    }
                }
                getReg(a) = Value(vb <= vc);
                break;
            }
            case OpCode::GT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value(vb.asInt32() > vc.asInt32()); break;
                } else if (vb.isDouble() && vc.isDouble()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                    getReg(a) = Value(vb.asDoubleRaw() > vc.asDoubleRaw()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { 
                    auto [meth, owner] = findDunder(vb, DUNDER_GT);
                    if (meth) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, owner, {vc}))); 
                        break; 
                    }
                    auto [methLt, ownerLt] = findDunder(vb, DUNDER_LT);
                    if (methLt) {
                        if (evaluateTruthiness(callDunder(vb, methLt, ownerLt, {vc}))) {
                            getReg(a) = Value(false);
                            break;
                        }
                        auto [methEq, ownerEq] = findDunder(vb, DUNDER_EQ);
                        if (methEq) {
                            getReg(a) = Value(!evaluateTruthiness(callDunder(vb, methEq, ownerEq, {vc})));
                            break;
                        }
                    }
                }
                if (vc.isInstance()) { 
                    auto [meth, owner] = findDunder(vc, DUNDER_LT);
                    if (meth) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, owner, {vb}))); 
                        break; 
                    }
                    auto [methGt, ownerGt] = findDunder(vc, DUNDER_GT);
                    if (methGt) {
                        if (evaluateTruthiness(callDunder(vc, methGt, ownerGt, {vb}))) {
                            getReg(a) = Value(false);
                            break;
                        }
                        auto [methEq, ownerEq] = findDunder(vc, DUNDER_EQ);
                        if (methEq) {
                            getReg(a) = Value(!evaluateTruthiness(callDunder(vc, methEq, ownerEq, {vb})));
                            break;
                        }
                    }
                }
                getReg(a) = Value(vb > vc);
                break;
            }
            case OpCode::GE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    getReg(a) = Value(vb.asInt32() >= vc.asInt32()); break;
                } else if (vb.isDouble() && vc.isDouble()) {
                    const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                    getReg(a) = Value(vb.asDoubleRaw() >= vc.asDoubleRaw()); break;
                }
                const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                if (vb.isInstance()) { 
                    auto [meth, owner] = findDunder(vb, DUNDER_GE);
                    if (meth) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, owner, {vc}))); 
                        break; 
                    }
                    auto [methLt, ownerLt] = findDunder(vb, DUNDER_LT);
                    if (methLt) {
                        getReg(a) = Value(!evaluateTruthiness(callDunder(vb, methLt, ownerLt, {vc})));
                        break;
                    }
                }
                if (vc.isInstance()) { 
                    auto [meth, owner] = findDunder(vc, DUNDER_LE);
                    if (meth) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, owner, {vb}))); 
                        break; 
                    }
                    auto [methGt, ownerGt] = findDunder(vc, DUNDER_GT);
                    if (methGt) {
                        getReg(a) = Value(!evaluateTruthiness(callDunder(vc, methGt, ownerGt, {vb})));
                        break;
                    }
                }
                getReg(a) = Value(vb >= vc);
                break;
            }
            case OpCode::IS: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                else if (vb.isDouble() && vc.isDouble()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                else if (vb.isString() && vc.isString()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x04;
                else const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                getReg(a) = Value(vb.as_bits == vc.as_bits);
                break;
            }
            case OpCode::JMP: {
                if (sax < 0) {
                    int loopHeaderIp = ip + sax;
                    if (loopHeaderIp >= 0 && loopHeaderIp < static_cast<int>(chunk->code.size())) {
                        ENSURE_OSR_LOOP_HEADERS();
                        if (chunk->osrLoopHeaderFlags[loopHeaderIp]) {
                            if (++const_cast<Chunk*>(chunk)->osrCounters[op_ip] >= 1000) {
                                if (g_enableJit && frame->closure && frame->closure->isBytecode()) {
                                    int fnIdx = frame->closure->compiledFnIndex;
                                    if (osrEntryPoints[fnIdx].find(loopHeaderIp) == osrEntryPoints[fnIdx].end()) {
                                        compileForOSR(fnIdx, loopHeaderIp);
                                    }
                                    bool osrTriggered = false;
                                    ATTEMPT_OSR(loopHeaderIp);
                                    if (osrTriggered) break;
                                }
                            }
                        }
                    }
                }
                ip += sax;
                break;
            }
            case OpCode::JMP_TRUE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value& val = getReg(a);
                bool cond;
                if (val.isBool()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x08; cond = val.asBool(); }
                else if (val.isInt32()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; cond = val.asInt32() != 0; }
                else if (val.isDouble()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw()); }
                else { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy(); }
                if (cond) {
                    if (sbx < 0) {
                        int loopHeaderIp = ip + sbx;
                        if (loopHeaderIp >= 0 && loopHeaderIp < static_cast<int>(chunk->code.size())) {
                            ENSURE_OSR_LOOP_HEADERS();
                            if (chunk->osrLoopHeaderFlags[loopHeaderIp]) {
                                if (++const_cast<Chunk*>(chunk)->osrCounters[op_ip] >= 1000) {
                                    if (g_enableJit && frame->closure && frame->closure->isBytecode()) {
                                        int fnIdx = frame->closure->compiledFnIndex;
                                        if (osrEntryPoints[fnIdx].find(loopHeaderIp) == osrEntryPoints[fnIdx].end()) {
                                            compileForOSR(fnIdx, loopHeaderIp);
                                        }
                                        bool osrTriggered = false;
                                        ATTEMPT_OSR(loopHeaderIp);
                                        if (osrTriggered) break;
                                    }
                                }
                            }
                        }
                    }
                    ip += sbx;
                }
                break;
            }
            case OpCode::JMP_FALSE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value& val = getReg(a);
                bool cond;
                if (val.isBool()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x08; cond = val.asBool(); }
                else if (val.isInt32()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01; cond = val.asInt32() != 0; }
                else if (val.isDouble()) { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02; cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw()); }
                else { const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80; cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy(); }
                if (!cond) {
                    if (sbx < 0) {
                        int loopHeaderIp = ip + sbx;
                        if (loopHeaderIp >= 0 && loopHeaderIp < static_cast<int>(chunk->code.size())) {
                            ENSURE_OSR_LOOP_HEADERS();
                            if (chunk->osrLoopHeaderFlags[loopHeaderIp]) {
                                if (++const_cast<Chunk*>(chunk)->osrCounters[op_ip] >= 1000) {
                                    if (g_enableJit && frame->closure && frame->closure->isBytecode()) {
                                        int fnIdx = frame->closure->compiledFnIndex;
                                        if (osrEntryPoints[fnIdx].find(loopHeaderIp) == osrEntryPoints[fnIdx].end()) {
                                            compileForOSR(fnIdx, loopHeaderIp);
                                        }
                                        bool osrTriggered = false;
                                        ATTEMPT_OSR(loopHeaderIp);
                                        if (osrTriggered) break;
                                    }
                                }
                            }
                        }
                    }
                    ip += sbx;
                }
                break;
            }
            case OpCode::BUILD_LIST: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                ObjList* list = GcHeap::get().allocate<ObjList>();
                getReg(a) = Value(list); // ★ 立即 Root 防止 GC 误杀
                list->vec.reserve(c);
                for (int i = 0; i < c; ++i) {
                    list->vec.push_back(getReg(b + i));
                }
                break;
            }
            case OpCode::BUILD_DICT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                const auto& shape = chunk->matrixShapes.data()[c];
                uint16_t rows = shape.rows;
                const std::vector<uint16_t>& rowCols = shape.rowCols;

                int total = 0;
                for (uint16_t cols : rowCols) total += cols;

                bool hasComplex = false;
                bool hasSymbolic = false;
                bool hasOther = false;

                auto canBeMatrixElement = [](const Value& v) -> bool {
                    return v.isNumber() || v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION) ||
                        v.isObjType(ObjType::COMPLEX) ||
                        v.isObjType(ObjType::SYMBOLIC) ||
                        v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || 
                        v.isObjType(ObjType::SYM_MATRIX);
                };

                for (int ii = 0; ii < total; ++ii) {
                    const Value& v = getReg(b + ii);
                    if (v.isObjType(ObjType::COMPLEX) || v.isObjType(ObjType::COMPLEX_MATRIX)) hasComplex = true;
                    if (v.isSymbolic() || v.isObjType(ObjType::SYM_MATRIX)) hasSymbolic = true;
                    if (!canBeMatrixElement(v)) {
                        hasOther = true;
                    } else if (v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION)) {
                        try { v.asDouble(); } catch (...) { 
                            if (!hasSymbolic) hasOther = true; // 如果有符号，大整数/分数可以直接转为符号，不算 Other
                        }
                    }
                }

                Value result;

                if (hasOther) {
                    throw std::runtime_error("VM Error: Matrix elements must be numeric, complex, or symbolic. Use @[...] for lists.");
                } else {
                    bool hasSubMatrix = false;
                    for (int ii = 0; ii < total; ++ii) {
                        const Value& v = getReg(b + ii);
                        if (v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || v.isObjType(ObjType::SYM_MATRIX)) hasSubMatrix = true;
                    }

                    if (hasSubMatrix) {
                        auto extractCell = [&](Value& cell) {
                            if (!cell.isObjType(ObjType::REAL_MATRIX) && !cell.isObjType(ObjType::COMPLEX_MATRIX) && !cell.isObjType(ObjType::SYM_MATRIX)) {
                                if (hasSymbolic) {
                                    cell = Value(SymMatrix(1, 1, { cell.asSymbolic() }));
                                } else if (hasComplex) {
                                    cell = Value(ComplexMatrix(1, 1, { cell.asComplex() }));
                                } else {
                                    cell = Value(RealMatrix(1, 1, { cell.asDouble() }));
                                }
                            }
                            if (hasSymbolic) {
                                if (cell.isObjType(ObjType::REAL_MATRIX) || cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                                    cell = Value(cell.asSymMatrix());
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
                                        if (hasSymbolic) rowResult = Value(static_cast<ObjSymMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjSymMatrix*>(cell.asObj())->mat));
                                        else if (hasComplex) rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                                        else rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                                    }
                                }
                                if (matResult.isNone()) {
                                    matResult = rowResult;
                                } else {
                                    if (hasSymbolic) matResult = Value(static_cast<ObjSymMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjSymMatrix*>(rowResult.asObj())->mat));
                                    else if (hasComplex) matResult = Value(static_cast<ObjComplexMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat));
                                    else matResult = Value(static_cast<ObjRealMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat));
                                }
                            }
                            result = matResult;
                        } catch (...) {
                            throw std::runtime_error("VM Error: Dimension mismatch during block matrix concatenation.");
                        }
                    } else {
                        int expectedCols = rows > 0 ? rowCols[0] : 0;
                        bool uniformCols = true;
                        for (int i = 1; i < rows; ++i) {
                            if (rowCols[i] != expectedCols) { uniformCols = false; break; }
                        }
                        if (!uniformCols) throw std::runtime_error("VM Error: Matrix rows must have the same number of columns.");

                        if (hasSymbolic) {
                            std::vector<SymExpr> flat(total);
                            for (int ii = 0; ii < total; ++ii) flat[ii] = getReg(b + ii).asSymbolic();
                            result = Value(SymMatrix(rows, expectedCols, flat));
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                getReg(a) = Value(GcHeap::get().allocate<ObjList>());
                break;
            }
            case OpCode::LIST_APPEND: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value& listVal = getReg(a);
                if (listVal.isObjType(ObjType::LIST)) {
                    static_cast<ObjList*>(listVal.asObj())->mut().push_back(getReg(b));
                } else {
                    throw std::runtime_error("VM Error: LIST_APPEND target is not a list.");
                }
                break;
            }
            case OpCode::SET_INIT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                getReg(a) = Value(GcHeap::get().allocate<ObjSet>());
                break;
            }
            case OpCode::SET_APPEND: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value& setVal = getReg(a);
                if (setVal.isObjType(ObjType::SET)) {
                    static_cast<ObjSet*>(setVal.asObj())->add(getReg(b));
                } else {
                    throw std::runtime_error("VM Error: SET_APPEND target is not a set.");
                }
                break;
            }
            case OpCode::DICT_INIT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                getReg(a) = Value(GcHeap::get().allocate<ObjDict>());
                break;
            }
            case OpCode::DICT_APPEND: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                Value& dictVal = getReg(a);
                if (dictVal.isObjType(ObjType::DICT)) {
                    static_cast<ObjDict*>(dictVal.asObj())->set(getReg(b), getReg(c));
                } else {
                    throw std::runtime_error("VM Error: DICT_APPEND target is not a dict.");
                }
                break;
            }
            case OpCode::STRINGIFY: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value v = getReg(b);
                if (v.isString()) {
                    getReg(a) = v;
                } else {
                    auto [d, owner] = findDunder(v, DUNDER_STR);
                    if (d) {
                        getReg(a) = callDunder(v, d, owner, {});
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
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
            case OpCode::SET_KW_ARGC: {
                pendingKwArgc = ax;
                break;
            }
            case OpCode::FORMAT_STRING: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                const std::string& spec = chunk->constants.data()[c].asString();
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                bool noThrow = (c & 0x80) != 0;
                int dims = c & 0x7F;
                
                Value obj = getReg(b);
                std::vector<Value> args;
                args.reserve(dims);
                for (int i = 0; i < dims; ++i) {
                    args.push_back(getReg(b + 1 + i));
                }

                Value result;

                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    auto [getitemMethod, owner] = findDunder(obj, DUNDER_GETITEM);
                    if (getitemMethod) {
                        try {
                            result = callDunder(obj, getitemMethod, owner, args);
                        } catch (...) {
                            if (noThrow) result = Value::uninit();
                            else throw;
                        }
                        getReg(a) = result;
                        break;
                    }
                    if (dims == 1 && args[0].isString()) {
                        std::string keyStr = args[0].asString();
                        if (isReservedInternalName(keyStr)) {
                            if (noThrow) result = Value::uninit();
                            else throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
                            getReg(a) = result;
                            break;
                        }
                        ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                        bool foundPrivate = false;
                        if (ctxOwner) {
                            std::string mangledName = manglePrivate(ctxOwner->classId, keyStr);
                            auto it = inst->properties.find(mangledName);
                            if (it != inst->properties.end()) {
                                result = it->second.val;
                                foundPrivate = true;
                            }
                        }
                        if (!foundPrivate) {
                            auto it = inst->properties.find(keyStr);
                            if (it != inst->properties.end() && !it->second.is_local) {
                                result = it->second.val;
                            } else {
                                if (noThrow) result = Value::uninit();
                                else throw std::runtime_error("VM Error: Property '" + keyStr + "' not found.");
                            }
                        }
                        getReg(a) = result;
                        break;
                    } else {
                        if (noThrow) { getReg(a) = Value::uninit(); break; }
                        throw std::runtime_error("TypeError: Instance does not support this indexing. Implement __getitem__.");
                    }
                }

                if (dims == 1) {
                    Value idx = args[0];
                    if (obj.isObjType(ObjType::LIST)) {
                        auto list = static_cast<ObjList*>(obj.asObj());
                        auto range = idx.parseIndex(static_cast<int>(list->vec.size()), noThrow);
                        if (!range.isSlice && range.scalarIdx == -1) result = Value::uninit();
                        else if (!range.isSlice) result = list->vec[range.scalarIdx];
                        else {
                            ObjList* resList = GcHeap::get().allocate<ObjList>();
                            resList->vec.reserve(range.sliceInfo.count);
                            for (int i = 0; i < range.sliceInfo.count; ++i) {
                                resList->vec.push_back(list->vec[range.sliceInfo.start + i * range.sliceInfo.step]);
                            }
                            result = Value(resList);
                        }
                    } else if (obj.isString()) {
                        ObjString* objStr = obj.asObjString();
                        auto range = idx.parseIndex(static_cast<int>(objStr->charLength), noThrow);
                        if (!range.isSlice && range.scalarIdx == -1) result = Value::uninit();
                        else if (!range.isSlice) {
                            if (objStr->isAscii) {
                                char c_str[2] = { objStr->str[range.scalarIdx], '\0' };
                                result = Value(c_str);
                            } else {
                                result = Value(utf8::substring(objStr->str, range.scalarIdx, 1, objStr->isAscii));
                            }
                        } else {
                            std::string resStr;
                            if (objStr->isAscii) {
                                resStr.reserve(range.sliceInfo.count);
                                for (int i = 0; i < range.sliceInfo.count; ++i) {
                                    resStr += objStr->str[range.sliceInfo.start + i * range.sliceInfo.step];
                                }
                            } else {
                                for (int i = 0; i < range.sliceInfo.count; ++i) {
                                    resStr += utf8::substring(objStr->str, range.sliceInfo.start + i * range.sliceInfo.step, 1, false);
                                }
                            }
                            result = Value(resStr);
                        }
                    } else if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
                        auto processMatGet = [&](const auto& m) -> Value {
                            using MatType = std::decay_t<decltype(m)>;
                            int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                            auto range = idx.parseIndex(n, noThrow);
                            if (!range.isSlice && range.scalarIdx == -1) return Value::uninit();
                            
                            if (!range.isSlice) {
                                if (m.getRows() == 1) return Value(m(0, range.scalarIdx));
                                else if (m.getCols() == 1) return Value(m(range.scalarIdx, 0));
                                else {
                                    using ElemType = std::decay_t<decltype(m(0,0))>;
                                    std::vector<ElemType> row(m.getCols());
                                    for (int j = 0; j < m.getCols(); ++j) row[j] = m(range.scalarIdx, j);
                                    return Value(MatType(1, m.getCols(), row));
                                }
                            } else {
                                // 切片：返回视图（Matrix<T> 零拷贝，SymMatrix 拷贝）
                                if (m.getRows() == 1) {
                                    return Value(m.view(0, 1, 1, range.sliceInfo.start, range.sliceInfo.step, range.sliceInfo.count));
                                } else if (m.getCols() == 1) {
                                    return Value(m.view(range.sliceInfo.start, range.sliceInfo.step, range.sliceInfo.count, 0, 1, 1));
                                } else {
                                    return Value(m.view(range.sliceInfo.start, range.sliceInfo.step, range.sliceInfo.count, 0, 1, m.getCols()));
                                }
                            }
                        };
                        try {
                            if (obj.isObjType(ObjType::REAL_MATRIX)) result = processMatGet(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
                            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) result = processMatGet(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
                            else result = processMatGet(static_cast<ObjSymMatrix*>(obj.asObj())->mat);
                        } catch (...) {
                            throw;
                        }
                    } else if (obj.isObjType(ObjType::DICT)) {
                        if (idx.isSlice()) throw std::runtime_error("TypeError: Dict does not support slice indexing.");
                        auto dict = static_cast<ObjDict*>(obj.asObj());
                        auto it = dict->keyMap.find(idx);
                        if (it == dict->keyMap.end()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::runtime_error("VM Error: Key not found.");
                        } else {
                            result = dict->elements[it->second].second;
                        }
                    } else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        if (!idx.isString()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::runtime_error("VM Error: Namespace keys must be strings.");
                        } else {
                            std::string key = idx.asString();
                            auto it = ns->fields.find(key);
                            if (it == ns->fields.end()) {
                                if (noThrow) result = Value::uninit();
                                else throw std::runtime_error("VM Error: Key not found in namespace.");
                            } else {
                                result = *(it->second.upval->location);
                            }
                        }
                    } else if (obj.isClass()) {
                        auto cls = static_cast<ObjClass*>(obj.asObj());
                        if (!idx.isString()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::runtime_error("VM Error: Class static field keys must be strings.");
                        } else {
                            std::string key = idx.asString();
                            if (isReservedInternalName(key)) {
                                if (noThrow) result = Value::uninit();
                                else throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
                            } else {
                                bool foundStatic = false;
                                ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                                if (ctxOwner) {
                                    std::string mangledName = manglePrivate(ctxOwner->classId, key);
                                    auto it = ctxOwner->properties.find(mangledName);
                                    if (it != ctxOwner->properties.end()) {
                                        if (it->second.val.isFunctionClosure()) {
                                            auto rawMethod = it->second.val.asFunction();
                                            auto bound = GcHeap::get().allocate<ObjClosure>(
                                                std::vector<std::string>{}, std::vector<bool>{}, key, nullptr
                                            );
                                            bound->paramNames = rawMethod->paramNames;
                                            bound->isRef = rawMethod->isRef;
                                            bound->defaultValues = rawMethod->defaultValues;
                                            bound->restName = rawMethod->restName;
                                            bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                            if (rawMethod->upvalueCount > 0) {
                                                bound->upvalueCount = rawMethod->upvalueCount;
                                                bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                                for (int i = 0; i < bound->upvalueCount; ++i) {
                                                    bound->upvalues[i] = rawMethod->upvalues[i];
                                                }
                                            }
                                            if (rawMethod->paramTypesCount > 0) {
                                                bound->paramTypesCount = rawMethod->paramTypesCount;
                                                bound->paramTypes = new Value[bound->paramTypesCount];
                                                for (int i = 0; i < bound->paramTypesCount; ++i) {
                                                    bound->paramTypes[i] = rawMethod->paramTypes[i];
                                                }
                                            }
                                            bound->returnType = rawMethod->returnType;
                                            bound->nativeFn = rawMethod->nativeFn;
                                            bound->boundSelf = Value::none();
                                            bound->boundClass = Value(ctxOwner);
                                            bound->is_local = true;
                                            result = Value(bound);
                                        } else {
                                            result = it->second.val;
                                        }
                                        foundStatic = true;
                                    }
                                }
                                if (!foundStatic) {
                                    auto c_cls = cls;
                                    while (c_cls) {
                                        auto it = c_cls->properties.find(key);
                                        if (it != c_cls->properties.end() && !it->second.is_local) {
                                            if (it->second.val.isFunctionClosure()) {
                                                auto rawMethod = it->second.val.asFunction();
                                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                                    std::vector<std::string>{}, std::vector<bool>{}, key, nullptr
                                                );
                                                bound->paramNames = rawMethod->paramNames;
                                                bound->isRef = rawMethod->isRef;
                                                bound->defaultValues = rawMethod->defaultValues;
                                                bound->restName = rawMethod->restName;
                                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                                if (rawMethod->upvalueCount > 0) {
                                                    bound->upvalueCount = rawMethod->upvalueCount;
                                                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                                    for (int i = 0; i < bound->upvalueCount; ++i) {
                                                        bound->upvalues[i] = rawMethod->upvalues[i];
                                                    }
                                                }
                                                if (rawMethod->paramTypesCount > 0) {
                                                    bound->paramTypesCount = rawMethod->paramTypesCount;
                                                    bound->paramTypes = new Value[bound->paramTypesCount];
                                                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                                                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                                                    }
                                                }
                                                bound->returnType = rawMethod->returnType;
                                                bound->nativeFn = rawMethod->nativeFn;
                                                bound->boundSelf = Value::none();
                                                bound->boundClass = Value(c_cls);
                                                result = Value(bound);
                                            } else {
                                                result = it->second.val;
                                            }
                                            foundStatic = true;
                                            break;
                                        }
                                        c_cls = c_cls->parent;
                                    }
                                }
                                if (!foundStatic) {
                                    if (noThrow) result = Value::uninit();
                                    else throw std::runtime_error("VM Error: Static field not found in class.");
                                }
                            }
                        }
                    } else if (obj.isSlice()) {
                        if (!idx.isString()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::runtime_error("VM Error: Slice properties must be accessed with string keys.");
                        } else {
                            Value prop = obj.asSlice()->getProperty(idx.asString());
                            if (prop.isUninit()) {
                                if (noThrow) result = Value::uninit();
                                else throw std::runtime_error("VM Error: Property '" + idx.asString() + "' not found on slice.");
                            } else {
                                result = prop;
                            }
                        }
                    } else {
                        if (noThrow) result = Value::uninit();
                        else throw std::runtime_error("VM Error: Unsupported 1D index get.");
                    }
                } else if (dims == 2) {
                    Value rowIdx = args[0];
                    Value colIdx = args[1];
                    if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
                        auto processMatGet2D = [&](const auto& m) -> Value {
                            auto rRange = rowIdx.parseIndex(m.getRows(), noThrow);
                            auto cRange = colIdx.parseIndex(m.getCols(), noThrow);
                            
                            if ((!rRange.isSlice && rRange.scalarIdx == -1) || (!cRange.isSlice && cRange.scalarIdx == -1)) return Value::uninit();
                            
                            if (!rRange.isSlice && !cRange.isSlice) {
                                return Value(m(rRange.scalarIdx, cRange.scalarIdx));
                            } else if (!rRange.isSlice && cRange.isSlice) {
                                return Value(m.view(rRange.scalarIdx, 1, 1, cRange.sliceInfo.start, cRange.sliceInfo.step, cRange.sliceInfo.count));
                            } else if (rRange.isSlice && !cRange.isSlice) {
                                return Value(m.view(rRange.sliceInfo.start, rRange.sliceInfo.step, rRange.sliceInfo.count, cRange.scalarIdx, 1, 1));
                            } else {
                                return Value(m.view(rRange.sliceInfo.start, rRange.sliceInfo.step, rRange.sliceInfo.count, cRange.sliceInfo.start, cRange.sliceInfo.step, cRange.sliceInfo.count));
                            }
                        };
                        try {
                            if (obj.isObjType(ObjType::REAL_MATRIX)) result = processMatGet2D(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
                            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) result = processMatGet2D(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
                            else result = processMatGet2D(static_cast<ObjSymMatrix*>(obj.asObj())->mat);
                        } catch (...) {
                            throw;
                        }
                    } else {
                        if (noThrow) result = Value::uninit();
                        else throw std::runtime_error("VM Error: Unsupported 2D index get.");
                    }
                } else {
                    throw std::runtime_error("VM Error: Unsupported index dimensionality.");
                }
                getReg(a) = result;
                break;
            }
            case OpCode::INDEX_SET: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                int dims = c;
                Value obj = getReg(a);
                Value val = getReg(a + c + 1);
                
                std::vector<Value> args;
                args.reserve(dims);
                for (int i = 0; i < dims; ++i) {
                    args.push_back(getReg(a + 1 + i));
                }

                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    inst->checkModify();
                    auto [setitemMethod, owner] = findDunder(obj, DUNDER_SETITEM);
                    if (setitemMethod) {
                        args.push_back(val);
                        callDunder(obj, setitemMethod, owner, args);
                        break;
                    }
                    if (dims == 1 && args[0].isString()) {
                        std::string keyStr = args[0].asString();
                        if (isReservedInternalName(keyStr)) {
                            throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
                        }
                        ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                        bool foundPrivate = false;
                        if (ctxOwner) {
                            std::string mangledName = manglePrivate(ctxOwner->classId, keyStr);
                            auto it = inst->properties.find(mangledName);
                            if (it != inst->properties.end()) {
                                if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const private property '" + keyStr + "'.");
                                invalidateJITOnContainerReplace(it->second.val, val);
                                it->second.val = val;
                                foundPrivate = true;
                            }
                        }
                        if (!foundPrivate) {
                            Value oldVal = Value::none();
                            auto oldIt = inst->properties.find(keyStr);
                            if (oldIt != inst->properties.end()) oldVal = oldIt->second.val;
                            invalidateJITOnContainerReplace(oldVal, val);
                            inst->setProperty(keyStr, val);
                        }
                        break;
                    } else {
                        throw std::runtime_error("TypeError: Instance does not support this indexing. Implement __setitem__.");
                    }
                }

                if (dims == 1) {
                    Value idx = args[0];
                    if (obj.isObjType(ObjType::LIST)) {
                        auto list = static_cast<ObjList*>(obj.asObj());
                        auto range = idx.parseIndex(static_cast<int>(list->vec.size()), false);
                        if (!range.isSlice) {
                            invalidateJITOnContainerReplace(list->vec[range.scalarIdx], val);
                            list->mut()[range.scalarIdx] = val;
                        } else {
                            if (val.isObjType(ObjType::LIST)) {
                                const auto& srcL = static_cast<ObjList*>(val.asObj())->vec;
                                if (static_cast<int>(srcL.size()) != range.sliceInfo.count) throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                                for (int k = 0; k < range.sliceInfo.count; ++k) list->mut()[range.sliceInfo.start + k * range.sliceInfo.step] = srcL[k];
                            } else {
                                for (int i = 0; i < range.sliceInfo.count; ++i) list->mut()[range.sliceInfo.start + i * range.sliceInfo.step] = val;
                            }
                        }
                    } else if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
                        throw std::runtime_error("Runtime Error: Matrices are immutable. Use setItem(i, x) / setSlice(sr, sc, x) to get a new matrix.");
                    } else if (obj.isObjType(ObjType::DICT)) {
                        if (idx.isSlice()) throw std::runtime_error("TypeError: Dict does not support slice indexing.");
                        auto dict = static_cast<ObjDict*>(obj.asObj());
                        Value oldVal = Value::none();
                        auto dit = dict->keyMap.find(idx);
                        if (dit != dict->keyMap.end()) oldVal = dict->elements[dit->second].second;
                        invalidateJITOnContainerReplace(oldVal, val);
                        dict->set(idx, val);
                    } else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        if (!idx.isString()) throw std::runtime_error("VM Error: Namespace keys must be strings.");
                        std::string key = idx.asString();
                        Value oldVal = Value::none();
                        auto nsIt = ns->fields.find(key);
                        if (nsIt != ns->fields.end()) oldVal = *(nsIt->second.upval->location);
                        invalidateJITOnContainerReplace(oldVal, val);
                        ns->setField(key, val);
                    } else if (obj.isClass()) {
                        auto cls = static_cast<ObjClass*>(obj.asObj());
                        if (!idx.isString()) throw std::runtime_error("VM Error: Class static field keys must be strings.");
                        std::string key = idx.asString();
                        if (isReservedInternalName(key)) {
                            throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
                        }
                        
                        bool found = false;
                        ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                        if (ctxOwner) {
                            std::string mangledName = manglePrivate(ctxOwner->classId, key);
                            auto it = ctxOwner->properties.find(mangledName);
                            if (it != ctxOwner->properties.end()) {
                                if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const private static property '" + key + "'.");
                                invalidateJITOnContainerReplace(it->second.val, val);
                                it->second.val = val;
                                found = true;
                            }
                        }
                        if (!found) {
                            auto c_cls = cls;
                            while (c_cls) {
                                auto it = c_cls->properties.find(key);
                                if (it != c_cls->properties.end()) {
                                    if (it->second.is_local) {
                                        if (c_cls == cls) throw std::runtime_error("VM Error: Cannot modify private static property '" + key + "'.");
                                        break;
                                    }
                                    if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const static property '" + key + "'.");
                                    invalidateJITOnContainerReplace(it->second.val, val);
                                    it->second.val = val;
                                    found = true;
                                    break;
                                }
                                c_cls = c_cls->parent;
                            }
                        }
                        if (!found) {
                            invalidateJITOnContainerReplace(Value::none(), val);
                            if (cls) cls->properties[key] = { val, false, false };
                        }
                    } else {
                        throw std::runtime_error("VM Error: Unsupported 1D index set.");
                    }
                } else if (dims == 2) {
                    Value rowIdx = args[0];
                    Value colIdx = args[1];
                    if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
                        throw std::runtime_error("Runtime Error: Matrices are immutable. Use setElement(r, c, x) / setSlice(sr, sc, x) to get a new matrix.");
                    } else {
                        throw std::runtime_error("VM Error: Unsupported 2D index set.");
                    }
                } else {
                    throw std::runtime_error("VM Error: Unsupported index dimensionality.");
                }
                break;
            }
            case OpCode::ITER_INIT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                uint8_t destructFlag = static_cast<uint8_t>(c);
                Value iterable = getReg(b);
                
                if (iterable.isInstance()) {
                    auto [method, owner] = findDunder(iterable, DUNDER_ITER);
                    if (method) {
                        Value iterObj = callDunder(iterable, method, owner, {});
                        GcValueGuard iterGuard(iterObj);
                        ObjList* state = GcHeap::get().allocate<ObjList>();
                        state->vec.push_back(iterObj);
                        if (iterObj.isInstance() && iterObj.asInstance()->c_nativeNext) {
                            state->vec.push_back(Value::none());
                        } else {
                            auto [nextMethod, nextOwner] = findDunder(iterObj, DUNDER_NEXT);
                            if (!nextMethod) throw std::runtime_error("VM Error: Iterator missing __next__ method.");
                            state->vec.push_back(Value(nextMethod));
                            state->vec.push_back(Value(nextOwner));
                        }
                        getReg(a) = Value(state);
                        break;
                    }
                }
                
                if (iterable.isObjType(ObjType::LIST) || iterable.isString() || 
                    iterable.isObjType(ObjType::REAL_MATRIX) || iterable.isObjType(ObjType::COMPLEX_MATRIX) || 
                    iterable.isObjType(ObjType::SYM_MATRIX)) {
                    ObjList* state = GcHeap::get().allocate<ObjList>();
                    state->vec.push_back(iterable);
                    state->vec.push_back(Value::fromInt32(0));
                    getReg(a) = Value(state);
                    break;
                }
                
                ObjList* elements = GcHeap::get().allocate<ObjList>();
                getReg(a) = Value(elements); // ★ 立即 Root 防止 GC 误杀
                
                if (iterable.isObjType(ObjType::DICT)) {
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
                } else if (iterable.isObjType(ObjType::NAMESPACE)) {
                    const auto* ns = static_cast<ObjNamespace*>(iterable.asObj());
                    if (destructFlag) {
                        for (const auto& [key, field] : ns->fields) {
                            ObjList* pair = GcHeap::get().allocate<ObjList>();
                            pair->vec.push_back(Value(key));
                            pair->vec.push_back(*(field.upval->location));
                            pair->is_frozen = true;
                            elements->vec.push_back(Value(pair));
                        }
                    } else {
                        for (const auto& [key, field] : ns->fields) {
                            elements->vec.push_back(Value(key));
                        }
                    }
                } else if (iterable.isClass()) {
                    const auto* cls = static_cast<ObjClass*>(iterable.asObj());
                    std::unordered_set<std::string> seen;
                    while (cls) {
                        if (destructFlag) {
                            for (const auto& [key, prop] : cls->properties) {
                                if (prop.is_local || seen.count(key)) continue;
                                if (isReservedInternalName(key)) continue;
                                seen.insert(key);
                                ObjList* pair = GcHeap::get().allocate<ObjList>();
                                pair->vec.push_back(Value(key));
                                pair->vec.push_back(prop.val);
                                pair->is_frozen = true;
                                elements->vec.push_back(Value(pair));
                            }
                        } else {
                            for (const auto& [key, prop] : cls->properties) {
                                if (prop.is_local || seen.count(key)) continue;
                                if (isReservedInternalName(key)) continue;
                                seen.insert(key);
                                elements->vec.push_back(Value(key));
                            }
                        }
                        cls = cls->parent;
                    }
                } else if (iterable.isObjType(ObjType::SET)) {
                    const auto* s = static_cast<ObjSet*>(iterable.asObj());
                    for (const auto& val : s->elements) {
                        elements->vec.push_back(val);
                    }
                } else if (iterable.isInstance()) {
                    auto inst = iterable.asInstance();
                    if (destructFlag) {
                        for (const auto& [key, prop] : inst->properties) {
                            if (prop.is_local) continue;
                            if (isReservedInternalName(key)) continue;
                            ObjList* pair = GcHeap::get().allocate<ObjList>();
                            pair->vec.push_back(Value(key));
                            pair->vec.push_back(prop.val);
                            pair->is_frozen = true;
                            elements->vec.push_back(Value(pair));
                        }
                    } else {
                        for (const auto& [key, prop] : inst->properties) {
                            if (prop.is_local) continue;
                            if (isReservedInternalName(key)) continue;
                            elements->vec.push_back(Value(key));
                        }
                    }
                } else {
                    throw std::runtime_error("VM Error: Cannot iterate over this type.");
                }
                
                ObjList* state = GcHeap::get().allocate<ObjList>();
                state->vec.push_back(Value(elements));
                state->vec.push_back(Value::fromInt32(0));
                getReg(a) = Value(state);
                break;
            }
            case OpCode::ITER_NEXT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                
                Value& stateVal = getReg(b);
                
                auto state = static_cast<ObjList*>(stateVal.asObj());
                if (state->vec.size() >= 2 && state->vec[1].isInt32()) {
                    Value iterTarget = state->vec[0];
                    int i = state->vec[1].asInt32();
                
                    if (iterTarget.isObjType(ObjType::LIST)) {
                            const auto& elems = static_cast<ObjList*>(iterTarget.asObj())->vec;
                            if (i >= static_cast<int>(elems.size())) {
                                getReg(a) = Value::uninit();
                            } else {
                                getReg(a) = elems[i];
                                state->vec[1] = Value::fromInt32(i + 1);
                            }
                            break;
                        } else if (iterTarget.isString()) {
                            ObjString* objStr = iterTarget.asObjString();
                            if (i >= static_cast<int>(objStr->charLength)) {
                                getReg(a) = Value::uninit();
                            } else {
                                if (objStr->isAscii) {
                                    getReg(a) = Value(std::string(1, objStr->str[i]));
                                    state->vec[1] = Value::fromInt32(i + 1);
                                } else {
                                    int byteOffset = state->vec.size() > 2 ? state->vec[2].asInt32() : 0;
                                    int charLen = 1;
                                    unsigned char ch = objStr->str[byteOffset];
                                    if ((ch & 0xE0) == 0xC0) charLen = 2;
                                    else if ((ch & 0xF0) == 0xE0) charLen = 3;
                                    else if ((ch & 0xF8) == 0xF0) charLen = 4;
                                
                                    getReg(a) = Value(objStr->str.substr(byteOffset, charLen));
                                    state->vec[1] = Value::fromInt32(i + 1);
                                    if (state->vec.size() > 2) state->vec[2] = Value::fromInt32(byteOffset + charLen);
                                    else state->vec.push_back(Value::fromInt32(byteOffset + charLen));
                                }
                            }
                            break;
                        } else if (iterTarget.isObjType(ObjType::REAL_MATRIX)) {
                            const auto& m = static_cast<ObjRealMatrix*>(iterTarget.asObj())->mat;
                            int len = (m.getRows() == 1) ? m.getCols() : m.getRows();
                            if (i >= len) {
                                getReg(a) = Value::uninit();
                            } else {
                                if (m.getRows() == 1) getReg(a) = Value(m(0, i));
                                else if (m.getCols() == 1) getReg(a) = Value(m(i, 0));
                                else {
                                    std::vector<double> row(m.getCols());
                                    for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                                    getReg(a) = Value(RealMatrix(1, m.getCols(), row));
                                }
                                state->vec[1] = Value::fromInt32(i + 1);
                            }
                            break;
                        } else if (iterTarget.isObjType(ObjType::COMPLEX_MATRIX)) {
                            const auto& m = static_cast<ObjComplexMatrix*>(iterTarget.asObj())->mat;
                            int len = (m.getRows() == 1) ? m.getCols() : m.getRows();
                            if (i >= len) {
                                getReg(a) = Value::uninit();
                            } else {
                                if (m.getRows() == 1) getReg(a) = Value(m(0, i));
                                else if (m.getCols() == 1) getReg(a) = Value(m(i, 0));
                                else {
                                    std::vector<Complex> row(m.getCols());
                                    for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                                    getReg(a) = Value(ComplexMatrix(1, m.getCols(), row));
                                }
                                state->vec[1] = Value::fromInt32(i + 1);
                            }
                            break;
                        } else if (iterTarget.isObjType(ObjType::SYM_MATRIX)) {
                            const auto& m = static_cast<ObjSymMatrix*>(iterTarget.asObj())->mat;
                            int len = (m.getRows() == 1) ? m.getCols() : m.getRows();
                            if (i >= len) {
                                getReg(a) = Value::uninit();
                            } else {
                                if (m.getRows() == 1) getReg(a) = Value(m(0, i));
                                else if (m.getCols() == 1) getReg(a) = Value(m(i, 0));
                                else {
                                    std::vector<SymExpr> row(m.getCols());
                                    for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                                    getReg(a) = Value(SymMatrix(1, m.getCols(), row));
                                }
                                state->vec[1] = Value::fromInt32(i + 1);
                            }
                            break;
                        }
                    }
                
                Value iterObj = state->vec[0];
                if (iterObj.isInstance() && iterObj.asInstance()->c_nativeNext) {
                    getReg(a) = iterObj.asInstance()->c_nativeNext(iterObj.asInstance());
                } else {
                    ObjClosure* method = state->vec[1].asFunction();
                    ObjClass* owner = state->vec.size() > 2 ? static_cast<ObjClass*>(state->vec[2].asObj()) : nullptr;
                    Value nextVal = callDunder(iterObj, method, owner, {});
                    if (nextVal.isNone()) {
                        getReg(a) = Value::uninit();
                    } else {
                        getReg(a) = nextVal;
                    }
                }
                break;
            }
            case OpCode::IN: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                Value needle = getReg(b);
                Value haystack = getReg(c);
                bool found = false;
                
                if (needle.isString() && haystack.isString()) {
                    found = haystack.asString().find(needle.asString()) != std::string::npos;
                } else if (haystack.isObjType(ObjType::LIST)) {
                    const auto& L = static_cast<ObjList*>(haystack.asObj())->vec;
                    for (const auto& e : L) {
                        try {
                            bool eq = (needle.isString() && e.isString()) ? (needle.asString() == e.asString()) : Value::equals(needle, e);
                            if (eq) {
                                found = true;
                                break;
                            }
                        } catch (...) {}
                    }
                } else if (haystack.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(haystack.asObj());
                    found = d->keyMap.find(needle) != d->keyMap.end();
                } else if (haystack.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(haystack.asObj());
                    if (needle.isString()) {
                        found = ns->fields.find(needle.asString()) != ns->fields.end();
                    }
                } else if (haystack.isClass()) {
                    auto cls = static_cast<ObjClass*>(haystack.asObj());
                    if (needle.isString()) {
                        std::string key = needle.asString();
                        ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                        if (ctxOwner) {
                            auto it = ctxOwner->properties.find(key);
                            if (it != ctxOwner->properties.end() && it->second.is_local) {
                                found = true;
                            }
                        }
                        if (!found) {
                            while (cls) {
                                auto it = cls->properties.find(key);
                                if (it != cls->properties.end() && !it->second.is_local) {
                                    found = true;
                                    break;
                                }
                                cls = cls->parent;
                            }
                        }
                    }
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
                } else if (haystack.isObjType(ObjType::SYM_MATRIX)) {
                    const auto& m = static_cast<ObjSymMatrix*>(haystack.asObj())->mat;
                    if (needle.isSymbolic() || needle.isNumber() || needle.isBigInt() || needle.isObjType(ObjType::FRACTION) || needle.isComplex()) {
                        SymExpr nv = needle.asSymbolic();
                        for (int i = 0; i < m.getRows(); ++i) {
                            for (int j = 0; j < m.getCols(); ++j) {
                                if (m(i, j) == nv) { found = true; break; }
                            }
                            if (found) break;
                        }
                    }
                } else if (haystack.isType()) {
                    auto td = static_cast<ObjTypeDef*>(haystack.asObj());
                    found = checkValueType(needle, td);  // 包含：needle 是 td 类型的值
                } else if (haystack.isInstance()) {
                    auto [method, owner] = findDunder(haystack, DUNDER_CONTAINS);
                    if (method) {
                        found = evaluateTruthiness(callDunder(haystack, method, owner, {needle}));
                    } else {
                        auto inst = haystack.asInstance();
                        if (needle.isString()) {
                            std::string key = needle.asString();
                            ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                            if (ctxOwner) {
                                std::string mangledName = manglePrivate(ctxOwner->classId, key);
                                if (inst->properties.find(mangledName) != inst->properties.end()) {
                                    found = true;
                                }
                            }
                            if (!found) {
                                auto it = inst->properties.find(key);
                                if (it != inst->properties.end() && !it->second.is_local) {
                                    found = true;
                                }
                            }
                            if (!found) {
                                auto cls = inst->classDef;
                                while (cls) {
                                    auto cit = cls->properties.find(key);
                                    if (cit != cls->properties.end() && !cit->second.is_local && cit->second.val.isFunctionClosure()) {
                                        found = true;
                                        break;
                                    }
                                    cls = cls->parent;
                                }
                                if (!found) {
                                    auto [getattrMethod, getattrOwner] = findDunder(haystack, DUNDER_GETATTR);
                                    if (getattrMethod) {
                                        try {
                                            callDunder(haystack, getattrMethod, getattrOwner, {needle});
                                            found = true;
                                        } catch (...) {
                                            // Fall through to false
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    throw std::runtime_error("VM Error: 'in' requires a string, list, dict, set, matrix, or instance.");
                }
                
                getReg(a) = Value(found);
                break;
            }
            case OpCode::TRY_BEGIN: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                ExceptionHandler handler;
                handler.frameIndex = frameCount - 1;
                handler.ip = ip + sbx;
                handler.registerBase = frame->registerBase;
                handler.errReg = a;
                handler.deferBase = static_cast<int>(deferStack.size());
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value errVal = getReg(a);
                frame->ip = ip;
                errVal = wrapException("Exception", errVal);
                throw ValueException(errVal);
            }
            case OpCode::CLASS: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (bx == ESCAPE_NORMAL_16) bx = FETCH_EXTRA();
                const std::string& name = chunk->constants.data()[bx].asString();
                auto cls = GcHeap::get().allocate<ObjClass>();
                cls->name = name;
                getReg(a) = Value(cls);
                break;
            }
            case OpCode::METHOD:
            case OpCode::METHOD_PRIVATE:
            case OpCode::METHOD_CONST:
            case OpCode::METHOD_PRIVATE_CONST: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                const std::string& methodName = chunk->constants.data()[b].asString();
                Value classVal = getReg(a);
                Value closureVal = getReg(c);
                
                if (!classVal.isClass()) throw std::runtime_error("VM Error: METHOD requires a class.");
                auto cls = static_cast<ObjClass*>(classVal.asObj());
                
                if (closureVal.isFunctionClosure()) {
                    ObjClosure* fn = closureVal.asFunction();
                    if (op == OpCode::METHOD_PRIVATE || op == OpCode::METHOD_PRIVATE_CONST) {
                        fn->is_local = true;
                        fn->owner_class = cls;
                        std::string mangledName = manglePrivate(cls ? cls->classId : 0, methodName);
                        if (cls) cls->properties[mangledName] = {closureVal, op == OpCode::METHOD_PRIVATE_CONST, true};
                    } else {
                        if (cls) cls->properties[methodName] = {closureVal, op == OpCode::METHOD_CONST, false};
                    }
                } else {
                    throw std::runtime_error("VM Error: Invalid closure type for method.");
                }
                break;
            }
            case OpCode::INHERIT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                
                Value subClass = getReg(a);
                Value superClass = getReg(b);
                
                if (!subClass.isClass() || !superClass.isClass()) throw std::runtime_error("VM Error: Inheritance requires two classes.");
                auto sub = static_cast<ObjClass*>(subClass.asObj());
                auto sup = static_cast<ObjClass*>(superClass.asObj());
                
                if (sub) sub->parent = sup;
                break;
            }
            case OpCode::GET_PRIVATE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[c]);
                Value keyVal = chunk->constants.data()[ic.nameIdx];
                Value obj = getReg(b);
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    ObjClass* owner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                    if (!owner) throw std::runtime_error("VM Error: Cannot access private property outside of class context.");
                    
                    std::string mangledName = manglePrivate(owner->classId, keyVal.asString());
                    auto it = inst->properties.find(mangledName);
                    if (it != inst->properties.end()) {
                        getReg(a) = it->second.val;
                        break;
                    }
                    
                    auto cit = owner->properties.find(mangledName);
                    if (cit != owner->properties.end()) {
                        if (cit->second.val.isFunctionClosure()) {
                            auto rawMethod = cit->second.val.asFunction();
                            auto bound = GcHeap::get().allocate<ObjClosure>(
                                std::vector<std::string>{}, std::vector<bool>{}, keyVal.asString(), nullptr
                            );
                            bound->paramNames = rawMethod->paramNames;
                            bound->isRef = rawMethod->isRef;
                            bound->defaultValues = rawMethod->defaultValues;
                            bound->restName = rawMethod->restName;
                            bound->compiledFnIndex = rawMethod->compiledFnIndex;
                            if (rawMethod->upvalueCount > 0) {
                                bound->upvalueCount = rawMethod->upvalueCount;
                                bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                for (int i = 0; i < bound->upvalueCount; ++i) {
                                    bound->upvalues[i] = rawMethod->upvalues[i];
                                }
                            }
                            if (rawMethod->paramTypesCount > 0) {
                                bound->paramTypesCount = rawMethod->paramTypesCount;
                                bound->paramTypes = new Value[bound->paramTypesCount];
                                for (int i = 0; i < bound->paramTypesCount; ++i) {
                                    bound->paramTypes[i] = rawMethod->paramTypes[i];
                                }
                            }
                            bound->returnType = rawMethod->returnType;
                            bound->nativeFn = rawMethod->nativeFn;
                            bound->boundSelf = Value(inst);
                            bound->boundClass = Value(owner);
                            bound->is_local = true;
                            getReg(a) = Value(bound);
                        } else {
                            getReg(a) = cit->second.val;
                        }
                        break;
                    }
                    
                    throw std::runtime_error("VM Error: Private property '" + keyVal.asString() + "' not found.");
                } else if (obj.isClass()) {
                    ObjClass* owner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                    if (!owner) throw std::runtime_error("VM Error: Cannot access private property outside of class context.");
                    
                    std::string mangledName = manglePrivate(owner->classId, keyVal.asString());
                    auto it = owner->properties.find(mangledName);
                    if (it != owner->properties.end()) {
                        if (it->second.val.isFunctionClosure()) {
                            auto rawMethod = it->second.val.asFunction();
                            auto bound = GcHeap::get().allocate<ObjClosure>(
                                std::vector<std::string>{}, std::vector<bool>{}, keyVal.asString(), nullptr
                            );
                            bound->paramNames = rawMethod->paramNames;
                            bound->isRef = rawMethod->isRef;
                            bound->defaultValues = rawMethod->defaultValues;
                            bound->restName = rawMethod->restName;
                            bound->compiledFnIndex = rawMethod->compiledFnIndex;
                            if (rawMethod->upvalueCount > 0) {
                                bound->upvalueCount = rawMethod->upvalueCount;
                                bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                for (int i = 0; i < bound->upvalueCount; ++i) {
                                    bound->upvalues[i] = rawMethod->upvalues[i];
                                }
                            }
                            if (rawMethod->paramTypesCount > 0) {
                                bound->paramTypesCount = rawMethod->paramTypesCount;
                                bound->paramTypes = new Value[bound->paramTypesCount];
                                for (int i = 0; i < bound->paramTypesCount; ++i) {
                                    bound->paramTypes[i] = rawMethod->paramTypes[i];
                                }
                            }
                            bound->returnType = rawMethod->returnType;
                            bound->nativeFn = rawMethod->nativeFn;
                            bound->boundSelf = Value::none();
                            bound->boundClass = Value(owner);
                            bound->is_local = true;
                            getReg(a) = Value(bound);
                        } else {
                            getReg(a) = it->second.val;
                        }
                        break;
                    }
                    throw std::runtime_error("VM Error: Private static property '" + keyVal.asString() + "' not found.");
                }
                throw std::runtime_error("VM Error: Cannot get private property on this type.");
            }
            case OpCode::GET_PROP: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[c]);
                Value keyVal = chunk->constants.data()[ic.nameIdx];
                Value obj = getReg(b);
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    auto it = inst->properties.find(keyVal.asString());
                    if (it != inst->properties.end() && !it->second.is_local) {
                        getReg(a) = it->second.val;
                        break;
                    }
                } else if (obj.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(obj.asObj());
                    if (ic.cachedBuiltinType == BuiltinType::DICT && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(d->elements.size())) {
                        if (d->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                            getReg(a) = d->elements[ic.cachedFieldIndex].second;
                            break;
                        }
                    }
                    auto it = d->keyMap.find(keyVal);
                    if (it != d->keyMap.end()) {
                        getReg(a) = d->elements[it->second].second;
                        ic.cachedBuiltinType = BuiltinType::DICT;
                        ic.cachedFieldIndex = static_cast<int>(it->second);
                        break;
                    }
                }

                const std::string& field = keyVal.asString();
                bool found = false;
                Value result;
                
                BuiltinType objBt = BuiltinType::UNKNOWN;
                if (obj.isObjType(ObjType::LIST)) objBt = BuiltinType::LIST;
                else if (obj.isObjType(ObjType::DICT)) objBt = BuiltinType::DICT;
                else if (obj.isObjType(ObjType::SET)) objBt = BuiltinType::SET;
                else if (obj.isString()) objBt = BuiltinType::STRING;
                else if (obj.isObjType(ObjType::REAL_MATRIX)) objBt = BuiltinType::REALMAT;
                else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) objBt = BuiltinType::COMPLEXMAT;
                else if (obj.isObjType(ObjType::SYM_MATRIX)) objBt = BuiltinType::SYMMAT;

                if (objBt != BuiltinType::UNKNOWN && ic.cachedBuiltinType == objBt && ic.cachedMethod) {
                    auto rawMethod = ic.cachedMethod;
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->restName = rawMethod->restName;
                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    if (rawMethod->paramTypesCount > 0) {
                        bound->paramTypesCount = rawMethod->paramTypesCount;
                        bound->paramTypes = new Value[bound->paramTypesCount];
                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                        }
                    }
                    bound->returnType = rawMethod->returnType;
                    bound->nativeFn = rawMethod->nativeFn;
                    bound->boundSelf = obj;
                    bound->boundClass = Value(ic.cachedClass);
                    result = Value(bound);
                    found = true;
                } else if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
                        auto rawMethod = ic.cachedMethod;
                        auto bound = GcHeap::get().allocate<ObjClosure>(
                            std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                        );
                        bound->paramNames = rawMethod->paramNames;
                        bound->isRef = rawMethod->isRef;
                        bound->defaultValues = rawMethod->defaultValues;
                        bound->restName = rawMethod->restName;
                        bound->compiledFnIndex = rawMethod->compiledFnIndex;
                        if (rawMethod->upvalueCount > 0) {
                            bound->upvalueCount = rawMethod->upvalueCount;
                            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                            for (int i = 0; i < bound->upvalueCount; ++i) {
                                bound->upvalues[i] = rawMethod->upvalues[i];
                            }
                        }
                        if (rawMethod->paramTypesCount > 0) {
                            bound->paramTypesCount = rawMethod->paramTypesCount;
                            bound->paramTypes = new Value[bound->paramTypesCount];
                            for (int i = 0; i < bound->paramTypesCount; ++i) {
                                bound->paramTypes[i] = rawMethod->paramTypes[i];
                            }
                        }
                        bound->returnType = rawMethod->returnType;
                        bound->nativeFn = rawMethod->nativeFn;
                        bound->boundSelf = Value(inst);
                        bound->boundClass = Value(ic.cachedClass);
                        result = Value(bound);
                        found = true;
                    }
                    if (!found) {
                        auto cls = inst->classDef;
                        while (cls) {
                            auto it = cls->properties.find(field);
                            if (it != cls->properties.end() && !it->second.is_local && it->second.val.isFunctionClosure()) {
                                auto rawMethod = it->second.val.asFunction();
                                ic.cachedClassId = inst->classDef->classId;
                                ic.cachedMethod = rawMethod;
                                ic.cachedClass = cls;
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->paramNames = rawMethod->paramNames;
                                bound->isRef = rawMethod->isRef;
                                bound->defaultValues = rawMethod->defaultValues;
                                bound->restName = rawMethod->restName;
                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                if (rawMethod->upvalueCount > 0) {
                                    bound->upvalueCount = rawMethod->upvalueCount;
                                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                    for (int i = 0; i < bound->upvalueCount; ++i) {
                                        bound->upvalues[i] = rawMethod->upvalues[i];
                                    }
                                }
                                if (rawMethod->paramTypesCount > 0) {
                                    bound->paramTypesCount = rawMethod->paramTypesCount;
                                    bound->paramTypes = new Value[bound->paramTypesCount];
                                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                                    }
                                }
                                bound->returnType = rawMethod->returnType;
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
                            auto [getattrMethod, owner] = findDunder(obj, "__getattr__");
                            if (getattrMethod) {
                                try {
                                    result = callDunder(obj, getattrMethod, owner, {Value(field)});
                                    found = true;
                                } catch (...) {
                                    found = false;
                                }
                            }
                        }
                    }
                } else if (!found) {
                    ObjClass* nativeProto = nullptr;
                    if (objBt == BuiltinType::LIST) nativeProto = listProto;
                    else if (objBt == BuiltinType::DICT) nativeProto = dictProto;
                    else if (objBt == BuiltinType::SET) nativeProto = setProto;
                    else if (objBt == BuiltinType::STRING) nativeProto = stringProto;
                    else if (objBt == BuiltinType::REALMAT || objBt == BuiltinType::COMPLEXMAT || objBt == BuiltinType::SYMMAT) nativeProto = matrixProto;

                    if (nativeProto) {
                        auto it = nativeProto->properties.find(field);
                        if (it != nativeProto->properties.end() && !it->second.is_local) {
                            if (it->second.val.isFunctionClosure()) {
                                auto rawMethod = it->second.val.asFunction();
                                ic.cachedBuiltinType = objBt;
                                ic.cachedMethod = rawMethod;
                                ic.cachedClass = nativeProto;
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->paramNames = rawMethod->paramNames;
                                bound->isRef = rawMethod->isRef;
                                bound->defaultValues = rawMethod->defaultValues;
                                bound->restName = rawMethod->restName;
                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                if (rawMethod->upvalueCount > 0) {
                                    bound->upvalueCount = rawMethod->upvalueCount;
                                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                    for (int i = 0; i < bound->upvalueCount; ++i) {
                                        bound->upvalues[i] = rawMethod->upvalues[i];
                                    }
                                }
                                if (rawMethod->paramTypesCount > 0) {
                                    bound->paramTypesCount = rawMethod->paramTypesCount;
                                    bound->paramTypes = new Value[bound->paramTypesCount];
                                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                                    }
                                }
                                bound->returnType = rawMethod->returnType;
                                bound->nativeFn = rawMethod->nativeFn;
                                bound->boundSelf = obj;
                                bound->boundClass = Value(nativeProto);
                                result = Value(bound);
                            } else {
                                result = it->second.val;
                            }
                            found = true;
                        }
                    }
                }
                
                if (!found && obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    auto it = ns->fields.find(field);
                    if (it != ns->fields.end()) {
                        result = *(it->second.upval->location);
                        found = true;
                    }
                } else if (!found && obj.isSlice()) {
                    Value prop = obj.asSlice()->getProperty(field);
                    if (!prop.isUninit()) {
                        result = prop;
                        found = true;
                    }
                } else if (!found && obj.isClass()) {
                    auto cls = static_cast<ObjClass*>(obj.asObj());
                    while (cls) {
                        auto it = cls->properties.find(field);
                        if (it != cls->properties.end() && !it->second.is_local) {
                            if (it->second.val.isFunctionClosure()) {
                                auto rawMethod = it->second.val.asFunction();
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->paramNames = rawMethod->paramNames;
                                bound->isRef = rawMethod->isRef;
                                bound->defaultValues = rawMethod->defaultValues;
                                bound->restName = rawMethod->restName;
                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                if (rawMethod->upvalueCount > 0) {
                                    bound->upvalueCount = rawMethod->upvalueCount;
                                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                    for (int i = 0; i < bound->upvalueCount; ++i) {
                                        bound->upvalues[i] = rawMethod->upvalues[i];
                                    }
                                }
                                if (rawMethod->paramTypesCount > 0) {
                                    bound->paramTypesCount = rawMethod->paramTypesCount;
                                    bound->paramTypes = new Value[bound->paramTypesCount];
                                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                                    }
                                }
                                bound->returnType = rawMethod->returnType;
                                bound->nativeFn = rawMethod->nativeFn;
                                bound->boundSelf = Value::none();
                                bound->boundClass = Value(cls);
                                result = Value(bound);
                            } else {
                                result = it->second.val;
                            }
                            found = true;
                            break;
                        }
                        cls = cls->parent;
                    }
                }
                
                if (!found) {
                    if (ic.cachedGlobalSlot == -4) {
                        auto bound = GcHeap::get().allocate<ObjClosure>(
                            std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                        );
                        bound->boundSelf = obj;
                        
                        Value builtinClosureVal = getBuiltinClosure(field);
                        ObjClosure* targetFn = builtinClosureVal.asFunction();
                        bound->paramNames = targetFn->paramNames;
                        bound->isRef = targetFn->isRef;
                        bound->defaultValues = targetFn->defaultValues;
                        bound->restName = targetFn->restName;
                        bound->isUFCS = true;

                        bound->nativeFn = ic.cachedNativeFn;
                        result = Value(bound);
                        found = true;
                    } else {
                        if (ic.cachedGlobalSlot >= 0) {
                            if (globals[ic.cachedGlobalSlot].isFunctionClosure()) {
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->boundSelf = obj;
                                ObjClosure* targetFn = globals[ic.cachedGlobalSlot].asFunction();
                            
                                bound->restName = targetFn->restName;
                                bound->paramNames = targetFn->paramNames;
                                bound->isRef = targetFn->isRef;
                                bound->defaultValues = targetFn->defaultValues;
                                bound->isUFCS = true;

                                if (targetFn->isBytecode()) {
                                    bound->compiledFnIndex = targetFn->compiledFnIndex;
                                    if (targetFn->upvalueCount > 0) {
                                        bound->upvalueCount = targetFn->upvalueCount;
                                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                        for (int i = 0; i < bound->upvalueCount; ++i) {
                                            bound->upvalues[i] = targetFn->upvalues[i];
                                        }
                                    }
                                    if (targetFn->paramTypesCount > 0) {
                                        bound->paramTypesCount = targetFn->paramTypesCount;
                                        bound->paramTypes = new Value[bound->paramTypesCount];
                                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                                            bound->paramTypes[i] = targetFn->paramTypes[i];
                                        }
                                    }
                                    bound->returnType = targetFn->returnType;
                                } else {
                                    bound->boundClass = targetFn->boundClass;
                                }
                                bound->nativeFn = targetFn->nativeFn;
                                result = Value(bound);
                                found = true;
                            }
                        } else {
                            auto gIt = globalNames.find(field);
                            if (gIt != globalNames.end() && globals[gIt->second].isFunctionClosure()) {
                                ic.cachedGlobalSlot = gIt->second;
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->boundSelf = obj;
                                ObjClosure* targetFn = globals[gIt->second].asFunction();
                            
                                bound->restName = targetFn->restName;
                                bound->paramNames = targetFn->paramNames;
                                bound->isRef = targetFn->isRef;
                                bound->defaultValues = targetFn->defaultValues;
                                bound->isUFCS = true;

                                if (targetFn->isBytecode()) {
                                    bound->compiledFnIndex = targetFn->compiledFnIndex;
                                    if (targetFn->upvalueCount > 0) {
                                        bound->upvalueCount = targetFn->upvalueCount;
                                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                        for (int i = 0; i < bound->upvalueCount; ++i) {
                                            bound->upvalues[i] = targetFn->upvalues[i];
                                        }
                                    }
                                    if (targetFn->paramTypesCount > 0) {
                                        bound->paramTypesCount = targetFn->paramTypesCount;
                                        bound->paramTypes = new Value[bound->paramTypesCount];
                                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                                            bound->paramTypes[i] = targetFn->paramTypes[i];
                                        }
                                    }
                                    bound->returnType = targetFn->returnType;
                                } else {
                                    bound->boundClass = targetFn->boundClass;
                                }
                                bound->nativeFn = targetFn->nativeFn;
                                result = Value(bound);
                                found = true;
                            }
                        }

                        if (!found) {
                            auto nIt = nativeBuiltins.find(field);
                            if (nIt != nativeBuiltins.end()) {
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->boundSelf = obj;
                                
                                Value builtinClosureVal = getBuiltinClosure(field);
                                ObjClosure* targetFn = builtinClosureVal.asFunction();
                                bound->paramNames = targetFn->paramNames;
                                bound->isRef = targetFn->isRef;
                                bound->defaultValues = targetFn->defaultValues;
                                bound->restName = targetFn->restName;
                                bound->isUFCS = true;

                                NativeCallable nativeFn = nIt->second;
                                
                                auto ait = builtinArity.find(field);
                                std::set<int> allowedArities;
                                if (ait != builtinArity.end()) allowedArities = ait->second;

                                ic.cachedGlobalSlot = -4;
                                ic.cachedNativeFn = std::make_any<NativeCallable>(nativeFn);
                                bound->nativeFn = ic.cachedNativeFn;
                                
                                result = Value(bound);
                                found = true;
                            }
                        }
                    }
                }

                if (!found) {
                    throw std::runtime_error("VM Error: Property '" + field + "' not found.");
                }
                getReg(a) = result;
                break;
            }
            case OpCode::TRY_GET_PROP: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[c]);
                Value keyVal = chunk->constants.data()[ic.nameIdx];
                Value obj = getReg(b);
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    auto it = inst->properties.find(keyVal.asString());
                    if (it != inst->properties.end() && !it->second.is_local) {
                        getReg(a) = it->second.val;
                        break;
                    }
                } else if (obj.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(obj.asObj());
                    if (ic.cachedBuiltinType == BuiltinType::DICT && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(d->elements.size())) {
                        if (d->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                            getReg(a) = d->elements[ic.cachedFieldIndex].second;
                            break;
                        }
                    }
                    auto it = d->keyMap.find(keyVal);
                    if (it != d->keyMap.end()) {
                        getReg(a) = d->elements[it->second].second;
                        ic.cachedBuiltinType = BuiltinType::DICT;
                        ic.cachedFieldIndex = static_cast<int>(it->second);
                        break;
                    }
                }

                const std::string& field = keyVal.asString();
                bool found = false;
                Value result;
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
                        auto rawMethod = ic.cachedMethod;
                        auto bound = GcHeap::get().allocate<ObjClosure>(
                            std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                        );
                        bound->paramNames = rawMethod->paramNames;
                        bound->isRef = rawMethod->isRef;
                        bound->defaultValues = rawMethod->defaultValues;
                        bound->restName = rawMethod->restName;
                        bound->compiledFnIndex = rawMethod->compiledFnIndex;
                        if (rawMethod->upvalueCount > 0) {
                            bound->upvalueCount = rawMethod->upvalueCount;
                            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                            for (int i = 0; i < bound->upvalueCount; ++i) {
                                bound->upvalues[i] = rawMethod->upvalues[i];
                            }
                        }
                        if (rawMethod->paramTypesCount > 0) {
                            bound->paramTypesCount = rawMethod->paramTypesCount;
                            bound->paramTypes = new Value[bound->paramTypesCount];
                            for (int i = 0; i < bound->paramTypesCount; ++i) {
                                bound->paramTypes[i] = rawMethod->paramTypes[i];
                            }
                        }
                        bound->returnType = rawMethod->returnType;
                        bound->nativeFn = rawMethod->nativeFn;
                        bound->boundSelf = Value(inst);
                        bound->boundClass = Value(ic.cachedClass);
                        result = Value(bound);
                        found = true;
                    }
                    if (!found) {
                        auto cls = inst->classDef;
                        while (cls) {
                            auto it = cls->properties.find(field);
                            if (it != cls->properties.end() && !it->second.is_local && it->second.val.isFunctionClosure()) {
                                auto rawMethod = it->second.val.asFunction();
                                ic.cachedClassId = inst->classDef->classId;
                                ic.cachedMethod = rawMethod;
                                ic.cachedClass = cls;
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->paramNames = rawMethod->paramNames;
                                bound->isRef = rawMethod->isRef;
                                bound->defaultValues = rawMethod->defaultValues;
                                bound->restName = rawMethod->restName;
                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                if (rawMethod->upvalueCount > 0) {
                                    bound->upvalueCount = rawMethod->upvalueCount;
                                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                    for (int i = 0; i < bound->upvalueCount; ++i) {
                                        bound->upvalues[i] = rawMethod->upvalues[i];
                                    }
                                }
                                if (rawMethod->paramTypesCount > 0) {
                                    bound->paramTypesCount = rawMethod->paramTypesCount;
                                    bound->paramTypes = new Value[bound->paramTypesCount];
                                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                                    }
                                }
                                bound->returnType = rawMethod->returnType;
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
                            auto [getattrMethod, owner] = findDunder(obj, "__getattr__");
                            if (getattrMethod) {
                                try {
                                    result = callDunder(obj, getattrMethod, owner, {Value(field)});
                                    found = true;
                                } catch (...) {
                                    found = false;
                                }
                            }
                        }
                    }
                } else {
                    ObjClass* nativeProto = nullptr;
                    if (obj.isObjType(ObjType::LIST)) nativeProto = listProto;
                    else if (obj.isObjType(ObjType::DICT)) nativeProto = dictProto;
                    else if (obj.isObjType(ObjType::SET)) nativeProto = setProto;
                    else if (obj.isString()) nativeProto = stringProto;
                    else if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX)) nativeProto = matrixProto;

                    if (nativeProto) {
                        auto it = nativeProto->properties.find(field);
                        if (it != nativeProto->properties.end() && !it->second.is_local) {
                            if (it->second.val.isFunctionClosure()) {
                                auto rawMethod = it->second.val.asFunction();
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->paramNames = rawMethod->paramNames;
                                bound->isRef = rawMethod->isRef;
                                bound->defaultValues = rawMethod->defaultValues;
                                bound->restName = rawMethod->restName;
                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                if (rawMethod->upvalueCount > 0) {
                                    bound->upvalueCount = rawMethod->upvalueCount;
                                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                    for (int i = 0; i < bound->upvalueCount; ++i) {
                                        bound->upvalues[i] = rawMethod->upvalues[i];
                                    }
                                }
                                if (rawMethod->paramTypesCount > 0) {
                                    bound->paramTypesCount = rawMethod->paramTypesCount;
                                    bound->paramTypes = new Value[bound->paramTypesCount];
                                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                                    }
                                }
                                bound->returnType = rawMethod->returnType;
                                bound->nativeFn = rawMethod->nativeFn;
                                bound->boundSelf = obj;
                                bound->boundClass = Value(nativeProto);
                                result = Value(bound);
                            } else {
                                result = it->second.val;
                            }
                            found = true;
                        }
                    }
                }
                
                if (!found && obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    auto it = ns->fields.find(field);
                    if (it != ns->fields.end()) {
                        result = *(it->second.upval->location);
                        found = true;
                    }
                } else if (!found && obj.isSlice()) {
                    Value prop = obj.asSlice()->getProperty(field);
                    if (!prop.isUninit()) {
                        result = prop;
                        found = true;
                    }
                } else if (!found && obj.isClass()) {
                    auto cls = static_cast<ObjClass*>(obj.asObj());
                    auto it = cls->properties.find(field);
                    if (it != cls->properties.end() && !it->second.is_local) {
                        result = it->second.val;
                        found = true;
                    }
                }
                
                if (!found) {
                    if (ic.cachedGlobalSlot == -4) {
                        auto bound = GcHeap::get().allocate<ObjClosure>(
                            std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                        );
                        bound->boundSelf = obj;
                        bound->nativeFn = ic.cachedNativeFn;
                        result = Value(bound);
                        found = true;
                    } else {
                        if (ic.cachedGlobalSlot >= 0) {
                            if (globals[ic.cachedGlobalSlot].isFunctionClosure()) {
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                                );
                                bound->boundSelf = obj;
                                ObjClosure* targetFn = globals[ic.cachedGlobalSlot].asFunction();
                            
                                if (targetFn->isBytecode()) {
                                    bound->compiledFnIndex = targetFn->compiledFnIndex;
                                    if (targetFn->upvalueCount > 0) {
                                        bound->upvalueCount = targetFn->upvalueCount;
                                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                        for (int i = 0; i < bound->upvalueCount; ++i) {
                                            bound->upvalues[i] = targetFn->upvalues[i];
                                        }
                                    }
                                    bound->restName = targetFn->restName;
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
                        } else {
                            auto gIt = globalNames.find(field);
                            if (gIt != globalNames.end() && globals[gIt->second].isFunctionClosure()) {
                                ic.cachedGlobalSlot = gIt->second;
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
                                    bound->restName = targetFn->restName;
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

                        if (!found) {
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
                                
                                ic.cachedGlobalSlot = -4;
                                ic.cachedNativeFn = bound->nativeFn;
                                
                                result = Value(bound);
                                found = true;
                            }
                        }
                    }
                }

                if (found) {
                    getReg(a) = Value(true);
                    getReg(a + 1) = result;
                } else {
                    getReg(a) = Value(false);
                    getReg(a + 1) = Value::none();
                }
                break;
            }
            case OpCode::SET_PRIVATE:
            case OpCode::DEFINE_PRIVATE:
            case OpCode::DEFINE_PRIVATE_CONST: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[b]);
                Value keyVal = chunk->constants.data()[ic.nameIdx];
                Value obj = getReg(a);
                Value val = getReg(c);
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    inst->checkModify();
                    ObjClass* owner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                    if (!owner) throw std::runtime_error("VM Error: Cannot access private property outside of class context.");
                    
                    std::string mangledName = manglePrivate(owner->classId, keyVal.asString());
                    auto it = inst->properties.find(mangledName);
                    if (op == OpCode::SET_PRIVATE) {
                        if (it == inst->properties.end()) throw std::runtime_error("VM Error: Private property '" + keyVal.asString() + "' not found.");
                        if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const private property '" + keyVal.asString() + "'.");
                        it->second.val = val;
                    } else {
                        if (it != inst->properties.end()) throw std::runtime_error("VM Error: Private property '" + keyVal.asString() + "' already defined.");
                        inst->properties[mangledName] = {val, op == OpCode::DEFINE_PRIVATE_CONST, true};
                    }
                } else if (obj.isClass()) {
                    auto cls = static_cast<ObjClass*>(obj.asObj());
                    std::string keyStr = keyVal.asString();
                    if (op == OpCode::SET_PRIVATE) {
                        ObjClass* owner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                        if (!owner) throw std::runtime_error("VM Error: Cannot access private property outside of class context.");
                        std::string mangledName = manglePrivate(owner->classId, keyStr);
                        auto it = owner->properties.find(mangledName);
                        if (it == owner->properties.end()) throw std::runtime_error("VM Error: Private static property '" + keyStr + "' not found.");
                        if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const private static property '" + keyStr + "'.");
                        it->second.val = val;
                    } else {
                        std::string mangledName = manglePrivate(cls->classId, keyStr);
                        auto it = cls->properties.find(mangledName);
                        if (it != cls->properties.end()) throw std::runtime_error("VM Error: Private static property '" + keyStr + "' already defined.");
                        if (cls) cls->properties[mangledName] = { val, op == OpCode::DEFINE_PRIVATE_CONST, true };
                    }
                } else {
                    throw std::runtime_error("VM Error: Cannot set private property on this type.");
                }
                break;
            }
            case OpCode::DEFINE_PROP:
            case OpCode::DEFINE_PROP_CONST: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[b]);
                Value keyVal = chunk->constants.data()[ic.nameIdx];
                Value obj = getReg(a);
                Value val = getReg(c);
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    inst->checkModify();
                    std::string keyStr = keyVal.asString();
                    
                    auto it = inst->properties.find(keyStr);
                    if (it != inst->properties.end()) {
                        if (it->second.is_local) throw std::runtime_error("VM Error: Cannot access private property '" + keyStr + "' externally.");
                        throw std::runtime_error("VM Error: Property '" + keyStr + "' already defined.");
                    }
                    
                    inst->properties[keyStr] = {val, op == OpCode::DEFINE_PROP_CONST, false};
                } else if (obj.isClass()) {
                    auto cls = static_cast<ObjClass*>(obj.asObj());
                    std::string keyStr = keyVal.asString();
                    auto it = cls->properties.find(keyStr);
                    if (it != cls->properties.end()) {
                        throw std::runtime_error("VM Error: Static property '" + keyStr + "' already defined.");
                    }
                    if (cls) cls->properties[keyStr] = { val, op == OpCode::DEFINE_PROP_CONST, false };
                } else {
                    throw std::runtime_error("VM Error: Cannot define property on this type.");
                }
                break;
            }
            case OpCode::SET_PROP: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[b]);
                Value keyVal = chunk->constants.data()[ic.nameIdx];
                Value obj = getReg(a);
                Value val = getReg(c);
                
                if (obj.isInstance()) {
                    auto inst = obj.asInstance();
                    std::string keyStr = keyVal.asString();
                    
                    auto [setattrMethod, owner] = findDunder(obj, DUNDER_SETATTR);
                    if (setattrMethod) {
                        inst->checkModify();
                        auto it = inst->properties.find(keyStr);
                        if (it != inst->properties.end()) {
                            if (it->second.is_local) throw std::runtime_error("Runtime Error: Cannot modify private property '" + keyStr + "'.");
                            if (it->second.is_const) throw std::runtime_error("Runtime Error: Cannot modify const property '" + keyStr + "'.");
                        }
                        callDunder(obj, setattrMethod, owner, {keyVal, val});
                    } else {
                        // ★ 容器属性被替换时，失效所有 JIT 代码（INSTANCE 分支）
                        Value oldVal = Value::none();
                        auto oldIt = inst->properties.find(keyStr);
                        if (oldIt != inst->properties.end()) oldVal = oldIt->second.val;
                        invalidateJITOnContainerReplace(oldVal, val);
                        inst->setProperty(keyStr, val);
                    }
                } else if (obj.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(obj.asObj());
                    if (ic.cachedBuiltinType == BuiltinType::DICT && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(d->elements.size())) {
                        if (d->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                            invalidateJITOnContainerReplace(d->elements[ic.cachedFieldIndex].second, val);
                            d->elements[ic.cachedFieldIndex].second = val;
                            goto set_prop_dict_done;
                        }
                    }
                    {
                        auto it = d->keyMap.find(keyVal);
                        if (it != d->keyMap.end()) {
                            invalidateJITOnContainerReplace(d->elements[it->second].second, val);
                            d->elements[it->second].second = val;
                            ic.cachedBuiltinType = BuiltinType::DICT;
                            ic.cachedFieldIndex = static_cast<int>(it->second);
                        } else {
                            invalidateJITOnContainerReplace(Value::none(), val);
                            ic.cachedBuiltinType = BuiltinType::DICT;
                            ic.cachedFieldIndex = static_cast<int>(d->elements.size());
                            d->keyMap[keyVal] = d->elements.size();
                            d->elements.push_back({keyVal, val});
                        }
                    }
                set_prop_dict_done:;
                } else if (obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    Value oldVal = Value::none();
                    auto nsIt = ns->fields.find(keyVal.asString());
                    if (nsIt != ns->fields.end()) oldVal = *(nsIt->second.upval->location);
                    invalidateJITOnContainerReplace(oldVal, val);
                    ns->setField(keyVal.asString(), val);
                } else if (obj.isClass()) {
                    auto cls = static_cast<ObjClass*>(obj.asObj());
                    std::string keyStr = keyVal.asString();
                    
                    bool found = false;
                    auto c_cls = cls;
                    while (c_cls) {
                        auto it = c_cls->properties.find(keyStr);
                        if (it != c_cls->properties.end()) {
                            if (it->second.is_local) {
                                if (c_cls == cls) throw std::runtime_error("VM Error: Cannot modify private static property '" + keyStr + "'.");
                                break;
                            }
                            if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const static property '" + keyStr + "'.");
                            invalidateJITOnContainerReplace(it->second.val, val);
                            it->second.val = val;
                            found = true;
                            break;
                        }
                        c_cls = c_cls->parent;
                    }
                    if (!found) {
                        invalidateJITOnContainerReplace(Value::none(), val);
                        if (cls) cls->properties[keyStr] = { val, false, false };
                    }
                } else {
                    throw std::runtime_error("VM Error: Cannot set property on this type.");
                }
                break;
            }
            case OpCode::DICT_REST: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
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
                    for (const auto& [k, prop] : inst->properties) {
                        if (prop.is_local) continue;
                        if (isReservedInternalName(k)) continue;
                        if (excludeKeys.count(k)) continue;
                        restDict->set(Value(k), prop.val);
                    }
                } else if (obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    for (const auto& [k, field] : ns->fields) {
                        if (excludeKeys.count(k)) continue;
                        restDict->set(Value(k), *(field.upval->location));
                    }
                } else if (obj.isClass()) {
                    auto cls = static_cast<ObjClass*>(obj.asObj());
                    while (cls) {
                        for (const auto& [k, prop] : cls->properties) {
                            if (prop.is_local) continue;
                            if (isReservedInternalName(k)) continue;
                            if (excludeKeys.count(k)) continue;
                            if (restDict->keyMap.find(Value(k)) == restDict->keyMap.end()) {
                                restDict->set(Value(k), prop.val);
                            }
                        }
                        cls = cls->parent;
                    }
                }
                break;
            }
            case OpCode::BUILD_NAMESPACE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                const std::string& nsName = chunk->constants.data()[b].asString();
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
            case OpCode::MATRIX_COMP_INIT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                ObjList* acc = GcHeap::get().allocate<ObjList>();
                getReg(a) = Value(acc);
                break;
            }
            case OpCode::MATRIX_COMP_APPEND: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value elem = getReg(b);
                Value m;
                if (elem.isObjType(ObjType::REAL_MATRIX) || elem.isObjType(ObjType::COMPLEX_MATRIX) || elem.isObjType(ObjType::SYM_MATRIX)) {
                    m = elem;
                } else if (elem.isSymbolic()) {
                    m = Value(SymMatrix(1, 1, { elem.asSymbolic() }));
                } else if (elem.isComplex()) {
                    m = Value(ComplexMatrix(1, 1, { elem.asComplex() }));
                } else if (elem.isNumber() || elem.isBigInt() || elem.isObjType(ObjType::FRACTION)) {
                    try {
                        m = Value(RealMatrix(1, 1, { elem.asDouble() }));
                    } catch (...) {
                        throw std::runtime_error("VM Error: Matrix elements must be numeric, complex, or symbolic. Use @[...] for lists.");
                    }
                } else {
                    throw std::runtime_error("VM Error: Matrix elements must be numeric, complex, or symbolic. Use @[...] for lists.");
                }
                GcValueGuard mGuard(m);
                static_cast<ObjList*>(getReg(a).asObj())->vec.push_back(m);
                break;
            }
            case OpCode::MATRIX_COMP_END: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                auto l = static_cast<ObjList*>(getReg(a).asObj());
                auto& vec = l->vec;
                if (vec.empty()) { getReg(a) = Value(RealMatrix(1, 0)); break; }
                
                bool hasComplex = false, hasSymbolic = false;
                for (const auto& v : vec) {
                    if (v.isObjType(ObjType::COMPLEX_MATRIX)) hasComplex = true;
                    if (v.isObjType(ObjType::SYM_MATRIX)) hasSymbolic = true;
                }
                
                try {
                    Value rowResult = Value::none();
                    for (int j = 0; j < static_cast<int>(vec.size()); ++j) {
                        Value cell = vec[j];
                        if (hasSymbolic) {
                            if (cell.isObjType(ObjType::REAL_MATRIX) || cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                                cell = Value(cell.asSymMatrix());
                            }
                        } else if (hasComplex && cell.isObjType(ObjType::REAL_MATRIX)) {
                            cell = Value(cell.asComplexMatrix());
                        }
                        if (rowResult.isNone()) {
                            rowResult = cell;
                        } else {
                            if (hasSymbolic)
                                rowResult = Value(static_cast<ObjSymMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjSymMatrix*>(cell.asObj())->mat));
                            else if (hasComplex)
                                rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                            else
                                rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                        }
                    }
                    getReg(a) = rowResult;
                } catch (...) {
                    throw std::runtime_error("VM Error: Dimension mismatch during matrix comprehension concatenation.");
                }
                break;
            }
            case OpCode::MAKE_SPREAD: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                ObjSpread* sp = GcHeap::get().allocate<ObjSpread>();
                getReg(a) = Value(sp);
                sp->value = getReg(b);
                sp->isKeyword = (c != 0);
                break;
            }
            case OpCode::BUILD_SLICE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                
                ObjSlice* slice = GcHeap::get().allocate<ObjSlice>();
                getReg(a) = Value(slice); // ★ 立即 Root 防止 GC 误杀
                
                auto readInt = [&](int idx) -> int {
                    Value v = getReg(b + idx);
                    if (v.isNone()) return ObjSlice::SLICE_NONE;
                    int64_t val64 = 0;
                    if (v.isInt32()) {
                        val64 = v.asInt32();
                    } else if (v.isDouble()) {
                        val64 = static_cast<int64_t>(std::round(v.asDoubleRaw()));
                    } else if (v.isBigInt()) {
                        try {
                            val64 = v.asBigInt().toInt64();
                        } catch (...) {
                            throw std::runtime_error("Value Error: slice absolute value exceeds 2^31-1.");
                        }
                    } else {
                        val64 = static_cast<int64_t>(std::round(v.asDouble()));
                    }
                    if (val64 > 2147483647LL || val64 < -2147483647LL) {
                        throw std::runtime_error("Value Error: slice absolute value exceeds 2^31-1.");
                    }
                    return static_cast<int>(val64);
                };
                
                slice->start = readInt(0);
                slice->end = readInt(1);
                slice->step = readInt(2);
                break;
            }
            case OpCode::DEFER: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                deferStack.push_back(getReg(a).asFunction());
                break;
            }
            case OpCode::RUN_DEFERS: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                int count = a;
                while (count-- > 0) {
                    ObjClosure* closure = deferStack.back();
                    deferStack.pop_back();
                    callVMFunction(closure->compiledFnIndex, {}, closure, closure->boundSelf, closure->boundClass);
                }
                break;
            }
            case OpCode::IMPORT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value pathVal = getReg(b);
                if (!pathVal.isString()) throw std::runtime_error("VM Error: import requires a string path.");
                getReg(a) = execImport(pathVal.asString());
                break;
            }
            case OpCode::ASSERT_PARAM_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                execAssertParamType(getReg(a), b, c);
                break;
            }
            case OpCode::ASSERT_RETURN_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                execAssertReturnType(getReg(a));
                break;
            }
            case OpCode::ASSERT_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                execAssertType(getReg(a), getReg(b), c);
                break;
            }
            case OpCode::MATCH_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                Value val = getReg(b);
                Value typeVal = getReg(c);
                if (typeVal.isClass()) {
                    ObjClass* expectedClass = static_cast<ObjClass*>(typeVal.asObj());
                    bool matched = false;
                    if (val.isInstance()) {
                        ObjClass* cls = val.asInstance()->classDef;
                        while (cls) {
                            if (cls == expectedClass) { matched = true; break; }
                            cls = cls->parent;
                        }
                    }
                    getReg(a) = Value(matched);
                } else {
                    if (!typeVal.isType()) throw std::runtime_error("TypeError: Expected a type object.");
                    getReg(a) = Value(checkValueType(val, static_cast<ObjTypeDef*>(typeVal.asObj())));
                }
                break;
            }
            case OpCode::IS_SUBSET: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                getReg(a) = opIsSubset(getReg(b), getReg(c));
                break;
            }
            case OpCode::MATCH_INIT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value val = getReg(b);
                if (val.isInstance()) {
                    auto [method, owner] = findDunder(val, "__match__");
                    if (method) {
                        Value view = callDunder(val, method, owner, {});
                        if (view.as_bits != val.as_bits) {
                            getReg(a) = view;
                            break;
                        }
                    }
                }
                getReg(a) = val;
                break;
            }
            case OpCode::MATCH_SHAPE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                const auto& sp = chunk->shapePatterns.data()[c];
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
                } else if (val.isObjType(ObjType::SYM_MATRIX)) {
                    const auto& m = static_cast<ObjSymMatrix*>(val.asObj())->mat;
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
            case OpCode::TAIL_INVOKE:
            case OpCode::INVOKE_PRIVATE:
            case OpCode::TAIL_INVOKE_PRIVATE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                bool isTailCall = (op == OpCode::TAIL_INVOKE || op == OpCode::TAIL_INVOKE_PRIVATE);
                bool isPrivate = (op == OpCode::INVOKE_PRIVATE || op == OpCode::TAIL_INVOKE_PRIVATE);
                frame->ip = ip;
                int prevIp = ip;
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[c]);
                std::string methodName = chunk->constants.data()[ic.nameIdx].asString();
                
                int kwArgc = pendingKwArgc;
                pendingKwArgc = 0;
                execInvoke(a, b, kwArgc, c, isTailCall, -1, isPrivate);
                
                if (isTailCall && frame->ip == prevIp) {
                    Value res = std::move(getReg(a));
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    int clearBase = frame->registerBase;
                    int clearCount = frame->function->localCount + frame->function->refCount;
                    for (int i = 0; i < clearCount; ++i) {
                        registers[clearBase + i] = Value::none();
                    }
                    frame->jitReturnSlot = Value::none();
                    frame->selfContext = Value::none();
                    frame->classContext = Value::none();

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    ip = frame->ip;
                }
                break;
            }
            case OpCode::INVOKE_FALLBACK:
            case OpCode::TAIL_INVOKE_FALLBACK: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                bool isTailCall = (op == OpCode::TAIL_INVOKE_FALLBACK);
                frame->ip = ip;
                int prevIp = ip;
                int kwArgc = pendingKwArgc;
                pendingKwArgc = 0;
                execInvoke(a, b, kwArgc, c, isTailCall, 1, false);
                if (isTailCall && frame->ip == prevIp) {
                    Value res = std::move(getReg(a));
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    int clearBase = frame->registerBase;
                    int clearCount = frame->function->localCount + frame->function->refCount;
                    for (int i = 0; i < clearCount; ++i) {
                        registers[clearBase + i] = Value::none();
                    }
                    frame->jitReturnSlot = Value::none();
                    frame->selfContext = Value::none();
                    frame->classContext = Value::none();

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    ip = frame->ip;
                }
                break;
            }
            case OpCode::GET_SUPER: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                const std::string& field = chunk->constants.data()[c].asString();
                Value selfVal = getReg(b);
                if (!selfVal.isInstance()) throw std::runtime_error("VM Error: 'super' requires an instance context.");
                auto inst = selfVal.asInstance();
                
                Value classVal = frame->classContext;
                if (!classVal.isClass()) throw std::runtime_error("VM Error: 'super' requires class context.");
                auto currentClass = static_cast<ObjClass*>(classVal.asObj());
                auto parentClass = currentClass->parent;
                if (!parentClass) throw std::runtime_error("VM Error: No parent class.");
                
                ObjClosure* rawMethod = nullptr;
                ObjClass* ownerClass = nullptr;
                auto cls = parentClass;
                while (cls) {
                    auto it = cls->properties.find(field);
                    if (it != cls->properties.end() && !it->second.is_local && it->second.val.isFunctionClosure()) {
                        rawMethod = it->second.val.asFunction();
                        ownerClass = cls;
                        break;
                    }
                    cls = cls->parent;
                }
                if (!rawMethod) throw std::runtime_error("VM Error: Parent class has no method '" + field + "'.");
                
                auto bound = GcHeap::get().allocate<ObjClosure>(
                    std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                );
                bound->paramNames = rawMethod->paramNames;
                bound->isRef = rawMethod->isRef;
                bound->defaultValues = rawMethod->defaultValues;
                bound->restName = rawMethod->restName;
                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                if (rawMethod->upvalueCount > 0) {
                    bound->upvalueCount = rawMethod->upvalueCount;
                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                    for (int i = 0; i < bound->upvalueCount; ++i) {
                        bound->upvalues[i] = rawMethod->upvalues[i];
                    }
                }
                if (rawMethod->paramTypesCount > 0) {
                    bound->paramTypesCount = rawMethod->paramTypesCount;
                    bound->paramTypes = new Value[bound->paramTypesCount];
                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                    }
                }
                bound->returnType = rawMethod->returnType;
                bound->nativeFn = rawMethod->nativeFn;
                bound->boundSelf = Value(inst);
                bound->boundClass = Value(ownerClass);
                
                getReg(a) = Value(bound);
                break;
            }
            case OpCode::SUPER_INVOKE:
            case OpCode::TAIL_SUPER_INVOKE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                bool isTailCall = (op == OpCode::TAIL_SUPER_INVOKE);
                frame->ip = ip;
                int prevIp = ip;
                int kwArgc = pendingKwArgc;
                pendingKwArgc = 0;
                execSuperInvoke(a, b, kwArgc, c, isTailCall);
                if (isTailCall && frame->ip == prevIp) {
                    Value res = std::move(getReg(a));
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    int clearBase = frame->registerBase;
                    int clearCount = frame->function->localCount + frame->function->refCount;
                    for (int i = 0; i < clearCount; ++i) {
                        registers[clearBase + i] = Value::none();
                    }
                    frame->jitReturnSlot = Value::none();
                    frame->selfContext = Value::none();
                    frame->classContext = Value::none();

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    ip = frame->ip;
                }
                break;
            }
            case OpCode::GET_SELF: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (frame->selfContext.isNone()) throw std::runtime_error("VM Error: 'self' accessed outside of context.");
                getReg(a) = frame->selfContext;
                break;
            }
            case OpCode::GET_CURRENT_CLOSURE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                getReg(a) = Value(frame->closure);
                break;
            }
            case OpCode::CALL:
            case OpCode::TAIL_CALL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                if (c > 0) {
                    Value& arg1 = getReg(b + 1);
                    if (arg1.isInt32()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x01;
                    else if (arg1.isDouble()) const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x02;
                    else const_cast<Chunk*>(chunk)->typeFeedback[op_ip] |= 0x80;
                }

                bool isTailCall = (op == OpCode::TAIL_CALL);
                frame->ip = ip; // 保存当前 IP
                int prevIp = ip;
                int kwArgc = pendingKwArgc;
                pendingKwArgc = 0;
                execCall(b, c, kwArgc, a, isTailCall);
                if (isTailCall && frame->ip == prevIp) {
                    Value res = std::move(getReg(a));
                    while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                        exceptionHandlers.pop_back();
                    }
                    closeUpvalues(frame->registerBase);
                    int targetReg = frame->returnRegister;
                    bool isInit = (frame->function && frame->function->name == "init");
                    Value selfCtx = frame->selfContext;

                    int clearBase = frame->registerBase;
                    int clearCount = frame->function->localCount + frame->function->refCount;
                    for (int i = 0; i < clearCount; ++i) {
                        registers[clearBase + i] = Value::none();
                    }
                    frame->jitReturnSlot = Value::none();
                    frame->selfContext = Value::none();
                    frame->classContext = Value::none();

                    frameCount--;
                    if (frameCount <= targetFrameDepth) return res;
                    
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    
                    if (isInit) getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                    else getReg(targetReg) = res;
                } else {
                    frame = &frames[frameCount - 1];
                    chunk = frame->chunk;
                    code = chunk->code.data();
                    frameRegs = &registers[frame->registerBase];
                    ip = frame->ip;
                }
                break;
            }
            case OpCode::RETURN: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value res = std::move(getReg(a));
            
                runDefersDownTo(frame->deferBase);
            
                while (!exceptionHandlers.empty() && exceptionHandlers.back().frameIndex >= frameCount - 1) {
                    exceptionHandlers.pop_back();
                }
                
                closeUpvalues(frame->registerBase);
                
                int targetReg = frame->returnRegister;
                bool isInit = (frame->function && frame->function->name == "init");
                Value selfCtx = frame->selfContext;

                profileFrameEnd(frame);

                int clearBase = frame->registerBase;
                int clearCount = frame->function->localCount + frame->function->refCount;
                for (int i = 0; i < clearCount; ++i) {
                    registers[clearBase + i] = Value::none();
                }
                frame->jitReturnSlot = Value::none();
                frame->selfContext = Value::none();
                frame->classContext = Value::none();

                frameCount--;
                if (frameCount <= targetFrameDepth) {
                    return res;
                }
                
                // 恢复调用方帧状态
                frame = &frames[frameCount - 1];
                chunk = frame->chunk;
                code = chunk->code.data();
                frameRegs = &registers[frame->registerBase];
                ip = frame->ip;
                
                if (isInit) {
                    getReg(targetReg) = selfCtx.isNone() ? res : selfCtx;
                } else {
                    getReg(targetReg) = res;
                }
                break;
            }
                    default:
                        throw std::runtime_error("VM Error: Unimplemented opcode " + std::to_string(static_cast<int>(op)));
                }
            }
        } catch (const EngineInterruptError&) {
            throw;
        } catch (const ValueException& ex) {
            frame->ip = ip;
            Value errVal = wrapException("Exception", ex.val);
            if (!handleExceptionUnwind(&errVal)) {
                throw ValueException(errVal);
            }
            frame = &frames[frameCount - 1];
            chunk = frame->chunk;
            code = chunk->code.data();
            frameRegs = &registers[frame->registerBase];
            ip = frame->ip;
        } catch (const RuntimeError& ex) {
            frame->ip = ip;
            Value errVal = wrapException(ex.type, ex.message);
            if (!handleExceptionUnwind(&errVal)) {
                throw ValueException(errVal);
            }
            frame = &frames[frameCount - 1];
            chunk = frame->chunk;
            code = chunk->code.data();
            frameRegs = &registers[frame->registerBase];
            ip = frame->ip;
        } catch (const std::exception& ex) {
            frame->ip = ip;
            std::string msg = ex.what();
            std::string type = "Exception";
            size_t colonPos = msg.find(": ");
            if (colonPos != std::string::npos) {
                std::string prefix = msg.substr(0, colonPos);
                if (prefix.find(' ') == std::string::npos) {
                    type = prefix;
                    msg = msg.substr(colonPos + 2);
                } else if (prefix == "VM Error" || prefix == "Runtime Error" || prefix == "Type Error" || prefix == "Math Error" || prefix == "IO Error" || prefix == "Syntax Error") {
                    type = prefix;
                    type.erase(std::remove(type.begin(), type.end(), ' '), type.end());
                    msg = msg.substr(colonPos + 2);
                }
            }
            Value errVal = wrapException(type, Value(msg));
            if (!handleExceptionUnwind(&errVal)) {
                throw ValueException(errVal);
            }
            frame = &frames[frameCount - 1];
            chunk = frame->chunk;
            code = chunk->code.data();
            frameRegs = &registers[frame->registerBase];
            ip = frame->ip;
        } catch (...) {
            frame->ip = ip;
            Value errVal = wrapException("Exception", Value("Unknown VM Error"));
            if (!handleExceptionUnwind(&errVal)) {
                throw ValueException(errVal);
            }
            frame = &frames[frameCount - 1];
            chunk = frame->chunk;
            code = chunk->code.data();
            frameRegs = &registers[frame->registerBase];
            ip = frame->ip;
        }
    }
    #undef getReg
}

// ============================================================================
// JIT Runtime Helpers (Step 84)
// ============================================================================

uint64_t jc2_jit_build_list(uint64_t* values, uint32_t count) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    ObjList* list = GcHeap::get().allocate<ObjList>();
    Value res(list);
    GcValueGuard guard(res);
    list->vec.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        list->vec.push_back(Value::fromRawBits(values[i]));
    }
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_build_dict(uint64_t* values, uint32_t count) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    ObjDict* dict = GcHeap::get().allocate<ObjDict>();
    Value res(dict);
    GcValueGuard guard(res);
    dict->elements.reserve(count);
    dict->keyMap.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        dict->set(Value::fromRawBits(values[i * 2]), Value::fromRawBits(values[i * 2 + 1]));
    }
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_build_set(uint64_t* values, uint32_t count) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    ObjSet* set = GcHeap::get().allocate<ObjSet>();
    Value res(set);
    GcValueGuard guard(res);
    set->elements.reserve(count);
    set->keys.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        set->add(Value::fromRawBits(values[i]));
    }
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_build_matrix(uint64_t* values, int total, uint32_t shapeIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    const auto& shape = chunk->matrixShapes[shapeIdx];
    uint16_t rows = shape.rows;
    const std::vector<uint16_t>& rowCols = shape.rowCols;

    std::vector<Value> vals(total);
    for (int _i = 0; _i < static_cast<int>(total); ++_i) vals[_i] = Value::fromRawBits(values[_i]);

    bool hasComplex = false;
    bool hasSymbolic = false;
    bool hasOther = false;

    auto canBeMatrixElement = [](const Value& v) -> bool {
        return v.isNumber() || v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION) ||
            v.isObjType(ObjType::COMPLEX) ||
            v.isObjType(ObjType::SYMBOLIC) ||
            v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || 
            v.isObjType(ObjType::SYM_MATRIX);
    };

    for (int ii = 0; ii < total; ++ii) {
        const Value& v = vals[ii];
        if (v.isObjType(ObjType::COMPLEX) || v.isObjType(ObjType::COMPLEX_MATRIX)) hasComplex = true;
        if (v.isSymbolic() || v.isObjType(ObjType::SYM_MATRIX)) hasSymbolic = true;
        if (!canBeMatrixElement(v)) {
            hasOther = true;
        } else if (v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION)) {
            try { v.asDouble(); } catch (...) { 
                if (!hasSymbolic) hasOther = true; 
            }
        }
    }

    Value result;

    if (hasOther) {
        throw std::runtime_error("VM Error: Matrix elements must be numeric, complex, or symbolic. Use @[...] for lists.");
    } else {
        bool hasSubMatrix = false;
        for (int ii = 0; ii < total; ++ii) {
            const Value& v = vals[ii];
            if (v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) || v.isObjType(ObjType::SYM_MATRIX)) hasSubMatrix = true;
        }

        if (hasSubMatrix) {
            auto extractCell = [&](Value& cell) {
                if (!cell.isObjType(ObjType::REAL_MATRIX) && !cell.isObjType(ObjType::COMPLEX_MATRIX) && !cell.isObjType(ObjType::SYM_MATRIX)) {
                    if (hasSymbolic) {
                        cell = Value(SymMatrix(1, 1, { cell.asSymbolic() }));
                    } else if (hasComplex) {
                        cell = Value(ComplexMatrix(1, 1, { cell.asComplex() }));
                    } else {
                        cell = Value(RealMatrix(1, 1, { cell.asDouble() }));
                    }
                }
                if (hasSymbolic) {
                    if (cell.isObjType(ObjType::REAL_MATRIX) || cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                        cell = Value(cell.asSymMatrix());
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
                        Value cell = vals[idx++];
                        extractCell(cell);
                        if (rowResult.isNone()) {
                            rowResult = cell;
                        } else {
                            if (hasSymbolic) rowResult = Value(static_cast<ObjSymMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjSymMatrix*>(cell.asObj())->mat));
                            else if (hasComplex) rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                            else rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                        }
                    }
                    if (matResult.isNone()) {
                        matResult = rowResult;
                    } else {
                        if (hasSymbolic) matResult = Value(static_cast<ObjSymMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjSymMatrix*>(rowResult.asObj())->mat));
                        else if (hasComplex) matResult = Value(static_cast<ObjComplexMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat));
                        else matResult = Value(static_cast<ObjRealMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat));
                    }
                }
                result = matResult;
            } catch (...) {
                throw std::runtime_error("VM Error: Dimension mismatch during block matrix concatenation.");
            }
        } else {
            int expectedCols = rows > 0 ? rowCols[0] : 0;
            bool uniformCols = true;
            for (int i = 1; i < rows; ++i) {
                if (rowCols[i] != expectedCols) { uniformCols = false; break; }
            }
            if (!uniformCols) throw std::runtime_error("VM Error: Matrix rows must have the same number of columns.");

            if (hasSymbolic) {
                std::vector<SymExpr> flat(total);
                for (int ii = 0; ii < total; ++ii) flat[ii] = vals[ii].asSymbolic();
                result = Value(SymMatrix(rows, expectedCols, flat));
            } else if (hasComplex) {
                std::vector<Complex> flat(total);
                for (int ii = 0; ii < total; ++ii) flat[ii] = vals[ii].asComplex();
                result = Value(ComplexMatrix(rows, expectedCols, flat));
            } else {
                std::vector<double> flat(total);
                for (int ii = 0; ii < total; ++ii) flat[ii] = vals[ii].asDouble();
                result = Value(RealMatrix(rows, expectedCols, flat));
            }
        }
    }
    vm->getCurrentFrame()->jitReturnSlot = result;
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_build_slice(uint64_t start_bits, uint64_t stop_bits, uint64_t step_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    ObjSlice* slice = GcHeap::get().allocate<ObjSlice>();
    Value res(slice);
    GcValueGuard guard(res);
    
    auto readInt = [&](const Value& v) -> int {
        if (v.isNone()) return ObjSlice::SLICE_NONE;
        int64_t val64 = 0;
        if (v.isInt32()) {
            val64 = v.asInt32();
        } else if (v.isDouble()) {
            val64 = static_cast<int64_t>(std::round(v.asDoubleRaw()));
        } else if (v.isBigInt()) {
            try {
                val64 = v.asBigInt().toInt64();
            } catch (...) {
                throw std::runtime_error("Value Error: slice absolute value exceeds 2^31-1.");
            }
        } else {
            val64 = static_cast<int64_t>(std::round(v.asDouble()));
        }
        if (val64 > 2147483647LL || val64 < -2147483647LL) {
            throw std::runtime_error("Value Error: slice absolute value exceeds 2^31-1.");
        }
        return static_cast<int>(val64);
    };
    
    slice->start = readInt(Value::fromRawBits(start_bits));
    slice->end = readInt(Value::fromRawBits(stop_bits));
    slice->step = readInt(Value::fromRawBits(step_bits));
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_build_class(uint32_t nameIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    const std::string& name = chunk->constants[nameIdx].asString();
    auto cls = GcHeap::get().allocate<ObjClass>();
    cls->name = name;
    Value res(cls);
    VM::activeVM->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_dict_init() {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    ObjDict* dict = GcHeap::get().allocate<ObjDict>();
    Value res(dict);
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

void jc2_jit_dict_append(uint64_t dict_bits, uint64_t key_bits, uint64_t val_bits) {
    JIT_CALLOUT_TRY
    
    Value dictVal = Value::fromRawBits(dict_bits);
    Value keyVal = Value::fromRawBits(key_bits);
    Value valVal = Value::fromRawBits(val_bits);
    
    if (dictVal.isObjType(ObjType::DICT)) {
        static_cast<ObjDict*>(dictVal.asObj())->set(keyVal, valVal);
    } else {
        throw std::runtime_error("VM Error: DICT_APPEND target is not a dict.");
    }
    JIT_CALLOUT_CATCH_VOID
}

uint64_t jc2_jit_get_ref_param(uint32_t bx) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();
    if (frame->refParamsBase == -1) throw std::runtime_error("VM Error: Invalid ref param index.");
    Value res = *(static_cast<ObjUpVal*>(vm->getRegisters()[frame->refParamsBase + bx].asObj())->location);
    frame->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

void jc2_jit_set_ref_param(uint32_t bx, uint64_t val_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();
    if (frame->refParamsBase == -1) throw std::runtime_error("VM Error: Invalid ref param index.");
    Value val = Value::fromRawBits(val_bits);
    *(static_cast<ObjUpVal*>(vm->getRegisters()[frame->refParamsBase + bx].asObj())->location) = val;
    JIT_CALLOUT_CATCH_VOID
}

void jc2_jit_set_global(uint32_t icIdx, uint64_t val_bits, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
    const std::string& name = chunk->constants[ic.nameIdx].asString();
    Value val = Value::fromRawBits(val_bits);
    
    if (name == "<class>") throw std::runtime_error("Syntax Error: cannot override context keyword 'class'.");
    if (name == "<namespace>") throw std::runtime_error("Syntax Error: cannot override context keyword 'namespace'.");
    
    vm->setGlobal(name, val);
    JIT_CALLOUT_CATCH_VOID
}

uint64_t jc2_jit_get_global(uint32_t icIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
    const std::string& name = chunk->constants[ic.nameIdx].asString();
    
    if (name == "<class>") {
        ic.cachedGlobalSlot = -2;
        Value ctx = vm->getCurrentFrame()->classContext;
        if (ctx.isNone()) throw std::runtime_error("VM Error: 'class' accessed outside of context.");
        vm->getCurrentFrame()->jitReturnSlot = ctx;
        return ctx.as_bits;
    }
    
    Value val = vm->getGlobal(name);
    if (!val.isNone()) {
        vm->getCurrentFrame()->jitReturnSlot = val;
        return val.as_bits;
    }
    
    Value builtinVal = vm->getBuiltinValue(name);
    if (builtinVal.isNone()) builtinVal = vm->getBuiltinClosure(name);
    if (!builtinVal.isNone()) {
        vm->setGlobal(name, builtinVal);
        vm->getCurrentFrame()->jitReturnSlot = builtinVal;
        return builtinVal.as_bits;
    }
    
    throw std::runtime_error("VM Error: Undefined global variable '" + name + "'.");
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_build_namespace(uint64_t* values, uint32_t count, uint32_t nameIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();
    int base = frame->registerBase;
    const std::string& nsName = chunk->constants[nameIdx].asString();
    ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
    Value res(ns);
    GcValueGuard guard(res);
    ns->name = nsName;
    
    for (uint32_t i = 0; i < count; ++i) {
        Value keyVal = Value::fromRawBits(values[i * 3]);
        Value slotVal = Value::fromRawBits(values[i * 3 + 1]);
        Value isConstVal = Value::fromRawBits(values[i * 3 + 2]);
        
        std::string key = keyVal.asString();
        int slot = static_cast<int>(slotVal.asDouble());
        bool isConst = isConstVal.truthy();
        
        ObjUpVal* upval = vm->captureUpvaluePublic(base + slot);
        ns->fields[key] = { upval, isConst };
    }
    frame->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_concat_strings(uint64_t* values, uint32_t count) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    bool allStrings = true;
    size_t totalLen = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Value v = Value::fromRawBits(values[i]);
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
        for (uint32_t i = 0; i < count; ++i) {
            result += Value::fromRawBits(values[i]).asString();
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            Value v = Value::fromRawBits(values[i]);
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
    Value res(result);
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_format_string(uint64_t val_bits, uint32_t specIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value val = Value::fromRawBits(val_bits);
    const std::string& spec = chunk->constants[specIdx].asString();

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
    Value res(result);
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_get_prop(uint64_t obj_bits, uint32_t icIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    
    Value obj = Value::fromRawBits(obj_bits);
    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
    Value keyVal = chunk->constants[ic.nameIdx];
    
    if (obj.isInstance()) {
        auto inst = obj.asInstance();
        auto it = inst->properties.find(keyVal.asString());
        if (it != inst->properties.end() && !it->second.is_local) {
            vm->getCurrentFrame()->jitReturnSlot = it->second.val;
            return it->second.val.as_bits;
        }
    } else if (obj.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(obj.asObj());
        if (ic.cachedBuiltinType == BuiltinType::DICT && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(d->elements.size())) {
            if (d->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                vm->getCurrentFrame()->jitReturnSlot = d->elements[ic.cachedFieldIndex].second;
                return d->elements[ic.cachedFieldIndex].second.as_bits;
            }
        }
        auto it = d->keyMap.find(keyVal);
        if (it != d->keyMap.end()) {
            ic.cachedBuiltinType = BuiltinType::DICT;
            ic.cachedFieldIndex = static_cast<int>(it->second);
            vm->getCurrentFrame()->jitReturnSlot = d->elements[it->second].second;
            return d->elements[it->second].second.as_bits;
        }
    }

    const std::string& field = keyVal.asString();
    bool found = false;
    Value result;
    
    BuiltinType objBt = BuiltinType::UNKNOWN;
    if (obj.isObjType(ObjType::LIST)) objBt = BuiltinType::LIST;
    else if (obj.isObjType(ObjType::DICT)) objBt = BuiltinType::DICT;
    else if (obj.isObjType(ObjType::SET)) objBt = BuiltinType::SET;
    else if (obj.isString()) objBt = BuiltinType::STRING;
    else if (obj.isObjType(ObjType::REAL_MATRIX)) objBt = BuiltinType::REALMAT;
    else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) objBt = BuiltinType::COMPLEXMAT;
    else if (obj.isObjType(ObjType::SYM_MATRIX)) objBt = BuiltinType::SYMMAT;

    if (objBt != BuiltinType::UNKNOWN && ic.cachedBuiltinType == objBt && ic.cachedMethod) {
        auto rawMethod = ic.cachedMethod;
        auto bound = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
        );
        bound->paramNames = rawMethod->paramNames;
        bound->isRef = rawMethod->isRef;
        bound->defaultValues = rawMethod->defaultValues;
        bound->restName = rawMethod->restName;
        bound->compiledFnIndex = rawMethod->compiledFnIndex;
        if (rawMethod->upvalueCount > 0) {
            bound->upvalueCount = rawMethod->upvalueCount;
            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
            for (int i = 0; i < bound->upvalueCount; ++i) {
                bound->upvalues[i] = rawMethod->upvalues[i];
            }
        }
        if (rawMethod->paramTypesCount > 0) {
            bound->paramTypesCount = rawMethod->paramTypesCount;
            bound->paramTypes = new Value[bound->paramTypesCount];
            for (int i = 0; i < bound->paramTypesCount; ++i) {
                bound->paramTypes[i] = rawMethod->paramTypes[i];
            }
        }
        bound->returnType = rawMethod->returnType;
        bound->nativeFn = rawMethod->nativeFn;
        bound->boundSelf = obj;
        bound->boundClass = Value(ic.cachedClass);
        result = Value(bound);
        found = true;
    } else if (obj.isInstance()) {
        auto inst = obj.asInstance();
        if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
            auto rawMethod = ic.cachedMethod;
            auto bound = GcHeap::get().allocate<ObjClosure>(
                std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
            );
            bound->paramNames = rawMethod->paramNames;
            bound->isRef = rawMethod->isRef;
            bound->defaultValues = rawMethod->defaultValues;
            bound->restName = rawMethod->restName;
            bound->compiledFnIndex = rawMethod->compiledFnIndex;
            if (rawMethod->upvalueCount > 0) {
                bound->upvalueCount = rawMethod->upvalueCount;
                bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                for (int i = 0; i < bound->upvalueCount; ++i) {
                    bound->upvalues[i] = rawMethod->upvalues[i];
                }
            }
            if (rawMethod->paramTypesCount > 0) {
                bound->paramTypesCount = rawMethod->paramTypesCount;
                bound->paramTypes = new Value[bound->paramTypesCount];
                for (int i = 0; i < bound->paramTypesCount; ++i) {
                    bound->paramTypes[i] = rawMethod->paramTypes[i];
                }
            }
            bound->returnType = rawMethod->returnType;
            bound->nativeFn = rawMethod->nativeFn;
            bound->boundSelf = Value(inst);
            bound->boundClass = Value(ic.cachedClass);
            result = Value(bound);
            found = true;
        }
        if (!found) {
            auto cls = inst->classDef;
            while (cls) {
                auto it = cls->properties.find(field);
                if (it != cls->properties.end() && !it->second.is_local && it->second.val.isFunctionClosure()) {
                    auto rawMethod = it->second.val.asFunction();
                    ic.cachedClassId = inst->classDef->classId;
                    ic.cachedMethod = rawMethod;
                    ic.cachedClass = cls;
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->restName = rawMethod->restName;
                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    if (rawMethod->paramTypesCount > 0) {
                        bound->paramTypesCount = rawMethod->paramTypesCount;
                        bound->paramTypes = new Value[bound->paramTypesCount];
                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                        }
                    }
                    bound->returnType = rawMethod->returnType;
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
                auto [getattrMethod, owner] = vm->findDunder(obj, "__getattr__");
                if (getattrMethod) {
                    try {
                        result = vm->callDunder(obj, getattrMethod, owner, {Value(field)});
                        found = true;
                    } catch (...) {
                        found = false;
                    }
                }
            }
        }
    } else if (!found) {
        ObjClass* nativeProto = nullptr;
        if (objBt == BuiltinType::LIST) nativeProto = vm->listProto;
        else if (objBt == BuiltinType::DICT) nativeProto = vm->dictProto;
        else if (objBt == BuiltinType::SET) nativeProto = vm->setProto;
        else if (objBt == BuiltinType::STRING) nativeProto = vm->stringProto;
        else if (objBt == BuiltinType::REALMAT || objBt == BuiltinType::COMPLEXMAT || objBt == BuiltinType::SYMMAT) nativeProto = vm->matrixProto;

        if (nativeProto) {
            auto it = nativeProto->properties.find(field);
            if (it != nativeProto->properties.end() && !it->second.is_local) {
                if (it->second.val.isFunctionClosure()) {
                    auto rawMethod = it->second.val.asFunction();
                    ic.cachedBuiltinType = objBt;
                    ic.cachedMethod = rawMethod;
                    ic.cachedClass = nativeProto;
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->restName = rawMethod->restName;
                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    if (rawMethod->paramTypesCount > 0) {
                        bound->paramTypesCount = rawMethod->paramTypesCount;
                        bound->paramTypes = new Value[bound->paramTypesCount];
                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                        }
                    }
                    bound->returnType = rawMethod->returnType;
                    bound->nativeFn = rawMethod->nativeFn;
                    bound->boundSelf = obj;
                    bound->boundClass = Value(nativeProto);
                    result = Value(bound);
                } else {
                    result = it->second.val;
                }
                found = true;
            }
        }
    }
    
    if (!found && obj.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(obj.asObj());
        auto it = ns->fields.find(field);
        if (it != ns->fields.end()) {
            result = *(it->second.upval->location);
            found = true;
        }
    } else if (!found && obj.isSlice()) {
        Value prop = obj.asSlice()->getProperty(field);
        if (!prop.isUninit()) {
            result = prop;
            found = true;
        }
    } else if (!found && obj.isClass()) {
        auto cls = static_cast<ObjClass*>(obj.asObj());
        while (cls) {
            auto it = cls->properties.find(field);
            if (it != cls->properties.end() && !it->second.is_local) {
                if (it->second.val.isFunctionClosure()) {
                    auto rawMethod = it->second.val.asFunction();
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->restName = rawMethod->restName;
                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    if (rawMethod->paramTypesCount > 0) {
                        bound->paramTypesCount = rawMethod->paramTypesCount;
                        bound->paramTypes = new Value[bound->paramTypesCount];
                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                        }
                    }
                    bound->returnType = rawMethod->returnType;
                    bound->nativeFn = rawMethod->nativeFn;
                    bound->boundSelf = Value::none();
                    bound->boundClass = Value(cls);
                    result = Value(bound);
                } else {
                    result = it->second.val;
                }
                found = true;
                break;
            }
            cls = cls->parent;
        }
    }
    
    if (!found) {
        if (ic.cachedGlobalSlot == -4) {
            auto bound = GcHeap::get().allocate<ObjClosure>(
                std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
            );
            bound->boundSelf = obj;
            
            Value builtinClosureVal = vm->getBuiltinClosure(field);
            ObjClosure* targetFn = builtinClosureVal.asFunction();
            bound->paramNames = targetFn->paramNames;
            bound->isRef = targetFn->isRef;
            bound->defaultValues = targetFn->defaultValues;
            bound->restName = targetFn->restName;
            bound->isUFCS = true;

            bound->nativeFn = ic.cachedNativeFn;
            result = Value(bound);
            found = true;
        } else {
            if (ic.cachedGlobalSlot >= 0) {
                Value gVal = vm->getGlobal(field);
                if (gVal.isFunctionClosure()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    ObjClosure* targetFn = gVal.asFunction();
                
                    bound->restName = targetFn->restName;
                    bound->paramNames = targetFn->paramNames;
                    bound->isRef = targetFn->isRef;
                    bound->defaultValues = targetFn->defaultValues;
                    bound->isUFCS = true;

                    if (targetFn->isBytecode()) {
                        bound->compiledFnIndex = targetFn->compiledFnIndex;
                        if (targetFn->upvalueCount > 0) {
                            bound->upvalueCount = targetFn->upvalueCount;
                            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                            for (int i = 0; i < bound->upvalueCount; ++i) {
                                bound->upvalues[i] = targetFn->upvalues[i];
                            }
                        }
                        if (targetFn->paramTypesCount > 0) {
                            bound->paramTypesCount = targetFn->paramTypesCount;
                            bound->paramTypes = new Value[bound->paramTypesCount];
                            for (int i = 0; i < bound->paramTypesCount; ++i) {
                                bound->paramTypes[i] = targetFn->paramTypes[i];
                            }
                        }
                        bound->returnType = targetFn->returnType;
                    } else {
                        bound->boundClass = targetFn->boundClass;
                    }
                    bound->nativeFn = targetFn->nativeFn;
                    result = Value(bound);
                    found = true;
                } else if (gVal.isType() || gVal.isClass()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    bound->isUFCS = true;
                    bound->nativeFn = std::make_any<NativeCallable>(
                        [gVal](const std::vector<Value>& args) -> Value {
                            Value capturedObj = helpers::nativeSelfStack.back();
                            std::vector<Value> fullArgs;
                            fullArgs.reserve(args.size() + 1);
                            fullArgs.push_back(capturedObj);
                            fullArgs.insert(fullArgs.end(), args.begin(), args.end());
                            
                            if (gVal.isType()) {
                                ObjTypeDef* td = static_cast<ObjTypeDef*>(gVal.asObj());
                                if (td->converter) {
                                    return td->converter(fullArgs);
                                }
                                if (td->types.size() == 1 && std::holds_alternative<BuiltinType>(td->types[0])) {
                                    BuiltinType bt = std::get<BuiltinType>(td->types[0]);
                                    if (bt == BuiltinType::TYPE_DEF) {
                                        if (fullArgs.size() != 1) throw std::runtime_error("TypeError: type() expects 1 argument.");
                                        Value v = fullArgs[0];
                                        ObjTypeDef* resTd = GcHeap::get().allocate<ObjTypeDef>();
                                        if (v.isType()) resTd->types.push_back(BuiltinType::TYPE_DEF);
                                        else if (v.isClass()) resTd->types.push_back(BuiltinType::CLASS);
                                        else if (v.isInstance()) resTd->types.push_back(v.asInstance()->classDef);
                                        else {
                                            BuiltinType vbt = BuiltinType::ANY;
                                            if (v.isInt32() || v.isBigInt()) vbt = BuiltinType::INT;
                                            else if (v.isDouble()) vbt = BuiltinType::FLOAT;
                                            else if (v.isString()) vbt = BuiltinType::STRING;
                                            else if (v.isBool()) vbt = BuiltinType::BOOL;
                                            else if (v.isNone()) vbt = BuiltinType::NONE_TYPE;
                                            else if (v.isObjType(ObjType::LIST)) vbt = BuiltinType::LIST;
                                            else if (v.isObjType(ObjType::DICT)) vbt = BuiltinType::DICT;
                                            else if (v.isObjType(ObjType::SET)) vbt = BuiltinType::SET;
                                            else if (v.isObjType(ObjType::FRACTION)) vbt = BuiltinType::FRACTION;
                                            else if (v.isObjType(ObjType::COMPLEX)) vbt = BuiltinType::COMPLEX;
                                            else if (v.isObjType(ObjType::SYMBOLIC)) vbt = BuiltinType::SYMBOLIC;
                                            else if (v.isObjType(ObjType::REAL_MATRIX)) vbt = BuiltinType::REALMAT;
                                            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) vbt = BuiltinType::COMPLEXMAT;
                                            else if (v.isObjType(ObjType::SYM_MATRIX)) vbt = BuiltinType::SYMMAT;
                                            else if (v.isFunctionClosure()) vbt = BuiltinType::FUNC;
                                            else if (v.isObjType(ObjType::NAMESPACE)) vbt = BuiltinType::NAMESPACE;
                                            else if (v.isObjType(ObjType::SLICE)) vbt = BuiltinType::SLICE;
                                            resTd->types.push_back(vbt);
                                        }
                                        resTd->normalize();
                                        return Value(resTd);
                                    }
                                }
                                throw std::runtime_error("TypeError: This type object is not callable.");
                            } else {
                                auto cls = static_cast<ObjClass*>(gVal.asObj());
                                if (cls->native_allocator) {
                                    return cls->native_allocator(fullArgs);
                                }
                                auto instance = GcHeap::get().allocate<ObjInstance>();
                                Value res(instance);
                                GcValueGuard guard(res);
                                instance->classDef = cls;
                                
                                ObjClosure* initMethod = nullptr;
                                auto c = cls;
                                while (c) {
                                    auto it = c->properties.find("<init>");
                                    if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                                        initMethod = it->second.val.asFunction();
                                        break;
                                    }
                                    c = c->parent;
                                }
                                
                                if (initMethod) {
                                    if (initMethod->isBytecode()) {
                                        VM::activeVM->callVMFunction(initMethod->compiledFnIndex, fullArgs, initMethod, res, Value(cls));
                                    } else if (initMethod->isNative()) {
                                        helpers::nativeSelfStack.push_back(res);
                                        helpers::nativeClassStack.push_back(Value(cls));
                                        auto& fn = std::any_cast<NativeCallable&>(initMethod->nativeFn);
                                        fn(fullArgs);
                                        helpers::nativeSelfStack.pop_back();
                                        helpers::nativeClassStack.pop_back();
                                    }
                                }
                                return res;
                            }
                        }
                    );
                    result = Value(bound);
                    found = true;
                }
            } else {
                Value gVal = vm->getGlobal(field);
                if (gVal.isFunctionClosure()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    ObjClosure* targetFn = gVal.asFunction();
                
                    bound->restName = targetFn->restName;
                    bound->paramNames = targetFn->paramNames;
                    bound->isRef = targetFn->isRef;
                    bound->defaultValues = targetFn->defaultValues;
                    bound->isUFCS = true;

                    if (targetFn->isBytecode()) {
                        bound->compiledFnIndex = targetFn->compiledFnIndex;
                        if (targetFn->upvalueCount > 0) {
                            bound->upvalueCount = targetFn->upvalueCount;
                            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                            for (int i = 0; i < bound->upvalueCount; ++i) {
                                bound->upvalues[i] = targetFn->upvalues[i];
                            }
                        }
                        if (targetFn->paramTypesCount > 0) {
                            bound->paramTypesCount = targetFn->paramTypesCount;
                            bound->paramTypes = new Value[bound->paramTypesCount];
                            for (int i = 0; i < bound->paramTypesCount; ++i) {
                                bound->paramTypes[i] = targetFn->paramTypes[i];
                            }
                        }
                        bound->returnType = targetFn->returnType;
                    } else {
                        bound->boundClass = targetFn->boundClass;
                    }
                    bound->nativeFn = targetFn->nativeFn;
                    result = Value(bound);
                    found = true;
                } else if (gVal.isType() || gVal.isClass()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    bound->isUFCS = true;
                    bound->nativeFn = std::make_any<NativeCallable>(
                        [gVal](const std::vector<Value>& args) -> Value {
                            Value capturedObj = helpers::nativeSelfStack.back();
                            std::vector<Value> fullArgs;
                            fullArgs.reserve(args.size() + 1);
                            fullArgs.push_back(capturedObj);
                            fullArgs.insert(fullArgs.end(), args.begin(), args.end());
                            
                            if (gVal.isType()) {
                                ObjTypeDef* td = static_cast<ObjTypeDef*>(gVal.asObj());
                                if (td->converter) {
                                    return td->converter(fullArgs);
                                }
                                if (td->types.size() == 1 && std::holds_alternative<BuiltinType>(td->types[0])) {
                                    BuiltinType bt = std::get<BuiltinType>(td->types[0]);
                                    if (bt == BuiltinType::TYPE_DEF) {
                                        if (fullArgs.size() != 1) throw std::runtime_error("TypeError: type() expects 1 argument.");
                                        Value v = fullArgs[0];
                                        ObjTypeDef* resTd = GcHeap::get().allocate<ObjTypeDef>();
                                        if (v.isType()) resTd->types.push_back(BuiltinType::TYPE_DEF);
                                        else if (v.isClass()) resTd->types.push_back(BuiltinType::CLASS);
                                        else if (v.isInstance()) resTd->types.push_back(v.asInstance()->classDef);
                                        else {
                                            BuiltinType vbt = BuiltinType::ANY;
                                            if (v.isInt32() || v.isBigInt()) vbt = BuiltinType::INT;
                                            else if (v.isDouble()) vbt = BuiltinType::FLOAT;
                                            else if (v.isString()) vbt = BuiltinType::STRING;
                                            else if (v.isBool()) vbt = BuiltinType::BOOL;
                                            else if (v.isNone()) vbt = BuiltinType::NONE_TYPE;
                                            else if (v.isObjType(ObjType::LIST)) vbt = BuiltinType::LIST;
                                            else if (v.isObjType(ObjType::DICT)) vbt = BuiltinType::DICT;
                                            else if (v.isObjType(ObjType::SET)) vbt = BuiltinType::SET;
                                            else if (v.isObjType(ObjType::FRACTION)) vbt = BuiltinType::FRACTION;
                                            else if (v.isObjType(ObjType::COMPLEX)) vbt = BuiltinType::COMPLEX;
                                            else if (v.isObjType(ObjType::SYMBOLIC)) vbt = BuiltinType::SYMBOLIC;
                                            else if (v.isObjType(ObjType::REAL_MATRIX)) vbt = BuiltinType::REALMAT;
                                            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) vbt = BuiltinType::COMPLEXMAT;
                                            else if (v.isObjType(ObjType::SYM_MATRIX)) vbt = BuiltinType::SYMMAT;
                                            else if (v.isFunctionClosure()) vbt = BuiltinType::FUNC;
                                            else if (v.isObjType(ObjType::NAMESPACE)) vbt = BuiltinType::NAMESPACE;
                                            else if (v.isObjType(ObjType::SLICE)) vbt = BuiltinType::SLICE;
                                            resTd->types.push_back(vbt);
                                        }
                                        resTd->normalize();
                                        return Value(resTd);
                                    }
                                }
                                throw std::runtime_error("TypeError: This type object is not callable.");
                            } else {
                                auto cls = static_cast<ObjClass*>(gVal.asObj());
                                if (cls->native_allocator) {
                                    return cls->native_allocator(fullArgs);
                                }
                                auto instance = GcHeap::get().allocate<ObjInstance>();
                                Value res(instance);
                                GcValueGuard guard(res);
                                instance->classDef = cls;
                                
                                ObjClosure* initMethod = nullptr;
                                auto c = cls;
                                while (c) {
                                    auto it = c->properties.find("<init>");
                                    if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                                        initMethod = it->second.val.asFunction();
                                        break;
                                    }
                                    c = c->parent;
                                }
                                
                                if (initMethod) {
                                    if (initMethod->isBytecode()) {
                                        VM::activeVM->callVMFunction(initMethod->compiledFnIndex, fullArgs, initMethod, res, Value(cls));
                                    } else if (initMethod->isNative()) {
                                        helpers::nativeSelfStack.push_back(res);
                                        helpers::nativeClassStack.push_back(Value(cls));
                                        auto& fn = std::any_cast<NativeCallable&>(initMethod->nativeFn);
                                        fn(fullArgs);
                                        helpers::nativeSelfStack.pop_back();
                                        helpers::nativeClassStack.pop_back();
                                    }
                                }
                                return res;
                            }
                        }
                    );
                    result = Value(bound);
                    found = true;
                }
            }

            if (!found) {
                const auto& nativeBuiltins = vm->getNativeBuiltins();
                auto nIt = nativeBuiltins.find(field);
                if (nIt != nativeBuiltins.end()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    
                    Value builtinClosureVal = vm->getBuiltinClosure(field);
                    ObjClosure* targetFn = builtinClosureVal.asFunction();
                    bound->paramNames = targetFn->paramNames;
                    bound->isRef = targetFn->isRef;
                    bound->defaultValues = targetFn->defaultValues;
                    bound->restName = targetFn->restName;
                    bound->isUFCS = true;

                    NativeCallable nativeFn = nIt->second;
                    
                    const auto& builtinArity = vm->getBuiltinArity();
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
                    
                    ic.cachedGlobalSlot = -4;
                    ic.cachedNativeFn = bound->nativeFn;
                    
                    result = Value(bound);
                    found = true;
                }
            }
        }
    }

    if (!found) {
        throw std::runtime_error("VM Error: Property '" + field + "' not found.");
    }
    
    vm->getCurrentFrame()->jitReturnSlot = result;
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_try_get_prop(uint64_t obj_bits, uint32_t icIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    
    Value obj = Value::fromRawBits(obj_bits);
    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
    Value keyVal = chunk->constants[ic.nameIdx];
    
    if (obj.isInstance()) {
        auto inst = obj.asInstance();
        auto it = inst->properties.find(keyVal.asString());
        if (it != inst->properties.end() && !it->second.is_local) {
            vm->getCurrentFrame()->jitReturnSlot = it->second.val;
            return it->second.val.as_bits;
        }
    } else if (obj.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(obj.asObj());
        if (ic.cachedBuiltinType == BuiltinType::DICT && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(d->elements.size())) {
            if (d->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                vm->getCurrentFrame()->jitReturnSlot = d->elements[ic.cachedFieldIndex].second;
                return d->elements[ic.cachedFieldIndex].second.as_bits;
            }
        }
        auto it = d->keyMap.find(keyVal);
        if (it != d->keyMap.end()) {
            ic.cachedBuiltinType = BuiltinType::DICT;
            ic.cachedFieldIndex = static_cast<int>(it->second);
            vm->getCurrentFrame()->jitReturnSlot = d->elements[it->second].second;
            return d->elements[it->second].second.as_bits;
        }
    }

    const std::string& field = keyVal.asString();
    bool found = false;
    Value result;
    
    BuiltinType objBt = BuiltinType::UNKNOWN;
    if (obj.isObjType(ObjType::LIST)) objBt = BuiltinType::LIST;
    else if (obj.isObjType(ObjType::DICT)) objBt = BuiltinType::DICT;
    else if (obj.isObjType(ObjType::SET)) objBt = BuiltinType::SET;
    else if (obj.isString()) objBt = BuiltinType::STRING;
    else if (obj.isObjType(ObjType::REAL_MATRIX)) objBt = BuiltinType::REALMAT;
    else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) objBt = BuiltinType::COMPLEXMAT;
    else if (obj.isObjType(ObjType::SYM_MATRIX)) objBt = BuiltinType::SYMMAT;

    if (objBt != BuiltinType::UNKNOWN && ic.cachedBuiltinType == objBt && ic.cachedMethod) {
        auto rawMethod = ic.cachedMethod;
        auto bound = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
        );
        bound->paramNames = rawMethod->paramNames;
        bound->isRef = rawMethod->isRef;
        bound->defaultValues = rawMethod->defaultValues;
        bound->restName = rawMethod->restName;
        bound->compiledFnIndex = rawMethod->compiledFnIndex;
        if (rawMethod->upvalueCount > 0) {
            bound->upvalueCount = rawMethod->upvalueCount;
            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
            for (int i = 0; i < bound->upvalueCount; ++i) {
                bound->upvalues[i] = rawMethod->upvalues[i];
            }
        }
        if (rawMethod->paramTypesCount > 0) {
            bound->paramTypesCount = rawMethod->paramTypesCount;
            bound->paramTypes = new Value[bound->paramTypesCount];
            for (int i = 0; i < bound->paramTypesCount; ++i) {
                bound->paramTypes[i] = rawMethod->paramTypes[i];
            }
        }
        bound->returnType = rawMethod->returnType;
        bound->nativeFn = rawMethod->nativeFn;
        bound->boundSelf = obj;
        bound->boundClass = Value(ic.cachedClass);
        result = Value(bound);
        found = true;
    } else if (obj.isInstance()) {
        auto inst = obj.asInstance();
        if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
            auto rawMethod = ic.cachedMethod;
            auto bound = GcHeap::get().allocate<ObjClosure>(
                std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
            );
            bound->paramNames = rawMethod->paramNames;
            bound->isRef = rawMethod->isRef;
            bound->defaultValues = rawMethod->defaultValues;
            bound->restName = rawMethod->restName;
            bound->compiledFnIndex = rawMethod->compiledFnIndex;
            if (rawMethod->upvalueCount > 0) {
                bound->upvalueCount = rawMethod->upvalueCount;
                bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                for (int i = 0; i < bound->upvalueCount; ++i) {
                    bound->upvalues[i] = rawMethod->upvalues[i];
                }
            }
            if (rawMethod->paramTypesCount > 0) {
                bound->paramTypesCount = rawMethod->paramTypesCount;
                bound->paramTypes = new Value[bound->paramTypesCount];
                for (int i = 0; i < bound->paramTypesCount; ++i) {
                    bound->paramTypes[i] = rawMethod->paramTypes[i];
                }
            }
            bound->returnType = rawMethod->returnType;
            bound->nativeFn = rawMethod->nativeFn;
            bound->boundSelf = Value(inst);
            bound->boundClass = Value(ic.cachedClass);
            result = Value(bound);
            found = true;
        }
        if (!found) {
            auto cls = inst->classDef;
            while (cls) {
                auto it = cls->properties.find(field);
                if (it != cls->properties.end() && !it->second.is_local && it->second.val.isFunctionClosure()) {
                    auto rawMethod = it->second.val.asFunction();
                    ic.cachedClassId = inst->classDef->classId;
                    ic.cachedMethod = rawMethod;
                    ic.cachedClass = cls;
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->restName = rawMethod->restName;
                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    if (rawMethod->paramTypesCount > 0) {
                        bound->paramTypesCount = rawMethod->paramTypesCount;
                        bound->paramTypes = new Value[bound->paramTypesCount];
                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                        }
                    }
                    bound->returnType = rawMethod->returnType;
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
                auto [getattrMethod, owner] = vm->findDunder(obj, "__getattr__");
                if (getattrMethod) {
                    try {
                        result = vm->callDunder(obj, getattrMethod, owner, {Value(field)});
                        found = true;
                    } catch (...) {
                        found = false;
                    }
                }
            }
        }
    } else if (!found) {
        ObjClass* nativeProto = nullptr;
        if (objBt == BuiltinType::LIST) nativeProto = vm->listProto;
        else if (objBt == BuiltinType::DICT) nativeProto = vm->dictProto;
        else if (objBt == BuiltinType::SET) nativeProto = vm->setProto;
        else if (objBt == BuiltinType::STRING) nativeProto = vm->stringProto;
        else if (objBt == BuiltinType::REALMAT || objBt == BuiltinType::COMPLEXMAT || objBt == BuiltinType::SYMMAT) nativeProto = vm->matrixProto;

        if (nativeProto) {
            auto it = nativeProto->properties.find(field);
            if (it != nativeProto->properties.end() && !it->second.is_local) {
                if (it->second.val.isFunctionClosure()) {
                    auto rawMethod = it->second.val.asFunction();
                    ic.cachedBuiltinType = objBt;
                    ic.cachedMethod = rawMethod;
                    ic.cachedClass = nativeProto;
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->restName = rawMethod->restName;
                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    if (rawMethod->paramTypesCount > 0) {
                        bound->paramTypesCount = rawMethod->paramTypesCount;
                        bound->paramTypes = new Value[bound->paramTypesCount];
                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                        }
                    }
                    bound->returnType = rawMethod->returnType;
                    bound->nativeFn = rawMethod->nativeFn;
                    bound->boundSelf = obj;
                    bound->boundClass = Value(nativeProto);
                    result = Value(bound);
                } else {
                    result = it->second.val;
                }
                found = true;
            }
        }
    }
    
    if (!found && obj.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(obj.asObj());
        auto it = ns->fields.find(field);
        if (it != ns->fields.end()) {
            result = *(it->second.upval->location);
            found = true;
        }
    } else if (!found && obj.isSlice()) {
        Value prop = obj.asSlice()->getProperty(field);
        if (!prop.isUninit()) {
            result = prop;
            found = true;
        }
    } else if (!found && obj.isClass()) {
        auto cls = static_cast<ObjClass*>(obj.asObj());
        while (cls) {
            auto it = cls->properties.find(field);
            if (it != cls->properties.end() && !it->second.is_local) {
                if (it->second.val.isFunctionClosure()) {
                    auto rawMethod = it->second.val.asFunction();
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->paramNames = rawMethod->paramNames;
                    bound->isRef = rawMethod->isRef;
                    bound->defaultValues = rawMethod->defaultValues;
                    bound->restName = rawMethod->restName;
                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                    if (rawMethod->upvalueCount > 0) {
                        bound->upvalueCount = rawMethod->upvalueCount;
                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                        for (int i = 0; i < bound->upvalueCount; ++i) {
                            bound->upvalues[i] = rawMethod->upvalues[i];
                        }
                    }
                    if (rawMethod->paramTypesCount > 0) {
                        bound->paramTypesCount = rawMethod->paramTypesCount;
                        bound->paramTypes = new Value[bound->paramTypesCount];
                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                        }
                    }
                    bound->returnType = rawMethod->returnType;
                    bound->nativeFn = rawMethod->nativeFn;
                    bound->boundSelf = Value::none();
                    bound->boundClass = Value(cls);
                    result = Value(bound);
                } else {
                    result = it->second.val;
                }
                found = true;
                break;
            }
            cls = cls->parent;
        }
    }
    
    if (!found) {
        if (ic.cachedGlobalSlot == -4) {
            auto bound = GcHeap::get().allocate<ObjClosure>(
                std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
            );
            bound->boundSelf = obj;
            
            Value builtinClosureVal = vm->getBuiltinClosure(field);
            ObjClosure* targetFn = builtinClosureVal.asFunction();
            bound->paramNames = targetFn->paramNames;
            bound->isRef = targetFn->isRef;
            bound->defaultValues = targetFn->defaultValues;
            bound->restName = targetFn->restName;
            bound->isUFCS = true;

            bound->nativeFn = ic.cachedNativeFn;
            result = Value(bound);
            found = true;
        } else {
            if (ic.cachedGlobalSlot >= 0) {
                Value gVal = vm->getGlobal(field);
                if (gVal.isFunctionClosure()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    ObjClosure* targetFn = gVal.asFunction();
                
                    bound->restName = targetFn->restName;
                    bound->paramNames = targetFn->paramNames;
                    bound->isRef = targetFn->isRef;
                    bound->defaultValues = targetFn->defaultValues;
                    bound->isUFCS = true;

                    if (targetFn->isBytecode()) {
                        bound->compiledFnIndex = targetFn->compiledFnIndex;
                        if (targetFn->upvalueCount > 0) {
                            bound->upvalueCount = targetFn->upvalueCount;
                            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                            for (int i = 0; i < bound->upvalueCount; ++i) {
                                bound->upvalues[i] = targetFn->upvalues[i];
                            }
                        }
                        if (targetFn->paramTypesCount > 0) {
                            bound->paramTypesCount = targetFn->paramTypesCount;
                            bound->paramTypes = new Value[bound->paramTypesCount];
                            for (int i = 0; i < bound->paramTypesCount; ++i) {
                                bound->paramTypes[i] = targetFn->paramTypes[i];
                            }
                        }
                        bound->returnType = targetFn->returnType;
                    } else {
                        bound->boundClass = targetFn->boundClass;
                    }
                    bound->nativeFn = targetFn->nativeFn;
                    result = Value(bound);
                    found = true;
                } else if (gVal.isType() || gVal.isClass()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    bound->isUFCS = true;
                    bound->nativeFn = std::make_any<NativeCallable>(
                        [gVal](const std::vector<Value>& args) -> Value {
                            Value capturedObj = helpers::nativeSelfStack.back();
                            std::vector<Value> fullArgs;
                            fullArgs.reserve(args.size() + 1);
                            fullArgs.push_back(capturedObj);
                            fullArgs.insert(fullArgs.end(), args.begin(), args.end());
                            
                            if (gVal.isType()) {
                                ObjTypeDef* td = static_cast<ObjTypeDef*>(gVal.asObj());
                                if (td->converter) {
                                    return td->converter(fullArgs);
                                }
                                if (td->types.size() == 1 && std::holds_alternative<BuiltinType>(td->types[0])) {
                                    BuiltinType bt = std::get<BuiltinType>(td->types[0]);
                                    if (bt == BuiltinType::TYPE_DEF) {
                                        if (fullArgs.size() != 1) throw std::runtime_error("TypeError: type() expects 1 argument.");
                                        Value v = fullArgs[0];
                                        ObjTypeDef* resTd = GcHeap::get().allocate<ObjTypeDef>();
                                        if (v.isType()) resTd->types.push_back(BuiltinType::TYPE_DEF);
                                        else if (v.isClass()) resTd->types.push_back(BuiltinType::CLASS);
                                        else if (v.isInstance()) resTd->types.push_back(v.asInstance()->classDef);
                                        else {
                                            BuiltinType vbt = BuiltinType::ANY;
                                            if (v.isInt32() || v.isBigInt()) vbt = BuiltinType::INT;
                                            else if (v.isDouble()) vbt = BuiltinType::FLOAT;
                                            else if (v.isString()) vbt = BuiltinType::STRING;
                                            else if (v.isBool()) vbt = BuiltinType::BOOL;
                                            else if (v.isNone()) vbt = BuiltinType::NONE_TYPE;
                                            else if (v.isObjType(ObjType::LIST)) vbt = BuiltinType::LIST;
                                            else if (v.isObjType(ObjType::DICT)) vbt = BuiltinType::DICT;
                                            else if (v.isObjType(ObjType::SET)) vbt = BuiltinType::SET;
                                            else if (v.isObjType(ObjType::FRACTION)) vbt = BuiltinType::FRACTION;
                                            else if (v.isObjType(ObjType::COMPLEX)) vbt = BuiltinType::COMPLEX;
                                            else if (v.isObjType(ObjType::SYMBOLIC)) vbt = BuiltinType::SYMBOLIC;
                                            else if (v.isObjType(ObjType::REAL_MATRIX)) vbt = BuiltinType::REALMAT;
                                            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) vbt = BuiltinType::COMPLEXMAT;
                                            else if (v.isObjType(ObjType::SYM_MATRIX)) vbt = BuiltinType::SYMMAT;
                                            else if (v.isFunctionClosure()) vbt = BuiltinType::FUNC;
                                            else if (v.isObjType(ObjType::NAMESPACE)) vbt = BuiltinType::NAMESPACE;
                                            else if (v.isObjType(ObjType::SLICE)) vbt = BuiltinType::SLICE;
                                            resTd->types.push_back(vbt);
                                        }
                                        resTd->normalize();
                                        return Value(resTd);
                                    }
                                }
                                throw std::runtime_error("TypeError: This type object is not callable.");
                            } else {
                                auto cls = static_cast<ObjClass*>(gVal.asObj());
                                if (cls->native_allocator) {
                                    return cls->native_allocator(fullArgs);
                                }
                                auto instance = GcHeap::get().allocate<ObjInstance>();
                                Value res(instance);
                                GcValueGuard guard(res);
                                instance->classDef = cls;
                                
                                ObjClosure* initMethod = nullptr;
                                auto c = cls;
                                while (c) {
                                    auto it = c->properties.find("<init>");
                                    if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                                        initMethod = it->second.val.asFunction();
                                        break;
                                    }
                                    c = c->parent;
                                }
                                
                                if (initMethod) {
                                    if (initMethod->isBytecode()) {
                                        VM::activeVM->callVMFunction(initMethod->compiledFnIndex, fullArgs, initMethod, res, Value(cls));
                                    } else if (initMethod->isNative()) {
                                        helpers::nativeSelfStack.push_back(res);
                                        helpers::nativeClassStack.push_back(Value(cls));
                                        auto& fn = std::any_cast<NativeCallable&>(initMethod->nativeFn);
                                        fn(fullArgs);
                                        helpers::nativeSelfStack.pop_back();
                                        helpers::nativeClassStack.pop_back();
                                    }
                                }
                                return res;
                            }
                        }
                    );
                    result = Value(bound);
                    found = true;
                }
            } else {
                Value gVal = vm->getGlobal(field);
                if (gVal.isFunctionClosure()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    ObjClosure* targetFn = gVal.asFunction();
                
                    bound->restName = targetFn->restName;
                    bound->paramNames = targetFn->paramNames;
                    bound->isRef = targetFn->isRef;
                    bound->defaultValues = targetFn->defaultValues;
                    bound->isUFCS = true;

                    if (targetFn->isBytecode()) {
                        bound->compiledFnIndex = targetFn->compiledFnIndex;
                        if (targetFn->upvalueCount > 0) {
                            bound->upvalueCount = targetFn->upvalueCount;
                            bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                            for (int i = 0; i < bound->upvalueCount; ++i) {
                                bound->upvalues[i] = targetFn->upvalues[i];
                            }
                        }
                        if (targetFn->paramTypesCount > 0) {
                            bound->paramTypesCount = targetFn->paramTypesCount;
                            bound->paramTypes = new Value[bound->paramTypesCount];
                            for (int i = 0; i < bound->paramTypesCount; ++i) {
                                bound->paramTypes[i] = targetFn->paramTypes[i];
                            }
                        }
                        bound->returnType = targetFn->returnType;
                    } else {
                        bound->boundClass = targetFn->boundClass;
                    }
                    bound->nativeFn = targetFn->nativeFn;
                    result = Value(bound);
                    found = true;
                } else if (gVal.isType() || gVal.isClass()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    bound->isUFCS = true;
                    bound->nativeFn = std::make_any<NativeCallable>(
                        [gVal](const std::vector<Value>& args) -> Value {
                            Value capturedObj = helpers::nativeSelfStack.back();
                            std::vector<Value> fullArgs;
                            fullArgs.reserve(args.size() + 1);
                            fullArgs.push_back(capturedObj);
                            fullArgs.insert(fullArgs.end(), args.begin(), args.end());
                            
                            if (gVal.isType()) {
                                ObjTypeDef* td = static_cast<ObjTypeDef*>(gVal.asObj());
                                if (td->converter) {
                                    return td->converter(fullArgs);
                                }
                                if (td->types.size() == 1 && std::holds_alternative<BuiltinType>(td->types[0])) {
                                    BuiltinType bt = std::get<BuiltinType>(td->types[0]);
                                    if (bt == BuiltinType::TYPE_DEF) {
                                        if (fullArgs.size() != 1) throw std::runtime_error("TypeError: type() expects 1 argument.");
                                        Value v = fullArgs[0];
                                        ObjTypeDef* resTd = GcHeap::get().allocate<ObjTypeDef>();
                                        if (v.isType()) resTd->types.push_back(BuiltinType::TYPE_DEF);
                                        else if (v.isClass()) resTd->types.push_back(BuiltinType::CLASS);
                                        else if (v.isInstance()) resTd->types.push_back(v.asInstance()->classDef);
                                        else {
                                            BuiltinType vbt = BuiltinType::ANY;
                                            if (v.isInt32() || v.isBigInt()) vbt = BuiltinType::INT;
                                            else if (v.isDouble()) vbt = BuiltinType::FLOAT;
                                            else if (v.isString()) vbt = BuiltinType::STRING;
                                            else if (v.isBool()) vbt = BuiltinType::BOOL;
                                            else if (v.isNone()) vbt = BuiltinType::NONE_TYPE;
                                            else if (v.isObjType(ObjType::LIST)) vbt = BuiltinType::LIST;
                                            else if (v.isObjType(ObjType::DICT)) vbt = BuiltinType::DICT;
                                            else if (v.isObjType(ObjType::SET)) vbt = BuiltinType::SET;
                                            else if (v.isObjType(ObjType::FRACTION)) vbt = BuiltinType::FRACTION;
                                            else if (v.isObjType(ObjType::COMPLEX)) vbt = BuiltinType::COMPLEX;
                                            else if (v.isObjType(ObjType::SYMBOLIC)) vbt = BuiltinType::SYMBOLIC;
                                            else if (v.isObjType(ObjType::REAL_MATRIX)) vbt = BuiltinType::REALMAT;
                                            else if (v.isObjType(ObjType::COMPLEX_MATRIX)) vbt = BuiltinType::COMPLEXMAT;
                                            else if (v.isObjType(ObjType::SYM_MATRIX)) vbt = BuiltinType::SYMMAT;
                                            else if (v.isFunctionClosure()) vbt = BuiltinType::FUNC;
                                            else if (v.isObjType(ObjType::NAMESPACE)) vbt = BuiltinType::NAMESPACE;
                                            else if (v.isObjType(ObjType::SLICE)) vbt = BuiltinType::SLICE;
                                            resTd->types.push_back(vbt);
                                        }
                                        resTd->normalize();
                                        return Value(resTd);
                                    }
                                }
                                throw std::runtime_error("TypeError: This type object is not callable.");
                            } else {
                                auto cls = static_cast<ObjClass*>(gVal.asObj());
                                if (cls->native_allocator) {
                                    return cls->native_allocator(fullArgs);
                                }
                                auto instance = GcHeap::get().allocate<ObjInstance>();
                                Value res(instance);
                                GcValueGuard guard(res);
                                instance->classDef = cls;
                                
                                ObjClosure* initMethod = nullptr;
                                auto c = cls;
                                while (c) {
                                    auto it = c->properties.find("<init>");
                                    if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                                        initMethod = it->second.val.asFunction();
                                        break;
                                    }
                                    c = c->parent;
                                }
                                
                                if (initMethod) {
                                    if (initMethod->isBytecode()) {
                                        VM::activeVM->callVMFunction(initMethod->compiledFnIndex, fullArgs, initMethod, res, Value(cls));
                                    } else if (initMethod->isNative()) {
                                        helpers::nativeSelfStack.push_back(res);
                                        helpers::nativeClassStack.push_back(Value(cls));
                                        auto& fn = std::any_cast<NativeCallable&>(initMethod->nativeFn);
                                        fn(fullArgs);
                                        helpers::nativeSelfStack.pop_back();
                                        helpers::nativeClassStack.pop_back();
                                    }
                                }
                                return res;
                            }
                        }
                    );
                    result = Value(bound);
                    found = true;
                }
            }

            if (!found) {
                const auto& nativeBuiltins = vm->getNativeBuiltins();
                auto nIt = nativeBuiltins.find(field);
                if (nIt != nativeBuiltins.end()) {
                    auto bound = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
                    );
                    bound->boundSelf = obj;
                    
                    Value builtinClosureVal = vm->getBuiltinClosure(field);
                    ObjClosure* targetFn = builtinClosureVal.asFunction();
                    bound->paramNames = targetFn->paramNames;
                    bound->isRef = targetFn->isRef;
                    bound->defaultValues = targetFn->defaultValues;
                    bound->restName = targetFn->restName;
                    bound->isUFCS = true;

                    NativeCallable nativeFn = nIt->second;
                    
                    const auto& builtinArity = vm->getBuiltinArity();
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
                    
                    ic.cachedGlobalSlot = -4;
                    ic.cachedNativeFn = bound->nativeFn;
                    
                    result = Value(bound);
                    found = true;
                }
            }
        }
    }

    if (!found) {
        result = Value::uninit();
    }
    
    vm->getCurrentFrame()->jitReturnSlot = result;
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

// ============================================================================
// opIn / opMatchType 辅助方法 + IN / IMPORT / MATCH_TYPE 的 callout
// ============================================================================
// ============================================================================
// opIterInit / opIterNext 辅助方法 + ITER_INIT / ITER_NEXT / ASSERT_PARAM_TYPE 的 callout
// ============================================================================
Value VM::opIterInit(Value iterable, uint8_t destructFlag) {
    if (iterable.isInstance()) {
        auto [method, owner] = findDunder(iterable, DUNDER_ITER);
        if (method) {
            Value iterObj = callDunder(iterable, method, owner, {});
            GcValueGuard iterGuard(iterObj);
            ObjList* state = GcHeap::get().allocate<ObjList>();
            state->vec.push_back(iterObj);
            if (iterObj.isInstance() && iterObj.asInstance()->c_nativeNext) {
                state->vec.push_back(Value::none());
            } else {
                auto [nextMethod, nextOwner] = findDunder(iterObj, DUNDER_NEXT);
                if (!nextMethod) throw std::runtime_error("VM Error: Iterator missing __next__ method.");
                state->vec.push_back(Value(nextMethod));
                state->vec.push_back(Value(nextOwner));
            }
            return Value(state);
        }
    }

    if (iterable.isObjType(ObjType::LIST) || iterable.isString() ||
        iterable.isObjType(ObjType::REAL_MATRIX) || iterable.isObjType(ObjType::COMPLEX_MATRIX) ||
        iterable.isObjType(ObjType::SYM_MATRIX)) {
        ObjList* state = GcHeap::get().allocate<ObjList>();
        state->vec.push_back(iterable);
        state->vec.push_back(Value::fromInt32(0));
        return Value(state);
    }

    ObjList* elements = GcHeap::get().allocate<ObjList>();
    Value result(elements); // ★ 立即 Root 防止 GC 误杀

    if (iterable.isObjType(ObjType::DICT)) {
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
    } else if (iterable.isObjType(ObjType::NAMESPACE)) {
        const auto* ns = static_cast<ObjNamespace*>(iterable.asObj());
        if (destructFlag) {
            for (const auto& [key, field] : ns->fields) {
                ObjList* pair = GcHeap::get().allocate<ObjList>();
                pair->vec.push_back(Value(key));
                pair->vec.push_back(*(field.upval->location));
                pair->is_frozen = true;
                elements->vec.push_back(Value(pair));
            }
        } else {
            for (const auto& [key, field] : ns->fields) {
                elements->vec.push_back(Value(key));
            }
        }
    } else if (iterable.isClass()) {
        const auto* cls = static_cast<ObjClass*>(iterable.asObj());
        std::unordered_set<std::string> seen;
        while (cls) {
            if (destructFlag) {
                for (const auto& [key, prop] : cls->properties) {
                    if (prop.is_local || seen.count(key)) continue;
                    if (isReservedInternalName(key)) continue;
                    seen.insert(key);
                    ObjList* pair = GcHeap::get().allocate<ObjList>();
                    pair->vec.push_back(Value(key));
                    pair->vec.push_back(prop.val);
                    pair->is_frozen = true;
                    elements->vec.push_back(Value(pair));
                }
            } else {
                for (const auto& [key, prop] : cls->properties) {
                    if (prop.is_local || seen.count(key)) continue;
                    if (isReservedInternalName(key)) continue;
                    seen.insert(key);
                    elements->vec.push_back(Value(key));
                }
            }
            cls = cls->parent;
        }
    } else if (iterable.isObjType(ObjType::SET)) {
        const auto* s = static_cast<ObjSet*>(iterable.asObj());
        for (const auto& val : s->elements) {
            elements->vec.push_back(val);
        }
    } else if (iterable.isInstance()) {
        auto inst = iterable.asInstance();
        if (destructFlag) {
            for (const auto& [key, prop] : inst->properties) {
                if (prop.is_local) continue;
                if (isReservedInternalName(key)) continue;
                ObjList* pair = GcHeap::get().allocate<ObjList>();
                pair->vec.push_back(Value(key));
                pair->vec.push_back(prop.val);
                pair->is_frozen = true;
                elements->vec.push_back(Value(pair));
            }
        } else {
            for (const auto& [key, prop] : inst->properties) {
                if (prop.is_local) continue;
                if (isReservedInternalName(key)) continue;
                elements->vec.push_back(Value(key));
            }
        }
    } else {
        throw std::runtime_error("VM Error: Cannot iterate over this type.");
    }

    ObjList* state = GcHeap::get().allocate<ObjList>();
    state->vec.push_back(Value(elements));
    state->vec.push_back(Value::fromInt32(0));
    return Value(state);
}

Value VM::opIterNext(Value stateVal) {
    auto state = static_cast<ObjList*>(stateVal.asObj());
    if (state->vec.size() >= 2 && state->vec[1].isInt32()) {
        Value iterTarget = state->vec[0];
        int i = state->vec[1].asInt32();

        if (iterTarget.isObjType(ObjType::LIST)) {
            const auto& elems = static_cast<ObjList*>(iterTarget.asObj())->vec;
            if (i >= static_cast<int>(elems.size())) return Value::uninit();
            Value out = elems[i];
            state->vec[1] = Value::fromInt32(i + 1);
            return out;
        } else if (iterTarget.isString()) {
            ObjString* objStr = iterTarget.asObjString();
            if (i >= static_cast<int>(objStr->charLength)) return Value::uninit();
            if (objStr->isAscii) {
                Value out = Value(std::string(1, objStr->str[i]));
                state->vec[1] = Value::fromInt32(i + 1);
                return out;
            } else {
                int byteOffset = state->vec.size() > 2 ? state->vec[2].asInt32() : 0;
                int charLen = 1;
                unsigned char ch = objStr->str[byteOffset];
                if ((ch & 0xE0) == 0xC0) charLen = 2;
                else if ((ch & 0xF0) == 0xE0) charLen = 3;
                else if ((ch & 0xF8) == 0xF0) charLen = 4;
                Value out = Value(objStr->str.substr(byteOffset, charLen));
                state->vec[1] = Value::fromInt32(i + 1);
                if (state->vec.size() > 2) state->vec[2] = Value::fromInt32(byteOffset + charLen);
                else state->vec.push_back(Value::fromInt32(byteOffset + charLen));
                return out;
            }
        } else if (iterTarget.isObjType(ObjType::REAL_MATRIX)) {
            const auto& m = static_cast<ObjRealMatrix*>(iterTarget.asObj())->mat;
            int len = (m.getRows() == 1) ? m.getCols() : m.getRows();
            if (i >= len) return Value::uninit();
            Value out;
            if (m.getRows() == 1) out = Value(m(0, i));
            else if (m.getCols() == 1) out = Value(m(i, 0));
            else {
                std::vector<double> row(m.getCols());
                for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                out = Value(RealMatrix(1, m.getCols(), row));
            }
            state->vec[1] = Value::fromInt32(i + 1);
            return out;
        } else if (iterTarget.isObjType(ObjType::COMPLEX_MATRIX)) {
            const auto& m = static_cast<ObjComplexMatrix*>(iterTarget.asObj())->mat;
            int len = (m.getRows() == 1) ? m.getCols() : m.getRows();
            if (i >= len) return Value::uninit();
            Value out;
            if (m.getRows() == 1) out = Value(m(0, i));
            else if (m.getCols() == 1) out = Value(m(i, 0));
            else {
                std::vector<Complex> row(m.getCols());
                for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                out = Value(ComplexMatrix(1, m.getCols(), row));
            }
            state->vec[1] = Value::fromInt32(i + 1);
            return out;
        } else if (iterTarget.isObjType(ObjType::SYM_MATRIX)) {
            const auto& m = static_cast<ObjSymMatrix*>(iterTarget.asObj())->mat;
            int len = (m.getRows() == 1) ? m.getCols() : m.getRows();
            if (i >= len) return Value::uninit();
            Value out;
            if (m.getRows() == 1) out = Value(m(0, i));
            else if (m.getCols() == 1) out = Value(m(i, 0));
            else {
                std::vector<SymExpr> row(m.getCols());
                for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                out = Value(SymMatrix(1, m.getCols(), row));
            }
            state->vec[1] = Value::fromInt32(i + 1);
            return out;
        }
    }

    Value iterObj = state->vec[0];
    if (iterObj.isInstance() && iterObj.asInstance()->c_nativeNext) {
        return iterObj.asInstance()->c_nativeNext(iterObj.asInstance());
    } else {
        ObjClosure* method = state->vec[1].asFunction();
        ObjClass* owner = state->vec.size() > 2 ? static_cast<ObjClass*>(state->vec[2].asObj()) : nullptr;
        Value nextVal = callDunder(iterObj, method, owner, {});
        if (nextVal.isNone()) return Value::uninit();
        return nextVal;
    }
}

uint64_t jc2_jit_is_uninit(uint64_t val_bits) {
    Value val = Value::fromRawBits(val_bits);
    return val.isUninit() ? 1 : 0;
}

uint64_t jc2_jit_iter_init(uint64_t iterable_bits, uint32_t c) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value result = vm->opIterInit(Value::fromRawBits(iterable_bits), (uint8_t)c);
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_iter_next(uint64_t state_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value result = vm->opIterNext(Value::fromRawBits(state_bits));
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

void jc2_jit_assert_param_type(uint64_t a_bits, uint32_t b, uint32_t c) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    vm->execAssertParamType(Value::fromRawBits(a_bits), (int)b, c);
    JIT_CALLOUT_CATCH_VOID
}

void jc2_jit_assert_type(uint64_t a_bits, uint64_t b_bits, uint32_t c) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    vm->execAssertType(Value::fromRawBits(a_bits), Value::fromRawBits(b_bits), c);
    JIT_CALLOUT_CATCH_VOID
}

bool VM::opIn(Value needle, Value haystack) {
    CallFrame* frame = getCurrentFrame();
    bool found = false;

    if (needle.isString() && haystack.isString()) {
        found = haystack.asString().find(needle.asString()) != std::string::npos;
    } else if (haystack.isObjType(ObjType::LIST)) {
        const auto& L = static_cast<ObjList*>(haystack.asObj())->vec;
        for (const auto& e : L) {
            try {
                bool eq = (needle.isString() && e.isString()) ? (needle.asString() == e.asString()) : Value::equals(needle, e);
                if (eq) { found = true; break; }
            } catch (...) {}
        }
    } else if (haystack.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(haystack.asObj());
        found = d->keyMap.find(needle) != d->keyMap.end();
    } else if (haystack.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(haystack.asObj());
        if (needle.isString()) {
            found = ns->fields.find(needle.asString()) != ns->fields.end();
        }
    } else if (haystack.isClass()) {
        auto cls = static_cast<ObjClass*>(haystack.asObj());
        if (needle.isString()) {
            std::string key = needle.asString();
            ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
            if (ctxOwner) {
                auto it = ctxOwner->properties.find(key);
                if (it != ctxOwner->properties.end() && it->second.is_local) {
                    found = true;
                }
            }
            if (!found) {
                while (cls) {
                    auto it = cls->properties.find(key);
                    if (it != cls->properties.end() && !it->second.is_local) {
                        found = true;
                        break;
                    }
                    cls = cls->parent;
                }
            }
        }
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
    } else if (haystack.isObjType(ObjType::SYM_MATRIX)) {
        const auto& m = static_cast<ObjSymMatrix*>(haystack.asObj())->mat;
        if (needle.isSymbolic() || needle.isNumber() || needle.isBigInt() || needle.isObjType(ObjType::FRACTION) || needle.isComplex()) {
            SymExpr nv = needle.asSymbolic();
            for (int i = 0; i < m.getRows(); ++i) {
                for (int j = 0; j < m.getCols(); ++j) {
                    if (m(i, j) == nv) { found = true; break; }
                }
                if (found) break;
            }
        }
    } else if (haystack.isInstance()) {
        auto [method, owner] = findDunder(haystack, DUNDER_CONTAINS);
        if (method) {
            found = evaluateTruthiness(callDunder(haystack, method, owner, {needle}));
        } else {
            auto inst = haystack.asInstance();
            if (needle.isString()) {
                std::string key = needle.asString();
                ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                if (ctxOwner) {
                    std::string mangledName = manglePrivate(ctxOwner->classId, key);
                    if (inst->properties.find(mangledName) != inst->properties.end()) {
                        found = true;
                    }
                }
                if (!found) {
                    auto it = inst->properties.find(key);
                    if (it != inst->properties.end() && !it->second.is_local) {
                        found = true;
                    }
                }
                if (!found) {
                    auto cls = inst->classDef;
                    while (cls) {
                        auto cit = cls->properties.find(key);
                        if (cit != cls->properties.end() && !cit->second.is_local && cit->second.val.isFunctionClosure()) {
                            found = true;
                            break;
                        }
                        cls = cls->parent;
                    }
                    if (!found) {
                        auto [getattrMethod, getattrOwner] = findDunder(haystack, DUNDER_GETATTR);
                        if (getattrMethod) {
                            try {
                                callDunder(haystack, getattrMethod, getattrOwner, {needle});
                                found = true;
                            } catch (...) {
                                // Fall through to false
                            }
                        }
                    }
                }
            }
        }
    } else if (haystack.isType()) {
        found = checkValueType(needle, static_cast<ObjTypeDef*>(haystack.asObj()));  // 包含：needle 是类型的值
    } else {
        throw std::runtime_error("VM Error: 'in' requires a string, list, dict, set, matrix, or instance.");
    }

    return found;
}

Value VM::opMatchType(Value val, Value typeVal) {
    if (typeVal.isClass()) {
        ObjClass* expectedClass = static_cast<ObjClass*>(typeVal.asObj());
        bool matched = false;
        if (val.isInstance()) {
            ObjClass* cls = val.asInstance()->classDef;
            while (cls) {
                if (cls == expectedClass) { matched = true; break; }
                cls = cls->parent;
            }
        }
        return Value(matched);
    } else {
        if (!typeVal.isType()) throw std::runtime_error("TypeError: Expected a type object.");
        return Value(checkValueType(val, static_cast<ObjTypeDef*>(typeVal.asObj())));
    }
}

Value VM::opIsSubset(Value a, Value b) {
    // 1. dunder 重载：a.__subsets__(b)
    if (a.isInstance()) {
        auto [method, owner] = findDunder(a, DUNDER_SUBSET);
        if (method) {
            return callDunder(a, method, owner, {b});
        }
    }
    // 2. 类型子集（Class 自动提升为 typedef，集合子集 + ANY 特判 + class 子类）
    auto promoteToType = [](const Value& v) -> ObjTypeDef* {
        if (v.isType()) return static_cast<ObjTypeDef*>(v.asObj());
        if (v.isClass()) return internType({static_cast<ObjClass*>(v.asObj())});
        return nullptr;
    };
    ObjTypeDef* ta = promoteToType(a);
    ObjTypeDef* tb = promoteToType(b);
    if (ta && tb) {
        for (const auto& t : ta->types) {
            bool covered = false;
            for (const auto& u : tb->types) {
                if (std::holds_alternative<BuiltinType>(t) && std::holds_alternative<BuiltinType>(u)) {
                    BuiltinType bt = std::get<BuiltinType>(t);
                    BuiltinType bu = std::get<BuiltinType>(u);
                    if (bu == BuiltinType::ANY || bt == bu) { covered = true; break; }
                } else if (std::holds_alternative<ObjClass*>(t) && std::holds_alternative<ObjClass*>(u)) {
                    ObjClass* ca = std::get<ObjClass*>(t);
                    ObjClass* cb = std::get<ObjClass*>(u);
                    while (ca) { if (ca == cb) { covered = true; break; } ca = ca->parent; }
                    if (covered) break;
                }
            }
            if (!covered) return Value(false);
        }
        return Value(true);
    }
    // 3. 集合子集
    if (a.isObjType(ObjType::SET) && b.isObjType(ObjType::SET)) {
        auto sa = static_cast<ObjSet*>(a.asObj());
        auto sb = static_cast<ObjSet*>(b.asObj());
        for (const auto& k : sa->keys) {
            if (sb->keys.find(k) == sb->keys.end()) return Value(false);
        }
        return Value(true);
    }
    throw std::runtime_error("TypeError: '<:' requires types, classes, sets, or an instance with __subsets__.");
}

uint64_t jc2_jit_in(uint64_t b_bits, uint64_t c_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    bool found = vm->opIn(Value::fromRawBits(b_bits), Value::fromRawBits(c_bits));
    return Value(found).as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_import(uint64_t b_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value path = Value::fromRawBits(b_bits);
    if (!path.isString()) throw std::runtime_error("VM Error: import requires a string path.");
    Value result = vm->importModule(path.asString());
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_match_type(uint64_t b_bits, uint64_t c_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value result = vm->opMatchType(Value::fromRawBits(b_bits), Value::fromRawBits(c_bits));
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_is_subset(uint64_t b_bits, uint64_t c_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value result = vm->opIsSubset(Value::fromRawBits(b_bits), Value::fromRawBits(c_bits));
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_index_get(uint64_t* values, uint32_t dims, uint32_t noThrow) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();

    Value obj = Value::fromRawBits(values[0]);
    std::vector<Value> args;
    args.reserve(dims);
    for (uint32_t i = 0; i < dims; ++i) {
        args.push_back(Value::fromRawBits(values[1 + i]));
    }

    Value result;

    if (obj.isInstance()) {
        auto inst = obj.asInstance();
        auto [getitemMethod, owner] = vm->findDunder(obj, "__getitem__");
        if (getitemMethod) {
            try {
                result = vm->callDunder(obj, getitemMethod, owner, args);
            } catch (...) {
                if (noThrow) result = Value::uninit();
                else throw;
            }
        } else if (dims == 1 && args[0].isString()) {
            std::string keyStr = args[0].asString();
            if (isReservedInternalName(keyStr)) {
                if (noThrow) result = Value::uninit();
                else throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
            } else {
                ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                bool foundPrivate = false;
                if (ctxOwner) {
                    std::string mangledName = manglePrivate(ctxOwner->classId, keyStr);
                    auto it = inst->properties.find(mangledName);
                    if (it != inst->properties.end()) {
                        result = it->second.val;
                        foundPrivate = true;
                    }
                }
                if (!foundPrivate) {
                    auto it = inst->properties.find(keyStr);
                    if (it != inst->properties.end() && !it->second.is_local) {
                        result = it->second.val;
                    } else {
                        if (noThrow) result = Value::uninit();
                        else throw std::runtime_error("VM Error: Property '" + keyStr + "' not found.");
                    }
                }
            }
        } else {
            if (noThrow) result = Value::uninit();
            else throw std::runtime_error("TypeError: Instance does not support this indexing. Implement __getitem__.");
        }
    } else if (dims == 1) {
        Value idx = args[0];
        if (obj.isObjType(ObjType::LIST)) {
            auto list = static_cast<ObjList*>(obj.asObj());
            auto range = idx.parseIndex(static_cast<int>(list->vec.size()), noThrow != 0);
            if (!range.isSlice && range.scalarIdx == -1) result = Value::uninit();
            else if (!range.isSlice) result = list->vec[range.scalarIdx];
            else {
                ObjList* resList = GcHeap::get().allocate<ObjList>();
                resList->vec.reserve(range.sliceInfo.count);
                for (int i = 0; i < range.sliceInfo.count; ++i) {
                    resList->vec.push_back(list->vec[range.sliceInfo.start + i * range.sliceInfo.step]);
                }
                result = Value(resList);
            }
        } else if (obj.isString()) {
            ObjString* objStr = obj.asObjString();
            auto range = idx.parseIndex(static_cast<int>(objStr->charLength), noThrow != 0);
            if (!range.isSlice && range.scalarIdx == -1) result = Value::uninit();
            else if (!range.isSlice) {
                if (objStr->isAscii) {
                    char c_str[2] = { objStr->str[range.scalarIdx], '\0' };
                    result = Value(c_str);
                } else {
                    result = Value(utf8::substring(objStr->str, range.scalarIdx, 1, objStr->isAscii));
                }
            } else {
                std::string resStr;
                if (objStr->isAscii) {
                    resStr.reserve(range.sliceInfo.count);
                    for (int i = 0; i < range.sliceInfo.count; ++i) {
                        resStr += objStr->str[range.sliceInfo.start + i * range.sliceInfo.step];
                    }
                } else {
                    for (int i = 0; i < range.sliceInfo.count; ++i) {
                        resStr += utf8::substring(objStr->str, range.sliceInfo.start + i * range.sliceInfo.step, 1, false);
                    }
                }
                result = Value(resStr);
            }
        } else if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
            auto processMatGet = [&](const auto& m) -> Value {
                using MatType = std::decay_t<decltype(m)>;
                int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                auto range = idx.parseIndex(n, noThrow != 0);
                if (!range.isSlice && range.scalarIdx == -1) return Value::uninit();
                
                if (!range.isSlice) {
                    if (m.getRows() == 1) return Value(m(0, range.scalarIdx));
                    else if (m.getCols() == 1) return Value(m(range.scalarIdx, 0));
                    else {
                        using ElemType = std::decay_t<decltype(m(0,0))>;
                        std::vector<ElemType> row(m.getCols());
                        for (int j = 0; j < m.getCols(); ++j) row[j] = m(range.scalarIdx, j);
                        return Value(MatType(1, m.getCols(), row));
                    }
                } else {
                    // 切片：返回视图（Matrix<T> 零拷贝，SymMatrix 拷贝）
                    if (m.getRows() == 1) {
                        return Value(m.view(0, 1, 1, range.sliceInfo.start, range.sliceInfo.step, range.sliceInfo.count));
                    } else if (m.getCols() == 1) {
                        return Value(m.view(range.sliceInfo.start, range.sliceInfo.step, range.sliceInfo.count, 0, 1, 1));
                    } else {
                        return Value(m.view(range.sliceInfo.start, range.sliceInfo.step, range.sliceInfo.count, 0, 1, m.getCols()));
                    }
                }
            };
            if (obj.isObjType(ObjType::REAL_MATRIX)) result = processMatGet(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) result = processMatGet(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
            else result = processMatGet(static_cast<ObjSymMatrix*>(obj.asObj())->mat);
        } else if (obj.isObjType(ObjType::DICT)) {
            if (idx.isSlice()) throw std::runtime_error("TypeError: Dict does not support slice indexing.");
            auto dict = static_cast<ObjDict*>(obj.asObj());
            auto it = dict->keyMap.find(idx);
            if (it == dict->keyMap.end()) {
                if (noThrow) result = Value::uninit();
                else throw std::runtime_error("VM Error: Key not found.");
            } else {
                result = dict->elements[it->second].second;
            }
        } else if (obj.isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(obj.asObj());
            if (!idx.isString()) {
                if (noThrow) result = Value::uninit();
                else throw std::runtime_error("VM Error: Namespace keys must be strings.");
            } else {
                std::string key = idx.asString();
                auto it = ns->fields.find(key);
                if (it == ns->fields.end()) {
                    if (noThrow) result = Value::uninit();
                    else throw std::runtime_error("VM Error: Key not found in namespace.");
                } else {
                    result = *(it->second.upval->location);
                }
            }
        } else if (obj.isClass()) {
            auto cls = static_cast<ObjClass*>(obj.asObj());
            if (!idx.isString()) {
                if (noThrow) result = Value::uninit();
                else throw std::runtime_error("VM Error: Class static field keys must be strings.");
            } else {
                std::string key = idx.asString();
                if (isReservedInternalName(key)) {
                    if (noThrow) result = Value::uninit();
                    else throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
                } else {
                    bool foundStatic = false;
                    ObjClass* ctxOwner = frame->classContext.isClass() ? static_cast<ObjClass*>(frame->classContext.asObj()) : nullptr;
                    if (ctxOwner) {
                        std::string mangledName = manglePrivate(ctxOwner->classId, key);
                        auto it = ctxOwner->properties.find(mangledName);
                        if (it != ctxOwner->properties.end()) {
                            if (it->second.val.isFunctionClosure()) {
                                auto rawMethod = it->second.val.asFunction();
                                auto bound = GcHeap::get().allocate<ObjClosure>(
                                    std::vector<std::string>{}, std::vector<bool>{}, key, nullptr
                                );
                                bound->paramNames = rawMethod->paramNames;
                                bound->isRef = rawMethod->isRef;
                                bound->defaultValues = rawMethod->defaultValues;
                                bound->restName = rawMethod->restName;
                                bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                if (rawMethod->upvalueCount > 0) {
                                    bound->upvalueCount = rawMethod->upvalueCount;
                                    bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                    for (int i = 0; i < bound->upvalueCount; ++i) {
                                        bound->upvalues[i] = rawMethod->upvalues[i];
                                    }
                                }
                                if (rawMethod->paramTypesCount > 0) {
                                    bound->paramTypesCount = rawMethod->paramTypesCount;
                                    bound->paramTypes = new Value[bound->paramTypesCount];
                                    for (int i = 0; i < bound->paramTypesCount; ++i) {
                                        bound->paramTypes[i] = rawMethod->paramTypes[i];
                                    }
                                }
                                bound->returnType = rawMethod->returnType;
                                bound->nativeFn = rawMethod->nativeFn;
                                bound->boundSelf = Value::none();
                                bound->boundClass = Value(ctxOwner);
                                bound->is_local = true;
                                result = Value(bound);
                            } else {
                                result = it->second.val;
                            }
                            foundStatic = true;
                        }
                    }
                    if (!foundStatic) {
                        auto c_cls = cls;
                        while (c_cls) {
                            auto it = c_cls->properties.find(key);
                            if (it != c_cls->properties.end() && !it->second.is_local) {
                                if (it->second.val.isFunctionClosure()) {
                                    auto rawMethod = it->second.val.asFunction();
                                    auto bound = GcHeap::get().allocate<ObjClosure>(
                                        std::vector<std::string>{}, std::vector<bool>{}, key, nullptr
                                    );
                                    bound->paramNames = rawMethod->paramNames;
                                    bound->isRef = rawMethod->isRef;
                                    bound->defaultValues = rawMethod->defaultValues;
                                    bound->restName = rawMethod->restName;
                                    bound->compiledFnIndex = rawMethod->compiledFnIndex;
                                    if (rawMethod->upvalueCount > 0) {
                                        bound->upvalueCount = rawMethod->upvalueCount;
                                        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
                                        for (int i = 0; i < bound->upvalueCount; ++i) {
                                            bound->upvalues[i] = rawMethod->upvalues[i];
                                        }
                                    }
                                    if (rawMethod->paramTypesCount > 0) {
                                        bound->paramTypesCount = rawMethod->paramTypesCount;
                                        bound->paramTypes = new Value[bound->paramTypesCount];
                                        for (int i = 0; i < bound->paramTypesCount; ++i) {
                                            bound->paramTypes[i] = rawMethod->paramTypes[i];
                                        }
                                    }
                                    bound->returnType = rawMethod->returnType;
                                    bound->nativeFn = rawMethod->nativeFn;
                                    bound->boundSelf = Value::none();
                                    bound->boundClass = Value(c_cls);
                                    result = Value(bound);
                                } else {
                                    result = it->second.val;
                                }
                                foundStatic = true;
                                break;
                            }
                            c_cls = c_cls->parent;
                        }
                    }
                    if (!foundStatic) {
                        if (noThrow) result = Value::uninit();
                        else throw std::runtime_error("VM Error: Static field not found in class.");
                    }
                }
            }
        } else if (obj.isSlice()) {
            if (!idx.isString()) {
                if (noThrow) result = Value::uninit();
                else throw std::runtime_error("VM Error: Slice properties must be accessed with string keys.");
            } else {
                Value prop = obj.asSlice()->getProperty(idx.asString());
                if (prop.isUninit()) {
                    if (noThrow) result = Value::uninit();
                    else throw std::runtime_error("VM Error: Property '" + idx.asString() + "' not found on slice.");
                } else {
                    result = prop;
                }
            }
        } else {
            if (noThrow) result = Value::uninit();
            else throw std::runtime_error("VM Error: Unsupported 1D index get.");
        }
    } else if (dims == 2) {
        Value rowIdx = args[0];
        Value colIdx = args[1];
        if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
            auto processMatGet2D = [&](const auto& m) -> Value {
                auto rRange = rowIdx.parseIndex(m.getRows(), noThrow != 0);
                auto cRange = colIdx.parseIndex(m.getCols(), noThrow != 0);
                
                if ((!rRange.isSlice && rRange.scalarIdx == -1) || (!cRange.isSlice && cRange.scalarIdx == -1)) return Value::uninit();
                
                if (!rRange.isSlice && !cRange.isSlice) {
                    return Value(m(rRange.scalarIdx, cRange.scalarIdx));
                } else if (!rRange.isSlice && cRange.isSlice) {
                    return Value(m.view(rRange.scalarIdx, 1, 1, cRange.sliceInfo.start, cRange.sliceInfo.step, cRange.sliceInfo.count));
                } else if (rRange.isSlice && !cRange.isSlice) {
                    return Value(m.view(rRange.sliceInfo.start, rRange.sliceInfo.step, rRange.sliceInfo.count, cRange.scalarIdx, 1, 1));
                } else {
                    return Value(m.view(rRange.sliceInfo.start, rRange.sliceInfo.step, rRange.sliceInfo.count, cRange.sliceInfo.start, cRange.sliceInfo.step, cRange.sliceInfo.count));
                }
            };
            if (obj.isObjType(ObjType::REAL_MATRIX)) result = processMatGet2D(static_cast<ObjRealMatrix*>(obj.asObj())->mat);
            else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) result = processMatGet2D(static_cast<ObjComplexMatrix*>(obj.asObj())->mat);
            else result = processMatGet2D(static_cast<ObjSymMatrix*>(obj.asObj())->mat);
        } else {
            if (noThrow) result = Value::uninit();
            else throw std::runtime_error("VM Error: Unsupported 2D index get.");
        }
    } else {
        throw std::runtime_error("VM Error: Unsupported index dimensionality.");
    }

    vm->getCurrentFrame()->jitReturnSlot = result;
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

// 核心索引赋值：对 obj 做 args[i] = val。原地改（引用类型）或 COW（矩阵，更新 obj 引用）。
// 不写寄存器，返回结果 obj（原地或 COW 新对象）。
static Value vmIndexSetCore(VM* vm, Value obj, std::vector<Value>& args, Value val) {
    if (obj.isInstance()) {
        auto inst = obj.asInstance();
        inst->checkModify();
        auto [setitemMethod, owner] = vm->findDunder(obj, "__setitem__");
        if (setitemMethod) {
            args.push_back(val);
            vm->callDunder(obj, setitemMethod, owner, args);
        } else if (args.size() == 1 && args[0].isString()) {
            std::string keyStr = args[0].asString();
            if (isReservedInternalName(keyStr)) {
                throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
            }
            ObjClass* ctxOwner = vm->getCurrentFrame()->classContext.isClass() ? static_cast<ObjClass*>(vm->getCurrentFrame()->classContext.asObj()) : nullptr;
            bool foundPrivate = false;
            if (ctxOwner) {
                std::string mangledName = manglePrivate(ctxOwner->classId, keyStr);
                auto it = inst->properties.find(mangledName);
                if (it != inst->properties.end()) {
                    if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const private property '" + keyStr + "'.");
                    vm->invalidateJITOnContainerReplace(it->second.val, val);
                    it->second.val = val;
                    foundPrivate = true;
                }
            }
            if (!foundPrivate) {
                Value oldVal = Value::none();
                auto oldIt = inst->properties.find(keyStr);
                if (oldIt != inst->properties.end()) oldVal = oldIt->second.val;
                vm->invalidateJITOnContainerReplace(oldVal, val);
                inst->setProperty(keyStr, val);
            }
        } else {
            throw std::runtime_error("TypeError: Instance does not support this indexing. Implement __setitem__.");
        }
    } else if (args.size() == 1) {
        Value idx = args[0];
        if (obj.isObjType(ObjType::LIST)) {
            auto list = static_cast<ObjList*>(obj.asObj());
            auto range = idx.parseIndex(static_cast<int>(list->vec.size()), false);
            if (!range.isSlice) {
                vm->invalidateJITOnContainerReplace(list->vec[range.scalarIdx], val);
                list->mut()[range.scalarIdx] = val;
            } else {
                if (val.isObjType(ObjType::LIST)) {
                    const auto& srcL = static_cast<ObjList*>(val.asObj())->vec;
                    if (static_cast<int>(srcL.size()) != range.sliceInfo.count) throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                    for (int k = 0; k < range.sliceInfo.count; ++k) list->mut()[range.sliceInfo.start + k * range.sliceInfo.step] = srcL[k];
                } else {
                    for (int i = 0; i < range.sliceInfo.count; ++i) list->mut()[range.sliceInfo.start + i * range.sliceInfo.step] = val;
                }
            }
        } else if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
            throw std::runtime_error("Runtime Error: Matrices are immutable. Use setItem(i, x) / setSlice(sr, sc, x) to get a new matrix.");
        } else if (obj.isObjType(ObjType::DICT)) {
            if (idx.isSlice()) throw std::runtime_error("TypeError: Dict does not support slice indexing.");
            auto dict = static_cast<ObjDict*>(obj.asObj());
            Value oldVal = Value::none();
            auto dit = dict->keyMap.find(idx);
            if (dit != dict->keyMap.end()) oldVal = dict->elements[dit->second].second;
            vm->invalidateJITOnContainerReplace(oldVal, val);
            dict->set(idx, val);
        } else if (obj.isObjType(ObjType::NAMESPACE)) {
            auto ns = static_cast<ObjNamespace*>(obj.asObj());
            if (!idx.isString()) throw std::runtime_error("VM Error: Namespace keys must be strings.");
            std::string key = idx.asString();
            Value oldVal = Value::none();
            auto nsIt = ns->fields.find(key);
            if (nsIt != ns->fields.end()) oldVal = *(nsIt->second.upval->location);
            vm->invalidateJITOnContainerReplace(oldVal, val);
            ns->setField(key, val);
        } else if (obj.isClass()) {
            auto cls = static_cast<ObjClass*>(obj.asObj());
            if (!idx.isString()) throw std::runtime_error("VM Error: Class static field keys must be strings.");
            std::string key = idx.asString();
            if (isReservedInternalName(key)) {
                throw std::runtime_error("Runtime Error: Cannot access private or lifecycle properties dynamically.");
            }
            
            bool found = false;
            ObjClass* ctxOwner = vm->getCurrentFrame()->classContext.isClass() ? static_cast<ObjClass*>(vm->getCurrentFrame()->classContext.asObj()) : nullptr;
            if (ctxOwner) {
                std::string mangledName = manglePrivate(ctxOwner->classId, key);
                auto it = ctxOwner->properties.find(mangledName);
                if (it != ctxOwner->properties.end()) {
                    if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const private static property '" + key + "'.");
                    vm->invalidateJITOnContainerReplace(it->second.val, val);
                    it->second.val = val;
                    found = true;
                }
            }
            if (!found) {
                auto c_cls = cls;
                while (c_cls) {
                    auto it = c_cls->properties.find(key);
                    if (it != c_cls->properties.end()) {
                        if (it->second.is_local) {
                            if (c_cls == cls) throw std::runtime_error("VM Error: Cannot modify private static property '" + key + "'.");
                            break;
                        }
                        if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const static property '" + key + "'.");
                        vm->invalidateJITOnContainerReplace(it->second.val, val);
                        it->second.val = val;
                        found = true;
                        break;
                    }
                    c_cls = c_cls->parent;
                }
            }
            if (!found) {
                vm->invalidateJITOnContainerReplace(Value::none(), val);
                if (cls) cls->properties[key] = { val, false, false };
            }
        } else {
            throw std::runtime_error("VM Error: Unsupported 1D index set.");
        }
    } else if (args.size() == 2) {
        Value rowIdx = args[0];
        Value colIdx = args[1];
        if (obj.isObjType(ObjType::REAL_MATRIX) || obj.isObjType(ObjType::COMPLEX_MATRIX) || obj.isObjType(ObjType::SYM_MATRIX)) {
            throw std::runtime_error("Runtime Error: Matrices are immutable. Use setElement(r, c, x) / setSlice(sr, sc, x) to get a new matrix.");
        } else {
            throw std::runtime_error("VM Error: Unsupported 2D index set.");
        }
    } else {
        throw std::runtime_error("VM Error: Unsupported index dimensionality.");
    }
    
    return obj;
}

uint64_t jc2_jit_index_set(uint64_t* values, uint32_t dims, uint32_t objReg) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value obj = Value::fromRawBits(values[0]);
    Value val = Value::fromRawBits(values[dims + 1]);
    std::vector<Value> args;
    args.reserve(dims);
    for (uint32_t i = 0; i < dims; ++i) {
        args.push_back(Value::fromRawBits(values[1 + i]));
    }
    Value result = vmIndexSetCore(vm, obj, args, val);
    CallFrame* frame = vm->getCurrentFrame();
    vm->getRegisters()[frame->registerBase + objReg] = result;
    vm->getCurrentFrame()->jitReturnSlot = result;
    return result.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_truthy(uint64_t val_bits) {
    Value val = Value::fromRawBits(val_bits);
    return VM::activeVM->evaluateTruthiness(val) ? 1 : 0;
}

uint64_t jc2_jit_invoke(uint64_t* values, const JitInvokeInfo* info) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value* regs = vm->getRegisters();
    int base = vm->getCurrentFrame()->registerBase;
    int initialFrameCount = vm->frameCount;
    
    uint32_t total = info->argc + 1 + (info->fbType == 1 ? 1 : 0);
    for (uint32_t i = 0; i < total; ++i) {
        regs[base + info->objReg + i] = Value::fromRawBits(values[i]);
    }
    
    vm->execInvoke(info->objReg, info->argc, 0, info->icIdx, false, static_cast<int>(info->fbType), info->isPrivate != 0);
    
    if (vm->frameCount > initialFrameCount) {
        int targetDepth = initialFrameCount;
        Value res = vm->run(targetDepth);
        vm->getCurrentFrame()->jitReturnSlot = res;
        return res.as_bits;
    } else {
        Value res = regs[base + info->objReg];
        vm->getCurrentFrame()->jitReturnSlot = res;
        return res.as_bits;
    }
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_super_invoke(uint64_t* values, const JitSuperInvokeInfo* info) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value* regs = vm->getRegisters();
    int base = vm->getCurrentFrame()->registerBase;
    int initialFrameCount = vm->frameCount;
    
    uint32_t total = info->argc + 1;
    for (uint32_t i = 0; i < total; ++i) {
        regs[base + info->objReg + i] = Value::fromRawBits(values[i]);
    }
    
    vm->execSuperInvoke(info->objReg, info->argc, 0, info->nameIdx, false);
    
    if (vm->frameCount > initialFrameCount) {
        int targetDepth = initialFrameCount;
        Value res = vm->run(targetDepth);
        vm->getCurrentFrame()->jitReturnSlot = res;
        return res.as_bits;
    } else {
        Value res = regs[base + info->objReg];
        vm->getCurrentFrame()->jitReturnSlot = res;
        return res.as_bits;
    }
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_get_self() {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value selfCtx = vm->getCurrentFrame()->selfContext;
    if (selfCtx.isNone()) throw std::runtime_error("VM Error: 'self' accessed outside of context.");
    vm->getCurrentFrame()->jitReturnSlot = selfCtx;
    return selfCtx.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_get_current_closure() {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value closure(vm->getCurrentFrame()->closure);
    vm->getCurrentFrame()->jitReturnSlot = closure;
    return closure.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_get_upval(uint32_t uvIdx) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();
    if (!frame->closure || uvIdx >= static_cast<uint32_t>(frame->closure->upvalueCount))
        throw std::runtime_error("VM Error: Invalid upvalue index.");
    Value res = *(frame->closure->upvalues[uvIdx]->location);
    frame->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

void jc2_jit_set_upval(uint32_t uvIdx, uint64_t val_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();
    if (!frame->closure || uvIdx >= static_cast<uint32_t>(frame->closure->upvalueCount))
        throw std::runtime_error("VM Error: Invalid upvalue index.");
    *(frame->closure->upvalues[uvIdx]->location) = Value::fromRawBits(val_bits);
    JIT_CALLOUT_CATCH_VOID
}

uint64_t jc2_jit_get_super(uint64_t obj_bits, uint32_t nameIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();
    
    const std::string& field = chunk->constants[nameIdx].asString();
    Value selfVal = Value::fromRawBits(obj_bits);
    if (!selfVal.isInstance()) throw std::runtime_error("VM Error: 'super' requires an instance context.");
    auto inst = selfVal.asInstance();
    
    Value classVal = frame->classContext;
    if (!classVal.isClass()) throw std::runtime_error("VM Error: 'super' requires class context.");
    auto currentClass = static_cast<ObjClass*>(classVal.asObj());
    auto parentClass = currentClass->parent;
    if (!parentClass) throw std::runtime_error("VM Error: No parent class.");
    
    ObjClosure* rawMethod = nullptr;
    ObjClass* ownerClass = nullptr;
    auto cls = parentClass;
    while (cls) {
        auto it = cls->properties.find(field);
        if (it != cls->properties.end() && !it->second.is_local && it->second.val.isFunctionClosure()) {
            rawMethod = it->second.val.asFunction();
            ownerClass = cls;
            break;
        }
        cls = cls->parent;
    }
    if (!rawMethod) throw std::runtime_error("VM Error: Parent class has no method '" + field + "'.");
    
    auto bound = GcHeap::get().allocate<ObjClosure>(
        std::vector<std::string>{}, std::vector<bool>{}, field, nullptr
    );
    bound->paramNames = rawMethod->paramNames;
    bound->isRef = rawMethod->isRef;
    bound->defaultValues = rawMethod->defaultValues;
    bound->restName = rawMethod->restName;
    bound->compiledFnIndex = rawMethod->compiledFnIndex;
    if (rawMethod->upvalueCount > 0) {
        bound->upvalueCount = rawMethod->upvalueCount;
        bound->upvalues = new ObjUpVal*[bound->upvalueCount];
        for (int i = 0; i < bound->upvalueCount; ++i) {
            bound->upvalues[i] = rawMethod->upvalues[i];
        }
    }
    if (rawMethod->paramTypesCount > 0) {
        bound->paramTypesCount = rawMethod->paramTypesCount;
        bound->paramTypes = new Value[bound->paramTypesCount];
        for (int i = 0; i < bound->paramTypesCount; ++i) {
            bound->paramTypes[i] = rawMethod->paramTypes[i];
        }
    }
    bound->returnType = rawMethod->returnType;
    bound->nativeFn = rawMethod->nativeFn;
    bound->boundSelf = Value(inst);
    bound->boundClass = Value(ownerClass);
    
    Value res(bound);
    frame->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

void jc2_jit_set_prop(uint64_t obj_bits, uint64_t val_bits, uint32_t icIdx, const Chunk* chunk) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    
    Value obj = Value::fromRawBits(obj_bits);
    Value val = Value::fromRawBits(val_bits);
    InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches[icIdx]);
    Value keyVal = chunk->constants[ic.nameIdx];
    
    if (obj.isInstance()) {
        auto inst = obj.asInstance();
        std::string keyStr = keyVal.asString();
        
        auto [setattrMethod, owner] = vm->findDunder(obj, DUNDER_SETATTR);
        if (setattrMethod) {
            inst->checkModify();
            auto it = inst->properties.find(keyStr);
            if (it != inst->properties.end()) {
                if (it->second.is_local) throw std::runtime_error("Runtime Error: Cannot modify private property '" + keyStr + "'.");
                if (it->second.is_const) throw std::runtime_error("Runtime Error: Cannot modify const property '" + keyStr + "'.");
            }
            vm->callDunder(obj, setattrMethod, owner, {keyVal, val});
        } else {
            Value oldVal = Value::none();
            auto oldIt = inst->properties.find(keyStr);
            if (oldIt != inst->properties.end()) oldVal = oldIt->second.val;
            vm->invalidateJITOnContainerReplace(oldVal, val);
            inst->setProperty(keyStr, val);
        }
    } else if (obj.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(obj.asObj());
        if (ic.cachedBuiltinType == BuiltinType::DICT && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(d->elements.size())) {
            if (d->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                vm->invalidateJITOnContainerReplace(d->elements[ic.cachedFieldIndex].second, val);
                d->elements[ic.cachedFieldIndex].second = val;
                return;
            }
        }
        auto it = d->keyMap.find(keyVal);
        if (it != d->keyMap.end()) {
            vm->invalidateJITOnContainerReplace(d->elements[it->second].second, val);
            d->elements[it->second].second = val;
            ic.cachedBuiltinType = BuiltinType::DICT;
            ic.cachedFieldIndex = static_cast<int>(it->second);
        } else {
            vm->invalidateJITOnContainerReplace(Value::none(), val);
            ic.cachedBuiltinType = BuiltinType::DICT;
            ic.cachedFieldIndex = static_cast<int>(d->elements.size());
            d->keyMap[keyVal] = d->elements.size();
            d->elements.push_back({keyVal, val});
        }
    } else if (obj.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(obj.asObj());
        Value oldVal = Value::none();
        auto nsIt = ns->fields.find(keyVal.asString());
        if (nsIt != ns->fields.end()) oldVal = *(nsIt->second.upval->location);
        vm->invalidateJITOnContainerReplace(oldVal, val);
        ns->setField(keyVal.asString(), val);
    } else if (obj.isClass()) {
        auto cls = static_cast<ObjClass*>(obj.asObj());
        std::string keyStr = keyVal.asString();
        
        bool found = false;
        auto c_cls = cls;
        while (c_cls) {
            auto it = c_cls->properties.find(keyStr);
            if (it != c_cls->properties.end()) {
                if (it->second.is_local) {
                    if (c_cls == cls) throw std::runtime_error("VM Error: Cannot modify private static property '" + keyStr + "'.");
                    break;
                }
                if (it->second.is_const) throw std::runtime_error("VM Error: Cannot modify const static property '" + keyStr + "'.");
                vm->invalidateJITOnContainerReplace(it->second.val, val);
                it->second.val = val;
                found = true;
                break;
            }
            c_cls = c_cls->parent;
        }
        if (!found) {
            vm->invalidateJITOnContainerReplace(Value::none(), val);
            if (cls) cls->properties[keyStr] = { val, false, false };
        }
    } else {
        throw std::runtime_error("VM Error: Cannot set property on this type.");
    }
    JIT_CALLOUT_CATCH_VOID
}

uint64_t jc2_jit_dict_rest(uint64_t obj_bits, uint64_t exclude_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value obj = Value::fromRawBits(obj_bits);
    Value excludeKeysVal = Value::fromRawBits(exclude_bits);
    
    std::unordered_set<std::string> excludeKeys;
    if (excludeKeysVal.isObjType(ObjType::LIST)) {
        for (const auto& k : static_cast<ObjList*>(excludeKeysVal.asObj())->vec) {
            if (k.isString()) excludeKeys.insert(k.asString());
        }
    }
    
    ObjDict* restDict = GcHeap::get().allocate<ObjDict>();
    Value res(restDict);
    GcValueGuard guard(res);
    
    if (obj.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(obj.asObj());
        for (const auto& [k, v] : d->elements) {
            if (k.isString() && excludeKeys.count(k.asString())) continue;
            restDict->set(k, v);
        }
    } else if (obj.isInstance()) {
        auto inst = obj.asInstance();
        for (const auto& [k, prop] : inst->properties) {
            if (prop.is_local) continue;
            if (isReservedInternalName(k)) continue;
            if (excludeKeys.count(k)) continue;
            restDict->set(Value(k), prop.val);
        }
    } else if (obj.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(obj.asObj());
        for (const auto& [k, field] : ns->fields) {
            if (excludeKeys.count(k)) continue;
            restDict->set(Value(k), *(field.upval->location));
        }
    } else if (obj.isClass()) {
        auto cls = static_cast<ObjClass*>(obj.asObj());
        while (cls) {
            for (const auto& [k, prop] : cls->properties) {
                if (prop.is_local) continue;
                if (isReservedInternalName(k)) continue;
                if (excludeKeys.count(k)) continue;
                if (restDict->keyMap.find(Value(k)) == restDict->keyMap.end()) {
                    restDict->set(Value(k), prop.val);
                }
            }
            cls = cls->parent;
        }
    }
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

void jc2_jit_assign_global(uint32_t slot, uint64_t src_bits) {
    JIT_CALLOUT_TRY
    Value src = Value::fromRawBits(src_bits);
    VM::activeVM->setGlobalSlot(slot, src);
    JIT_CALLOUT_CATCH_VOID
}

uint64_t jc2_jit_arith_add(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_ADD); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RADD); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs + rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_arith_sub(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_SUB); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RSUB); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs - rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_arith_mul(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_MUL); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RMUL); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs * rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_arith_div(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_DIV); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RDIV); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs / rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_arith_idiv(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_IDIV); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RIDIV); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = idivide(lhs, rhs);
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_arith_mod(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_MOD); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RMOD); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs % rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_arith_pow(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_POW); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RPOW); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs ^ rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_arith_ldiv(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_LDIV); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RLDIV); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = ldivide(lhs, rhs);
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_bitwise_and(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_BITAND); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RBITAND); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs & rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_bitwise_or(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_BITOR); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RBITOR); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs | rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_bitwise_xor(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_BITXOR); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RBITXOR); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = bitXor(lhs, rhs);
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_bitwise_shl(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_LSHIFT); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RLSHIFT); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs << rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_bitwise_shr(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, DUNDER_RSHIFT); if (meth) { res = vm->callDunder(lhs, meth, owner, {rhs}); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, DUNDER_RRSHIFT); if (meth) { res = vm->callDunder(rhs, meth, owner, {lhs}); goto done; } }
    res = lhs >> rhs;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_unary_unm(uint64_t val_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value val = Value::fromRawBits(val_bits);
    Value res;
    if (val.isInstance()) { auto [meth, owner] = vm->findDunder(val, DUNDER_NEG); if (meth) { res = vm->callDunder(val, meth, owner, {}); goto done; } }
    res = -val;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_unary_bnot(uint64_t val_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value val = Value::fromRawBits(val_bits);
    Value res;
    if (val.isInstance()) { auto [meth, owner] = vm->findDunder(val, DUNDER_BITNOT); if (meth) { res = vm->callDunder(val, meth, owner, {}); goto done; } }
    res = ~val;
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_cmp_eq(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, "__eq__"); if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(lhs, meth, owner, {rhs}))); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, "__eq__"); if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(rhs, meth, owner, {lhs}))); goto done; } }
    res = Value(Value::equals(lhs, rhs));
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_cmp_neq(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, "__neq__"); if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(lhs, meth, owner, {rhs}))); goto done; } }
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, "__eq__"); if (meth) { res = Value(!vm->evaluateTruthiness(vm->callDunder(lhs, meth, owner, {rhs}))); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, "__neq__"); if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(rhs, meth, owner, {lhs}))); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, "__eq__"); if (meth) { res = Value(!vm->evaluateTruthiness(vm->callDunder(rhs, meth, owner, {lhs}))); goto done; } }
    res = Value(!Value::equals(lhs, rhs));
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_cmp_lt(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { auto [meth, owner] = vm->findDunder(lhs, "__lt__"); if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(lhs, meth, owner, {rhs}))); goto done; } }
    if (rhs.isInstance()) { auto [meth, owner] = vm->findDunder(rhs, "__gt__"); if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(rhs, meth, owner, {lhs}))); goto done; } }
    res = Value(lhs < rhs);
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_cmp_le(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { 
        auto [meth, owner] = vm->findDunder(lhs, "__le__");
        if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(lhs, meth, owner, {rhs}))); goto done; }
        auto [methLt, ownerLt] = vm->findDunder(lhs, "__lt__");
        if (methLt) {
            if (vm->evaluateTruthiness(vm->callDunder(lhs, methLt, ownerLt, {rhs}))) { res = Value(true); goto done; }
            auto [methEq, ownerEq] = vm->findDunder(lhs, "__eq__");
            if (methEq) { res = Value(vm->evaluateTruthiness(vm->callDunder(lhs, methEq, ownerEq, {rhs}))); goto done; }
        }
    }
    if (rhs.isInstance()) { 
        auto [meth, owner] = vm->findDunder(rhs, "__ge__");
        if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(rhs, meth, owner, {lhs}))); goto done; }
        auto [methGt, ownerGt] = vm->findDunder(rhs, "__gt__");
        if (methGt) {
            if (vm->evaluateTruthiness(vm->callDunder(rhs, methGt, ownerGt, {lhs}))) { res = Value(true); goto done; }
            auto [methEq, ownerEq] = vm->findDunder(rhs, "__eq__");
            if (methEq) { res = Value(vm->evaluateTruthiness(vm->callDunder(rhs, methEq, ownerEq, {lhs}))); goto done; }
        }
    }
    res = Value(lhs <= rhs);
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_cmp_gt(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { 
        auto [meth, owner] = vm->findDunder(lhs, "__gt__");
        if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(lhs, meth, owner, {rhs}))); goto done; }
        auto [methLt, ownerLt] = vm->findDunder(lhs, "__lt__");
        if (methLt) {
            if (vm->evaluateTruthiness(vm->callDunder(lhs, methLt, ownerLt, {rhs}))) { res = Value(false); goto done; }
            auto [methEq, ownerEq] = vm->findDunder(lhs, "__eq__");
            if (methEq) { res = Value(!vm->evaluateTruthiness(vm->callDunder(lhs, methEq, ownerEq, {rhs}))); goto done; }
        }
    }
    if (rhs.isInstance()) { 
        auto [meth, owner] = vm->findDunder(rhs, "__lt__");
        if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(rhs, meth, owner, {lhs}))); goto done; }
        auto [methGt, ownerGt] = vm->findDunder(rhs, "__gt__");
        if (methGt) {
            if (vm->evaluateTruthiness(vm->callDunder(rhs, methGt, ownerGt, {lhs}))) { res = Value(false); goto done; }
            auto [methEq, ownerEq] = vm->findDunder(rhs, "__eq__");
            if (methEq) { res = Value(!vm->evaluateTruthiness(vm->callDunder(rhs, methEq, ownerEq, {lhs}))); goto done; }
        }
    }
    res = Value(lhs > rhs);
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_cmp_ge(uint64_t lhs_bits, uint64_t rhs_bits) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    Value lhs = Value::fromRawBits(lhs_bits);
    Value rhs = Value::fromRawBits(rhs_bits);
    Value res;
    if (lhs.isInstance()) { 
        auto [meth, owner] = vm->findDunder(lhs, "__ge__");
        if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(lhs, meth, owner, {rhs}))); goto done; }
        auto [methLt, ownerLt] = vm->findDunder(lhs, "__lt__");
        if (methLt) { res = Value(!vm->evaluateTruthiness(vm->callDunder(lhs, methLt, ownerLt, {rhs}))); goto done; }
    }
    if (rhs.isInstance()) { 
        auto [meth, owner] = vm->findDunder(rhs, "__le__");
        if (meth) { res = Value(vm->evaluateTruthiness(vm->callDunder(rhs, meth, owner, {lhs}))); goto done; }
        auto [methGt, ownerGt] = vm->findDunder(rhs, "__gt__");
        if (methGt) { res = Value(!vm->evaluateTruthiness(vm->callDunder(rhs, methGt, ownerGt, {lhs}))); goto done; }
    }
    res = Value(lhs >= rhs);
done:
    vm->getCurrentFrame()->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

uint64_t jc2_jit_closure(uint32_t fnIdx, uint32_t registerOffset) {
    JIT_CALLOUT_TRY
    VM* vm = VM::activeVM;
    CallFrame* frame = vm->getCurrentFrame();
    Value* regs = vm->getRegisters();
    int base = frame->registerBase;
    
    if (fnIdx >= vm->getCompiledFunctions().size())
        throw std::runtime_error("JIT Error: Invalid function index.");

    auto& fn = vm->getCompiledFunctions()[fnIdx];
    auto closure = GcHeap::get().allocate<ObjClosure>(
        std::vector<std::string>{}, std::vector<bool>{}, fn->name, nullptr
    );
    Value res(closure);
    GcValueGuard closureGuard(res);
    closure->compiledFnIndex = fnIdx;

    if (!fn->upvalues.empty()) {
        closure->upvalueCount = static_cast<int>(fn->upvalues.size());
        closure->upvalues = new ObjUpVal*[closure->upvalueCount];
        for (int i = 0; i < closure->upvalueCount; ++i) closure->upvalues[i] = nullptr;
        for (int i = 0; i < closure->upvalueCount; ++i) {
            auto& uv = fn->upvalues[i];
            if (uv.isRef) {
                if (uv.isLocal) {
                    if (uv.isRefParam) {
                        closure->upvalues[i] = static_cast<ObjUpVal*>(regs[frame->refParamsBase + uv.index].asObj());
                    } else {
                        closure->upvalues[i] = vm->captureUpvaluePublic(base + registerOffset + uv.index);
                    }
                } else {
                    if (frame->closure && uv.index < frame->closure->upvalueCount)
                        closure->upvalues[i] = frame->closure->upvalues[uv.index];
                    else {
                        auto dummy = GcHeap::get().allocate<ObjUpVal>();
                        dummy->closed = Value::none();
                        dummy->location = &dummy->closed;
                        closure->upvalues[i] = dummy;
                    }
                }
            } else {
                auto dummy = GcHeap::get().allocate<ObjUpVal>();
                if (uv.isExplicitState) {
                    dummy->closed = Value::uninit();
                } else if (uv.isLocal) {
                    if (uv.isRefParam) {
                        dummy->closed = *(static_cast<ObjUpVal*>(regs[frame->refParamsBase + uv.index].asObj())->location);
                    } else {
                        dummy->closed = regs[base + registerOffset + uv.index];
                    }
                } else {
                    if (frame->closure && uv.index < frame->closure->upvalueCount) {
                        dummy->closed = *(frame->closure->upvalues[uv.index]->location);
                    } else if (uv.isGlobal) {
                        dummy->closed = vm->getGlobalChecked(uv.name);
                    } else {
                        dummy->closed = Value::none();
                    }
                }
                dummy->location = &dummy->closed;
                closure->upvalues[i] = dummy;
            }
        }
    }
            
    int capturedFnIdx = fnIdx;
    Value currentSelf = frame->selfContext;
    Value currentClass = frame->classContext;
    closure->nativeFn = std::make_any<NativeCallable>(
        [vm, capturedFnIdx, closure, currentSelf, currentClass](const std::vector<Value>& args) -> Value {
            Value s = !helpers::nativeSelfStack.empty() ? helpers::nativeSelfStack.back() : currentSelf;
            Value c = !helpers::nativeClassStack.empty() ? helpers::nativeClassStack.back() : currentClass;
            return vm->callVMFunction(capturedFnIdx, args, closure, s, c);
        }
    );

    closure->paramNames = fn->paramNames;
    closure->isRef = fn->paramIsRef;
    closure->isConst = fn->paramIsConst;
    int defaultLimit = fn->maxArity;
    for (int j = fn->arity; j < defaultLimit; ++j) {
        closure->defaultValues.push_back(Value::uninit());
    }
    closure->restName = fn->restName;
    closure->kwargNames = fn->kwargNames;
    closure->kwargIsRef = fn->kwargIsRef;
    closure->kwargIsConst = fn->kwargIsConst;
    closure->kwargHasDefault = fn->kwargHasDefault;
    closure->kwargsName = fn->kwargsName;
    closure->boundSelf = frame->selfContext;
    closure->boundClass = frame->classContext;
    
    if (!fn->paramTypeRegs.empty()) {
        closure->paramTypesCount = static_cast<int>(fn->paramTypeRegs.size());
        closure->paramTypes = new Value[closure->paramTypesCount];
        for (int i = 0; i < closure->paramTypesCount; ++i) {
            int reg = fn->paramTypeRegs[i];
            closure->paramTypes[i] = (reg != -1) ? regs[base + registerOffset + reg] : Value::none();
        }
    }
    if (fn->returnTypeReg != -1) {
        closure->returnType = regs[base + registerOffset + fn->returnTypeReg];
    }
    
    frame->jitReturnSlot = res;
    return res.as_bits;
    JIT_CALLOUT_CATCH
}

} // namespace jc
