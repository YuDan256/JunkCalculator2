#include "../jc2_extension_cpp.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

enum class FFIType { INT, DOUBLE };

template<typename Ret, typename... Args>
struct FFIDispatch {
    static Ret call(void* sym, const std::vector<FFIType>& types, const uint64_t* vals, size_t idx, Args... args) {
        if (idx == types.size()) {
            return reinterpret_cast<Ret(*)(Args...)>(sym)(args...);
        }
        if constexpr (sizeof...(Args) >= 6) {
            throw std::runtime_error("FFI Error: Maximum 6 arguments supported.");
        } else {
            if (types[idx] == FFIType::INT) {
                return FFIDispatch<Ret, Args..., uint64_t>::call(sym, types, vals, idx + 1, args..., vals[idx]);
            } else {
                double d; std::memcpy(&d, &vals[idx], sizeof(double));
                return FFIDispatch<Ret, Args..., double>::call(sym, types, vals, idx + 1, args..., d);
            }
        }
    }
};

struct FFICallData {
    void* sym;
    std::string retTypeStr;
    std::vector<std::string> argTypeStrs;
    std::vector<FFIType> argTypes;
};

static jc2::Class* g_ffiLibClass = nullptr;
static jc2::Class* g_ffiCallableClass = nullptr;

JC2_ValueHandle ffi_callable_call(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    auto data = self.get_native_data<FFICallData>();
    if (!data) jc2::throw_error("FFI Error: Invalid callable.");

    size_t nargs = data->argTypes.size();
    if ((size_t)(argc - 1) != nargs) {
        jc2::throw_error("FFI Error: Argument count mismatch.");
    }

    std::vector<std::string> strArgs(nargs);
    uint64_t args[6] = {0};

    for (size_t i = 0; i < nargs; ++i) {
        jc2::Value arg(argv[i + 1]);
        const std::string& t = data->argTypeStrs[i];
        if (t == "string") {
            strArgs[i] = arg.as_string();
            args[i] = (uint64_t)strArgs[i].c_str();
        } else if (t == "f64") {
            double d = arg.as_double();
            std::memcpy(&args[i], &d, sizeof(double));
        } else if (t == "pointer") {
            if (arg.is_instance()) {
                jc2::Instance inst(arg.get_handle());
                jc2::Value bytesVal = inst;
                jc2::Value bufField = inst.get("buf");
                if (!bufField.is_none()) bytesVal = bufField;
                
                if (bytesVal.is_instance()) {
                    jc2::Instance bInst(bytesVal.get_handle());
                    auto vecPtr = bInst.get_native_data<std::vector<uint8_t>>();
                    if (vecPtr) {
                        args[i] = (uint64_t)vecPtr->data();
                        continue;
                    }
                }
            }
            if (arg.is_bigint()) {
                args[i] = (uint64_t)std::stoll(jc2::BigInt(arg.get_handle()).to_string());
            } else {
                args[i] = (uint64_t)arg.as_double();
            }
        } else {
            if (arg.is_bigint()) {
                args[i] = (uint64_t)std::stoll(jc2::BigInt(arg.get_handle()).to_string());
            } else {
                args[i] = (uint64_t)arg.as_double();
            }
        }
    }

    if (data->retTypeStr == "void") {
        FFIDispatch<void>::call(data->sym, data->argTypes, args, 0);
        return jc2::Value().get_handle();
    } else if (data->retTypeStr == "f32") {
        jc2::throw_error("FFI Error: f32 return type not supported.");
        return jc2::Value().get_handle();
    } else if (data->retTypeStr == "f64") {
        double ret = FFIDispatch<double>::call(data->sym, data->argTypes, args, 0);
        return jc2::Value(ret).get_handle();
    } else {
        uint64_t ret = FFIDispatch<uint64_t>::call(data->sym, data->argTypes, args, 0);
        if (data->retTypeStr == "i32") return jc2::Value((double)(int32_t)ret).get_handle();
        if (data->retTypeStr == "u32") return jc2::BigInt(std::to_string((uint32_t)ret)).get_handle();
        if (data->retTypeStr == "string") return ret ? jc2::Value((const char*)ret).get_handle() : jc2::Value().get_handle();
        if (data->retTypeStr == "pointer") return jc2::BigInt(std::to_string((int64_t)ret)).get_handle();
        return jc2::BigInt(std::to_string((int64_t)ret)).get_handle();
    }
}

JC2_ValueHandle ffi_lib_bind(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    void* handle = self.get_native_data<void>();
    if (!handle) jc2::throw_error("FFI Error: Invalid library handle.");

    std::string fnName = jc2::Value(argv[1]).as_string();
    std::string retTypeStr = jc2::Value(argv[2]).as_string();
    
    std::vector<std::string> parsedArgTypes;
    jc2::Value argTypesVal(argv[3]);
    if (argTypesVal.is_list()) {
        jc2::List argList(argTypesVal.get_handle());
        for (size_t i = 0; i < argList.size(); ++i) {
            parsedArgTypes.push_back(argList.get(i).as_string());
        }
    } else if (argTypesVal.is_string_matrix()) {
        jc2::StringMatrix argMat(argTypesVal.get_handle());
        for (int i = 0; i < argMat.rows(); ++i) {
            for (int j = 0; j < argMat.cols(); ++j) {
                parsedArgTypes.push_back(argMat.get(i, j));
            }
        }
    } else {
        jc2::throw_error("FFI Error: Argument types must be a list or string matrix.");
    }

    void* sym = nullptr;
#ifdef _WIN32
    sym = (void*)GetProcAddress((HMODULE)handle, fnName.c_str());
#else
    sym = dlsym(handle, fnName.c_str());
#endif
    if (!sym) jc2::throw_error("FFI Error: Symbol '" + fnName + "' not found.");

    size_t nargs = parsedArgTypes.size();
    if (nargs > 6) {
        jc2::throw_error("FFI Error: Zero-dependency FFI supports a maximum of 6 arguments.");
    }

    auto callData = new FFICallData();
    callData->sym = sym;
    callData->retTypeStr = retTypeStr;
    
    for (size_t i = 0; i < nargs; ++i) {
        std::string t = parsedArgTypes[i];
        callData->argTypeStrs.push_back(t);
        if (t == "f32") {
            delete callData;
            jc2::throw_error("FFI Error: f32 is not supported. Use f64.");
        } else if (t == "f64") {
            callData->argTypes.push_back(FFIType::DOUBLE);
        } else {
            callData->argTypes.push_back(FFIType::INT);
        }
    }

    jc2::Instance callableInst(*g_ffiCallableClass);
    callableInst.set_native_data(callData, [](void* ptr) {
        delete static_cast<FFICallData*>(ptr);
    });

    return callableInst.get_handle();
}

JC2_ValueHandle global_load(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    std::string path = jc2::Value(argv[0]).as_string();
    void* handle = nullptr;
#ifdef _WIN32
    handle = LoadLibraryA(path.c_str());
#else
    handle = dlopen(path.c_str(), RTLD_LAZY);
#endif
    if (!handle) {
        jc2::throw_error("FFI Error: Failed to load library '" + path + "'");
    }

    jc2::Instance libInst(*g_ffiLibClass);
    libInst.set_native_data(handle, [](void*) {
        // Optional: FreeLibrary / dlclose
    });

    return libInst.get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_ffiLibClass = new jc2::Class("FFILibrary");
    g_ffiLibClass->bind_method("bind", ffi_lib_bind, 3, 3, false);
    mod.register_value("FFILibrary", *g_ffiLibClass);

    g_ffiCallableClass = new jc2::Class("FFICallable");
    g_ffiCallableClass->bind_method("__call__", ffi_callable_call, 0, 6, true);
    mod.register_value("FFICallable", *g_ffiCallableClass);

    mod.register_function("load", global_load, 1, 1, false);

    mod.register_help("ffi",
        "═══ Foreign Function Interface (FFI) — Native Module ═══\n\n"
        "  Requires: import ffi\n\n"
        "  The `ffi` module provides a zero-dependency Foreign Function Interface\n"
        "  allowing JC2 to dynamically load C/C++ shared libraries (.dll, .so, .dylib)\n"
        "  and call their exported functions directly from scripts.\n\n"
        "  Loading Libraries\n"
        "  ──────────────────────\n"
        "    libc = ffi.load(\"msvcrt.dll\")    // Windows C standard library\n"
        "    libm = ffi.load(\"libm.so.6\")     // Linux math library\n\n"
        "  Binding Functions\n"
        "  ──────────────────────\n"
        "    func = libc.bind(\"function_name\", \"return_type\", [\"arg1_type\", ...])\n\n"
        "    Supported Types:\n"
        "      \"i32\", \"u32\", \"i64\", \"u64\" : Integers (passed/returned as 64-bit internally)\n"
        "      \"f64\"                      : 64-bit double precision float\n"
        "      \"string\"                   : Null-terminated C string (const char*)\n"
        "      \"pointer\"                  : Raw memory address (passed as int)\n"
        "      \"void\"                     : No return value (returns none)\n\n"
        "    * Note: \"f32\" (float) is intentionally unsupported to prevent binary bloat.\n"
        "      Maximum supported arguments per function is 6.\n\n"
        "  Examples\n"
        "  ──────────────────────\n"
        "    import ffi\n"
        "    libc = ffi.load(\"msvcrt.dll\")\n\n"
        "    // int puts(const char *str);\n"
        "    puts = libc.bind(\"puts\", \"i32\", [\"string\"])\n"
        "    puts(\"Hello from C!\")\n\n"
        "    // double pow(double x, double y);\n"
        "    c_pow = libc.bind(\"pow\", \"f64\", [\"f64\", \"f64\"])\n"
        "    print(c_pow(2.0, 10.0))  // 1024.0\n\n"
        "  Memory & Pointers\n"
        "  ──────────────────────\n"
        "    Use the `buffer` module to allocate raw memory and pass it as a \"pointer\".\n"
        "    import buffer\n"
        "    time = libc.bind(\"time\", \"i64\", [\"pointer\"])\n"
        "    ptr = buffer.alloc(8)\n"
        "    time(ptr)              // ptr is automatically passed as void*\n"
        "    print(ptr.readI64())"
    );

    mod.register_function_help("ffi.load", "ffi.load(path)", "Loads a dynamic shared library (.dll, .so, .dylib) and returns a library object containing the `bind` method.", "libc = ffi.load(\"msvcrt.dll\")");
    mod.register_function_help("bind", "lib.bind(name, retType, argTypes)", "Binds a C function from a loaded library. argTypes can be a list or stringmatrix of type names.", "puts = libc.bind(\"puts\", \"i32\", [\"string\"])");

    return 0;
}

JC2_EXTENSION_INIT
