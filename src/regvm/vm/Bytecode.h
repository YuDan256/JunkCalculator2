#ifndef JC2_REGVM_BYTECODE_H
#define JC2_REGVM_BYTECODE_H

#include "../../memory/Value.h"
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>

namespace jc {
namespace regvm {

// ============================================================================
// 寄存器虚拟机操作码 (Register VM OpCodes)
// ============================================================================
enum class OpCode : uint8_t {
    // 寄存器与常量加载
    MOVE,           // R(A) := R(B)
    LOADK,          // R(A) := Kst(Bx)
    LOADKX,         // R(A) := Kst(EXTRAARG)
    EXTRAARG,       // 扩展参数 Ax (24-bit)
    LOAD_NIL,       // R(A) := none
    LOAD_BOOL,      // R(A) := (bool)B
    
    // 溢出槽访问
    LOAD_EXT,       // R(A) := Spill(Bx)
    STORE_EXT,      // Spill(Bx) := R(A)

    // 全局变量与上值
    GET_GLOBAL,     // R(A) := Globals[Bx]
    SET_GLOBAL,     // Globals[Bx] := R(A)
    SET_GLOBAL_REF, // Globals[Bx] := ref R(A)
    DEFINE_CONST_GLOBAL, // Globals[Bx] := const R(A)
    GET_UPVAL,      // R(A) := UpValue[B]
    SET_UPVAL,      // UpValue[B] := R(A)
    DELETE_GLOBAL,  // Delete Globals[Bx]
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
    
    JMP,            // PC += sBx
    JMP_TRUE,       // if (R(A) truthy) PC += sBx
    JMP_FALSE,      // if (!R(A) truthy) PC += sBx

    // 函数调用与返回
    CALL,           // R(A) := Call(callee = R(A), args = R(A+1)...R(A+B))
    CALL_EXT,       // 极端调用 (参数在溢出槽)
    TAIL_CALL,      // 尾调用
    RETURN,         // return R(A)
    CLOSURE,        // R(A) := Closure(fnIdx = Bx)

    // 引用参数
    GET_REF_PARAM,  // R(A) := RefParam[Bx]
    SET_REF_PARAM,  // RefParam[Bx] := R(A)
    PASS_REFS,      // pass_refs(sigIdx = Bx)

    // 面向对象与属性
    GET_PROP,       // R(A) := R(B)[RK(C)]
    TRY_GET_PROP,   // R(A), R(A+1) := try_get(R(B), RK(C))
    SET_PROP,       // R(A)[RK(B)] := RK(C)
    INVOKE,         // R(A) := Invoke(obj = R(A), args = R(A+1)...R(A+B), icIdx = C)
    TAIL_INVOKE,
    INVOKE_FALLBACK,// R(A) := InvokeFallback(obj = R(A), args = R(A+1)...R(A+B), fbIdx = C)
    TAIL_INVOKE_FALLBACK,
    GET_SUPER,      // R(A) := super(Kst(B))
    SUPER_INVOKE,   // R(A) := SuperInvoke(self = R(A), args = R(A+1)...R(A+B), nameIdx = C)
    TAIL_SUPER_INVOKE,
    GET_SELF,       // R(A) := self
    CLASS,          // R(A) := Class(nameIdx = Bx)
    METHOD,         // R(A).Method(nameIdx = B) := R(C)
    INHERIT,        // R(A) inherits R(B)

    // 容器构建与操作
    BUILD_LIST,     // R(A) := List(R(B) ... R(B+C-1))
    BUILD_DICT,     // R(A) := Dict(R(B) ... R(B+C-1))
    DICT_REST,      // R(A) := dict_rest(R(B), exclude_keys = R(C))
    BUILD_SET,      // R(A) := Set(R(B) ... R(B+C-1))
    BUILD_MATRIX,   // R(A) := Matrix(shapeIdx = Bx, elements = R(A+1)...)
    BUILD_NAMESPACE,// R(A) := Namespace(nameIdx = Bx)
    LIST_INIT,      // R(A) := []
    LIST_APPEND,    // R(A).append(R(B))
    LIST_COMP_END,  // R(A) := comp_end(R(A))
    INDEX_GET,      // R(A) := R(B)[R(A+1)...R(A+C)]
    INDEX_SET,      // R(A)[R(A+1)...R(A+C)] := R(B)
    SLICE_GET,      // R(A) := slice_get(R(B), dims = C, args = R(A+1)...)
    SLICE_SET,      // slice_set(R(A), R(B), dims = C, args = R(A+1)...)

    // 字符串操作
    STRINGIFY,      // R(A) := str(R(B))
    CONCAT_STRINGS, // R(A) := concat(R(B) ... R(B+C-1))
    FORMAT_STRING,  // R(A) := format(R(B), Kst(C))

    // 异常处理
    TRY_BEGIN,      // Push Try Handler (catch PC = PC + sBx, errReg = A)
    TRY_END,        // Pop Try Handler
    THROW,          // Throw R(A)

    // 迭代器与包含
    ITER_INIT,      // R(A) := Iter(R(B))
    ITER_NEXT,      // R(A) := Next(R(B)). If exhausted PC++, else PC+=2
    IN,             // R(A) := R(B) in R(C)

    // 模块导入
    IMPORT,         // R(A) := import(R(B))

    // 类型与断言
    ASSERT_PARAM_TYPE,  // assert_param(R(A), typeIC = B, nameKst = C)
    ASSERT_RETURN_TYPE, // assert_return(R(A), typeIC = B)
    MATCH_TYPE,         // R(A) := match_type(R(B), typeIC = C)
    MATCH_SHAPE,        // R(A) := match_shape(R(B), shapeIdx = C)
};

// ============================================================================
// 32-bit 指令编码与解码宏
// 格式 A (iABC) : [OpCode:8] [A:8] [B:8] [C:8]
// 格式 B (iABx) : [OpCode:8] [A:8] [Bx:16]
// 格式 C (iAsBx): [OpCode:8] [A:8] [sBx:16]
// 格式 D (iAx)  : [OpCode:8] [Ax:24]
// ============================================================================
using Instruction = uint32_t;

// 提取字段
inline OpCode GET_OPCODE(Instruction i) { return static_cast<OpCode>(i & 0xFF); }
inline int GET_A(Instruction i) { return (i >> 8) & 0xFF; }
inline int GET_B(Instruction i) { return (i >> 16) & 0xFF; }
inline int GET_C(Instruction i) { return (i >> 24) & 0xFF; }
inline int GET_Bx(Instruction i) { return (i >> 16) & 0xFFFF; }
inline int GET_sBx(Instruction i) { return static_cast<int>(GET_Bx(i)) - 0x7FFF; } // 偏移量 0x7FFF
inline int GET_Ax(Instruction i) { return (i >> 8) & 0xFFFFFF; }

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

// ============================================================================
// K-Bit 常量复用机制
// B 和 C 操作数的最高位 (第 8 位，即 0x80) 为 1 时，表示常量池索引 (0~127)
// ============================================================================
constexpr int BITRK = 0x80;
inline bool ISK(int x) { return (x & BITRK) != 0; }
inline int INDEXK(int x) { return x & ~BITRK; }
inline int RKASK(int x) { return x | BITRK; }

// ============================================================================
// 寄存器机 Chunk (存储字节码与元数据)
// ============================================================================
struct InlineCache {
    uint32_t nameIdx = 0;
    int cachedGlobalSlot = -1;
    // ... 其他缓存字段
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

struct FallbackInfo {
    uint32_t icIdx;
    uint8_t fbType;
    uint32_t fbIdx;
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
    std::vector<FallbackInfo> fallbackInfos;

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

    uint32_t addFallbackInfo(uint32_t icIdx, uint8_t fbType, uint32_t fbIdx) {
        fallbackInfos.push_back({icIdx, fbType, fbIdx});
        return static_cast<uint32_t>(fallbackInfos.size() - 1);
    }
};

} // namespace regvm
} // namespace jc

#endif // JC2_REGVM_BYTECODE_H
