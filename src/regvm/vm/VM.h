#ifndef JC2_REGVM_VM_H
#define JC2_REGVM_VM_H

#include "Bytecode.h"
#include "../../memory/Value.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <set>

namespace jc {
namespace regvm {

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
    Value selfContext = Value::none();
    Value classContext = Value::none();
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
    std::unordered_set<std::string> importedModules;

    std::vector<std::shared_ptr<CompiledFunction>> compiledFunctions;
    std::vector<std::pair<int, ObjUpVal*>> pendingCallRefs;

    ObjUpVal* openUpvalues = nullptr;
    void closeUpvalues(int lastRegIndex);
    ObjUpVal* captureUpvalue(int regIndex);

    struct ExceptionHandler {
        int frameIndex = 0;
        int ip = 0;
        int registerBase = 0;
        int errReg = 0;
    };
    std::vector<ExceptionHandler> exceptionHandlers;

    int currentTargetFrameDepth = 0;

    Value run(int targetFrameDepth = 0);
    bool handleExceptionUnwind(Value errVal);

    void execCall(int a, int b, bool isTailCall = false);
    void populateRefParams(CallFrame& newFrame, const CompiledFunction* fn);

    void execInvoke(int a, int b, uint32_t icIdx, bool isTailCall, int fbType, uint32_t fbIdx);
    void execSuperInvoke(int a, int b, uint32_t nameIdx, bool isTailCall);
    void execSliceGet(int a, int b, uint8_t dims);
    void execSliceSet(int a, int c, uint8_t dims);
    Value execImport(const std::string& name);

    void execAssertParamType(const Value& val, uint32_t icIdx, uint32_t nameIdx);
    void execAssertReturnType(const Value& val, uint32_t icIdx);

    bool checkValueType(const Value& val, BuiltinType btype, const std::string& typeStr);
    std::string getTypeName(const Value& val);
    ObjClosure* findDunder(const Value& val, const std::string& name);
    Value callDunder(const Value& obj, ObjClosure* method, const std::vector<Value>& args);
    bool evaluateTruthiness(const Value& val);

public:
    VM();
    ~VM();

    void clearAllGlobalICs() {
        for (auto& fn : compiledFunctions) {
            for (auto& ic : fn->chunk.inlineCaches) {
                ic.cachedGlobalSlot = -1;
            }
        }
    }

    void clearGlobals() {
        globals.clear();
        globalNames.clear();
        constGlobals.clear();
        importedModules.clear();
        loadedModules.clear();
        openUpvalues = nullptr;
        clearAllGlobalICs();
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

    void setGlobal(const std::string& name, const Value& val) {
        auto it = globalNames.find(name);
        if (it != globalNames.end()) {
            globals[it->second] = val;
        } else {
            globalNames[name] = static_cast<uint32_t>(globals.size());
            globals.push_back(val);
        }
    }

    void setCompiledFunctions(const std::vector<std::shared_ptr<CompiledFunction>>& fns) {
        compiledFunctions = fns;
    }
    
    std::vector<std::shared_ptr<CompiledFunction>>& getCompiledFunctions() {
        return compiledFunctions;
    }

    Value execute(const Chunk& mainChunk, int localCount);
};

} // namespace regvm
} // namespace jc

#endif // JC2_REGVM_VM_H
