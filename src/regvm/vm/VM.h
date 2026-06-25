#ifndef JC2_REGVM_VM_H
#define JC2_REGVM_VM_H

#include "Bytecode.h"
#include "../../memory/Value.h"
#include <vector>
#include <unordered_map>

namespace jc {
namespace regvm {

// ============================================================================
// 寄存器机调用帧 (Register Window Frame)
// ============================================================================
struct CallFrame {
    const Chunk* chunk = nullptr;
    int ip = 0;
    int registerBase = 0; // 寄存器窗口基址 (指向全局 registers 数组)
    // ObjClosure* closure = nullptr; // 后续添加闭包支持
};

// ============================================================================
// 寄存器虚拟机核心引擎
// ============================================================================
class VM {
private:
    // 统一地址空间：100万个槽位，足以容纳极深的调用栈和海量的溢出槽
    static constexpr int MAX_REGISTERS = 1024 * 1024; 
    static constexpr int MAX_FRAMES = 1024;

    Value* registers = nullptr;
    CallFrame* frames = nullptr;
    int frameCount = 0;

    std::vector<Value> globals;
    std::unordered_map<std::string, uint32_t> globalNames;

    Value run();

public:
    VM();
    ~VM();

    Value execute(const Chunk& mainChunk);
};

} // namespace regvm
} // namespace jc

#endif // JC2_REGVM_VM_H
