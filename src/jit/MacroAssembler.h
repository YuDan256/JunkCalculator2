#ifndef JC2_JIT_MACRO_ASSEMBLER_H
#define JC2_JIT_MACRO_ASSEMBLER_H

#include <vector>
#include <cstdint>
#include <cstring>
#include "ExecutableMemory.h"
#include "Registers.h"
#include "Operand.h"

namespace jc {
namespace jit {

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

    // --- ALU Helper ---
    void emitALU(uint8_t op_r_rm, Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(op_r_rm);
        emitModRM(3, dst.id(), src.id());
    }
    void emitALU(uint8_t op_r_rm, Register dst, const Operand& src) {
        emitRex(false, dst, src);
        emit8(op_r_rm);
        emitOperand(dst, src);
    }
    void emitALU_rm_r(uint8_t op_rm_r, const Operand& dst, Register src) {
        emitRex(false, src, dst);
        emit8(op_rm_r);
        emitOperand(src, dst);
    }
    void emitALU_imm(uint8_t ext, Register dst, int32_t imm) {
        emitRex(false, Register(), dst);
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
    void emitALU_imm(uint8_t ext, const Operand& dst, int32_t imm) {
        emitRex(false, Register(), dst);
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

    // --- SUB (0x2B, 0x29, ext 5) ---
    void sub(Register dst, Register src) { emitALU(0x2B, dst, src); }
    void sub(Register dst, const Operand& src) { emitALU(0x2B, dst, src); }
    void sub(const Operand& dst, Register src) { emitALU_rm_r(0x29, dst, src); }
    void sub(Register dst, int32_t imm) { emitALU_imm(5, dst, imm); }
    void sub(const Operand& dst, int32_t imm) { emitALU_imm(5, dst, imm); }

    // --- AND (0x23, 0x21, ext 4) ---
    void and_(Register dst, Register src) { emitALU(0x23, dst, src); }
    void and_(Register dst, const Operand& src) { emitALU(0x23, dst, src); }
    void and_(const Operand& dst, Register src) { emitALU_rm_r(0x21, dst, src); }
    void and_(Register dst, int32_t imm) { emitALU_imm(4, dst, imm); }
    void and_(const Operand& dst, int32_t imm) { emitALU_imm(4, dst, imm); }

    // --- OR (0x0B, 0x09, ext 1) ---
    void or_(Register dst, Register src) { emitALU(0x0B, dst, src); }
    void or_(Register dst, const Operand& src) { emitALU(0x0B, dst, src); }
    void or_(const Operand& dst, Register src) { emitALU_rm_r(0x09, dst, src); }
    void or_(Register dst, int32_t imm) { emitALU_imm(1, dst, imm); }
    void or_(const Operand& dst, int32_t imm) { emitALU_imm(1, dst, imm); }

    // --- XOR (0x33, 0x31, ext 6) ---
    void xor_(Register dst, Register src) { emitALU(0x33, dst, src); }
    void xor_(Register dst, const Operand& src) { emitALU(0x33, dst, src); }
    void xor_(const Operand& dst, Register src) { emitALU_rm_r(0x31, dst, src); }
    void xor_(Register dst, int32_t imm) { emitALU_imm(6, dst, imm); }
    void xor_(const Operand& dst, int32_t imm) { emitALU_imm(6, dst, imm); }

    // --- CMP (0x3B, 0x39, ext 7) ---
    void cmp(Register dst, Register src) { emitALU(0x3B, dst, src); }
    void cmp(Register dst, const Operand& src) { emitALU(0x3B, dst, src); }
    void cmp(const Operand& dst, Register src) { emitALU_rm_r(0x39, dst, src); }
    void cmp(Register dst, int32_t imm) { emitALU_imm(7, dst, imm); }
    void cmp(const Operand& dst, int32_t imm) { emitALU_imm(7, dst, imm); }

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

    // --- CDQ / CQO (符号扩展 EAX->EDX:EAX 或 RAX->RDX:RAX) ---
    void cdq() {
        emit8(0x99);
    }
    void cqo() {
        emitRex(true, Register(), Register(), Register());
        emit8(0x99);
    }

    // --- SHIFT Helper ---
    void emitShift(uint8_t ext, Register dst, int32_t imm) {
        emitRex(false, Register(), dst);
        if (imm == 1) {
            emit8(0xD1); emitModRM(3, ext, dst.id());
        } else {
            emit8(0xC1); emitModRM(3, ext, dst.id()); emit8(static_cast<uint8_t>(imm));
        }
    }
    void emitShift(uint8_t ext, const Operand& dst, int32_t imm) {
        emitRex(false, Register(), dst);
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
    void shl_cl(Register dst) { emitShiftCL(4, dst); }
    void shl_cl(const Operand& dst) { emitShiftCL(4, dst); }

    // --- SHR (ext 5) ---
    void shr(Register dst, int32_t imm) { emitShift(5, dst, imm); }
    void shr(const Operand& dst, int32_t imm) { emitShift(5, dst, imm); }
    void shr_cl(Register dst) { emitShiftCL(5, dst); }
    void shr_cl(const Operand& dst) { emitShiftCL(5, dst); }

    // --- SAR (ext 7) ---
    void sar(Register dst, int32_t imm) { emitShift(7, dst, imm); }
    void sar(const Operand& dst, int32_t imm) { emitShift(7, dst, imm); }
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
    void movsd(const Operand& dst, XMMRegister src) {
        emit8(0xF2); emitRex(false, src, dst); emit8(0x0F); emit8(0x11); emitOperand(src, dst);
    }

    // --- SSE2 Math Helper ---
    void emitSSE2Math(uint8_t op, XMMRegister dst, XMMRegister src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(op); emitModRM(3, dst.id(), src.id());
    }
    void emitSSE2Math(uint8_t op, XMMRegister dst, const Operand& src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(op); emitOperand(dst, src);
    }

    // --- ADDSD, SUBSD, MULSD, DIVSD ---
    void addsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x58, dst, src); }
    void addsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x58, dst, src); }
    void subsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x5C, dst, src); }
    void subsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x5C, dst, src); }
    void mulsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x59, dst, src); }
    void mulsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x59, dst, src); }
    void divsd(XMMRegister dst, XMMRegister src) { emitSSE2Math(0x5E, dst, src); }
    void divsd(XMMRegister dst, const Operand& src) { emitSSE2Math(0x5E, dst, src); }

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

    // --- CVTTSD2SI (xmm/m64 -> r32) ---
    void cvttsd2si(Register dst, XMMRegister src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x2C); emitModRM(3, dst.id(), src.id());
    }
    void cvttsd2si(Register dst, const Operand& src) {
        emit8(0xF2); emitRex(false, dst, src); emit8(0x0F); emit8(0x2C); emitOperand(dst, src);
    }

    // ========================================================================
    // C++ ABI 辅助封装 (Step 15)
    // ========================================================================

    // --- 64-bit Helpers for Pointers ---
    void movq(Register dst, Register src) {
        emitRex(true, dst, src);
        emit8(0x8B);
        emitModRM(3, dst.id(), src.id());
    }
    void subq(Register dst, int32_t imm) {
        emitRex(true, Register(), dst);
        if (isInt8(imm)) {
            emit8(0x83);
            emitModRM(3, 5, dst.id());
            emit8(static_cast<uint8_t>(imm));
        } else {
            emit8(0x81);
            emitModRM(3, 5, dst.id());
            emit32(imm);
        }
    }

    // --- Prologue ---
    // stackSize: 需要分配的局部变量空间大小（不包含 Shadow Space）
    // 会自动处理 16 字节对齐和 Windows 32 字节 Shadow Space
    void prologue(int32_t stackSize = 0) {
        push(rbp);
        movq(rbp, rsp);
        
        // Windows x64 ABI 要求 32 字节的 Shadow Space
        // 加上用户请求的 stackSize，然后向上对齐到 16 字节
        int32_t totalStack = stackSize + 32;
        totalStack = (totalStack + 15) & ~15;
        
        if (totalStack > 0) {
            subq(rsp, totalStack);
        }
    }

    // --- Epilogue ---
    void epilogue() {
        movq(rsp, rbp);
        pop(rbp);
        ret();
    }

private:
    std::vector<uint8_t> buffer_;

    // 安全获取寄存器 ID，如果无效则返回 255
    template <typename T>
    uint8_t safeId(T reg) const {
        return reg.isValid() ? reg.id() : 255;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_MACRO_ASSEMBLER_H
