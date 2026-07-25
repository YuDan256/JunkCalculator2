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
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <filesystem>
#include <fstream>

extern bool g_showIR;
extern bool g_autoDebug;
extern bool g_profile;

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#undef IN
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace jc {

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
                if (inst && inst->fields) {
                    auto it = inst->fields->keyMap.find(Value("suppressed"));
                    if (it != inst->fields->keyMap.end()) {
                        Value suppList = inst->fields->elements[it->second].second;
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
                if (inst && inst->fields) {
                    auto it = inst->fields->keyMap.find(Value("suppressed"));
                    if (it != inst->fields->keyMap.end()) {
                        Value suppList = inst->fields->elements[it->second].second;
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
                if (inst && inst->fields) {
                    auto it = inst->fields->keyMap.find(Value("suppressed"));
                    if (it != inst->fields->keyMap.end()) {
                        Value suppList = inst->fields->elements[it->second].second;
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
                if (inst && inst->fields) {
                    auto it = inst->fields->keyMap.find(Value("suppressed"));
                    if (it != inst->fields->keyMap.end()) {
                        Value suppList = inst->fields->elements[it->second].second;
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

void VM::execCall(int calleeReg, int argc, int dstReg, bool isTailCall) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    const Value& callee = registers[currentFrame->registerBase + calleeReg];
    
    if (callee.isString()) {
        const std::string& tag = callee.asString();
        auto it = globalNames.find(tag);
        if (it != globalNames.end()) {
            registers[currentFrame->registerBase + dstReg] = globals[it->second];
        } else {
            auto nIt = nativeBuiltins.find(tag);
            if (nIt != nativeBuiltins.end()) {
                std::vector<Value> args;
                args.reserve(argc);
                for (int i = 0; i < argc; ++i) {
                    args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
                }
                pendingCallRefs.clear();
                registers[currentFrame->registerBase + dstReg] = nIt->second(args);
                return;
            }
            throw std::runtime_error("VM Error: Unknown function or not callable '" + tag + "()'.");
        }
    }

    if (callee.isFunctionClosure()) {
        auto closure = callee.asFunction();
        if (closure->isBytecode()) {
            auto& fnDef = compiledFunctions[closure->compiledFnIndex];
            
            int ufcsOffset = closure->isUFCS ? 1 : 0;
            int totalArgc = argc + ufcsOffset;
            
            if (closure->isUFCS) {
                for (auto& pr : pendingCallRefs) pr.first += 1;
            }

            int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + calleeReg + 1;
            int newTotalCount = fnDef->localCount + fnDef->refCount;
            PendingFrameGuard pfg(this, newBase, newTotalCount);

            if (fnDef->hasRestParam) {
                int fixedMax = fnDef->maxArity - 1;
                if (totalArgc < fnDef->arity) {
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                }
                ObjList* restList = GcHeap::get().allocate<ObjList>();
                if (totalArgc > fixedMax) {
                    int restCount = totalArgc - fixedMax;
                    restList->vec.reserve(restCount);
                    for (int j = 0; j < restCount; j++) {
                        int srcIdx = fixedMax + j;
                        if (closure->isUFCS && srcIdx == 0) restList->vec.push_back(closure->boundSelf);
                        else restList->vec.push_back(registers[currentFrame->registerBase + calleeReg + 1 + srcIdx - ufcsOffset]);
                    }
                }
                
                if (closure->isUFCS) {
                    registers[newBase] = closure->boundSelf;
                    for (int i = 1; i < std::min(totalArgc, fixedMax); ++i) {
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + i];
                    }
                } else {
                    for (int i = 0; i < std::min(totalArgc, fixedMax); ++i) {
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + 1 + i];
                    }
                }
                for (int i = totalArgc; i < fixedMax; ++i) {
                    registers[newBase + i] = Value::uninit();
                }
                registers[newBase + fixedMax] = Value(restList);
            } else {
                if (totalArgc < fnDef->arity || totalArgc > fnDef->maxArity) {
                    throw std::runtime_error("VM Error: '" + fnDef->name + "' expects " + std::to_string(fnDef->arity) + " to " + std::to_string(fnDef->maxArity) + " arguments, got " + std::to_string(totalArgc) + ".");
                }
                if (closure->isUFCS) {
                    registers[newBase] = closure->boundSelf;
                    for (int i = 1; i < totalArgc; ++i) {
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + i];
                    }
                } else {
                    for (int i = 0; i < totalArgc; ++i) {
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + 1 + i];
                    }
                }
                for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                    registers[newBase + i] = Value::uninit();
                }
            }

            for (int i = fnDef->maxArity; i < fnDef->localCount; ++i) {
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
                args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
            }
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
        auto instance = GcHeap::get().allocate<ObjInstance>();
        registers[currentFrame->registerBase + dstReg] = Value(instance); // ★ 立即 Root 防止 GC 误杀
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
                
                int totalArgc = argc;
                int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + calleeReg + 1;
                int newTotalCount = fnDef->localCount + fnDef->refCount;
                PendingFrameGuard pfg(this, newBase, newTotalCount);

                if (fnDef->hasRestParam) {
                    int fixedMax = fnDef->maxArity - 1;
                    if (totalArgc < fnDef->arity) {
                        throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                    }
                    ObjList* restList = GcHeap::get().allocate<ObjList>();
                    if (totalArgc > fixedMax) {
                        int restCount = totalArgc - fixedMax;
                        restList->vec.reserve(restCount);
                        for (int j = 0; j < restCount; j++) {
                            restList->vec.push_back(registers[currentFrame->registerBase + calleeReg + 1 + fixedMax + j]);
                        }
                    }
                    
                    for (int i = 0; i < std::min(totalArgc, fixedMax); ++i) {
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + 1 + i];
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
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + 1 + i];
                    }
                    for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                        registers[newBase + i] = Value::uninit();
                    }
                }

                for (int i = fnDef->maxArity; i < fnDef->localCount; ++i) {
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
                helpers::nativeSelfStack.push_back(Value(instance));
                helpers::nativeClassStack.push_back(Value(cls));
                std::vector<Value> args;
                args.reserve(argc);
                for (int i = 0; i < argc; ++i) args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
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
                
                int totalArgc = argc;
                int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + calleeReg + 1;
                int newTotalCount = fnDef->localCount + fnDef->refCount;
                PendingFrameGuard pfg(this, newBase, newTotalCount);

                if (fnDef->hasRestParam) {
                    int fixedMax = fnDef->maxArity - 1;
                    if (totalArgc < fnDef->arity) {
                        throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
                    }
                    ObjList* restList = GcHeap::get().allocate<ObjList>();
                    if (totalArgc > fixedMax) {
                        int restCount = totalArgc - fixedMax;
                        restList->vec.reserve(restCount);
                        for (int j = 0; j < restCount; j++) {
                            restList->vec.push_back(registers[currentFrame->registerBase + calleeReg + 1 + fixedMax + j]);
                        }
                    }
                    
                    for (int i = 0; i < std::min(totalArgc, fixedMax); ++i) {
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + 1 + i];
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
                        registers[newBase + i] = registers[currentFrame->registerBase + calleeReg + 1 + i];
                    }
                    for (int i = totalArgc; i < fnDef->maxArity; ++i) {
                        registers[newBase + i] = Value::uninit();
                    }
                }

                for (int i = fnDef->maxArity; i < fnDef->localCount; ++i) {
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
                helpers::nativeSelfStack.push_back(callee);
                helpers::nativeClassStack.push_back(Value(owningClass));
                std::vector<Value> args;
                args.reserve(argc);
                for (int i = 0; i < argc; ++i) {
                    args.push_back(registers[currentFrame->registerBase + calleeReg + 1 + i]);
                }
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
    std::vector<Value> rootedArgs = args;
    std::vector<std::unique_ptr<GcValueGuard>> guards;
    for (auto& arg : rootedArgs) guards.push_back(std::make_unique<GcValueGuard>(arg));

    auto inst = obj.asInstance();
    if (method->isNative() && !method->isBytecode()) {
        helpers::nativeSelfStack.push_back(Value(inst));
        helpers::nativeClassStack.push_back(Value(inst->classDef));
        pendingCallRefs.clear();
        Value result;
        try {
            auto& fn = std::any_cast<NativeCallable&>(method->nativeFn);
            result = fn(rootedArgs);
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
            newBase = currentFrame->registerBase + currentFrame->function->localCount + currentFrame->function->refCount;
        }
        
        int newTotalCount = fnDef->localCount + fnDef->refCount;
        PendingFrameGuard pfg(this, newBase, newTotalCount);

        newFrame.registerBase = newBase;
        newFrame.returnRegister = 0;
        newFrame.deferBase = static_cast<int>(deferStack.size());
        newFrame.closure = method;
        newFrame.selfContext = Value(inst);
        newFrame.classContext = Value(inst->classDef);
        
        int totalArgc = static_cast<int>(args.size());

        if (fnDef->hasRestParam) {
            int fixedMax = fnDef->maxArity - 1;
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

        for (int i = fnDef->maxArity; i < fnDef->localCount; ++i) {
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
                f->closure = nullptr;
                f->refParamsBase = -1;
                frameCount--;
            }
            throw;
        }
    }
    throw std::runtime_error("VM Error: Dunder method is not callable.");
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
            else typeVal = getBuiltinValue(typeStr);
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
    InlineCache& ic = const_cast<InlineCache&>(currentFrame->function->chunk.inlineCaches.data()[icIdx]);
    if (ic.cachedBuiltinType == BuiltinType::UNKNOWN) {
        ic.cachedBuiltinType = parseBuiltinType(currentFrame->function->chunk.constants.data()[ic.nameIdx].asString());
    }
    const std::string& expectedType = currentFrame->function->chunk.constants.data()[ic.nameIdx].asString();

    if (!checkValueType(val, ic.cachedBuiltinType, expectedType)) {
        const std::string& paramName = currentFrame->function->chunk.constants.data()[nameIdx].asString();
        throw std::runtime_error("TypeError: Parameter '" + paramName +
            "' expected type '" + expectedType +
            "', got '" + getTypeName(val) + "'.");
    }
}

void VM::execAssertReturnType(const Value& val, uint32_t icIdx) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    InlineCache& ic = const_cast<InlineCache&>(currentFrame->function->chunk.inlineCaches.data()[icIdx]);
    if (ic.cachedBuiltinType == BuiltinType::UNKNOWN) {
        ic.cachedBuiltinType = parseBuiltinType(currentFrame->function->chunk.constants.data()[ic.nameIdx].asString());
    }
    const std::string& expectedType = currentFrame->function->chunk.constants.data()[ic.nameIdx].asString();

    if (!checkValueType(val, ic.cachedBuiltinType, expectedType)) {
        throw std::runtime_error("TypeError: Function '" + currentFrame->function->name +
            "' expected to return '" + expectedType +
            "', but returned '" + getTypeName(val) + "'.");
    }
}

void VM::execInvoke(int a, int b, uint32_t icIdx, bool isTailCall, int fbType) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    InlineCache& ic = const_cast<InlineCache&>(currentFrame->function->chunk.inlineCaches.data()[icIdx]);
    uint32_t nameIdx = ic.nameIdx;
    Value keyVal = currentFrame->function->chunk.constants.data()[nameIdx];
    
    int argc = b;
    const Value& obj = registers[currentFrame->registerBase + a];

    ObjClosure* method = nullptr;
    ObjClass* owningClass = nullptr;

    const std::string& methodName = keyVal.asString();

    if (obj.isInstance()) {
        auto inst = obj.asInstance();
        if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
            if (!inst->fields || inst->fields->keyMap.find(keyVal) == inst->fields->keyMap.end()) {
                method = ic.cachedMethod;
                owningClass = inst->classDef;
                goto invoke_method;
            }
        }
    }

    if (obj.isObjType(ObjType::DICT)) {
        auto d = static_cast<ObjDict*>(obj.asObj());
        auto it = d->keyMap.find(keyVal);
        if (it != d->keyMap.end()) {
            Value fv = d->elements[it->second].second;
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, a, isTailCall);
                return;
            }
        }
    } else if (obj.isObjType(ObjType::NAMESPACE)) {
        auto ns = static_cast<ObjNamespace*>(obj.asObj());
        auto it = ns->fields.find(keyVal.asString());
        if (it != ns->fields.end()) {
            Value fv = *(it->second.upval->location);
            if (fv.isFunctionClosure()) {
                method = fv.asFunction();
            } else {
                registers[currentFrame->registerBase + a] = fv;
                execCall(a, b, a, isTailCall);
                return;
            }
        }
    } else if (obj.isInstance()) {
        auto inst = obj.asInstance();
        bool foundInField = false;

        if (ic.cachedClassId == inst->classDef->classId && ic.cachedMethod) {
            if (!inst->fields || inst->fields->keyMap.find(keyVal) == inst->fields->keyMap.end()) {
                method = ic.cachedMethod;
                owningClass = inst->classDef;
                goto invoke_method;
            }
        }

        if (inst->fields) {
            auto it = inst->fields->keyMap.find(keyVal);
            if (it != inst->fields->keyMap.end()) {
                Value fv = inst->fields->elements[it->second].second;
                if (fv.isFunctionClosure()) {
                    method = fv.asFunction();
                    owningClass = inst->classDef;
                    foundInField = true;
                } else {
                    registers[currentFrame->registerBase + a] = fv;
                    execCall(a, b, a, isTailCall);
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
                ic.cachedClassId = inst->classDef->classId;
                ic.cachedMethod = method;
            }

            if (!method) {
                auto getattrMethod = findDunder(obj, "__getattr__");
                if (getattrMethod) {
                    std::vector<Value> args = { keyVal };
                    Value fv = callDunder(obj, getattrMethod, args);
                    if (fv.isFunctionClosure()) {
                        method = fv.asFunction();
                        owningClass = inst->classDef;
                    } else {
                        registers[currentFrame->registerBase + a] = fv;
                        execCall(a, b, a, isTailCall);
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
            execCall(a, argc + 1, a, isTailCall);
            return;
        }
        
        if (ic.cachedGlobalSlot == -4) {
            std::vector<Value> argsVec;
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
        
        if (ic.cachedGlobalSlot >= 0) {
            if (globals[ic.cachedGlobalSlot].isFunctionClosure()) {
                for (int i = argc - 1; i >= 0; --i) {
                    registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
                }
                registers[currentFrame->registerBase + a + 1] = obj;
                registers[currentFrame->registerBase + a] = globals[ic.cachedGlobalSlot];
                for (auto& pr : pendingCallRefs) {
                    pr.first += 1;
                }
                execCall(a, argc + 1, a, isTailCall);
                return;
            }
        } else {
            auto gIt = globalNames.find(methodName);
            if (gIt != globalNames.end() && globals[gIt->second].isFunctionClosure()) {
                ic.cachedGlobalSlot = gIt->second;
                for (int i = argc - 1; i >= 0; --i) {
                    registers[currentFrame->registerBase + a + 2 + i] = registers[currentFrame->registerBase + a + 1 + i];
                }
                registers[currentFrame->registerBase + a + 1] = obj;
                registers[currentFrame->registerBase + a] = globals[gIt->second];
                for (auto& pr : pendingCallRefs) {
                    pr.first += 1;
                }
                execCall(a, argc + 1, a, isTailCall);
                return;
            }
        }

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
        
        throw std::runtime_error("VM Error: Cannot invoke method '" + methodName + "' on this type.");
    }

    if (method->isBytecode()) {
        auto& fnDef = compiledFunctions[method->compiledFnIndex];
        
        int totalArgc = argc;
        int newBase = isTailCall ? currentFrame->registerBase : currentFrame->registerBase + a + 1;
        int newTotalCount = fnDef->localCount + fnDef->refCount;
        PendingFrameGuard pfg(this, newBase, newTotalCount);

        if (fnDef->hasRestParam) {
            int fixedMax = fnDef->maxArity - 1;
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

        for (int i = fnDef->maxArity; i < fnDef->localCount; ++i) {
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

void VM::execSuperInvoke(int a, int b, uint32_t nameIdx, bool isTailCall) {
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
        auto it = c->methods.find(methodName);
        if (it != c->methods.end()) {
            method = it->second;
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

        if (fnDef->hasRestParam) {
            int fixedMax = fnDef->maxArity - 1;
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

        for (int i = fnDef->maxArity; i < fnDef->localCount; ++i) {
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

void VM::execSliceGet(int a, int b, uint8_t dims) {
    CallFrame* currentFrame = &frames[frameCount - 1];
    const Value& obj = registers[currentFrame->registerBase + b];
    
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
            if (idx < 0 || idx >= dimSize) throw std::out_of_range("VM Error: Index out of bounds.");
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
        auto start = readOptionalInt(0);
        auto end = readOptionalInt(1);
        auto step = readOptionalInt(2);
        
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
        
        throw std::runtime_error("VM Error: Cannot slice a value of type '" + getTypeName(obj) + "'.");
    } else if (dims == 2) {
        auto rStart = readOptionalInt(0);
        auto rEnd = readOptionalInt(1);
        auto rStep = readOptionalInt(2);
        auto cStart = readOptionalInt(3);
        auto cEnd = readOptionalInt(4);
        auto cStep = readOptionalInt(5);
        
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
            throw std::runtime_error("VM Error: 2D slicing requires a matrix.");
        }
    } else {
        throw std::runtime_error("VM Error: Unsupported slice dimensionality.");
    }
}

void VM::execSliceSet(int a, int c, uint8_t dims) {
    (void)c;
    CallFrame* currentFrame = &frames[frameCount - 1];
    Value obj = registers[currentFrame->registerBase + a];
    const Value& val = registers[currentFrame->registerBase + a + 3 * dims + 1];
    
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
            if (idx < 0 || idx >= dimSize) throw std::out_of_range("VM Error: Index out of bounds.");
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
        auto start = readOptionalInt(0);
        auto end = readOptionalInt(1);
        auto step = readOptionalInt(2);
        
        if (obj.isObjType(ObjType::LIST)) {
            auto list = static_cast<ObjList*>(obj.asObj());
            auto info = buildSliceInfo(static_cast<int>(list->vec.size()), start, end, step);
            if (val.isObjType(ObjType::LIST)) {
                const auto& srcL = static_cast<ObjList*>(val.asObj())->vec;
                if (static_cast<int>(srcL.size()) != info.count) throw std::runtime_error("VM Error: Slice assignment size mismatch.");
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
                    if (static_cast<int>(srcFlat.size()) != info.count) throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                    if (m.getRows() == 1) {
                        for (int k = 0; k < info.count; ++k) m(0, info.start + k * info.step) = srcFlat[k];
                    } else {
                        for (int k = 0; k < info.count; ++k) m(info.start + k * info.step, 0) = srcFlat[k];
                    }
                } else {
                    if (static_cast<int>(srcFlat.size()) != info.count * m.getCols()) throw std::runtime_error("VM Error: Slice assignment size mismatch for matrix row.");
                    for (int k = 0; k < info.count; ++k) {
                        int id = info.start + k * info.step;
                        for (int j = 0; j < m.getCols(); ++j) m(id, j) = srcFlat[k * m.getCols() + j];
                    }
                }
            } else {
                throw std::runtime_error("VM Error: Cannot assign this type to slice.");
            }
            registers[currentFrame->registerBase + a] = obj;
        } else {
            throw std::runtime_error("VM Error: Cannot slice-assign a value of type '" + getTypeName(obj) + "'.");
        }
    } else if (dims == 2) {
        auto rStart = readOptionalInt(0);
        auto rEnd = readOptionalInt(1);
        auto rStep = readOptionalInt(2);
        auto cStart = readOptionalInt(3);
        auto cEnd = readOptionalInt(4);
        auto cStep = readOptionalInt(5);
        
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
                if (srcR != dstR || srcC != dstC) throw std::runtime_error("VM Error: Slice assignment size mismatch.");
                
                for (int i = 0; i < dstR; ++i) {
                    int ri = rInfo.start + i * rInfo.step;
                    for (int j = 0; j < dstC; ++j) {
                        int ci = cInfo.start + j * cInfo.step;
                        if constexpr (std::is_same_v<ElemType, double>) {
                            if (val.isObjType(ObjType::REAL_MATRIX)) m(ri, ci) = static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j);
                            else throw std::runtime_error("VM Error: Cannot assign complex/string matrix to real matrix slice.");
                        } else if constexpr (std::is_same_v<ElemType, Complex>) {
                            if (val.isObjType(ObjType::COMPLEX_MATRIX)) m(ri, ci) = static_cast<ObjComplexMatrix*>(val.asObj())->mat(i, j);
                            else if (val.isObjType(ObjType::REAL_MATRIX)) m(ri, ci) = Complex(static_cast<ObjRealMatrix*>(val.asObj())->mat(i, j));
                            else throw std::runtime_error("VM Error: Cannot assign string matrix to complex matrix slice.");
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
            throw std::runtime_error("VM Error: 2D slice assignment requires a matrix.");
        }
    } else {
        throw std::runtime_error("VM Error: Unsupported slice assignment dimensionality.");
    }
}

Value VM::execImport(const std::string& name) {
    std::string baseName = std::filesystem::path(name).stem().string();

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
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
        std::string modPath = (std::filesystem::path(exePath).parent_path() / "lib" / nativeName).string();
        if (std::filesystem::is_regular_file(modPath)) resolved = modPath;
    }
#else
    char exePath[4096];
    ssize_t count = readlink("/proc/self/exe", exePath, 4096);
    if (count != -1) {
        std::string modPath = (std::filesystem::path(std::string(exePath, count)).parent_path() / "lib" / nativeName).string();
        if (std::filesystem::is_regular_file(modPath)) resolved = modPath;
    }
#endif

    // 2. 其次查找当前目录下的原生库
    if (resolved.empty()) {
        std::string localModPath = helpers::safeResolvePath(nativeName);
        if (std::filesystem::is_regular_file(localModPath)) resolved = localModPath;
    }

    // 3. 查找 .jcb 字节码
    std::string jcbPath = "";
    if (resolved.empty()) {
        std::string p = helpers::safeResolvePath(name);
        if (std::filesystem::path(p).extension() == ".jcb" && std::filesystem::is_regular_file(p)) {
            jcbPath = p;
        } else {
            p = helpers::safeResolvePath(name + ".jcb");
            if (std::filesystem::is_regular_file(p)) jcbPath = p;
        }
    }

    // 4. 查找 .jc2 模块脚本
    std::string jc2Path = "";
    if (resolved.empty() && jcbPath.empty()) {
        std::string p = helpers::safeResolvePath(name);
        if (std::filesystem::path(p).extension() == ".jc2" && std::filesystem::is_regular_file(p)) {
            jc2Path = p;
        } else {
            p = helpers::safeResolvePath(name + ".jc2");
            if (std::filesystem::is_regular_file(p)) jc2Path = p;
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
        std::string ext = std::filesystem::path(resolved).extension().string();
#if defined(_WIN32)
        HMODULE handle = LoadLibraryA(resolved.c_str());
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

        ModuleLoadContext mctx = { &tempGlobals, &tempNatives, &tempArity };

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
            if (ait != tempArity.end() && !ait->second.empty()) {
                int maxA = *ait->second.rbegin();
                int minA = *ait->second.begin();
                for (int j = 0; j < maxA; ++j) {
                    closure->paramNames.push_back("_" + std::to_string(j));
                    closure->isRef.push_back(false);
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
        std::ifstream file(jc2Path);
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
        modFn->hasRestParam = false;

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

    std::string scriptDir = std::filesystem::path(executePath).parent_path().string();
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
    std::string baseName = std::filesystem::path(name).stem().string();

    if (loadedModules.count(name)) {
        return;
    }

    std::string resolved = "";
    
    resolved = helpers::safeResolvePath(name);
    if (!std::filesystem::is_regular_file(resolved)) {
        resolved = helpers::safeResolvePath(name + ".jc2");
    }

    if (resolved.empty() || !std::filesystem::is_regular_file(resolved)) {
        throw std::runtime_error("VM Error: Cannot find compile-time module '" + name + "'.");
    }

    std::ifstream file(resolved);
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
    modFn->hasRestParam = false;

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

    std::string scriptDir = std::filesystem::path(resolved).parent_path().string();
    helpers::g_scriptDirStack.push_back(scriptDir);
    Value nsVal;
    try {
        nsVal = run(targetDepth);
        frames[frameCount].selfContext = Value::none();
        frames[frameCount].classContext = Value::none();
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
        if (inst->fields) {
            auto it = inst->fields->keyMap.find(Value("traceback"));
            if (it != inst->fields->keyMap.end()) {
                Value tbVal = inst->fields->elements[it->second].second;
                if (tbVal.isString() && tbVal.asString().empty()) {
                    inst->fields->elements[it->second].second = Value(buildStackTrace());
                }
            }
        }
        return val;
    }
    
    Value classVal = getBuiltinValue("Exception");
    if (!classVal.isClass()) return val;
    
    ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
    inst->classDef = static_cast<ObjClass*>(classVal.asObj());
    inst->fields = GcHeap::get().allocate<ObjDict>();
    
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
    
    inst->fields->set(Value("type"), Value(type));
    inst->fields->set(Value("message"), val);
    inst->fields->set(Value("traceback"), Value(buildStackTrace()));
    inst->fields->set(Value("suppressed"), Value(GcHeap::get().allocate<ObjList>()));
    
    return Value(inst);
}

std::string VM::formatException(const Value& errVal) {
    if (errVal.isInstance() && errVal.asInstance()->classDef->name == "Exception") {
        auto dunderStr = findDunder(errVal, "__str__");
        if (dunderStr) {
            try {
                return callDunder(errVal, dunderStr, {}).asString();
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
                try { fnName = std::filesystem::path(srcFile).filename().string(); }
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

void VM::profileFrameStart(CallFrame* frame) {
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
    registers = new Value[MAX_REGISTERS];
    frames = new CallFrame[MAX_FRAMES];
    
    GcHeap::get().markCallback = [this]() {
        for (auto& v : globals) GcHeap::get().markValue(v);
        
        int maxReg = 0;
        for (int i = 0; i < frameCount; ++i) {
            CallFrame& f = frames[i];
            GcHeap::get().markValue(f.selfContext);
            GcHeap::get().markValue(f.classContext);
            if (f.closure) GcHeap::get().markObj(f.closure);
            if (f.chunk) {
                for (auto& v : f.chunk->constants) GcHeap::get().markValue(v);
                for (auto& ic : f.chunk->inlineCaches) {
                    if (ic.cachedMethod) GcHeap::get().markObj(ic.cachedMethod);
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

        for (auto& v : helpers::nativeSelfStack) GcHeap::get().markValue(v);
        for (auto& v : helpers::nativeClassStack) GcHeap::get().markValue(v);
        for (auto& v : jc::nativeTempRefs) GcHeap::get().markValue(v);
        
        for (auto& fn : compiledFunctions) {
            if (fn) {
                for (auto& v : fn->chunk.constants) GcHeap::get().markValue(v);
                for (auto& ic : fn->chunk.inlineCaches) {
                    if (ic.cachedMethod) GcHeap::get().markObj(ic.cachedMethod);
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
    };

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
    
    int totalArgc = static_cast<int>(args.size());

    if (fnDef->hasRestParam) {
        int fixedMax = fnDef->maxArity - 1;
        if (totalArgc < fnDef->arity) {
            throw std::runtime_error("VM Error: '" + fnDef->name + "' requires at least " + std::to_string(fnDef->arity) + " arguments.");
        }
        ObjList* restList = GcHeap::get().allocate<ObjList>();
        if (totalArgc > fixedMax) {
            int restCount = totalArgc - fixedMax;
            restList->vec.reserve(restCount);
            for (int j = 0; j < restCount; j++) {
                restList->vec.push_back(args[fixedMax + j]);
            }
        }
        
        for (int i = 0; i < std::min(totalArgc, fixedMax); ++i) {
            registers[newBase + i] = args[i];
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
            registers[newBase + i] = args[i];
        }
        for (int i = totalArgc; i < fnDef->maxArity; ++i) {
            registers[newBase + i] = Value::uninit();
        }
    }

    for (int i = fnDef->maxArity; i < fnDef->localCount; ++i) {
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

    // 获取物理寄存器或溢出槽 (Unified Address Space)
    #define getReg(idx) frameRegs[idx]

    // K-Bit 机制：解析寄存器或常量池索引 (按值返回，强制放入寄存器，消除内存间接访问)
    #define GET_RK(rk) (ISK(rk) ? ((rk) == ESCAPE_KBIT_CONST ? chunk->constants.data()[FETCH_EXTRA()] : chunk->constants.data()[INDEXK(rk)]) : ((rk) == ESCAPE_KBIT_REG ? frameRegs[FETCH_EXTRA()] : frameRegs[rk]))

    int prevLine = -1;
    int lastBrokenLine = -1;

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
                Instruction instruction = code[ip++];
                OpCode op = static_cast<OpCode>(instruction & 0xFF);
                
                int a = (instruction >> 8) & 0xFF;
                int b = (instruction >> 16) & 0xFF;
                int c = instruction >> 24;
                int bx = instruction >> 16;
                int sbx = bx - 0x7FFF;
                int sax = static_cast<int>(instruction >> 8) - 0x7FFFFF;

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
                    if (frame->classContext.isNone()) throw std::runtime_error("VM Error: '<class>' accessed outside of context.");
                    getReg(a) = frame->classContext;
                } else {
                    const std::string& name = chunk->constants.data()[ic.nameIdx].asString();
                    if (name == "<class>") {
                        ic.cachedGlobalSlot = -2;
                        if (frame->classContext.isNone()) throw std::runtime_error("VM Error: '<class>' accessed outside of context.");
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
                            clearAllGlobalICs();
                            ic.cachedGlobalSlot = newSlot;
                            getReg(a) = builtinVal;
                        } else {
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
                if (name == "<class>") throw std::runtime_error("Syntax Error: cannot override context keyword '" + name + "'.");
                
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
                                    if (uv.isGlobal) {
                                        auto it = globalNames.find(uv.name);
                                        if (it != globalNames.end()) {
                                            dummy->location = &globals[it->second];
                                        } else {
                                            Value builtinVal = getBuiltinValue(uv.name);
                                            if (builtinVal.isNone()) builtinVal = getBuiltinClosure(uv.name);
                                            globalNames[uv.name] = static_cast<uint32_t>(globals.size());
                                            globals.push_back(builtinVal.isNone() ? Value::uninit() : builtinVal);
                                            clearAllGlobalICs();
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
                                    Value builtinVal = getBuiltinValue(uv.name);
                                    if (builtinVal.isNone()) builtinVal = getBuiltinClosure(uv.name);
                                    if (!builtinVal.isNone()) {
                                        dummy->closed = builtinVal;
                                    } else {
                                        throw std::runtime_error("VM Error: Undefined variable '" + uv.name + "'.");
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
                        
                getReg(a) = Value(closure);
                        
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
                                f->closure = nullptr;
                                f->refParamsBase = -1;
                                vm->frameCount--;
                            }
                            throw;
                        }
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
                    closure->defaultValues.push_back(Value::uninit());
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
                    int64_t res = static_cast<int64_t>(vb.asInt32()) + vc.asInt32();
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value::fromInt32(static_cast<int32_t>(res)); break; }
                } else if (vb.isDouble() && vc.isDouble()) { 
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() + vc.asDoubleRaw()); break; 
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_ADD)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RADD)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb + vc;
                break;
            }
            case OpCode::SUB: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    int64_t res = static_cast<int64_t>(vb.asInt32()) - vc.asInt32();
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value::fromInt32(static_cast<int32_t>(res)); break; }
                } else if (vb.isDouble() && vc.isDouble()) { 
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() - vc.asDoubleRaw()); break; 
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_SUB)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RSUB)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb - vc;
                break;
            }
            case OpCode::MUL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInt32() && vc.isInt32()) {
                    int64_t res = static_cast<int64_t>(vb.asInt32()) * vc.asInt32();
                    if (res >= INT32_MIN && res <= INT32_MAX) { getReg(a) = Value::fromInt32(static_cast<int32_t>(res)); break; }
                } else if (vb.isDouble() && vc.isDouble()) { 
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() * vc.asDoubleRaw()); break; 
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_MUL)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RMUL)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb * vc;
                break;
            }
            case OpCode::DIV: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isDouble() && vc.isDouble()) { 
                    if (vc.asDoubleRaw() == 0.0) throw std::runtime_error("Math Error: Division by zero.");
                    getReg(a) = Value::fromDouble(vb.asDoubleRaw() / vc.asDoubleRaw()); break; 
                }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_DIV)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RDIV)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb / vc;
                break;
            }
            case OpCode::MOD: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_MOD)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RMOD)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb % vc;
                break;
            }
            case OpCode::POW: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_POW)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RPOW)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb ^ vc;
                break;
            }
            case OpCode::LDIV: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_LDIV)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RLDIV)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = ldivide(vb, vc);
                break;
            }
            case OpCode::BAND: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITAND)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RBITAND)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb & vc;
                break;
            }
            case OpCode::BOR: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITOR)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RBITOR)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb | vc;
                break;
            }
            case OpCode::BXOR: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITXOR)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RBITXOR)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = bitXor(vb, vc);
                break;
            }
            case OpCode::SHL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_LSHIFT)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RLSHIFT)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb << vc;
                break;
            }
            case OpCode::SHR: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_RSHIFT)) { getReg(a) = callDunder(vb, meth, {vc}); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_RRSHIFT)) { getReg(a) = callDunder(vc, meth, {vb}); break; } }
                getReg(a) = vb >> vc;
                break;
            }
            case OpCode::UNM: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value vb = getReg(b);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_NEG)) { getReg(a) = callDunder(vb, meth, {}); break; } }
                getReg(a) = -vb;
                break;
            }
            case OpCode::NOT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value& val = getReg(b);
                bool cond;
                if (val.isBool()) cond = val.asBool();
                else if (val.isInt32()) cond = val.asInt32() != 0;
                else if (val.isDouble()) cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw());
                else cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy();
                getReg(a) = Value(!cond);
                break;
            }
            case OpCode::BNOT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value vb = getReg(b);
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_BITNOT)) { getReg(a) = callDunder(vb, meth, {}); break; } }
                getReg(a) = ~vb;
                break;
            }
            case OpCode::TO_BOOL: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                Value& val = getReg(b);
                bool cond;
                if (val.isBool()) cond = val.asBool();
                else if (val.isInt32()) cond = val.asInt32() != 0;
                else if (val.isDouble()) cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw());
                else cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy();
                getReg(a) = Value(cond);
                break;
            }
            case OpCode::EQ: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.as_bits == vc.as_bits) {
                    getReg(a) = Value(!vb.isDouble() || !std::isnan(vb.asDoubleRaw()));
                    break;
                }
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() == vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(false); break; }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_EQ)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_EQ)) { getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, {vb}))); break; } }
                getReg(a) = Value(Value::equals(vb, vc));
                break;
            }
            case OpCode::NEQ: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.as_bits == vc.as_bits) {
                    getReg(a) = Value(vb.isDouble() && std::isnan(vb.asDoubleRaw()));
                    break;
                }
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() != vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(true); break; }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_NEQ)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_EQ)) { getReg(a) = Value(!evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_NEQ)) { getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, {vb}))); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_EQ)) { getReg(a) = Value(!evaluateTruthiness(callDunder(vc, meth, {vb}))); break; } }
                getReg(a) = Value(!Value::equals(vb, vc));
                break;
            }
            case OpCode::LT: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() < vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(vb.asInt32() < vc.asInt32()); break; }
                if (vb.isInstance()) { if (auto meth = findDunder(vb, DUNDER_LT)) { getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); break; } }
                if (vc.isInstance()) { if (auto meth = findDunder(vc, DUNDER_GT)) { getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, {vb}))); break; } }
                getReg(a) = Value(vb < vc);
                break;
            }
            case OpCode::LE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() <= vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(vb.asInt32() <= vc.asInt32()); break; }
                if (vb.isInstance()) { 
                    if (auto meth = findDunder(vb, DUNDER_LE)) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); 
                        break; 
                    }
                    if (auto methLt = findDunder(vb, DUNDER_LT)) {
                        if (evaluateTruthiness(callDunder(vb, methLt, {vc}))) {
                            getReg(a) = Value(true);
                            break;
                        }
                        if (auto methEq = findDunder(vb, DUNDER_EQ)) {
                            getReg(a) = Value(evaluateTruthiness(callDunder(vb, methEq, {vc})));
                            break;
                        }
                    }
                }
                if (vc.isInstance()) { 
                    if (auto meth = findDunder(vc, DUNDER_GE)) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, {vb}))); 
                        break; 
                    }
                    if (auto methGt = findDunder(vc, DUNDER_GT)) {
                        if (evaluateTruthiness(callDunder(vc, methGt, {vb}))) {
                            getReg(a) = Value(true);
                            break;
                        }
                        if (auto methEq = findDunder(vc, DUNDER_EQ)) {
                            getReg(a) = Value(evaluateTruthiness(callDunder(vc, methEq, {vb})));
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
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() > vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(vb.asInt32() > vc.asInt32()); break; }
                if (vb.isInstance()) { 
                    if (auto meth = findDunder(vb, DUNDER_GT)) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); 
                        break; 
                    }
                    if (auto methLt = findDunder(vb, DUNDER_LT)) {
                        if (evaluateTruthiness(callDunder(vb, methLt, {vc}))) {
                            getReg(a) = Value(false);
                            break;
                        }
                        if (auto methEq = findDunder(vb, DUNDER_EQ)) {
                            getReg(a) = Value(!evaluateTruthiness(callDunder(vb, methEq, {vc})));
                            break;
                        }
                    }
                }
                if (vc.isInstance()) { 
                    if (auto meth = findDunder(vc, DUNDER_LT)) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, {vb}))); 
                        break; 
                    }
                    if (auto methGt = findDunder(vc, DUNDER_GT)) {
                        if (evaluateTruthiness(callDunder(vc, methGt, {vb}))) {
                            getReg(a) = Value(false);
                            break;
                        }
                        if (auto methEq = findDunder(vc, DUNDER_EQ)) {
                            getReg(a) = Value(!evaluateTruthiness(callDunder(vc, methEq, {vb})));
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
                if (vb.isDouble() && vc.isDouble()) { getReg(a) = Value(vb.asDoubleRaw() >= vc.asDoubleRaw()); break; }
                if (vb.isInt32() && vc.isInt32()) { getReg(a) = Value(vb.asInt32() >= vc.asInt32()); break; }
                if (vb.isInstance()) { 
                    if (auto meth = findDunder(vb, DUNDER_GE)) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vb, meth, {vc}))); 
                        break; 
                    }
                    if (auto methLt = findDunder(vb, DUNDER_LT)) {
                        getReg(a) = Value(!evaluateTruthiness(callDunder(vb, methLt, {vc})));
                        break;
                    }
                }
                if (vc.isInstance()) { 
                    if (auto meth = findDunder(vc, DUNDER_LE)) { 
                        getReg(a) = Value(evaluateTruthiness(callDunder(vc, meth, {vb}))); 
                        break; 
                    }
                    if (auto methGt = findDunder(vc, DUNDER_GT)) {
                        getReg(a) = Value(!evaluateTruthiness(callDunder(vc, methGt, {vb})));
                        break;
                    }
                }
                getReg(a) = Value(vb >= vc);
                break;
            }
            case OpCode::IS: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value vb = GET_RK(b); Value vc = GET_RK(c);
                getReg(a) = Value(vb.as_bits == vc.as_bits);
                break;
            }
            case OpCode::JMP: {
                ip += sax;
                break;
            }
            case OpCode::JMP_TRUE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value& val = getReg(a);
                bool cond;
                if (val.isBool()) cond = val.asBool();
                else if (val.isInt32()) cond = val.asInt32() != 0;
                else if (val.isDouble()) cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw());
                else cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy();
                if (cond) ip += sbx;
                break;
            }
            case OpCode::JMP_FALSE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                Value& val = getReg(a);
                bool cond;
                if (val.isBool()) cond = val.asBool();
                else if (val.isInt32()) cond = val.asInt32() != 0;
                else if (val.isDouble()) cond = val.asDoubleRaw() != 0.0 && !std::isnan(val.asDoubleRaw());
                else cond = val.isInstance() ? evaluateTruthiness(val) : val.truthy();
                if (!cond) ip += sbx;
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
                    if (!canBeMatrixElement(v)) {
                        hasOther = true;
                    } else if (v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION) || v.isObjType(ObjType::BASENUM)) {
                        try { v.asDouble(); } catch (...) { hasOther = true; }
                    }
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
                            throw std::runtime_error("VM Error: Dimension mismatch during block matrix concatenation.");
                        }
                    } else {
                        int expectedCols = rows > 0 ? rowCols[0] : 0;
                        bool uniformCols = true;
                        for (int i = 1; i < rows; ++i) {
                            if (rowCols[i] != expectedCols) { uniformCols = false; break; }
                        }
                        if (!uniformCols) throw std::runtime_error("VM Error: Matrix rows must have the same number of columns.");

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
                if (dims == 1) {
                    Value idx = getReg(b + 1);
                    Value result;
                    if (obj.isObjType(ObjType::LIST)) {
                        auto list = static_cast<ObjList*>(obj.asObj());
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = static_cast<int>(list->vec.size());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: List index out of bounds.");
                        } else {
                            result = list->vec[i];
                        }
                    } else if (obj.isObjType(ObjType::REAL_MATRIX)) {
                        const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        } else {
                            if (m.getRows() == 1) result = Value(m(0, i));
                            else if (m.getCols() == 1) result = Value(m(i, 0));
                            else {
                                std::vector<double> row(m.getCols());
                                for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                                result = Value(RealMatrix(1, m.getCols(), row));
                            }
                        }
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        const auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        } else {
                            if (m.getRows() == 1) result = Value(m(0, i));
                            else if (m.getCols() == 1) result = Value(m(i, 0));
                            else {
                                std::vector<Complex> row(m.getCols());
                                for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                                result = Value(ComplexMatrix(1, m.getCols(), row));
                            }
                        }
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        const auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        } else {
                            if (m.getRows() == 1) result = Value(m(0, i));
                            else if (m.getCols() == 1) result = Value(m(i, 0));
                            else {
                                std::vector<std::string> row(m.getCols());
                                for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                                result = Value(StringMatrix(1, m.getCols(), row));
                            }
                        }
                    } else if (obj.isString()) {
                        ObjString* objStr = obj.asObjString();
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int len = static_cast<int>(objStr->charLength);
                        if (i < 0) i = len + i;
                        if (i < 0 || i >= len) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: String index out of bounds.");
                        } else {
                            if (objStr->isAscii) {
                                char c_str[2] = { objStr->str[i], '\0' };
                                result = Value(c_str);
                            } else {
                                result = Value(utf8::substring(objStr->str, i, 1, objStr->isAscii));
                            }
                        }
                    } else if (obj.isObjType(ObjType::DICT)) {
                        auto dict = static_cast<ObjDict*>(obj.asObj());
                        auto it = dict->keyMap.find(idx);
                        if (it == dict->keyMap.end()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::runtime_error("VM Error: Key not found.");
                        } else {
                            result = dict->elements[it->second].second;
                        }
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
                            try {
                                result = callDunder(obj, getitemMethod, {idx});
                            } catch (...) {
                                if (noThrow) result = Value::uninit();
                                else throw;
                            }
                        } else {
                            bool found = false;
                            if (idx.isString() && inst->fields) {
                                auto it = inst->fields->keyMap.find(idx);
                                if (it != inst->fields->keyMap.end()) {
                                    result = inst->fields->elements[it->second].second;
                                    found = true;
                                }
                            }
                            if (!found) {
                                if (noThrow) result = Value::uninit();
                                else throw std::runtime_error("VM Error: Cannot index this instance (no __getitem__).");
                            }
                        }
                    } else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        if (!idx.isString()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::runtime_error("VM Error: Namespace keys must be strings.");
                        } else {
                            auto it = ns->fields.find(idx.asString());
                            if (it == ns->fields.end()) {
                                if (noThrow) result = Value::uninit();
                                else throw std::runtime_error("VM Error: Key not found in namespace.");
                            } else {
                                result = *(it->second.upval->location);
                            }
                        }
                    } else {
                        if (noThrow) result = Value::uninit();
                        else throw std::runtime_error("VM Error: Unsupported 1D index get.");
                    }
                    getReg(a) = result;
                } else if (dims == 2) {
                    Value row = getReg(b + 1);
                    Value col = getReg(b + 2);
                    int r = static_cast<int>(std::round(row.asDouble()));
                    int c_idx = static_cast<int>(std::round(col.asDouble()));
                    Value result;
                    if (obj.isObjType(ObjType::REAL_MATRIX)) {
                        const auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        } else {
                            result = Value(m(r, c_idx));
                        }
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        const auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        } else {
                            result = Value(m(r, c_idx));
                        }
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        const auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) {
                            if (noThrow) result = Value::uninit();
                            else throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        } else {
                            result = Value(m(r, c_idx));
                        }
                    } else {
                        if (noThrow) result = Value::uninit();
                        else throw std::runtime_error("VM Error: Unsupported 2D index get.");
                    }
                    getReg(a) = result;
                } else {
                    throw std::runtime_error("VM Error: Unsupported index dimensionality.");
                }
                break;
            }
            case OpCode::INDEX_SET: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                Value obj = getReg(a);
                Value val = getReg(a + c + 1);
                
                if (c == 1) {
                    Value idx = getReg(a + 1);
                    if (obj.isObjType(ObjType::LIST)) {
                        auto list = static_cast<ObjList*>(obj.asObj());
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = static_cast<int>(list->vec.size());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("VM Error: List index out of bounds.");
                        list->mut()[i] = val;
                    } else if (obj.isObjType(ObjType::REAL_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(RealMatrix(static_cast<ObjRealMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjRealMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        
                        if (m.getRows() == 1) m(0, i) = val.asDouble();
                        else if (m.getCols() == 1) m(i, 0) = val.asDouble();
                        else {
                            if (val.isObjType(ObjType::REAL_MATRIX)) {
                                const auto& src = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = src(0, j);
                                } else throw std::runtime_error("VM Error: Matrix row assignment dimension mismatch.");
                            } else throw std::runtime_error("VM Error: Matrix row assignment requires a row vector.");
                        }
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        
                        if (m.getRows() == 1) m(0, i) = val.asComplex();
                        else if (m.getCols() == 1) m(i, 0) = val.asComplex();
                        else {
                            if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
                                const auto& src = static_cast<ObjComplexMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = src(0, j);
                                } else throw std::runtime_error("VM Error: Matrix row assignment dimension mismatch.");
                            } else if (val.isObjType(ObjType::REAL_MATRIX)) {
                                const auto& src = static_cast<ObjRealMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = Complex(src(0, j));
                                } else throw std::runtime_error("VM Error: Matrix row assignment dimension mismatch.");
                            } else throw std::runtime_error("VM Error: Matrix row assignment requires a row vector.");
                        }
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        int i = idx.isInt32() ? idx.asInt32() : static_cast<int>(idx.asDouble());
                        int n = (m.getRows() == 1) ? m.getCols() : ((m.getCols() == 1) ? m.getRows() : m.getRows());
                        if (i < 0) i = n + i;
                        if (i < 0 || i >= n) throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        
                        if (m.getRows() == 1) m(0, i) = val.asString();
                        else if (m.getCols() == 1) m(i, 0) = val.asString();
                        else {
                            if (val.isObjType(ObjType::STRING_MATRIX)) {
                                const auto& src = static_cast<ObjStringMatrix*>(val.asObj())->mat;
                                if (src.getRows() == 1 && src.getCols() == m.getCols()) {
                                    for (int j = 0; j < m.getCols(); ++j) m(i, j) = src(0, j);
                                } else throw std::runtime_error("VM Error: Matrix row assignment dimension mismatch.");
                            } else throw std::runtime_error("VM Error: Matrix row assignment requires a row vector.");
                        }
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::DICT)) {
                        auto dict = static_cast<ObjDict*>(obj.asObj());
                        dict->set(idx, val);
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
                            if (idx.isString()) {
                                if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
                                inst->fields->set(idx, val);
                            } else {
                                throw std::runtime_error("VM Error: Cannot assign index on this instance (no __setitem__).");
                            }
                        }
                    } else if (obj.isObjType(ObjType::NAMESPACE)) {
                        auto ns = static_cast<ObjNamespace*>(obj.asObj());
                        if (ns->is_frozen) throw std::runtime_error("VM Error: Cannot modify frozen namespace.");
                        if (!idx.isString()) throw std::runtime_error("VM Error: Namespace keys must be strings.");
                        std::string key = idx.asString();
                        auto it = ns->fields.find(key);
                        if (it != ns->fields.end()) {
                            if (it->second.isConst) throw std::runtime_error("VM Error: Cannot modify const field '" + key + "'.");
                            *(it->second.upval->location) = val;
                        } else {
                            ObjUpVal* uv = GcHeap::get().allocate<ObjUpVal>();
                            uv->closed = val;
                            uv->location = &uv->closed;
                            ns->fields[key] = { uv, false };
                        }
                    } else {
                        throw std::runtime_error("VM Error: Unsupported 1D index set.");
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
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        m(r, c_idx) = val.asDouble();
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::COMPLEX_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(ComplexMatrix(static_cast<ObjComplexMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjComplexMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        m(r, c_idx) = val.asComplex();
                        getReg(a) = obj;
                    } else if (obj.isObjType(ObjType::STRING_MATRIX)) {
                        if (obj.asObj()->refCount > 2) obj = Value(StringMatrix(static_cast<ObjStringMatrix*>(obj.asObj())->mat));
                        auto& m = static_cast<ObjStringMatrix*>(obj.asObj())->mat;
                        if (r < 0) r = m.getRows() + r;
                        if (c_idx < 0) c_idx = m.getCols() + c_idx;
                        if (r < 0 || r >= m.getRows() || c_idx < 0 || c_idx >= m.getCols()) throw std::out_of_range("VM Error: Matrix index out of bounds.");
                        if (val.isString()) m(r, c_idx) = val.asString();
                        else { std::ostringstream oss; oss << val; m(r, c_idx) = oss.str(); }
                        getReg(a) = obj;
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
                    auto method = findDunder(iterable, DUNDER_ITER);
                    if (method) {
                        Value iterObj = callDunder(iterable, method, {});
                        GcValueGuard iterGuard(iterObj);
                        ObjList* state = GcHeap::get().allocate<ObjList>();
                        state->vec.push_back(iterObj);
                        if (iterObj.isInstance() && iterObj.asInstance()->c_nativeNext) {
                            state->vec.push_back(Value::none());
                        } else {
                            auto nextMethod = findDunder(iterObj, DUNDER_NEXT);
                            if (!nextMethod) throw std::runtime_error("VM Error: Iterator missing __next__ method.");
                            state->vec.push_back(Value(nextMethod));
                        }
                        getReg(a) = Value(state);
                        break;
                    }
                }
                
                if (iterable.isObjType(ObjType::LIST) || iterable.isString() || 
                    iterable.isObjType(ObjType::REAL_MATRIX) || iterable.isObjType(ObjType::COMPLEX_MATRIX) || 
                    iterable.isObjType(ObjType::STRING_MATRIX)) {
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
                } else if (iterable.isObjType(ObjType::SET)) {
                    const auto* s = static_cast<ObjSet*>(iterable.asObj());
                    for (const auto& val : s->elements) {
                        elements->vec.push_back(val);
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
                        } else if (iterTarget.isObjType(ObjType::STRING_MATRIX)) {
                            const auto& m = static_cast<ObjStringMatrix*>(iterTarget.asObj())->mat;
                            int len = (m.getRows() == 1) ? m.getCols() : m.getRows();
                            if (i >= len) {
                                getReg(a) = Value::uninit();
                            } else {
                                if (m.getRows() == 1) getReg(a) = Value(m(0, i));
                                else if (m.getCols() == 1) getReg(a) = Value(m(i, 0));
                                else {
                                    std::vector<std::string> row(m.getCols());
                                    for (int j = 0; j < m.getCols(); ++j) row[j] = m(i, j);
                                    getReg(a) = Value(StringMatrix(1, m.getCols(), row));
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
                    Value nextVal = callDunder(iterObj, method, {});
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
            case OpCode::METHOD: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                const std::string& methodName = chunk->constants.data()[b].asString();
                Value classVal = getReg(a);
                Value closureVal = getReg(c);
                
                if (!classVal.isClass()) throw std::runtime_error("VM Error: METHOD requires a class.");
                auto cls = static_cast<ObjClass*>(classVal.asObj());
                
                if (closureVal.isFunctionClosure()) {
                    cls->methods[methodName] = closureVal.asFunction();
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
                
                sub->parent = sup;
                for (auto& [name, method] : sup->methods) {
                    if (sub->methods.find(name) == sub->methods.end()) {
                        sub->methods[name] = method;
                    }
                }
                break;
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
                    if (ic.cachedClassId == inst->classDef->classId && ic.cachedFieldIndex != -1 && inst->fields && ic.cachedFieldIndex < static_cast<int>(inst->fields->elements.size())) {
                        if (inst->fields->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                            getReg(a) = inst->fields->elements[ic.cachedFieldIndex].second;
                            break;
                        }
                    }
                    if (inst->fields) {
                        auto it = inst->fields->keyMap.find(keyVal);
                        if (it != inst->fields->keyMap.end()) {
                            getReg(a) = inst->fields->elements[it->second].second;
                            ic.cachedClassId = inst->classDef->classId;
                            ic.cachedFieldIndex = static_cast<int>(it->second);
                            break;
                        }
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
                        bound->boundClass = Value(inst->classDef);
                        result = Value(bound);
                        found = true;
                    }
                    if (!found) {
                        auto cls = inst->classDef;
                        while (cls) {
                            auto it = cls->methods.find(field);
                            if (it != cls->methods.end()) {
                                auto rawMethod = it->second;
                                ic.cachedClassId = inst->classDef->classId;
                                ic.cachedMethod = rawMethod;
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
                                bound->boundClass = Value(cls);
                                result = Value(bound);
                                found = true;
                                break;
                            }
                            cls = cls->parent;
                        }
                        if (!found) {
                            auto getattrMethod = findDunder(obj, "__getattr__");
                            if (getattrMethod) {
                                result = callDunder(obj, getattrMethod, {Value(field)});
                                found = true;
                            }
                        }
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

                if (!found) throw std::runtime_error("VM Error: Property '" + field + "' not found.");
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
                    if (ic.cachedClassId == inst->classDef->classId && ic.cachedFieldIndex != -1 && inst->fields && ic.cachedFieldIndex < static_cast<int>(inst->fields->elements.size())) {
                        if (inst->fields->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                            getReg(a) = inst->fields->elements[ic.cachedFieldIndex].second;
                            break;
                        }
                    }
                    if (inst->fields) {
                        auto it = inst->fields->keyMap.find(keyVal);
                        if (it != inst->fields->keyMap.end()) {
                            getReg(a) = inst->fields->elements[it->second].second;
                            ic.cachedClassId = inst->classDef->classId;
                            ic.cachedFieldIndex = static_cast<int>(it->second);
                            break;
                        }
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
                        bound->boundClass = Value(inst->classDef);
                        result = Value(bound);
                        found = true;
                    }
                    if (!found) {
                        auto cls = inst->classDef;
                        while (cls) {
                            auto it = cls->methods.find(field);
                            if (it != cls->methods.end()) {
                                auto rawMethod = it->second;
                                ic.cachedClassId = inst->classDef->classId;
                                ic.cachedMethod = rawMethod;
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
                                bound->boundClass = Value(cls);
                                result = Value(bound);
                                found = true;
                                break;
                            }
                            cls = cls->parent;
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
                } else if (obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    auto it = ns->fields.find(field);
                    if (it != ns->fields.end()) {
                        result = *(it->second.upval->location);
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
                    getReg(a) = result;
                } else {
                    getReg(a) = Value::none();
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
                    inst->checkModify();
                    auto setattrMethod = findDunder(obj, DUNDER_SETATTR);
                    if (setattrMethod) {
                        callDunder(obj, setattrMethod, {keyVal, val});
                    } else {
                        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
                        if (ic.cachedClassId == inst->classDef->classId && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(inst->fields->elements.size())) {
                            if (inst->fields->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                                inst->fields->elements[ic.cachedFieldIndex].second = val;
                                goto set_prop_done;
                            }
                        }
                        {
                            auto it = inst->fields->keyMap.find(keyVal);
                            if (it != inst->fields->keyMap.end()) {
                                inst->fields->elements[it->second].second = val;
                                ic.cachedClassId = inst->classDef->classId;
                                ic.cachedFieldIndex = static_cast<int>(it->second);
                            } else {
                                ic.cachedClassId = inst->classDef->classId;
                                ic.cachedFieldIndex = static_cast<int>(inst->fields->elements.size());
                                inst->fields->keyMap[keyVal] = inst->fields->elements.size();
                                inst->fields->elements.push_back({keyVal, val});
                            }
                        }
                    set_prop_done:;
                    }
                } else if (obj.isObjType(ObjType::DICT)) {
                    auto d = static_cast<ObjDict*>(obj.asObj());
                    if (ic.cachedBuiltinType == BuiltinType::DICT && ic.cachedFieldIndex != -1 && ic.cachedFieldIndex < static_cast<int>(d->elements.size())) {
                        if (d->elements[ic.cachedFieldIndex].first.as_bits == keyVal.as_bits) {
                            d->elements[ic.cachedFieldIndex].second = val;
                            goto set_prop_dict_done;
                        }
                    }
                    {
                        auto it = d->keyMap.find(keyVal);
                        if (it != d->keyMap.end()) {
                            d->elements[it->second].second = val;
                            ic.cachedBuiltinType = BuiltinType::DICT;
                            ic.cachedFieldIndex = static_cast<int>(it->second);
                        } else {
                            ic.cachedBuiltinType = BuiltinType::DICT;
                            ic.cachedFieldIndex = static_cast<int>(d->elements.size());
                            d->keyMap[keyVal] = d->elements.size();
                            d->elements.push_back({keyVal, val});
                        }
                    }
                set_prop_dict_done:;
                } else if (obj.isObjType(ObjType::NAMESPACE)) {
                    auto ns = static_cast<ObjNamespace*>(obj.asObj());
                    if (ns->is_frozen) throw std::runtime_error("VM Error: Cannot modify frozen namespace.");
                    const std::string& field = keyVal.asString();
                    auto it = ns->fields.find(field);
                    if (it != ns->fields.end()) {
                        if (it->second.isConst) throw std::runtime_error("VM Error: Cannot modify const field '" + field + "'.");
                        *(it->second.upval->location) = val;
                    } else {
                        ObjUpVal* uv = GcHeap::get().allocate<ObjUpVal>();
                        uv->closed = val;
                        uv->location = &uv->closed;
                        ns->fields[field] = { uv, false };
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
            case OpCode::LIST_COMP_END: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
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
                    if (!canBeMatrixElement(v)) {
                        hasOther = true;
                    } else if (v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION) || v.isObjType(ObjType::BASENUM)) {
                        try { v.asDouble(); } catch (...) { hasOther = true; }
                    }
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
                        throw std::runtime_error("VM Error: Dimension mismatch during list comprehension matrix concatenation.");
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                execSliceGet(a, b, static_cast<uint8_t>(c));
                break;
            }
            case OpCode::SLICE_SET: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                execSliceSet(a, c, static_cast<uint8_t>(c));
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
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                execAssertReturnType(getReg(a), b);
                break;
            }
            case OpCode::MATCH_TYPE: {
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[c]);
                if (ic.cachedBuiltinType == BuiltinType::UNKNOWN) {
                    ic.cachedBuiltinType = parseBuiltinType(chunk->constants.data()[ic.nameIdx].asString());
                }
                const std::string& typeStr = chunk->constants.data()[ic.nameIdx].asString();
                getReg(a) = Value(checkValueType(getReg(b), ic.cachedBuiltinType, typeStr));
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                bool isTailCall = (op == OpCode::TAIL_INVOKE);
                frame->ip = ip;
                int prevIp = ip;
                
                InlineCache& ic = const_cast<InlineCache&>(chunk->inlineCaches.data()[c]);
                std::string methodName = chunk->constants.data()[ic.nameIdx].asString();
                
                execInvoke(a, b, c, isTailCall, -1);
                
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
                execInvoke(a, b, c, isTailCall, 1);
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
                    auto it = cls->methods.find(field);
                    if (it != cls->methods.end()) {
                        rawMethod = it->second;
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
                if (a == ESCAPE_NORMAL_8) a = FETCH_EXTRA();
                if (b == ESCAPE_NORMAL_8) b = FETCH_EXTRA();
                if (c == ESCAPE_NORMAL_8) c = FETCH_EXTRA();
                
                bool isTailCall = (op == OpCode::TAIL_SUPER_INVOKE);
                frame->ip = ip;
                int prevIp = ip;
                execSuperInvoke(a, b, c, isTailCall);
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
                
                bool isTailCall = (op == OpCode::TAIL_CALL);
                frame->ip = ip; // 保存当前 IP
                int prevIp = ip;
                execCall(b, c, a, isTailCall);
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

} // namespace jc
