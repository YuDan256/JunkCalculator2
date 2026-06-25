#ifndef JC2_REGVM_IR_H
#define JC2_REGVM_IR_H

#include <vector>
#include <string>
#include <memory>
#include "../../memory/Value.h"

namespace jc {
namespace regvm {

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
    TryEnd,
    Throw,

    // 算术与逻辑运算
    Add, Sub, Mul, Div, Mod, Pow, LeftDivide,
    Eq, Neq, Lt, Le, Gt, Ge,
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
    CallExt,    // 极端调用 (参数在溢出槽)
    TailCall,
    Invoke,
    TailInvoke,
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
    SliceGet,
    SliceSet,
    ListInit,
    ListAppend,
    ListCompEnd,

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
    Inherit,
    GetProperty,
    TryGetProperty,
    SetProperty,
    GetSuper,
    GetSelf,

    // 模块导入
    Import,

    // 类型与断言
    AssertParamType,
    AssertReturnType,
    MatchType,
    MatchShape,

    // 寄存器操作 (用于 Phi 去结构化和寄存器溢出)
    Move,       // 寄存器间移动
    LoadExt,    // 从溢出槽加载
    StoreExt    // 存入溢出槽
};

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
    
    // 寄存器分配信息
    int virtualReg = -1;                 // 虚拟寄存器 ID (SSA 阶段分配)
    int physicalReg = -1;                // 物理寄存器 ID (0~255) 或溢出槽 (>=256)
    
    // 附加数据载体
    Value constVal;                      // 用于 Constant 节点
    int localSlot = -1;                  // 用于 Parameter/LoadLocal/StoreLocal 节点
    std::string name;                    // 用于调试、全局变量名等
    
    // 指令立即数载体 (用于存储 argc, dims, shapeIdx, icIdx, fbType 等)
    uint32_t payload1 = 0;
    uint32_t payload2 = 0;
    uint32_t payload3 = 0;
    
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
// IR 图 (表示一个函数的完整中间表示)
// ============================================================================
class IRGraph {
private:
    std::vector<std::unique_ptr<IRNode>> nodes;
    int nextId = 0;
    int nextVirtualReg = 0; // 虚拟寄存器分配器

public:
    IRNode* startNode = nullptr;

    IRGraph() {
        startNode = createNode(IROp::Start);
        startNode->name = "Start";
    }

    // 创建通用节点
    IRNode* createNode(IROp op) {
        auto node = std::make_unique<IRNode>(nextId++, op);
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
};

} // namespace regvm
} // namespace jc

#endif // JC2_REGVM_IR_H
