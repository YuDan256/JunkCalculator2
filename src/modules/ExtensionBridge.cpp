#include "ExtensionBridge.h"
#include "../vm/VM.h"
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

static JC2_ValueHandle host_make_instance(JC2_VMContext ctx, const char* class_name) {
    VM* vm = static_cast<VM*>(ctx);
    auto globals = vm->getGlobals();
    auto it = globals.find(class_name);
    if (it == globals.end() || !it->second.isClass()) {
        throw std::runtime_error(std::string("Class not found: ") + class_name);
    }
    ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
    inst->classDef = static_cast<ObjClass*>(it->second.asObj());
    return Value(inst).as_bits;
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

static void host_register_function(JC2_VMContext, JC2_ModuleHandle mod, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, void* user_data) {
    ModuleLoadContext* mctx = static_cast<ModuleLoadContext*>(mod);
    
    NativeCallable callable = [fn, user_data](const std::vector<Value>& args) -> Value {
        std::vector<JC2_ValueHandle> c_args(args.size());
        for (size_t i = 0; i < args.size(); ++i) c_args[i] = args[i].as_bits;
        
        try {
            JC2_ValueHandle res = fn(VM::activeVM, static_cast<int>(args.size()), c_args.data(), user_data);
            return from_handle(res);
        } catch (const std::exception& e) {
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

static void host_throw_error(JC2_VMContext, const char* msg) {
    throw std::runtime_error(msg);
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
    host_make_instance,
    host_set_native_data,
    host_get_native_data,
    host_register_function,
    host_register_int,
    host_register_double,
    host_register_string,
    host_throw_error
};

const JC2_HostAPI* get_host_api() {
    return &host_api;
}

} // namespace jc
