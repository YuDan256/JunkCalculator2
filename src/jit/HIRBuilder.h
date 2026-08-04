#ifndef JC2_JIT_HIR_BUILDER_H
#define JC2_JIT_HIR_BUILDER_H

#include "HIR.h"
#include <memory>
#include <vector>
#include <cassert>
#include <iostream>

namespace jc {
namespace jit {

// ============================================================================
// HIR 图容器 (Step 30)
// 负责统一管理所有 HIR 节点的生命周期
// ============================================================================
class HIRGraph {
public:
    HIRGraph() = default;
    ~HIRGraph() {
        for (auto node : nodes_) {
            delete node;
        }
    }

    // 禁用拷贝和移动，确保节点指针的绝对稳定
    HIRGraph(const HIRGraph&) = delete;
    HIRGraph& operator=(const HIRGraph&) = delete;

    template <typename T, typename... Args>
    T* allocateNode(Args&&... args) {
        uint32_t id = static_cast<uint32_t>(nodes_.size());
        T* node = new T(id, std::forward<Args>(args)...);
        nodes_.push_back(node);
        return node;
    }

    const std::vector<HIRNode*>& nodes() const { return nodes_; }

    void printDOT(std::ostream& os) const {
        os << "digraph HIR {\n";
        os << "  node [shape=record, fontname=\"Courier New\"];\n";
        for (auto node : nodes_) {
            std::string extra = node->extraLabel();
            if (!extra.empty()) {
                os << "  node" << node->id() << " [label=\"{ #" << node->id() << " " << to_string(node->opcode()) << " | " << extra << " | " << to_string(node->type()) << " }\"];\n";
            } else {
                os << "  node" << node->id() << " [label=\"{ #" << node->id() << " " << to_string(node->opcode()) << " | " << to_string(node->type()) << " }\"];\n";
            }
            for (size_t i = 0; i < node->inputs().size(); ++i) {
                auto input = node->inputs()[i];
                if (input) {
                    os << "  node" << input->id() << " -> node" << node->id() << " [label=\"in" << i << "\"];\n";
                }
            }
        }
        os << "}\n";
    }

private:
    std::vector<HIRNode*> nodes_;
};

// ============================================================================
// HIR 构建器 (Step 30)
// 封装节点创建、连边逻辑、状态快照以及图的修改
// ============================================================================
class HIRBuilder {
public:
    explicit HIRBuilder(HIRGraph* graph) 
        : graph_(graph), currentControl_(nullptr), currentEffect_(nullptr) {
        // 预分配 256 个虚拟寄存器槽位，用于模拟解释器状态
        registers_.resize(256, nullptr);
    }

    // --- 状态环境管理 ---
    void setLocal(size_t index, HIRNode* node) {
        if (index < registers_.size()) {
            registers_[index] = node;
        }
    }

    HIRNode* getLocal(size_t index) const {
        return index < registers_.size() ? registers_[index] : nullptr;
    }

    void setCurrentControl(HIRNode* control) { currentControl_ = control; }
    HIRNode* currentControl() const { return currentControl_; }

    void setCurrentEffect(HIRNode* effect) { currentEffect_ = effect; }
    HIRNode* currentEffect() const { return currentEffect_; }

    // --- 基础控制流节点 ---
    HIRNode* createStart() {
        auto node = graph_->allocateNode<HIRNode>(HIROp::Start, JITType::Control);
        currentControl_ = node;
        currentEffect_ = node;
        return node;
    }

    HIRNode* createReturn(HIRNode* value) {
        auto ret = graph_->allocateNode<HIRNode>(HIROp::Return, JITType::Control);
        ret->addInput(currentControl_);
        ret->addInput(currentEffect_);
        ret->addInput(value);
        currentControl_ = nullptr; // 控制流在此终止
        return ret;
    }

    HIRNode* createJump(HIRNode* targetBlock) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::Jump, JITType::Control);
        node->addInput(currentControl_);
        if (targetBlock) node->addInput(targetBlock);
        currentControl_ = nullptr;
        return node;
    }

    HIRNode* createMerge(const std::vector<HIRNode*>& controls) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::Merge, JITType::Control);
        for (auto c : controls) node->addInput(c);
        currentControl_ = node;
        return node;
    }

    HIRNode* createLoopBegin(const std::vector<HIRNode*>& controls) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::LoopBegin, JITType::Control);
        for (auto c : controls) node->addInput(c);
        currentControl_ = node;
        return node;
    }

    HIRNode* createLoopEnd(HIRNode* loopBegin) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::LoopEnd, JITType::Control);
        node->addInput(currentControl_);
        node->addInput(loopBegin);
        currentControl_ = nullptr;
        return node;
    }

    HIRNode* createDeoptimize(FrameStateNode* frameState) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::Deoptimize, JITType::Control);
        node->addInput(currentControl_);
        node->addInput(currentEffect_);
        node->addInput(frameState);
        currentControl_ = nullptr;
        return node;
    }

    // --- 常量节点 ---
    Int32ConstantNode* createInt32Constant(int32_t val) {
        return graph_->allocateNode<Int32ConstantNode>(val);
    }

    DoubleConstantNode* createDoubleConstant(double val) {
        return graph_->allocateNode<DoubleConstantNode>(val);
    }

    BoolConstantNode* createBoolConstant(bool val) {
        return graph_->allocateNode<BoolConstantNode>(val);
    }

    StringConstantNode* createStringConstant(const std::string& val) {
        return graph_->allocateNode<StringConstantNode>(val);
    }

    NoneConstantNode* createNoneConstant() {
        return graph_->allocateNode<NoneConstantNode>();
    }

    // --- 算术与逻辑节点 ---
    BinaryOpNode* createAddI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::AddI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createSubI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::SubI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createMulI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::MulI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createDivI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::DivI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createIDivI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::IDivI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createAddF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::AddF64, JITType::Double, lhs, rhs);
    }

    BinaryOpNode* createSubF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::SubF64, JITType::Double, lhs, rhs);
    }

    BinaryOpNode* createMulF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::MulF64, JITType::Double, lhs, rhs);
    }

    BinaryOpNode* createDivF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::DivF64, JITType::Double, lhs, rhs);
    }

    BinaryOpNode* createIDivF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::IDivF64, JITType::Double, lhs, rhs);
    }

    BinaryOpNode* createModI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::ModI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createBitAndI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::BitAndI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createBitOrI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::BitOrI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createBitXorI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::BitXorI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createShlI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::ShlI32, JITType::Int32, lhs, rhs);
    }

    BinaryOpNode* createShrI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::ShrI32, JITType::Int32, lhs, rhs);
    }

    HIRNode* createNegI32(HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::NegI32, JITType::Int32);
        node->addInput(value);
        return node;
    }

    HIRNode* createNotI32(HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::NotI32, JITType::Int32);
        node->addInput(value);
        return node;
    }

    BinaryOpNode* createModF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::ModF64, JITType::Double, lhs, rhs);
    }

    HIRNode* createNegF64(HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::NegF64, JITType::Double);
        node->addInput(value);
        return node;
    }

    // --- 比较节点 ---
    BinaryOpNode* createCmpEqI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpEqI32, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpLtI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpLtI32, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpNeqI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpNeqI32, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpLeI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpLeI32, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpGtI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpGtI32, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpGeI32(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpGeI32, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpEqF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpEqF64, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpNeqF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpNeqF64, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpLtF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpLtF64, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpLeF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpLeF64, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpGtF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpGtF64, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpGeF64(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpGeF64, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpEqTagged(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpEqTagged, JITType::Bool, lhs, rhs);
    }

    BinaryOpNode* createCmpNeqTagged(HIRNode* lhs, HIRNode* rhs) {
        return graph_->allocateNode<BinaryOpNode>(HIROp::CmpNeqTagged, JITType::Bool, lhs, rhs);
    }

    // --- 状态与守卫节点 ---
    FrameStateNode* captureFrameState(uint32_t bailoutId, uint32_t ip) {
        auto fs = graph_->allocateNode<FrameStateNode>(bailoutId, ip);
        // 自动捕获当前所有虚拟寄存器的状态
        for (HIRNode* reg : registers_) {
            fs->addInput(reg);
        }
        return fs;
    }

    GuardNode* createGuardIsInt32(HIRNode* value, FrameStateNode* frameState) {
        auto guard = graph_->allocateNode<GuardNode>(HIROp::GuardIsInt32, currentControl_, currentEffect_, value, frameState);
        currentControl_ = guard;
        currentEffect_ = guard;
        return guard;
    }

    GuardNode* createGuardIsDouble(HIRNode* value, FrameStateNode* frameState) {
        auto guard = graph_->allocateNode<GuardNode>(HIROp::GuardIsDouble, currentControl_, currentEffect_, value, frameState);
        currentControl_ = guard;
        currentEffect_ = guard;
        return guard;
    }

    GuardNode* createGuardIsBool(HIRNode* value, FrameStateNode* frameState) {
        auto guard = graph_->allocateNode<GuardNode>(HIROp::GuardIsBool, currentControl_, currentEffect_, value, frameState);
        currentControl_ = guard;
        currentEffect_ = guard;
        return guard;
    }

    GuardNode* createGuardIsString(HIRNode* value, FrameStateNode* frameState) {
        auto guard = graph_->allocateNode<GuardNode>(HIROp::GuardIsString, currentControl_, currentEffect_, value, frameState);
        currentControl_ = guard;
        currentEffect_ = guard;
        return guard;
    }

    GuardNode* createGuardIsObject(HIRNode* value, FrameStateNode* frameState) {
        auto guard = graph_->allocateNode<GuardNode>(HIROp::GuardIsObject, currentControl_, currentEffect_, value, frameState);
        currentControl_ = guard;
        currentEffect_ = guard;
        return guard;
    }

    GuardNode* createGuardTruthy(HIRNode* value, FrameStateNode* frameState) {
        auto guard = graph_->allocateNode<GuardNode>(HIROp::GuardTruthy, currentControl_, currentEffect_, value, frameState);
        currentControl_ = guard;
        currentEffect_ = guard;
        return guard;
    }

    // --- 装箱与拆箱节点 ---
    HIRNode* createUnboxInt32(HIRNode* value, HIRNode* guard) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::UnboxInt32, JITType::Int32);
        node->addInput(guard); // Control/Effect dependency
        node->addInput(value); // Data dependency
        return node;
    }

    HIRNode* createBoxInt32(HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::BoxInt32, JITType::TaggedValue);
        node->addInput(value);
        return node;
    }

    HIRNode* createUnboxDouble(HIRNode* value, HIRNode* guard) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::UnboxDouble, JITType::Double);
        node->addInput(guard);
        node->addInput(value);
        return node;
    }

    HIRNode* createBoxDouble(HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::BoxDouble, JITType::TaggedValue);
        node->addInput(value);
        return node;
    }

    HIRNode* createUnboxBool(HIRNode* value, HIRNode* guard) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::UnboxBool, JITType::Bool);
        node->addInput(guard);
        node->addInput(value);
        return node;
    }

    HIRNode* createBoxBool(HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::BoxBool, JITType::TaggedValue);
        node->addInput(value);
        return node;
    }

    // --- 内存与对象操作 ---
    RegisterAccessNode* createLoadRegister(int regIndex) {
        auto node = graph_->allocateNode<RegisterAccessNode>(HIROp::LoadRegister, JITType::TaggedValue, currentControl_, currentEffect_, regIndex);
        currentEffect_ = node;
        return node;
    }

    RegisterAccessNode* createStoreRegister(int regIndex, HIRNode* value) {
        auto node = graph_->allocateNode<RegisterAccessNode>(HIROp::StoreRegister, JITType::Effect, currentControl_, currentEffect_, regIndex);
        node->addInput(value);
        currentEffect_ = node;
        return node;
    }

    GlobalAccessNode* createLoadGlobal(int slot) {
        auto node = graph_->allocateNode<GlobalAccessNode>(HIROp::LoadGlobal, JITType::TaggedValue, currentControl_, currentEffect_, slot);
        currentEffect_ = node;
        return node;
    }

    GlobalAccessNode* createStoreGlobal(int slot, HIRNode* value) {
        auto node = graph_->allocateNode<GlobalAccessNode>(HIROp::StoreGlobal, JITType::Effect, currentControl_, currentEffect_, slot);
        node->addInput(value);
        currentEffect_ = node;
        return node;
    }

    HIRNode* createLoadField(HIRNode* object, HIRNode* offset) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::LoadField, JITType::TaggedValue);
        node->addInput(currentControl_);
        node->addInput(currentEffect_);
        node->addInput(object);
        node->addInput(offset);
        currentEffect_ = node;
        return node;
    }

    HIRNode* createStoreField(HIRNode* object, HIRNode* offset, HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::StoreField, JITType::Effect);
        node->addInput(currentControl_);
        node->addInput(currentEffect_);
        node->addInput(object);
        node->addInput(offset);
        node->addInput(value);
        currentEffect_ = node;
        return node;
    }

    HIRNode* createLoadElement(HIRNode* array, HIRNode* index) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::LoadElement, JITType::TaggedValue);
        node->addInput(currentControl_);
        node->addInput(currentEffect_);
        node->addInput(array);
        node->addInput(index);
        currentEffect_ = node;
        return node;
    }

    HIRNode* createStoreElement(HIRNode* array, HIRNode* index, HIRNode* value) {
        auto node = graph_->allocateNode<HIRNode>(HIROp::StoreElement, JITType::Effect);
        node->addInput(currentControl_);
        node->addInput(currentEffect_);
        node->addInput(array);
        node->addInput(index);
        node->addInput(value);
        currentEffect_ = node;
        return node;
    }

    // --- 函数调用 ---
    CallNode* createCall(HIRNode* callee, uint32_t argc, const std::vector<HIRNode*>& args) {
        auto node = graph_->allocateNode<CallNode>(HIROp::Call, JITType::TaggedValue, currentControl_, currentEffect_, callee, argc);
        for (auto arg : args) node->addInput(arg);
        currentEffect_ = node;
        return node;
    }

    CallNode* createCallNative(HIRNode* callee, uint32_t argc, const std::vector<HIRNode*>& args) {
        auto node = graph_->allocateNode<CallNode>(HIROp::CallNative, JITType::TaggedValue, currentControl_, currentEffect_, callee, argc);
        for (auto arg : args) node->addInput(arg);
        currentEffect_ = node;
        return node;
    }

    CallNode* createCallBuiltin(HIRNode* callee, uint32_t argc, const std::vector<HIRNode*>& args) {
        auto node = graph_->allocateNode<CallNode>(HIROp::CallBuiltin, JITType::TaggedValue, currentControl_, currentEffect_, callee, argc);
        for (auto arg : args) node->addInput(arg);
        currentEffect_ = node;
        return node;
    }

    // --- 分支与控制流 ---
    BranchNode* createBranch(HIRNode* condition) {
        auto branch = graph_->allocateNode<BranchNode>(currentControl_, condition);
        currentControl_ = nullptr; // 控制流在此分叉，后续必须绑定到 IfTrue 或 IfFalse
        return branch;
    }

    IfNode* createIfTrue(BranchNode* branch) {
        return graph_->allocateNode<IfNode>(HIROp::IfTrue, branch);
    }

    IfNode* createIfFalse(BranchNode* branch) {
        return graph_->allocateNode<IfNode>(HIROp::IfFalse, branch);
    }

    HIRNode* createPhi(JITType type, const std::vector<HIRNode*>& inputs) {
        auto phi = graph_->allocateNode<HIRNode>(HIROp::Phi, type);
        phi->addInput(currentControl_); // Phi 的第一个输入通常是控制流汇合点 (Merge)
        for (HIRNode* input : inputs) {
            phi->addInput(input);
        }
        return phi;
    }

    // --- 图修改与优化 API ---
    void replaceNode(HIRNode* oldNode, HIRNode* newNode) {
        oldNode->replaceUsesWith(newNode);
    }

    void killNode(HIRNode* node) {
        // 断开该节点对其输入节点的依赖，使其成为死节点
        for (size_t i = 0; i < node->inputs().size(); ++i) {
            node->replaceInput(i, nullptr);
        }
    }

private:
    HIRGraph* graph_;
    HIRNode* currentControl_;
    HIRNode* currentEffect_;
    std::vector<HIRNode*> registers_; // 模拟解释器的虚拟寄存器状态
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_HIR_BUILDER_H
