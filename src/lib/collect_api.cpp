#include "collect_api.h"
#include <fstream>
#include <sstream>
#include <cstdio>

namespace jcsym {

    Collected g_collected;

    // ============ 真实收集的 host API 回调 ============
    static JC2_ValueHandle cb_make_class(JC2_VMContext, const char* name) {
        uint64_t h = g_collected.nextHandle++;
        g_collected.handleToClass[h] = name;
        Class c;
        c.name = name;
        g_collected.classes.push_back(std::move(c));
        return h; // dummy handle
    }

    static void cb_bind_method(JC2_VMContext, JC2_ValueHandle class_handle, const char* name, JC2_NativeFunc,
        int min_arity, int max_arity, const char** param_names, int param_count,
        const char* rest_name, const char** kwarg_names, int kwarg_count,
        const char* kwargs_name, int, void*) {
        auto it = g_collected.handleToClass.find(class_handle);
        if (it == g_collected.handleToClass.end()) return;
        for (auto& c : g_collected.classes) {
            if (c.name != it->second) continue;
            Func f;
            f.name = name;
            f.minArity = min_arity;
            f.maxArity = max_arity;
            for (int i = 0; i < param_count; ++i) if (param_names[i]) f.params.push_back(param_names[i]);
            if (rest_name) f.restName = rest_name;
            for (int i = 0; i < kwarg_count; ++i) if (kwarg_names[i]) f.kwargNames.push_back(kwarg_names[i]);
            if (kwargs_name) f.kwargsName = kwargs_name;
            c.methods.push_back(std::move(f));
            break;
        }
    }

    static void cb_register_function(JC2_VMContext, JC2_ModuleHandle, const char* name, JC2_NativeFunc,
        int min_arity, int max_arity, const char** param_names, int param_count,
        const char* rest_name, const char** kwarg_names, int kwarg_count,
        const char* kwargs_name, int, void*) {
        Func f;
        f.name = name;
        f.minArity = min_arity;
        f.maxArity = max_arity;
        for (int i = 0; i < param_count; ++i) if (param_names[i]) f.params.push_back(param_names[i]);
        if (rest_name) f.restName = rest_name;
        for (int i = 0; i < kwarg_count; ++i) if (kwarg_names[i]) f.kwargNames.push_back(kwarg_names[i]);
        if (kwargs_name) f.kwargsName = kwargs_name;
        g_collected.functions.push_back(std::move(f));
    }

    static void cb_register_function_help(JC2_VMContext, const char* name, const char* signature, const char* desc, const char*) {
        Help h;
        h.name = name ? name : "";
        h.signature = signature ? signature : "";
        h.desc = desc ? desc : "";
        g_collected.help.push_back(std::move(h));
    }

    static void cb_register_help(JC2_VMContext, const char*, const char*) {
        // topic help 不参与符号解析，忽略
    }

    static void cb_register_value(JC2_VMContext, JC2_ModuleHandle, const char* name, JC2_ValueHandle) {
        Constant c;
        c.name = name ? name : "";
        c.type = "value";
        g_collected.constants.push_back(std::move(c));
    }
    static void cb_register_int(JC2_VMContext, JC2_ModuleHandle, const char* name, int32_t val) {
        Constant c;
        c.name = name ? name : "";
        c.type = "int";
        c.value = std::to_string(val);
        g_collected.constants.push_back(std::move(c));
    }
    static void cb_register_double(JC2_VMContext, JC2_ModuleHandle, const char* name, double val) {
        Constant c;
        c.name = name ? name : "";
        c.type = "double";
        c.value = std::to_string(val);
        g_collected.constants.push_back(std::move(c));
    }
    static void cb_register_string(JC2_VMContext, JC2_ModuleHandle, const char* name, const char* val) {
        Constant c;
        c.name = name ? name : "";
        c.type = "string";
        c.value = val ? val : "";
        g_collected.constants.push_back(std::move(c));
    }

    // ============ 构建只收集 host API ============
    JC2_HostAPI makeCollectApi() {
        JC2_HostAPI api{};
        api.magic = JC2_EXT_MAGIC;
        api.version = JC2_EXT_VERSION;

        // 值创建（桩）
        api.make_none = [](JC2_VMContext) -> JC2_ValueHandle { return 0; };
        api.make_bool = [](JC2_VMContext, bool) -> JC2_ValueHandle { return 0; };
        api.make_int = [](JC2_VMContext, int32_t) -> JC2_ValueHandle { return 0; };
        api.make_double = [](JC2_VMContext, double) -> JC2_ValueHandle { return 0; };
        api.make_string = [](JC2_VMContext, const char*, size_t) -> JC2_ValueHandle { return 0; };
        api.make_complex = [](JC2_VMContext, double, double) -> JC2_ValueHandle { return 0; };

        // 类型检查（桩）
        api.is_none = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.is_bool = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.is_int = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.is_double = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.is_string = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.is_complex = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.is_instance = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.is_type = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // 值提取（桩）
        api.as_bool = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.as_int = [](JC2_VMContext, JC2_ValueHandle) -> int32_t { return 0; };
        api.as_double = [](JC2_VMContext, JC2_ValueHandle) -> double { return 0.0; };
        api.as_string = [](JC2_VMContext, JC2_ValueHandle, size_t* out) -> const char* { if (out) *out = 0; return ""; };
        api.complex_get_real = [](JC2_VMContext, JC2_ValueHandle) -> double { return 0.0; };
        api.complex_get_imag = [](JC2_VMContext, JC2_ValueHandle) -> double { return 0.0; };

        // 类 + 方法（真实收集）
        api.make_class = cb_make_class;
        api.make_instance = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.bind_method = cb_bind_method;
        api.set_native_data = [](JC2_VMContext, JC2_ValueHandle, void*, JC2_NativeDestructor) {};
        api.get_native_data = [](JC2_VMContext, JC2_ValueHandle) -> void* { return nullptr; };
        api.set_buffer_data = [](JC2_VMContext, JC2_ValueHandle, void*, size_t) {};
        api.get_buffer_data = [](JC2_VMContext, JC2_ValueHandle, size_t* out) -> void* { if (out) *out = 0; return nullptr; };

        // 模块注册（真实收集）
        api.register_help = cb_register_help;
        api.register_function_help = cb_register_function_help;
        api.register_function = cb_register_function;
        api.register_int = cb_register_int;
        api.register_double = cb_register_double;
        api.register_string = cb_register_string;
        api.register_value = cb_register_value;

        // 异常（桩）
        api.throw_error = [](JC2_VMContext, const char*) {};

        // 列表（桩）
        api.make_list = [](JC2_VMContext) -> JC2_ValueHandle { return 0; };
        api.list_push = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) {};
        api.list_size = [](JC2_VMContext, JC2_ValueHandle) -> size_t { return 0; };
        api.list_get = [](JC2_VMContext, JC2_ValueHandle, size_t) -> JC2_ValueHandle { return 0; };
        api.list_set = [](JC2_VMContext, JC2_ValueHandle, size_t, JC2_ValueHandle) {};
        api.is_list = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // 字典（桩）
        api.make_dict = [](JC2_VMContext) -> JC2_ValueHandle { return 0; };
        api.dict_set = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle, JC2_ValueHandle) {};
        api.dict_get = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.dict_has = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) -> bool { return false; };
        api.dict_remove = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) {};
        api.dict_size = [](JC2_VMContext, JC2_ValueHandle) -> size_t { return 0; };
        api.is_dict = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // 扩展（桩）
        api.dict_keys = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.get_global = [](JC2_VMContext, const char*) -> JC2_ValueHandle { return 0; };
        api.to_string = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.resolve_path = [](JC2_VMContext, const char*) -> JC2_ValueHandle { return 0; };

        // 矩阵（桩）
        api.make_real_matrix = [](JC2_VMContext, int, int) -> JC2_ValueHandle { return 0; };
        api.real_matrix_get = [](JC2_VMContext, JC2_ValueHandle, int, int) -> double { return 0.0; };
        api.real_matrix_set = [](JC2_VMContext, JC2_ValueHandle, int, int, double) {};
        api.real_matrix_rows = [](JC2_VMContext, JC2_ValueHandle) -> int { return 0; };
        api.real_matrix_cols = [](JC2_VMContext, JC2_ValueHandle) -> int { return 0; };
        api.is_real_matrix = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };
        api.make_complex_matrix = [](JC2_VMContext, int, int) -> JC2_ValueHandle { return 0; };
        api.complex_matrix_get_real = [](JC2_VMContext, JC2_ValueHandle, int, int) -> double { return 0.0; };
        api.complex_matrix_get_imag = [](JC2_VMContext, JC2_ValueHandle, int, int) -> double { return 0.0; };
        api.complex_matrix_set = [](JC2_VMContext, JC2_ValueHandle, int, int, double, double) {};
        api.complex_matrix_rows = [](JC2_VMContext, JC2_ValueHandle) -> int { return 0; };
        api.complex_matrix_cols = [](JC2_VMContext, JC2_ValueHandle) -> int { return 0; };
        api.is_complex_matrix = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // 集合（桩）
        api.make_set = [](JC2_VMContext) -> JC2_ValueHandle { return 0; };
        api.set_add = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) {};
        api.set_remove = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) {};
        api.set_has = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) -> bool { return false; };
        api.set_size = [](JC2_VMContext, JC2_ValueHandle) -> size_t { return 0; };
        api.set_elements = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.is_set = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // 函数（桩）
        api.call_function = [](JC2_VMContext, JC2_ValueHandle, int, JC2_ValueHandle*) -> JC2_ValueHandle { return 0; };
        api.is_function = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // BigInt（桩）
        api.make_bigint = [](JC2_VMContext, const char*) -> JC2_ValueHandle { return 0; };
        api.bigint_to_string = [](JC2_VMContext, JC2_ValueHandle, size_t* out) -> const char* { if (out) *out = 0; return ""; };
        api.is_bigint = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // Fraction（桩）
        api.make_fraction = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.fraction_get_num = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.fraction_get_den = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.is_fraction = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // Namespace（桩）
        api.make_namespace = [](JC2_VMContext, const char*) -> JC2_ValueHandle { return 0; };
        api.namespace_set = [](JC2_VMContext, JC2_ValueHandle, const char*, JC2_ValueHandle) {};
        api.namespace_get = [](JC2_VMContext, JC2_ValueHandle, const char*) -> JC2_ValueHandle { return 0; };
        api.is_namespace = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // Slice（桩）
        api.make_slice = [](JC2_VMContext, int, int, int) -> JC2_ValueHandle { return 0; };
        api.slice_get_start = [](JC2_VMContext, JC2_ValueHandle) -> int { return 0; };
        api.slice_get_end = [](JC2_VMContext, JC2_ValueHandle) -> int { return 0; };
        api.slice_get_step = [](JC2_VMContext, JC2_ValueHandle) -> int { return 0; };
        api.is_slice = [](JC2_VMContext, JC2_ValueHandle) -> bool { return false; };

        // 类型（桩）
        api.get_type = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.type_name = [](JC2_VMContext, JC2_ValueHandle, size_t* out) -> const char* { if (out) *out = 0; return ""; };
        api.type_union = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.type_intersect = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.check_type = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) -> bool { return false; };

        // 高级类（桩）
        api.set_class_parent = [](JC2_VMContext, JC2_ValueHandle, JC2_ValueHandle) {};
        api.get_class = [](JC2_VMContext, JC2_ValueHandle) -> JC2_ValueHandle { return 0; };
        api.set_class_allocator = [](JC2_VMContext, JC2_ValueHandle, JC2_NativeFunc, void*) {};
        api.instance_get_field = [](JC2_VMContext, JC2_ValueHandle, const char*) -> JC2_ValueHandle { return 0; };
        api.instance_set_field = [](JC2_VMContext, JC2_ValueHandle, const char*, JC2_ValueHandle) {};

        // 冻结（桩）
        api.freeze_object = [](JC2_VMContext, JC2_ValueHandle) {};

        return api;
    }

    // ============ JSON 输出 ============
    static std::string escapeJson(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
            }
        }
        return out;
    }

    void writeSidecarJson(const Collected& c, const std::string& module, const std::string& outPath) {
        std::ofstream f(outPath, std::ios::binary);
        if (!f.is_open()) return;

        f << "{\n";
        f << "  \"module\": \"" << escapeJson(module) << "\",\n";

        f << "  \"functions\": [";
        for (size_t i = 0; i < c.functions.size(); ++i) {
            const Func& fn = c.functions[i];
            if (i) f << ",";
            f << "\n    {\"name\": \"" << escapeJson(fn.name) << "\", \"minArity\": " << fn.minArity
              << ", \"maxArity\": " << fn.maxArity << ", \"params\": [";
            for (size_t j = 0; j < fn.params.size(); ++j) {
                if (j) f << ", ";
                f << "\"" << escapeJson(fn.params[j]) << "\"";
            }
            f << "]";
            if (!fn.restName.empty()) f << ", \"rest\": \"" << escapeJson(fn.restName) << "\"";
            if (!fn.kwargNames.empty()) {
                f << ", \"kwargs\": [";
                for (size_t j = 0; j < fn.kwargNames.size(); ++j) {
                    if (j) f << ", ";
                    f << "\"" << escapeJson(fn.kwargNames[j]) << "\"";
                }
                f << "]";
            }
            f << "}";
        }
        f << "\n  ],\n";

        f << "  \"classes\": [";
        for (size_t i = 0; i < c.classes.size(); ++i) {
            const Class& cls = c.classes[i];
            if (i) f << ",";
            f << "\n    {\"name\": \"" << escapeJson(cls.name) << "\", \"methods\": [";
            for (size_t j = 0; j < cls.methods.size(); ++j) {
                const Func& m = cls.methods[j];
                if (j) f << ",";
                f << "\n      {\"name\": \"" << escapeJson(m.name) << "\", \"minArity\": " << m.minArity
                  << ", \"maxArity\": " << m.maxArity << ", \"params\": [";
                for (size_t k = 0; k < m.params.size(); ++k) {
                    if (k) f << ", ";
                    f << "\"" << escapeJson(m.params[k]) << "\"";
                }
                f << "]";
                if (!m.restName.empty()) f << ", \"rest\": \"" << escapeJson(m.restName) << "\"";
                f << "}";
            }
            f << "\n    ]}";
        }
        f << "\n  ],\n";

        f << "  \"help\": [";
        for (size_t i = 0; i < c.help.size(); ++i) {
            const Help& h = c.help[i];
            if (i) f << ",";
            f << "\n    {\"name\": \"" << escapeJson(h.name) << "\", \"signature\": \"" << escapeJson(h.signature)
              << "\", \"desc\": \"" << escapeJson(h.desc) << "\"}";
        }
        f << "\n  ]\n";
        f << "}\n";
        f.close();
    }

} // namespace jcsym
