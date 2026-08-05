#ifndef JC2_VM_H
#define JC2_VM_H

#include "Bytecode.h"
#include "../memory/Value.h"
#include "BuiltinRegistry.h"
#include "../frontend/Token.h"
#include "../jit/backend/ExecutableMemory.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <set>
#include <utility>

namespace jc {

struct ValueException : public std::exception {
    Value val;
    explicit ValueException(Value v) : val(std::move(v)) {}
    const char* what() const noexcept override { return "ValueException"; }
};

// ============================================================================
// 寄存器机调用帧 (Register Window Frame)
// ============================================================================
struct CallFrame {
    const CompiledFunction* function = nullptr;
    const Chunk* chunk = nullptr;
    int ip = 0;
    int registerBase = 0; // 寄存器窗口基址 (指向全局 registers 数组)
    int returnRegister = 0; // ★ 记录当前帧返回时，结果应写入父帧的哪个物理寄存器
    ObjClosure* closure = nullptr;
    int refParamsBase = -1;
    int deferBase = 0;
    Value selfContext = Value::none();
    Value classContext = Value::none();
    
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    double childTimeMs = 0.0;
    uint64_t instructionCount = 0;
};

// ============================================================================
// 寄存器虚拟机核心引擎
// ============================================================================
class VM {
private:
    // 统一地址空间：100万个槽位，足以容纳极深的调用栈和海量的溢出槽
    static constexpr int MAX_REGISTERS = 1024 * 1024; 
    static constexpr int MAX_FRAMES = 1024;

    Value* registers = nullptr;
    CallFrame* frames = nullptr;
    int frameCount = 0;

    std::vector<Value> globals;
    std::unordered_map<std::string, uint32_t> globalNames;
    std::unordered_set<std::string> constGlobals;
    std::unordered_map<std::string, Value> loadedModules;
    std::unordered_map<std::string, Value> builtinModules;
    std::unordered_set<std::string> importedModules;

    std::vector<std::shared_ptr<CompiledFunction>> compiledFunctions;
    std::unordered_map<int, std::shared_ptr<jit::ExecutableMemory>> jitCompiledCode;
    std::unordered_map<int, void*> jitEntryPoints;
    
    // ★ JIT OSR Profiling (Step 79)
    std::unordered_map<int, std::unordered_map<int, std::shared_ptr<jit::ExecutableMemory>>> osrCompiledCode;
    std::unordered_map<int, std::unordered_map<int, void*>> osrEntryPoints;
    void compileForOSR(int fnIdx, int loopHeaderIp);

    std::vector<std::pair<int, ObjUpVal*>> pendingCallRefs;
    std::vector<ObjClosure*> deferStack;
    void runDefersDownTo(int targetBase, Value* currentException = nullptr);

    std::unordered_map<std::string, NativeCallable> nativeBuiltins;
    std::unordered_map<std::string, std::set<int>> builtinArity;
    std::unordered_map<std::string, std::vector<std::string>> builtinParamNames;
    std::unordered_map<std::string, Value> builtinClosures;
    std::unordered_map<std::string, Value> builtinValues;

    ObjUpVal* openUpvalues = nullptr;
    void closeUpvalues(int lastRegIndex);
    ObjUpVal* captureUpvalue(int regIndex);

public:
    ObjUpVal* captureUpvaluePublic(int regIndex) { return captureUpvalue(regIndex); }

private:
    struct ExceptionHandler {
        int frameIndex = 0;
        int ip = 0;
        int registerBase = 0;
        int errReg = 0;
        int deferBase = 0;
    };
    std::vector<ExceptionHandler> exceptionHandlers;

    int currentTargetFrameDepth = 0;

    int pendingFrameBase = -1;
    int pendingFrameCount = 0;

    struct PendingFrameGuard {
        VM* vm;
        int oldBase;
        int oldCount;
        PendingFrameGuard(VM* v, int base, int count) : vm(v), oldBase(v->pendingFrameBase), oldCount(v->pendingFrameCount) {
            vm->pendingFrameBase = base;
            vm->pendingFrameCount = count;
        }
        ~PendingFrameGuard() {
            vm->pendingFrameBase = oldBase;
            vm->pendingFrameCount = oldCount;
        }
    };

    struct ProfileRecord {
        int callCount = 0;
        double totalTimeMs = 0.0;
        double selfTimeMs = 0.0;
        uint64_t instructionCount = 0;
    };
    std::unordered_map<std::string, ProfileRecord> profileData;
    void profileFrameStart(CallFrame* frame);
    void profileFrameEnd(CallFrame* frame);

    Value run(int targetFrameDepth = 0);
    bool handleExceptionUnwind(Value* errValPtr);
    std::string buildStackTrace() const;
    Value wrapException(const std::string& type, Value val);
    std::string formatException(const Value& errVal);

    void execCall(int calleeReg, int argc, int kwArgc, int dstReg, bool isTailCall = false);
    void populateRefParams(CallFrame& newFrame, const CompiledFunction* fn);

    void execInvoke(int a, int b, int kwArgc, uint32_t icIdx, bool isTailCall, int fbType, bool isPrivate = false);
    void execSuperInvoke(int a, int b, int kwArgc, uint32_t nameIdx, bool isTailCall);
    Value execImport(const std::string& name);

    void execAssertParamType(const Value& val, int paramIdx, uint32_t nameIdx);
    void execAssertReturnType(const Value& val);

    std::vector<Value> alignArguments(int posArgc, int kwArgc, Value* argsBase, const std::vector<std::string>& paramNames, bool hasRestParam, Value boundSelf = Value::none());

    std::string getTypeName(const Value& val);
    bool evaluateTruthiness(const Value& val);

public:
    bool checkValueType(const Value& val, ObjTypeDef* td);
    std::pair<ObjClosure*, ObjClass*> findDunder(const Value& val, const std::string& name);
    Value callDunder(const Value& obj, ObjClosure* method, ObjClass* ownerClass, const std::vector<Value>& args);
    VM();
    ~VM();

    void shutdown() {
        clearGlobals();
        builtinModules.clear();
        loadedModules.clear();
        builtinValues.clear();
        compiledFunctions.clear();
        pendingCallRefs.clear();
    }

    void clearAllGlobalICs() {
        for (auto& fn : compiledFunctions) {
            for (auto& ic : fn->chunk.inlineCaches) {
                ic.cachedGlobalSlot = -1;
                ic.cachedClass = nullptr;
            }
        }
    }

    void clearGlobals() {
        globals.clear();
        globalNames.clear();
        constGlobals.clear();
        importedModules.clear();
        loadedModules = builtinModules;
        builtinClosures.clear();
        openUpvalues = nullptr;
        deferStack.clear();
        comptimeGlobals.clear();
        clearAllGlobalICs();
    }

    int parsingDepth = 0;
    std::vector<std::string> comptimeGlobals;
    void cleanupComptimeGlobals(size_t restoreToSize) {
        while (comptimeGlobals.size() > restoreToSize) {
            removeGlobal(comptimeGlobals.back());
            comptimeGlobals.pop_back();
        }
    }

    void removeGlobal(const std::string& name) {
        auto it = globalNames.find(name);
        if (it != globalNames.end()) {
            globals[it->second] = Value::none();
            globalNames.erase(it);
        }
        constGlobals.erase(name);
        clearAllGlobalICs();
    }

    std::unordered_map<std::string, Value> getGlobals() const {
        std::unordered_map<std::string, Value> res;
        for (const auto& [k, v] : globalNames) res[k] = globals[v];
        return res;
    }

    Value getGlobal(const std::string& name) const {
        auto it = globalNames.find(name);
        if (it != globalNames.end()) {
            return globals[it->second];
        }
        return Value::none();
    }

    void setGlobalSlot(uint32_t slot, const Value& val) {
        if (slot < globals.size()) globals[slot] = val;
    }

    void setGlobal(const std::string& name, const Value& val) {
        auto it = globalNames.find(name);
        if (it != globalNames.end()) {
            globals[it->second] = val;
        } else {
            globalNames[name] = static_cast<uint32_t>(globals.size());
            globals.push_back(val);
        }
    }

    void registerBuiltin(const std::string& name, NativeCallable fn, std::set<int> arity, std::vector<std::string> paramNames = {});
    Value getBuiltinClosure(const std::string& name);
    void registerBuiltinValue(const std::string& name, const Value& val) { builtinValues[name] = val; }
    void injectModule(const std::string& name, const Value& moduleVal) { 
        loadedModules[name] = moduleVal; 
        builtinModules[name] = moduleVal;
    }
    Value getBuiltinValue(const std::string& name) const {
        auto it = builtinValues.find(name);
        return it != builtinValues.end() ? it->second : Value::none();
    }
    const std::unordered_map<std::string, NativeCallable>& getNativeBuiltins() const { return nativeBuiltins; }
    const std::unordered_map<std::string, std::set<int>>& getBuiltinArity() const { return builtinArity; }

    void* getJitEntryPoint(int fnIdx) const {
        auto it = jitEntryPoints.find(fnIdx);
        return it != jitEntryPoints.end() ? it->second : nullptr;
    }

    void setCompiledFunctions(const std::vector<std::shared_ptr<CompiledFunction>>& fns) {
        compiledFunctions = fns;
    }
    
    std::vector<std::shared_ptr<CompiledFunction>>& getCompiledFunctions() {
        return compiledFunctions;
    }

    Value execute(const Chunk& mainChunk, int localCount);
    void execCompileTimeImport(const std::string& name);

    Value makeTokenInstance(const Token& t);

    ObjClass* listProto = nullptr;
    ObjClass* dictProto = nullptr;
    ObjClass* setProto = nullptr;
    ObjClass* stringProto = nullptr;
    ObjClass* matrixProto = nullptr;

    static inline VM* activeVM = nullptr;
    Value callVMFunction(int fnIdx, const std::vector<Value>& args, ObjClosure* closure = nullptr, Value boundSelf = Value::none(), Value boundClass = Value::none());

    void triggerDebugger();
    CallFrame* currentDebuggerFrame = nullptr;

    CallFrame* getCurrentFrame() { return frameCount > 0 ? &frames[frameCount - 1] : nullptr; }
    Value* getRegisters() { return registers; }
    
    void printProfileInfo();
    void clearProfileData() { profileData.clear(); }

    struct WatchPoint {
        int reg = -1;
        std::string op;
        double val = 0.0;
    };
    std::vector<WatchPoint> watchpoints;
    std::set<int> breakpoints;
};

// JIT Runtime Helpers (Step 84)
uint64_t jc2_jit_build_list(uint32_t startReg, uint32_t count);
uint64_t jc2_jit_build_dict(uint32_t startReg, uint32_t count);
uint64_t jc2_jit_build_set(uint32_t startReg, uint32_t count);
uint64_t jc2_jit_build_matrix(uint32_t startReg, uint32_t shapeIdx, const Chunk* chunk);
uint64_t jc2_jit_build_slice(uint32_t startReg);
uint64_t jc2_jit_build_class(uint32_t nameIdx, const Chunk* chunk);
uint64_t jc2_jit_build_namespace(uint32_t startReg, uint32_t count, uint32_t nameIdx, const Chunk* chunk, uint32_t registerOffset);
uint64_t jc2_jit_concat_strings(uint32_t startReg, uint32_t count);
uint64_t jc2_jit_format_string(uint32_t valReg, uint32_t specIdx, const Chunk* chunk);
uint64_t jc2_jit_dict_rest(uint32_t objReg, uint32_t excludeKeysReg);
uint64_t jc2_jit_closure(uint32_t fnIdx, uint32_t registerOffset);
void jc2_jit_assign_value(uint32_t regIndex, uint64_t src_bits);
void jc2_jit_assign_global(uint32_t slot, uint64_t src_bits);

} // namespace jc

#endif // JC2_VM_H
