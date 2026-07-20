#include "ExtensionBridge.h"
#include "../vm/HelpRouter.h"
#include "../vm/BuiltinRegistry.h"
#include "../vm/VM.h"
#include <stdexcept>

namespace jc {

thread_local std::vector<Value> nativeTempRefs;

static inline Value from_handle(JC2_ValueHandle h) { 
    Value v; 
    v.as_bits = h; 
    if (v.isObj()) v.asObj()->refCount++;
    return v; 
}

static inline JC2_ValueHandle protect(const Value& v) {
    if (v.isObj()) nativeTempRefs.push_back(v);
    return v.as_bits;
}

static JC2_ValueHandle host_make_none(JC2_VMContext) { return Value::none().as_bits; }
static JC2_ValueHandle host_make_bool(JC2_VMContext, bool b) { return Value(b).as_bits; }
static JC2_ValueHandle host_make_int(JC2_VMContext, int32_t i) { return Value(i).as_bits; }
static JC2_ValueHandle host_make_double(JC2_VMContext, double d) { return Value(d).as_bits; }
static JC2_ValueHandle host_make_string(JC2_VMContext, const char* str, size_t len) {
    return protect(Value(std::string(str, len)));
}
static JC2_ValueHandle host_make_complex(JC2_VMContext, double r, double i) {
    return protect(Value(Complex(r, i)));
}

static bool host_is_none(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isNone(); }
static bool host_is_bool(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isBool(); }
static bool host_is_int(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isInt32(); }
static bool host_is_double(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isDouble(); }
static bool host_is_string(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isString(); }
static bool host_is_complex(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isObjType(ObjType::COMPLEX); }
static bool host_is_instance(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).isInstance(); }

static bool host_as_bool(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).truthy(); }
static int32_t host_as_int(JC2_VMContext, JC2_ValueHandle v) {
    Value val = from_handle(v);
    if (val.isInt32()) return val.asInt32();
    if (val.isDouble()) return static_cast<int32_t>(val.asDoubleRaw());
    if (val.isBool()) return val.asBool() ? 1 : 0;
    // 如果不是基本数字类型，asDouble() 会执行严格的类型检查并抛出异常
    return static_cast<int32_t>(val.asDouble());
}
static double host_as_double(JC2_VMContext, JC2_ValueHandle v) { return from_handle(v).asDouble(); }
static const char* host_as_string(JC2_VMContext, JC2_ValueHandle v, size_t* out_len) {
    Value val = from_handle(v);
    if (!val.isString()) return nullptr;
    const std::string& s = val.asString();
    if (out_len) *out_len = s.length();
    return s.c_str();
}

static double host_complex_get_real(JC2_VMContext, JC2_ValueHandle v) {
    Value val = from_handle(v);
    if (val.isObjType(ObjType::COMPLEX)) return static_cast<ObjComplex*>(val.asObj())->comp.real;
    if (val.isNumber()) return val.asDouble();
    return 0.0;
}

static double host_complex_get_imag(JC2_VMContext, JC2_ValueHandle v) {
    Value val = from_handle(v);
    if (val.isObjType(ObjType::COMPLEX)) return static_cast<ObjComplex*>(val.asObj())->comp.imag;
    return 0.0;
}

static JC2_ValueHandle host_make_class(JC2_VMContext, const char* name) {
    ObjClass* cls = GcHeap::get().allocate<ObjClass>();
    cls->name = name;
    return protect(Value(cls));
}

static JC2_ValueHandle host_make_instance(JC2_VMContext, JC2_ValueHandle class_handle) {
    Value clsVal = from_handle(class_handle);
    if (!clsVal.isClass()) throw std::runtime_error("Type Error: make_instance expects a Class handle.");
    ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
    inst->classDef = static_cast<ObjClass*>(clsVal.asObj());
    return protect(Value(inst));
}

static void host_bind_method(JC2_VMContext, JC2_ValueHandle class_handle, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, void* user_data) {
    Value clsVal = from_handle(class_handle);
    if (!clsVal.isClass()) throw std::runtime_error("Type Error: bind_method expects a Class handle.");
    ObjClass* cls = static_cast<ObjClass*>(clsVal.asObj());
    
    NativeCallable callable = [fn, user_data](const std::vector<Value>& args) -> Value {
        size_t old_size = nativeTempRefs.size();
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
            Value ret = from_handle(res);
            nativeTempRefs.resize(old_size);
            return ret;
        } catch (...) {
            nativeTempRefs.resize(old_size);
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
        size_t old_size = nativeTempRefs.size();
        std::vector<JC2_ValueHandle> c_args(args.size());
        for (size_t i = 0; i < args.size(); ++i) c_args[i] = args[i].as_bits;
        
        try {
            JC2_ValueHandle res = fn(VM::activeVM, static_cast<int>(args.size()), c_args.data(), user_data);
            Value ret = from_handle(res);
            nativeTempRefs.resize(old_size);
            return ret;
        } catch (...) {
            nativeTempRefs.resize(old_size);
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
    return protect(Value(list));
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
        if (index < vec.size()) return protect(vec[index]);
    }
    return Value::none().as_bits;
}

static bool host_is_list(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::LIST);
}

static JC2_ValueHandle host_make_dict(JC2_VMContext) {
    ObjDict* dict = GcHeap::get().allocate<ObjDict>();
    return protect(Value(dict));
}

static void host_dict_set(JC2_VMContext, JC2_ValueHandle dict, JC2_ValueHandle key, JC2_ValueHandle val) {
    Value d = from_handle(dict);
    if (d.isObjType(ObjType::DICT)) {
        static_cast<ObjDict*>(d.asObj())->set(from_handle(key), from_handle(val));
    }
}

static JC2_ValueHandle host_dict_get(JC2_VMContext, JC2_ValueHandle dict, JC2_ValueHandle key) {
    Value d = from_handle(dict);
    if (d.isObjType(ObjType::DICT)) {
        ObjDict* obj = static_cast<ObjDict*>(d.asObj());
        Value k = from_handle(key);
        auto it = obj->keyMap.find(k);
        if (it != obj->keyMap.end()) return protect(obj->elements[it->second].second);
    }
    return Value::none().as_bits;
}

static bool host_dict_has(JC2_VMContext, JC2_ValueHandle dict, JC2_ValueHandle key) {
    Value d = from_handle(dict);
    if (d.isObjType(ObjType::DICT)) {
        ObjDict* obj = static_cast<ObjDict*>(d.asObj());
        return obj->keyMap.find(from_handle(key)) != obj->keyMap.end();
    }
    return false;
}

static size_t host_dict_size(JC2_VMContext, JC2_ValueHandle dict) {
    Value d = from_handle(dict);
    if (d.isObjType(ObjType::DICT)) {
        return static_cast<ObjDict*>(d.asObj())->elements.size();
    }
    return 0;
}

static bool host_is_dict(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::DICT);
}

static JC2_ValueHandle host_make_real_matrix(JC2_VMContext, int rows, int cols) {
    return protect(Value(RealMatrix(rows, cols)));
}

static double host_real_matrix_get(JC2_VMContext, JC2_ValueHandle mat, int row, int col) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::REAL_MATRIX)) {
        return static_cast<ObjRealMatrix*>(m.asObj())->mat(row, col);
    }
    return 0.0;
}

static void host_real_matrix_set(JC2_VMContext, JC2_ValueHandle mat, int row, int col, double val) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::REAL_MATRIX)) {
        static_cast<ObjRealMatrix*>(m.asObj())->mat(row, col) = val;
    }
}

static int host_real_matrix_rows(JC2_VMContext, JC2_ValueHandle mat) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::REAL_MATRIX)) {
        return static_cast<ObjRealMatrix*>(m.asObj())->mat.getRows();
    }
    return 0;
}

static int host_real_matrix_cols(JC2_VMContext, JC2_ValueHandle mat) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::REAL_MATRIX)) {
        return static_cast<ObjRealMatrix*>(m.asObj())->mat.getCols();
    }
    return 0;
}

static bool host_is_real_matrix(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::REAL_MATRIX);
}

static JC2_ValueHandle host_make_complex_matrix(JC2_VMContext, int rows, int cols) {
    return protect(Value(ComplexMatrix(rows, cols)));
}

static double host_complex_matrix_get_real(JC2_VMContext, JC2_ValueHandle mat, int row, int col) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::COMPLEX_MATRIX)) return static_cast<ObjComplexMatrix*>(m.asObj())->mat(row, col).real;
    return 0.0;
}

static double host_complex_matrix_get_imag(JC2_VMContext, JC2_ValueHandle mat, int row, int col) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::COMPLEX_MATRIX)) return static_cast<ObjComplexMatrix*>(m.asObj())->mat(row, col).imag;
    return 0.0;
}

static void host_complex_matrix_set(JC2_VMContext, JC2_ValueHandle mat, int row, int col, double r, double i) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::COMPLEX_MATRIX)) static_cast<ObjComplexMatrix*>(m.asObj())->mat(row, col) = Complex(r, i);
}

static int host_complex_matrix_rows(JC2_VMContext, JC2_ValueHandle mat) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::COMPLEX_MATRIX)) return static_cast<ObjComplexMatrix*>(m.asObj())->mat.getRows();
    return 0;
}

static int host_complex_matrix_cols(JC2_VMContext, JC2_ValueHandle mat) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::COMPLEX_MATRIX)) return static_cast<ObjComplexMatrix*>(m.asObj())->mat.getCols();
    return 0;
}

static bool host_is_complex_matrix(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::COMPLEX_MATRIX);
}

static JC2_ValueHandle host_make_string_matrix(JC2_VMContext, int rows, int cols) {
    return protect(Value(StringMatrix(rows, cols)));
}

static const char* host_string_matrix_get(JC2_VMContext, JC2_ValueHandle mat, int row, int col, size_t* out_len) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::STRING_MATRIX)) {
        const std::string& s = static_cast<ObjStringMatrix*>(m.asObj())->mat(row, col);
        if (out_len) *out_len = s.length();
        return s.c_str();
    }
    return nullptr;
}

static void host_string_matrix_set(JC2_VMContext, JC2_ValueHandle mat, int row, int col, const char* str, size_t len) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::STRING_MATRIX)) {
        static_cast<ObjStringMatrix*>(m.asObj())->mat(row, col) = std::string(str, len);
    }
}

static int host_string_matrix_rows(JC2_VMContext, JC2_ValueHandle mat) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::STRING_MATRIX)) return static_cast<ObjStringMatrix*>(m.asObj())->mat.getRows();
    return 0;
}

static int host_string_matrix_cols(JC2_VMContext, JC2_ValueHandle mat) {
    Value m = from_handle(mat);
    if (m.isObjType(ObjType::STRING_MATRIX)) return static_cast<ObjStringMatrix*>(m.asObj())->mat.getCols();
    return 0;
}

static bool host_is_string_matrix(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::STRING_MATRIX);
}

static JC2_ValueHandle host_make_set(JC2_VMContext) {
    ObjSet* set = GcHeap::get().allocate<ObjSet>();
    return protect(Value(set));
}

static void host_set_add(JC2_VMContext, JC2_ValueHandle set, JC2_ValueHandle val) {
    Value s = from_handle(set);
    if (s.isObjType(ObjType::SET)) {
        static_cast<ObjSet*>(s.asObj())->add(from_handle(val));
    }
}

static void host_set_remove(JC2_VMContext, JC2_ValueHandle set, JC2_ValueHandle val) {
    Value s = from_handle(set);
    if (s.isObjType(ObjType::SET)) {
        static_cast<ObjSet*>(s.asObj())->discard(from_handle(val));
    }
}

static bool host_set_has(JC2_VMContext, JC2_ValueHandle set, JC2_ValueHandle val) {
    Value s = from_handle(set);
    if (s.isObjType(ObjType::SET)) {
        ObjSet* obj = static_cast<ObjSet*>(s.asObj());
        return obj->keys.find(from_handle(val)) != obj->keys.end();
    }
    return false;
}

static size_t host_set_size(JC2_VMContext, JC2_ValueHandle set) {
    Value s = from_handle(set);
    if (s.isObjType(ObjType::SET)) {
        return static_cast<ObjSet*>(s.asObj())->elements.size();
    }
    return 0;
}

static bool host_is_set(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::SET);
}

static JC2_ValueHandle host_make_bigint(JC2_VMContext, const char* str) {
    try { return protect(Value(BigInt(std::string(str)))); }
    catch (...) { return Value::none().as_bits; }
}

static const char* host_bigint_to_string(JC2_VMContext, JC2_ValueHandle v, size_t* out_len) {
    Value val = from_handle(v);
    if (val.isBigInt()) {
        ObjString* s = internString(val.asBigInt().toString());
        if (out_len) *out_len = s->str.length();
        return s->str.c_str();
    }
    return nullptr;
}

static bool host_is_bigint(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isBigInt();
}

static JC2_ValueHandle host_make_fraction(JC2_VMContext, JC2_ValueHandle num, JC2_ValueHandle den) {
    try {
        return protect(Value(Fraction(from_handle(num).asBigInt(), from_handle(den).asBigInt())));
    } catch (...) { return Value::none().as_bits; }
}

static JC2_ValueHandle host_fraction_get_num(JC2_VMContext, JC2_ValueHandle v) {
    Value val = from_handle(v);
    if (val.isObjType(ObjType::FRACTION)) return protect(Value(static_cast<ObjFraction*>(val.asObj())->frac.getNum()));
    return Value::none().as_bits;
}

static JC2_ValueHandle host_fraction_get_den(JC2_VMContext, JC2_ValueHandle v) {
    Value val = from_handle(v);
    if (val.isObjType(ObjType::FRACTION)) return protect(Value(static_cast<ObjFraction*>(val.asObj())->frac.getDen()));
    return Value::none().as_bits;
}

static bool host_is_fraction(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::FRACTION);
}

static JC2_ValueHandle host_make_namespace(JC2_VMContext, const char* name) {
    ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
    ns->name = name;
    return protect(Value(ns));
}

static void host_namespace_set(JC2_VMContext, JC2_ValueHandle ns, const char* key, JC2_ValueHandle val) {
    Value n = from_handle(ns);
    if (n.isObjType(ObjType::NAMESPACE)) {
        ObjNamespace* obj = static_cast<ObjNamespace*>(n.asObj());
        if (!obj->is_frozen) {
            NamespaceField field;
            field.upval = GcHeap::get().allocate<ObjUpVal>();
            field.upval->closed = from_handle(val);
            field.upval->location = &field.upval->closed;
            field.isConst = false;
            obj->fields[key] = field;
        }
    }
}

static JC2_ValueHandle host_namespace_get(JC2_VMContext, JC2_ValueHandle ns, const char* key) {
    Value n = from_handle(ns);
    if (n.isObjType(ObjType::NAMESPACE)) {
        ObjNamespace* obj = static_cast<ObjNamespace*>(n.asObj());
        auto it = obj->fields.find(key);
        if (it != obj->fields.end() && it->second.upval && it->second.upval->location) {
            return protect(*(it->second.upval->location));
        }
    }
    return Value::none().as_bits;
}

static bool host_is_namespace(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isObjType(ObjType::NAMESPACE);
}

static void host_set_class_parent(JC2_VMContext, JC2_ValueHandle cls, JC2_ValueHandle parent) {
    Value c = from_handle(cls);
    Value p = from_handle(parent);
    if (c.isClass() && p.isClass()) {
        static_cast<ObjClass*>(c.asObj())->parent = static_cast<ObjClass*>(p.asObj());
    }
}

static JC2_ValueHandle host_get_class(JC2_VMContext, JC2_ValueHandle inst) {
    Value i = from_handle(inst);
    if (i.isInstance()) {
        ObjClass* cls = i.asInstance()->classDef;
        if (cls) return protect(Value(cls));
    }
    return Value::none().as_bits;
}

static JC2_ValueHandle host_instance_get_field(JC2_VMContext, JC2_ValueHandle inst, const char* name) {
    Value i = from_handle(inst);
    if (i.isInstance()) {
        ObjInstance* obj = i.asInstance();
        if (obj->fields) {
            Value key = Value(std::string(name));
            auto it = obj->fields->keyMap.find(key);
            if (it != obj->fields->keyMap.end()) {
                return protect(obj->fields->elements[it->second].second);
            }
        }
    }
    return Value::none().as_bits;
}

static void host_instance_set_field(JC2_VMContext, JC2_ValueHandle inst, const char* name, JC2_ValueHandle val) {
    Value i = from_handle(inst);
    if (i.isInstance()) {
        ObjInstance* obj = i.asInstance();
        if (!obj->fields) obj->fields = GcHeap::get().allocate<ObjDict>();
        obj->fields->set(Value(std::string(name)), from_handle(val));
    }
}

static JC2_ValueHandle host_dict_keys(JC2_VMContext, JC2_ValueHandle dict) {
    Value d = from_handle(dict);
    if (d.isObjType(ObjType::DICT)) {
        ObjList* list = GcHeap::get().allocate<ObjList>();
        for (const auto& kv : static_cast<ObjDict*>(d.asObj())->elements) {
            list->vec.push_back(kv.first);
        }
        return protect(Value(list));
    }
    return Value::none().as_bits;
}

static JC2_ValueHandle host_get_global(JC2_VMContext, const char* name) {
    if (!VM::activeVM) return Value::none().as_bits;
    auto globals = VM::activeVM->getGlobals();
    auto it = globals.find(name);
    if (it != globals.end()) {
        return protect(it->second);
    }
    Value builtinVal = VM::activeVM->getBuiltinClosure(name);
    if (!builtinVal.isNone()) {
        return protect(builtinVal);
    }
    return Value::none().as_bits;
}

static JC2_ValueHandle host_to_string(JC2_VMContext, JC2_ValueHandle v) {
    Value val = from_handle(v);
    std::ostringstream oss;
    oss << val;
    return protect(Value(std::string(oss.str())));
}

static JC2_ValueHandle host_call_function(JC2_VMContext, JC2_ValueHandle func, int argc, JC2_ValueHandle* argv) {
    Value f = from_handle(func);
    if (!f.isFunctionClosure()) return Value::none().as_bits;
    std::vector<Value> args(argc);
    for (int i = 0; i < argc; ++i) args[i] = from_handle(argv[i]);
    return protect(jc::helpers::safeCallFunction(static_cast<ObjClosure*>(f.asObj()), args));
}

static bool host_is_function(JC2_VMContext, JC2_ValueHandle v) {
    return from_handle(v).isFunctionClosure();
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
    host_is_complex,
    host_is_instance,
    host_as_bool,
    host_as_int,
    host_as_double,
    host_as_string,
    host_complex_get_real,
    host_complex_get_imag,
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
    host_is_list,
    host_make_dict,
    host_dict_set,
    host_dict_get,
    host_dict_has,
    host_dict_size,
    host_is_dict,
    host_dict_keys,
    host_get_global,
    host_to_string,
    host_make_real_matrix,
    host_real_matrix_get,
    host_real_matrix_set,
    host_real_matrix_rows,
    host_real_matrix_cols,
    host_is_real_matrix,
    host_make_complex_matrix,
    host_complex_matrix_get_real,
    host_complex_matrix_get_imag,
    host_complex_matrix_set,
    host_complex_matrix_rows,
    host_complex_matrix_cols,
    host_is_complex_matrix,
    host_make_string_matrix,
    host_string_matrix_get,
    host_string_matrix_set,
    host_string_matrix_rows,
    host_string_matrix_cols,
    host_is_string_matrix,
    host_make_set,
    host_set_add,
    host_set_remove,
    host_set_has,
    host_set_size,
    host_is_set,
    host_call_function,
    host_is_function,
    host_make_bigint,
    host_bigint_to_string,
    host_is_bigint,
    host_make_fraction,
    host_fraction_get_num,
    host_fraction_get_den,
    host_is_fraction,
    host_make_namespace,
    host_namespace_set,
    host_namespace_get,
    host_is_namespace,
    host_set_class_parent,
    host_get_class,
    host_instance_get_field,
    host_instance_set_field
};

const JC2_HostAPI* get_host_api() {
    return &host_api;
}

} // namespace jc
