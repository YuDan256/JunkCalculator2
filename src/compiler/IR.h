#ifndef JC2_COMPILER_IR_H
#define JC2_COMPILER_IR_H

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <iostream>
#include "../memory/Value.h"

namespace jc {

// ============================================================================
// IR 节点操作码 (Sea of Nodes)
// ============================================================================
enum class IROp {
    Nop,        // 空节点 (用于被消除的死代码)

    // 控制流节点 (Control Flow)
    Start,
    Return,
    If,
    IfTrue,     // If 的 True 分支
    IfFalse,    // If 的 False 分支
    Merge,      // 控制流汇合点
    Loop,       // 循环头

    // 数据节点 (Data)
    Parameter,
    Constant,
    
    // SSA 核心节点
    Phi,        // 在 Merge 点根据控制流选择数据

    TryBegin,   // 异常处理
    Catch,      // 捕获异常对象
    TryEnd,
    Throw,

    // 算术与逻辑运算
    Add, Sub, Mul, Div, IDiv, Mod, Pow, LeftDivide,
    Eq, Neq, Lt, Le, Gt, Ge, Is, IsSubset,
    Not, Neg, ToBool,
    BitAnd, BitOr, BitXor, BitNot, Shl, Shr,
    
    // 变量与内存 (在 SSA 构建阶段会被尽量消除，转化为 Phi)
    LoadLocal,
    StoreLocal,
    GetGlobal,
    SetGlobal,
    SetGlobalRef,
    DefineConstGlobal,
    DeleteGlobal,
    IsUninit,
    UpdateCaptured,
    
    // 闭包与上值
    Closure,
    GetUpvalue,
    SetUpvalue,

    // 引用参数
    GetRefParam,
    SetRefParam,
    PassRefs,

    // 函数调用
    Call,
    TailCall,
    Invoke,
    TailInvoke,
    InvokePrivate,
    TailInvokePrivate,
    InvokeFallback,
    TailInvokeFallback,
    SuperInvoke,
    TailSuperInvoke,

    // 容器与索引
    BuildList,
    BuildMatrix,
    BuildDict,
    DictRest,
    BuildNamespace,
    BuildSet,
    IndexGet,
    IndexSet,
    BuildSlice,
    MakeSpread,
    ListInit,
    ListAppend,
    MatrixCompInit,
    MatrixCompAppend,
    MatrixCompEnd,
    SetInit,
    SetAppend,
    DictInit,
    DictAppend,

    // 迭代器与 for-in
    IterInit,
    IterNext,
    In,

    // 字符串操作
    Stringify,
    ConcatStrings,
    FormatString,

    // 类与面向对象
    Class,
    Method,
    MethodPrivate,
    MethodConst,
    MethodPrivateConst,
    Inherit,
    GetProperty,
    GetPrivate,
    TryGetProperty,
    SetProperty,
    SetPrivate,
    DefinePrivate,
    DefinePrivateConst,
    DefineProp,
    DefinePropConst,
    GetSuper,
    GetSelf,
    GetCurrentClosure,

    // 模块导入
    Import,

    Defer,
    RunDefers,

    // 类型与断言
    AssertParamType,
    AssertReturnType,
    AssertType,
    MatchType,
    MatchShape,
    MatchInit,

    // 寄存器操作 (用于 Phi 去结构化和寄存器溢出)
    Move,       // 寄存器间移动
    FreeReg     // 释放寄存器 (帮助 GC)
};

inline std::string irOpToString(IROp op) {
    switch (op) {
        case IROp::Nop: return "Nop";
        case IROp::Start: return "Start";
        case IROp::Return: return "Return";
        case IROp::If: return "If";
        case IROp::IfTrue: return "IfTrue";
        case IROp::IfFalse: return "IfFalse";
        case IROp::Merge: return "Merge";
        case IROp::Loop: return "Loop";
        case IROp::Parameter: return "Parameter";
        case IROp::Constant: return "Constant";
        case IROp::Phi: return "Phi";
        case IROp::TryBegin: return "TryBegin";
        case IROp::Catch: return "Catch";
        case IROp::TryEnd: return "TryEnd";
        case IROp::Throw: return "Throw";
        case IROp::Add: return "Add";
        case IROp::Sub: return "Sub";
        case IROp::Mul: return "Mul";
        case IROp::Div: return "Div";
        case IROp::IDiv: return "IDiv";
        case IROp::Mod: return "Mod";
        case IROp::Pow: return "Pow";
        case IROp::LeftDivide: return "LeftDivide";
        case IROp::Eq: return "Eq";
        case IROp::Neq: return "Neq";
        case IROp::Lt: return "Lt";
        case IROp::Le: return "Le";
        case IROp::Gt: return "Gt";
        case IROp::Ge: return "Ge";
        case IROp::Is: return "Is";
        case IROp::IsSubset: return "IsSubset";
        case IROp::Not: return "Not";
        case IROp::Neg: return "Neg";
        case IROp::ToBool: return "ToBool";
        case IROp::BitAnd: return "BitAnd";
        case IROp::BitOr: return "BitOr";
        case IROp::BitXor: return "BitXor";
        case IROp::BitNot: return "BitNot";
        case IROp::Shl: return "Shl";
        case IROp::Shr: return "Shr";
        case IROp::LoadLocal: return "LoadLocal";
        case IROp::StoreLocal: return "StoreLocal";
        case IROp::GetGlobal: return "GetGlobal";
        case IROp::SetGlobal: return "SetGlobal";
        case IROp::SetGlobalRef: return "SetGlobalRef";
        case IROp::DefineConstGlobal: return "DefineConstGlobal";
        case IROp::DeleteGlobal: return "DeleteGlobal";
        case IROp::IsUninit: return "IsUninit";
        case IROp::UpdateCaptured: return "UpdateCaptured";
        case IROp::Closure: return "Closure";
        case IROp::GetUpvalue: return "GetUpvalue";
        case IROp::SetUpvalue: return "SetUpvalue";
        case IROp::GetRefParam: return "GetRefParam";
        case IROp::SetRefParam: return "SetRefParam";
        case IROp::PassRefs: return "PassRefs";
        case IROp::Call: return "Call";
        case IROp::TailCall: return "TailCall";
        case IROp::Invoke: return "Invoke";
        case IROp::TailInvoke: return "TailInvoke";
        case IROp::InvokePrivate: return "InvokePrivate";
        case IROp::TailInvokePrivate: return "TailInvokePrivate";
        case IROp::InvokeFallback: return "InvokeFallback";
        case IROp::TailInvokeFallback: return "TailInvokeFallback";
        case IROp::SuperInvoke: return "SuperInvoke";
        case IROp::TailSuperInvoke: return "TailSuperInvoke";
        case IROp::BuildList: return "BuildList";
        case IROp::BuildMatrix: return "BuildMatrix";
        case IROp::BuildDict: return "BuildDict";
        case IROp::DictRest: return "DictRest";
        case IROp::BuildNamespace: return "BuildNamespace";
        case IROp::BuildSet: return "BuildSet";
        case IROp::IndexGet: return "IndexGet";
        case IROp::IndexSet: return "IndexSet";
        case IROp::BuildSlice: return "BuildSlice";
        case IROp::MakeSpread: return "MakeSpread";
        case IROp::ListInit: return "ListInit";
        case IROp::ListAppend: return "ListAppend";
        case IROp::MatrixCompInit: return "MatrixCompInit";
        case IROp::MatrixCompAppend: return "MatrixCompAppend";
        case IROp::MatrixCompEnd: return "MatrixCompEnd";
        case IROp::SetInit: return "SetInit";
        case IROp::SetAppend: return "SetAppend";
        case IROp::DictInit: return "DictInit";
        case IROp::DictAppend: return "DictAppend";
        case IROp::IterInit: return "IterInit";
        case IROp::IterNext: return "IterNext";
        case IROp::In: return "In";
        case IROp::Stringify: return "Stringify";
        case IROp::ConcatStrings: return "ConcatStrings";
        case IROp::FormatString: return "FormatString";
        case IROp::Class: return "Class";
        case IROp::Method: return "Method";
        case IROp::MethodPrivate: return "MethodPrivate";
        case IROp::MethodConst: return "MethodConst";
        case IROp::MethodPrivateConst: return "MethodPrivateConst";
        case IROp::Inherit: return "Inherit";
        case IROp::GetProperty: return "GetProperty";
        case IROp::GetPrivate: return "GetPrivate";
        case IROp::TryGetProperty: return "TryGetProperty";
        case IROp::SetProperty: return "SetProperty";
        case IROp::SetPrivate: return "SetPrivate";
        case IROp::DefinePrivate: return "DefinePrivate";
        case IROp::DefinePrivateConst: return "DefinePrivateConst";
        case IROp::DefineProp: return "DefineProp";
        case IROp::DefinePropConst: return "DefinePropConst";
        case IROp::GetSuper: return "GetSuper";
        case IROp::GetSelf: return "GetSelf";
        case IROp::GetCurrentClosure: return "GetCurrentClosure";
        case IROp::Import: return "Import";
        case IROp::Defer: return "Defer";
        case IROp::RunDefers: return "RunDefers";
        case IROp::AssertParamType: return "AssertParamType";
        case IROp::AssertReturnType: return "AssertReturnType";
        case IROp::AssertType: return "AssertType";
        case IROp::MatchType: return "MatchType";
        case IROp::MatchShape: return "MatchShape";
        case IROp::MatchInit: return "MatchInit";
        case IROp::Move: return "Move";
        case IROp::FreeReg: return "FreeReg";
        default: return "Unknown";
    }
}

// ============================================================================
// IR 节点定义
// 在 Sea of Nodes 中，数据依赖和控制依赖被显式区分，以支持更复杂的图优化
// ============================================================================
struct IRNode {
    int id;
    IROp op;
    
    // 依赖边 (Sea of Nodes)
    IRNode* controlInput = nullptr;      // 控制依赖 (Control Flow)
    std::vector<IRNode*> dataInputs;     // 数据依赖 (Data Flow)
    
    IRNode* forwarding = nullptr;        // 用于节点替换时的转发
    
    IRNode* getResolved() {
        IRNode* curr = this;
        while (curr->forwarding) curr = curr->forwarding;
        return curr;
    }
    
    // 寄存器分配信息
    int virtualReg = -1;                 // 虚拟寄存器 ID (SSA 阶段分配)
    int physicalReg = -1;                // 物理寄存器 ID (0~127) 或溢出槽 (>=128)
    
    int line = 0;                        // 源代码行号
    
    // 附加数据载体
    Value constVal;                      // 用于 Constant 节点
    int localSlot = -1;                  // 用于 Parameter/LoadLocal/StoreLocal 节点
    std::string name;                    // 用于调试、全局变量名等
    
    // 指令立即数载体 (用于存储 argc, dims, shapeIdx, icIdx, fbType 等)
    uint32_t payload1 = 0;
    uint32_t payload2 = 0;
    uint32_t payload3 = 0;
    uint32_t payload4 = 0;
    uint32_t payload5 = 0;
    
    IRNode(int id, IROp op) : id(id), op(op) {}

    // 辅助方法：设置控制依赖
    void setControl(IRNode* ctrl) {
        controlInput = ctrl;
    }

    // 辅助方法：添加数据依赖
    void addData(IRNode* data) {
        dataInputs.push_back(data);
    }
};

// ============================================================================
// 基本块 (Basic Block) - 用于指令调度和发射
// ============================================================================
struct BasicBlock {
    int id = 0;
    IRNode* controlNode = nullptr;
    std::vector<IRNode*> instructions;
    std::vector<BasicBlock*> preds;
    std::vector<BasicBlock*> succs;

    std::unordered_set<int> def;
    std::unordered_set<int> use;
    std::unordered_set<int> liveIn;
    std::unordered_set<int> liveOut;
};

// ============================================================================
// IR 图 (表示一个函数的完整中间表示)
// ============================================================================
struct IRArgSource {
    uint8_t argIndex;
    uint8_t sourceType;
    std::string name;
    int upvalIdx;
    IRNode* localNode;
    std::string kwName = ""; // ★ 记录命名参数的键名
    bool isConst = false;    // ★ const 实参（传给 ref 参数时报错）
};

struct IRCallSignature {
    std::vector<IRArgSource> refs;
};

class IRGraph {
private:
    std::vector<std::unique_ptr<IRNode>> nodes;
    int nextId = 0;
    int nextVirtualReg = 0; // 虚拟寄存器分配器

public:
    int currentLine = 0;
    IRNode* startNode = nullptr;
    std::vector<std::unique_ptr<BasicBlock>> blocks; // 调度后的基本块序列
    std::vector<std::function<void()>> postAllocCallbacks;
    std::vector<IRCallSignature> callSignatures;

    IRGraph() {
        startNode = createNode(IROp::Start);
        startNode->name = "Start";
    }

    // 创建通用节点
    IRNode* createNode(IROp op) {
        auto node = std::make_unique<IRNode>(nextId++, op);
        node->line = currentLine;
        IRNode* ptr = node.get();
        nodes.push_back(std::move(node));
        return ptr;
    }

    // 创建带虚拟寄存器的节点 (产生值的节点)
    IRNode* createValueNode(IROp op) {
        IRNode* node = createNode(op);
        node->virtualReg = nextVirtualReg++;
        return node;
    }

    // 创建常量节点
    IRNode* createConstant(const Value& val) {
        IRNode* node = createValueNode(IROp::Constant);
        node->constVal = val;
        return node;
    }

    // 获取所有节点 (用于遍历、优化和最终的寄存器分配)
    const std::vector<std::unique_ptr<IRNode>>& getNodes() const {
        return nodes;
    }

    // 分配一个新的虚拟寄存器
    int allocateVirtualReg() {
        return nextVirtualReg++;
    }

    void print(const std::string& title) const {
        std::cout << "=== IR Graph: " << title << " ===" << std::endl;
        for (const auto& nodePtr : nodes) {
            IRNode* n = nodePtr.get();
            if (n->op == IROp::Nop) continue;
            std::cout << "ID: " << n->id << " | " << irOpToString(n->op);
            if (n->virtualReg != -1) std::cout << " | vR: " << n->virtualReg;
            if (n->physicalReg != -1) std::cout << " | pR: " << n->physicalReg;
            if (n->controlInput) std::cout << " | Ctrl: " << n->controlInput->id;
            if (!n->dataInputs.empty()) {
                std::cout << " | Data: [";
                for (size_t i = 0; i < n->dataInputs.size(); ++i) {
                    if (n->dataInputs[i]) std::cout << n->dataInputs[i]->id;
                    else std::cout << "null";
                    if (i < n->dataInputs.size() - 1) std::cout << ", ";
                }
                std::cout << "]";
            }
            if (n->op == IROp::Constant) {
                if (n->constVal.isString()) std::cout << " | Val: \"" << n->constVal.asString() << "\"";
                else std::cout << " | Val: " << n->constVal.toJC2Expression();
            }
            if (!n->name.empty()) std::cout << " | Name: " << n->name;
            std::cout << std::endl;
        }
        if (!blocks.empty()) {
            std::cout << "--- Basic Blocks ---" << std::endl;
            for (const auto& bb : blocks) {
                std::cout << "BB" << bb->id << " (Ctrl: " << (bb->controlNode ? std::to_string(bb->controlNode->id) : "null") << ")";
                if (!bb->preds.empty()) {
                    std::cout << " Preds: ";
                    for (auto p : bb->preds) std::cout << "BB" << p->id << " ";
                }
                if (!bb->succs.empty()) {
                    std::cout << " Succs: ";
                    for (auto s : bb->succs) std::cout << "BB" << s->id << " ";
                }
                std::cout << "\n  Insts: ";
                for (auto inst : bb->instructions) std::cout << inst->id << " ";
                std::cout << std::endl;
            }
        }
        std::cout << "=====================" << std::endl;
    }
};

} // namespace jc

#endif // JC2_COMPILER_IR_H
