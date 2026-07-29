#include "../jc2_extension_cpp.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <memory>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#error "FFI module currently only supports Windows x64."
#endif

using namespace jc2;

// ============================================================================
// 1. 内存管理层 (ExecutableMemory)
// ============================================================================
class ExecutableMemory {
    void* memory;
    size_t size;
public:
    ExecutableMemory(size_t sz) : size(sz) {
#ifdef _WIN32
        // 申请可读、可写、可执行的内存页
        memory = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory) {
            throw std::runtime_error("FFI Error: Failed to allocate executable memory.");
        }
#endif
    }

    ~ExecutableMemory() {
#ifdef _WIN32
        if (memory) {
            VirtualFree(memory, 0, MEM_RELEASE);
        }
#endif
    }

    void* get() const { return memory; }

    // 禁用拷贝语义，确保 RAII 安全
    ExecutableMemory(const ExecutableMemory&) = delete;
    ExecutableMemory& operator=(const ExecutableMemory&) = delete;
};

// ============================================================================
// 2. 类型描述系统 (FFITypeDesc)
// ============================================================================
enum class FFIType : uint8_t {
    VOID_TYPE,
    I8, U8, I16, U16, I32, U32, I64, U64,
    F32, F64,
    POINTER,
    STRING
};

struct FFITypeDesc {
    FFIType type;
    size_t size;
};

// 将字符串解析为底层类型描述
FFITypeDesc parseType(const std::string& t) {
    if (t == "void") return { FFIType::VOID_TYPE, 0 };
    if (t == "i8") return { FFIType::I8, 1 };
    if (t == "u8") return { FFIType::U8, 1 };
    if (t == "i16") return { FFIType::I16, 2 };
    if (t == "u16") return { FFIType::U16, 2 };
    if (t == "i32") return { FFIType::I32, 4 };
    if (t == "u32") return { FFIType::U32, 4 };
    if (t == "i64") return { FFIType::I64, 8 };
    if (t == "u64") return { FFIType::U64, 8 };
    if (t == "f32") return { FFIType::F32, 4 };
    if (t == "f64") return { FFIType::F64, 8 };
    if (t == "pointer") return { FFIType::POINTER, 8 };
    if (t == "string") return { FFIType::STRING, 8 };
    throw std::runtime_error("FFI Error: Unsupported type '" + t + "'.");
}

// ============================================================================
// 3. 核心调用引擎 (ABIHandler & Trampoline)
// ============================================================================
// Windows x64 通用调用蹦床 (JIT 机器码)
const uint8_t win64_trampoline_code[] = {
    0x55, 0x48, 0x89, 0xE5, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x49, 0x89, 0xCC, 0x4D, 0x89, 0xCD, 0x48, 0x89, 0xD0, 0x48,
    0x29, 0xC4, 0x48, 0x89, 0xD1, 0x48, 0xC1, 0xE9, 0x03, 0x4C,
    0x89, 0xC6, 0x48, 0x89, 0xE7, 0xF3, 0x48, 0xA5, 0x48, 0x8B,
    0x0C, 0x24, 0x66, 0x48, 0x0F, 0x6E, 0xC1, 0x48, 0x8B, 0x54,
    0x24, 0x08, 0x66, 0x48, 0x0F, 0x6E, 0xCA, 0x4C, 0x8B, 0x44,
    0x24, 0x10, 0x66, 0x49, 0x0F, 0x6E, 0xD0, 0x4C, 0x8B, 0x4C,
    0x24, 0x18, 0x66, 0x49, 0x0F, 0x6E, 0xD9, 0x41, 0xFF, 0xD4,
    0x66, 0x41, 0x0F, 0xD6, 0x45, 0x00, 0x4C, 0x8B, 0x5D, 0x30,
    0x49, 0x89, 0x03, 0x48, 0x8D, 0x65, 0xE0, 0x41, 0x5D, 0x41,
    0x5C, 0x5F, 0x5E, 0x5D, 0xC3
};

class ABIHandler {
public:
    virtual ~ABIHandler() = default;
    virtual Value invoke(void* func, const std::vector<Value>& args, const std::vector<FFITypeDesc>& types, FFITypeDesc
        retType) = 0;
};

class Win64ABIHandler : public ABIHandler {
    ExecutableMemory execMem;
    typedef void (*TrampolineFunc)(void* func, size_t stack_size, const void* stack_data, double* out_double, uint64_t*
        out_int);
    TrampolineFunc trampoline;

    uint64_t extract_u64(const Value& v) {
        if (v.is_bigint()) return std::stoull(v.to_string());
        return static_cast<uint64_t>(v.as_double());
    }

public:
    Win64ABIHandler() : execMem(sizeof(win64_trampoline_code)) {
        std::memcpy(execMem.get(), win64_trampoline_code, sizeof(win64_trampoline_code));
        trampoline = reinterpret_cast<TrampolineFunc>(execMem.get());
    }

    Value invoke(void* func, const std::vector<Value>& args, const std::vector<FFITypeDesc>& types, FFITypeDesc retType)
        override {
        size_t argc = args.size();
        size_t stack_slots = (argc < 4) ? 4 : argc;
        if (stack_slots % 2 != 0) stack_slots++; // 保证 16 字节对齐

        size_t stack_size = stack_slots * 8;
        std::vector<uint64_t> stack_data(stack_slots, 0);

        for (size_t i = 0; i < argc; ++i) {
            uint64_t val64 = 0;
            switch (types[i].type) {
            case FFIType::I8:
            case FFIType::U8:
            case FFIType::I16:
            case FFIType::U16:
            case FFIType::I32:
            case FFIType::U32:
            case FFIType::I64:
            case FFIType::U64:
            case FFIType::POINTER:
                val64 = extract_u64(args[i]);
                break;
            case FFIType::F32: {
                float f = static_cast<float>(args[i].as_double());
                std::memcpy(&val64, &f, sizeof(float));
                break;
            }
            case FFIType::F64: {
                double d = args[i].as_double();
                std::memcpy(&val64, &d, sizeof(double));
                break;
            }
            case FFIType::STRING:
                val64 = reinterpret_cast<uint64_t>(args[i].as_string().c_str());
                break;
            default:
                throw std::runtime_error("FFI Error: Unsupported argument type.");
            }
            stack_data[i] = val64;
        }

        double out_d = 0.0;
        uint64_t out_i = 0;

        // 核心调用：跳转到可执行内存中的机器码
        trampoline(func, stack_size, stack_data.data(), &out_d, &out_i);

        // 解析返回值
        switch (retType.type) {
        case FFIType::VOID_TYPE: return Value();
        case FFIType::I8: return Value(static_cast<int32_t>(static_cast<int8_t>(out_i)));
        case FFIType::U8: return Value(static_cast<int32_t>(static_cast<uint8_t>(out_i)));
        case FFIType::I16: return Value(static_cast<int32_t>(static_cast<int16_t>(out_i)));
        case FFIType::U16: return Value(static_cast<int32_t>(static_cast<uint16_t>(out_i)));
        case FFIType::I32: return Value(static_cast<int32_t>(out_i));
        case FFIType::U32: return Value(static_cast<double>(static_cast<uint32_t>(out_i)));
        case FFIType::I64:
        case FFIType::U64:
        case FFIType::POINTER: {
            if (out_i <= 9007199254740991ULL) {
                return Value(static_cast<double>(out_i));
            }
            else {
                return BigInt(std::to_string(out_i));
            }
        }
        case FFIType::F32: {
            float f;
            std::memcpy(&f, &out_d, sizeof(float));
            return Value(static_cast<double>(f));
        }
        case FFIType::F64: return Value(out_d);
        case FFIType::STRING: {
            if (out_i == 0) return Value();
            return Value(reinterpret_cast<const char*>(out_i));
        }
        default: return Value();
        }
    }
};

// ============================================================================
// 4. JC2 绑定层 (FFILibrary & FFIFunction)
// ============================================================================
struct LibraryData {
    HMODULE handle;
};

struct FunctionData {
    void* func_ptr;
    FFITypeDesc ret_type;
    std::vector<FFITypeDesc> arg_types;
};

std::unique_ptr<ABIHandler> g_abiHandler;
std::unique_ptr<Class> g_libClass;
std::unique_ptr<Class> g_funcClass;

JC2_ValueHandle lib_alloc(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx;
    (void)user_data;
    if (argc < 1 || !Value(argv[0]).is_string()) {
        throw_error("FFILibrary requires a string path.");
        return Value().get_handle();
    }
    std::string path = Value(argv[0]).as_string();
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle) {
        throw_error("FFI Error: Failed to load library '" + path + "'.");
        return Value().get_handle();
    }
    
    Instance inst(*g_libClass);
    LibraryData* data = new LibraryData{handle};
    inst.set_native_data(data, [](void* ptr) {
        LibraryData* d = static_cast<LibraryData*>(ptr);
        if (d->handle) FreeLibrary(d->handle);
        delete d;
    });
    
    return inst.get_handle();
}

JC2_ValueHandle lib_bind(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx;
    (void)user_data;
    if (argc < 3) {
        throw_error("FFILibrary.bind requires func_name and ret_type.");
        return Value().get_handle();
    }
    Instance self(argv[0]);
    LibraryData* data = self.get_native_data<LibraryData>();
    if (!data || !data->handle) {
        throw_error("FFI Error: Invalid library handle.");
        return Value().get_handle();
    }
    
    std::string func_name = Value(argv[1]).as_string();
    void* func_ptr = (void*)GetProcAddress(data->handle, func_name.c_str());
    if (!func_ptr) {
        throw_error("FFI Error: Function '" + func_name + "' not found.");
        return Value().get_handle();
    }
    
    FFITypeDesc ret_type;
    try {
        ret_type = parseType(Value(argv[2]).as_string());
    } catch (const std::exception& e) {
        throw_error(e.what());
        return Value().get_handle();
    }
    
    std::vector<FFITypeDesc> arg_types;
    for (int i = 3; i < argc; ++i) {
        try {
            arg_types.push_back(parseType(Value(argv[i]).as_string()));
        } catch (const std::exception& e) {
            throw_error(e.what());
            return Value().get_handle();
        }
    }
    
    Instance func_inst(*g_funcClass);
    FunctionData* fdata = new FunctionData{func_ptr, ret_type, arg_types};
    func_inst.set_native_data(fdata, [](void* ptr) {
        delete static_cast<FunctionData*>(ptr);
    });
    
    return func_inst.get_handle();
}

JC2_ValueHandle func_call(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx;
    (void)user_data;
    Instance self(argv[0]);
    FunctionData* data = self.get_native_data<FunctionData>();
    if (!data || !data->func_ptr) {
        throw_error("FFI Error: Invalid function handle.");
        return Value().get_handle();
    }
    
    if (argc - 1 != data->arg_types.size()) {
        throw_error("FFI Error: Argument count mismatch. Expected " + std::to_string(data->arg_types.size()) + ", got " + std::to_string(argc - 1) + ".");
        return Value().get_handle();
    }
    
    std::vector<Value> call_args;
    for (int i = 1; i < argc; ++i) {
        call_args.push_back(Value(argv[i]));
    }
    
    try {
        Value result = g_abiHandler->invoke(data->func_ptr, call_args, data->arg_types, data->ret_type);
        return result.get_handle();
    } catch (const std::exception& e) {
        throw_error(e.what());
        return Value().get_handle();
    }
}

// ============================================================================
// 模块初始化入口
// ============================================================================
int jc2_init(jc2::Module& mod) {
    g_abiHandler = std::make_unique<Win64ABIHandler>();
    
    g_libClass = std::make_unique<Class>("FFILibrary");
    g_funcClass = std::make_unique<Class>("FFIFunction");
    
    g_libClass->set_allocator(lib_alloc);
    g_libClass->bind_method("bind", lib_bind, 2, 255, true);
    
    g_funcClass->bind_method("__call__", func_call, 0, 255, true);
    
    mod.register_value("FFILibrary", *g_libClass);
    mod.register_value("FFIFunction", *g_funcClass);
    
    mod.register_help("ffi", 
        "═══ Zero-Dependency Foreign Function Interface (FFI) ═══\n\n"
        "  Requires: import \"ffi\"\n\n"
        "  The FFI module allows JC2 to dynamically load native C/C++ dynamic\n"
        "  libraries (.dll) and call their functions directly at runtime, with\n"
        "  zero third-party dependencies. (Currently supports Windows x64 ABI).\n\n"
        "  1. Loading a Library\n"
        "  ──────────────────────\n"
        "    lib = ffi.FFILibrary(\"msvcrt.dll\")\n\n"
        "  2. Binding a Function\n"
        "  ──────────────────────\n"
        "    // bind(function_name, return_type, arg1_type, arg2_type, ...)\n"
        "    puts = lib.bind(\"puts\", \"i32\", \"string\")\n\n"
        "  3. Supported C Types\n"
        "  ──────────────────────\n"
        "    Integers:  \"i8\", \"u8\", \"i16\", \"u16\", \"i32\", \"u32\", \"i64\", \"u64\"\n"
        "    Floats:    \"f32\" (float), \"f64\" (double)\n"
        "    Memory:    \"pointer\" (raw address), \"string\" (const char*)\n"
        "    Return:    \"void\" (only valid as return_type)\n\n"
        "  4. Pointers & Memory Management\n"
        "  ──────────────────────\n"
        "    The \"pointer\" type represents a 64-bit memory address (void*, int*, etc.).\n"
        "    In JC2, pointers are passed and returned as standard integers.\n"
        "    You can use the `buffer` module to allocate memory and get its address:\n"
        "      buf = buffer.alloc(1024)\n"
        "      c_func(buf.address())  // Pass the raw integer address to C\n\n"
        "  Example\n"
        "  ──────────────────────\n"
        "    import ffi\n"
        "    libc = ffi.FFILibrary(\"msvcrt.dll\")\n"
        "    \n"
        "    // double pow(double base, double exp);\n"
        "    c_pow = libc.bind(\"pow\", \"f64\", \"f64\", \"f64\")\n"
        "    print(c_pow(2.0, 10.0))  // -> 1024.0\n"
        "    \n"
        "    // int puts(const char* str);\n"
        "    c_puts = libc.bind(\"puts\", \"i32\", \"string\")\n"
        "    c_puts(\"Hello from Native C!\")\n"
        "    \n"
        "    // Pointers: void* malloc(size_t size); void free(void* ptr);\n"
        "    c_malloc = libc.bind(\"malloc\", \"pointer\", \"u64\")\n"
        "    c_free = libc.bind(\"free\", \"void\", \"pointer\")\n"
        "    ptr = c_malloc(1024)     // Returns an integer address\n"
        "    c_free(ptr)"
    );
    
    mod.register_function_help("ffi.FFILibrary", "ffi.FFILibrary(path)", "Loads a native dynamic library (.dll) into memory.", "lib = ffi.FFILibrary(\"msvcrt.dll\")");
    mod.register_function_help("FFILibrary.bind", "lib.bind(func_name, ret_type, [arg_types...])", "Binds a C function from the loaded library with the specified return type and argument types.", "puts = lib.bind(\"puts\", \"i32\", \"string\")");
    
    return 0;
}

JC2_EXTENSION_INIT
