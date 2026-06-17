#ifndef JC2_VM_H
#define JC2_VM_H

#include "Bytecode.h"
#include "../memory/Value.h"
#include <chrono>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstring>

namespace jc {

    using NativeCallable = std::function<Value(const std::vector<Value>&)>;

    class VM {
    private:

        std::vector<std::pair<int, ObjUpVal*>> pendingCallRefs;

        // ★ 多帧栈：支持嵌套函数调用
        CallFrame* frames = nullptr;
        int frameCount = 0;
        static constexpr int MAX_FRAMES = 1024;

        CallFrame& frame() { return frames[frameCount - 1]; }
        const Chunk& currentChunk() { return frame().function->chunk; }

        Value* stack = nullptr;
        Value* stackTop = nullptr;
        Value* stackLimit = nullptr;
        static constexpr int MAX_STACK = 65536;

        std::vector<Value> globalValues;
        std::unordered_map<std::string, uint32_t> globalNamesToSlots;
        std::unordered_map<std::string, NativeCallable> nativeBuiltins;
        std::unordered_map<std::string, Value> builtinClosures;    // ★ 新增：内置函数闭包缓存
        std::unordered_set<std::string> constGlobals;              // ★ 新增：const 变量追踪

        // ★ 存储编译后的函数对象
        std::vector<std::shared_ptr<CompiledFunction>> compiledFunctions;

        // 栈操作 (Inline 优化)
        inline void push(const Value& val) {
            if (stackTop >= stackLimit)
                throw std::runtime_error("VM Error: Stack overflow.");
            *stackTop++ = val;
        }
        inline void push(Value&& val) {
            if (stackTop >= stackLimit)
                throw std::runtime_error("VM Error: Stack overflow.");
            *stackTop++ = std::move(val);
        }
        inline Value pop() {
            stackTop--;
            Value val = std::move(*stackTop);
            return val;
        }
        inline Value& peek(int distance = 0) {
            return *(stackTop - 1 - distance);
        }
        
        inline size_t getStackSize() const {
            return static_cast<size_t>(stackTop - stack);
        }
        inline void setStackSize(size_t newSize) {
            while (getStackSize() > newSize) {
                pop();
            }
            stackTop = stack + newSize;
        }
        inline void eraseStack(int indexFromTop) {
            Value* target = stackTop - 1 - indexFromTop;
            if (target < stackTop - 1) {
                std::memmove(target, target + 1, (stackTop - 1 - target) * sizeof(Value));
            }
            stackTop--;
            *stackTop = Value::none();
        }
        inline void insertStack(int indexFromTop, const Value& val) {
            Value* target = stackTop - indexFromTop;
            if (target < stackTop) {
                std::memmove(target + 1, target, (stackTop - target) * sizeof(Value));
            }
            *target = val;
            stackTop++;
        }

        inline static bool isTruthy(const Value& val) { return val.truthy(); }

        // ★ 执行主循环
        Value run(int targetFrameDepth = 0);
        int currentTargetFrameDepth = 0;

        struct ExceptionHandler {
            int frameIndex = 0;     // 哪个 CallFrame
            int ip = 0;             // catch 块的起始地址
            int stackSize = 0;      // 进入 try 时的栈大小
            std::string catchVarName = ""; // catch 变量名（空则不绑定）
        };
        std::vector<ExceptionHandler> exceptionHandlers;

        // ★ 开放上值追踪 (Open Upvalues)
        ObjUpVal* openUpvalues = nullptr; // 改为侵入式链表
        void closeUpvalues(int lastStackIndex);
        ObjUpVal* captureUpvalue(Value* local);

        // ==============================================================
        // ★ 新增：干净统一的异常回滚处理与栈轨迹抓取 (Stack Trace)
        // ==============================================================
        bool handleExceptionUnwind(std::string& msg);
        std::string buildStackTrace(const std::string& errorMsg);
        Value callDunder(const Value& obj, const std::string& name,
            const std::vector<Value>& args);

        // ★ 类型检查冷路径：让繁重的字符串操作离开核心循环
        [[noreturn]] void triggerParamTypeError(const Value& val, uint32_t icIdx, uint32_t nameIdx);
        [[noreturn]] void triggerReturnTypeError(const Value& val, uint32_t icIdx);

        std::string getTypeName(const Value& val);
        bool checkValueType(const Value& val, BuiltinType btype, const std::string& typeStr);

        std::unordered_map<std::string, std::set<int>> builtinArity;  // ★ 新增
        std::unordered_set<std::string> importedModules;               // ★ 防重复导入
        std::unordered_map<std::string, Value> loadedModules;          // ★ 缓存模块的 Namespace

        int currentLine();

        // ★ 调试器专属状态
        bool debugMode = false;
        bool stepNextLine = false;
        int lastDebugLine = -1;
        std::set<int> breakpoints;
        void debugPrompt(); // 交互式调试终端

        //★ 性能探针 Profiler 专属状态
        bool profileMode = false;

        // 统计每种 OpCode 的执行总次数
        std::map<OpCode, uint64_t> opCounts;

        // 统计每个函数的调用次数和总耗时
        struct FuncProfile {
            uint64_t callCount = 0;
            double totalTimeMs = 0.0;
        };
        std::map<std::string, FuncProfile> funcProfiles;
        // 当一次完整的脚本执行完，打印报告
           // ═══ 垃圾回收器 (Mark-and-Sweep GC) ═══
        void collectGarbage();
        void markValue(const Value& val);
        void markObject(Obj* obj);
        void traceReferences();
        std::vector<Obj*> grayStack;
        int gcInstructionCounter_ = 0;

        void execCall(uint8_t argc, bool isTailCall = false);
        void execIndexGet(uint8_t dims);
        void execIndexSet(uint8_t dims);
        void execSliceGet(uint8_t dims);
        void execSliceSet(uint8_t dims);
        void execBuildMatrix(uint32_t shapeIdx);
        void execIn();
        Value execReturn(bool& shouldExit);
        void populateRefParams(CallFrame& newFrame, const CompiledFunction* fn);
        void execInvoke(uint8_t argc, uint32_t icIdx, bool isTailCall = false);
        void execSuperInvoke(uint32_t nameIdx, uint8_t argc, bool isTailCall = false);
        void execAssertParamType(const Value& val, uint32_t icIdx, uint32_t nameIdx);
        void execAssertReturnType(const Value& val, uint32_t icIdx);

    public:
        VM();
        ~VM();

        void registerBuiltin(const std::string& name, NativeCallable fn, std::set<int> arity);
        Value getBuiltinClosure(const std::string& name);
        void setGlobal(const std::string& name, const Value& val);
        inline static VM* activeVM = nullptr;
        static std::any makeNativeFn(NativeCallable fn);
        // ★ 接受编译后的函数列表
        void setCompiledFunctions(const std::vector<std::shared_ptr<CompiledFunction>>& fns) {
            compiledFunctions = fns;  // ★ 拷贝，不移动
        }
        const std::vector<std::shared_ptr<CompiledFunction>>& getCompiledFunctions() const {
            return compiledFunctions;
        }

        Value callVMFunction(int fnIdx, const std::vector<Value>& args,
            ObjClosure* closure = nullptr,
            Value boundSelf = Value::none(), Value boundClass = Value::none());
        const std::unordered_map<std::string, NativeCallable>& getNativeBuiltins() const { return nativeBuiltins; }


        Value execute(const Chunk& mainChunk);

        std::unordered_map<std::string, Value> getGlobals() const {
            std::unordered_map<std::string, Value> res;
            for (const auto& [k, v] : globalNamesToSlots) res[k] = globalValues[v];
            return res;
        }
        void clearAllGlobalICs() {
            for (auto& fn : compiledFunctions) {
                for (auto& ic : fn->chunk.inlineCaches) {
                    ic.cachedGlobalSlot = -1;
                }
            }
        }
        void clearGlobals() {
            globalValues.clear();
            globalNamesToSlots.clear();
            constGlobals.clear();
            importedModules.clear(); // ★ 核心修复：彻底粉碎模块导入的防环缓存！
            loadedModules.clear();
            openUpvalues = nullptr;
            clearAllGlobalICs();
            // ★ 贴心修复：清理全局变量后，自动把系统必不可少的基础常量重新注入环境
            setGlobal("PI", Value(3.14159265358979323846));
            setGlobal("E", Value(2.71828182845904523536));
            setGlobal("i", Value(Complex(0.0, 1.0)));
            setGlobal("I", Value(Complex(0.0, 1.0)));
        }
        void removeGlobal(const std::string& name) {
            auto it = globalNamesToSlots.find(name);
            if (it != globalNamesToSlots.end()) {
                globalValues[it->second] = Value::none();
                globalNamesToSlots.erase(it);
            }
            constGlobals.erase(name);
            clearAllGlobalICs();
        }

        void triggerDebugger() {
            debugMode = true;
            stepNextLine = true; // 立刻在下一行停下
            lastDebugLine = -1;  // 强制打破防抖
        }

        void disableDebugger() {
            debugMode = false;
            stepNextLine = false;
        }

        void printProfileReport();
        void enableProfiler(bool enable) { profileMode = enable; }

        int runGC();
    };

} // namespace jc

#endif // JC2_VM_H
