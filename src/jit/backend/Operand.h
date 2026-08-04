#ifndef JC2_JIT_OPERAND_H
#define JC2_JIT_OPERAND_H

#include "Registers.h"
#include <cstdint>

namespace jc {
namespace jit {

// x86-64 内存寻址的缩放因子 (Scale)
// 对应 SIB 字节中的 Scale 字段 (00=1, 01=2, 10=4, 11=8)
enum class Scale : uint8_t {
    Times1 = 0,
    Times2 = 1,
    Times4 = 2,
    Times8 = 3
};

// 内存操作数抽象类
// 封装 x86-64 的 [Base + Index * Scale + Displacement] 寻址模式
class Operand {
public:
    // 默认构造函数（构造一个无效的内存操作数）
    constexpr Operand() : base_(), index_(), scale_(Scale::Times1), disp_(0) {}

    // 模式 1: [Base + Disp]
    // 例如: [rax], [rbp - 8]
    explicit constexpr Operand(Register base, int32_t disp = 0)
        : base_(base), index_(), scale_(Scale::Times1), disp_(disp) {}

    // 模式 2: [Base + Index * Scale + Disp]
    // 例如: [rax + rcx * 4 + 16]
    constexpr Operand(Register base, Register index, Scale scale, int32_t disp = 0)
        : base_(base), index_(index), scale_(scale), disp_(disp) {}

    // 模式 3: [Index * Scale + Disp] (无 Base 寄存器)
    // 例如: [rcx * 8 + 0x1000]
    constexpr Operand(Register index, Scale scale, int32_t disp = 0)
        : base_(), index_(index), scale_(scale), disp_(disp) {}

    // 访问器
    constexpr Register base() const { return base_; }
    constexpr Register index() const { return index_; }
    constexpr Scale scale() const { return scale_; }
    constexpr int32_t disp() const { return disp_; }

private:
    Register base_;
    Register index_;
    Scale scale_;
    int32_t disp_;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_OPERAND_H
