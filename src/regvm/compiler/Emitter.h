#ifndef JC2_REGVM_EMITTER_H
#define JC2_REGVM_EMITTER_H

#include "IR.h"
#include "../../vm/Bytecode.h"

namespace jc {
namespace regvm {

class Emitter {
public:
    // 将分配好寄存器的 IR 图发射为 32-bit 字节码
    static int emit(IRGraph* graph, Chunk& chunk);
};

} // namespace regvm
} // namespace jc

#endif // JC2_REGVM_EMITTER_H
