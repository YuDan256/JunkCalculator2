#include "../jc2_extension_cpp.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <memory>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
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
struct StructLayoutData;

enum class FFIType : uint8_t {
    VOID_TYPE,
    I8, U8, I16, U16, I32, U32, I64, U64,
    F32, F64,
    POINTER,
    STRING,
    VARIADIC,
    STRUCT
};

struct FFITypeDesc {
    FFIType type;
    size_t size;
    size_t align;
    StructLayoutData* layout;
};

struct StructField {
    std::string name;
    FFITypeDesc type;
    size_t offset;
};

struct StructLayoutData {
    std::vector<StructField> fields;
    size_t size = 0;
    size_t align = 1;
};

struct StructInstanceData {
    StructLayoutData* layout;
    std::vector<uint8_t> memory;
};

extern std::unique_ptr<Class> g_structInstClass;

// 将字符串或结构体布局解析为底层类型描述
FFITypeDesc parseType(const Value& v) {
        if (v.is_string()) {
            std::string t = v.as_string();
            if (t == "void") return { FFIType::VOID_TYPE, 0, 1, nullptr };
            if (t == "i8") return { FFIType::I8, 1, 1, nullptr };
            if (t == "u8") return { FFIType::U8, 1, 1, nullptr };
            if (t == "i16") return { FFIType::I16, 2, 2, nullptr };
            if (t == "u16") return { FFIType::U16, 2, 2, nullptr };
            if (t == "i32") return { FFIType::I32, 4, 4, nullptr };
            if (t == "u32") return { FFIType::U32, 4, 4, nullptr };
            if (t == "i64") return { FFIType::I64, 8, 8, nullptr };
            if (t == "u64") return { FFIType::U64, 8, 8, nullptr };
            if (t == "f32") return { FFIType::F32, 4, 4, nullptr };
            if (t == "f64") return { FFIType::F64, 8, 8, nullptr };
            if (t == "pointer") return { FFIType::POINTER, 8, 8, nullptr };
            if (t == "string") return { FFIType::STRING, 8, 8, nullptr };
            if (t == "...") return { FFIType::VARIADIC, 0, 1, nullptr };
            throw std::runtime_error("FFI Error: Unsupported type '" + t + "'.");
        }
        if (v.is_instance()) {
            StructLayoutData* layout = v.get_native_data<StructLayoutData>();
            if (layout) {
                return { FFIType::STRUCT, layout->size, layout->align, layout };
            }
        }
        throw std::runtime_error("FFI Error: Invalid type descriptor.");
}

Value read_memory(const uint8_t* ptr, const FFITypeDesc& t) {
    switch (t.type) {
        case FFIType::I8: return Value(static_cast<int32_t>(*reinterpret_cast<const int8_t*>(ptr)));
        case FFIType::U8: return Value(static_cast<int32_t>(*reinterpret_cast<const uint8_t*>(ptr)));
        case FFIType::I16: return Value(static_cast<int32_t>(*reinterpret_cast<const int16_t*>(ptr)));
        case FFIType::U16: return Value(static_cast<int32_t>(*reinterpret_cast<const uint16_t*>(ptr)));
        case FFIType::I32: return Value(*reinterpret_cast<const int32_t*>(ptr));
        case FFIType::U32: return BigInt(std::to_string(*reinterpret_cast<const uint32_t*>(ptr)));
        case FFIType::I64: return BigInt(std::to_string(*reinterpret_cast<const int64_t*>(ptr)));
        case FFIType::U64: return BigInt(std::to_string(*reinterpret_cast<const uint64_t*>(ptr)));
        case FFIType::F32: return Value(static_cast<double>(*reinterpret_cast<const float*>(ptr)));
        case FFIType::F64: return Value(*reinterpret_cast<const double*>(ptr));
        case FFIType::POINTER: return BigInt(std::to_string(*reinterpret_cast<const uint64_t*>(ptr)));
        case FFIType::STRING: {
            const char* s = *reinterpret_cast<const char* const*>(ptr);
            return s ? Value(s) : Value();
        }
        case FFIType::STRUCT: {
            Instance inst(*g_structInstClass);
            StructInstanceData* data = new StructInstanceData{t.layout, std::vector<uint8_t>(t.size)};
            std::memcpy(data->memory.data(), ptr, t.size);
            inst.set_native_data(data, [](void* p) { delete static_cast<StructInstanceData*>(p); });
            return inst.get_handle();
        }
        default: return Value();
    }
}

void write_memory(uint8_t* ptr, const FFITypeDesc& t, const Value& v) {
    switch (t.type) {
        case FFIType::I8: *reinterpret_cast<int8_t*>(ptr) = static_cast<int8_t>(v.as_int()); break;
        case FFIType::U8: *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(v.as_int()); break;
        case FFIType::I16: *reinterpret_cast<int16_t*>(ptr) = static_cast<int16_t>(v.as_int()); break;
        case FFIType::U16: *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(v.as_int()); break;
        case FFIType::I32: *reinterpret_cast<int32_t*>(ptr) = v.as_int(); break;
        case FFIType::U32: *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(std::stoull(v.to_string())); break;
        case FFIType::I64: *reinterpret_cast<int64_t*>(ptr) = static_cast<int64_t>(std::stoull(v.to_string())); break;
        case FFIType::U64: *reinterpret_cast<uint64_t*>(ptr) = static_cast<uint64_t>(std::stoull(v.to_string())); break;
        case FFIType::F32: *reinterpret_cast<float*>(ptr) = static_cast<float>(v.as_double()); break;
        case FFIType::F64: *reinterpret_cast<double*>(ptr) = v.as_double(); break;
        case FFIType::POINTER: *reinterpret_cast<uint64_t*>(ptr) = static_cast<uint64_t>(std::stoull(v.to_string())); break;
        case FFIType::STRING: *reinterpret_cast<const char**>(ptr) = v.as_c_str(); break;
        case FFIType::STRUCT: {
            StructInstanceData* data = v.get_native_data<StructInstanceData>();
            if (!data || data->layout != t.layout) throw std::runtime_error("FFI Error: Struct type mismatch.");
            std::memcpy(ptr, data->memory.data(), t.size);
            break;
        }
        default: break;
    }
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
        bool hidden_ret = false;
        std::vector<uint8_t> ret_struct_mem;
        if (retType.type == FFIType::STRUCT && retType.size != 1 && retType.size != 2 && retType.size != 4 && retType.size != 8) {
            hidden_ret = true;
            ret_struct_mem.resize(retType.size);
        }

        size_t argc = args.size();
        size_t actual_argc = argc + (hidden_ret ? 1 : 0);
        size_t stack_slots = (actual_argc < 4) ? 4 : actual_argc;
        if (stack_slots % 2 != 0) stack_slots++; // 保证 16 字节对齐

        size_t stack_size = stack_slots * 8;
        std::vector<uint64_t> stack_data(stack_slots, 0);
        std::vector<std::vector<uint8_t>> temp_structs;

        size_t arg_idx = 0;
        if (hidden_ret) {
            stack_data[arg_idx++] = reinterpret_cast<uint64_t>(ret_struct_mem.data());
        }

        for (size_t i = 0; i < argc; ++i) {
            uint64_t val64 = 0;
            FFIType current_type;
            FFITypeDesc current_desc;
            
            if (i < types.size()) {
                current_desc = types[i];
                current_type = current_desc.type;
            } else {
                // 动态推断可变参数的类型
                if (args[i].is_bigint() || args[i].is_int()) { current_type = FFIType::I64; current_desc = {FFIType::I64, 8, 8, nullptr}; }
                else if (args[i].is_double()) { current_type = FFIType::F64; current_desc = {FFIType::F64, 8, 8, nullptr}; }
                else if (args[i].is_string()) { current_type = FFIType::STRING; current_desc = {FFIType::STRING, 8, 8, nullptr}; }
                else if (args[i].is_instance() && args[i].get_native_data<StructInstanceData>()) {
                    StructInstanceData* sdata = args[i].get_native_data<StructInstanceData>();
                    current_type = FFIType::STRUCT;
                    current_desc = {FFIType::STRUCT, sdata->layout->size, sdata->layout->align, sdata->layout};
                }
                else throw std::runtime_error("FFI Error: Unsupported variadic argument type.");
            }

            switch (current_type) {
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
                val64 = reinterpret_cast<uint64_t>(args[i].as_c_str());
                break;
            case FFIType::STRUCT: {
                StructInstanceData* sdata = args[i].get_native_data<StructInstanceData>();
                if (!sdata || sdata->layout != current_desc.layout) throw std::runtime_error("FFI Error: Struct type mismatch.");
                if (current_desc.size == 1 || current_desc.size == 2 || current_desc.size == 4 || current_desc.size == 8) {
                    std::memcpy(&val64, sdata->memory.data(), current_desc.size);
                } else {
                    temp_structs.push_back(sdata->memory);
                    val64 = reinterpret_cast<uint64_t>(temp_structs.back().data());
                }
                break;
            }
            default:
                throw std::runtime_error("FFI Error: Unsupported argument type.");
            }
            stack_data[arg_idx++] = val64;
        }

        double out_d = 0.0;
        uint64_t out_i = 0;

        // 核心调用：跳转到可执行内存中的机器码
        trampoline(func, stack_size, stack_data.data(), &out_d, &out_i);

        if (hidden_ret) {
            Instance inst(*g_structInstClass);
            StructInstanceData* data = new StructInstanceData{retType.layout, ret_struct_mem};
            inst.set_native_data(data, [](void* p) { delete static_cast<StructInstanceData*>(p); });
            return inst.get_handle();
        }

        // 解析返回值
        switch (retType.type) {
        case FFIType::VOID_TYPE: return Value();
        case FFIType::I8: return Value(static_cast<int32_t>(static_cast<int8_t>(out_i)));
        case FFIType::U8: return Value(static_cast<int32_t>(static_cast<uint8_t>(out_i)));
        case FFIType::I16: return Value(static_cast<int32_t>(static_cast<int16_t>(out_i)));
        case FFIType::U16: return Value(static_cast<int32_t>(static_cast<uint16_t>(out_i)));
        case FFIType::I32: return Value(static_cast<int32_t>(out_i));
        case FFIType::U32: return BigInt(std::to_string(static_cast<uint32_t>(out_i)));
        case FFIType::I64:
            return BigInt(std::to_string(static_cast<int64_t>(out_i)));
        case FFIType::U64:
        case FFIType::POINTER:
            return BigInt(std::to_string(out_i));
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
        case FFIType::STRUCT: {
            Instance inst(*g_structInstClass);
            StructInstanceData* data = new StructInstanceData{retType.layout, std::vector<uint8_t>(retType.size, 0)};
            std::memcpy(data->memory.data(), &out_i, retType.size);
            inst.set_native_data(data, [](void* p) { delete static_cast<StructInstanceData*>(p); });
            return inst.get_handle();
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
    bool is_variadic;
};

std::unique_ptr<ABIHandler> g_abiHandler;
std::unique_ptr<Class> g_libClass;
std::unique_ptr<Class> g_funcClass;
std::unique_ptr<Class> g_structLayoutClass;
std::unique_ptr<Class> g_structInstClass;

JC2_ValueHandle struct_layout_alloc(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)user_data;
    if (argc < 1 || !Value(argv[0]).is_dict()) {
        throw_error("ffi.Struct requires a dict.");
        return Value().get_handle();
    }
    Dict d(argv[0]);
    StructLayoutData* layout = new StructLayoutData();
    
    List keys = d.keys();
    for (size_t i = 0; i < keys.size(); ++i) {
        Value k = keys.get(i);
        Value v = d.get(k);
        try {
            FFITypeDesc t = parseType(v);
            size_t field_align = t.align;
            size_t offset = (layout->size + field_align - 1) & ~(field_align - 1);
            layout->fields.push_back({k.as_string(), t, offset});
            layout->size = offset + t.size;
            layout->align = std::max(layout->align, field_align);
        } catch (const std::exception& e) {
            delete layout;
            throw_error(e.what());
            return Value().get_handle();
        }
    }
    if (layout->align > 0) {
        layout->size = (layout->size + layout->align - 1) & ~(layout->align - 1);
    }
    
    Instance inst(*g_structLayoutClass);
    inst.set_native_data(layout, [](void* ptr) { delete static_cast<StructLayoutData*>(ptr); });
    return inst.get_handle();
}

JC2_ValueHandle struct_layout_call(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)argc; (void)user_data;
    Instance self(argv[0]);
    StructLayoutData* layout = self.get_native_data<StructLayoutData>();
    Instance inst(*g_structInstClass);
    StructInstanceData* data = new StructInstanceData{layout, std::vector<uint8_t>(layout->size, 0)};
    inst.set_native_data(data, [](void* ptr) { delete static_cast<StructInstanceData*>(ptr); });
    return inst.get_handle();
}

JC2_ValueHandle struct_inst_getattr(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)argc; (void)user_data;
    Instance self(argv[0]);
    std::string key = Value(argv[1]).as_string();
    StructInstanceData* data = self.get_native_data<StructInstanceData>();
    for (const auto& f : data->layout->fields) {
        if (f.name == key) {
            return read_memory(data->memory.data() + f.offset, f.type).get_handle();
        }
    }
    throw_error("Struct has no field '" + key + "'.");
    return Value().get_handle();
}

JC2_ValueHandle struct_inst_setattr(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)argc; (void)user_data;
    Instance self(argv[0]);
    std::string key = Value(argv[1]).as_string();
    Value val(argv[2]);
    StructInstanceData* data = self.get_native_data<StructInstanceData>();
    for (const auto& f : data->layout->fields) {
        if (f.name == key) {
            try {
                write_memory(data->memory.data() + f.offset, f.type, val);
                return Value().get_handle();
            } catch (const std::exception& e) {
                throw_error(e.what());
                return Value().get_handle();
            }
        }
    }
    throw_error("Struct has no field '" + key + "'.");
    return Value().get_handle();
}

JC2_ValueHandle struct_inst_address(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)argc; (void)user_data;
    Instance self(argv[0]);
    StructInstanceData* data = self.get_native_data<StructInstanceData>();
    uint64_t addr = reinterpret_cast<uint64_t>(data->memory.data());
    return BigInt(std::to_string(addr)).get_handle();
}

JC2_ValueHandle struct_inst_str(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    (void)ctx; (void)argc; (void)user_data;
    Instance self(argv[0]);
    StructInstanceData* data = self.get_native_data<StructInstanceData>();
    std::string res = "Struct {";
    for (size_t i = 0; i < data->layout->fields.size(); ++i) {
        const auto& f = data->layout->fields[i];
        res += f.name + ": " + read_memory(data->memory.data() + f.offset, f.type).to_string();
        if (i < data->layout->fields.size() - 1) res += ", ";
    }
    res += "}";
    return Value(res).get_handle();
}

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
        ret_type = parseType(Value(argv[2]));
    } catch (const std::exception& e) {
        throw_error(e.what());
        return Value().get_handle();
    }
    
    std::vector<FFITypeDesc> arg_types;
    bool is_variadic = false;
    for (int i = 3; i < argc; ++i) {
        try {
            FFITypeDesc t = parseType(Value(argv[i]));
            if (t.type == FFIType::VARIADIC) {
                if (i != argc - 1) {
                    throw_error("FFI Error: '...' must be the last argument type.");
                    return Value().get_handle();
                }
                is_variadic = true;
            } else {
                arg_types.push_back(t);
            }
        } catch (const std::exception& e) {
            throw_error(e.what());
            return Value().get_handle();
        }
    }
    
    Instance func_inst(*g_funcClass);
    FunctionData* fdata = new FunctionData{func_ptr, ret_type, arg_types, is_variadic};
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
    
    size_t provided_args = argc - 1;
    if (data->is_variadic) {
        if (provided_args < data->arg_types.size()) {
            throw_error("FFI Error: Not enough arguments for variadic function. Expected at least " + std::to_string(data->arg_types.size()) + ".");
            return Value().get_handle();
        }
    } else {
        if (provided_args != data->arg_types.size()) {
            throw_error("FFI Error: Argument count mismatch. Expected " + std::to_string(data->arg_types.size()) + ", got " + std::to_string(provided_args) + ".");
            return Value().get_handle();
        }
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
    g_structLayoutClass = std::make_unique<Class>("StructLayout");
    g_structInstClass = std::make_unique<Class>("StructInstance");
    
    g_libClass->set_allocator(lib_alloc);
    g_libClass->bind_method("bind", lib_bind, 2, 255, true);
    
    g_funcClass->bind_method("__call__", func_call, 0, 255, true);

    g_structLayoutClass->set_allocator(struct_layout_alloc);
    g_structLayoutClass->bind_method("__call__", struct_layout_call, 0, 0, false);

    g_structInstClass->bind_method("__getattr__", struct_inst_getattr, 1, 1, false);
    g_structInstClass->bind_method("__setattr__", struct_inst_setattr, 2, 2, false);
    g_structInstClass->bind_method("address", struct_inst_address, 0, 0, false);
    g_structInstClass->bind_method("__str__", struct_inst_str, 0, 0, false);
    
    mod.register_value("FFILibrary", *g_libClass);
    mod.register_value("FFIFunction", *g_funcClass);
    mod.register_value("Struct", *g_structLayoutClass);
    
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
        "    Return:    \"void\" (only valid as return_type)\n"
        "    Variadic:  \"...\" (must be the last argument type)\n"
        "               * Auto-infers: int/BigInt -> i64, double -> f64, string -> string\n\n"
        "  4. Pointers & Memory Management (buffer linkage)\n"
        "  ──────────────────────\n"
        "    The \"pointer\" type represents a 64-bit memory address (void*, int*, etc.).\n"
        "    In JC2, pointers are passed and returned as standard integers.\n"
        "    To pass memory to C/C++, use the `buffer` module to allocate memory,\n"
        "    get its raw address, and read/write the results:\n"
        "      import buffer\n"
        "      buf = buffer.alloc(1024)       // Allocate 1KB memory\n"
        "      c_func(buf.address())          // Pass raw address to C\n"
        "      buf.seek(0)\n"
        "      print(buf.readI32())           // Read data modified by C\n\n"
        "  5. Structs (By Value & Pointer)\n"
        "  ──────────────────────\n"
        "    You can define C-compatible structs using `ffi.Struct`.\n"
        "    It automatically handles memory alignment and padding.\n"
        "      Point = ffi.Struct({x: \"i32\", y: \"i32\"})\n"
        "      p = Point()\n"
        "      p.x = 100\n"
        "      c_func(p)              // Pass struct by value\n"
        "      c_func_ptr(p.address()) // Pass struct by pointer\n\n"
        "  Example\n"
        "  ──────────────────────\n"
        "    import ffi\n"
        "    import buffer\n"
        "    libc = ffi.FFILibrary(\"msvcrt.dll\")\n"
        "    \n"
        "    // double pow(double base, double exp);\n"
        "    c_pow = libc.bind(\"pow\", \"f64\", \"f64\", \"f64\")\n"
        "    print(c_pow(2.0, 10.0))  // -> 1024.0\n"
        "    \n"
        "    // int printf(const char* format, ...);\n"
        "    c_printf = libc.bind(\"printf\", \"i32\", \"string\", \"...\")\n"
        "    c_printf(\"Math: %d + %f = %f\\n\", 1, 2.5, 3.5)\n"
        "    \n"
        "    // Pointers & Buffer: void* memset(void* dest, int ch, size_t count);\n"
        "    c_memset = libc.bind(\"memset\", \"pointer\", \"pointer\", \"i32\", \"u64\")\n"
        "    buf = buffer.alloc(10)\n"
        "    c_memset(buf.address(), 255, buf.length())\n"
        "    print(buf.readU8())      // -> 255"
    );
    
    mod.register_function_help("ffi.FFILibrary", "ffi.FFILibrary(path)", "Loads a native dynamic library (.dll) into memory.", "lib = ffi.FFILibrary(\"msvcrt.dll\")");
    mod.register_function_help("FFILibrary.bind", "lib.bind(func_name, ret_type, [arg_types...])", "Binds a C function from the loaded library with the specified return type and argument types.", "puts = lib.bind(\"puts\", \"i32\", \"string\")");
    
    return 0;
}

JC2_EXTENSION_INIT
