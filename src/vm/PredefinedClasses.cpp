#include "PredefinedClasses.h"
#include "VM.h"
#include "../memory/GcHeap.h"
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
    rangeClass->name = "range";

    // __init__(*args)
    auto rangeInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"...args"}, std::vector<bool>{false}, "init", nullptr, true);
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
    rangeLen->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        return Value(BigInt(data.length));
    });
    rangeClass->methods["__len__"] = rangeLen;

    // __str__()
    auto rangeStr = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__str__", nullptr);
    rangeStr->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        std::ostringstream oss;
        oss << "range(" << data.start << ", " << data.end << ", " << data.step << ")";
        return Value(oss.str());
    });
    rangeClass->methods["__str__"] = rangeStr;

    // __getitem__(idx)
    auto rangeGetItem = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"idx"}, std::vector<bool>{false}, "__getitem__", nullptr);
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
    rangeIterClass->name = "range_iterator";

    // __iter__() for Range
    auto rangeIter = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__iter__", nullptr);
    rangeIter->nativeFn = std::make_any<NativeCallable>([rangeIterClass](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        ObjInstance* iterInst = GcHeap::get().allocate<ObjInstance>();
        iterInst->classDef = rangeIterClass;
        
        RangeIterData iterData{self, 0};
        iterInst->nativeData = std::make_any<RangeIterData>(iterData);
        
        // 为了让 GC 追踪 rangeObj，我们把它也放到 fields 里
        iterInst->fields = GcHeap::get().allocate<ObjDict>();
        iterInst->fields->set(Value("range"), self);

        return Value(iterInst);
    });
    rangeClass->methods["__iter__"] = rangeIter;

    // __next__() for RangeIterator
    auto rangeIterNext = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__next__", nullptr);
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

    // 注册到全局
    VM::activeVM->setGlobal("range", Value(rangeClass));
    VM::activeVM->setGlobal("__range_iterator", Value(rangeIterClass));
}

} // namespace jc
