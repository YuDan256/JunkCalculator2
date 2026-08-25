#ifndef JC2_EXTENSION_API_H
#define JC2_EXTENSION_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 跨平台导出宏 */
#if defined(_WIN32) || defined(__CYGWIN__)
    #define JC2_EXPORT __declspec(dllexport)
#else
    #define JC2_EXPORT __attribute__((visibility("default")))
#endif

/* =========================================================================
 * 1. 不透明句柄 (Opaque Handles)
 * 隔离 C++ 的 jc::Value, ObjNamespace, VM 等复杂对象
 * ========================================================================= */
#define JC2_EXT_MAGIC 0x4A433245 // 'JC2E'
#define JC2_EXT_VERSION 5

#define JC2_SLICE_NONE (-2147483647 - 1)

/* 完美契合 JC2 的 NaN-Boxing (8 bytes)，无需堆分配即可传递值 */
typedef uint64_t JC2_ValueHandle;
typedef void* JC2_ModuleHandle;
typedef void* JC2_VMContext;

/* =========================================================================
 * 2. 回调函数签名 (Callbacks)
 * ========================================================================= */
/* DLL 导出的原生函数签名 */
typedef JC2_ValueHandle (*JC2_NativeFunc)(
    JC2_VMContext ctx, 
    int argc, 
    JC2_ValueHandle* argv, 
    void* user_data
);

/* 原生 C++ 对象在 JC2 垃圾回收时的析构回调 */
typedef void (*JC2_NativeDestructor)(void* ptr);

/* =========================================================================
 * 3. 宿主 API 函数表 (Host API Table)
 * EXE 传递给 DLL 的函数指针集合，DLL 只能通过这些指针操作 JC2 引擎
 * ========================================================================= */
typedef struct JC2_HostAPI {
    uint32_t magic;
    uint32_t version;

    /* --- 值创建 (Value Creation) --- */
    JC2_ValueHandle (*make_none)(JC2_VMContext ctx);
    JC2_ValueHandle (*make_bool)(JC2_VMContext ctx, bool b);
    JC2_ValueHandle (*make_int)(JC2_VMContext ctx, int32_t i);
    JC2_ValueHandle (*make_double)(JC2_VMContext ctx, double d);
    JC2_ValueHandle (*make_string)(JC2_VMContext ctx, const char* str, size_t len);
    JC2_ValueHandle (*make_complex)(JC2_VMContext ctx, double real, double imag);
    
    /* --- 类型检查 (Value Inspection) --- */
    bool (*is_none)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_bool)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_int)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_double)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_string)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_complex)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_instance)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_type)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- 值提取 (Value Extraction) --- */
    bool (*as_bool)(JC2_VMContext ctx, JC2_ValueHandle v);
    int32_t (*as_int)(JC2_VMContext ctx, JC2_ValueHandle v);
    double (*as_double)(JC2_VMContext ctx, JC2_ValueHandle v);
    /* 返回的字符串指针由 JC2 引擎管理生命周期，DLL 不可 free */
    const char* (*as_string)(JC2_VMContext ctx, JC2_ValueHandle v, size_t* out_len);
    double (*complex_get_real)(JC2_VMContext ctx, JC2_ValueHandle v);
    double (*complex_get_imag)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- 原生对象生命周期管理 (Native Object Management) --- */
    JC2_ValueHandle (*make_class)(JC2_VMContext ctx, const char* name);
    JC2_ValueHandle (*make_instance)(JC2_VMContext ctx, JC2_ValueHandle class_handle);
    void (*bind_method)(JC2_VMContext ctx, JC2_ValueHandle class_handle, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, const char** param_names, int param_count, void* user_data);
    
    /* 将 DLL 内部 new 出来的指针绑定到实例，并注册析构回调 */
    void (*set_native_data)(JC2_VMContext ctx, JC2_ValueHandle instance, void* data, JC2_NativeDestructor dtor);
    /* 提取绑定的原生指针 */
    void* (*get_native_data)(JC2_VMContext ctx, JC2_ValueHandle instance);
    
    /* --- 官方 Buffer 协议 (Buffer Protocol) --- */
    void (*set_buffer_data)(JC2_VMContext ctx, JC2_ValueHandle instance, void* data, size_t size);
    void* (*get_buffer_data)(JC2_VMContext ctx, JC2_ValueHandle instance, size_t* out_size);
    
    /* --- 模块注册 (Module Registration) --- */
    void (*register_help)(JC2_VMContext ctx, const char* topic, const char* help_text);
    void (*register_function_help)(JC2_VMContext ctx, const char* name, const char* signature, const char* desc, const char* example);
    void (*register_function)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, const char** param_names, int param_count, void* user_data);
    void (*register_int)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, int32_t val);
    void (*register_double)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, double val);
    void (*register_string)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, const char* val);
    void (*register_value)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, JC2_ValueHandle val);
    
    /* --- 异常处理 (Error Handling) --- */
    /* 触发 JC2 异常，此函数内部会执行 C++ throw，因此在 DLL 中调用后不应再执行后续逻辑 */
    void (*throw_error)(JC2_VMContext ctx, const char* msg);
    
    /* --- 列表操作 (List Operations) --- */
    JC2_ValueHandle (*make_list)(JC2_VMContext ctx);
    void (*list_push)(JC2_VMContext ctx, JC2_ValueHandle list, JC2_ValueHandle val);
    size_t (*list_size)(JC2_VMContext ctx, JC2_ValueHandle list);
    JC2_ValueHandle (*list_get)(JC2_VMContext ctx, JC2_ValueHandle list, size_t index);
    void (*list_set)(JC2_VMContext ctx, JC2_ValueHandle list, size_t index, JC2_ValueHandle val);
    bool (*is_list)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- 字典操作 (Dict Operations) --- */
    JC2_ValueHandle (*make_dict)(JC2_VMContext ctx);
    void (*dict_set)(JC2_VMContext ctx, JC2_ValueHandle dict, JC2_ValueHandle key, JC2_ValueHandle val);
    JC2_ValueHandle (*dict_get)(JC2_VMContext ctx, JC2_ValueHandle dict, JC2_ValueHandle key);
    bool (*dict_has)(JC2_VMContext ctx, JC2_ValueHandle dict, JC2_ValueHandle key);
    void (*dict_remove)(JC2_VMContext ctx, JC2_ValueHandle dict, JC2_ValueHandle key);
    size_t (*dict_size)(JC2_VMContext ctx, JC2_ValueHandle dict);
    bool (*is_dict)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- 扩展操作 (Extended Operations) --- */
    JC2_ValueHandle (*dict_keys)(JC2_VMContext ctx, JC2_ValueHandle dict);
    JC2_ValueHandle (*get_global)(JC2_VMContext ctx, const char* name);
    JC2_ValueHandle (*to_string)(JC2_VMContext ctx, JC2_ValueHandle v);
    JC2_ValueHandle (*resolve_path)(JC2_VMContext ctx, const char* path);
    
    /* --- 矩阵操作 (Matrix Operations) --- */
    JC2_ValueHandle (*make_real_matrix)(JC2_VMContext ctx, int rows, int cols);
    double (*real_matrix_get)(JC2_VMContext ctx, JC2_ValueHandle mat, int row, int col);
    void (*real_matrix_set)(JC2_VMContext ctx, JC2_ValueHandle mat, int row, int col, double val);
    int (*real_matrix_rows)(JC2_VMContext ctx, JC2_ValueHandle mat);
    int (*real_matrix_cols)(JC2_VMContext ctx, JC2_ValueHandle mat);
    bool (*is_real_matrix)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    JC2_ValueHandle (*make_complex_matrix)(JC2_VMContext ctx, int rows, int cols);
    double (*complex_matrix_get_real)(JC2_VMContext ctx, JC2_ValueHandle mat, int row, int col);
    double (*complex_matrix_get_imag)(JC2_VMContext ctx, JC2_ValueHandle mat, int row, int col);
    void (*complex_matrix_set)(JC2_VMContext ctx, JC2_ValueHandle mat, int row, int col, double r, double i);
    int (*complex_matrix_rows)(JC2_VMContext ctx, JC2_ValueHandle mat);
    int (*complex_matrix_cols)(JC2_VMContext ctx, JC2_ValueHandle mat);
    bool (*is_complex_matrix)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- 集合操作 (Set Operations) --- */
    JC2_ValueHandle (*make_set)(JC2_VMContext ctx);
    void (*set_add)(JC2_VMContext ctx, JC2_ValueHandle set, JC2_ValueHandle val);
    void (*set_remove)(JC2_VMContext ctx, JC2_ValueHandle set, JC2_ValueHandle val);
    bool (*set_has)(JC2_VMContext ctx, JC2_ValueHandle set, JC2_ValueHandle val);
    size_t (*set_size)(JC2_VMContext ctx, JC2_ValueHandle set);
    JC2_ValueHandle (*set_elements)(JC2_VMContext ctx, JC2_ValueHandle set);
    bool (*is_set)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- 函数调用 (Function Calling) --- */
    JC2_ValueHandle (*call_function)(JC2_VMContext ctx, JC2_ValueHandle func, int argc, JC2_ValueHandle* argv);
    bool (*is_function)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- BigInt 操作 (BigInt Operations) --- */
    JC2_ValueHandle (*make_bigint)(JC2_VMContext ctx, const char* str);
    const char* (*bigint_to_string)(JC2_VMContext ctx, JC2_ValueHandle v, size_t* out_len);
    bool (*is_bigint)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- Fraction 操作 (Fraction Operations) --- */
    JC2_ValueHandle (*make_fraction)(JC2_VMContext ctx, JC2_ValueHandle num, JC2_ValueHandle den);
    JC2_ValueHandle (*fraction_get_num)(JC2_VMContext ctx, JC2_ValueHandle v);
    JC2_ValueHandle (*fraction_get_den)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_fraction)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- Namespace 操作 (Namespace Operations) --- */
    JC2_ValueHandle (*make_namespace)(JC2_VMContext ctx, const char* name);
    void (*namespace_set)(JC2_VMContext ctx, JC2_ValueHandle ns, const char* key, JC2_ValueHandle val);
    JC2_ValueHandle (*namespace_get)(JC2_VMContext ctx, JC2_ValueHandle ns, const char* key);
    bool (*is_namespace)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- Slice 操作 (Slice Operations) --- */
    JC2_ValueHandle (*make_slice)(JC2_VMContext ctx, int start, int end, int step);
    int (*slice_get_start)(JC2_VMContext ctx, JC2_ValueHandle v);
    int (*slice_get_end)(JC2_VMContext ctx, JC2_ValueHandle v);
    int (*slice_get_step)(JC2_VMContext ctx, JC2_ValueHandle v);
    bool (*is_slice)(JC2_VMContext ctx, JC2_ValueHandle v);

    /* --- 类型操作 (Type Operations) --- */
    JC2_ValueHandle (*get_type)(JC2_VMContext ctx, JC2_ValueHandle v);
    const char* (*type_name)(JC2_VMContext ctx, JC2_ValueHandle type_obj, size_t* out_len);
    JC2_ValueHandle (*type_union)(JC2_VMContext ctx, JC2_ValueHandle t1, JC2_ValueHandle t2);
    JC2_ValueHandle (*type_intersect)(JC2_VMContext ctx, JC2_ValueHandle t1, JC2_ValueHandle t2);
    bool (*check_type)(JC2_VMContext ctx, JC2_ValueHandle v, JC2_ValueHandle type_obj);
    
    /* --- 高级类与实例操作 (Advanced Class & Instance Operations) --- */
    void (*set_class_parent)(JC2_VMContext ctx, JC2_ValueHandle cls, JC2_ValueHandle parent);
    JC2_ValueHandle (*get_class)(JC2_VMContext ctx, JC2_ValueHandle inst);
    void (*set_class_allocator)(JC2_VMContext ctx, JC2_ValueHandle cls, JC2_NativeFunc fn, void* user_data);
    JC2_ValueHandle (*instance_get_field)(JC2_VMContext ctx, JC2_ValueHandle inst, const char* name);
    void (*instance_set_field)(JC2_VMContext ctx, JC2_ValueHandle inst, const char* name, JC2_ValueHandle val);
    
    /* --- 对象冻结 (Object Freezing) --- */
    void (*freeze_object)(JC2_VMContext ctx, JC2_ValueHandle v);
    
} JC2_HostAPI;

/* =========================================================================
 * 4. 模块入口点签名 (Entry Point Signature)
 * 所有的 JC2 动态库必须导出一个名为 `jc2_extension_init` 的函数，并匹配此签名
 * ========================================================================= */
typedef int (*JC2_ExtensionInitFunc)(JC2_VMContext ctx, JC2_ModuleHandle mod, const JC2_HostAPI* api);

#ifdef __cplusplus
}
#endif

#endif /* JC2_EXTENSION_API_H */
