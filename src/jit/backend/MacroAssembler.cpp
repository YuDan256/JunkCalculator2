#include "MacroAssembler.h"

namespace jc {
namespace jit {

void MacroAssembler::finalize(ExecutableMemory& execMem) {
    if (buffer_.empty()) {
        return;
    }
    
    // 分配具有 RW 权限的内存页
    execMem.allocate(buffer_.size());
    
    // 将生成的机器码拷贝进去
    std::memcpy(execMem.get(), buffer_.data(), buffer_.size());
    
    // 修改权限为 RX 并刷新指令缓存
    execMem.finalize();
}

} // namespace jit
} // namespace jc
