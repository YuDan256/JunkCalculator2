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
#define JC2_EXT_VERSION 1

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
    bool (*is_instance)(JC2_VMContext ctx, JC2_ValueHandle v);
    
    /* --- 值提取 (Value Extraction) --- */
    bool (*as_bool)(JC2_VMContext ctx, JC2_ValueHandle v);
    int32_t (*as_int)(JC2_VMContext ctx, JC2_ValueHandle v);
    double (*as_double)(JC2_VMContext ctx, JC2_ValueHandle v);
    /* 返回的字符串指针由 JC2 引擎管理生命周期，DLL 不可 free */
    const char* (*as_string)(JC2_VMContext ctx, JC2_ValueHandle v, size_t* out_len);
    
    /* --- 原生对象生命周期管理 (Native Object Management) --- */
    JC2_ValueHandle (*make_class)(JC2_VMContext ctx, const char* name);
    JC2_ValueHandle (*make_instance)(JC2_VMContext ctx, JC2_ValueHandle class_handle);
    void (*bind_method)(JC2_VMContext ctx, JC2_ValueHandle class_handle, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, void* user_data);
    
    /* 将 DLL 内部 new 出来的指针绑定到实例，并注册析构回调 */
    void (*set_native_data)(JC2_VMContext ctx, JC2_ValueHandle instance, void* data, JC2_NativeDestructor dtor);
    /* 提取绑定的原生指针 */
    void* (*get_native_data)(JC2_VMContext ctx, JC2_ValueHandle instance);
    
    /* --- 模块注册 (Module Registration) --- */
    void (*register_help)(JC2_VMContext ctx, const char* topic, const char* help_text);
    void (*register_function)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, JC2_NativeFunc fn, int min_arity, int max_arity, bool has_rest, void* user_data);
    void (*register_int)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, int32_t val);
    void (*register_double)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, double val);
    void (*register_string)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, const char* val);
    void (*register_value)(JC2_VMContext ctx, JC2_ModuleHandle mod, const char* name, JC2_ValueHandle val);
    
    /* --- 异常处理 (Error Handling) --- */
    /* 触发 JC2 异常，此函数内部会执行 C++ throw，因此在 DLL 中调用后不应再执行后续逻辑 */
    void (*throw_error)(JC2_VMContext ctx, const char* msg);
    
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
