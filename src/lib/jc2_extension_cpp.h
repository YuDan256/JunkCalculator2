#ifndef JC2_EXTENSION_CPP_H
#define JC2_EXTENSION_CPP_H

#include "jc2_extension_api.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace jc2 {

class Env {
public:
    static inline JC2_VMContext ctx = nullptr;
    static inline const JC2_HostAPI* api = nullptr;
};

class Value {
    JC2_ValueHandle handle;
public:
    Value(JC2_ValueHandle h) : handle(h) {}
    Value() : handle(Env::api->make_none(Env::ctx)) {}
    Value(bool b) : handle(Env::api->make_bool(Env::ctx, b)) {}
    Value(int32_t i) : handle(Env::api->make_int(Env::ctx, i)) {}
    Value(double d) : handle(Env::api->make_double(Env::ctx, d)) {}
    Value(const char* s) : handle(Env::api->make_string(Env::ctx, s, strlen(s))) {}
    Value(const std::string& s) : handle(Env::api->make_string(Env::ctx, s.c_str(), s.length())) {}

    JC2_ValueHandle get_handle() const { return handle; }

    bool is_none() const { return Env::api->is_none(Env::ctx, handle); }
    bool is_bool() const { return Env::api->is_bool(Env::ctx, handle); }
    bool is_int() const { return Env::api->is_int(Env::ctx, handle); }
    bool is_double() const { return Env::api->is_double(Env::ctx, handle); }
    bool is_string() const { return Env::api->is_string(Env::ctx, handle); }
    bool is_complex() const { return Env::api->is_complex(Env::ctx, handle); }
    bool is_instance() const { return Env::api->is_instance(Env::ctx, handle); }
    bool is_list() const { return Env::api->is_list(Env::ctx, handle); }
    bool is_dict() const { return Env::api->is_dict(Env::ctx, handle); }
    bool is_set() const { return Env::api->is_set(Env::ctx, handle); }
    bool is_real_matrix() const { return Env::api->is_real_matrix(Env::ctx, handle); }
    bool is_complex_matrix() const { return Env::api->is_complex_matrix(Env::ctx, handle); }
    bool is_function() const { return Env::api->is_function(Env::ctx, handle); }
    bool is_bigint() const { return Env::api->is_bigint(Env::ctx, handle); }
    bool is_fraction() const { return Env::api->is_fraction(Env::ctx, handle); }
    bool is_namespace() const { return Env::api->is_namespace(Env::ctx, handle); }
    bool is_slice() const { return Env::api->is_slice(Env::ctx, handle); }
    bool is_type() const { return Env::api->is_type(Env::ctx, handle); }

    Value get_type() const { return Value(Env::api->get_type(Env::ctx, handle)); }
    
    std::string type_name() const {
        if (!is_type()) return "";
        size_t len = 0;
        const char* s = Env::api->type_name(Env::ctx, handle, &len);
        return s ? std::string(s, len) : "";
    }

    bool matches_type(const Value& type_obj) const {
        return Env::api->check_type(Env::ctx, handle, type_obj.get_handle());
    }

    std::string to_string() const {
        JC2_ValueHandle str_h = Env::api->to_string(Env::ctx, handle);
        size_t len = 0;
        const char* s = Env::api->as_string(Env::ctx, str_h, &len);
        return s ? std::string(s, len) : "";
    }

    bool as_bool() const { return Env::api->as_bool(Env::ctx, handle); }
    int32_t as_int() const { return Env::api->as_int(Env::ctx, handle); }
    double as_double() const { return Env::api->as_double(Env::ctx, handle); }
    std::string as_string() const {
        size_t len = 0;
        const char* s = Env::api->as_string(Env::ctx, handle, &len);
        return s ? std::string(s, len) : "";
    }

    const char* as_c_str() const {
        return Env::api->as_string(Env::ctx, handle, nullptr);
    }

    void set_native_data(void* data, JC2_NativeDestructor dtor) {
        Env::api->set_native_data(Env::ctx, handle, data, dtor);
    }

    template<typename T>
    T* get_native_data() const {
        return static_cast<T*>(Env::api->get_native_data(Env::ctx, handle));
    }

    void freeze() const {
        Env::api->freeze_object(Env::ctx, handle);
    }
};

class Type : public Value {
public:
    Type(JC2_ValueHandle h) : Value(h) {}
    // 通过内置类型的名称（如 "int", "string", "list"）直接获取全局类型对象
    Type(const std::string& builtin_name) : Value(Env::api->get_global(Env::ctx, builtin_name.c_str())) {}
    
    Type operator|(const Type& other) const {
        return Type(Env::api->type_union(Env::ctx, get_handle(), other.get_handle()));
    }
    
    Type operator&(const Type& other) const {
        return Type(Env::api->type_intersect(Env::ctx, get_handle(), other.get_handle()));
    }
};

class Class : public Value {
public:
    Class(const std::string& name) : Value(Env::api->make_class(Env::ctx, name.c_str())) {}
    Class(JC2_ValueHandle h) : Value(h) {}

    void set_parent(const Class& parent) {
        Env::api->set_class_parent(Env::ctx, get_handle(), parent.get_handle());
    }

    void bind_method(const std::string& name, JC2_NativeFunc fn, int min_arity = 0, int max_arity = 16777215, bool has_rest = true, const std::vector<std::string>& param_names = {}, void* user_data = nullptr) {
        std::vector<const char*> c_param_names;
        for (const auto& p : param_names) c_param_names.push_back(p.c_str());
        Env::api->bind_method(Env::ctx, get_handle(), name.c_str(), fn, min_arity, max_arity, has_rest, c_param_names.data(), static_cast<int>(c_param_names.size()), user_data);
    }

    void set_allocator(JC2_NativeFunc fn, void* user_data = nullptr) {
        Env::api->set_class_allocator(Env::ctx, get_handle(), fn, user_data);
    }
};

class Instance : public Value {
public:
    Instance(const Class& cls) : Value(Env::api->make_instance(Env::ctx, cls.get_handle())) {}
    Instance(JC2_ValueHandle h) : Value(h) {}
    
    Class get_class() const {
        return Class(Env::api->get_class(Env::ctx, get_handle()));
    }
    
    Value get(const std::string& name) const {
        return Value(Env::api->instance_get_field(Env::ctx, get_handle(), name.c_str()));
    }
    
    void set(const std::string& name, const Value& val) {
        Env::api->instance_set_field(Env::ctx, get_handle(), name.c_str(), val.get_handle());
    }
};

class List : public Value {
public:
    List() : Value(Env::api->make_list(Env::ctx)) {}
    List(JC2_ValueHandle h) : Value(h) {}
    
    void push_back(const Value& val) {
        Env::api->list_push(Env::ctx, get_handle(), val.get_handle());
    }
    
    size_t size() const {
        return Env::api->list_size(Env::ctx, get_handle());
    }
    
    Value get(size_t index) const {
        return Value(Env::api->list_get(Env::ctx, get_handle(), index));
    }
    
    void set(size_t index, const Value& val) {
        Env::api->list_set(Env::ctx, get_handle(), index, val.get_handle());
    }
};

class Dict : public Value {
public:
    Dict() : Value(Env::api->make_dict(Env::ctx)) {}
    Dict(JC2_ValueHandle h) : Value(h) {}
    
    void set(const Value& key, const Value& val) {
        Env::api->dict_set(Env::ctx, get_handle(), key.get_handle(), val.get_handle());
    }
    
    Value get(const Value& key) const {
        return Value(Env::api->dict_get(Env::ctx, get_handle(), key.get_handle()));
    }
    
    bool has(const Value& key) const {
        return Env::api->dict_has(Env::ctx, get_handle(), key.get_handle());
    }
    
    void remove(const Value& key) {
        Env::api->dict_remove(Env::ctx, get_handle(), key.get_handle());
    }
    
    size_t size() const {
        return Env::api->dict_size(Env::ctx, get_handle());
    }
    
    List keys() const {
        return List(Env::api->dict_keys(Env::ctx, get_handle()));
    }
};

class Complex : public Value {
public:
    Complex(double r, double i) : Value(Env::api->make_complex(Env::ctx, r, i)) {}
    Complex(JC2_ValueHandle h) : Value(h) {}
    
    double real() const { return Env::api->complex_get_real(Env::ctx, get_handle()); }
    double imag() const { return Env::api->complex_get_imag(Env::ctx, get_handle()); }
};

class Set : public Value {
public:
    Set() : Value(Env::api->make_set(Env::ctx)) {}
    Set(JC2_ValueHandle h) : Value(h) {}
    
    void add(const Value& val) { Env::api->set_add(Env::ctx, get_handle(), val.get_handle()); }
    void remove(const Value& val) { Env::api->set_remove(Env::ctx, get_handle(), val.get_handle()); }
    bool has(const Value& val) const { return Env::api->set_has(Env::ctx, get_handle(), val.get_handle()); }
    size_t size() const { return Env::api->set_size(Env::ctx, get_handle()); }
    
    List elements() const {
        return List(Env::api->set_elements(Env::ctx, get_handle()));
    }
};

class RealMatrix : public Value {
public:
    RealMatrix(int rows, int cols) : Value(Env::api->make_real_matrix(Env::ctx, rows, cols)) {}
    RealMatrix(JC2_ValueHandle h) : Value(h) {}
    
    double get(int row, int col) const {
        return Env::api->real_matrix_get(Env::ctx, get_handle(), row, col);
    }
    
    void set(int row, int col, double val) {
        Env::api->real_matrix_set(Env::ctx, get_handle(), row, col, val);
    }
    
    int rows() const {
        return Env::api->real_matrix_rows(Env::ctx, get_handle());
    }
    
    int cols() const {
        return Env::api->real_matrix_cols(Env::ctx, get_handle());
    }
};

class ComplexMatrix : public Value {
public:
    ComplexMatrix(int rows, int cols) : Value(Env::api->make_complex_matrix(Env::ctx, rows, cols)) {}
    ComplexMatrix(JC2_ValueHandle h) : Value(h) {}
    
    double get_real(int row, int col) const { return Env::api->complex_matrix_get_real(Env::ctx, get_handle(), row, col); }
    double get_imag(int row, int col) const { return Env::api->complex_matrix_get_imag(Env::ctx, get_handle(), row, col); }
    void set(int row, int col, double r, double i) { Env::api->complex_matrix_set(Env::ctx, get_handle(), row, col, r, i); }
    int rows() const { return Env::api->complex_matrix_rows(Env::ctx, get_handle()); }
    int cols() const { return Env::api->complex_matrix_cols(Env::ctx, get_handle()); }
};

class Function : public Value {
public:
    Function(JC2_ValueHandle h) : Value(h) {}
    
    Value call(const std::vector<Value>& args) const {
        std::vector<JC2_ValueHandle> handles(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            handles[i] = args[i].get_handle();
        }
        return Value(Env::api->call_function(Env::ctx, get_handle(), static_cast<int>(handles.size()), handles.data()));
    }
};

class BigInt : public Value {
public:
    BigInt(const std::string& str) : Value(Env::api->make_bigint(Env::ctx, str.c_str())) {}
    BigInt(JC2_ValueHandle h) : Value(h) {}
    
    std::string to_string() const {
        size_t len = 0;
        const char* s = Env::api->bigint_to_string(Env::ctx, get_handle(), &len);
        return s ? std::string(s, len) : "";
    }
};

class Fraction : public Value {
public:
    Fraction(const Value& num, const Value& den) : Value(Env::api->make_fraction(Env::ctx, num.get_handle(), den.get_handle())) {}
    Fraction(JC2_ValueHandle h) : Value(h) {}
    
    Value num() const { return Value(Env::api->fraction_get_num(Env::ctx, get_handle())); }
    Value den() const { return Value(Env::api->fraction_get_den(Env::ctx, get_handle())); }
};

class Namespace : public Value {
public:
    Namespace(const std::string& name) : Value(Env::api->make_namespace(Env::ctx, name.c_str())) {}
    Namespace(JC2_ValueHandle h) : Value(h) {}
    
    void set(const std::string& key, const Value& val) {
        Env::api->namespace_set(Env::ctx, get_handle(), key.c_str(), val.get_handle());
    }
    
    Value get(const std::string& key) const {
        return Value(Env::api->namespace_get(Env::ctx, get_handle(), key.c_str()));
    }
};

class Slice : public Value {
public:
    static const int NONE = JC2_SLICE_NONE;

    Slice(int start, int end, int step) : Value(Env::api->make_slice(Env::ctx, start, end, step)) {}
    Slice(JC2_ValueHandle h) : Value(h) {}
    
    int start() const { return Env::api->slice_get_start(Env::ctx, get_handle()); }
    int end() const { return Env::api->slice_get_end(Env::ctx, get_handle()); }
    int step() const { return Env::api->slice_get_step(Env::ctx, get_handle()); }
};

class Module {
    JC2_ModuleHandle mod;
public:
    Module(JC2_ModuleHandle m) : mod(m) {}

    void register_help(const std::string& topic, const std::string& help_text) {
        Env::api->register_help(Env::ctx, topic.c_str(), help_text.c_str());
    }

    void register_function_help(const std::string& name, const std::string& signature, const std::string& desc, const std::string& example = "") {
        Env::api->register_function_help(Env::ctx, name.c_str(), signature.c_str(), desc.c_str(), example.c_str());
    }

    void register_function(const std::string& name, JC2_NativeFunc fn, int min_arity = 0, int max_arity = 16777215, bool has_rest = true, const std::vector<std::string>& param_names = {}, void* user_data = nullptr) {
        std::vector<const char*> c_param_names;
        for (const auto& p : param_names) c_param_names.push_back(p.c_str());
        Env::api->register_function(Env::ctx, mod, name.c_str(), fn, min_arity, max_arity, has_rest, c_param_names.data(), static_cast<int>(c_param_names.size()), user_data);
    }

    void register_int(const std::string& name, int32_t val) {
        Env::api->register_int(Env::ctx, mod, name.c_str(), val);
    }

    void register_double(const std::string& name, double val) {
        Env::api->register_double(Env::ctx, mod, name.c_str(), val);
    }

    void register_string(const std::string& name, const std::string& val) {
        Env::api->register_string(Env::ctx, mod, name.c_str(), val.c_str());
    }

    void register_value(const std::string& name, const Value& val) {
        Env::api->register_value(Env::ctx, mod, name.c_str(), val.get_handle());
    }
};

inline void throw_error(const std::string& msg) {
    Env::api->throw_error(Env::ctx, msg.c_str());
}

} // namespace jc2

// 开发者只需实现 jc2_init，宏会自动处理版本校验和环境初始化
#define JC2_EXTENSION_INIT \
    extern "C" JC2_EXPORT int jc2_extension_init(JC2_VMContext ctx, JC2_ModuleHandle mod, const JC2_HostAPI* api) { \
        jc2::Env::ctx = ctx; \
        jc2::Env::api = api; \
        if (api->magic != JC2_EXT_MAGIC || api->version != JC2_EXT_VERSION) return -1; \
        jc2::Module m(mod); \
        return jc2_init(m); \
    }

// User must implement: int jc2_init(jc2::Module& mod);

#endif
