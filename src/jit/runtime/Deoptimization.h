#ifndef JC2_JIT_DEOPTIMIZATION_H
#define JC2_JIT_DEOPTIMIZATION_H

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <string>
#include "../ir/LIR.h"
#include "../ir/HIR.h"
#include "../../memory/Value.h"
#include "../../vm/VM.h"

namespace jc {
namespace jit {

// 机器码跳板保存的寄存器上下文 (与 MacroAssembler::emitDeoptTrampoline 严格对应)
struct SavedRegisters {
    double xmm[16];   // 偏移 0 - 127
    uint64_t gpr[16]; // 偏移 128 - 255 (索引直接对应 Register::id())
};

// 栈图槽位：描述一个虚拟寄存器在去优化时的物理位置和类型
struct StackMapSlot {
    LIROperand location; // 物理位置 (PhysicalGPR, PhysicalXMM, StackSlot, Constant)
    JITType type;        // 数据类型 (用于决定如何装箱)
};

// 栈图：描述一个 BailoutId 对应的解释器状态
struct StackMap {
    uint32_t bailoutId;
    uint32_t bytecodeIp; // 对应的字节码 IP
    std::vector<StackMapSlot> locals; // 虚拟寄存器 0~N 的映射
};

// 去优化注册表：存储所有编译函数的 Stack Maps
class DeoptRegistry {
public:
    static DeoptRegistry& get() {
        static DeoptRegistry instance;
        return instance;
    }

    void addStackMap(const StackMap& map) {
        maps_[map.bailoutId] = map;
    }

    const StackMap* getStackMap(uint32_t bailoutId) const {
        auto it = maps_.find(bailoutId);
        return it != maps_.end() ? &it->second : nullptr;
    }

private:
    std::unordered_map<uint32_t, StackMap> maps_;
};

// 去优化异常：用于跨越 JIT 边界安全地退回到解释器
struct DeoptimizationException : public std::exception {
    const char* what() const noexcept override {
        return "Deoptimization triggered";
    }
};

// 去优化运行时函数 (Step 45)
// 由汇编跳板调用，负责重建解释器状态并触发退回
inline void jc2_jit_deoptimize(SavedRegisters* regs, uint32_t bailoutId) {
    const StackMap* map = DeoptRegistry::get().getStackMap(bailoutId);
    if (!map) {
        throw std::runtime_error("JIT Error: StackMap not found for BailoutId " + std::to_string(bailoutId));
    }

    VM* vm = VM::activeVM;
    if (!vm) {
        throw std::runtime_error("JIT Error: Active VM not found during deoptimization.");
    }

    // 获取当前执行帧
    CallFrame* frame = vm->getCurrentFrame();
    if (!frame) {
        throw std::runtime_error("JIT Error: No active CallFrame during deoptimization.");
    }

    Value* vmRegisters = vm->getRegisters();
    int registerBase = frame->registerBase;

    // 1. 恢复虚拟寄存器状态
    for (size_t i = 0; i < map->locals.size(); ++i) {
        const StackMapSlot& slot = map->locals[i];
        Value reconstructed;

        if (slot.location.isPhysicalGPR()) {
            uint64_t rawVal = regs->gpr[slot.location.pregGPR().id()];
            if (slot.type == JITType::Int32) {
                reconstructed = Value::fromInt32(static_cast<int32_t>(rawVal));
            } else if (slot.type == JITType::Bool) {
                reconstructed = Value(rawVal != 0);
            } else if (slot.type == JITType::TaggedValue) {
                reconstructed.as_bits = rawVal;
            }
        } else if (slot.location.isPhysicalXMM()) {
            double rawVal = regs->xmm[slot.location.pregXMM().id()];
            if (slot.type == JITType::Double) {
                reconstructed = Value::fromDouble(rawVal);
            }
        } else if (slot.location.isStackSlot()) {
            // 从机器栈槽位恢复 (栈槽相对于 RBP)
            // regs->gpr[5] 是 RBP
            uint64_t frame_rbp = regs->gpr[5];
            uint64_t* slotPtr = reinterpret_cast<uint64_t*>(frame_rbp - slot.location.slot() - 8);
            if (slot.type == JITType::Int32) {
                reconstructed = Value::fromInt32(static_cast<int32_t>(*slotPtr));
            } else if (slot.type == JITType::Double) {
                double d;
                std::memcpy(&d, slotPtr, sizeof(double));
                reconstructed = Value::fromDouble(d);
            } else if (slot.type == JITType::Bool) {
                reconstructed = Value(*slotPtr != 0);
            } else if (slot.type == JITType::TaggedValue) {
                reconstructed.as_bits = *slotPtr;
            }
        } else if (slot.location.isImm32()) {
            if (slot.type == JITType::Int32) {
                reconstructed = Value::fromInt32(slot.location.imm32());
            } else if (slot.type == JITType::Bool) {
                reconstructed = Value(slot.location.imm32() != 0);
            }
        } else if (slot.location.isImm64()) {
            if (slot.type == JITType::Double) {
                double d;
                uint64_t bits = slot.location.imm64();
                std::memcpy(&d, &bits, sizeof(double));
                reconstructed = Value::fromDouble(d);
            } else if (slot.type == JITType::TaggedValue) {
                reconstructed.as_bits = slot.location.imm64();
            }
        }

        // 写回解释器寄存器数组
        vmRegisters[registerBase + i] = reconstructed;
    }

    // 2. 恢复指令指针
    frame->ip = map->bytecodeIp;

    // 3. 抛出异常以展开 C++ 栈，退回到 VM::run 的 catch 块
    throw DeoptimizationException();
}

} // namespace jit
} // namespace jc

#endif // JC2_JIT_DEOPTIMIZATION_H
