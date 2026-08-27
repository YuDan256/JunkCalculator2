<div align="right">
  <a href="README.md">English</a> | <strong>简体中文</strong>
</div>

# Junk Calculator 2.6.2.0

![Version](https://img.shields.io/badge/Version-v2.6.2.0-orange.svg?style=flat-square)
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
- **标识符 (Identifiers)**：完整支持 UTF-8 标识符，并支持反引号引用标识符（`` `text` ``），可把任意文本（关键字、空格、标点）作为名字（`` `if` = 1 ``、`` obj.`key name` ``）。
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
- **函数特性**：支持闭包、Lambda 表达式 `(x) => expr`、默认参数、关键字参数 (`f(a=1, b=2)`)、仅关键字参数 (`f(a; b=0)`)、关键字收集 (`f(; ...kw)`)、可变长参数 (`...args`) 以及 `ref` 引用参数绑定。
- **泛型容器 API**：数组操作方法（`push`、`slice`、`map`、`filter`、`reduce`、`sort`、`join`、`zip` 等）挂载在 `List`、`Matrix`、`String`、`Dict`、`Set` 的原型上，支持 UFCS 风格管道（`data |> .sort() |> .unique()`）。异构字面量使用 `@[...]`。
- **集合代数 (Set Algebra)**：提供具有 O(1) 成员判定性能 (`in`) 的 `Set` 类型（支持 `@{...}` 字面量语法）。支持并集 (`|`)、交集 (`&`)、差集 (`-`) 和笛卡尔积 (`*`) 运算符。内置幂集生成 (`powerSet`) 及包含关系断言机制。

### 数学与计算机代数系统 (CAS)
- **计算机代数系统 (CAS)**：基于有向无环图 (DAG) 的符号数学引擎。具备代数化简（`simplify`、`expand`、`contract`、`factor`、`trigsimp`）、符号微积分（`diff`、`integ`、`limit`、`taylor`）以及精确解析求根（`solveEq`）功能。
- **多项式代数**：利用子结式伪余数序列求解多项式 GCD，并采用有限域 $\mathbb{Z}_p$ 映射（Cantor-Zassenhaus 算法）进行多元多项式因式分解。
- **积分引擎**：实现了 Risch 算法的核心子集，包含 Hermite 归约、Rothstein-Trager 算法以及刘维尔微分域扩张。
- **任意精度运算**：采用 Base-2³² limb 布局的 `BigInt` 引擎。实现了高基数除法、GCD/LCM 及模幂运算。
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

## v2.6.2.0 版本更新说明

### 函数与参数解包
- **仅关键字参数**：`;` 分隔参数列表——分号后为仅关键字（`f(a; b, c=0)`），分号后的 `...kw` 将多余关键字收集成字典（`f(a, ...rest; b, ...kw)`）。参数元数据拆分为四个独立字段（`paramNames`/`restName`/`kwargNames`/`kwargsName`），取代旧的 `...` 前缀 hack。
- **参数解包（spread）**：`...expr` 内联展开——`f(...args)` 将 list/set/matrix/string 按位置展开，`f(1; ...opts)` 将字典展开为关键字参数。解包可出现在任意位置、可多次出现。
- **字面量解包**：`@[...a]`、`@{...s}`、`{...d}` 在 list/set/dict 字面量内解包。字典解包从左到右合并，后者覆盖前者；显式 key 始终优先。
- **`__unpack__` / `__mapping__`**：自定义类型通过这两个 dunder 返回 list（位置）或 dict（关键字）来接入解包；`apply` 改用 `__unpack__` 而非 `__iter__`，避免无限迭代。
- **统一原生调用约定**：`rest` 在原生函数中始终以 list 形式到达，消除了旧有的「展开 vs 收集」分裂。
- **更丰富的签名**：`toString` 现显示 `const`/`ref` 修饰符与内建关键字默认值——`print` 呈现为 `<function print(...args; sep = " ", end = "\n")>`。

### `print` / `println` 合并
- **`print(...args; sep = " ", end = "\n")`**：`print` 新增 Python 风格的仅关键字 `sep`/`end`，默认以换行结尾。`println` 移除；`print("不换行", end = "")` 覆盖旧的不换行行为。这是首个使用仅关键字默认值的内建函数。

### 矩阵
- **不可变矩阵**：矩阵不再写时复制；因值不再变化，哈希得以缓存。
- **零拷贝视图**：切片、`trans`、`getRow`、`getCol` 返回基于步幅的视图，而非复制。
- **2D 切片**：`getItem`/`getSlice` 及升级后的 `setSlice(sr, sc, val)` 支持行列区间。

### Tensor
- **性能**：基于模板的 dtype 分派、连续快路径、分块缓存 matmul、批量 matmul 的 Strassen 快路径，以及零开销的 `TensorImpl` 句柄架构。
- **自动求导**：拓扑排序反向传播，配合真正的 `no_grad` 上下文。
- **广播与归约**：广播步幅迭代、1D/批量 `matmul`、`sum`/`mean` 轴、`clamp`、`argmax`；`DType::Bool` 严格索引分派；归约支持 `keepdim`；嵌套列表初始化与形状推断。

### 标准库
- **`bytes`**：原生 `Buffer` 取代旧的 `buffer.jc2` 脚本——Hex/Base64 编解码、零拷贝 view/slice、链式类型化读写方法。
- **`ffi`**：跨平台（Linux/macOS）、零拷贝多维数组视图、嵌套结构体支持、内联数组索引赋值。
- **`io`**：`File` 流类，零拷贝二进制 I/O、RFC 4180 CSV 引擎、文件系统操作、Windows 上的 UTF-8 路径处理。

### CAS
- **化简**：同指数幂在乘法中合并，矩阵运算化简 / 幂折叠改进。

### 修复
- 宏调用参数恢复为 AST 节点——`@m(a=1)` 将 `a=1` 解析为 `Assign` 语句，而非命名参数。
- `maxArity` 检查改用解包后的位置参数个数，`f(1, 2, ...@[], 3, 4)` 正常工作。
- rest/kwargs 必须为最后一个参数；`;` 允许出现在参数列表开头（全仅关键字）。

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
