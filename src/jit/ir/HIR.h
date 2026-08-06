#ifndef JC2_JIT_HIR_H
#define JC2_JIT_HIR_H

#include <vector>
#include <cstdint>
#include <string>
#include <cstdio>

namespace jc {
namespace jit {

// ============================================================================
// JIT 编译器理解的底层数据类型 (Step 27)
// ============================================================================
enum class JITType : uint8_t {
    Control,    // 控制流边 (Control Flow)
    Effect,     // 副作用边 (Memory/State Dependency)
    Int32,      // 原生 32 位整数 (Unboxed)
    Double,     // 原生 64 位浮点数 (Unboxed)
    Bool,       // 原生布尔值 (Unboxed)
    TaggedValue,// JC2 的 64 位 NaN-Boxed Value (Boxed)
    FrameState, // 解释器状态快照 (用于去优化)
    Unknown     // 未知或 Bottom 类型
};

inline std::string to_string(JITType type) {
    switch (type) {
        case JITType::Control: return "Control";
        case JITType::Effect: return "Effect";
        case JITType::Int32: return "Int32";
        case JITType::Double: return "Double";
        case JITType::Bool: return "Bool";
        case JITType::TaggedValue: return "TaggedValue";
        case JITType::FrameState: return "FrameState";
        case JITType::Unknown: return "Unknown";
        default: return "Invalid";
    }
}

// ============================================================================
// HIR 节点操作码 (Step 27)
// ============================================================================
enum class HIROp : uint16_t {
    // ==========================================
    // 控制流节点 (Control Flow)
    // ==========================================
    Start,
    OSREntry,
    Return,
    Branch,
    IfTrue,
    IfFalse,
    Jump,
    Merge,
    LoopBegin,
    LoopEnd,
    Deoptimize, // 显式去优化节点

    // ==========================================
    // 常量节点 (Constants)
    // ==========================================
    Int32Constant,
    Int64Constant,
    DoubleConstant,
    BoolConstant,
    NoneConstant,
    StringConstant,

    // ==========================================
    // 算术与逻辑运算 (Int32 Unboxed)
    // ==========================================
    AddI32,
    SubI32,
    MulI32,
    DivI32,
    IDivI32,
    ModI32,
    BitAndI32,
    BitOrI32,
    BitXorI32,
    ShlI32,
    ShrI32,
    NegI32,
    NotI32,

    // ==========================================
    // 算术运算 (Double Unboxed)
    // ==========================================
    AddF64,
    SubF64,
    MulF64,
    DivF64,
    IDivF64,
    ModF64,
    NegF64,
    SqrtF64,
    SinF64,
    CosF64,
    AbsF64,
    FloorF64,
    CeilF64,
    RoundF64,
    TruncF64,

    // ==========================================
    // 比较运算 (Int32 Unboxed)
    // ==========================================
    CmpEqI32,
    CmpNeqI32,
    CmpLtI32,
    CmpLeI32,
    CmpGtI32,
    CmpGeI32,

    // ==========================================
    // 比较运算 (Double Unboxed)
    // ==========================================
    CmpEqF64,
    CmpNeqF64,
    CmpLtF64,
    CmpLeF64,
    CmpGtF64,
    CmpGeF64,

    // ==========================================
    // 比较运算 (Tagged Value)
    // ==========================================
    CmpEqTagged,
    CmpNeqTagged,

    // ==========================================
    // 守卫与状态 (Guards & State)
    // ==========================================
    FrameState,
    GuardIsInt32,
    GuardIsDouble,
    GuardIsBool,
    GuardIsString,
    GuardIsObject,
    GuardIsClass,
    GuardTruthy,

    // ==========================================
    // 装箱与拆箱 (Boxing / Unboxing)
    // ==========================================
    UnboxInt32,
    BoxInt32,
    UnboxDouble,
    BoxDouble,
    UnboxBool,
    BoxBool,
    Int32ToDouble,

    // ==========================================
    // 内存与对象操作 (Memory & Objects)
    // ==========================================
    LoadGlobal,
    StoreGlobal,
    LoadField,
    StoreField,
    LoadElement,
    StoreElement,
    LoadRegister,
    StoreRegister,

    // ==========================================
    // 函数调用 (Calls)
    // ==========================================
    Call,
    CallNative,
    CallBuiltin,
    Callout,

    // ==========================================
    // SSA 节点
    // ==========================================
    Phi
};

inline std::string to_string(HIROp op) {
    switch (op) {
        case HIROp::Start: return "Start";
        case HIROp::OSREntry: return "OSREntry";
        case HIROp::Return: return "Return";
        case HIROp::Branch: return "Branch";
        case HIROp::IfTrue: return "IfTrue";
        case HIROp::IfFalse: return "IfFalse";
        case HIROp::Jump: return "Jump";
        case HIROp::Merge: return "Merge";
        case HIROp::LoopBegin: return "LoopBegin";
        case HIROp::LoopEnd: return "LoopEnd";
        case HIROp::Deoptimize: return "Deoptimize";
        case HIROp::Int32Constant: return "Int32Constant";
        case HIROp::Int64Constant: return "Int64Constant";
        case HIROp::DoubleConstant: return "DoubleConstant";
        case HIROp::BoolConstant: return "BoolConstant";
        case HIROp::NoneConstant: return "NoneConstant";
        case HIROp::StringConstant: return "StringConstant";
        case HIROp::AddI32: return "AddI32";
        case HIROp::SubI32: return "SubI32";
        case HIROp::MulI32: return "MulI32";
        case HIROp::DivI32: return "DivI32";
        case HIROp::IDivI32: return "IDivI32";
        case HIROp::ModI32: return "ModI32";
        case HIROp::BitAndI32: return "BitAndI32";
        case HIROp::BitOrI32: return "BitOrI32";
        case HIROp::BitXorI32: return "BitXorI32";
        case HIROp::ShlI32: return "ShlI32";
        case HIROp::ShrI32: return "ShrI32";
        case HIROp::NegI32: return "NegI32";
        case HIROp::NotI32: return "NotI32";
        case HIROp::AddF64: return "AddF64";
        case HIROp::SubF64: return "SubF64";
        case HIROp::MulF64: return "MulF64";
        case HIROp::DivF64: return "DivF64";
        case HIROp::IDivF64: return "IDivF64";
        case HIROp::ModF64: return "ModF64";
        case HIROp::NegF64: return "NegF64";
        case HIROp::SqrtF64: return "SqrtF64";
        case HIROp::SinF64: return "SinF64";
        case HIROp::CosF64: return "CosF64";
        case HIROp::AbsF64: return "AbsF64";
        case HIROp::FloorF64: return "FloorF64";
        case HIROp::CeilF64: return "CeilF64";
        case HIROp::RoundF64: return "RoundF64";
        case HIROp::TruncF64: return "TruncF64";
        case HIROp::CmpEqI32: return "CmpEqI32";
        case HIROp::CmpNeqI32: return "CmpNeqI32";
        case HIROp::CmpLtI32: return "CmpLtI32";
        case HIROp::CmpLeI32: return "CmpLeI32";
        case HIROp::CmpGtI32: return "CmpGtI32";
        case HIROp::CmpGeI32: return "CmpGeI32";
        case HIROp::CmpEqF64: return "CmpEqF64";
        case HIROp::CmpNeqF64: return "CmpNeqF64";
        case HIROp::CmpLtF64: return "CmpLtF64";
        case HIROp::CmpLeF64: return "CmpLeF64";
        case HIROp::CmpGtF64: return "CmpGtF64";
        case HIROp::CmpGeF64: return "CmpGeF64";
        case HIROp::CmpEqTagged: return "CmpEqTagged";
        case HIROp::CmpNeqTagged: return "CmpNeqTagged";
        case HIROp::FrameState: return "FrameState";
        case HIROp::GuardIsInt32: return "GuardIsInt32";
        case HIROp::GuardIsDouble: return "GuardIsDouble";
        case HIROp::GuardIsBool: return "GuardIsBool";
        case HIROp::GuardIsString: return "GuardIsString";
        case HIROp::GuardIsObject: return "GuardIsObject";
        case HIROp::GuardIsClass: return "GuardIsClass";
        case HIROp::GuardTruthy: return "GuardTruthy";
        case HIROp::UnboxInt32: return "UnboxInt32";
        case HIROp::BoxInt32: return "BoxInt32";
        case HIROp::UnboxDouble: return "UnboxDouble";
        case HIROp::BoxDouble: return "BoxDouble";
        case HIROp::UnboxBool: return "UnboxBool";
        case HIROp::BoxBool: return "BoxBool";
        case HIROp::Int32ToDouble: return "Int32ToDouble";
        case HIROp::LoadGlobal: return "LoadGlobal";
        case HIROp::StoreGlobal: return "StoreGlobal";
        case HIROp::LoadField: return "LoadField";
        case HIROp::StoreField: return "StoreField";
        case HIROp::LoadElement: return "LoadElement";
        case HIROp::StoreElement: return "StoreElement";
        case HIROp::LoadRegister: return "LoadRegister";
        case HIROp::StoreRegister: return "StoreRegister";
        case HIROp::Call: return "Call";
        case HIROp::CallNative: return "CallNative";
        case HIROp::CallBuiltin: return "CallBuiltin";
        case HIROp::Callout: return "Callout";
        case HIROp::Phi: return "Phi";
        default: return "UnknownOp";
    }
}

// ============================================================================
// HIR 节点基类 (Sea of Nodes) (Step 27)
// ============================================================================
class HIRNode {
public:
    HIRNode(uint32_t id, HIROp op, JITType type)
        : id_(id), opcode_(op), type_(type) {}

    virtual ~HIRNode() = default;

    virtual std::string extraLabel() const { return ""; }

    uint32_t id() const { return id_; }
    HIROp opcode() const { return opcode_; }
    JITType type() const { return type_; }

    const std::vector<HIRNode*>& inputs() const { return inputs_; }
    const std::vector<HIRNode*>& uses() const { return uses_; }

    // ========================================================================
    // Use-Def / Def-Use 链管理 (Step 28)
    // ========================================================================
    
    // 添加一个输入节点，并自动更新对方的 uses 链
    void addInput(HIRNode* node) {
        inputs_.push_back(node);
        if (node) {
            node->addUse(this);
        }
    }

    // 替换指定位置的输入节点，并自动维护新旧节点的 uses 链
    void replaceInput(size_t index, HIRNode* newNode) {
        if (index >= inputs_.size()) return;
        HIRNode* oldNode = inputs_[index];
        if (oldNode == newNode) return;

        if (oldNode) {
            oldNode->removeUse(this);
        }
        inputs_[index] = newNode;
        if (newNode) {
            newNode->addUse(this);
        }
    }

    // 清空所有输入节点，并自动维护 uses 链
    void clearInputs() {
        for (HIRNode* oldNode : inputs_) {
            if (oldNode) {
                oldNode->removeUse(this);
            }
        }
        inputs_.clear();
    }

    // 将所有依赖当前节点的节点，改为依赖 newNode (用于节点替换/优化)
    void replaceUsesWith(HIRNode* newNode) {
        if (this == newNode) return;
        // 拷贝 uses 数组，因为 replaceInput 会在遍历过程中修改 uses_
        std::vector<HIRNode*> currentUses = uses_;
        for (HIRNode* useNode : currentUses) {
            for (size_t i = 0; i < useNode->inputs_.size(); ++i) {
                if (useNode->inputs_[i] == this) {
                    useNode->replaceInput(i, newNode);
                }
            }
        }
    }

protected:
    uint32_t id_;
    HIROp opcode_;
    JITType type_;
    
private:
    std::vector<HIRNode*> inputs_; // Use-Def 链 (当前节点依赖的输入)
    std::vector<HIRNode*> uses_;   // Def-Use 链 (依赖当前节点的其他节点)

    // 仅允许内部或友元方法修改 uses 链
    void addUse(HIRNode* node) {
        uses_.push_back(node);
    }

    void removeUse(HIRNode* node) {
        for (auto it = uses_.begin(); it != uses_.end(); ++it) {
            if (*it == node) {
                uses_.erase(it);
                break;
            }
        }
    }
};

// ============================================================================
// 具体 HIR 节点类派生 (Step 29)
// ============================================================================

// --- 常量节点 ---
class Int32ConstantNode : public HIRNode {
    int32_t value_;
public:
    Int32ConstantNode(uint32_t id, int32_t val)
        : HIRNode(id, HIROp::Int32Constant, JITType::Int32), value_(val) {}
    int32_t value() const { return value_; }
    std::string extraLabel() const override { return std::to_string(value_); }
};

class Int64ConstantNode : public HIRNode {
    uint64_t value_;
public:
    Int64ConstantNode(uint32_t id, uint64_t val)
        : HIRNode(id, HIROp::Int64Constant, JITType::Int32), value_(val) {}
    uint64_t value() const { return value_; }
    std::string extraLabel() const override { return std::to_string(value_); }
};

class DoubleConstantNode : public HIRNode {
    double value_;
public:
    DoubleConstantNode(uint32_t id, double val)
        : HIRNode(id, HIROp::DoubleConstant, JITType::Double), value_(val) {}
    double value() const { return value_; }
    std::string extraLabel() const override { return std::to_string(value_); }
};

class BoolConstantNode : public HIRNode {
    bool value_;
public:
    BoolConstantNode(uint32_t id, bool val)
        : HIRNode(id, HIROp::BoolConstant, JITType::Bool), value_(val) {}
    bool value() const { return value_; }
    std::string extraLabel() const override { return value_ ? "true" : "false"; }
};

class StringConstantNode : public HIRNode {
    std::string value_;
public:
    StringConstantNode(uint32_t id, const std::string& val)
        : HIRNode(id, HIROp::StringConstant, JITType::TaggedValue), value_(val) {}
    const std::string& value() const { return value_; }
    std::string extraLabel() const override { return "\\\"" + value_ + "\\\""; }
};

class NoneConstantNode : public HIRNode {
public:
    NoneConstantNode(uint32_t id)
        : HIRNode(id, HIROp::NoneConstant, JITType::TaggedValue) {}
};

// --- 状态与去优化节点 ---
class FrameStateNode : public HIRNode {
    uint32_t bailoutId_;
    uint32_t ip_;
public:
    FrameStateNode(uint32_t id, uint32_t bailoutId, uint32_t ip)
        : HIRNode(id, HIROp::FrameState, JITType::FrameState), bailoutId_(bailoutId), ip_(ip) {}
    
    uint32_t bailoutId() const { return bailoutId_; }
    uint32_t ip() const { return ip_; }
    
    // FrameState 的 inputs_ 存储了当前所有存活的虚拟寄存器对应的 HIRNode
};

class GuardNode : public HIRNode {
public:
    GuardNode(uint32_t id, HIROp op, HIRNode* control, HIRNode* effect, HIRNode* value, FrameStateNode* frameState)
        : HIRNode(id, op, JITType::Control) {
        addInput(control);
        addInput(effect);
        addInput(value);
        addInput(frameState);
    }
    
    HIRNode* control() const { return inputs()[0]; }
    HIRNode* effect() const { return inputs()[1]; }
    HIRNode* value() const { return inputs()[2]; }
    FrameStateNode* frameState() const { return static_cast<FrameStateNode*>(inputs()[3]); }
};

class GuardIsClassNode : public GuardNode {
    uint64_t classId_;
public:
    GuardIsClassNode(uint32_t id, HIRNode* control, HIRNode* effect, HIRNode* value, FrameStateNode* frameState, uint64_t classId)
        : GuardNode(id, HIROp::GuardIsClass, control, effect, value, frameState), classId_(classId) {}
    
    uint64_t classId() const { return classId_; }
    std::string extraLabel() const override { return "classId:" + std::to_string(classId_); }
};

// --- 算术与逻辑节点 (双操作数) ---
class BinaryOpNode : public HIRNode {
public:
    BinaryOpNode(uint32_t id, HIROp op, JITType type, HIRNode* lhs, HIRNode* rhs, FrameStateNode* fs = nullptr)
        : HIRNode(id, op, type) {
        addInput(lhs);
        addInput(rhs);
        if (fs) addInput(fs);
    }
    
    HIRNode* lhs() const { return inputs()[0]; }
    HIRNode* rhs() const { return inputs()[1]; }
    FrameStateNode* frameState() const { return inputs().size() > 2 ? static_cast<FrameStateNode*>(inputs()[2]) : nullptr; }
};

// --- 内存访问节点 ---
class RegisterAccessNode : public HIRNode {
    int regIndex_;
public:
    RegisterAccessNode(uint32_t id, HIROp op, JITType type, HIRNode* control, HIRNode* effect, int regIndex)
        : HIRNode(id, op, type), regIndex_(regIndex) {
        addInput(control);
        addInput(effect);
    }
    
    int regIndex() const { return regIndex_; }
    HIRNode* control() const { return inputs()[0]; }
    HIRNode* effect() const { return inputs()[1]; }
};

class GlobalAccessNode : public HIRNode {
    int slot_;
public:
    GlobalAccessNode(uint32_t id, HIROp op, JITType type, HIRNode* control, HIRNode* effect, int slot)
        : HIRNode(id, op, type), slot_(slot) {
        addInput(control);
        addInput(effect);
    }
    
    int slot() const { return slot_; }
    HIRNode* control() const { return inputs()[0]; }
    HIRNode* effect() const { return inputs()[1]; }
};

// --- 函数调用节点 ---
class CallNode : public HIRNode {
    uint32_t argc_;
public:
    CallNode(uint32_t id, HIROp op, JITType type, HIRNode* control, HIRNode* effect, HIRNode* callee, uint32_t argc, FrameStateNode* frameState)
        : HIRNode(id, op, type), argc_(argc) {
        addInput(control);
        addInput(effect);
        addInput(callee);
        addInput(frameState);
        // 后续的参数通过 addInput 追加
    }
    
    uint32_t argc() const { return argc_; }
    HIRNode* control() const { return inputs()[0]; }
    HIRNode* effect() const { return inputs()[1]; }
    HIRNode* callee() const { return inputs()[2]; }
    FrameStateNode* frameState() const { return static_cast<FrameStateNode*>(inputs()[3]); }
    HIRNode* arg(uint32_t index) const { return inputs()[4 + index]; }
};

class CalloutNode : public HIRNode {
    void* functionPtr_;
    uint32_t argc_;
public:
    CalloutNode(uint32_t id, JITType type, HIRNode* control, HIRNode* effect, void* functionPtr, uint32_t argc, FrameStateNode* frameState)
        : HIRNode(id, HIROp::Callout, type), functionPtr_(functionPtr), argc_(argc) {
        addInput(control);
        addInput(effect);
        addInput(frameState);
        // 后续的参数通过 addInput 追加
    }
    
    void* functionPtr() const { return functionPtr_; }
    uint32_t argc() const { return argc_; }
    HIRNode* control() const { return inputs()[0]; }
    HIRNode* effect() const { return inputs()[1]; }
    FrameStateNode* frameState() const { return static_cast<FrameStateNode*>(inputs()[2]); }
    HIRNode* arg(uint32_t index) const { return inputs()[3 + index]; }
    
    std::string extraLabel() const override {
        char buf[32];
        snprintf(buf, sizeof(buf), "%p", functionPtr_);
        return std::string("fn:") + buf;
    }
};

// --- 控制流节点 ---
class OSREntryNode : public HIRNode {
    int loopHeaderIp_;
public:
    OSREntryNode(uint32_t id, int loopHeaderIp)
        : HIRNode(id, HIROp::OSREntry, JITType::Control), loopHeaderIp_(loopHeaderIp) {}
    
    int loopHeaderIp() const { return loopHeaderIp_; }
    std::string extraLabel() const override { return "ip:" + std::to_string(loopHeaderIp_); }
};

class BranchNode : public HIRNode {
public:
    BranchNode(uint32_t id, HIRNode* control, HIRNode* condition)
        : HIRNode(id, HIROp::Branch, JITType::Control) {
        addInput(control);
        addInput(condition);
    }
    
    HIRNode* control() const { return inputs()[0]; }
    HIRNode* condition() const { return inputs()[1]; }
};

class IfNode : public HIRNode {
public:
    IfNode(uint32_t id, HIROp op, BranchNode* branch)
        : HIRNode(id, op, JITType::Control) {
        addInput(branch);
    }
    
    BranchNode* branch() const { return static_cast<BranchNode*>(inputs()[0]); }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_HIR_H
