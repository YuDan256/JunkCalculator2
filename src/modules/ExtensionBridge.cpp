#include "ExtensionBridge.h"
#include "../vm/VM.h"
#include "../vm/HelpRouter.h"
#include <stdexcept>

namespace jc {

static inline Value from_handle(JC2_ValueHandle h) { Value v; v.as_bits = h; return v; }

static JC2_ValueHandle host_make_none(JC2_VMContext) { return Value::none().as_bits; }
static JC2_ValueHandle host_make_bool(JC2_VMContext, bool b) { return Value(b).as_bits; }
static JC2_ValueHandle host_make_int(JC2_VMContext, int32_t i) { return Value(i).as_bits; }
static JC2_ValueHandle host_make_double(JC2_VMContext, double d) { return Value(d).as_bits; }
static JC2_ValueHandle host_make_string(JC2_VMContext, const char* str, size_t len) {
    return Value(std::string(str, len)).as_bits;
}
static JC2_ValueHandle host_make_complex(JC2_VMContext, double r, double i) {
    return Value(Complex(r, i)).as_bits;
}

static bool host_is_none(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isNone(); }
static bool host_is_bool(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isBool(); }
static bool host_is_int(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isInt32(); }
static bool host_is_double(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isDouble(); }
static bool host_is_string(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isString(); }
static bool host_is_instance(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isInstance(); }

static bool host_as_bool(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).truthy(); }
static int32_t host_as_int(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).asInt32(); }
static double host_as_double(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).asDouble(); }
static const char* host_as_string(JC2_VMContext, JC2_ValueHandle v, size_t* out_len) {
    Value val = from_handle(v);
    if (!val.isString()) return nullptr;
    const std::string& s = val.asString();
    if (out_len) *out_len = s.length();
    return s.c_str();
}

static JC2_ValueHandle host_make_class(JC2_VMContext, const char* name) {
    ObjClass* cls = GcHeap::get().allocate<ObjClass>();
    cls->name = name;
    return Value(cls).as_bits;
}

static JC2_ValueHandle host_make_instance(JC2_VMContext, JC2_ValueHandle class_handle) {
    Value clsVal = from_handle(class_handle);
    if (!clsVal.isClass()) throw std::runtime_error("Type Error: make_instance expects a Class handle.");
    ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
    inst->classDef = static_cast<ObjClass*>(clsVal.asObj());
    return Value(inst).as_bits;
}

static void host_bind_method(JC2_VMContext, JC2_ValueHandle class_handle, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, void* user_data) {
    Value clsVal = from_handle(class_handle);
    if (!clsVal.isClass()) throw std::runtime_error("Type Error: bind_method expects a Class handle.");
    ObjClass* cls = static_cast<ObjClass*>(clsVal.asObj());
    
    NativeCallable callable = [fn, user_data](const std::vector<Value>& args) -> Value {
        std::vector<JC2_ValueHandle> c_args;
        c_args.reserve(args.size() + 1);
        if (!jc::helpers::nativeSelfStack.empty()) {
            c_args.push_back(jc::helpers::nativeSelfStack.back().as_bits);
        } else {
            c_args.push_back(Value::none().as_bits);
        }
        for (size_t i = 0; i < args.size(); ++i) c_args.push_back(args[i].as_bits);
        try {
            JC2_ValueHandle res = fn(VM::activeVM, static_cast<int>(c_args.size()), c_args.data(), user_data);
            return from_handle(res);
        } catch (...) {
            throw;
        }
    };

    auto closure = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, name, nullptr);
    closure->nativeFn = std::make_any<NativeCallable>(callable);
    
    if (!has_rest) {
        for (int i = 0; i < max_arity; ++i) {
            closure->paramNames.push_back("_" + std::to_string(i));
            closure->isRef.push_back(false);
        }
        for (int i = min_arity; i < max_arity; ++i) {
            closure->defaultValues.push_back(Value::none());
        }
    }
    
    cls->methods[name] = closure;
}

static void host_set_native_data(JC2_VMContext, JC2_ValueHandle instance, void* data, JC2_NativeDestructor dtor) {
    Value val = from_handle(instance);
    if (val.isInstance()) {
        auto inst = val.asInstance();
        inst->c_nativeData = data;
        inst->c_nativeDtor = dtor;
    }
}

static void* host_get_native_data(JC2_VMContext, JC2_ValueHandle instance) {
    Value val = from_handle(instance);
    if (val.isInstance()) {
        return val.asInstance()->c_nativeData;
    }
    return nullptr;
}

static void host_register_help(JC2_VMContext, const char* topic, const char* help_text) {
    jc::DynamicHelp[topic] = help_text;
}

static void host_register_function_help(JC2_VMContext, const char* name, const char* signature, const char* desc, const char* example) {
    jc::HelpRouter::addFunctionHelp(name, signature, desc, example);
}

static void host_register_function(JC2_VMContext, JC2_ModuleHandle mod, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, void* user_data) {
    ModuleLoadContext* mctx = static_cast<ModuleLoadContext*>(mod);
    
    NativeCallable callable = [fn, user_data](const std::vector<Value>& args) -> Value {
        std::vector<JC2_ValueHandle> c_args(args.size());
        for (size_t i = 0; i < args.size(); ++i) c_args[i] = args[i].as_bits;
        
        try {
            JC2_ValueHandle res = fn(VM::activeVM, static_cast<int>(args.size()), c_args.data(), user_data);
            return from_handle(res);
        } catch (...) {
            throw; // 拦截 C++ 异常，交由 VM 处理
        }
    };

    (*mctx->builtins)[name] = callable;
    
    std::set<int> aritySet;
    if (!has_rest) {
        for (int i = min_arity; i <= max_arity; ++i) {
            aritySet.insert(i);
        }
    }
    (*mctx->arity)[name] = aritySet;
}

static void host_register_int(JC2_VMContext, JC2_ModuleHandle mod, const char* name, int32_t val) {
    ModuleLoadContext* mctx = static_cast<ModuleLoadContext*>(mod);
    (*mctx->env)[name] = Value(val);
}

static void host_register_double(JC2_VMContext, JC2_ModuleHandle mod, const char* name, double val) {
    ModuleLoadContext* mctx = static_cast<ModuleLoadContext*>(mod);
    (*mctx->env)[name] = Value(val);
}

static void host_register_string(JC2_VMContext, JC2_ModuleHandle mod, const char* name, const char* val) {
    ModuleLoadContext* mctx = static_cast<ModuleLoadContext*>(mod);
    (*mctx->env)[name] = Value(std::string(val));
}

static void host_register_value(JC2_VMContext, JC2_ModuleHandle mod, const char* name, JC2_ValueHandle val) {
    ModuleLoadContext* mctx = static_cast<ModuleLoadContext*>(mod);
    (*mctx->env)[name] = from_handle(val);
}

static void host_throw_error(JC2_VMContext, const char* msg) {
    throw std::runtime_error(msg);
}

static JC2_ValueHandle host_make_list(JC2_VMContext) {
    ObjList* list = GcHeap::get().allocate<ObjList>();
    return Value(list).as_bits;
}

static void host_list_push(JC2_VMContext, JC2_ValueHandle list, JC2_ValueHandle val) {
    Value l = from_handle(list);
    if (l.isObjType(ObjType::LIST)) {
        static_cast<ObjList*>(l.asObj())->vec.push_back(from_handle(val));
    }
}

static size_t host_list_size(JC2_VMContext, JC2_ValueHandle list) {
    Value l = from_handle(list);
    if (l.isObjType(ObjType::LIST)) {
        return static_cast<ObjList*>(l.asObj())->vec.size();
    }
    return 0;
}

static JC2_ValueHandle host_list_get(JC2_VMContext, JC2_ValueHandle list, size_t index) {
    Value l = from_handle(list);
    if (l.isObjType(ObjType::LIST)) {
        auto& vec = static_cast<ObjList*>(l.asObj())->vec;
        if (index < vec.size()) return vec[index].as_bits;
    }
    return Value::none().as_bits;
}

static bool host_is_list(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::LIST);
}

static const JC2_HostAPI host_api = {
    JC2_EXT_MAGIC,
    JC2_EXT_VERSION,
    host_make_none,
    host_make_bool,
    host_make_int,
    host_make_double,
    host_make_string,
    host_make_complex,
    host_is_none,
    host_is_bool,
    host_is_int,
    host_is_double,
    host_is_string,
    host_is_instance,
    host_as_bool,
    host_as_int,
    host_as_double,
    host_as_string,
    host_make_class,
    host_make_instance,
    host_bind_method,
    host_set_native_data,
    host_get_native_data,
    host_register_help,
    host_register_function_help,
    host_register_function,
    host_register_int,
    host_register_double,
    host_register_string,
    host_register_value,
    host_throw_error,
    host_make_list,
    host_list_push,
    host_list_size,
    host_list_get,
    host_is_list
};

const JC2_HostAPI* get_host_api() {
    return &host_api;
}

} // namespace jc
