#ifndef JC2_JIT_REGISTERS_H
#define JC2_JIT_REGISTERS_H

#include <cstdint>

namespace jc {
namespace jit {

// ============================================================================
// 通用寄存器 (General Purpose Registers)
// ============================================================================
class Register {
public:
    constexpr Register() : id_(255) {} // 默认构造为无效寄存器
    constexpr explicit Register(uint8_t id) : id_(id) {}

    constexpr uint8_t id() const { return id_; }
    constexpr bool isValid() const { return id_ < 16; }

    constexpr bool operator==(const Register& other) const { return id_ == other.id_; }
    constexpr bool operator!=(const Register& other) const { return id_ != other.id_; }

private:
    uint8_t id_;
};

// x86-64 物理通用寄存器常量
constexpr Register rax(0);
constexpr Register rcx(1);
constexpr Register rdx(2);
constexpr Register rbx(3);
constexpr Register rsp(4);
constexpr Register rbp(5);
constexpr Register rsi(6);
constexpr Register rdi(7);
constexpr Register r8(8);
constexpr Register r9(9);
constexpr Register r10(10);
constexpr Register r11(11);
constexpr Register r12(12);
constexpr Register r13(13);
constexpr Register r14(14);
constexpr Register r15(15);

// ============================================================================
// 浮点/SIMD 寄存器 (XMM Registers)
// ============================================================================
class XMMRegister {
public:
    constexpr XMMRegister() : id_(255) {} // 默认构造为无效寄存器
    constexpr explicit XMMRegister(uint8_t id) : id_(id) {}

    constexpr uint8_t id() const { return id_; }
    constexpr bool isValid() const { return id_ < 16; }

    constexpr bool operator==(const XMMRegister& other) const { return id_ == other.id_; }
    constexpr bool operator!=(const XMMRegister& other) const { return id_ != other.id_; }

private:
    uint8_t id_;
};

// x86-64 物理 XMM 寄存器常量
constexpr XMMRegister xmm0(0);
constexpr XMMRegister xmm1(1);
constexpr XMMRegister xmm2(2);
constexpr XMMRegister xmm3(3);
constexpr XMMRegister xmm4(4);
constexpr XMMRegister xmm5(5);
constexpr XMMRegister xmm6(6);
constexpr XMMRegister xmm7(7);
constexpr XMMRegister xmm8(8);
constexpr XMMRegister xmm9(9);
constexpr XMMRegister xmm10(10);
constexpr XMMRegister xmm11(11);
constexpr XMMRegister xmm12(12);
constexpr XMMRegister xmm13(13);
constexpr XMMRegister xmm14(14);
constexpr XMMRegister xmm15(15);

} // namespace jit
} // namespace jc

#endif // JC2_JIT_REGISTERS_H
