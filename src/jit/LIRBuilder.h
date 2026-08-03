#ifndef JC2_JIT_LIR_BUILDER_H
#define JC2_JIT_LIR_BUILDER_H

#include "LIR.h"
#include <vector>
#include <memory>
#include <cassert>

namespace jc {
namespace jit {

// ============================================================================
// LIR 图容器 (Step 37)
// 负责统一管理所有 LIR 基本块的生命周期
// ============================================================================
class LIRGraph {
public:
    LIRGraph() = default;
    ~LIRGraph() {
        for (auto block : blocks_) {
            delete block;
        }
    }

    // 禁用拷贝和移动，确保基本块指针的绝对稳定
    LIRGraph(const LIRGraph&) = delete;
    LIRGraph& operator=(const LIRGraph&) = delete;

    LIRBlock* createBlock() {
        uint32_t id = static_cast<uint32_t>(blocks_.size());
        auto block = new LIRBlock(id);
        blocks_.push_back(block);
        return block;
    }

    const std::vector<LIRBlock*>& blocks() const { return blocks_; }

private:
    std::vector<LIRBlock*> blocks_;
};

// ============================================================================
// LIR 构建器 (Step 37)
// 封装基本块管理、指令发射、物理约束注入以及控制流跳转
// ============================================================================
class LIRBuilder {
public:
    explicit LIRBuilder(LIRGraph* graph) 
        : graph_(graph), currentBlock_(nullptr), nextVReg_(0), nextInstId_(0) {}

    // --- 基本块管理 ---
    void setCurrentBlock(LIRBlock* block) { 
        currentBlock_ = block; 
    }
    
    LIRBlock* currentBlock() const { 
        return currentBlock_; 
    }

    // --- 虚拟寄存器分配 ---
    uint32_t allocateVirtualRegister() {
        return nextVReg_++;
    }

    // --- 基础指令发射 ---
    LIRInst* emit(LIROpcode opcode) {
        auto inst = new LIRInst(nextInstId_++, opcode);
        if (currentBlock_) {
            currentBlock_->addInstruction(inst);
        }
        return inst;
    }

    LIRInst* emit(LIROpcode opcode, const std::vector<LIROperand>& defs, const std::vector<LIROperand>& uses) {
        auto inst = emit(opcode);
        for (const auto& def : defs) {
            inst->addDef(def);
        }
        for (const auto& use : uses) {
            inst->addUse(use);
        }
        return inst;
    }

    // --- 物理约束注入 ---
    LIRInst* emitWithConstraints(LIROpcode opcode, 
                                 const std::vector<std::pair<LIROperand, LIRConstraint>>& defs, 
                                 const std::vector<std::pair<LIROperand, LIRConstraint>>& uses) {
        auto inst = emit(opcode);
        for (const auto& def : defs) {
            inst->addDef(def.first, def.second);
        }
        for (const auto& use : uses) {
            inst->addUse(use.first, use.second);
        }
        return inst;
    }

    // --- 控制流跳转 ---
    LIRInst* emitJump(LIRBlock* targetBlock) {
        assert(targetBlock != nullptr);
        auto inst = emit(LIROpcode::Jmp);
        inst->setTarget(targetBlock);
        // 注意：CFG 的前驱/后继关系已由 GCM 阶段构建，这里不再重复添加
        return inst;
    }

    LIRInst* emitCondJump(Condition cond, LIRBlock* trueBlock, LIRBlock* falseBlock) {
        assert(trueBlock != nullptr && falseBlock != nullptr);
        auto inst = emit(LIROpcode::Jcc);
        inst->setCondition(cond);
        inst->setTarget(trueBlock);
        
        // 显式发射 False 分支的无条件跳转
        emitJump(falseBlock);
        
        return inst;
    }

private:
    LIRGraph* graph_;
    LIRBlock* currentBlock_;
    uint32_t nextVReg_;
    uint32_t nextInstId_;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_LIR_BUILDER_H
