#ifndef JC2_JIT_LIR_H
#define JC2_JIT_LIR_H

#include <cstdint>
#include <vector>
#include <string>
#include <stdexcept>
#include <unordered_set>
#include "../backend/Registers.h"
#include "../backend/MacroAssembler.h" // For Condition
#include "HIR.h" // For JITType

namespace jc {
namespace jit {

// ============================================================================
// LIR 操作数类型 (Step 36)
// ============================================================================
enum class LIROperandType : uint8_t {
    Invalid,
    VirtualReg,   // 虚拟寄存器 (等待分配)
    PhysicalGPR,  // 物理通用寄存器 (如 RAX, RCX)
    PhysicalXMM,  // 物理浮点寄存器 (如 XMM0)
    StackSlot,    // 机器栈槽位 (用于溢出或去优化)
    Memory,       // 内存寻址 [Base + Index*Scale + Disp]
    Imm32,        // 32 位立即数
    Imm64         // 64 位立即数
};

// ============================================================================
// LIR 寄存器约束 (Step 36 补充)
// ============================================================================
enum class LIRConstraintType : uint8_t {
    None,           // 无约束 (可以是寄存器或栈槽)
    AnyReg,         // 必须分配到任意物理寄存器
    FixedReg,       // 必须分配到指定的物理寄存器
    SameAsInput     // 输出必须与指定的输入使用相同的物理寄存器 (x86 常见，如 ADD)
};

struct LIRConstraint {
    LIRConstraintType type = LIRConstraintType::None;
    uint8_t value = 255; // FixedReg 的寄存器 ID，或 SameAsInput 的输入索引

    static LIRConstraint none() { return {LIRConstraintType::None, 255}; }
    static LIRConstraint anyReg() { return {LIRConstraintType::AnyReg, 255}; }
    static LIRConstraint fixedReg(uint8_t regId) { return {LIRConstraintType::FixedReg, regId}; }
    static LIRConstraint sameAsInput(uint8_t inputIdx) { return {LIRConstraintType::SameAsInput, inputIdx}; }
};

// ============================================================================
// LIR 操作数 (Step 36)
// ============================================================================
class LIROperand {
public:
    LIROperand() : type_(LIROperandType::Invalid) { value_.imm64_ = 0; }

    static LIROperand createVirtual(uint32_t vreg) {
        LIROperand op; op.type_ = LIROperandType::VirtualReg; op.value_.vreg_ = vreg; return op;
    }
    static LIROperand createPhysicalGPR(Register reg) {
        LIROperand op; op.type_ = LIROperandType::PhysicalGPR; op.value_.preg_ = reg.id(); return op;
    }
    static LIROperand createPhysicalXMM(XMMRegister reg) {
        LIROperand op; op.type_ = LIROperandType::PhysicalXMM; op.value_.preg_ = reg.id(); return op;
    }
    static LIROperand createStackSlot(int32_t offset) {
        LIROperand op; op.type_ = LIROperandType::StackSlot; op.value_.slot_ = offset; return op;
    }
    static LIROperand createMemory(const Operand& mem) {
        LIROperand op; op.type_ = LIROperandType::Memory; op.mem_ = mem; return op;
    }
    static LIROperand createImm32(int32_t imm) {
        LIROperand op; op.type_ = LIROperandType::Imm32; op.value_.imm32_ = imm; return op;
    }
    static LIROperand createImm64(uint64_t imm) {
        LIROperand op; op.type_ = LIROperandType::Imm64; op.value_.imm64_ = imm; return op;
    }

    LIROperandType type() const { return type_; }
    bool isInvalid() const { return type_ == LIROperandType::Invalid; }
    bool isVirtual() const { return type_ == LIROperandType::VirtualReg; }
    bool isPhysicalGPR() const { return type_ == LIROperandType::PhysicalGPR; }
    bool isPhysicalXMM() const { return type_ == LIROperandType::PhysicalXMM; }
    bool isStackSlot() const { return type_ == LIROperandType::StackSlot; }
    bool isMemory() const { return type_ == LIROperandType::Memory; }
    bool isImm32() const { return type_ == LIROperandType::Imm32; }
    bool isImm64() const { return type_ == LIROperandType::Imm64; }

    uint32_t vreg() const { return value_.vreg_; }
    Register pregGPR() const { return Register(value_.preg_); }
    XMMRegister pregXMM() const { return XMMRegister(value_.preg_); }
    int32_t slot() const { return value_.slot_; }
    Operand memory() const { return mem_; }
    int32_t imm32() const { return value_.imm32_; }
    uint64_t imm64() const { return value_.imm64_; }

    bool operator==(const LIROperand& other) const {
        if (type_ != other.type_) return false;
        if (type_ == LIROperandType::Memory) {
            return mem_.base() == other.mem_.base() &&
                   mem_.index() == other.mem_.index() &&
                   mem_.scale() == other.mem_.scale() &&
                   mem_.disp() == other.mem_.disp();
        }
        return value_.imm64_ == other.value_.imm64_;
    }
    bool operator!=(const LIROperand& other) const { return !(*this == other); }

    std::string toString() const {
        switch (type_) {
            case LIROperandType::Invalid: return "Invalid";
            case LIROperandType::VirtualReg: return "v" + std::to_string(value_.vreg_);
            case LIROperandType::PhysicalGPR: return "r" + std::to_string(value_.preg_);
            case LIROperandType::PhysicalXMM: return "xmm" + std::to_string(value_.preg_);
            case LIROperandType::StackSlot: return "[sp+" + std::to_string(value_.slot_) + "]";
            case LIROperandType::Memory: return "[mem]";
            case LIROperandType::Imm32: return std::to_string(value_.imm32_);
            case LIROperandType::Imm64: return std::to_string(value_.imm64_);
            default: return "?";
        }
    }

private:
    LIROperandType type_;
    union {
        uint32_t vreg_;
        uint8_t preg_;
        int32_t slot_;
        int32_t imm32_;
        uint64_t imm64_;
    } value_;
    Operand mem_;
};

// ============================================================================
// LIR 操作码 (Step 36)
// ============================================================================
enum class LIROpcode : uint16_t {
    Label,
    Move,
    ParallelMove,
    LoadImm32,
    LoadImm64,
    AddI32, SubI32, MulI32, DivI32, IDivI32, ModI32,
    AndI32, OrI32, XorI32, ShlI32, ShrI32, SarI32,
    NegI32, NotI32,
    AddF64, SubF64, MulF64, DivF64, IDivF64, ModF64,
    NegF64,
    SqrtF64, SinF64, CosF64, AbsF64, FloorF64, CeilF64, RoundF64, TruncF64,
    CmpI32, Cmp64, TestI32, CmpF64,
    Setcc,
    Jmp, Jcc,
    Call, Callout, Ret,
    Deoptimize,
    BoxInt32, BoxDouble, BoxBool,
    UnboxInt32, UnboxDouble, UnboxBool,
    Int32ToDouble,
    LoadGlobal,
    LoadField, StoreField,
    GuardIsInt32, GuardIsDouble, GuardIsBool, GuardIsString, GuardIsObject, GuardTruthy,
    GuardIsClass
};

inline std::string to_string(LIROpcode op) {
    switch (op) {
        case LIROpcode::Label: return "Label";
        case LIROpcode::Move: return "Move";
        case LIROpcode::ParallelMove: return "ParallelMove";
        case LIROpcode::LoadImm32: return "LoadImm32";
        case LIROpcode::LoadImm64: return "LoadImm64";
        case LIROpcode::AddI32: return "AddI32";
        case LIROpcode::SubI32: return "SubI32";
        case LIROpcode::MulI32: return "MulI32";
        case LIROpcode::DivI32: return "DivI32";
        case LIROpcode::IDivI32: return "IDivI32";
        case LIROpcode::ModI32: return "ModI32";
        case LIROpcode::AndI32: return "AndI32";
        case LIROpcode::OrI32: return "OrI32";
        case LIROpcode::XorI32: return "XorI32";
        case LIROpcode::ShlI32: return "ShlI32";
        case LIROpcode::ShrI32: return "ShrI32";
        case LIROpcode::SarI32: return "SarI32";
        case LIROpcode::NegI32: return "NegI32";
        case LIROpcode::NotI32: return "NotI32";
        case LIROpcode::AddF64: return "AddF64";
        case LIROpcode::SubF64: return "SubF64";
        case LIROpcode::MulF64: return "MulF64";
        case LIROpcode::DivF64: return "DivF64";
        case LIROpcode::IDivF64: return "IDivF64";
        case LIROpcode::ModF64: return "ModF64";
        case LIROpcode::NegF64: return "NegF64";
        case LIROpcode::SqrtF64: return "SqrtF64";
        case LIROpcode::SinF64: return "SinF64";
        case LIROpcode::CosF64: return "CosF64";
        case LIROpcode::AbsF64: return "AbsF64";
        case LIROpcode::FloorF64: return "FloorF64";
        case LIROpcode::CeilF64: return "CeilF64";
        case LIROpcode::RoundF64: return "RoundF64";
        case LIROpcode::TruncF64: return "TruncF64";
        case LIROpcode::CmpI32: return "CmpI32";
        case LIROpcode::Cmp64: return "Cmp64";
        case LIROpcode::TestI32: return "TestI32";
        case LIROpcode::CmpF64: return "CmpF64";
        case LIROpcode::Setcc: return "Setcc";
        case LIROpcode::Jmp: return "Jmp";
        case LIROpcode::Jcc: return "Jcc";
        case LIROpcode::Call: return "Call";
        case LIROpcode::Callout: return "Callout";
        case LIROpcode::Ret: return "Ret";
        case LIROpcode::Deoptimize: return "Deoptimize";
        case LIROpcode::BoxInt32: return "BoxInt32";
        case LIROpcode::BoxDouble: return "BoxDouble";
        case LIROpcode::BoxBool: return "BoxBool";
        case LIROpcode::UnboxInt32: return "UnboxInt32";
        case LIROpcode::UnboxDouble: return "UnboxDouble";
        case LIROpcode::UnboxBool: return "UnboxBool";
        case LIROpcode::Int32ToDouble: return "Int32ToDouble";
        case LIROpcode::LoadGlobal: return "LoadGlobal";
        case LIROpcode::LoadField: return "LoadField";
        case LIROpcode::StoreField: return "StoreField";
        case LIROpcode::GuardIsInt32: return "GuardIsInt32";
        case LIROpcode::GuardIsDouble: return "GuardIsDouble";
        case LIROpcode::GuardIsBool: return "GuardIsBool";
        case LIROpcode::GuardIsString: return "GuardIsString";
        case LIROpcode::GuardIsObject: return "GuardIsObject";
        case LIROpcode::GuardTruthy: return "GuardTruthy";
        case LIROpcode::GuardIsClass: return "GuardIsClass";
        default: return "Unknown";
    }
}

// ============================================================================
// LIR 指令 (Step 36)
// ============================================================================
class LIRInst {
public:
    LIRInst(uint32_t id, LIROpcode op) : id_(id), opcode_(op) {}

    uint32_t id() const { return id_; }
    LIROpcode opcode() const { return opcode_; }

    void addDef(const LIROperand& op, LIRConstraint constraint = LIRConstraint::none()) { 
        defs_.push_back(op); 
        defConstraints_.push_back(constraint);
    }
    void addUse(const LIROperand& op, LIRConstraint constraint = LIRConstraint::none()) { 
        uses_.push_back(op); 
        useConstraints_.push_back(constraint);
    }
    void addClobber(Register reg) {
        clobbers_.push_back(reg);
    }

    const std::vector<LIROperand>& defs() const { return defs_; }
    const std::vector<LIROperand>& uses() const { return uses_; }
    const std::vector<LIRConstraint>& defConstraints() const { return defConstraints_; }
    const std::vector<LIRConstraint>& useConstraints() const { return useConstraints_; }
    const std::vector<Register>& clobbers() const { return clobbers_; }
    
    std::vector<LIROperand>& defsMut() { return defs_; }
    std::vector<LIROperand>& usesMut() { return uses_; }

    // 供条件跳转 (Jcc) 和 Setcc 使用
    void setCondition(Condition cond) { cond_ = cond; }
    Condition condition() const { return cond_; }

    // 供跳转指令 (Jmp/Jcc) 使用
    void setTarget(class LIRBlock* target) { target_ = target; }
    class LIRBlock* target() const { return target_; }

    // 供 Deoptimize 使用
    void setBailoutId(uint32_t id, uint32_t ip) { bailoutId_ = id; bytecodeIp_ = ip; hasBailoutId_ = true; }
    uint32_t bailoutId() const { return bailoutId_; }
    uint32_t bytecodeIp() const { return bytecodeIp_; }
    bool hasBailoutId() const { return hasBailoutId_; }

    void addFsUse(const LIROperand& op, JITType type) { fsUses_.push_back({op, type}); }
    std::vector<std::pair<LIROperand, JITType>>& fsUsesMut() { return fsUses_; }
    const std::vector<std::pair<LIROperand, JITType>>& fsUses() const { return fsUses_; }

    // 供 Callout 使用
    void setFunctionPtr(void* ptr) { functionPtr_ = ptr; }
    void* functionPtr() const { return functionPtr_; }
    void setArgc(uint32_t argc) { argc_ = argc; }
    uint32_t argc() const { return argc_; }

    // 供寄存器分配器使用 (线性 ID)
    void setLinearId(uint32_t id) { linearId_ = id; }
    uint32_t linearId() const { return linearId_; }

private:
    uint32_t id_;
    uint32_t linearId_ = 0;
    LIROpcode opcode_;
    std::vector<LIROperand> defs_;
    std::vector<LIROperand> uses_;
    std::vector<LIRConstraint> defConstraints_;
    std::vector<LIRConstraint> useConstraints_;
    std::vector<Register> clobbers_;
    std::vector<std::pair<LIROperand, JITType>> fsUses_;
    Condition cond_ = Condition::Equal;
    class LIRBlock* target_ = nullptr;
    uint32_t bailoutId_ = 0;
    uint32_t bytecodeIp_ = 0;
    bool hasBailoutId_ = false;
    void* functionPtr_ = nullptr;
    uint32_t argc_ = 0;
};

// ============================================================================
// LIR 基本块 (Step 36)
// ============================================================================
class LIRBlock {
public:
    explicit LIRBlock(uint32_t id) : id_(id) {}
    ~LIRBlock() {
        for (auto inst : instructions_) inst->~LIRInst();
    }

    uint32_t id() const { return id_; }

    void addInstruction(LIRInst* inst) { instructions_.push_back(inst); }
    const std::vector<LIRInst*>& instructions() const { return instructions_; }

    void addPredecessor(LIRBlock* block) { predecessors_.push_back(block); }
    void addSuccessor(LIRBlock* block) { successors_.push_back(block); }

    const std::vector<LIRBlock*>& predecessors() const { return predecessors_; }
    const std::vector<LIRBlock*>& successors() const { return successors_; }
    
    std::vector<LIRInst*>& instructionsMut() { return instructions_; }

    // 供寄存器分配器使用 (循环嵌套深度)
    void setLoopDepth(int depth) { loopDepth_ = depth; }
    int loopDepth() const { return loopDepth_; }

    // 供活跃区间分析使用
    std::unordered_set<uint32_t> liveIn;
    std::unordered_set<uint32_t> liveOut;

private:
    uint32_t id_;
    std::vector<LIRInst*> instructions_;
    std::vector<LIRBlock*> predecessors_;
    std::vector<LIRBlock*> successors_;
    int loopDepth_ = 0;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_LIR_H
