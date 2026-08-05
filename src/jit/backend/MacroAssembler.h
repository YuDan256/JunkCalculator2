#ifndef JC2_JIT_MACRO_ASSEMBLER_H
#define JC2_JIT_MACRO_ASSEMBLER_H

#include <vector>
#include <deque>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "ExecutableMemory.h"
#include "Registers.h"
#include "Operand.h"

namespace jc {
namespace jit {

// ========================================================================
// 条件码 (Condition Codes)
// ========================================================================
enum class Condition : uint8_t {
    Overflow = 0x0,
    NoOverflow = 0x1,
    Below = 0x2,
    AboveOrEqual = 0x3,
    Equal = 0x4,
    Zero = 0x4,
    NotEqual = 0x5,
    NotZero = 0x5,
    BelowOrEqual = 0x6,
    Above = 0x7,
    Sign = 0x8,
    NoSign = 0x9,
    Parity = 0xA,
    NoParity = 0xB,
    Less = 0xC,
    GreaterOrEqual = 0xD,
    LessOrEqual = 0xE,
    Greater = 0xF
};

// ========================================================================
// 控制流标签 (Step 17)
// ========================================================================
class Label {
public:
    Label() : bound_(false), pos_(0) {}

    // 是否已经绑定到具体的机器码偏移量
    bool isBound() const { return bound_; }
    
    // 获取绑定的机器码偏移量
    int pos() const { return pos_; }

    // 绑定到指定的偏移量
    void bindTo(int position) {
        bound_ = true;
        pos_ = position;
    }

    // 记录一个未决的跳转指令偏移量（等待回填）
    void addUnresolvedJump(int jumpOffset) {
        unresolvedJumps_.push_back(jumpOffset);
    }

    // 获取所有未决的跳转指令偏移量
    const std::vector<int>& unresolvedJumps() const {
        return unresolvedJumps_;
    }

private:
    bool bound_;
    int pos_;
    std::vector<int> unresolvedJumps_;
};

class MacroAssembler {
public:
    MacroAssembler() {
        // 预分配一定空间，减少早期的动态扩容开销
        buffer_.reserve(1024);
    }

    // 获取当前汇编缓冲区的字节数（即当前指令的偏移量）
    size_t offset() const { return buffer_.size(); }

    // 发射 8 位数据
    void emit8(uint8_t value) {
        buffer_.push_back(value);
    }

    // 发射 16 位数据 (小端序)
    void emit16(uint16_t value) {
        size_t pos = buffer_.size();
        buffer_.resize(pos + sizeof(uint16_t));
        std::memcpy(buffer_.data() + pos, &value, sizeof(uint16_t));
    }

    // 发射 32 位数据 (小端序)
    void emit32(uint32_t value) {
        size_t pos = buffer_.size();
        buffer_.resize(pos + sizeof(uint32_t));
        std::memcpy(buffer_.data() + pos, &value, sizeof(uint32_t));
    }

    // 发射 64 位数据 (小端序)
    void emit64(uint64_t value) {
        size_t pos = buffer_.size();
        buffer_.resize(pos + sizeof(uint64_t));
        std::memcpy(buffer_.data() + pos, &value, sizeof(uint64_t));
    }

    // 将缓冲区中的机器码复制到可执行内存中，并修改内存权限为 RX
    void finalize(ExecutableMemory& execMem);

    // 获取底层缓冲区引用（供测试或高级操作使用）
    const std::vector<uint8_t>& buffer() const { return buffer_; }

    // ========================================================================
    // x86-64 REX 前缀发射逻辑 (Step 9)
    // ========================================================================
    
    // 发射 REX 前缀 (完整版)
    // w: 是否为 64 位操作数 (W bit)
    // r: ModR/M reg 字段扩展 (R bit)
    // x: SIB index 字段扩展 (X bit)
    // b: ModR/M r/m 字段扩展，或 SIB base 字段扩展，或 Opcode reg 字段扩展 (B bit)
    template <typename RegR, typename RegX, typename RegB>
    void emitRex(bool w, RegR r, RegX x, RegB b) {
        uint8_t rex = 0x40;
        if (w) rex |= 0x08;
        if (safeId(r) >= 8 && safeId(r) < 16) rex |= 0x04;
        if (safeId(x) >= 8 && safeId(x) < 16) rex |= 0x02;
        if (safeId(b) >= 8 && safeId(b) < 16) rex |= 0x01;
        
        // 只有当 W, R, X, B 中至少有一个被设置时，才需要发射 REX 前缀
        if (rex != 0x40) {
            emit8(rex);
        }
    }

    // 发射 REX 前缀 (省略 X 字段)
    template <typename RegR, typename RegB>
    void emitRex(bool w, RegR r, RegB b) {
        emitRex(w, r, Register(), b);
    }

    // 发射 REX 前缀 (仅 B 字段，常用于单操作数指令或 push/pop)
    template <typename RegB>
    void emitRex(bool w, RegB b) {
        emitRex(w, Register(), Register(), b);
    }

    // 发射 REX 前缀 (带有内存操作数)
    template <typename RegR>
    void emitRex(bool w, RegR r, const Operand& rm) {
        emitRex(w, r, rm.index(), rm.base());
    }

    // ========================================================================
    // x86-64 ModR/M 和 SIB 发射逻辑 (Step 10)
    // ========================================================================

    // 发射 ModR/M 字节
    void emitModRM(int mod, uint8_t reg, uint8_t rm) {
        emit8(static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
    }

    // 发射 SIB 字节
    void emitSIB(Scale scale, uint8_t index, uint8_t base) {
        emit8(static_cast<uint8_t>((static_cast<uint8_t>(scale) << 6) | ((index & 7) << 3) | (base & 7)));
    }

    // 判断一个 32 位整数是否可以用 8 位有符号整数表示
    bool isInt8(int32_t value) const {
        return value >= -128 && value <= 127;
    }

    // 发射内存操作数 (ModR/M, SIB, Displacement)
    // regCode: 目标寄存器的 ID (或 Opcode 扩展码)
    void emitOperand(uint8_t regCode, const Operand& op) {
        if (op.index().isValid()) {
            // 存在 Index 寄存器，必须使用 SIB
            if (op.base().isValid()) {
                int mod = 0;
                if (op.disp() == 0 && (op.base().id() & 7) != 5) {
                    mod = 0;
                } else if (isInt8(op.disp())) {
                    mod = 1;
                } else {
                    mod = 2;
                }
                emitModRM(mod, regCode, 4); // rm = 4 表示有 SIB
                emitSIB(op.scale(), op.index().id(), op.base().id());
                
                if (mod == 1) emit8(static_cast<uint8_t>(op.disp()));
                else if (mod == 2) emit32(op.disp());
            } else {
                // 只有 Index，没有 Base: [Index * Scale + disp32]
                emitModRM(0, regCode, 4);
                emitSIB(op.scale(), op.index().id(), 5); // base = 5 在 mod=0 时表示无 base
                emit32(op.disp());
            }
        } else {
            // 没有 Index 寄存器
            if (op.base().isValid()) {
                uint8_t baseId = op.base().id() & 7;
                int mod = 0;
                if (op.disp() == 0 && baseId != 5) {
                    mod = 0;
                } else if (isInt8(op.disp())) {
                    mod = 1;
                } else {
                    mod = 2;
                }
                
                if (baseId == 4) {
                    // RSP/R12 作为 Base 时，即使没有 Index 也必须使用 SIB
                    emitModRM(mod, regCode, 4);
                    emitSIB(Scale::Times1, 4, op.base().id()); // index = 4 表示无 index
                } else {
                    emitModRM(mod, regCode, op.base().id());
                }
                
                if (mod == 1) emit8(static_cast<uint8_t>(op.disp()));
                else if (mod == 2) emit32(op.disp());
            } else {
                // 既没有 Base 也没有 Index: 绝对寻址 [disp32]
                emitModRM(0, regCode, 4);
                emitSIB(Scale::Times1, 4, 5);
                emit32(op.disp());
            }
        }
    }

    // 模板重载：允许直接传入 Register 或 XMMRegister
    template <typename RegType>
    void emitOperand(RegType reg, const Operand& op) {
        emitOperand(safeId(reg), op);
    }

    // ========================================================================
    // x86-64 基础 ALU 与逻辑指令发射 (Step 11)
    // ========================================================================

    // --- MOV ---
    void mov(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x8B);
        emitModRM(3, dst.id(), src.id());
    }
    void mov(Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x8B);
        emitOperand(dst, src);
    }
    void mov(const Operand& dst, Register src) {
        emitRex(false, src, dst);
        emit8(0x89);
        emitOperand(src, dst);
    }
    void mov(Register dst, int32_t imm) {
        emitRex(false, Register(), dst);
        emit8(static_cast<uint8_t>(0xB8 + (dst.id() & 7)));
        emit32(imm);
    }
    void mov(const Operand& dst, int32_t imm) {
        emitRex(false, Register(), dst);
        emit8(0xC7);
        emitOperand(static_cast<uint8_t>(0), dst);
        emit32(imm);
    }
    void movq(Register dst, Register src) {
        emitRex(true, dst, src);
        emit8(0x8B);
        emitModRM(3, dst.id(), src.id());
    }
    void movq(Register dst, const Operand& src) {
        emitRex(true, dst, src);
        emit8(0x8B);
        emitOperand(dst, src);
    }
    void movq(const Operand& dst, Register src) {
        emitRex(true, src, dst);
        emit8(0x89);
        emitOperand(src, dst);
    }
    void movq(Register dst, int32_t imm) {
        emitRex(true, Register(), dst);
        emit8(0xC7);
        emitModRM(3, 0, dst.id());
        emit32(imm);
    }
    void movq(const Operand& dst, int32_t imm) {
        emitRex(true, Register(), dst);
        emit8(0xC7);
        emitOperand(static_cast<uint8_t>(0), dst);
        emit32(imm);
    }

    // --- ALU Helper ---
    void emitALU(uint8_t op_r_rm, Register dst, Register src, bool is64 = false) {
        emitRex(is64, dst, src);
        emit8(op_r_rm);
        emitModRM(3, dst.id(), src.id());
    }
    void emitALU(uint8_t op_r_rm, Register dst, const Operand& src, bool is64 = false) {
        emitRex(is64, dst, src);
        emit8(op_r_rm);
        emitOperand(dst, src);
    }
    void emitALU_rm_r(uint8_t op_rm_r, const Operand& dst, Register src, bool is64 = false) {
        emitRex(is64, src, dst);
        emit8(op_rm_r);
        emitOperand(src, dst);
    }
    void emitALU_imm(uint8_t ext, Register dst, int32_t imm, bool is64 = false) {
        emitRex(is64, Register(), dst);
        if (isInt8(imm)) {
            emit8(0x83);
            emitModRM(3, ext, dst.id());
            emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x81);
            emitModRM(3, ext, dst.id());
            emit32(imm);
        }
    }
    void emitALU_imm(uint8_t ext, const Operand& dst, int32_t imm, bool is64 = false) {
        emitRex(is64, Register(), dst);
        if (isInt8(imm)) {
            emit8(0x83);
            emitOperand(ext, dst);
            emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x81);
            emitOperand(ext, dst);
            emit32(imm);
        }
    }

    // --- ADD (0x03, 0x01, ext 0) ---
    void add(Register dst, Register src) { emitALU(0x03, dst, src); }
    void add(Register dst, const Operand& src) { emitALU(0x03, dst, src); }
    void add(const Operand& dst, Register src) { emitALU_rm_r(0x01, dst, src); }
    void add(Register dst, int32_t imm) { emitALU_imm(0, dst, imm); }
    void add(const Operand& dst, int32_t imm) { emitALU_imm(0, dst, imm); }
    void addq(Register dst, Register src) { emitALU(0x03, dst, src, true); }
    void addq(Register dst, const Operand& src) { emitALU(0x03, dst, src, true); }
    void addq(const Operand& dst, Register src) { emitALU_rm_r(0x01, dst, src, true); }
    void addq(Register dst, int32_t imm) { emitALU_imm(0, dst, imm, true); }
    void addq(const Operand& dst, int32_t imm) { emitALU_imm(0, dst, imm, true); }

    // --- SUB (0x2B, 0x29, ext 5) ---
    void sub(Register dst, Register src) { emitALU(0x2B, dst, src); }
    void sub(Register dst, const Operand& src) { emitALU(0x2B, dst, src); }
    void sub(const Operand& dst, Register src) { emitALU_rm_r(0x29, dst, src); }
    void sub(Register dst, int32_t imm) { emitALU_imm(5, dst, imm); }
    void sub(const Operand& dst, int32_t imm) { emitALU_imm(5, dst, imm); }
    void subq(Register dst, Register src) { emitALU(0x2B, dst, src, true); }
    void subq(Register dst, const Operand& src) { emitALU(0x2B, dst, src, true); }
    void subq(const Operand& dst, Register src) { emitALU_rm_r(0x29, dst, src, true); }
    void subq(Register dst, int32_t imm) { emitALU_imm(5, dst, imm, true); }
    void subq(const Operand& dst, int32_t imm) { emitALU_imm(5, dst, imm, true); }

    // --- AND (0x23, 0x21, ext 4) ---
    void and_(Register dst, Register src) { emitALU(0x23, dst, src); }
    void and_(Register dst, const Operand& src) { emitALU(0x23, dst, src); }
    void and_(const Operand& dst, Register src) { emitALU_rm_r(0x21, dst, src); }
    void and_(Register dst, int32_t imm) { emitALU_imm(4, dst, imm); }
    void and_(const Operand& dst, int32_t imm) { emitALU_imm(4, dst, imm); }
    void andq(Register dst, Register src) { emitALU(0x23, dst, src, true); }
    void andq(Register dst, const Operand& src) { emitALU(0x23, dst, src, true); }
    void andq(const Operand& dst, Register src) { emitALU_rm_r(0x21, dst, src, true); }
    void andq(Register dst, int32_t imm) { emitALU_imm(4, dst, imm, true); }
    void andq(const Operand& dst, int32_t imm) { emitALU_imm(4, dst, imm, true); }

    // --- OR (0x0B, 0x09, ext 1) ---
    void or_(Register dst, Register src) { emitALU(0x0B, dst, src); }
    void or_(Register dst, const Operand& src) { emitALU(0x0B, dst, src); }
    void or_(const Operand& dst, Register src) { emitALU_rm_r(0x09, dst, src); }
    void or_(Register dst, int32_t imm) { emitALU_imm(1, dst, imm); }
    void or_(const Operand& dst, int32_t imm) { emitALU_imm(1, dst, imm); }
    void orq(Register dst, Register src) { emitALU(0x0B, dst, src, true); }
    void orq(Register dst, const Operand& src) { emitALU(0x0B, dst, src, true); }
    void orq(const Operand& dst, Register src) { emitALU_rm_r(0x09, dst, src, true); }
    void orq(Register dst, int32_t imm) { emitALU_imm(1, dst, imm, true); }
    void orq(const Operand& dst, int32_t imm) { emitALU_imm(1, dst, imm, true); }

    // --- XOR (0x33, 0x31, ext 6) ---
    void xor_(Register dst, Register src) { emitALU(0x33, dst, src); }
    void xor_(Register dst, const Operand& src) { emitALU(0x33, dst, src); }
    void xor_(const Operand& dst, Register src) { emitALU_rm_r(0x31, dst, src); }
    void xor_(Register dst, int32_t imm) { emitALU_imm(6, dst, imm); }
    void xor_(const Operand& dst, int32_t imm) { emitALU_imm(6, dst, imm); }
    void xorq(Register dst, Register src) { emitALU(0x33, dst, src, true); }
    void xorq(Register dst, const Operand& src) { emitALU(0x33, dst, src, true); }
    void xorq(const Operand& dst, Register src) { emitALU_rm_r(0x31, dst, src, true); }
    void xorq(Register dst, int32_t imm) { emitALU_imm(6, dst, imm, true); }
    void xorq(const Operand& dst, int32_t imm) { emitALU_imm(6, dst, imm, true); }

    // --- CMP (0x3B, 0x39, ext 7) ---
    void cmp(Register dst, Register src) { emitALU(0x3B, dst, src); }
    void cmp(Register dst, const Operand& src) { emitALU(0x3B, dst, src); }
    void cmp(const Operand& dst, Register src) { emitALU_rm_r(0x39, dst, src); }
    void cmp(Register dst, int32_t imm) { emitALU_imm(7, dst, imm); }
    void cmp(const Operand& dst, int32_t imm) { emitALU_imm(7, dst, imm); }
    void cmpq(Register dst, Register src) { emitALU(0x3B, dst, src, true); }
    void cmpq(Register dst, const Operand& src) { emitALU(0x3B, dst, src, true); }
    void cmpq(const Operand& dst, Register src) { emitALU_rm_r(0x39, dst, src, true); }
    void cmpq(Register dst, int32_t imm) { emitALU_imm(7, dst, imm, true); }
    void cmpq(const Operand& dst, int32_t imm) { emitALU_imm(7, dst, imm, true); }

    // --- TEST (0x85, ext 0 for F7) ---
    void test(Register dst, Register src) {
        emitRex(false, src, dst);
        emit8(0x85);
        emitModRM(3, src.id(), dst.id());
    }
    void test(Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x85);
        emitOperand(dst, src);
    }
    void test(const Operand& dst, Register src) {
        emitRex(false, src, dst);
        emit8(0x85);
        emitOperand(src, dst);
    }
    void test(Register dst, int32_t imm) {
        emitRex(false, Register(), dst);
        emit8(0xF7);
        emitModRM(3, 0, dst.id());
        emit32(imm);
    }
    void test(const Operand& dst, int32_t imm) {
        emitRex(false, Register(), dst);
        emit8(0xF7);
        emitOperand(static_cast<uint8_t>(0), dst);
        emit32(imm);
    }
    void testq(Register dst, Register src) {
        emitRex(true, src, dst);
        emit8(0x85);
        emitModRM(3, src.id(), dst.id());
    }
    void testq(Register dst, const Operand& src) {
        emitRex(true, dst, src);
        emit8(0x85);
        emitOperand(dst, src);
    }
    void testq(const Operand& dst, Register src) {
        emitRex(true, src, dst);
        emit8(0x85);
        emitOperand(src, dst);
    }
    void testq(Register dst, int32_t imm) {
        emitRex(true, Register(), dst);
        emit8(0xF7);
        emitModRM(3, 0, dst.id());
        emit32(imm);
    }
    void testq(const Operand& dst, int32_t imm) {
        emitRex(true, Register(), dst);
        emit8(0xF7);
        emitOperand(static_cast<uint8_t>(0), dst);
        emit32(imm);
    }

    // ========================================================================
    // x86-64 复杂 ALU 与移位指令发射 (Step 12)
    // ========================================================================

    // --- IMUL ---
    void imul(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xAF);
        emitModRM(3, dst.id(), src.id());
    }
    void imul(Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xAF);
        emitOperand(dst, src);
    }
    void imul(Register dst, Register src, int32_t imm) {
        emitRex(false, dst, src);
        if (isInt8(imm)) {
            emit8(0x6B); emitModRM(3, dst.id(), src.id()); emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x69); emitModRM(3, dst.id(), src.id()); emit32(imm);
        }
    }
    void imul(Register dst, const Operand& src, int32_t imm) {
        emitRex(false, dst, src);
        if (isInt8(imm)) {
            emit8(0x6B); emitOperand(dst, src); emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x69); emitOperand(dst, src); emit32(imm);
        }
    }
    void imulq(Register dst, Register src) {
        emitRex(true, dst, src);
        emit8(0x0F); emit8(0xAF);
        emitModRM(3, dst.id(), src.id());
    }
    void imulq(Register dst, const Operand& src) {
        emitRex(true, dst, src);
        emit8(0x0F); emit8(0xAF);
        emitOperand(dst, src);
    }
    void imulq(Register dst, Register src, int32_t imm) {
        emitRex(true, dst, src);
        if (isInt8(imm)) {
            emit8(0x6B); emitModRM(3, dst.id(), src.id()); emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x69); emitModRM(3, dst.id(), src.id()); emit32(imm);
        }
    }
    void imulq(Register dst, const Operand& src, int32_t imm) {
        emitRex(true, dst, src);
        if (isInt8(imm)) {
            emit8(0x6B); emitOperand(dst, src); emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x69); emitOperand(dst, src); emit32(imm);
        }
    }

    // --- IDIV (隐式使用 EDX:EAX) ---
    void idiv(Register src) {
        emitRex(false, Register(), src);
        emit8(0xF7);
        emitModRM(3, 7, src.id());
    }
    void idiv(const Operand& src) {
        emitRex(false, Register(), src);
        emit8(0xF7);
        emitOperand(static_cast<uint8_t>(7), src);
    }
    void idivq(Register src) {
        emitRex(true, Register(), src);
        emit8(0xF7);
        emitModRM(3, 7, src.id());
    }
    void idivq(const Operand& src) {
        emitRex(true, Register(), src);
        emit8(0xF7);
        emitOperand(static_cast<uint8_t>(7), src);
    }

    // --- CDQ / CQO (符号扩展 EAX->EDX:EAX 或 RAX->RDX:RAX) ---
    void cdq() {
        emit8(0x99);
    }
    void cqo() {
        emitRex(true, Register(), Register(), Register());
        emit8(0x99);
    }

    // --- NEG / NOT / INC / DEC ---
    void neg(Register dst) { emitRex(false, Register(), dst); emit8(0xF7); emitModRM(3, 3, dst.id()); }
    void neg(const Operand& dst) { emitRex(false, Register(), dst); emit8(0xF7); emitOperand(static_cast<uint8_t>(3), dst); }
    void negq(Register dst) { emitRex(true, Register(), dst); emit8(0xF7); emitModRM(3, 3, dst.id()); }
    void negq(const Operand& dst) { emitRex(true, Register(), dst); emit8(0xF7); emitOperand(static_cast<uint8_t>(3), dst); }

    void not_(Register dst) { emitRex(false, Register(), dst); emit8(0xF7); emitModRM(3, 2, dst.id()); }
    void not_(const Operand& dst) { emitRex(false, Register(), dst); emit8(0xF7); emitOperand(static_cast<uint8_t>(2), dst); }
    void notq(Register dst) { emitRex(true, Register(), dst); emit8(0xF7); emitModRM(3, 2, dst.id()); }
    void notq(const Operand& dst) { emitRex(true, Register(), dst); emit8(0xF7); emitOperand(static_cast<uint8_t>(2), dst); }

    void inc(Register dst) { emitRex(false, Register(), dst); emit8(0xFF); emitModRM(3, 0, dst.id()); }
    void inc(const Operand& dst) { emitRex(false, Register(), dst); emit8(0xFF); emitOperand(static_cast<uint8_t>(0), dst); }
    void incq(Register dst) { emitRex(true, Register(), dst); emit8(0xFF); emitModRM(3, 0, dst.id()); }
    void incq(const Operand& dst) { emitRex(true, Register(), dst); emit8(0xFF); emitOperand(static_cast<uint8_t>(0), dst); }

    void dec(Register dst) { emitRex(false, Register(), dst); emit8(0xFF); emitModRM(3, 1, dst.id()); }
    void dec(const Operand& dst) { emitRex(false, Register(), dst); emit8(0xFF); emitOperand(static_cast<uint8_t>(1), dst); }
    void decq(Register dst) { emitRex(true, Register(), dst); emit8(0xFF); emitModRM(3, 1, dst.id()); }
    void decq(const Operand& dst) { emitRex(true, Register(), dst); emit8(0xFF); emitOperand(static_cast<uint8_t>(1), dst); }

    // --- SHIFT Helper ---
    void emitShift(uint8_t ext, Register dst, int32_t imm, bool is64 = false) {
        emitRex(is64, Register(), dst);
        if (imm == 1) {
            emit8(0xD1); emitModRM(3, ext, dst.id());
        } else {
            emit8(0xC1); emitModRM(3, ext, dst.id()); emit8(static_cast<uint8_t>(imm));
        }
    }
    void emitShift(uint8_t ext, const Operand& dst, int32_t imm, bool is64 = false) {
        emitRex(is64, Register(), dst);
        if (imm == 1) {
            emit8(0xD1); emitOperand(ext, dst);
        } else {
            emit8(0xC1); emitOperand(ext, dst); emit8(static_cast<uint8_t>(imm));
        }
    }
    void emitShiftCL(uint8_t ext, Register dst) {
        emitRex(false, Register(), dst);
        emit8(0xD3); emitModRM(3, ext, dst.id());
    }
    void emitShiftCL(uint8_t ext, const Operand& dst) {
        emitRex(false, Register(), dst);
        emit8(0xD3); emitOperand(ext, dst);
    }

    // --- SHL (ext 4) ---
    void shl(Register dst, int32_t imm) { emitShift(4, dst, imm); }
    void shl(const Operand& dst, int32_t imm) { emitShift(4, dst, imm); }
    void shlq(Register dst, int32_t imm) { emitShift(4, dst, imm, true); }
    void shlq(const Operand& dst, int32_t imm) { emitShift(4, dst, imm, true); }
    void shl_cl(Register dst) { emitShiftCL(4, dst); }
    void shl_cl(const Operand& dst) { emitShiftCL(4, dst); }

    // --- SHR (ext 5) ---
    void shr(Register dst, int32_t imm) { emitShift(5, dst, imm); }
    void shr(const Operand& dst, int32_t imm) { emitShift(5, dst, imm); }
    void shrq(Register dst, int32_t imm) { emitShift(5, dst, imm, true); }
    void shrq(const Operand& dst, int32_t imm) { emitShift(5, dst, imm, true); }
    void shr_cl(Register dst) { emitShiftCL(5, dst); }
    void shr_cl(const Operand& dst) { emitShiftCL(5, dst); }

    // --- SAR (ext 7) ---
    void sar(Register dst, int32_t imm) { emitShift(7, dst, imm); }
    void sar(const Operand& dst, int32_t imm) { emitShift(7, dst, imm); }
    void sarq(Register dst, int32_t imm) { emitShift(7, dst, imm, true); }
    void sarq(const Operand& dst, int32_t imm) { emitShift(7, dst, imm, true); }
    void sar_cl(Register dst) { emitShiftCL(7, dst); }
    void sar_cl(const Operand& dst) { emitShiftCL(7, dst); }

    // ========================================================================
    // x86-64 栈操作与 64 位立即数加载 (Step 13)
    // ========================================================================

    // --- PUSH ---
    void push(Register src) {
        emitRex(false, src);
        emit8(static_cast<uint8_t>(0x50 + (src.id() & 7)));
    }
    void push(const Operand& src) {
        emitRex(false, Register(), src);
        emit8(0xFF);
        emitOperand(static_cast<uint8_t>(6), src);
    }
    void push(int32_t imm) {
        if (isInt8(imm)) {
            emit8(0x6A);
            emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x68);
            emit32(imm);
        }
    }

    // --- POP ---
    void pop(Register dst) {
        emitRex(false, dst);
        emit8(static_cast<uint8_t>(0x58 + (dst.id() & 7)));
    }
    void pop(const Operand& dst) {
        emitRex(false, Register(), dst);
        emit8(0x8F);
        emitOperand(static_cast<uint8_t>(0), dst);
    }

    // --- XCHG ---
    void xchg(Register dst, Register src) {
        if (dst == rax) {
            emitRex(false, src);
            emit8(static_cast<uint8_t>(0x90 + (src.id() & 7)));
        } else if (src == rax) {
            emitRex(false, dst);
            emit8(static_cast<uint8_t>(0x90 + (dst.id() & 7)));
        } else {
            emitRex(false, dst, src);
            emit8(0x87);
            emitModRM(3, dst.id(), src.id());
        }
    }
    void xchgq(Register dst, Register src) {
        if (dst == rax) {
            emitRex(true, src);
            emit8(static_cast<uint8_t>(0x90 + (src.id() & 7)));
        } else if (src == rax) {
            emitRex(true, dst);
            emit8(static_cast<uint8_t>(0x90 + (dst.id() & 7)));
        } else {
            emitRex(true, dst, src);
            emit8(0x87);
            emitModRM(3, dst.id(), src.id());
        }
    }

    // --- MOVZX / MOVSX ---
    void movzxb(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xB6);
        emitModRM(3, dst.id(), src.id());
    }
    void movzxb(Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xB6);
        emitOperand(dst, src);
    }
    void movzxw(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xB7);
        emitModRM(3, dst.id(), src.id());
    }
    void movzxw(Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xB7);
        emitOperand(dst, src);
    }
    void movsxb(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xBE);
        emitModRM(3, dst.id(), src.id());
    }
    void movsxb(Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xBE);
        emitOperand(dst, src);
    }
    void movsxw(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xBF);
        emitModRM(3, dst.id(), src.id());
    }
    void movsxw(Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x0F); emit8(0xBF);
        emitOperand(dst, src);
    }
    void movsxd(Register dst, Register src) {
        emitRex(true, dst, src);
        emit8(0x63);
        emitModRM(3, dst.id(), src.id());
    }
    void movsxd(Register dst, const Operand& src) {
        emitRex(true, dst, src);
        emit8(0x63);
        emitOperand(dst, src);
    }

    // --- MOVABS (Load 64-bit immediate) ---
    void movabs(Register dst, uint64_t imm64) {
        emitRex(true, Register(), dst); // W=1
        emit8(static_cast<uint8_t>(0xB8 + (dst.id() & 7)));
        emit64(imm64);
    }

    // --- RET ---
    void ret() {
        emit8(0xC3);
    }

    // --- NOP ---
    void nop() {
        emit8(0x90);
    }

    // --- INT3 ---
    void int3() {
        emit8(0xCC);
    }

    // ========================================================================
    // 常量池与 RIP 相对寻址 (Step 20)
    // ========================================================================

    // --- RIP-Relative Helper ---
    void emitRipRelative(uint8_t regCode, Label& L) {
        emitModRM(0, regCode, 5); // mod=00, rm=101 (5) 表示 RIP 相对寻址
        if (L.isBound()) {
            int32_t offset_val = L.pos() - static_cast<int32_t>(offset()) - 4;
            emit32(offset_val);
        } else {
            L.addUnresolvedJump(static_cast<int>(offset()));
            emit32(0);
        }
    }

    // --- LEA ---
    void lea(Register dst, const Operand& src) {
        emitRex(true, dst, src); // LEA 通常用于 64 位地址计算
        emit8(0x8D);
        emitOperand(dst, src);
    }
    void lea(Register dst, Label& L) {
        emitRex(true, dst, Register());
        emit8(0x8D);
        emitRipRelative(dst.id(), L);
    }

    // --- MOVQ (RIP-relative) ---
    void movq(Register dst, Label& L) {
        emitRex(true, dst, Register());
        emit8(0x8B);
        emitRipRelative(dst.id(), L);
    }

    // --- Constant Pool API ---
    Label& addConstant64(uint64_t val) {
        constants_.emplace_back(Label(), val);
        return constants_.back().first;
    }
    Label& addConstantDouble(double val) {
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(double));
        return addConstant64(bits);
    }
    void emitConstantPool() {
        for (auto& pair : constants_) {
            bind(pair.first);
            emit64(pair.second);
        }
        constants_.clear();
    }

    // ========================================================================
    // 控制流与函数调用 (Step 18 & 19)
    // ========================================================================

    // --- BIND ---
    void bind(Label& L) {
        if (L.isBound()) {
            throw std::runtime_error("JIT Error: Label is already bound.");
        }
        int current_pos = static_cast<int>(offset());
        L.bindTo(current_pos);
        
        // 回填 (Backpatching) 所有未决的跳转指令
        for (int jump_pos : L.unresolvedJumps()) {
            int32_t offset_val = current_pos - jump_pos - 4;
            std::memcpy(buffer_.data() + jump_pos, &offset_val, sizeof(int32_t));
        }
    }

    // --- JMP ---
    void jmp(Label& L) {
        emit8(0xE9);
        if (L.isBound()) {
            int32_t offset_val = L.pos() - static_cast<int32_t>(offset()) - 4;
            emit32(offset_val);
        } else {
            L.addUnresolvedJump(static_cast<int>(offset()));
            emit32(0);
        }
    }
    void jmp(Register reg) {
        emitRex(false, Register(), reg);
        emit8(0xFF);
        emitModRM(3, 4, reg.id());
    }

    // --- CMOVCC ---
    void cmovcc(Condition cond, Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x0F);
        emit8(0x40 + static_cast<uint8_t>(cond));
        emitModRM(3, dst.id(), src.id());
    }
    void cmovcc(Condition cond, Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(0x0F);
        emit8(0x40 + static_cast<uint8_t>(cond));
        emitOperand(dst, src);
    }
    void cmovqcc(Condition cond, Register dst, Register src) {
        emitRex(true, dst, src);
        emit8(0x0F);
        emit8(0x40 + static_cast<uint8_t>(cond));
        emitModRM(3, dst.id(), src.id());
    }
    void cmovqcc(Condition cond, Register dst, const Operand& src) {
        emitRex(true, dst, src);
        emit8(0x0F);
        emit8(0x40 + static_cast<uint8_t>(cond));
        emitOperand(dst, src);
    }

    // --- SETCC ---
    void setcc(Condition cond, Register dst) {
        uint8_t id = dst.id();
        if (id >= 4 && id <= 7) {
            emit8(0x40); // Force REX to access SPL, BPL, SIL, DIL
        } else {
            emitRex(false, Register(), dst);
        }
        emit8(0x0F);
        emit8(0x90 + static_cast<uint8_t>(cond));
        emitModRM(3, 0, id);
    }
    void setcc(Condition cond, const Operand& dst) {
        emitRex(false, Register(), dst);
        emit8(0x0F);
        emit8(0x90 + static_cast<uint8_t>(cond));
        emitOperand(static_cast<uint8_t>(0), dst);
    }

    // --- JCC ---
    void jcc(Condition cond, Label& L) {
        emit8(0x0F);
        emit8(0x80 + static_cast<uint8_t>(cond));
        if (L.isBound()) {
            int32_t offset_val = L.pos() - static_cast<int32_t>(offset()) - 4;
            emit32(offset_val);
        } else {
            L.addUnresolvedJump(static_cast<int>(offset()));
            emit32(0);
        }
    }

    // --- CALL C++ Function (ABI Compliant) ---
    void callCFunction(void* funcPtr) {
        // 1. 保存 R12 (Callee-Saved)
        push(r12);
        // 2. 保存原始 RSP 到 R12
        movq(r12, rsp);
        // 3. 16 字节对齐
        andq(rsp, -16);
        // 4. 分配 Shadow Space (Windows x64 需要 32 字节)
#ifdef _WIN32
        subq(rsp, 32);
#endif
        // 5. 调用目标函数
        movabs(rax, reinterpret_cast<uint64_t>(funcPtr));
        call(rax);
        // 6. 恢复原始 RSP
        movq(rsp, r12);
        // 7. 恢复 R12
        pop(r12);
    }

    // --- CALL ---
    void call(Label& L) {
        emit8(0xE8);
        if (L.isBound()) {
            int32_t offset_val = L.pos() - static_cast<int32_t>(offset()) - 4;
            emit32(offset_val);
        } else {
            L.addUnresolvedJump(static_cast<int>(offset()));
            emit32(0);
        }
    }
    void call(Register reg) {
        emitRex(false, Register(), reg);
        emit8(0xFF);
        emitModRM(3, 2, reg.id());
    }

    // ========================================================================
    // x86-64 浮点标量指令 (SSE2) (Step 14)
    // ========================================================================

    // --- MOVSD ---
    void movsd(XMMRegister dst, XMMRegister src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x10); emitModRM(3, dst.id(), src.id());
    }
    void movsd(XMMRegister dst, const Operand& src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x10); emitOperand(dst, src);
    }
    void movsd(XMMRegister dst, Label& L) {
        emit8(0xF2); emitRex(false, dst, Register()); emit8(0x0F); emit8(0x10); emitRipRelative(dst.id(), L);
    }
    void movsd(const Operand& dst, XMMRegister src) {
        emit8(0xF2); emitRex(false, src, dst); emit8(0x0F); emit8(0x11); emitOperand(src, dst);
    }

    // --- MOVAPD ---
    void movapd(XMMRegister dst, XMMRegister src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x28); emitModRM(3, dst.id(), src.id());
    }
    void movapd(XMMRegister dst, const Operand& src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x28); emitOperand(dst, src);
    }
    void movapd(const Operand& dst, XMMRegister src) {
        emit8(0x66); emitRex(false, src, dst); emit8(0x0F); emit8(0x29); emitOperand(src, dst);
    }

    // --- SSE2 Math Helper ---
    void emitSSE2Math(uint8_t op, XMMRegister dst, XMMRegister src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(op); emitModRM(3, dst.id(), src.id());
    }
    void emitSSE2Math(uint8_t op, XMMRegister dst, const Operand& src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(op); emitOperand(dst, src);
    }

    // --- ADDSD, SUBSD, MULSD, DIVSD, SQRTSD ---
    void addsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x58, dst, src); }
    void addsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x58, dst, src); }
    void subsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x5C, dst, src); }
    void subsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x5C, dst, src); }
    void mulsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x59, dst, src); }
    void mulsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x59, dst, src); }
    void divsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x5E, dst, src); }
    void divsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x5E, dst, src); }
    void sqrtsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x51, dst, src); }
    void sqrtsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x51, dst, src); }

    // --- SSE2 Logical ---
    void andpd(XMMRegister dst, XMMRegister src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x54); emitModRM(3, dst.id(), src.id());
    }
    void andpd(XMMRegister dst, const Operand& src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x54); emitOperand(dst, src);
    }
    void andpd(XMMRegister dst, Label& L) {
        emit8(0x66); emitRex(false, dst, Register()); emit8(0x0F); emit8(0x54); emitRipRelative(dst.id(), L);
    }
    void xorpd(XMMRegister dst, XMMRegister src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x57); emitModRM(3, dst.id(), src.id());
    }
    void xorpd(XMMRegister dst, const Operand& src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x57); emitOperand(dst, src);
    }
    void xorpd(XMMRegister dst, Label& L) {
        emit8(0x66); emitRex(false, dst, Register()); emit8(0x0F); emit8(0x57); emitRipRelative(dst.id(), L);
    }
    void pxor(XMMRegister dst, XMMRegister src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0xEF); emitModRM(3, dst.id(), src.id());
    }
    void pxor(XMMRegister dst, const Operand& src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0xEF); emitOperand(dst, src);
    }
    void pand(XMMRegister dst, XMMRegister src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0xDB); emitModRM(3, dst.id(), src.id());
    }
    void pand(XMMRegister dst, const Operand& src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0xDB); emitOperand(dst, src);
    }
    void por(XMMRegister dst, XMMRegister src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0xEB); emitModRM(3, dst.id(), src.id());
    }
    void por(XMMRegister dst, const Operand& src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0xEB); emitOperand(dst, src);
    }

    // --- SSE4.1 ROUNDSD ---
    void roundsd(XMMRegister dst, XMMRegister src, uint8_t imm8) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x3A); emit8(0x0B); emitModRM(3, dst.id(), src.id()); emit8(imm8);
    }
    void roundsd(XMMRegister dst, const Operand& src, uint8_t imm8) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x3A); emit8(0x0B); emitOperand(dst, src); emit8(imm8);
    }

    // --- x87 FPU ---
    void fld_d(const Operand& src) {
        emitRex(false, Register(), src); emit8(0xDD); emitOperand(0, src);
    }
    void fstp_d(const Operand& dst) {
        emitRex(false, Register(), dst); emit8(0xDD); emitOperand(3, dst);
    }
    void fsin() { emit8(0xD9); emit8(0xFE); }
    void fcos() { emit8(0xD9); emit8(0xFF); }

    // --- UCOMISD ---
    void ucomisd(XMMRegister dst, XMMRegister src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x2E); emitModRM(3, dst.id(), src.id());
    }
    void ucomisd(XMMRegister dst, const Operand& src) {
        emit8(0x66); emitRex(false, dst, src); emit8(0x0F); emit8(0x2E); emitOperand(dst, src);
    }

    // --- CVTSI2SD (r/m32 -> xmm) ---
    void cvtsi2sd(XMMRegister dst, Register src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x2A); emitModRM(3, dst.id(), src.id());
    }
    void cvtsi2sd(XMMRegister dst, const Operand& src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x2A); emitOperand(dst, src);
    }
    void cvtsi2sdq(XMMRegister dst, Register src) {
        emit8(0xF2); emitRex(true, dst, src); emit8(0x0F); emit8(0x2A); emitModRM(3, dst.id(), src.id());
    }
    void cvtsi2sdq(XMMRegister dst, const Operand& src) {
        emit8(0xF2); emitRex(true, dst, src); emit8(0x0F); emit8(0x2A); emitOperand(dst, src);
    }

    // --- CVTTSD2SI (xmm/m64 -> r32) ---
    void cvttsd2si(Register dst, XMMRegister src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x2C); emitModRM(3, dst.id(), src.id());
    }
    void cvttsd2si(Register dst, const Operand& src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x2C); emitOperand(dst, src);
    }
    void cvttsd2siq(Register dst, XMMRegister src) {
        emit8(0xF2); emitRex(true, dst, src); emit8(0x0F); emit8(0x2C); emitModRM(3, dst.id(), src.id());
    }
    void cvttsd2siq(Register dst, const Operand& src) {
        emit8(0xF2); emitRex(true, dst, src); emit8(0x0F); emit8(0x2C); emitOperand(dst, src);
    }

    // ========================================================================
    // C++ ABI 辅助封装 (Step 15)
    // ========================================================================

    // --- Deoptimization Trampoline (Step 44) ---
    void emitDeoptTrampoline(Label& trampolineLabel, void* deoptRuntimeFunc) {
        bind(trampolineLabel);
        
        // 1. 保存所有通用寄存器 (GPRs)
        // 逆序压栈，使得内存中的数组索引直接对应寄存器 ID (RAX=0, RCX=1...)
        push(r15);
        push(r14);
        push(r13);
        push(r12);
        push(r11);
        push(r10); // 包含 BailoutId
        push(r9);
        push(r8);
        push(rdi);
        push(rsi);
        push(rbp);
        subq(rsp, 8); // 占位 RSP (id = 4)
        push(rbx);
        push(rdx);
        push(rcx);
        push(rax);

        // 2. 保存所有浮点寄存器 (XMM0 - XMM15)
        // 仅保存低 64 位 (Double)
        subq(rsp, 128);
        movsd(Operand(rsp, 0), xmm0);
        movsd(Operand(rsp, 8), xmm1);
        movsd(Operand(rsp, 16), xmm2);
        movsd(Operand(rsp, 24), xmm3);
        movsd(Operand(rsp, 32), xmm4);
        movsd(Operand(rsp, 40), xmm5);
        movsd(Operand(rsp, 48), xmm6);
        movsd(Operand(rsp, 56), xmm7);
        movsd(Operand(rsp, 64), xmm8);
        movsd(Operand(rsp, 72), xmm9);
        movsd(Operand(rsp, 80), xmm10);
        movsd(Operand(rsp, 88), xmm11);
        movsd(Operand(rsp, 96), xmm12);
        movsd(Operand(rsp, 104), xmm13);
        movsd(Operand(rsp, 112), xmm14);
        movsd(Operand(rsp, 120), xmm15);

        // 3. 准备调用 C++ 运行时函数
        movq(rcx, rsp); // arg1: savedRegs (指向栈顶的 256 字节结构体)
        movq(rdx, r10); // arg2: bailoutId (之前存在 R10)

        movq(r11, rsp); // 保存原始 RSP
        andq(rsp, -16); // 16 字节对齐
        subq(rsp, 32);  // Shadow Space

        if (deoptRuntimeFunc) {
            movabs(rax, reinterpret_cast<uint64_t>(deoptRuntimeFunc));
            call(rax);
        }

        // 恢复原始 RSP
        movq(rsp, r11);

        // 4. 恢复所有浮点寄存器
        addq(rsp, 128); // 丢弃浮点寄存器保存区

        // 5. 恢复所有通用寄存器
        addq(rsp, 128); // 丢弃通用寄存器保存区

        // 6. 执行 Epilogue 并返回到 VM::run
        epilogue();
    }

    // --- OSR Prologue (Step 81) ---
    // OSR Prologue 负责在热切换时建立 JIT 栈帧。
    // 具体的变量装载（从解释器栈到物理寄存器）由 HIR 的 LoadRegister 节点
    // 结合 LinearScan 自动完成，从而完美复用现有的寄存器分配和死代码消除管线。
    void osrPrologue(int32_t stackSize = 0) {
        prologue(stackSize);
    }

    // --- Prologue ---
    // stackSize: 需要分配的局部变量空间大小（不包含 Shadow Space）
    // 会自动处理 16 字节对齐和 Windows 32 字节 Shadow Space
    void prologue(int32_t stackSize = 0) {
        push(rbp);
        movq(rbp, rsp);
        
        // 保存 Callee-Saved 寄存器 (兼容 Windows x64 和 System V ABI)
        push(rbx);
        push(r12);
        push(r13);
        push(r14);
        push(r15);
        push(rdi);
        push(rsi);
        
        int32_t totalStack = stackSize + 32;
        if (totalStack > 0) {
            subq(rsp, totalStack);
        }
        andq(rsp, -16); // 强制 16 字节对齐
    }

    // --- Epilogue ---
    void epilogue() {
        // 恢复 RSP 到保存 Callee-Saved 寄存器后的位置 (7 个寄存器 * 8 字节 = 56)
        lea(rsp, Operand(rbp, -56));
        pop(rsi);
        pop(rdi);
        pop(r15);
        pop(r14);
        pop(r13);
        pop(r12);
        pop(rbx);
        pop(rbp);
        ret();
    }

private:
    std::vector<uint8_t> buffer_;
    std::deque<std::pair<Label, uint64_t>> constants_;

    // 安全获取寄存器 ID，如果无效则返回 255
    template <typename T>
    uint8_t safeId(T reg) const {
        if constexpr (std::is_integral_v<T>) {
            return static_cast<uint8_t>(reg);
        } else {
            return reg.isValid() ? reg.id() : 255;
        }
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_MACRO_ASSEMBLER_H
