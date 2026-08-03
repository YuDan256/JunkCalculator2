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
    // x86-64 基础 ALU 指令发射 (Step 11)
    // ========================================================================

    // mov r32, r32
    void mov(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x8B);
        emitModRM(3, dst.id(), src.id());
    }

    // mov r32, imm32
    void mov(Register dst, int32_t imm) {
        emitRex(false, Register(), dst);
        emit8(static_cast<uint8_t>(0xB8 + (dst.id() & 7)));
        emit32(imm);
    }

    // add r32, r32
    void add(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x03);
        emitModRM(3, dst.id(), src.id());
    }

    // sub r32, r32
    void sub(Register dst, Register src) {
        emitRex(false, dst, src);
        emit8(0x2B);
        emitModRM(3, dst.id(), src.id());
    }

    // ret
    void ret() {
        emit8(0xC3);
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
