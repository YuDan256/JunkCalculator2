<div align="right">
  <a href="README.md">English</a> | <strong>简体中文</strong>
</div>

# Junk Calculator 2.6.0.0

![Version](https://img.shields.io/badge/Version-v2.6.0.0-orange.svg?style=flat-square)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B)
![Zero Dependencies](https://img.shields.io/badge/Dependencies-0-brightgreen.svg?style=flat-square)
![CMake](https://img.shields.io/badge/CMake-3.15+-064F8C.svg?style=flat-square&logo=cmake)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)

一个基于 C++20 实现的脚本语言及计算机代数系统 (CAS)。该方案采用自定义字节码编译器和基于寄存器的虚拟机执行，**完全无第三方依赖**。

由清华大学 Yu Liangyang 开发。

---

## 技术概览

### 底层架构
- **词法分析器 (Lexer)**：支持超过 55 种词法单元，涵盖字符串插值 (`f""`)、自定义定界符原始字符串 (`r"TAG()TAG"`)、交替单双引号、虚数后缀 (`3i`) 以及可变长参数 (`...`)。
- **语法分析器 (Parser)**：递归下降解析器，生成包含 30 余种节点类型的抽象语法树 (AST)。支持运算符优先级、块语句、逗号序列求值以及解构提取。
- **编译器 (Compiler)**：引入基于“节点海 (Sea of Nodes)”的中间表示 (IR) 和静态单赋值 (SSA) 形式。包含多趟优化流水线（CSE、DCE、常量折叠等），并采用图着色算法进行寄存器分配。
- **即时编译器 (JIT)**：基于类型特化的即时编译器，涵盖 HIR 提升、LIR 指令选择、线性扫描寄存器分配、x86-64 代码生成、栈上替换 (OSR) 以及基于栈映射的去优化。
- **虚拟机 (Virtual Machine)**：基于寄存器的字节码解释器。实现了内联缓存 (Inline Caching)、底层快速路径 (Fast Paths)、尾调用优化 (TCO)、带行号回溯的异常处理、交互式步进调试器、执行性能分析器 (Profiler) 以及动态运算符分发。

### 语言语义
- **类型系统与内存管理**：由 NaN-boxing (NaN 装箱) 驱动的动态类型系统，内部包含 20 余种数据类型（含隐藏类型）。
  - *值类型*：标量（双精度浮点数、大整数、复数）与矩阵（实数矩阵、复数矩阵）采用连续内存，遵循“值传递 (pass-by-value)”语义。
  - *引用类型*：容器 (`List`, `Dict`, `Set`) 与面向对象 `Instance` 采用“引用传递”语义（底层基于 PIMPL 架构和 `std::shared_ptr`）。
- **渐进式类型 (Gradual Typing)**：支持对函数参数和返回值进行运行时类型契约校验（例如 `func(a: double, b: matrix) -> bool = ...`）。涵盖基础类型、容器类型及类的继承校验。
- **垃圾回收 (GC)**：运行于 VM 栈上的标记-清扫 (Mark-and-Sweep) 垃圾回收器 (`GcHeap`)。追踪 GC 根节点（全局变量、调用栈、闭包上值及上下文）以打破并清除循环引用。
- **面向对象 (OOP)**：支持单继承 (`extends`)、`super` 超类分发以及通过魔术方法（如 `__add__`）实现的运算符重载。实例对象支持解构赋值。
- **控制流与模式匹配**：包含 `if/else`、`while`、`for`、`for-in`、`switch/case`、`match`（支持深度解构与依赖绑定）、`break/continue/return` 以及用于资源清理的 `defer`。
- **错误处理**：提供 `try/catch/throw` 结构与支持栈追踪的结构化 `Exception` 对象。
- **元编程 (Metaprogramming)**：支持基于 AST 的编译时宏系统 (`macro`)，提供代码引用 (`quote`)、解引用 (`$`) 以及卫生宏 (`gensym`) 能力，允许在编译阶段进行代码生成。
- **执行控制**：具备强大的 `Ctrl+C` 中断机制，可在不崩溃虚拟机的前提下安全暂停死循环或重型 CAS 计算。连续三次 `Ctrl+C` 将触发强制退出。
- **函数特性**：支持闭包、Lambda 表达式 `(x) => expr`、默认参数、关键字参数 (`f(a=1, b=2)`)、可变长参数 (`...args`) 以及 `ref` 引用参数绑定。
- **泛型容器 API**：数组操作方法（`push`、`slice`、`map`、`filter`、`reduce`、`sort`、`join`、`zip` 等）挂载在 `List`、`Matrix`、`String`、`Dict`、`Set` 的原型上，支持 UFCS 风格管道（`data |> .sort() |> .unique()`）。异构字面量使用 `@[...]`。
- **集合代数 (Set Algebra)**：提供具有 O(1) 成员判定性能 (`in`) 的 `Set` 类型（支持 `@{...}` 字面量语法）。支持并集 (`|`)、交集 (`&`)、差集 (`-`) 和笛卡尔积 (`*`) 运算符。内置幂集生成 (`setPow`) 及包含关系断言机制。

### 数学与计算机代数系统 (CAS)
- **计算机代数系统 (CAS)**：基于有向无环图 (DAG) 的符号数学引擎。具备代数化简（`simplify`、`expand`、`contract`、`factor`、`trigsimp`）、符号微积分（`diff`、`integ`、`limit`、`taylor`）以及精确解析求根（`solveEq`）功能。
- **多项式代数**：利用子结式伪余数序列求解多项式 GCD，并采用有限域 $\mathbb{Z}_p$ 映射（Cantor-Zassenhaus 算法）进行多元多项式因式分解。
- **积分引擎**：实现了 Risch 算法的核心子集，包含 Hermite 归约、Rothstein-Trager 算法以及刘维尔微分域扩张。
- **任意精度运算**：采用 Base-10^9 压缩布局的 `BigInt` 引擎。实现了高基数除法、GCD/LCM 及模幂运算。
- **精确有理数与符号提升**：`Fraction` 类型支持递归交叉约分。当遇到无法数值计算的精确有理数幂（如 `(1/2)^(1/2)`）时，会自动“提升”为 `SymExpr` CAS 符号树，彻底杜绝浮点精度丢失。
- **线性代数**：`Matrix<T>` 模板库，支持 Gauss-Jordan 消元、QR 分解（修正的格拉姆-施密特正交化）、LU 分解（Doolittle 部分主元消去法）以及特征值求解（赫森伯格矩阵 + Givens QR 迭代）。

### 原生模块与标准库
注入到执行运行时的原生 C++ 扩展 (Native Modules)：
- `image`：基于 OOP 的高并发 BMP 图像生成器。绘图组件内建 SDF（符号距离场）支持，实现亚像素级抗锯齿，并包含 ASCII 字体渲染器。
- `prob`：概率论与统计模块。提供面向对象的分布类（支持 PDF、CDF 及基于牛顿迭代的分位数逆函数），以及建设性的假设检验功能。
- `json`：高性能 JSON 序列化与反序列化引擎。
- `socket`：操作系统底层的 TCP/IP 网络栈控制引擎（封装了 WinSock2 / POSIX）。
- `bytes`：内存缓冲区控制及裸二进制文件 I/O 引擎。
- `window`：原生 GUI 窗口渲染引擎。支持第一人称鼠标指针捕获 (Mouse-Look) 和独立的输入法 (IME) 状态接管 (Win32)。
- `latex`：双向 LaTeX 引擎。可将 JC2 数学对象序列化为 LaTeX 代码，或将原始 LaTeX 公式解析并即时编译为可执行的 JC2 闭包函数。
- `ffi`：零依赖的外部函数接口 (Foreign Function Interface，仅支持 Windows x64)。支持动态加载共享库 (DLL)，直接调用 C ABI 函数，以及裸指针与内存的直接读写。
- `regex`：高性能原生正则表达式引擎（字节码虚拟机 + 全状态记忆化）。
- `tensor`：支持自动求导的 N 维张量引擎。
- `decimal`：任意精度十进制运算。

通过 `import` 加载的 JC2 标准库：
- `collections`：常用数据结构，包含栈、队列、双端队列、优先队列（堆）以及多种搜索树。
- `discrete`：离散数学工具箱，涵盖组合数学、二元关系和图遍历。
- `engine`：面向对象的可视化/游戏框架，在 `window` 模块基础上抽象了渲染主循环与事件状态机。
- `net`：TCP 数据流的高级 OOP 封装（`TcpSocket` 和 `TcpServer`）。
- `http`：现代 HTTP/1.1 客户端，支持 URL 解析及 GET/POST 请求。
- `buffer`：提供游标寻址能力的高级二进制操作 API。

---

## v2.6.0.0 版本更新说明

### JIT 编译器（全新）
- **即时编译器 (JIT)**：从零实现了完整的 JIT 编译流水线：可执行内存分配、宏汇编器、带类型特化的字节码到 HIR 提升、LIR 指令选择、图着色寄存器分配，以及 x86-64 机器码生成。
- **自适应优化**：运行时类型剖析驱动类型守卫、内联缓存、函数内联，以及针对多态调用点的 callout 回退。整数运算发射溢出检查并支持去优化。
- **栈上替换 (OSR)**：热点循环原地编译，通过 OSR 入口切换执行，配合基于栈映射的去优化实现精确状态恢复。
- **配套工具**：内置 x86-64 反汇编器（`--mc`）、HIR 图打印（`--hir`），以及 `--jit` 命令行开关（默认关闭）。

### 语言核心
- **关键字参数**：命名参数可用 `f(b=5, a=3)` 调用。原生函数暴露参数名，`...rest` 参数只收集多余的位置参数。
- **原型链 (Prototype Chain)**：容器方法迁移到 `List`、`Matrix`、`String`、`Dict`、`Set` 原型上，支持 UFCS 风格管道，如 `data |> .sort() |> .unique()`。
- **内置命名空间**：`sys`、`io`、`cas`、`math`、`random` 注入为原生命名空间，取代散落的全局函数。
- **一等切片对象**：`slice(start, end, step)` 对象，配合多维索引（`A[i, j]`）与多维切片（`A[1:3, 2:4]`），`__getitem__` 直接接收 slice 对象。
- **生命周期与匹配协议**：`finalize()` 析构钩子（取代 `__del__`），以及 `__match__()` 协议支持自定义模式匹配视图。
- **递归宏**：宏现在可在 `quote` 块内递归调用自身，用于 AST 遍历与变换。
- **类型断言**：赋值、解构与独立声明上的单次运行时断言（`x: int = 10`、`[a: int, b: string] = data`、`local x: int`）。
- **编译器指令与 Shebang**：`#` 引入行级编译器/VM 指令；`#!/usr/bin/env jc2` Shebang 通过 no-op 的 `!` 指令识别。
- **整数除法**：新增 `~/` 运算符（向零截断），支持 `__idiv__` 重载。
- **新内置函数**：容器上的 `enumerate` 与 `groupBy`。

### CAS 引擎
- **多项式核心重写**：稀疏多项式引擎（`SparsePoly`）与多元引擎（`MultiPoly`），64 位结构化哈希，Arena 分配器取代 `shared_ptr`，以及针对性的 GCD/factorReal 优化。
- **高级算法**：Buchberger 的 LCM 准则、子结式 PRS，以及原生 64 位 Cantor-Zassenhaus 因式分解快速路径。
- **符号矩阵 (`SymMatrix`)**：精确符号线性代数，涵盖 LU、QR、对角化、charPoly、特征值/特征向量、矩阵指数、Jacobian、Hessian、Kronecker 积、零空间、秩、范数及逐元素积分。

### API 现代化
- **移除废弃 API**：删除 `isXXX` 类型谓词与 `pcall`，改用 `isinstance`、一等类型对象与 `try/catch`。
- **统一深拷贝**：`copy(obj, freeze)` 三态冻结标志（`none`/`true`/`false`），取代 `clone`/`val`。
- **`BaseNum` 降级**：`basenum` 现为 `math.BaseNum` 类，支持基数移位。
- **严格矩阵字面量**：异构字面量需用 `@[...]`；普通 `[...]` 构造同质矩阵。`StringMatrix` 类型已移除。
- **类型对象驻留 (Interning)**：类型对象全局驻留，使 `type(5) is int` 成为 O(1) 指针比较。

### 内存与 GC
- **终结器 (Finalizer)**：GC 调用 `finalize()`，保证每个对象生命周期内恰好执行一次。
- **Use-After-Free 修复**：解决长跑脚本（zombie 场景）暴露的 GC use-after-free 缺陷。

---

## 构建指南

本项目要求使用支持 C++20 的主流编译器，以及 CMake 3.15 或以上版本进行构建。

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

*注意：在使用 MSVC 构建时，底层 CMake 脚本将默认启用 `/MT` 运行时静态链接与全局链接时代码生成 (LTCG / `/GL`)，以压榨极限性能。*

---

## 命令行接口使用方法

    JunkCalculator2                    # 运行交互式 REPL 会话
    JunkCalculator2 script.jc2         # 执行目标脚本
    JunkCalculator2 --run script.jc2   # 执行目标脚本（显式传递）
    JunkCalculator2 script.jc2 -d      # 执行脚本，并在执行时打印虚拟机字节码反汇编流
    JunkCalculator2 script.jc2 --ir    # 执行并打印 IR 图
    JunkCalculator2 script.jc2 --hir   # 执行并打印 HIR 图
    JunkCalculator2 script.jc2 --mc    # 执行并打印机器码反汇编
    JunkCalculator2 script.jc2 --jit   # 开启 JIT 编译执行
    JunkCalculator2 script.jc2 --debug # 开启交互式步进调试器模式运行
    JunkCalculator2 script.jc2 --profile # 运行结束后输出 VM 执行火焰图指令性能报告

*脚本路径上下文：`run` 与 `import` 指令执行时，将动态地使执行文件所在目录压入路径栈。因此，在脚本内部请求相对物理路径资源时，解析规则始终基于脚本自身所在目录，而脱离终端命令执行点的影响。*

---

## 项目代码结构

    +-- src/
    |   +-- main.cpp                引擎入口，CLI 解析及工作区环境控制
    |   +-- resource.rc             Windows 资源文件 (图标与版本信息)
    |   +-- frontend/               前端语法组件 (Lexer, Parser, AST, Highlight)
    |   +-- compiler/               中后端编译组件 (IRBuilder, Optimizer, Emitter, Resolver)
    |   +-- jit/                    即时编译器 (HIR/LIR, RegAlloc, CodeGen, OSR)
    |   +-- vm/                     虚拟机核心 (VM, Bytecode, BuiltinRegistry, 中断控制)
    |   +-- memory/                 内存与类型系统 (Value 动态类型, GcHeap 垃圾回收)
    |   +-- math/                   基础数学库 (BigInt, Fraction, Complex, Matrix, Base)
    |   +-- cas/                    计算机代数系统 (Symbolic, Integration, Factorization, Groebner)
    |   +-- lib/                    原生 C++ 扩展与 C ABI (Image, JSON, FFI, Regex 等)
    +-- modules/                    标准 JC2 库
    +-- docs/                       设计文档 (JIT, VM, Extension API 等)
    +-- data/                       捆绑数据 (帮助文档、图标、质数表)
    +-- examples/                   内置展示用项目示例
    +-- tests/                      自动化测试脚本套件
    +-- jc2-language/               配套 Visual Studio Code 插件支持

---

## 许可证 (License)

MIT License
