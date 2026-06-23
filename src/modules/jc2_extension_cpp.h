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
    bool is_instance() const { return Env::api->is_instance(Env::ctx, handle); }

    bool as_bool() const { return Env::api->as_bool(Env::ctx, handle); }
    int32_t as_int() const { return Env::api->as_int(Env::ctx, handle); }
    double as_double() const { return Env::api->as_double(Env::ctx, handle); }
    std::string as_string() const {
        size_t len = 0;
        const char* s = Env::api->as_string(Env::ctx, handle, &len);
        return s ? std::string(s, len) : "";
    }

    void set_native_data(void* data, JC2_NativeDestructor dtor) {
        Env::api->set_native_data(Env::ctx, handle, data, dtor);
    }

    template<typename T>
    T* get_native_data() const {
        return static_cast<T*>(Env::api->get_native_data(Env::ctx, handle));
    }
};

class Class : public Value {
public:
    Class(const std::string& name) : Value(Env::api->make_class(Env::ctx, name.c_str())) {}

    void bind_method(const std::string& name, JC2_NativeFunc fn, int min_arity = 0, int max_arity = 255, bool has_rest = true, void* user_data = nullptr) {
        Env::api->bind_method(Env::ctx, get_handle(), name.c_str(), fn, min_arity, max_arity, has_rest, user_data);
    }
};

class Instance : public Value {
public:
    Instance(const Class& cls) 
        : Value(Env::api->make_instance(Env::ctx, cls.get_handle())) {}
};

class Module {
    JC2_ModuleHandle mod;
public:
    Module(JC2_ModuleHandle m) : mod(m) {}

    void register_help(const std::string& topic, const std::string& help_text) {
        Env::api->register_help(Env::ctx, topic.c_str(), help_text.c_str());
    }

    void register_function(const std::string& name, JC2_NativeFunc fn, int min_arity = 0, int max_arity = 255, bool has_rest = true, void* user_data = nullptr) {
        Env::api->register_function(Env::ctx, mod, name.c_str(), fn, min_arity, max_arity, has_rest, user_data);
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
