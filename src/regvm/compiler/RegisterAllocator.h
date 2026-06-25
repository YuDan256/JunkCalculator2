#ifndef JC2_REGVM_REGISTER_ALLOCATOR_H
#define JC2_REGVM_REGISTER_ALLOCATOR_H

#include "IR.h"

namespace jc {
namespace regvm {

class RegisterAllocator {
public:
    // 执行寄存器分配：将无限的虚拟寄存器映射到 256 个物理寄存器或溢出槽
    static void allocate(IRGraph* graph);
};

} // namespace regvm
} // namespace jc

#endif // JC2_REGVM_REGISTER_ALLOCATOR_H
