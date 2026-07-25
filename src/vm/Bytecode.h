#ifndef JC2_BYTECODE_H
#define JC2_BYTECODE_H

#include "../memory/Value.h"
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>

namespace jc {

// ============================================================================
// 寄存器虚拟机操作码 (Register VM OpCodes)
// ============================================================================
enum class OpCode : uint8_t {
    // 寄存器与常量加载
    MOVE,           // R(A) := R(B)
    LOADK,          // R(A) := Kst(Bx) [Ext]
    EXTRAARG,       // 扩展参数 Ax (24-bit)，紧跟在需要扩展的指令后
    LOAD_NIL,       // R(A) := none
    LOAD_BOOL,      // R(A) := (bool)B
    
    // 全局变量与上值
    GET_GLOBAL,     // R(A) := Globals[Bx] [Ext]
    SET_GLOBAL,     // Globals[Bx] := R(A) [Ext]
    SET_GLOBAL_REF, // Globals[Bx] := ref R(A) [Ext]
    DEFINE_CONST_GLOBAL, // Globals[Bx] := const R(A) [Ext]
    GET_UPVAL,      // R(A) := UpValue[B] [Ext]
    SET_UPVAL,      // UpValue[B] := R(A) [Ext]
    DELETE_GLOBAL,  // Delete Globals[Bx] [Ext]
    IS_UNINIT,      // R(A) := is_uninit(R(B))

    // 算术与逻辑运算 (支持 K-Bit: B 和 C 可以是寄存器或常量)
    ADD,            // R(A) := RK(B) + RK(C)
    SUB,            // R(A) := RK(B) - RK(C)
    MUL,            // R(A) := RK(B) * RK(C)
    DIV,            // R(A) := RK(B) / RK(C)
    MOD,            // R(A) := RK(B) % RK(C)
    POW,            // R(A) := RK(B) ^ RK(C)
    LDIV,           // R(A) := RK(B) \ RK(C)
    BAND,           // R(A) := RK(B) & RK(C)
    BOR,            // R(A) := RK(B) | RK(C)
    BXOR,           // R(A) := RK(B) ^^ RK(C)
    SHL,            // R(A) := RK(B) << RK(C)
    SHR,            // R(A) := RK(B) >> RK(C)
    
    // 一元运算
    UNM,            // R(A) := -R(B)
    NOT,            // R(A) := !R(B)
    BNOT,           // R(A) := ~R(B)
    TO_BOOL,        // R(A) := !!R(B)

    // 比较与跳转
    EQ,             // R(A) := RK(B) == RK(C)
    NEQ,            // R(A) := RK(B) != RK(C)
    LT,             // R(A) := RK(B) <  RK(C)
    LE,             // R(A) := RK(B) <= RK(C)
    GT,             // R(A) := RK(B) >  RK(C)
    GE,             // R(A) := RK(B) >= RK(C)
    IS,             // R(A) := RK(B) is RK(C)
    
    JMP,            // PC += sAx (24-bit 超大范围跳转)
    JMP_TRUE,       // if (R(A) truthy) PC += sBx
    JMP_FALSE,      // if (!R(A) truthy) PC += sBx

    // 函数调用与返回
    CALL,           // R(A) := Call(callee = R(B), args = R(B+1)...R(B+C)) [Ext A, B, C]
    TAIL_CALL,      // 尾调用 [Ext A, B, C]
    RETURN,         // return R(A)
    CLOSURE,        // R(A) := Closure(fnIdx = Bx) [Ext]

    // 引用参数
    GET_REF_PARAM,  // R(A) := RefParam[Bx] [Ext]
    SET_REF_PARAM,  // RefParam[Bx] := R(A) [Ext]
    PASS_REFS,      // pass_refs(sigIdx = Bx) [Ext]

    // 面向对象与属性
    GET_PROP,       // R(A) := R(B)[icIdx = C] [Ext A, B, C]
    TRY_GET_PROP,   // R(A), R(A+1) := try_get(R(B), icIdx = C) [Ext A, B, C]
    SET_PROP,       // R(A)[icIdx = B] := R(C) [Ext A, B, C]
    INVOKE,         // R(A) := Invoke(obj = R(A), args = R(A+1)...R(A+B), icIdx = C) [Ext A, B, C]
    TAIL_INVOKE,    // [Ext A, B, C]
    INVOKE_FALLBACK,// R(A) := InvokeFallback(obj = R(A), args = R(A+1)...R(A+B), fallback = R(A+B+1), icIdx = C) [Ext A, B, C]
    TAIL_INVOKE_FALLBACK, // [Ext A, B, C]
    GET_SUPER,      // R(A) := super(self = R(B), nameIdx = C) [Ext A, B, C]
    SUPER_INVOKE,   // R(A) := SuperInvoke(self = R(A), args = R(A+1)...R(A+B), nameIdx = C) [Ext A, B, C]
    TAIL_SUPER_INVOKE, // [Ext A, B, C]
    GET_SELF,       // R(A) := self
    GET_CURRENT_CLOSURE, // R(A) := current_closure
    CLASS,          // R(A) := Class(nameIdx = Bx) [Ext]
    METHOD,         // R(A).Method(nameIdx = B) := R(C) [Ext]
    INHERIT,        // R(A) inherits R(B)

    // 容器构建与操作
    BUILD_LIST,     // R(A) := List(R(B) ... R(B+C-1)) [Ext B, C]
    BUILD_DICT,     // R(A) := Dict(R(B) ... R(B+C-1)) [Ext B, C]
    DICT_REST,      // R(A) := dict_rest(R(B), exclude_keys = R(C)) [Ext C]
    BUILD_SET,      // R(A) := Set(R(B) ... R(B+C-1)) [Ext B, C]
    BUILD_MATRIX,   // R(A) := Matrix(elements = R(B)..., shapeIdx = C) [Ext A, B, C]
    BUILD_NAMESPACE,// R(A) := Namespace(nameIdx = B, count = C). Triplets (key, slot, isConst) start at R(A+1) [Ext A, B, C]
    LIST_INIT,      // R(A) := []
    LIST_APPEND,    // R(A).append(R(B))
    LIST_COMP_END,  // R(A) := comp_end(R(A))
    SET_INIT,       // R(A) := @{}
    SET_APPEND,     // R(A).add(R(B))
    DICT_INIT,      // R(A) := {}
    DICT_APPEND,    // R(A)[R(B)] = R(C)
    INDEX_GET,      // R(A) := R(B)[R(B+1)...R(B+C)] [Ext A, B, C]
    INDEX_SET,      // R(A)[R(A+1)...R(A+C)] := R(A+C+1) [Ext A, C]
    SLICE_GET,      // R(A) := slice_get(R(B), dims = C, args = R(B+1)...) [Ext A, B, C]
    SLICE_SET,      // slice_set(R(A), dims = C, args = R(A+1)..., val = R(A + 3*C + 1)) [Ext A, C]

    // 字符串操作
    STRINGIFY,      // R(A) := str(R(B))
    CONCAT_STRINGS, // R(A) := concat(R(B) ... R(B+C-1)) [Ext B, C]
    FORMAT_STRING,  // R(A) := format(R(B), Kst(C))

    // 异常处理
    TRY_BEGIN,      // Push Try Handler (catch PC = PC + sBx, errReg = A)
    TRY_END,        // Pop Try Handler
    THROW,          // Throw R(A)

    // 迭代器与包含
    ITER_INIT,      // R(A) := Iter(R(B), destruct = C)
    ITER_NEXT,      // R(A) := Next(R(B)). Returns uninit if exhausted.
    IN,             // R(A) := R(B) in R(C)

    // 模块导入
    IMPORT,         // R(A) := import(R(B))

    DEFER,          // defer(closure = R(A))
    RUN_DEFERS,     // run_defers(count = A)

    // 类型与断言
    ASSERT_PARAM_TYPE,  // assert_param(R(A), typeIC = B, nameKst = C) [Ext B, C]
    ASSERT_RETURN_TYPE, // assert_return(R(A), typeIC = B) [Ext]
    MATCH_TYPE,         // R(A) := match_type(R(B), typeIC = C) [Ext]
    MATCH_SHAPE,        // R(A) := match_shape(R(B), shapeIdx = C) [Ext]
};

inline std::string opCodeToString(OpCode op) {
    switch (op) {
        case OpCode::MOVE: return "MOVE";
        case OpCode::LOADK: return "LOADK";
        case OpCode::EXTRAARG: return "EXTRAARG";
        case OpCode::LOAD_NIL: return "LOAD_NIL";
        case OpCode::LOAD_BOOL: return "LOAD_BOOL";
        case OpCode::GET_GLOBAL: return "GET_GLOBAL";
        case OpCode::SET_GLOBAL: return "SET_GLOBAL";
        case OpCode::SET_GLOBAL_REF: return "SET_GLOBAL_REF";
        case OpCode::DEFINE_CONST_GLOBAL: return "DEFINE_CONST_GLOBAL";
        case OpCode::GET_UPVAL: return "GET_UPVAL";
        case OpCode::SET_UPVAL: return "SET_UPVAL";
        case OpCode::DELETE_GLOBAL: return "DELETE_GLOBAL";
        case OpCode::IS_UNINIT: return "IS_UNINIT";
        case OpCode::ADD: return "ADD";
        case OpCode::SUB: return "SUB";
        case OpCode::MUL: return "MUL";
        case OpCode::DIV: return "DIV";
        case OpCode::MOD: return "MOD";
        case OpCode::POW: return "POW";
        case OpCode::LDIV: return "LDIV";
        case OpCode::BAND: return "BAND";
        case OpCode::BOR: return "BOR";
        case OpCode::BXOR: return "BXOR";
        case OpCode::SHL: return "SHL";
        case OpCode::SHR: return "SHR";
        case OpCode::UNM: return "UNM";
        case OpCode::NOT: return "NOT";
        case OpCode::BNOT: return "BNOT";
        case OpCode::TO_BOOL: return "TO_BOOL";
        case OpCode::EQ: return "EQ";
        case OpCode::NEQ: return "NEQ";
        case OpCode::LT: return "LT";
        case OpCode::LE: return "LE";
        case OpCode::GT: return "GT";
        case OpCode::GE: return "GE";
        case OpCode::IS: return "IS";
        case OpCode::JMP: return "JMP";
        case OpCode::JMP_TRUE: return "JMP_TRUE";
        case OpCode::JMP_FALSE: return "JMP_FALSE";
        case OpCode::CALL: return "CALL";
        case OpCode::TAIL_CALL: return "TAIL_CALL";
        case OpCode::RETURN: return "RETURN";
        case OpCode::CLOSURE: return "CLOSURE";
        case OpCode::GET_REF_PARAM: return "GET_REF_PARAM";
        case OpCode::SET_REF_PARAM: return "SET_REF_PARAM";
        case OpCode::PASS_REFS: return "PASS_REFS";
        case OpCode::GET_PROP: return "GET_PROP";
        case OpCode::TRY_GET_PROP: return "TRY_GET_PROP";
        case OpCode::SET_PROP: return "SET_PROP";
        case OpCode::INVOKE: return "INVOKE";
        case OpCode::TAIL_INVOKE: return "TAIL_INVOKE";
        case OpCode::INVOKE_FALLBACK: return "INVOKE_FALLBACK";
        case OpCode::TAIL_INVOKE_FALLBACK: return "TAIL_INVOKE_FALLBACK";
        case OpCode::GET_SUPER: return "GET_SUPER";
        case OpCode::SUPER_INVOKE: return "SUPER_INVOKE";
        case OpCode::TAIL_SUPER_INVOKE: return "TAIL_SUPER_INVOKE";
        case OpCode::GET_SELF: return "GET_SELF";
        case OpCode::GET_CURRENT_CLOSURE: return "GET_CURRENT_CLOSURE";
        case OpCode::CLASS: return "CLASS";
        case OpCode::METHOD: return "METHOD";
        case OpCode::INHERIT: return "INHERIT";
        case OpCode::BUILD_LIST: return "BUILD_LIST";
        case OpCode::BUILD_DICT: return "BUILD_DICT";
        case OpCode::DICT_REST: return "DICT_REST";
        case OpCode::BUILD_SET: return "BUILD_SET";
        case OpCode::BUILD_MATRIX: return "BUILD_MATRIX";
        case OpCode::BUILD_NAMESPACE: return "BUILD_NAMESPACE";
        case OpCode::LIST_INIT: return "LIST_INIT";
        case OpCode::LIST_APPEND: return "LIST_APPEND";
        case OpCode::LIST_COMP_END: return "LIST_COMP_END";
        case OpCode::SET_INIT: return "SET_INIT";
        case OpCode::SET_APPEND: return "SET_APPEND";
        case OpCode::DICT_INIT: return "DICT_INIT";
        case OpCode::DICT_APPEND: return "DICT_APPEND";
        case OpCode::INDEX_GET: return "INDEX_GET";
        case OpCode::INDEX_SET: return "INDEX_SET";
        case OpCode::SLICE_GET: return "SLICE_GET";
        case OpCode::SLICE_SET: return "SLICE_SET";
        case OpCode::STRINGIFY: return "STRINGIFY";
        case OpCode::CONCAT_STRINGS: return "CONCAT_STRINGS";
        case OpCode::FORMAT_STRING: return "FORMAT_STRING";
        case OpCode::TRY_BEGIN: return "TRY_BEGIN";
        case OpCode::TRY_END: return "TRY_END";
        case OpCode::THROW: return "THROW";
        case OpCode::ITER_INIT: return "ITER_INIT";
        case OpCode::ITER_NEXT: return "ITER_NEXT";
        case OpCode::IN: return "IN";
        case OpCode::IMPORT: return "IMPORT";
        case OpCode::DEFER: return "DEFER";
        case OpCode::RUN_DEFERS: return "RUN_DEFERS";
        case OpCode::ASSERT_PARAM_TYPE: return "ASSERT_PARAM_TYPE";
        case OpCode::ASSERT_RETURN_TYPE: return "ASSERT_RETURN_TYPE";
        case OpCode::MATCH_TYPE: return "MATCH_TYPE";
        case OpCode::MATCH_SHAPE: return "MATCH_SHAPE";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// 32-bit 指令编码与解码宏
// 格式 A (iABC) : [OpCode:8] [A:8] [B:8] [C:8]
// 格式 B (iABx) : [OpCode:8] [A:8] [Bx:16]
// 格式 C (iAsBx): [OpCode:8] [A:8] [sBx:16]
// 格式 D (iAx)  : [OpCode:8] [Ax:24]
// 格式 E (isAx) : [OpCode:8] [sAx:24]
// ============================================================================
using Instruction = uint32_t;

// 提取字段
inline OpCode GET_OPCODE(Instruction i) { return static_cast<OpCode>(i & 0xFF); }
inline int GET_A(Instruction i) { return (i >> 8) & 0xFF; }
inline int GET_B(Instruction i) { return (i >> 16) & 0xFF; }
inline int GET_C(Instruction i) { return (i >> 24) & 0xFF; }
inline int GET_Bx(Instruction i) { return i >> 16; }
inline int GET_sBx(Instruction i) { return static_cast<int>(i >> 16) - 0x7FFF; } // 偏移量 0x7FFF
inline int GET_Ax(Instruction i) { return i >> 8; }
inline int GET_sAx(Instruction i) { return static_cast<int>(i >> 8) - 0x7FFFFF; } // 偏移量 0x7FFFFF

// 构造指令
inline Instruction CREATE_ABC(OpCode op, int a, int b, int c) {
    return (static_cast<uint32_t>(op) & 0xFF) |
           ((static_cast<uint32_t>(a) & 0xFF) << 8) |
           ((static_cast<uint32_t>(b) & 0xFF) << 16) |
           ((static_cast<uint32_t>(c) & 0xFF) << 24);
}

inline Instruction CREATE_ABx(OpCode op, int a, int bx) {
    return (static_cast<uint32_t>(op) & 0xFF) |
           ((static_cast<uint32_t>(a) & 0xFF) << 8) |
           ((static_cast<uint32_t>(bx) & 0xFFFF) << 16);
}

inline Instruction CREATE_AsBx(OpCode op, int a, int sbx) {
    return CREATE_ABx(op, a, sbx + 0x7FFF);
}

inline Instruction CREATE_Ax(OpCode op, int ax) {
    return (static_cast<uint32_t>(op) & 0xFF) |
           ((static_cast<uint32_t>(ax) & 0xFFFFFF) << 8);
}

inline Instruction CREATE_sAx(OpCode op, int sax) {
    return CREATE_Ax(op, sax + 0x7FFFFF);
}

// ============================================================================
// K-Bit 常量复用机制与 EXTRAARG 转义标志
// B 和 C 操作数的最高位 (第 8 位，即 0x80) 为 1 时，表示常量池索引 (0~127)
// ============================================================================
constexpr int BITRK = 0x80;
inline bool ISK(int x) { return (x & BITRK) != 0; }
inline int INDEXK(int x) { return x & ~BITRK; }
inline int RKASK(int x) { return x | BITRK; }

// 转义标志定义
constexpr int ESCAPE_NORMAL_8 = 0xFF;    // 普通 8-bit 操作数 (如 A) 的转义标志
constexpr int ESCAPE_NORMAL_16 = 0xFFFF; // 普通 16-bit 操作数 (如 Bx) 的转义标志
constexpr int ESCAPE_KBIT_REG = 0x7F;    // K-Bit 寄存器的转义标志 (127)
constexpr int ESCAPE_KBIT_CONST = 0xFF;  // K-Bit 常量的转义标志 (255)

// ============================================================================
// 寄存器机 Chunk (存储字节码与元数据)
// ============================================================================
enum class BuiltinType : int8_t {
    UNKNOWN = -1,
    ANY, INT, FLOAT, REAL, NUMBER, WHOLE, EXACT, STRING, BOOL, BINARY, NONE_TYPE,
    LIST, DICT, SET, FRACTION, COMPLEX, BASENUM, SYMBOLIC,
    REALMAT, COMPLEXMAT, STRINGMAT, MATRIX, FUNC, CLASS, INSTANCE, NAMESPACE,
    ITERABLE, CALLABLE, INDEXABLE, HASHABLE, NUMERIC,
    CUSTOM_CLASS
};

struct InlineCache {
    uint32_t nameIdx = 0;
    int cachedGlobalSlot = -1;
    uint64_t cachedClassId = 0;
    ObjClosure* cachedMethod = nullptr;
    ObjClass* cachedClass = nullptr;
    int cachedFieldIndex = -1;
    BuiltinType cachedBuiltinType = BuiltinType::UNKNOWN;
    std::any cachedNativeFn;
};

struct ShapePattern {
    uint32_t minRows;
    uint32_t maxRows;
    uint32_t minCols;
    uint32_t maxCols;
    uint8_t exactMask;
};

struct MatrixShape {
    uint16_t rows;
    std::vector<uint16_t> rowCols;
};

struct ArgSource {
    uint8_t argIndex;
    uint8_t sourceType;
    uint32_t sourceRef;
};

struct CallSignature {
    std::vector<ArgSource> refs;
};

class Chunk {
public:
    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::vector<int> lines;
    std::vector<InlineCache> inlineCaches;
    std::vector<ShapePattern> shapePatterns;
    std::vector<MatrixShape> matrixShapes;
    std::vector<CallSignature> callSignatures;

    void write(Instruction inst, int line) {
        code.push_back(inst);
        lines.push_back(line);
    }

    uint32_t addConstant(const Value& val) {
        constants.push_back(val);
        return static_cast<uint32_t>(constants.size() - 1);
    }

    uint32_t addInlineCache(uint32_t nameIdx) {
        inlineCaches.push_back(InlineCache{nameIdx, -1});
        return static_cast<uint32_t>(inlineCaches.size() - 1);
    }

    uint32_t addShapePattern(uint32_t minR, uint32_t maxR, uint32_t minC, uint32_t maxC, uint8_t mask) {
        shapePatterns.push_back({minR, maxR, minC, maxC, mask});
        return static_cast<uint32_t>(shapePatterns.size() - 1);
    }

    uint32_t addMatrixShape(uint16_t rows, const std::vector<uint16_t>& rowCols) {
        matrixShapes.push_back({rows, rowCols});
        return static_cast<uint32_t>(matrixShapes.size() - 1);
    }

    uint32_t addCallSignature(const std::vector<ArgSource>& refs) {
        callSignatures.push_back({refs});
        return static_cast<uint32_t>(callSignatures.size() - 1);
    }

    void disassemble(const std::string& name) const {
        std::cout << "=== " << name << " ===" << std::endl;
        for (int offset = 0; offset < static_cast<int>(code.size()); ++offset) {
            disassembleInstruction(offset);
        }
        std::cout << "==================" << std::endl;
    }

    void disassembleInstruction(int offset) const {
        Instruction inst = code[offset];
        std::cout << std::right << std::setw(4) << std::setfill('0') << offset << "  " << std::setfill(' ');

        if (offset > 0 && lines[offset] == lines[offset - 1])
            std::cout << "   | ";
        else
            std::cout << std::right << std::setw(4) << lines[offset] << " ";

        OpCode op = GET_OPCODE(inst);
        std::string opName = opCodeToString(op);
        std::cout << std::left << std::setw(20) << opName;

        int a = GET_A(inst);
        int b = GET_B(inst);
        int c = GET_C(inst);
        int bx = GET_Bx(inst);
        int sbx = GET_sBx(inst);
        int ax = GET_Ax(inst);
        int sax = GET_sAx(inst);

        auto formatConstant = [&](int idx) -> std::string {
            if (idx < 0 || idx >= static_cast<int>(constants.size())) return "?";
            const Value& v = constants[idx];
            if (v.isString()) return "\"" + v.asString() + "\"";
            std::ostringstream oss; oss << v; return oss.str();
        };

        auto formatRK = [&](int rk) {
            if (ISK(rk)) {
                int idx = INDEXK(rk);
                if (idx != ESCAPE_KBIT_CONST) {
                    return "K(" + std::to_string(idx) + ":" + formatConstant(idx) + ")";
                }
                return "K(" + std::to_string(idx) + ")";
            }
            return "R(" + std::to_string(rk) + ")";
        };

        switch (op) {
            case OpCode::ADD: case OpCode::SUB: case OpCode::MUL: case OpCode::DIV:
            case OpCode::MOD: case OpCode::POW: case OpCode::LDIV: case OpCode::BAND:
            case OpCode::BOR: case OpCode::BXOR: case OpCode::SHL: case OpCode::SHR:
            case OpCode::EQ: case OpCode::NEQ: case OpCode::LT: case OpCode::LE:
            case OpCode::GT: case OpCode::GE: case OpCode::IS:
                std::cout << "R(" << a << ") " << formatRK(b) << " " << formatRK(c);
                break;
            
            case OpCode::BUILD_LIST: case OpCode::BUILD_DICT: case OpCode::BUILD_SET:
            case OpCode::CONCAT_STRINGS: case OpCode::DICT_REST: case OpCode::BUILD_MATRIX:
            case OpCode::INDEX_GET: case OpCode::INDEX_SET: 
            case OpCode::SLICE_GET: case OpCode::SLICE_SET: 
            case OpCode::ITER_INIT: case OpCode::IN: case OpCode::MATCH_SHAPE:
            case OpCode::DICT_APPEND:
                std::cout << "R(" << a << ") " << b << " " << c;
                break;

            case OpCode::FORMAT_STRING:
                std::cout << "R(" << a << ") " << b << " " << c;
                if (c != ESCAPE_NORMAL_8 && c < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << formatConstant(c);
                }
                break;

            case OpCode::BUILD_NAMESPACE:
                std::cout << "R(" << a << ") " << b << " " << c;
                if (b != ESCAPE_NORMAL_8 && b < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << constants[b].asString();
                }
                break;

            case OpCode::ASSERT_PARAM_TYPE:
                std::cout << "R(" << a << ") " << b << " " << c;
                if (c != ESCAPE_NORMAL_8 && c < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << constants[c].asString();
                }
                break;

            case OpCode::MATCH_TYPE:
            case OpCode::GET_PROP: case OpCode::TRY_GET_PROP: case OpCode::SET_PROP: 
            case OpCode::INVOKE: case OpCode::TAIL_INVOKE: 
            case OpCode::INVOKE_FALLBACK: case OpCode::TAIL_INVOKE_FALLBACK:
                std::cout << "R(" << a << ") " << b << " " << c;
                if (c != ESCAPE_NORMAL_8 && c < static_cast<int>(inlineCaches.size())) {
                    int nameIdx = inlineCaches[c].nameIdx;
                    if (nameIdx < static_cast<int>(constants.size())) {
                        std::cout << "  ; " << constants[nameIdx].asString();
                    }
                }
                break;

            case OpCode::SUPER_INVOKE: case OpCode::TAIL_SUPER_INVOKE:
                std::cout << "R(" << a << ") " << b << " " << c;
                if (c != ESCAPE_NORMAL_8 && c < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << constants[c].asString();
                }
                break;

            case OpCode::METHOD:
                std::cout << "R(" << a << ") " << b << " " << c;
                if (b != ESCAPE_NORMAL_8 && b < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << constants[b].asString();
                }
                break;

            case OpCode::MOVE: case OpCode::LOAD_NIL:
            case OpCode::GET_UPVAL: case OpCode::SET_UPVAL: case OpCode::IS_UNINIT:
            case OpCode::UNM: case OpCode::NOT: case OpCode::BNOT: case OpCode::TO_BOOL:
            case OpCode::INHERIT: case OpCode::LIST_APPEND: case OpCode::SET_APPEND:
            case OpCode::STRINGIFY: case OpCode::ITER_NEXT: case OpCode::IMPORT:
                std::cout << "R(" << a << ") " << b;
                break;

            case OpCode::CALL: case OpCode::TAIL_CALL:
                std::cout << "R(" << a << ") " << b << " " << c;
                break;

            case OpCode::LOAD_BOOL:
                std::cout << "R(" << a << ") " << b;
                std::cout << "  ; " << (b ? "true" : "false");
                break;

            case OpCode::ASSERT_RETURN_TYPE:
                std::cout << "R(" << a << ") " << b;
                if (b != ESCAPE_NORMAL_8 && b < static_cast<int>(inlineCaches.size())) {
                    int nameIdx = inlineCaches[b].nameIdx;
                    if (nameIdx < static_cast<int>(constants.size())) {
                        std::cout << "  ; " << constants[nameIdx].asString();
                    }
                }
                break;

            case OpCode::GET_SUPER:
                std::cout << "R(" << a << ") " << b << " " << c;
                if (c != ESCAPE_NORMAL_8 && c < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << constants[c].asString();
                }
                break;

            case OpCode::RETURN: case OpCode::GET_SELF: case OpCode::GET_CURRENT_CLOSURE:
            case OpCode::LIST_INIT: case OpCode::LIST_COMP_END: case OpCode::SET_INIT: case OpCode::DICT_INIT:
            case OpCode::TRY_END: case OpCode::THROW: case OpCode::DEFER:
                std::cout << "R(" << a << ")";
                break;

            case OpCode::RUN_DEFERS:
                std::cout << a;
                break;

            case OpCode::LOADK:
                std::cout << "R(" << a << ") " << bx;
                if (bx != ESCAPE_NORMAL_16 && bx < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << formatConstant(bx);
                }
                break;

            case OpCode::GET_GLOBAL: case OpCode::SET_GLOBAL:
            case OpCode::SET_GLOBAL_REF: case OpCode::DEFINE_CONST_GLOBAL:
                std::cout << "R(" << a << ") " << bx;
                if (bx != ESCAPE_NORMAL_16 && bx < static_cast<int>(inlineCaches.size())) {
                    int nameIdx = inlineCaches[bx].nameIdx;
                    if (nameIdx < static_cast<int>(constants.size())) {
                        std::cout << "  ; " << constants[nameIdx].asString();
                    }
                }
                break;

            case OpCode::CLASS:
                std::cout << "R(" << a << ") " << bx;
                if (bx != ESCAPE_NORMAL_16 && bx < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << constants[bx].asString();
                }
                break;

            case OpCode::CLOSURE:
                std::cout << "R(" << a << ") " << bx;
                if (bx != ESCAPE_NORMAL_16 && bx < static_cast<int>(constants.size())) {
                    std::cout << "  ; fnIdx=" << constants[bx].asDouble();
                }
                break;

            case OpCode::GET_REF_PARAM: case OpCode::SET_REF_PARAM:
                std::cout << "R(" << a << ") " << bx;
                break;

            case OpCode::DELETE_GLOBAL:
                std::cout << bx;
                if (bx != ESCAPE_NORMAL_16 && bx < static_cast<int>(constants.size())) {
                    std::cout << "  ; " << constants[bx].asString();
                }
                break;

            case OpCode::PASS_REFS:
                std::cout << bx;
                break;

            case OpCode::JMP_TRUE: case OpCode::JMP_FALSE: case OpCode::TRY_BEGIN:
                std::cout << "R(" << a << ") " << sbx;
                break;

            case OpCode::EXTRAARG:
                std::cout << ax;
                break;

            case OpCode::JMP:
                std::cout << sax;
                break;

            default:
                std::cout << "?";
                break;
        }
        std::cout << std::endl;
    }
};

struct CompiledFunction {
    std::string name;
    std::string sourceFile;
    int arity = 0;
    int maxArity = 0;
    int localCount = 0;
    bool hasRestParam = false;
    Chunk chunk;

    struct UpvalueInfo {
        std::string name;
        bool isLocal;
        int index;
        bool isRef = false;
        bool isGlobal = false;
        bool isExplicitState = false;
        bool isRefParam = false;
        bool isCapturedState = false;
    };
    std::vector<UpvalueInfo> upvalues;
    std::vector<bool> paramIsRef;
    std::vector<bool> paramIsConst;
    int refCount = 0;
};

} // namespace jc

#endif // JC2_BYTECODE_H
