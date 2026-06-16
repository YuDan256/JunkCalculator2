#ifndef JC2_BYTECODE_H
#define JC2_BYTECODE_H

#include "../memory/Value.h"
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace jc {

    // ═══════════════════════════════════════════
    // 操作码 (Opcode)
    // ═══════════════════════════════════════════
    enum class OpCode : uint8_t {
        // 栈操作
        OP_CONSTANT,        // 压入常量: [idx:16bit]
        OP_NONE,            // 压入 none
        OP_TRUE,            // 压入 1.0
        OP_FALSE,           // 压入 0.0
        OP_POP,             // 弹出栈顶

        // 算术
        OP_ADD,
        OP_SUBTRACT,
        OP_MULTIPLY,
        OP_DIVIDE,
        OP_MODULO,
        OP_POWER,
        OP_LEFT_DIVIDE,     // ★ \ 左除
        OP_NEGATE,          // 一元取负
        OP_NOT,             // 逻辑取反
        OP_BIT_NOT,         // ★ ~ 按位取反
        OP_BIT_AND,         // ★ & 
        OP_BIT_OR,          // ★ |
        OP_BIT_XOR,         // ★ ^^
        OP_BIT_SHIFT_LEFT,  // ★ <<
        OP_BIT_SHIFT_RIGHT, // ★ >>

        // 比较
        OP_EQUAL,
        OP_NOT_EQUAL,
        OP_LESS,
        OP_LESS_EQUAL,
        OP_GREATER,
        OP_GREATER_EQUAL,

        // 全局变量
        OP_GET_GLOBAL,      // [name_idx:16bit]
        OP_SET_GLOBAL,      // [name_idx:16bit]
        OP_SET_GLOBAL_REF,  // [name_idx:16bit] ★ 新增
        OP_DEFINE_CONST_GLOBAL, // [name_idx:16bit]
        OP_IS_UNINIT,       // 检查栈顶是否为 UNINIT，返回 bool

        // 局部变量
        OP_GET_LOCAL,       // [slot:16bit]
        OP_SET_LOCAL,       // [slot:16bit]

        // 跳转
        OP_JUMP,            // 无条件跳转 [offset:16bit]
        OP_JUMP_IF_FALSE,   // 条件跳转 [offset:16bit]
        OP_JUMP_IF_TRUE,    // 条件跳转 [offset:16bit]
        OP_LOOP,            // 回跳 [offset:16bit]

        // 函数
        OP_CALL,            // [arg_count:8bit]
        OP_RETURN,

        // 输出（临时，调试用）
        OP_PRINT,

        // 索引
        OP_INDEX_GET,       // [dim_count:8bit]
        OP_INDEX_SET,       // [dim_count:8bit]

        // 字符串/矩阵
        OP_BUILD_LIST,      // [count:16bit]
        OP_BUILD_MATRIX,    // [shape_idx:16bit]

        // 函数（扩展）
        OP_CLOSURE,         // 创建闭包: [func_idx:16bit]

        // 复合赋值辅助
        OP_DUP,             // 复制栈顶

        // for-in
        OP_ITER_INIT,       // 将可迭代对象转换为内部迭代器
        OP_ITER_NEXT,       // 取下一个元素，如果结束则跳转 [offset:16bit]
        OP_IN,

        // 字符串
        OP_STRINGIFY,       // 将栈顶转为字符串
        OP_CONCAT_STRINGS,  // [count:16bit] 拼接 N 个字符串

        // 异常处理
        OP_TRY_BEGIN,       // [catch_offset:16bit] 设置异常处理器
        OP_TRY_END,         // 移除异常处理器
        OP_THROW,           // 抛出异常

        // 字典
        OP_BUILD_DICT,      // [count:16bit] 从 2N 个值构建字典
        OP_DICT_REST,       // [count:16bit] ★ 新增
        OP_BUILD_NAMESPACE, // [name_idx:16bit, count:16bit] ★ 新增
        OP_BUILD_SET,       // [count:16bit] ★ 新增：从 N 个值构建集合

        // 格式化字符串
        OP_FORMAT_STRING,   // [spec_idx:16bit] 格式化栈顶值

        // 列表推导式
        OP_LIST_INIT,       // 压入空 List
        OP_LIST_APPEND,     // 将栈顶值追加到栈中第 N 位置的 List
        OP_LIST_COMP_END,   // ★ 新增：列表推导式结束，执行智能降维

        // 闭包上值
        OP_GET_UPVALUE,     // [idx:16bit]
        OP_SET_UPVALUE,     // [idx:16bit]

        // 类 & OOP
        OP_CLASS,           // [name_idx:16bit] 创建 ClassDefinition
        OP_METHOD,          // [name_idx:16bit] 添加方法到类
        OP_INHERIT,         // 继承
        OP_GET_PROPERTY,    // [ic_idx:16bit]
        OP_TRY_GET_PROPERTY,// [ic_idx:16bit]
        OP_SET_PROPERTY,    // [ic_idx:16bit]
        OP_INVOKE,          // [argc:8bit, ic_idx:16bit]
        OP_GET_SUPER,       // [name_idx:16bit]
        OP_SUPER_INVOKE,    // [name_idx:16bit, argc:8bit]
        OP_GET_SELF,        // ★ 新增

        OP_TAIL_CALL,       // [arg_count:8bit]
        OP_TAIL_INVOKE,     // [argc:8bit, ic_idx:16bit]
        OP_TAIL_SUPER_INVOKE, // [name_idx:16bit, argc:8bit]

        // 导入
        OP_IMPORT,          // [path_idx:16bit]

        // 切片
        OP_SLICE_GET,       // 切片索引读取
        OP_SLICE_SET,
        OP_GET_REF_PARAM,   // [idx:16bit] ★ 新增
        OP_SET_REF_PARAM,   // [idx:16bit] ★ 新增
        OP_PASS_REFS,       // [sig_idx:16bit] ★ 新增

        OP_ASSERT_PARAM_TYPE,   // 参数断言：[type_idx:16bit, name_idx:16bit]
        OP_ASSERT_RETURN_TYPE,  // 返回值断言：[type_idx:16bit]

        OP_MATCH_TYPE,          // [type_idx:16bit] 检查栈顶类型，返回 bool
        OP_MATCH_SHAPE,         // [shape_idx:16bit] 检查栈顶形状，返回 bool
    };

    // =================================================================
// ★ 统一的 OpCode 字符串映射 (供反汇编器与 Profiler 共享)
// =================================================================
    inline std::string opCodeToString(OpCode op) {
        switch (op) {
        case OpCode::OP_CONSTANT: return "OP_CONSTANT";
        case OpCode::OP_NONE: return "OP_NONE";
        case OpCode::OP_TRUE: return "OP_TRUE";
        case OpCode::OP_FALSE: return "OP_FALSE";
        case OpCode::OP_POP: return "OP_POP";
        case OpCode::OP_ADD: return "OP_ADD";
        case OpCode::OP_SUBTRACT: return "OP_SUBTRACT";
        case OpCode::OP_MULTIPLY: return "OP_MULTIPLY";
        case OpCode::OP_DIVIDE: return "OP_DIVIDE";
        case OpCode::OP_MODULO: return "OP_MODULO";
        case OpCode::OP_POWER: return "OP_POWER";
        case OpCode::OP_LEFT_DIVIDE: return "OP_LEFT_DIVIDE";
        case OpCode::OP_NEGATE: return "OP_NEGATE";
        case OpCode::OP_NOT: return "OP_NOT";
        case OpCode::OP_BIT_NOT: return "OP_BIT_NOT";
        case OpCode::OP_EQUAL: return "OP_EQUAL";
        case OpCode::OP_NOT_EQUAL: return "OP_NOT_EQUAL";
        case OpCode::OP_LESS: return "OP_LESS";
        case OpCode::OP_LESS_EQUAL: return "OP_LESS_EQUAL";
        case OpCode::OP_GREATER: return "OP_GREATER";
        case OpCode::OP_GREATER_EQUAL: return "OP_GREATER_EQUAL";
        case OpCode::OP_GET_GLOBAL: return "OP_GET_GLOBAL";
        case OpCode::OP_SET_GLOBAL: return "OP_SET_GLOBAL";
        case OpCode::OP_SET_GLOBAL_REF: return "OP_SET_GLOBAL_REF";
        case OpCode::OP_DEFINE_CONST_GLOBAL: return "OP_DEFINE_CONST_GLOBAL";
        case OpCode::OP_IS_UNINIT: return "OP_IS_UNINIT";
        case OpCode::OP_GET_LOCAL: return "OP_GET_LOCAL";
        case OpCode::OP_SET_LOCAL: return "OP_SET_LOCAL";
        case OpCode::OP_JUMP: return "OP_JUMP";
        case OpCode::OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE";
        case OpCode::OP_JUMP_IF_TRUE: return "OP_JUMP_IF_TRUE";
        case OpCode::OP_LOOP: return "OP_LOOP";
        case OpCode::OP_CALL: return "OP_CALL";
        case OpCode::OP_RETURN: return "OP_RETURN";
        case OpCode::OP_PRINT: return "OP_PRINT";
        case OpCode::OP_INDEX_GET: return "OP_INDEX_GET";
        case OpCode::OP_INDEX_SET: return "OP_INDEX_SET";
        case OpCode::OP_BUILD_LIST: return "OP_BUILD_LIST";
        case OpCode::OP_BUILD_MATRIX: return "OP_BUILD_MATRIX";
        case OpCode::OP_CLOSURE: return "OP_CLOSURE";
        case OpCode::OP_DUP: return "OP_DUP";
        case OpCode::OP_ITER_INIT: return "OP_ITER_INIT";
        case OpCode::OP_ITER_NEXT: return "OP_ITER_NEXT";
        case OpCode::OP_IN: return "OP_IN";
        case OpCode::OP_STRINGIFY: return "OP_STRINGIFY";
        case OpCode::OP_CONCAT_STRINGS: return "OP_CONCAT_STRINGS";
        case OpCode::OP_TRY_BEGIN: return "OP_TRY_BEGIN";
        case OpCode::OP_TRY_END: return "OP_TRY_END";
        case OpCode::OP_THROW: return "OP_THROW";
        case OpCode::OP_BUILD_DICT: return "OP_BUILD_DICT";
        case OpCode::OP_DICT_REST: return "OP_DICT_REST";
        case OpCode::OP_BUILD_NAMESPACE: return "OP_BUILD_NAMESPACE";
        case OpCode::OP_BUILD_SET: return "OP_BUILD_SET";
        case OpCode::OP_FORMAT_STRING: return "OP_FORMAT_STRING";
        case OpCode::OP_LIST_INIT: return "OP_LIST_INIT";
        case OpCode::OP_LIST_APPEND: return "OP_LIST_APPEND";
        case OpCode::OP_LIST_COMP_END: return "OP_LIST_COMP_END";
        case OpCode::OP_GET_UPVALUE: return "OP_GET_UPVALUE";
        case OpCode::OP_SET_UPVALUE: return "OP_SET_UPVALUE";
        case OpCode::OP_CLASS: return "OP_CLASS";
        case OpCode::OP_METHOD: return "OP_METHOD";
        case OpCode::OP_INHERIT: return "OP_INHERIT";
        case OpCode::OP_GET_PROPERTY: return "OP_GET_PROPERTY";
        case OpCode::OP_TRY_GET_PROPERTY: return "OP_TRY_GET_PROPERTY";
        case OpCode::OP_SET_PROPERTY: return "OP_SET_PROPERTY";
        case OpCode::OP_INVOKE: return "OP_INVOKE";
        case OpCode::OP_GET_SUPER: return "OP_GET_SUPER";
        case OpCode::OP_SUPER_INVOKE: return "OP_SUPER_INVOKE";
        case OpCode::OP_GET_SELF: return "OP_GET_SELF";
        case OpCode::OP_IMPORT: return "OP_IMPORT";
        case OpCode::OP_SLICE_GET: return "OP_SLICE_GET";
        case OpCode::OP_SLICE_SET: return "OP_SLICE_SET";
        case OpCode::OP_GET_REF_PARAM: return "OP_GET_REF_PARAM";
        case OpCode::OP_SET_REF_PARAM: return "OP_SET_REF_PARAM";
        case OpCode::OP_PASS_REFS: return "OP_PASS_REFS";
        case OpCode::OP_BIT_AND: return "OP_BIT_AND";
        case OpCode::OP_BIT_OR: return "OP_BIT_OR";
        case OpCode::OP_BIT_XOR: return "OP_BIT_XOR";
        case OpCode::OP_BIT_SHIFT_LEFT: return "OP_BIT_SHIFT_LEFT";
        case OpCode::OP_BIT_SHIFT_RIGHT: return "OP_BIT_SHIFT_RIGHT";
        case OpCode::OP_ASSERT_PARAM_TYPE: return "OP_ASSERT_PARAM_TYPE";
        case OpCode::OP_ASSERT_RETURN_TYPE: return "OP_ASSERT_RETURN_TYPE";
        case OpCode::OP_MATCH_TYPE: return "OP_MATCH_TYPE";
        case OpCode::OP_MATCH_SHAPE: return "OP_MATCH_SHAPE";
        case OpCode::OP_TAIL_CALL: return "OP_TAIL_CALL";
        case OpCode::OP_TAIL_INVOKE: return "OP_TAIL_INVOKE";
        case OpCode::OP_TAIL_SUPER_INVOKE: return "OP_TAIL_SUPER_INVOKE";
        default: return "UNKNOWN_OP";
        }
    }

    struct InlineCache {
        uint16_t nameIdx = 0;
        ObjClass* cachedClass = nullptr;
        ObjClosure* cachedMethod = nullptr;
        int cachedFieldIndex = -1;
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
        uint16_t sourceRef;
    };

    struct CallSignature {
        std::vector<ArgSource> refs;
    };

    // ═══════════════════════════════════════════
    // 字节码块 (Chunk)
    // 存储一段编译后的字节码 + 常量池 + 行号信息
    // ═══════════════════════════════════════════
    class Chunk {
    public:
        std::vector<uint8_t> code;      // 字节码流
        std::vector<Value> constants;   // 常量池
        std::vector<int> lines;         // 每条指令对应的源码行号
        std::vector<InlineCache> inlineCaches; // ★ 内联缓存池
        std::vector<ShapePattern> shapePatterns; // ★ 形状匹配模式池
        std::vector<MatrixShape> matrixShapes;   // ★ 矩阵形状池
        std::vector<CallSignature> callSignatures; // ★ 调用签名池

        // ── 写入接口 ──

        void write(uint8_t byte, int line) {
            code.push_back(byte);
            lines.push_back(line);
        }

        void write(OpCode op, int line) {
            write(static_cast<uint8_t>(op), line);
        }

        // 写入 16-bit 操作数 (大端序)
        void write16(uint16_t val, int line) {
            write(static_cast<uint8_t>((val >> 8) & 0xFF), line);
            write(static_cast<uint8_t>(val & 0xFF), line);
        }

        // 写入 32-bit 操作数 (大端序)
        void write32(uint32_t val, int line) {
            write(static_cast<uint8_t>((val >> 24) & 0xFF), line);
            write(static_cast<uint8_t>((val >> 16) & 0xFF), line);
            write(static_cast<uint8_t>((val >> 8) & 0xFF), line);
            write(static_cast<uint8_t>(val & 0xFF), line);
        }

        // 添加常量到常量池，返回索引
        uint16_t addConstant(const Value& val) {
            constants.push_back(val);
            if (constants.size() > 65535)
                throw std::runtime_error("Compiler Error: Too many constants in one chunk (max 65535).");
            return static_cast<uint16_t>(constants.size() - 1);
        }

        // 添加常量并生成 OP_CONSTANT 指令
        void emitConstant(const Value& val, int line) {
            uint16_t idx = addConstant(val);
            write(OpCode::OP_CONSTANT, line);
            write16(idx, line);
        }

        uint16_t addInlineCache(uint16_t nameIdx) {
            inlineCaches.push_back(InlineCache{nameIdx, nullptr, nullptr, -1});
            if (inlineCaches.size() > 65535) throw std::runtime_error("Compiler Error: Too many inline caches.");
            return static_cast<uint16_t>(inlineCaches.size() - 1);
        }

        uint16_t addShapePattern(uint32_t minR, uint32_t maxR, uint32_t minC, uint32_t maxC, uint8_t mask) {
            shapePatterns.push_back({minR, maxR, minC, maxC, mask});
            if (shapePatterns.size() > 65535) throw std::runtime_error("Compiler Error: Too many shape patterns.");
            return static_cast<uint16_t>(shapePatterns.size() - 1);
        }

        uint16_t addMatrixShape(uint16_t rows, const std::vector<uint16_t>& rowCols) {
            matrixShapes.push_back({rows, rowCols});
            if (matrixShapes.size() > 65535) throw std::runtime_error("Compiler Error: Too many matrix shapes.");
            return static_cast<uint16_t>(matrixShapes.size() - 1);
        }

        uint16_t addCallSignature(const std::vector<ArgSource>& refs) {
            callSignatures.push_back({refs});
            if (callSignatures.size() > 65535) throw std::runtime_error("Compiler Error: Too many call signatures.");
            return static_cast<uint16_t>(callSignatures.size() - 1);
        }

        // 生成跳转指令，返回需要回填的偏移位置
        int emitJump(OpCode op, int line) {
            write(op, line);
            write(0xFF, line);  // 占位
            write(0xFF, line);  // 占位
            return static_cast<int>(code.size()) - 2;
        }

        // 回填跳转偏移
        void patchJump(int offset) {
            int jump = static_cast<int>(code.size()) - offset - 2;
            if (jump > 65535)
                throw std::runtime_error("Compiler Error: Jump too large.");
            code[offset] = static_cast<uint8_t>((jump >> 8) & 0xFF);
            code[offset + 1] = static_cast<uint8_t>(jump & 0xFF);
        }

        // 生成回跳（用于循环）
        void emitLoop(int loopStart, int line) {
            write(OpCode::OP_LOOP, line);
            int offset = static_cast<int>(code.size()) - loopStart + 2;
            if (offset > 65535)
                throw std::runtime_error("Compiler Error: Loop body too large.");
            write16(static_cast<uint16_t>(offset), line);
        }

        // 读取 16-bit 操作数
        uint16_t read16(int offset) const {
            return static_cast<uint16_t>((code[offset] << 8) | code[offset + 1]);
        }

        // 读取 32-bit 操作数
        uint32_t read32(int offset) const {
            return static_cast<uint32_t>((code[offset] << 24) | (code[offset + 1] << 16) | (code[offset + 2] << 8) | code[offset + 3]);
        }

        // ── 反汇编器（调试用）──

        void disassemble(const std::string& name) const {
            std::cout << "=== " << name << " ===" << std::endl;
            int offset = 0;
            while (offset < static_cast<int>(code.size())) {
                offset = disassembleInstruction(offset);
            }
            std::cout << "==================" << std::endl;
        }

        int disassembleInstruction(int offset) const {
            std::cout << std::right << std::setw(4) << std::setfill('0') << offset << "  " << std::setfill(' ');

            if (offset > 0 && lines[offset] == lines[offset - 1])
                std::cout << "   | ";
            else
                std::cout << std::right << std::setw(4) << lines[offset] << " ";

            auto op = static_cast<OpCode>(code[offset]);

            // ★ 一键提取并左对齐 18 个字符宽，告别过去那种拼凑空格的痛苦
            std::string opName = opCodeToString(op);
            std::cout << std::left << std::setw(18) << opName;

            switch (op) {
                // ============================================
                // 格式 0: 无操作数 (1 字节)
                // ============================================
            case OpCode::OP_NONE: case OpCode::OP_TRUE: case OpCode::OP_FALSE:
            case OpCode::OP_POP: case OpCode::OP_ADD: case OpCode::OP_SUBTRACT:
            case OpCode::OP_MULTIPLY: case OpCode::OP_DIVIDE: case OpCode::OP_MODULO:
            case OpCode::OP_POWER: case OpCode::OP_LEFT_DIVIDE: case OpCode::OP_NEGATE: case OpCode::OP_NOT:
            case OpCode::OP_BIT_NOT:
            case OpCode::OP_EQUAL: case OpCode::OP_NOT_EQUAL: case OpCode::OP_LESS:
            case OpCode::OP_LESS_EQUAL: case OpCode::OP_GREATER: case OpCode::OP_GREATER_EQUAL:
            case OpCode::OP_PRINT: case OpCode::OP_RETURN: case OpCode::OP_DUP:
            case OpCode::OP_IN: case OpCode::OP_STRINGIFY: case OpCode::OP_TRY_END:
            case OpCode::OP_THROW: case OpCode::OP_LIST_INIT: case OpCode::OP_INHERIT:
            case OpCode::OP_IMPORT: case OpCode::OP_BIT_AND: case OpCode::OP_BIT_OR:
            case OpCode::OP_BIT_XOR:
            case OpCode::OP_BIT_SHIFT_LEFT: case OpCode::OP_BIT_SHIFT_RIGHT:
            case OpCode::OP_GET_SELF: case OpCode::OP_LIST_COMP_END:
            case OpCode::OP_IS_UNINIT:
                std::cout << std::endl;
                return offset + 1;

            // ============================================
            // 格式 1: 1 个 uint8_t 操作数 (2 字节)
            // ============================================
            case OpCode::OP_CALL:
            case OpCode::OP_TAIL_CALL: {
                uint8_t argc = code[offset + 1];
                std::cout << static_cast<int>(argc) << " args" << std::endl;
                return offset + 2;
            }
            case OpCode::OP_INDEX_GET:
            case OpCode::OP_INDEX_SET:
            case OpCode::OP_SLICE_GET:
            case OpCode::OP_SLICE_SET: {
                uint8_t dims = code[offset + 1];
                std::cout << static_cast<int>(dims) << " dims" << std::endl;
                return offset + 2;
            }
            case OpCode::OP_ITER_INIT: {
                uint8_t flag = code[offset + 1];
                std::cout << (flag ? "destruct" : "normal") << std::endl;
                return offset + 2;
            }

            // ============================================
            // 格式 2: 1 个 uint16_t 常量池引用 (3 字节)
            // ============================================
            case OpCode::OP_CONSTANT:
            case OpCode::OP_GET_GLOBAL:
            case OpCode::OP_SET_GLOBAL:
            case OpCode::OP_SET_GLOBAL_REF:
            case OpCode::OP_DEFINE_CONST_GLOBAL:
            case OpCode::OP_CLASS:
            case OpCode::OP_METHOD:
            case OpCode::OP_GET_SUPER: 
            case OpCode::OP_ASSERT_RETURN_TYPE:
            case OpCode::OP_MATCH_TYPE:
            case OpCode::OP_MATCH_SHAPE: {
                uint16_t idx = read16(offset + 1);
                if (op == OpCode::OP_MATCH_SHAPE) {
                    if (idx < shapePatterns.size()) {
                        const auto& sp = shapePatterns[idx];
                        std::cout << "shape_idx:" << idx << " [" << sp.minRows << "~" << (sp.maxRows == 0xFFFFFFFF ? "inf" : std::to_string(sp.maxRows)) 
                                  << "]x[" << sp.minCols << "~" << (sp.maxCols == 0xFFFFFFFF ? "inf" : std::to_string(sp.maxCols)) 
                                  << "] (mask:" << static_cast<int>(sp.exactMask) << ")" << std::endl;
                    } else {
                        std::cout << "INVALID_SHAPE_IDX" << std::endl;
                    }
                } else {
                    std::cout << idx << " (";
                    if (idx < constants.size()) {
                        if (constants[idx].isString())
                            std::cout << constants[idx].asString();
                        else
                            std::cout << constants[idx];
                    }
                    std::cout << ")" << std::endl;
                }
                return offset + 3;
            }

            case OpCode::OP_GET_PROPERTY:
            case OpCode::OP_TRY_GET_PROPERTY:
            case OpCode::OP_SET_PROPERTY: {
                uint16_t icIdx = read16(offset + 1);
                std::cout << "[IC:" << icIdx << "]";
                if (icIdx < inlineCaches.size()) {
                    uint16_t nameIdx = inlineCaches[icIdx].nameIdx;
                    std::cout << " -> " << nameIdx << " (";
                    if (nameIdx < constants.size() && constants[nameIdx].isString())
                        std::cout << constants[nameIdx].asString();
                    std::cout << ")";
                }
                std::cout << std::endl;
                return offset + 3;
            }

            // ============================================
            // 格式 3: 1 个 uint16_t 槽位/数量/偏移等 (3 字节)
            // ============================================
            case OpCode::OP_GET_LOCAL:
            case OpCode::OP_SET_LOCAL:
            case OpCode::OP_GET_UPVALUE:
            case OpCode::OP_SET_UPVALUE: {
                uint16_t slot = read16(offset + 1);
                std::cout << "slot " << slot << std::endl;
                return offset + 3;
            }
            case OpCode::OP_GET_REF_PARAM:
            case OpCode::OP_SET_REF_PARAM: {
                uint16_t idx = read16(offset + 1);
                std::cout << "idx " << idx << std::endl;
                return offset + 3;
            }
            case OpCode::OP_PASS_REFS: {
                uint16_t idx = read16(offset + 1);
                std::cout << "sig_idx:" << idx;
                if (idx < callSignatures.size()) {
                    const auto& sig = callSignatures[idx];
                    std::cout << " (" << sig.refs.size() << " refs)";
                    for (const auto& ref : sig.refs) {
                        std::string typeName = (ref.sourceType == 1) ? "global" : ((ref.sourceType == 2) ? "local" : ((ref.sourceType == 3) ? "upvalue" : "refparam"));
                        std::cout << "\n         |                  arg " << static_cast<int>(ref.argIndex)
                            << " -> " << typeName << " " << ref.sourceRef;
                    }
                }
                std::cout << std::endl;
                return offset + 3;
            }
            case OpCode::OP_JUMP:
            case OpCode::OP_JUMP_IF_FALSE:
            case OpCode::OP_JUMP_IF_TRUE:
            case OpCode::OP_ITER_NEXT: {
                uint16_t jump = read16(offset + 1);
                std::cout << "-> " << (offset + 3 + jump) << std::endl;
                return offset + 3;
            }
            case OpCode::OP_LOOP: {
                uint16_t jump = read16(offset + 1);
                std::cout << "-> " << (offset + 3 - jump) << std::endl;
                return offset + 3;
            }
            case OpCode::OP_BUILD_LIST:
            case OpCode::OP_CONCAT_STRINGS:
            case OpCode::OP_BUILD_DICT:
            case OpCode::OP_DICT_REST:
            case OpCode::OP_BUILD_SET: {
                uint16_t count = read16(offset + 1);
                std::cout << count << " items" << std::endl;
                return offset + 3;
            }
            case OpCode::OP_BUILD_MATRIX: {
                uint16_t idx = read16(offset + 1);
                if (idx < matrixShapes.size()) {
                    const auto& shape = matrixShapes[idx];
                    std::cout << "shape_idx:" << idx << " (" << shape.rows << " rows: ";
                    for (uint16_t c : shape.rowCols) std::cout << c << " ";
                    std::cout << ")" << std::endl;
                } else {
                    std::cout << "INVALID_SHAPE_IDX" << std::endl;
                }
                return offset + 3;
            }
            case OpCode::OP_BUILD_NAMESPACE: {
                uint16_t nameIdx = read16(offset + 1);
                uint16_t count = read16(offset + 3);
                std::cout << nameIdx << " (";
                if (nameIdx < constants.size() && constants[nameIdx].isString())
                    std::cout << constants[nameIdx].asString();
                std::cout << ") " << count << " items" << std::endl;
                return offset + 5;
            }
            case OpCode::OP_CLOSURE:
            case OpCode::OP_FORMAT_STRING:
            case OpCode::OP_LIST_APPEND: {
                uint16_t idx = read16(offset + 1);
                std::cout << idx << std::endl;
                return offset + 3;
            }

            // ============================================
            // 格式 4: uint16_t 名称索引 + uint8_t 参数个数 (4 字节)
            // ============================================
            case OpCode::OP_SUPER_INVOKE:
            case OpCode::OP_TAIL_SUPER_INVOKE: {
                uint16_t idx = read16(offset + 1);
                uint8_t argc = code[offset + 3];
                std::cout << idx << " (";
                if (idx < constants.size() && constants[idx].isString())
                    std::cout << constants[idx].asString();
                std::cout << ") " << static_cast<int>(argc) << " args" << std::endl;
                return offset + 4;
            }
            case OpCode::OP_INVOKE:
            case OpCode::OP_TAIL_INVOKE: {
                uint8_t argc = code[offset + 1];
                uint16_t icIdx = read16(offset + 2);
                std::cout << static_cast<int>(argc) << " args [IC:" << icIdx << "]";
                if (icIdx < inlineCaches.size()) {
                    uint16_t nameIdx = inlineCaches[icIdx].nameIdx;
                    std::cout << " -> " << nameIdx << " (";
                    if (nameIdx < constants.size() && constants[nameIdx].isString())
                        std::cout << constants[nameIdx].asString();
                    std::cout << ")";
                }
                std::cout << std::endl;
                return offset + 4;
            }

            // ============================================
            // 格式 5: 定长双短参数 (5 字节)
            // ============================================
            case OpCode::OP_TRY_BEGIN: {
                uint16_t jump = read16(offset + 1);
                uint16_t nameIdx = read16(offset + 3);
                std::cout << "catch -> " << (offset + 5 + jump);
                if (nameIdx < constants.size() && constants[nameIdx].isString())
                    std::cout << " (var: " << constants[nameIdx].asString() << ")";
                std::cout << std::endl;
                return offset + 5;
            }
            case OpCode::OP_ASSERT_PARAM_TYPE: {      // ★ 新增一整块
                uint16_t typeIdx = read16(offset + 1);
                uint16_t nameIdx = read16(offset + 3);
                std::cout << "typeConst: " << typeIdx << ", nameConst: " << nameIdx << std::endl;
                return offset + 5;
            }

            default:
                std::cout << "UNKNOWN FORMAT" << std::endl;
                return offset + 1;
            }
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

        // ★ 上值捕获信息
        struct UpvalueInfo {
            std::string name;
            bool isLocal;   // true=来自外层局部变量, false=来自外层上值
            int index;      // 对应的 slot 或 upvalue index
            bool isRef = false; // ★ 新增：是否按引用捕获
            bool isGlobal = false; // ★ 新增：是否来自全局变量
            bool isExplicitState = false; // ★ 新增：是否是显式初始化的 state
            bool isRefParam = false; // ★ 新增：是否来自外层的 ref 参数
        };
        std::vector<UpvalueInfo> upvalues;
        std::vector<bool> paramIsRef;
    };

    // ═══════════════════════════════════════════
    // 调用帧 (Call Frame)
    // ═══════════════════════════════════════════
    struct CallFrame {
        const CompiledFunction* function = nullptr;
        int ip = 0;
        int stackBase = 0;
        std::shared_ptr<std::vector<std::shared_ptr<UpVal>>> upvalues;
        std::vector<std::shared_ptr<UpVal>> refParams; // ★ 新增：动态注入的引用参数

        // ★ 新增：独立与当前调用帧绑定的物理上下文寄存器！
        Value selfContext = Value::none();
        Value classContext = Value::none();
    };


} // namespace jc

#endif // JC2_BYTECODE_H
