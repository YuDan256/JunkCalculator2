#include "PredefinedClasses.h"
#include "VM.h"
#include "../memory/GcHeap.h"
#include "../frontend/Highlight.h"
#include <cmath>
#include <sstream>

namespace jc {

struct RangeData {
    double start;
    double end;
    double step;
    bool isInt;
    int64_t length;
};

struct RangeIterData {
    Value rangeObj;
    int64_t currentIndex;
};

void registerPredefinedClasses() {
    if (!VM::activeVM) return;

    // --- Range Class ---
    ObjClass* rangeClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard rcGuard(rangeClass);
    rangeClass->name = "Range";

    // __init__(*args)
    auto rangeInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"...args"}, std::vector<bool>{false}, "init", nullptr, true);
    GcObjGuard riGuard(rangeInit);
    rangeInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        
        double start = 0, end = 0, step = 1;
        if (args.size() == 1) {
            end = args[0].asDouble();
        } else if (args.size() == 2) {
            start = args[0].asDouble();
            end = args[1].asDouble();
        } else if (args.size() == 3) {
            start = args[0].asDouble();
            end = args[1].asDouble();
            step = args[2].asDouble();
        } else {
            throw std::runtime_error("TypeError: range expected 1 to 3 arguments.");
        }

        if (step == 0.0) throw std::runtime_error("ValueError: range() arg 3 must not be zero.");

        bool isInt = (std::floor(start) == start) && (std::floor(step) == step) && (std::floor(end) == end);
        
        int64_t len = 0;
        if (step > 0 && end > start) {
            len = static_cast<int64_t>(std::ceil((end - start) / step));
        } else if (step < 0 && end < start) {
            len = static_cast<int64_t>(std::ceil((start - end) / (-step)));
        }

        RangeData data{start, end, step, isInt, len};
        inst->nativeData = std::make_any<RangeData>(data);
        
        return self;
    });
    rangeClass->methods["init"] = rangeInit;

    // __len__()
    auto rangeLen = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__len__", nullptr);
    GcObjGuard rlGuard(rangeLen);
    rangeLen->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        return Value(BigInt(data.length));
    });
    rangeClass->methods["__len__"] = rangeLen;

    // __str__()
    auto rangeStr = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__str__", nullptr);
    GcObjGuard rsGuard(rangeStr);
    rangeStr->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        std::ostringstream oss;
        oss << "range(" << data.start << ", " << data.end << ", " << data.step << ")";
        return Value(oss.str());
    });
    rangeClass->methods["__str__"] = rangeStr;
    rangeClass->methods["__repl__"] = rangeStr;

    // __getitem__(idx)
    auto rangeGetItem = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"idx"}, std::vector<bool>{false}, "__getitem__", nullptr);
    GcObjGuard rgiGuard(rangeGetItem);
    rangeGetItem->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        int64_t idx = static_cast<int64_t>(std::round(args[0].asDouble()));
        if (idx < 0) idx += data.length;
        if (idx < 0 || idx >= data.length) throw std::out_of_range("IndexError: range object index out of range");
        
        double val = data.start + idx * data.step;
        if (data.isInt) return Value(BigInt(static_cast<int64_t>(val)));
        return Value(val);
    });
    rangeClass->methods["__getitem__"] = rangeGetItem;

    // --- RangeIterator Class ---
    ObjClass* rangeIterClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard ricGuard(rangeIterClass);
    rangeIterClass->name = "RangeIterator";

    // __iter__() for Range
    auto rangeIter = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__iter__", nullptr);
    GcObjGuard riterGuard(rangeIter);
    rangeIter->nativeFn = std::make_any<NativeCallable>([rangeIterClass](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        ObjInstance* iterInst = GcHeap::get().allocate<ObjInstance>();
        GcObjGuard guard(iterInst);
        iterInst->classDef = rangeIterClass;
        
        RangeIterData iterData{self, 0};
        iterInst->nativeData = std::make_any<RangeIterData>(iterData);
        
        iterInst->c_nativeNext = [](ObjInstance* inst) -> Value {
            auto& iterData = std::any_cast<RangeIterData&>(inst->nativeData);
            auto& rangeData = std::any_cast<RangeData&>(iterData.rangeObj.asInstance()->nativeData);
            if (iterData.currentIndex >= rangeData.length) return Value::uninit();
            double val = rangeData.start + iterData.currentIndex * rangeData.step;
            iterData.currentIndex++;
            if (rangeData.isInt) return Value(BigInt(static_cast<int64_t>(val)));
            return Value(val);
        };
        
        // 为了让 GC 追踪 rangeObj，我们把它也放到 fields 里
        iterInst->fields = GcHeap::get().allocate<ObjDict>();
        iterInst->fields->set(Value("range"), self);

        return Value(iterInst);
    });
    rangeClass->methods["__iter__"] = rangeIter;

    // __next__() for RangeIterator
    auto rangeIterNext = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__next__", nullptr);
    GcObjGuard rinGuard(rangeIterNext);
    rangeIterNext->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& iterData = std::any_cast<RangeIterData&>(self.asInstance()->nativeData);
        auto& rangeData = std::any_cast<RangeData&>(iterData.rangeObj.asInstance()->nativeData);
        
        if (iterData.currentIndex >= rangeData.length) {
            return Value::none();
        }
        
        double val = rangeData.start + iterData.currentIndex * rangeData.step;
        iterData.currentIndex++;
        
        if (rangeData.isInt) return Value(BigInt(static_cast<int64_t>(val)));
        return Value(val);
    });
    rangeIterClass->methods["__next__"] = rangeIterNext;

    // --- ASTNode Class (For Macros) ---
    ObjClass* astNodeClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard astGuard(astNodeClass);
    astNodeClass->name = "ASTNode";

    auto astInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"type", "line", "props"}, std::vector<bool>{false, false, false}, "init", nullptr);
    GcObjGuard astInitGuard(astInit);
    astInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
        
        inst->fields->set(Value("type"), args[0]);
        inst->fields->set(Value("line"), args[1]);
        
        if (args.size() > 2 && args[2].isObjType(ObjType::DICT)) {
            auto props = static_cast<ObjDict*>(args[2].asObj());
            for (const auto& [k, v] : props->elements) {
                inst->fields->set(k, v);
            }
        }
        return self;
    });
    astNodeClass->methods["init"] = astInit;

    auto astStr = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__str__", nullptr);
    GcObjGuard astStrGuard(astStr);
    astStr->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        std::string typeStr = "ASTNode";
        if (inst->fields) {
            auto it = inst->fields->keyMap.find(Value("type"));
            if (it != inst->fields->keyMap.end()) {
                typeStr = inst->fields->elements[it->second].second.toString();
            }
        }
        return Value("<ASTNode " + typeStr + ">");
    });
    astNodeClass->methods["__str__"] = astStr;
    astNodeClass->methods["__repl__"] = astStr;

    // --- Exception Class ---
    ObjClass* exceptionClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard excGuard(exceptionClass);
    exceptionClass->name = "Exception";

    auto excInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"value"}, std::vector<bool>{false}, "init", nullptr);
    GcObjGuard excInitGuard(excInit);
    excInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
        
        inst->fields->set(Value("value"), args[0]);
        inst->fields->set(Value("message"), Value(args[0].isString() ? args[0].asString() : args[0].toString()));
        inst->fields->set(Value("traceback"), Value(""));
        inst->fields->set(Value("suppressed"), Value(GcHeap::get().allocate<ObjList>()));
        
        return self;
    });
    exceptionClass->methods["init"] = excInit;

    auto excRepl = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__repl__", nullptr);
    GcObjGuard excReplGuard(excRepl);
    excRepl->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        std::string msg = "Unknown Error";
        if (inst->fields) {
            auto itMsg = inst->fields->keyMap.find(Value("message"));
            if (itMsg != inst->fields->keyMap.end()) msg = inst->fields->elements[itMsg->second].second.asString();
        }
        return Value("<Exception: " + msg + ">");
    });
    exceptionClass->methods["__repl__"] = excRepl;

    auto excStr = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__str__", nullptr);
    GcObjGuard excStrGuard(excStr);
    excStr->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        std::string msg = "Unknown Error";
        std::string tb = "";
        ObjList* supp = nullptr;
        
        if (inst->fields) {
            auto itMsg = inst->fields->keyMap.find(Value("message"));
            if (itMsg != inst->fields->keyMap.end()) msg = inst->fields->elements[itMsg->second].second.asString();
            
            auto itTb = inst->fields->keyMap.find(Value("traceback"));
            if (itTb != inst->fields->keyMap.end()) tb = inst->fields->elements[itTb->second].second.asString();
            
            auto itSupp = inst->fields->keyMap.find(Value("suppressed"));
            if (itSupp != inst->fields->keyMap.end()) {
                Value sVal = inst->fields->elements[itSupp->second].second;
                if (sVal.isObjType(ObjType::LIST)) supp = static_cast<ObjList*>(sVal.asObj());
            }
        }
        
        std::ostringstream oss;
        if (msg.find("Error:") != std::string::npos || msg.find("Exception:") != std::string::npos) {
            oss << msg;
        } else {
            oss << "Exception: " << msg;
        }
        if (!tb.empty()) {
            oss << tb;
        }
        if (supp && !supp->vec.empty()) {
            oss << "\n\n" << jc::col(jc::Ansi::BRIGHT_RED) << "Suppressed Exceptions (from defer):" << jc::col(jc::Ansi::RESET);
            for (const auto& s : supp->vec) {
                oss << "\n----------------------------------------\n";
                if (s.isInstance() && s.asInstance()->classDef->name == "Exception") {
                    auto dunderStr = VM::activeVM->findDunder(s, "__str__");
                    if (dunderStr) {
                        try {
                            oss << VM::activeVM->callDunder(s, dunderStr, {}).asString();
                        } catch (...) {
                            oss << s.toString();
                        }
                    } else {
                        oss << s.toString();
                    }
                } else {
                    oss << s.toString();
                }
            }
        }
        return Value(oss.str());
    });
    exceptionClass->methods["__str__"] = excStr;

    // 注册到全局
    VM::activeVM->registerBuiltinValue("range", Value(rangeClass));
    VM::activeVM->registerBuiltinValue("__range_iterator", Value(rangeIterClass));
    VM::activeVM->registerBuiltinValue("ASTNode", Value(astNodeClass));
    VM::activeVM->registerBuiltinValue("Exception", Value(exceptionClass));
}

} // namespace jc
