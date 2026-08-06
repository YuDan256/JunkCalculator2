# JC2 工业级 JIT 编译器设计草案

本文档记录了在 JC2 引擎中引入工业级 JIT（Just-In-Time）编译器的核心架构设计与讨论。JC2 的 JIT 旨在通过自定义 IR 和手写机器码发射器，实现高性能的动态语言执行，而非依赖庞大的第三方后端（如 LLVM）。

## 1. 分层编译架构 (Tiered Compilation)

JC2 采用 **Method-JIT（基于方法的 JIT）** 架构。工业级 JIT 采用分层执行模型，以平衡启动速度与峰值性能：

*   **Tier 0 (现有的字节码解释器)：** 负责快速启动。增强现有的 `InlineCache`，不仅缓存全局变量和属性查找，还要**收集类型信息（Type Profiling）**。记录每个操作码（如 `ADD`）在运行时的实际操作数类型。
*   **Tier 1 (Baseline JIT - 可选)：** 极速编译，不做复杂优化，直接将字节码 1:1 翻译为机器码，主要为了消除解释器的分发开销（Dispatch Overhead）。
*   **Tier 2 (Optimizing JIT)：** 真正的重头戏。当某个函数被调用次数超过阈值（Hot Function），触发后台线程编译。它将读取 Tier 0 收集的类型反馈，利用 JIT 专用的 IR 进行**类型特化（Type Specialization）**。
*   **OSR (On-Stack Replacement)：** 针对“调用次数少，但内部有耗时循环”的函数，当循环回边（Backedge）计数器达到阈值时，直接在栈上将解释器状态热切换到 JIT 机器码执行。

## 2. JIT 专用 IR 的设计 (HIR & LIR)

现有的 AOT IR 是为了生成 JC2 字节码设计的，操作数是泛型的 `Value`。JIT 需要一套全新的 IR 来实现**“拆箱 (Unboxing)”**，即在原生 CPU 寄存器上直接运算。

### HIR (High-Level IR)
带有类型推导和守卫的图表示：
*   **类型守卫 (Guard Nodes)：** 例如 `GuardIsInt32(v)`，在运行时检查变量类型。如果检查失败，控制流立刻跳转到 `Bailout`（逃生）节点。
*   **状态快照 (FrameState)：** 每一个 `Guard` 节点都必须挂载一个 `FrameState` 节点。记录在当前执行点，JC2 虚拟机的 256 个虚拟寄存器分别对应 JIT IR 中的哪些节点，用于去优化时的状态还原。
*   **拆箱操作 (Unboxed Ops)：** 例如 `AddI32`、`MulF64`。它们只接受纯粹的机器类型，不处理 JC2 的 `Value` 装箱对象。

### LIR (Low-Level IR)
贴近机器指令的线性序列：
*   在 HIR 完成常量折叠、公共子表达式消除（CSE）和循环不变量外提（LICM）后，降级为 LIR。
*   LIR 引入**物理寄存器约束**。显式表达特定 CPU 指令对物理寄存器的绑定关系（如 x86-64 的 `idiv` 强制要求 `RAX` 和 `RDX`）。

## 3. 自定义机器码发射器 (Macro Assembler)

不依赖第三方后端，纯手工打造宏汇编器：

*   **内存管理 (Executable Memory)：** 向操作系统申请具有 `RWE`（读/写/执行）权限的内存页（`VirtualAlloc` / `mmap`）。写入机器码后，必须调用指令缓存刷新（Instruction Cache Flush）。
*   **指令编码 (Instruction Encoding)：** 对照 CPU 开发者手册（如 Intel/AMD），手动拼接机器码（REX 前缀、ModR/M 字节、SIB 字节等）。提供类似 `emit_add_reg_imm(RCX, 42)` 的 C++ 接口。
*   **物理寄存器分配 (Physical Register Allocation)：** 面对极其有限的 CPU 物理寄存器，采用**线性扫描寄存器分配器 (Linear Scan Register Allocator)**，以保证极快的编译速度和良好的代码质量。

## 4. 去优化机制与 ABI 边界 (Deoptimization & ABI)

这是动态语言 JIT 最核心且最复杂的安全机制：

*   **C++ 与 JIT 代码的边界 (ABI Compliance)：** JIT 代码调用 JC2 的 C++ 运行时函数（如分配内存）时，必须严格遵守操作系统的 C ABI（如 Windows x64 ABI 的寄存器传参、栈对齐、Shadow Space），否则会导致崩溃。
*   **去优化跳板 (Deopt Trampoline)：** 当 HIR 中的 `Guard` 失败时，机器码跳转到汇编跳板代码：
    1. 将 CPU 的所有物理寄存器压入机器栈。
    2. 将触发去优化的 `BailoutId` 作为参数，调用 C++ 的 `Deoptimize` 运行时函数。
*   **状态重建 (State Reconstruction)：** C++ 的 `Deoptimize` 函数根据 `BailoutId` 查找编译时生成的 **Stack Map（栈图 / OSR Map）**。根据栈图，把物理寄存器和栈上的值重新打包成 JC2 的 `Value`（装箱），填回解释器的 `CallFrame` 和 `registers` 数组中，最后修改解释器的 `ip` 指针，平滑退回到字节码解释执行。
*   **栈上替换 (OSR, On-Stack Replacement)：** 这是去优化的逆过程。当解释器中的循环跑热时，JIT 编译完成后，解释器会根据 OSR Map，将当前的 `VM::registers` 拆箱并塞入 CPU 物理寄存器和机器栈，然后直接 `jmp` 到机器码的循环头继续执行。
*   **与 GC 的协作 (Safepoints)：** JIT 代码在循环回边和函数调用处插入 Safepoint 检查。在 Safepoint 处，JIT 生成栈图指导 GC 扫描原生机器栈和寄存器中的有效 `Value` 指针。

## 5. 类型反馈收集机制 (Type Profiling in Tier 0)

没有准确的类型信息，JIT 就无法生成高效的机器码。但收集信息不能严重拖慢解释器（Tier 0）的执行速度。

*   **Type Feedback Vector (类型反馈向量)：** 
    在 `CompiledFunction` 或 `Chunk` 中引入一个与字节码指令对应的数组。只有可能触发多态的指令（如 `ADD`, `MUL`, `GET_PROP`, `CALL`）才会分配反馈槽（Feedback Slot）。
*   **类型状态机 (Type State Machine)：**
    每个槽位维护一个状态机，记录运行时的类型突变：
    1.  `Uninitialized` (未初始化)：该指令尚未执行过。
    2.  `Monomorphic` (单态)：100% 遇到同一种类型（例如 `ADD` 的左右操作数全是 `Int32`）。这是 JIT 最喜欢的状态，可以直接生成极速的特化机器码。
    3.  `Polymorphic` (多态)：遇到 2~4 种类型（例如有时是 `Int32`，有时是 `Double`）。JIT 会生成一个小型 `switch-case` 类型的机器码分支。
    4.  `Megamorphic` (超态/复态)：类型极其混乱（超过 4 种）。JIT 会放弃类型特化，直接生成调用 C++ 运行时泛型函数的机器码（Fallback）。
*   **低开销收集：** 
    利用现有的 NaN-Boxing 机制，提取类型的开销极低（只需位运算）。在解释器的 `OpCode::ADD` 等指令中，插入轻量级的宏来更新 Feedback Slot。

## 6. JIT HIR 节点设计细节 (HIR Node Semantics)

JIT 的 HIR 必须能够表达“推测执行（Speculative Execution）”和“状态快照”。以下是几种核心的 HIR 节点设计：

*   **`GuardIsInt32(value, bailout_id)`**
    *   **语义：** 断言 `value` 必须是 `Int32`。
    *   **机器码表现：** 检查 NaN-Boxing 的 Tag，如果不是 Int32，直接 `jmp` 到去优化跳板（Deopt Trampoline）。
*   **`FrameState(ip, registers...)`**
    *   **语义：** 这是一个幽灵节点（不生成实际机器码），它必须挂载在每一个 `Guard` 节点上。
    *   **作用：** 记录如果在这个 `Guard` 失败了，JC2 虚拟机的 `ip` 应该回退到哪里，以及虚拟机的 256 个寄存器分别对应当前 HIR 图中的哪些节点。
*   **`UnboxInt32(value)` / `BoxInt32(raw_int)`**
    *   **语义：** 拆箱与装箱。`UnboxInt32` 将 JC2 的 64 位 `Value` 剥离 Tag，变成纯粹的 32 位原生整数，供后续的 `AddI32` 节点使用。
*   **`AddI32(lhs_raw, rhs_raw)`**
    *   **语义：** 纯机器级别的 32 位整数加法。
    *   **溢出处理：** 工业级 JIT 必须处理整数溢出。如果 `AddI32` 发生溢出（x86 的 Overflow Flag 被置位），同样会触发去优化，退回到解释器，解释器会自动将其提升为 `BigInt` 或 `Double`。

## 7. 严格的 API 封装规范 (Strict API Encapsulation)

为了避免手动操作图节点和拼接机器码带来的易错性，JIT 的所有核心操作必须经过高度封装的 API，严禁越过 API 直接操作底层数据结构。

### 7.1 HIR 构建与修改 API (HIRBuilder)
绝对禁止手动修改 HIR 节点的指针（如直接赋值 `node->inputs[0] = other` 或手动维护 `uses` 链）。必须通过 `HIRBuilder` 统一管理：
*   **节点创建 (Node Creation)：** 提供如 `builder.createAddI32(lhs, rhs)`、`builder.createGuardIsInt32(val, frameState)` 的强类型接口。API 内部自动维护双向的 Use-Def 链和控制流图（CFG）的连通性。
*   **图替换与优化 (Graph Mutation)：** 提供 `builder.replaceNode(oldNode, newNode)`，自动更新所有依赖 `oldNode` 的使用点（Uses），确保图的完整性，防止悬空指针。提供 `builder.killNode(node)` 安全地消除死代码。
*   **状态快照管理 (FrameState Management)：** 提供 `builder.captureFrameState(ip)`，自动捕获当前虚拟寄存器到 HIR 节点的映射关系，并生成 `FrameState` 节点。
*   **控制流构建 (Control Flow)：** 提供 `builder.createBranch(condition)`、`builder.bindBlock(block)`，隐藏底层的控制边（Control Edge）连接细节。

### 7.2 LIR 构建 API (LIRBuilder)
LIR 负责将图结构线性化，同样禁止手动操作指令列表。必须通过 `LIRBuilder` 统一管理：
*   **基本块管理 (Block Management)：** 提供 `builder.createBlock()`、`builder.setCurrentBlock(block)`。
*   **指令发射 (Instruction Emission)：** 提供 `builder.emit(LIROpcode, defs, uses)`。API 内部自动处理虚拟寄存器的分配和生命周期记录。
*   **物理约束注入 (Constraint Injection)：** 提供 `builder.emitWithConstraints(LIROpcode, defs, uses, constraints)`，显式声明 x86-64 的物理寄存器绑定要求（如强制要求 RAX），供后续的线性扫描分配器使用。
*   **控制流跳转 (Jumps & Branches)：** 提供 `builder.emitJump(targetBlock)`、`builder.emitCondJump(cond, trueBlock, falseBlock)`，自动维护 LIR 块之间的前驱/后继（Predecessor/Successor）关系。

### 7.3 机器码发射 API (MacroAssembler)
绝对禁止在业务逻辑中直接拼接十六进制字节流（如 `buffer.push_back(0x48)`）。必须通过 `MacroAssembler` 屏蔽底层指令编码细节：
*   **强类型寄存器：** 引入物理寄存器类型（如 `Register::RAX`, `XMMRegister::XMM0`），利用 C++ 类型系统防止通用寄存器与浮点寄存器混用。
*   **指令发射接口：** 提供语义清晰的接口，如 `masm.mov(rax, rbx)`、`masm.add(rcx, imm32(42))`。API 内部负责处理 x86-64 的 REX 前缀、ModR/M 字节和 SIB 字节的复杂编码。
*   **标签与跳转 (Labels & Backpatching)：** 提供 `Label` 类和 `masm.bind(&label)`、`masm.jmp(&label)`、`masm.jcc(Condition::Equal, &label)`。由汇编器自动计算相对偏移量（Relative Offset）并在代码生成末期进行回填（Backpatching）。

## 8. JIT HIR 结构设计 (HIR Structure)

JIT HIR 采用 **Sea of Nodes（节点海）** 架构，将控制流、数据流和副作用（Effect）统一在同一张有向图中。

### 8.1 节点基类设计 (HIRNode)
所有 HIR 节点继承自统一的基类，维护双向图关系：
*   **`id`**: 节点的唯一标识符。
*   **`opcode`**: 节点操作码（如 `HIROp::AddI32`, `HIROp::GuardIsInt32`）。
*   **`type`**: 节点产生的数据类型（如 `JITType::Int32`, `JITType::Double`, `JITType::TaggedValue`, `JITType::Control`）。
*   **`inputs` (Use-Def)**: 指向当前节点依赖的输入节点指针数组。
*   **`uses` (Def-Use)**: 记录哪些节点将当前节点作为输入（用于快速的死代码消除和节点替换）。

### 8.2 边的类型 (Edge Types)
在 `inputs` 数组中，根据索引位置约定边的语义：
1.  **Control Edge (控制边)**: 决定执行的先后顺序。例如 `Branch` 节点的输入控制边指向 `Start` 或上一个基本块。
2.  **Data Edge (数据边)**: 纯粹的数值依赖。例如 `AddI32` 依赖两个 `Int32` 类型的数据节点。
3.  **Effect Edge (副作用边)**: 用于串联内存读写操作（如 `StoreField`），防止指令调度器（Scheduler）打乱具有依赖关系的内存操作。

### 8.3 核心节点分类
*   **控制流节点 (Control Nodes)**:
    *   `Start`: 图的入口。
    *   `Branch(control, condition)`: 根据条件分发控制流。
    *   `IfTrue(control)` / `IfFalse(control)`: 挂载在 `Branch` 之后。
    *   `Return(control, effect, value)`: 函数返回。
*   **数据与运算节点 (Data Nodes)**:
    *   `Int32Constant(value)` / `DoubleConstant(value)`: 常量。
    *   `AddI32(lhs, rhs)` / `MulF64(lhs, rhs)`: 纯机器类型运算。
    *   `Phi(control, val1, val2, ...)`: SSA 形式中的 Phi 节点，用于在控制流汇合处合并数据。
*   **守卫与去优化节点 (Guard & Deopt Nodes)**:
    *   `FrameState(ip, locals...)`: 记录解释器状态的幽灵节点。
    *   `GuardIsInt32(control, effect, value, frame_state)`: 运行时类型检查。如果 `value` 不是 Int32，则触发去优化，利用 `frame_state` 恢复解释器。
    *   `UnboxInt32(value)`: 将 JC2 的 `Value` 剥离 Tag 转换为原生 32 位整数。

## 9. JIT LIR 结构设计 (LIR Structure)

LIR (Low-Level IR) 是连接 HIR 与底层机器码的桥梁。它的核心任务是将无序的图结构线性化，并引入物理机器的约束（如寄存器和调用约定）。

### 9.1 控制流图与基本块 (CFG & Basic Blocks)
LIR 放弃了 Sea of Nodes 结构，转而使用传统的控制流图 (CFG)：
*   **Basic Block (基本块)：** 包含线性排列的 LIR 指令序列。只有一个入口和一个出口（通常是跳转或返回指令）。
*   **指令调度 (Instruction Scheduling)：** 通过全局代码移动 (Global Code Motion) 算法，将 HIR 节点根据数据依赖和控制依赖，排序并分配到具体的 LIR 基本块中。

### 9.2 操作数抽象 (LIR Operands)
LIR 指令的操作数 (`LIROperand`) 必须是具体的存储位置或常量：
*   **VirtualReg (虚拟寄存器)：** 数量无限，由 HIR 节点 ID 映射而来，等待寄存器分配器处理。
*   **PhysicalReg (物理寄存器)：** 具体的 CPU 寄存器（如 x86-64 的 `RAX`, `XMM0`）。
*   **StackSlot (栈槽)：** 机器栈上的内存偏移量，用于处理寄存器溢出 (Spill) 或去优化状态保存。
*   **Immediate (立即数)：** 直接编码在机器指令中的常数。

### 9.3 指令与物理寄存器约束 (Instructions & Constraints)
LIR 指令 (`LIRInst`) 明确区分了定义 (Defs) 和使用 (Uses)，并引入了 x86-64 架构特有的物理约束：
*   **Fixed Register Constraints (固定寄存器约束)：** 某些机器指令对寄存器有硬性要求。例如，LIR 的 `DivI32` 指令会明确声明：“我的左操作数必须在 `RAX`，我的输出也在 `RAX`，并且我会破坏 (Clobber) `RDX`”。
*   **寄存器分配器的职责：** 线性扫描寄存器分配器 (Linear Scan) 在处理 LIR 时，如果遇到固定约束，会自动在前后插入 `mov` 指令，将虚拟寄存器的数据腾挪到指定的物理寄存器中。

### 9.4 去优化与栈图伪指令 (Deopt & Stack Maps in LIR)
*   HIR 中的 `Guard` 节点在 LIR 中会被降级为两条指令：一条原生的比较指令（如 `cmp`），紧跟一条条件跳转指令（如 `jne`）。
*   跳转的目标是一个特殊的 **Bailout Block (逃生块)**。在这个块中，LIR 会生成一条 `Deoptimize` 伪指令，该指令携带了从 HIR `FrameState` 继承来的元数据，指导汇编器生成 OSR/Stack Map 表，以便 C++ 运行时能够重建解释器状态。

## 10. HIRBuilder 设计 (Bytecode to Sea of Nodes)

`HIRBuilder` 的核心任务是通过**抽象解释 (Abstract Interpretation)**，将线性的 JC2 字节码转换为 Sea of Nodes 图结构，并注入类型特化。

### 10.1 虚拟寄存器状态模拟 (Virtual Register Environment)
在遍历字节码时，`HIRBuilder` 必须维护一个“模拟状态表”。
*   表的大小为 256（对应 JC2 的虚拟寄存器数量）。
*   表中的元素不是真实的值，而是**指向当前 HIR 节点的指针**。
*   例如，遇到 `MOVE R1, R2` 字节码时，Builder 不生成任何 HIR 节点，仅仅是将状态表中 R2 的节点指针复制给 R1。这天然实现了**复写传播 (Copy Propagation)**。

### 10.2 多 Pass 构建与 Phi 节点插入
由于字节码包含跳转（分支和循环），构建图不能一蹴而就：
1.  **Pass 1 (CFG 发现)：** 扫描字节码的跳转指令（`JMP`, `JMP_TRUE` 等），划分基本块边界，构建初始的控制流图（CFG）。
2.  **Pass 2 (抽象解释)：** 按拓扑序遍历基本块。
3.  **Phi 节点生成：** 当控制流汇合时（如 `if-else` 结束处），如果同一个虚拟寄存器在不同分支中对应了不同的 HIR 节点，Builder 会自动在汇合块的开头插入一个 `Phi` 节点，合并数据流。

### 10.3 类型特化注入 (Type Specialization Injection)
在生成运算节点（如 `ADD`）前，`HIRBuilder` 会查询 Tier 0 收集的 Profiling 数据：
*   如果 Profiling 显示操作数 100% 是 `Int32`，Builder 会依次生成：`GuardIsInt32` -> `UnboxInt32` -> `AddI32`。
*   同时，在生成 `Guard` 时，Builder 会调用 `captureFrameState()`，将当前的“模拟状态表”打包成 `FrameState` 节点挂载到 `Guard` 上。

## 11. LIRBuilder 设计 (Sea of Nodes to LIR)

`LIRBuilder` 的核心任务是将无序的 HIR 图重新线性化，并进行指令选择（Instruction Selection）。

### 11.1 全局代码移动 (Global Code Motion, GCM)
HIR 是没有基本块归属的（除了控制节点）。GCM 算法负责为每个数据节点寻找最优的执行位置：
1.  **Schedule Early (尽早调度)：** 顺着数据依赖向下遍历，找到节点能被合法执行的“最高”基本块（必须在其所有输入节点之后）。
2.  **Schedule Late (尽晚调度)：** 顺着使用链向上遍历，找到节点必须被执行的“最低”基本块（必须在其所有使用者之前）。
3.  **Block Selection (块选择)：** 在 Early 和 Late 之间的合法基本块中，选择**循环嵌套深度最浅**的块，从而天然实现**循环不变量外提 (LICM)**。

### 11.2 指令选择与树模式匹配 (Tree Pattern Matching)
将 HIR 节点映射为 LIR 指令。工业级 JIT 通常支持将多个 HIR 节点折叠为一条高效的 LIR 指令：
*   例如，HIR 子图 `AddI32(a, Int32Constant(1))` 可以被模式匹配捕获，直接降级为一条 LIR 的 `IncI32(a)` 指令。
*   HIR 子图 `LoadMemory(AddI32(base, offset))` 可以被折叠为 x86 的复杂寻址模式 `[base + offset]`。

### 11.3 物理寄存器约束生成
在生成 LIR 指令时，`LIRBuilder` 必须显式声明 x86-64 的硬件约束。
*   例如，降级 HIR 的 `DivI32(a, b)` 时，生成的 LIR 指令会带有元数据：`Input0 必须在 RAX`，`Input1 必须在任意通用寄存器`，`Output 必须在 RAX`，`Clobbers (破坏) RDX`。
*   这些约束将作为契约，严格指导后续的线性扫描寄存器分配器（Linear Scan Allocator）插入必要的 `mov` 指令。

## 12. MacroAssembler 具体实现设计 (x86-64 Backend)

`MacroAssembler` 是 JIT 的最底层，负责将抽象的汇编指令转换为真实的 x86-64 机器码字节流。为了保证代码的健壮性，它的设计必须高度结构化。

### 12.1 寄存器与内存操作数抽象 (Operands Abstraction)
利用 C++ 的强类型系统防止寄存器误用：
*   **`Register` 类：** 封装 0-15 的整数 ID。预定义全局常量如 `rax`, `rcx`, `r8`, `r15`。
*   **`XMMRegister` 类：** 封装浮点寄存器，类型上与 `Register` 隔离，防止在 `add(rax, xmm0)` 时编译通过。
*   **`Operand` 类 (内存寻址)：** 封装 x86 的 `[base + index * scale + disp]` 寻址模式。提供便捷的构造函数，例如 `Operand(rbp, -8)` 或 `Operand(rax, rcx, Scale::Times4, 16)`。

### 12.2 核心编码引擎 (Instruction Encoding Core)
x86-64 的指令编码极其复杂，必须拆分为几个正交的发射阶段：
1.  **`emitRex(bool w, Register r, Register x, Register b)`：** 
    自动计算并发射 REX 前缀（0x40 - 0x4F）。处理 64 位操作数（W=1）以及扩展寄存器（R8-R15 触发 R, X, B 标志位）。
2.  **`emitOpcode(uint8_t op)` / `emitOpcode(uint8_t op1, uint8_t op2)`：** 
    发射单字节或双字节（如 `0x0F` 开头）操作码。
3.  **`emitModRM(int mod, Register reg, Register rm)`：** 
    计算 ModR/M 字节，决定操作数是寄存器还是内存。
4.  **`emitSIB(Scale scale, Register index, Register base)`：** 
    当 ModR/M 指示需要 SIB 时，计算并发射 SIB 字节。
5.  **`emitDisp(int32_t disp)` / `emitImm(int32_t imm)`：** 
    发射位移和立即数（小端序）。

### 12.3 标签与控制流回填 (Labels & Backpatching)
在发射前向跳转（Forward Jump）时，目标地址尚未可知。
*   **`Label` 类：** 维护两个状态：`isBound`（是否已绑定地址）和 `pos`（绑定的字节偏移量）。如果未绑定，它会维护一个 `unresolvedJumps` 链表，记录所有跳向它的指令偏移量。
*   **`jmp(Label& L)` / `jcc(Condition cond, Label& L)`：** 
    如果 `L` 已绑定，直接计算相对偏移量 `offset = L.pos - current_pos - 4` 并发射。如果未绑定，发射一个占位的 `0x00000000`，并将当前位置加入 `L.unresolvedJumps`。
*   **`bind(Label& L)`：** 
    标记 `L` 为已绑定，记录当前汇编缓冲区的偏移量。然后遍历 `L.unresolvedJumps`，计算真实的相对偏移量，并**回填（Patch）**到之前占位的字节处。

### 12.4 可执行内存管理 (Executable Memory Allocator)
操作系统默认禁止在堆或栈上执行代码（DEP/NX bit）。
*   **`ExecutableMemory` 类：** 封装跨平台的内存分配。
    *   Windows: 使用 `VirtualAlloc(..., PAGE_READWRITE)` 分配。
    *   POSIX (Linux/macOS): 使用 `mmap(..., PROT_READ | PROT_WRITE, ...)` 分配。
*   **`finalize()` 阶段：** 
    当机器码全部写入后，必须调用 `VirtualProtect` 或 `mprotect` 将内存权限修改为 `PAGE_EXECUTE_READ` (RX)。
    **关键：** 修改权限后，必须调用指令缓存刷新（Windows: `FlushInstructionCache`，GCC/Clang: `__builtin___clear_cache`），否则 CPU 可能会执行流水线中残留的旧指令。

## 13. 寄存器分配器设计 (Linear Scan Register Allocator)

为了在极短的编译时间内生成高质量的机器码，JIT 采用**线性扫描寄存器分配算法 (LSRA)**，而非 AOT 编译器常用的图着色算法。它的核心任务是将 LIR 中无限的虚拟寄存器 (VirtualReg) 映射到 x86-64 有限的物理寄存器 (PhysicalReg) 或栈槽 (StackSlot) 上。

### 13.1 活跃区间分析 (Liveness Analysis)
在分配前，必须计算每个虚拟寄存器的生命周期：
*   **Live Interval (活跃区间)：** 记录一个虚拟寄存器从第一次被定义 (Def) 到最后一次被使用 (Use) 的 LIR 指令编号范围 `[start, end]`。
*   **Live Holes (活跃空洞)：** 工业级 LSRA 需要支持区间空洞（例如变量在循环内部未被使用），以提高寄存器复用率。

### 13.2 线性扫描与溢出策略 (Linear Scan & Spilling)
按指令编号递增的顺序扫描所有活跃区间，维护一个按结束位置排序的“活跃列表 (Active List)”：
*   **分配：** 遇到新的区间时，从空闲物理寄存器池中分配一个。
*   **回收：** 释放活跃列表中 `end` 小于当前指令位置的物理寄存器。
*   **溢出 (Spill)：** 当没有空闲物理寄存器时，选择活跃列表中**最晚结束 (Latest End Point)** 的区间，将其溢出到机器栈上。分配器会自动在 LIR 中插入 `SpillStore` 和 `SpillLoad` 指令。

### 13.3 物理寄存器约束处理 (Handling Constraints)
x86-64 架构对寄存器有严苛的硬性要求，分配器必须满足 LIRBuilder 注入的约束：
*   **预着色区间 (Pre-colored Intervals)：** 如果 LIR 指令要求操作数必须在 `RCX` 中（如位移指令 `shl`），分配器会在该指令处创建一个极短的、固定在 `RCX` 的活跃区间。
*   **冲突解决：** 如果 `RCX` 已经被其他变量占用，分配器会强制将原变量驱逐（Evict）到其他寄存器或栈上，并在前后插入 `mov` 指令。
*   **分配提示 (Allocation Hints)：** 尽量将有数据流关联的虚拟寄存器（如 Phi 节点或 `mov` 的源和目标）分配到同一个物理寄存器，从而在最终生成机器码时消除这些 `mov` 指令。

### 13.4 调用约定与寄存器保护 (Calling Conventions & Save/Restore)
JIT 代码在执行过程中经常需要调用 C++ 运行时函数（如分配内存、抛出异常）：
*   **ABI 兼容：** 分配器必须知道哪些是调用者保存寄存器 (Caller-Saved，如 `RAX`, `RCX`, `RDX`)，哪些是被调用者保存寄存器 (Callee-Saved，如 `RBX`, `R12-R15`)。
*   **跨调用存活：** 如果一个虚拟寄存器的活跃区间跨越了 C++ 函数调用 (Call 指令)，分配器应优先将其分配到 Callee-Saved 寄存器中。如果只能分配到 Caller-Saved 寄存器，分配器必须在 Call 指令前后自动插入 `push` 和 `pop` 进行现场保护。

## 14. JIT 稳健开发路线图 (Expanded Industrial Roadmap)

为了保证 JIT 编译器的绝对稳定，避免难以调试的机器码崩溃，整个开发过程被严格拆分为 45 个微小步骤。每一步都必须独立验证，绝不大步迈进。

### Phase 1: 基础设施与可执行内存 (Steps 1-5)
*   **Step 1:** 定义 `ExecutableMemory` 类的基础结构（头文件设计）。
*   **Step 2:** 实现 Windows 下的 `VirtualAlloc` 和 `VirtualFree` 逻辑。
*   **Step 3:** 实现 POSIX (Linux/macOS) 下的 `mmap` 和 `munmap` 逻辑。
*   **Step 4:** 实现内存权限修改 (`VirtualProtect`/`mprotect`) 与指令缓存刷新 (`FlushInstructionCache`)。
*   **Step 5:** **[验证]** 编写 C++ 单元测试：分配内存，手动写入 `0xC3` (x86 `ret` 指令)，转换为函数指针并成功调用。

### Phase 2: 宏汇编器核心指令集 (MacroAssembler Core) (Steps 6-16)
*   **[已完成] Step 6:** 定义强类型的 `Register` 和 `XMMRegister` 类，预定义 `rax`, `xmm0` 等常量。
*   **[已完成] Step 7:** 定义 `Operand` 类，支持 Base + Index * Scale + Disp 内存寻址模式。
*   **[已完成] Step 8:** 实现 `MacroAssembler` 的底层 Buffer 管理（动态扩容的字节数组）。
*   **[已完成] Step 9:** 实现 x86-64 的 REX 前缀发射逻辑 (`emitRex`)。
*   **[已完成] Step 10:** 实现 ModR/M 和 SIB 字节的计算与发射逻辑。
*   **[已完成] Step 11:** 实现基础 ALU 与逻辑指令 (`mov`, `add`, `sub`, `cmp`, `test`, `and`, `or`, `xor`)，支持寄存器与内存操作数。
*   **[已完成] Step 12:** 实现复杂 ALU 指令 (`imul`, `idiv`, `cdq`/`cqo`, `shl`, `shr`, `sar`) 及隐式寄存器约束处理。
*   **[已完成] Step 13:** 实现栈操作与 64 位立即数加载 (`push`, `pop`, `movabs`)。
*   **[已完成] Step 14:** 实现浮点标量指令 (SSE2: `movsd`, `addsd`, `subsd`, `mulsd`, `divsd`, `ucomisd`, `cvtsi2sd`, `cvttsd2si`)。
*   **[已完成] Step 15:** 实现 C++ ABI 辅助封装 (Prologue/Epilogue, 16字节栈对齐, Windows 32字节 Shadow Space 分配)。
*   **[已完成] Step 16:** **[验证]** 编写单元测试：发射包含浮点运算和 C++ 函数调用的机器码，验证 ABI 兼容性。

### Phase 3: 控制流与常量池 (Control Flow & Constant Pool) (Steps 17-21)
*   **[已完成] Step 17:** 定义 `Label` 类，维护绑定状态和未决跳转链表。
*   **[已完成] Step 18:** 实现无条件跳转 `jmp`、条件跳转 `jcc` 和函数调用 `call` 的发射逻辑（支持 32 位相对偏移）。
*   **[已完成] Step 19:** 实现 `bind(Label)` 逻辑，完成相对偏移量的计算与回填 (Patch)。
*   **[已完成] Step 20:** 实现常量池 (Constant Pool) 与 RIP 相对寻址 (用于高效加载 Double 常量和 64 位指针)。
*   **[已完成] Step 21:** **[验证]** 编写单元测试：发射一段包含循环、条件分支和常量池加载的汇编代码，执行并验证结果。

### Phase 4: 解释器类型收集 (Tier 0 Profiling) (Steps 22-26)

**【设计方案：零开销类型收集 (Zero-Overhead Profiling)】**
为了绝对不拖慢现有的 Tier 0 解释器，我们采用以下极简设计：
1.  **O(1) 映射的反馈向量**：在 `Chunk` 中新增 `std::vector<uint8_t> typeFeedback`，其大小与 `code`（字节码指令数组）完全一致。这样通过 `ip - 1` 即可 O(1) 直接访问当前指令的反馈槽，无需任何哈希查找，内存开销仅为 1 字节/指令。
2.  **位掩码状态机 (Bitmask States)**：使用 8 个比特位记录类型突变：
    *   `0x00`: 未执行 (Uninitialized)
    *   `0x01`: 纯 Int32 (Monomorphic Int32)
    *   `0x02`: 纯 Double (Monomorphic Double)
    *   `0x04`: 纯 String (Monomorphic String)
    *   `0x08`: 纯 Bool (Monomorphic Bool)
    *   `0x10`: 结果溢出或类型突变 (Overflow / Fraction Output) - 工业级 JIT 必须记录输出类型的突变，例如 Int32 相加溢出，或 Int32 相除产生 Fraction。
    *   `0x80`: 其他对象/复杂类型 (Megamorphic)
    *   组合状态（如 `0x03` 表示既有 Int32 又有 Double，即 Polymorphic Number）。
3.  **顺风车收集 (Piggybacking)**：绝不在解释器中新增专门的 `if` 类型判断分支。解释器的 `OpCode::ADD` 等指令原本就已经有 `if (vb.isInt32() && vc.isInt32())` 的快速通道。我们只需在这些现有的快速通道内部，追加一条极快的位或运算 `chunk->typeFeedback[ip - 1] |= 0x01;` 即可。
4.  **热点计数器与延迟收集 (Warm-up Profiling)**：在 `CompiledFunction` 中增加 `uint32_t callCount`。
    *   为了避免引入 `if (callCount > 50)` 导致解释器内层循环变慢，我们**无条件**在内层循环执行 `|=` 收集（因为位运算比分支判断更快）。
    *   为了解决“早期类型污染”（前几次调用传入了非稳态类型），我们在函数入口处判断：当 `callCount == 50` 时，清空（`memset` 为 0）之前的 `typeFeedback`，重新开始收集稳态类型。
    *   当 `callCount == 1000` 时，触发 JIT 编译。

*   **[已完成] Step 22:** 在 `Chunk` 中定义 `typeFeedback` 数组，在 `CompiledFunction` 中定义 `callCount`。
*   **[已完成] Step 23:** 在 `BytecodeSerializer` 中处理 `typeFeedback` 的序列化与反序列化（初始化为 0）。
*   **[已完成] Step 24:** 改造解释器的所有核心算术、逻辑、比较和位运算指令，实现工业级的输入/输出类型收集。
*   **[已完成] Step 25:** 在函数入口处实现 `callCount` 递增与 50 次调用时的类型洗牌逻辑。
*   **[已完成] Step 26:** **[验证]** 编写 JC2 测试脚本，运行后打印 `typeFeedback` 数据，确认类型收集准确无误且性能无损。

### Phase 5: HIR 数据结构与构建器 (HIR Structures) (Steps 27-31)
*   **[已完成] Step 27:** 定义 `JITType` 枚举和 `HIRNode` 基类。
*   **[已完成] Step 28:** 实现 `HIRNode` 的 Use-Def 和 Def-Use 链管理（设为 private）。
*   **[已完成] Step 29:** 定义具体的 HIR 节点类（如 `Int32Constant`, `AddI32`, `GuardIsInt32`, `FrameState`）。
*   **[已完成] Step 30:** 实现 `HIRBuilder` API（如 `createAddI32`），封装节点创建和连边逻辑。
*   **[已完成] Step 31:** **[验证]** 编写单元测试：手动调用 Builder 构建一个微型图，并打印为 Graphviz DOT 格式。

### Phase 6: 字节码到 HIR 的转换 (Bytecode to HIR) (Steps 32-35)
*   **[已完成] Step 32:** 实现字节码的基本块 (Basic Block) 划分算法。
*   **[已完成] Step 33:** 实现抽象解释器的主循环，维护 256 个虚拟寄存器到 HIR 节点的映射表。
*   **[已完成] Step 34:** 实现线性字节码（如 `LOADK`, `MOVE`）到 HIR 的转换（复写传播）。
*   **[已完成] Step 35:** 结合 Profiling 数据，实现 `OpCode::ADD` 的类型特化（注入 `Guard` 和 `Unbox` 节点）。
*   **[已完成] Step 35.5:** **[验证]** 编写单元测试：手动构造包含 `LOADK` 和 `ADD` 的 Chunk，并验证生成的 HIR 图。

### Phase 7: LIR 与指令选择 (LIR & Instruction Selection) (Steps 36-39)
*   **[已完成] Step 36:** 定义 `LIRInst`, `LIROperand` (Virtual/Physical/Stack) 和 `LIRBlock`。
*   **[已完成] Step 37:** 实现 `LIRBuilder` API，支持物理寄存器约束的声明。
*   **[已完成] Step 38:** 实现全局代码移动 (GCM) 算法，将无序的 HIR 节点调度到 LIR 基本块中。
*   **[已完成] Step 39:** 实现指令选择器，将 HIR 的 `AddI32` 降级为 LIR 的 `add` 指令。

### Phase 8: 寄存器分配 (Register Allocation) (Steps 40-42)
*   **[已完成] Step 40:** 实现活跃区间分析 (Liveness Analysis)，计算每个 VirtualReg 的生命周期。
*   **[已完成] Step 41:** 实现线性扫描寄存器分配器 (LSRA) 的核心分配与回收逻辑。
*   **[已完成] Step 42:** 在 LSRA 中处理 x86-64 的固定物理寄存器约束和栈溢出 (Spilling)。

### Phase 9: 代码生成与去优化 (Code Gen & Deoptimization) (Steps 43-45)
*   **[已完成] Step 43:** 遍历分配好寄存器的 LIR，调用 `MacroAssembler` 生成最终机器码。
*   **[已完成] Step 44:** 实现去优化跳板 (Deopt Trampoline) 的汇编代码，保存所有物理寄存器。
*   **[已完成] Step 45:** 实现 C++ `Deoptimize` 运行时函数，根据 Stack Map 重建解释器 `CallFrame` 并平滑回退。
*   **[已完成] Step 45.5:** **[验证]** 编写端到端测试 (`test_jit_pipeline.cpp`)，成功验证包含控制流分支、Phi 节点汇聚、64位指针与 NaN-Boxing 数据的完整 JIT 编译管线。

## 15. JIT 与类型系统及 GC 的交互 (JIT, Type System & GC Interaction)

JC2 的运行时具有两个显著特征：**64位 NaN-Boxing** 和 **Mark-and-Sweep + 引用计数 (Ref Count) 混合内存管理**。JIT 生成的机器码必须与这些机制完美契合，否则会导致严重的内存破坏。

### 15.1 NaN-Boxing 的机器码特化 (Fast Type Checks)
JIT 的核心优势在于消除 C++ 层面的类型分支。在机器码层面，我们需要将 `Value.h` 中的宏翻译为极速的汇编指令：
*   **Check IsInt32:** 
    JC2 的 Int32 掩码是 `0x7FFC000100000000`。
    汇编实现：将 64 位 `Value` 逻辑右移 32 位，与 `0x7FFC0001` 比较。如果不等，则触发去优化 (Deopt)。
*   **Check IsObject:**
    JC2 的 Obj 掩码是 `SIGN_BIT | QNAN` (`0xFFFC000000000000`)。
    汇编实现：将 64 位 `Value` 逻辑右移 48 位，与 `0xFFFC` 比较。
*   **Check IsDouble:**
    只要不带有 `QNAN` (`0x7FFC000000000000`) 掩码的都是 Double。

### 15.2 引用计数屏障 (Write Barriers for Ref Counting)
JC2 的 `Value` 赋值操作符 (`operator=`) 内部包含了复杂的逻辑：增加新对象的 `refCount`，减少旧对象的 `refCount`，如果归零则调用 `GcHeap::freeObj`。
**致命陷阱：** 如果 JIT 机器码直接使用 `mov [dst], rax` 覆盖一个寄存器，就会破坏引用计数，导致内存泄漏或 Use-After-Free。
**解决方案：**
*   **纯数值操作 (Unboxed)：** 如果 JIT 确信某个虚拟寄存器只存储原生 Int32 或 Double，可以直接使用 `mov`，因为它们不涉及堆内存。
*   **对象赋值 (Boxed Value)：** 当 JIT 需要将一个装箱的 `Value` 写入 `VM::registers` 时，**绝对不能**直接发射 `mov`。必须发射一个 `Call` 指令，调用 C++ 侧的辅助函数（Trampoline），例如 `jc2_jit_assign_value(Value* dst, Value src)`，让 C++ 来安全地处理引用计数。

### 15.3 GC 安全点与状态同步 (GC Safepoints & Eager Sync)
当 JIT 代码调用 C++ 运行时函数（例如分配新对象、抛出异常、或者调用未被 JIT 的复杂内置函数）时，C++ 内部可能会触发 `GcHeap::collectGarbage()`。
JC2 的 GC 会扫描 `VM::registers` 数组作为根节点 (Roots)。
**致命陷阱：** 在 JIT 执行期间，最新的变量值可能存在于 CPU 的物理寄存器（如 `RAX`）中，而 `VM::registers` 里的值是过期的。如果此时触发 GC，存活的对象可能会被误杀。
**解决方案 (Eager Register Sync)：**
为了避免实现极其复杂的 Stack Map 扫描器，我们采用工业级 JIT 常用的“主动同步”策略：
1.  **Safepoint 标记：** 任何可能触发 GC 的 C++ 调用点都被标记为 Safepoint。
2.  **寄存器刷回 (Spill to VM)：** 在发射 Call 指令调用 C++ 之前，`MacroAssembler` 必须将当前所有活跃的、且被修改过的虚拟寄存器，从物理寄存器刷回 (Write-Through) 到 `VM::registers` 对应的内存槽位中。
3.  **装箱 (Re-boxing)：** 如果物理寄存器里存的是拆箱后的原生 Int32，刷回时必须加上 `INT32_MASK` 重新装箱。
通过这种方式，当 C++ 侧触发 GC 时，`VM::registers` 永远处于完美的一致状态，现有的 GC 标记逻辑无需任何修改即可安全运行。

## 16. 复杂类型与原生函数调用 (Complex Types & Native Calls)

JIT 的核心优势在于极速处理标量（Int32, Double, Bool）的控制流和运算。对于矩阵计算、大整数、字符串拼接以及 C++ 原生函数调用，JIT 绝不能试图将它们展开为内联汇编，而必须通过安全的边界调用交还给 C++ 运行时处理。

### 16.1 运行时辅助函数 (Runtime Callouts / Trampolines)
对于包含 `BUILD_MATRIX`、`CLASS`、`NAMESPACE` 等复杂指令的函数，JIT 采用“控制流内联 + 复杂操作呼出 (Callout)”的核心策略。
当 Tier 0 Profiler 发现某个 `OpCode::ADD` 的操作数是 `RealMatrix`，或者遇到无法直接用简单机器码表达的复杂指令时，JIT 不会尝试生成内联汇编，而是生成一个 **Callout（呼出）** 节点：
1.  **控制流与简单指令正常展开：** 内联时，目标函数内的基本控制流（如 `if`、`while`）和简单运算依然会被正常展开并织入当前的 HIR 图中，享受寄存器分配和常量折叠等优化。
2.  **复杂指令降级为 Callout 节点：** 遇到复杂指令时，生成一个指向专门的 C++ 运行时辅助函数（例如 `jc2_jit_build_matrix`）的 Callout 节点，并将所需参数传递给它。
3.  **强制状态同步 (Eager Sync) 保障 GC 安全：** 由于复杂指令通常涉及堆内存分配，极易触发垃圾回收 (GC)。生成的 CalloutNode 必须挂载当前的 `FrameState`。在执行 Callout 跃迁到 C++ 之前，JIT 会自动将所有活跃的物理寄存器装箱并刷回 `VM::registers`，确保 GC 扫描时内存状态绝对一致。
4.  **结果接收：** C++ 辅助函数执行完毕后，将构建好的复杂对象（如 Matrix 或 Class）作为 `Value` 返回给 JIT（通常放在 `RAX` 寄存器中），JIT 代码继续向下执行。
这种设计使得 JIT 代码极其紧凑，把函数的外壳拆掉，将其内部的复杂指令转化为对 C++ 运行时的安全调用，从而在保留 JIT 优化空间的同时，完全复用了 C++ 侧高度优化的复杂逻辑。

### 16.2 C++ ABI 兼容与现场保护 (ABI Compliance & Context Saving)
在 JIT 机器码中调用 C++ 函数是极其危险的操作，必须严格遵守操作系统的 C ABI（Application Binary Interface）：
*   **寄存器传参：** Windows x64 要求前 4 个参数放在 `RCX, RDX, R8, R9`；System V (Linux/macOS) 要求放在 `RDI, RSI, RDX, RCX, R8, R9`。`MacroAssembler` 必须根据宏定义自动抹平这种跨平台差异。
*   **栈对齐 (Stack Alignment)：** 在执行 `call` 指令前，机器栈指针 `RSP` 必须严格对齐到 16 字节边界，否则 C++ 内部的 SIMD 指令（如 `movaps`）会直接触发段错误 (Segfault) 崩溃。
*   **影子空间 (Shadow Space)：** Windows x64 强制要求调用者在栈上预留 32 字节的空间，供被调用函数溢出寄存器使用。
*   **Caller-Saved 寄存器保护：** C++ 函数有权破坏 `RAX, RCX, RDX, R8-R11, XMM0-XMM5` 等寄存器。JIT 必须在 `call` 之前将这些寄存器中存活的虚拟寄存器压栈保护（或由寄存器分配器提前驱逐）。

### 16.3 内置函数的内联替换 (Builtin Intrinsics)
对于极高频且简单的数学函数（如 `math.sin`, `math.sqrt`, `math.abs`），如果走常规的 C++ 调用开销太大。
*   **Intrinsics 机制：** JIT 编译器内部维护一个“已知函数表”。如果发现代码在调用 `math.sqrt`，且参数被推导为 `Double`，JIT 会直接拦截这个调用。
*   **直接发射 FPU 指令：** JIT 不再生成 `call`，而是直接发射一条 x86-64 的硬件指令 `sqrtsd xmm0, xmm1`。这能将数学运算的性能提升数十倍。

### 16.4 泛型 Native 函数调用 (Generic Native Calls)
对于用户通过 `import` 引入的 C++ 扩展库（如 `image`, `tensor`），JIT 无法提前知道它们的签名。
*   当 JIT 遇到对 `NativeCallable` 的调用时，它会生成一段特殊的汇编跳板。
*   这段跳板会在机器栈上动态构造一个 `std::vector<Value>`，将 JIT 寄存器中的参数打包进去，然后调用 C++ 的 `std::any_cast<NativeCallable>` 包装器。
*   这保证了 JIT 代码可以无缝、安全地与任何现有的 JC2 扩展库交互。

## 17. 除法语义与整除操作符 (Division Semantics & `~/`)

在 JC2 中，普通的除法 `/` 具有精确的数学语义：如果两个整数不能整除，会返回一个精确的 `Fraction` 对象。然而，x86-64 的硬件指令 `idiv` 只能执行截断除法。为了在 JIT 中高效且正确地处理除法，我们采用以下混合架构：

### 17.1 输出类型 Profiling (Output Type Profiling)
对于普通的 `/` 运算符，JIT 依赖 Tier 0 解释器收集的**输出类型反馈**：
*   **乐观整除假设：** 如果 Profiler 显示某处的 `/` 100% 输出 `Int32`，JIT 会发射 `idiv` 指令，并紧跟一个**余数守卫 (Remainder Guard)**。它检查 `EDX` 寄存器，如果 `EDX != 0`，则触发去优化 (Deoptimization)，退回解释器生成 `Fraction`。
*   **分数回退 (Fraction Fallback)：** 如果 Profiler 发现该除法曾经输出过 `Fraction`，JIT 将放弃内联汇编，直接生成一个 Callout，调用 C++ 运行时的除法函数。

### 17.2 引入整除操作符 `~/`
为了给性能敏感的场景提供极致的整数除法速度，我们在语言层面引入专用的整除操作符 `~/`（因为 `//` 已被用作注释）。
*   **语义：** `~/` 强制执行向零截断的整数除法。
*   **JIT 编译：** 当 JIT 遇到 `~/` 操作符，且操作数被推导为 `Int32` 时，JIT 直接发射 `idiv` 指令，并**直接丢弃 `EDX` 寄存器中的余数**，不需要任何 Guard 检查，实现零开销的机器码执行。
*   **安全优势：** 相比于将 `idiv()` 函数作为 Intrinsic 内联，使用专用操作符避免了动态语言中函数名被用户重写（Shadowing）导致的语义破坏问题。

## 18. JIT 进阶扩展路线图 (Advanced Expansion Roadmap)

为了让 JIT 能够加速真实世界中复杂的 JC2 脚本，接下来的开发将围绕循环、内存访问、函数内联和 OSR 展开。以下是严格拆分的 6 个 Phase，共 36 个微小步骤：

### Phase 11: 中端优化 Pass (Mid-level Optimizations) (Steps 48-53)
*   **[已完成] Step 48:** 实现常量折叠 (Constant Folding) 基础框架，支持算术指令的预计算。
*   **[已完成] Step 49:** 实现死代码消除 (Dead Code Elimination, DCE)，基于 Use-Def 链反向标记存活节点。
*   **[已完成] Step 50:** 实现全局值编号 (Global Value Numbering, GVN) 核心哈希表，用于识别等价节点。
*   **[已完成] Step 51:** 在 GVN 中实现公共子表达式消除 (CSE)，合并相同的算术与逻辑节点。
*   **[已完成] Step 52:** 实现代数化简 (Algebraic Simplification)，如 `x * 1 -> x`, `x + 0 -> x`。
*   **[已完成] Step 53:** 将中端优化 Pass 集成到 `BytecodeToHIR` 之后、`GCM` 调度之前，并编写测试验证图的精简。

### Phase 12: 循环与前向数据流 (Loops & Loop Phis) (Steps 54-59)
*   **[已完成] Step 54:** 扩展 `BytecodeCFG`，识别循环头 (Loop Header) 和循环回边 (Back-edge)。
*   **[已完成] Step 55:** 在 `BytecodeToHIR` 中实现“乐观 Phi 插入”，在遇到循环头时为所有活跃寄存器预创建 Phi 节点。
*   **[已完成] Step 56:** 实现循环体解析完毕后的回边数据流绑定，将回边变量接入预创建的 Phi 节点。
*   **[已完成] Step 57:** 实现死 Phi 节点消除 (Dead Phi Elimination)，清理循环中未被实际修改的冗余 Phi 节点。
*   **[已完成] Step 58:** 在 `GCM` 中完善对循环节点的调度支持，确保循环不变量外提 (LICM) 正常工作。
*   **[已完成] Step 59:** 编写端到端测试，验证包含 `while` 和 `for` 循环的字节码能被正确编译为高效机器码。

### Phase 13: 内存访问与内联缓存特化 (Memory Access & IC Specialization) (Steps 60-65)
*   **[已完成] Step 60:** 在 HIR 中引入 `LoadGlobal` 和 `StoreGlobal` 节点，并在 LIR/MacroAssembler 中实现绝对地址寻址。
*   **[已完成] Step 61:** 修改 `BytecodeToHIR`，读取 `GET_GLOBAL` 的 IC 数据，命中时直接生成 `LoadGlobal` 节点。
*   **[已完成] Step 62:** 在 HIR 中引入 `GuardIsClass` 和 `LoadField`/`StoreField` 节点，支持对象属性的内存偏移访问。
*   **[已完成] Step 63:** 修改 `BytecodeToHIR`，读取 `GET_PROP` 的 IC 数据，命中时生成类守卫与直接内存读取。
*   **[已完成] Step 64:** 在 LIR 和寄存器分配器中处理内存访问指令的物理寄存器约束（如基址寄存器分配）。
*   **[已完成] Step 65:** **[验证]** 编写端到端测试，验证面向对象代码（类属性读写）在 JIT 下的极速执行。

### Phase 14: 数学内联函数 (Math Intrinsics) (Steps 66-71)
*   **[已完成] Step 66:** 在 HIR 中引入专用的数学硬件节点（如 `SqrtF64`, `SinF64`, `CosF64`, `AbsF64`, `FloorF64` 等）。
*   **[已完成] Step 67:** 在 `MacroAssembler` 中实现对应的 x86-64 FPU/SSE2 硬件指令发射（如 `sqrtsd`, `roundsd`, `fsin`, `fcos` 等）。
*   **[已完成] Step 68:** 修改 `BytecodeToHIR`，在解析 `CALL` 时识别目标是否为已知的 `math` 内置函数。
*   **[已完成] Step 69:** 结合 Profiling 数据，如果参数为 `Double`，则将内置函数调用直接替换为对应的 HIR 数学节点。
*   **[已完成] Step 70:** 实现对 Int32 参数的自动类型提升（Int32 -> Double），以扩大 Intrinsics 的适用范围。
*   **[已完成] Step 71:** **[验证]** 编写性能基准测试，验证数学密集型代码（如计算素数或几何距离）的性能飞跃。

### Phase 15: 函数调用与内联 (Function Calls & Inlining) (Steps 72-77)
*   **[已完成] Step 72:** 在 HIR 中引入 `Callout` 节点，用于 JIT 代码安全地调用 C++ 运行时函数。
*   **[已完成] Step 73:** 在 `MacroAssembler` 和 `LinearScan` 中实现 Caller-Saved 寄存器的自动溢出与恢复机制。
*   **[已完成] Step 74:** 实现 Eager Sync 机制，在 Callout 前将活跃的虚拟寄存器刷回 `VM::registers` 以保证 GC 安全。
*   **[已完成] Step 75:** 实现 JIT-to-JIT 直接调用，跳过解释器 `CallFrame` 创建，直接 `call` 目标机器码入口。
*   **[已完成] Step 76:** 在 `BytecodeToHIR` 中实现基础的函数内联 (Method Inlining) 启发式算法（基于函数大小和调用深度）。
*   **[已完成] Step 77:** 实现内联展开逻辑，将被调用函数的字节码直接并入当前 HIR 图中，并消除参数传递开销。

### Phase 16: 栈上替换 (OSR - On-Stack Replacement) (Steps 78-83)
*   **[已完成] Step 78:** 在解释器的循环回边指令（如 `JMP` 往回跳时）增加独立的 OSR 热点计数器。
*   **[已完成] Step 79:** 当 OSR 计数器达标时，触发后台 JIT 编译，并标记该编译任务为 OSR 模式。
*   **[已完成] Step 80:** 在 OSR 模式的 HIR 构建中，生成特殊的 `OSREntry` 节点，代替常规的 `Start` 节点。
*   **[已完成] Step 81:** 在 `MacroAssembler` 中实现 OSR Prologue，负责从解释器栈中读取当前变量并装载到物理寄存器中。
*   **[已完成] Step 82:** 在解释器中实现热切换逻辑：发现 OSR 机器码就绪后，直接 `jmp` 跃入机器码的循环体。
*   **[已完成] Step 83:** **[验证]** 编写端到端测试，验证“单次调用但包含死循环”的函数能够被成功 OSR 加速。

### Phase 17: 复杂指令的 Callout 降级与内联支持 (Steps 84-88)
*   **[已完成] Step 84:** 在 C++ 侧实现一系列 JIT 专用的运行时辅助函数（如 `jc2_jit_build_matrix`, `jc2_jit_build_class` 等）。
*   **[已完成] Step 85:** 扩展 `BytecodeToHIR`，在遇到 `BUILD_MATRIX`, `CLASS`, `NAMESPACE` 等复杂指令时，生成对应的 `Callout` 节点。
*   **[已完成] Step 86:** 确保 `Callout` 节点正确挂载 `FrameState`，并在 `InstructionSelector` 中完善 Eager Sync 机制，保证 GC 触发时的内存安全。
*   **[已完成] Step 87:** 调整内联启发式算法（Heuristics），允许包含复杂指令的函数被内联（仅展开其控制流和简单指令，复杂指令走 Callout）。
*   **[已完成] Step 88:** 编写端到端测试，验证包含矩阵构建和类定义的函数在被内联后，既能享受 JIT 加速，又不会发生 GC 崩溃。

### Phase 18: 算术与逻辑指令的超态回退 (Megamorphic Math Fallbacks) (Steps 89-93)
*   **[已完成] Step 89:** 在 C++ 侧实现超态算术运算的 JIT 辅助函数（涵盖 `ADD`, `SUB`, `MUL`, `DIV`, `IDIV`, `MOD`, `POW`, `LDIV`）。
*   **[已完成] Step 90:** 在 C++ 侧实现超态位运算与一元运算的 JIT 辅助函数（涵盖 `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, `UNM`, `BNOT`）。
*   **[已完成] Step 91:** 在 C++ 侧实现超态比较运算的 JIT 辅助函数（涵盖 `EQ`, `NEQ`, `LT`, `LE`, `GT`, `GE`）。
*   **[已完成] Step 92:** 修改 `BytecodeToHIR`，当算术/逻辑/比较指令的类型反馈为 `0x80` (超态) 或 `0x10` (溢出/突变) 时，不再忽略或去优化，而是生成对应的 `Callout` 节点。
*   **[已完成] Step 93:** 编写端到端测试，验证包含矩阵运算、字符串拼接和大整数计算的循环能够被 JIT 成功编译并极速执行，全程无去优化。

### Phase 19: 动态属性与索引访问 (Dynamic Properties & Indexing) (Steps 94-98)
*   **Step 94:** 在 C++ 侧实现动态属性访问的 JIT 辅助函数（涵盖 `GET_PROP`, `SET_PROP`, `TRY_GET_PROP`），用于处理 Inline Cache (IC) 未命中的情况。
*   **Step 95:** 修改 `BytecodeToHIR`，将属性访问的 IC Miss 路径从生成 `Deoptimize` 节点改为生成 `Callout` 节点，彻底消除多态对象导致的去优化。
*   **Step 96:** 在 C++ 侧实现 1D 和 2D 索引访问的 JIT 辅助函数（涵盖 `INDEX_GET`, `INDEX_SET`）。
*   **Step 97:** 修改 `BytecodeToHIR`，全面支持 `INDEX_GET` 和 `INDEX_SET` 指令，将其降级为 `Callout` 节点。
*   **Step 98:** 编写端到端测试，验证包含字典键值查找、矩阵切片读写和动态对象属性操作的函数在 JIT 下的正确性与 GC 安全性。

### Phase 20: 迭代器与 For-In 循环 (Iterators & For-In Loops) (Steps 99-103)
*   **Step 99:** 在 C++ 侧实现迭代器初始化的 JIT 辅助函数（对应 `ITER_INIT` 指令）。
*   **Step 100:** 在 C++ 侧实现迭代器步进的 JIT 辅助函数（对应 `ITER_NEXT` 指令）。
*   **Step 101:** 修改 `BytecodeToHIR`，支持将 `ITER_INIT` 和 `ITER_NEXT` 降级为 `Callout` 节点。
*   **Step 102:** 调整 `BytecodeCFG` 和 `BytecodeToHIR` 的控制流分析逻辑，确保 `for-in` 循环产生的隐式分支和循环头能够被正确识别并生成 Phi 节点。
*   **Step 103:** 编写端到端测试，验证遍历 List、Dict 和 Matrix 的 `for-in` 循环能够被 JIT 完美加速。

### Phase 21: 模式匹配与高级类型检查 (Pattern Matching & Type Checks) (Steps 104-108)
*   **Step 104:** 在 C++ 侧实现类型与形状匹配的 JIT 辅助函数（涵盖 `MATCH_TYPE`, `MATCH_SHAPE`, `MATCH_INIT`）。
*   **Step 105:** 在 C++ 侧实现成员包含测试的 JIT 辅助函数（对应 `IN` 关键字）。
*   **Step 106:** 修改 `BytecodeToHIR`，支持将 `MATCH_TYPE`, `MATCH_SHAPE`, `MATCH_INIT` 和 `IN` 降级为 `Callout` 节点。
*   **Step 107:** 再次放宽 `BytecodeToHIR` 中的内联启发式算法，允许包含模式匹配和 `for-in` 循环的复杂函数被内联展开。
*   **Step 108:** 编写终极综合测试，验证一个包含深度解构赋值、模式匹配和复杂类型检查的算法，在 JIT 编译管线中稳定、极速地运行。
