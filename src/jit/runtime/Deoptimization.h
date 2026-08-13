#ifndef JC2_JIT_DEOPTIMIZATION_H
#define JC2_JIT_DEOPTIMIZATION_H

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <string>
#include <iostream>
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
    JITType type = JITType::Unknown; // 数据类型 (用于决定如何装箱)
};

// 栈图：描述一个 BailoutId 对应的解释器状态
struct StackMap {
    uint32_t bailoutId = 0;
    uint32_t bytecodeIp = 0; // 对应的字节码 IP
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

    // 为每次编译分配一段全局唯一的 bailoutId 基址，避免 OSR / Tier 2 多次编译
    // 之间 bailoutId 冲突（后编译的 StackMap 覆盖先编译的，导致去优化拿错映射）。
    uint32_t allocateBailoutIdBase() {
        uint32_t base = nextBase_;
        nextBase_ += 4096;
        return base;
    }

private:
    std::unordered_map<uint32_t, StackMap> maps_;
    uint32_t nextBase_ = 0;
};

inline bool g_jc2_jit_deoptimized = false;
inline uint32_t g_jit_pending_exception = 0;

// 主动同步运行时函数 (Eager Sync)
// 由汇编跳板调用，负责将 JIT 物理寄存器状态安全地刷回解释器，触发正确的引用计数
inline void jc2_jit_sync_frame(SavedRegisters* regs, uint32_t bailoutId) {
    const StackMap* map = DeoptRegistry::get().getStackMap(bailoutId);
    if (!map) return;

    VM* vm = VM::activeVM;
    if (!vm) return;

    CallFrame* frame = vm->getCurrentFrame();
    if (!frame) return;

    Value* vmRegisters = vm->getRegisters();
    int registerBase = frame->registerBase;

    for (size_t i = 0; i < map->locals.size(); ++i) {
        const StackMapSlot& slot = map->locals[i];
        if (slot.location.isInvalid()) continue;
        
        Value reconstructed;

        if (slot.location.isPhysicalGPR()) {
            uint64_t rawVal = regs->gpr[slot.location.pregGPR().id()];
            if (slot.type == JITType::Int32) {
                reconstructed = Value::fromInt32(static_cast<int32_t>(rawVal));
            } else if (slot.type == JITType::Bool) {
                reconstructed = Value(rawVal != 0);
            } else if (slot.type == JITType::TaggedValue) {
                reconstructed = Value::fromRawBits(rawVal);
            }
        } else if (slot.location.isPhysicalXMM()) {
            double rawVal = regs->xmm[slot.location.pregXMM().id()];
            if (slot.type == JITType::Double) {
                reconstructed = Value::fromDouble(rawVal);
            }
        } else if (slot.location.isStackSlot()) {
            uint64_t frame_rbp = regs->gpr[5];
            uint64_t* slotPtr = reinterpret_cast<uint64_t*>(frame_rbp - slot.location.slot() - 64);
            if (slot.type == JITType::Int32) {
                reconstructed = Value::fromInt32(static_cast<int32_t>(*slotPtr));
            } else if (slot.type == JITType::Double) {
                double d;
                std::memcpy(&d, slotPtr, sizeof(double));
                reconstructed = Value::fromDouble(d);
            } else if (slot.type == JITType::Bool) {
                reconstructed = Value(*slotPtr != 0);
            } else if (slot.type == JITType::TaggedValue) {
                reconstructed = Value::fromRawBits(*slotPtr);
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
                reconstructed = Value::fromRawBits(slot.location.imm64());
            }
        }

        // 写回解释器寄存器数组，安全触发 operator= 和引用计数
        vmRegisters[registerBase + i] = reconstructed;
    }
}

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
        if (slot.location.isInvalid()) continue;
        
        Value reconstructed;

        if (slot.location.isPhysicalGPR()) {
            uint64_t rawVal = regs->gpr[slot.location.pregGPR().id()];
            if (slot.type == JITType::Int32) {
                reconstructed = Value::fromInt32(static_cast<int32_t>(rawVal));
            } else if (slot.type == JITType::Bool) {
                reconstructed = Value(rawVal != 0);
            } else if (slot.type == JITType::TaggedValue) {
                reconstructed = Value::fromRawBits(rawVal);
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
            uint64_t* slotPtr = reinterpret_cast<uint64_t*>(frame_rbp - slot.location.slot() - 64);
            if (slot.type == JITType::Int32) {
                reconstructed = Value::fromInt32(static_cast<int32_t>(*slotPtr));
            } else if (slot.type == JITType::Double) {
                double d;
                std::memcpy(&d, slotPtr, sizeof(double));
                reconstructed = Value::fromDouble(d);
            } else if (slot.type == JITType::Bool) {
                reconstructed = Value(*slotPtr != 0);
            } else if (slot.type == JITType::TaggedValue) {
                reconstructed = Value::fromRawBits(*slotPtr);
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
                reconstructed = Value::fromRawBits(slot.location.imm64());
            }
        }

        // 写回解释器寄存器数组
        vmRegisters[registerBase + i] = reconstructed;
    }

    // 2. 恢复指令指针
    frame->ip = map->bytecodeIp;

    std::string fnName = frame->function ? frame->function->name : "<unknown>";
    std::cout << "[DEBUG] Deoptimized in function '" << fnName << "' at BailoutId: " << bailoutId << ", Bytecode IP: " << map->bytecodeIp << "\n";
    std::cout << "[DEBUG] Registers state upon deoptimization:\n";
    for (size_t i = 0; i < map->locals.size(); ++i) {
        if (!map->locals[i].location.isInvalid()) {
            std::cout << "  R(" << i << ") = " << vmRegisters[registerBase + i] << "\n";
        }
    }

    // 3. 标记去优化状态，让跳板返回到 VM::run
    g_jc2_jit_deoptimized = true;
}

} // namespace jit
} // namespace jc

#endif // JC2_JIT_DEOPTIMIZATION_H
