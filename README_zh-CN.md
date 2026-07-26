<div align="right">
  <a href="README.md">English</a> | <strong>简体中文</strong>
</div>

# Junk Calculator 2.5.3.0

![Version](https://img.shields.io/badge/Version-v2.5.3.0-orange.svg?style=flat-square)
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
- **虚拟机 (Virtual Machine)**：基于寄存器的字节码解释器。实现了内联缓存 (Inline Caching)、底层快速路径 (Fast Paths)、尾调用优化 (TCO)、带行号回溯的异常处理、交互式步进调试器、执行性能分析器 (Profiler) 以及动态运算符分发。

### 语言语义
- **类型系统与内存管理**：由 NaN-boxing (NaN 装箱) 驱动的动态类型系统，内部包含 20 余种数据类型（含隐藏类型）。
  - *值类型*：标量（双精度浮点数、大整数、复数）与矩阵（实数矩阵、复数矩阵、字符串矩阵）采用连续内存，遵循“值传递 (pass-by-value)”语义。
  - *引用类型*：容器 (`List`, `Dict`, `Set`) 与面向对象 `Instance` 采用“引用传递”语义（底层基于 PIMPL 架构和 `std::shared_ptr`）。
- **渐进式类型 (Gradual Typing)**：支持对函数参数和返回值进行运行时类型契约校验（例如 `func(a: double, b: matrix) -> bool = ...`）。涵盖基础类型、容器类型及类的继承校验。
- **垃圾回收 (GC)**：运行于 VM 栈上的标记-清扫 (Mark-and-Sweep) 垃圾回收器 (`GcHeap`)。追踪 GC 根节点（全局变量、调用栈、闭包上值及上下文）以打破并清除循环引用。
- **面向对象 (OOP)**：支持单继承 (`extends`)、`super` 超类分发以及通过魔术方法（如 `__add__`）实现的运算符重载。实例对象支持解构赋值。
- **控制流与模式匹配**：包含 `if/else`、`while`、`for`、`for-in`、`switch/case`、`match`（支持深度解构与依赖绑定）、`break/continue/return` 以及用于资源清理的 `defer`。
- **错误处理**：提供 `try/catch/throw` 结构、结构化 `Exception` 对象以及支持栈追踪的函数式 `pcall`。
- **元编程 (Metaprogramming)**：支持基于 AST 的编译时宏系统 (`macro`)，提供代码引用 (`quote`)、解引用 (`$`) 以及卫生宏 (`gensym`) 能力，允许在编译阶段进行代码生成。
- **执行控制**：具备强大的 `Ctrl+C` 中断机制，可在不崩溃虚拟机的前提下安全暂停死循环或重型 CAS 计算。连续三次 `Ctrl+C` 将触发强制退出。
- **函数特性**：支持闭包、Lambda 表达式 `(x) => expr`、默认参数、可变长参数 (`...args`) 以及 `ref` 引用参数绑定。
- **泛型容器 API**：提供统一的数组操作接口（如 `push`、`slice`、`map`、`filter`、`reduce`、`sort`、`join`、`zip` 等），可无缝运行于四种底层数据结构：`RealMatrix`、`ComplexMatrix`、`StringMatrix` 与 `List`（支持 `@[...]` 强制列表字面量）。
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
- `ffi` (experimental)：零依赖的外部函数接口 (Foreign Function Interface)。支持动态加载共享库 (DLL/SO)，直接调用 C ABI 函数，以及裸指针与内存的直接读写。

通过 `import` 加载的 JC2 标准库：
- `collections`：常用数据结构，包含栈、队列、双端队列、优先队列（堆）以及多种搜索树。
- `regex`：面向对象的 NFA 正则表达式引擎，支持捕获组、选择分支与量词。
- `discrete`：离散数学工具箱，涵盖组合数学、二元关系和图遍历。
- `engine`：面向对象的可视化/游戏框架，在 `window` 模块基础上抽象了渲染主循环与事件状态机。
- `net`：TCP 数据流的高级 OOP 封装（`TcpSocket` 和 `TcpServer`）。
- `http`：现代 HTTP/1.1 客户端，支持 URL 解析及 GET/POST 请求。
- `buffer`：提供游标寻址能力的高级二进制操作 API。

---

## v2.5.3.0 版本更新说明

### 一等公民类型系统 (First-Class Types)
- **类型对象与代数**：类型正式提升为一等公民（`ObjTypeDef`）。支持作为值传递、直接调用构造（如 `int("123")`），并支持类型代数运算（如 `int | string`）。
- **闭包期注解固化**：函数参数与返回值的类型注解全面升级为表达式，在闭包定义时即时求值并固化，彻底消除运行时的字符串解析开销。
- **O(1) 极速断言**：类型检查引入内联缓存 (Inline Caching) 机制，将原本的字符串匹配升级为 O(1) 的底层指针比对。

### 核心语法与特性增强
- **枚举 (`enum`)**：新增编译期零开销的枚举语法，底层直接编译为冻结的命名空间，可安全用于字典键或集合元素。
- **对象一致性 (`is`)**：新增 `is` 运算符，用于 O(1) 极速比较两个对象的内存地址或底层位模式身份。
- **零开销丢弃符 (`_`)**：将 `_` 正式实现为零开销占位符，在模式匹配和解构赋值中不产生任何 IR 指令与内存分配。
- **体验优化**：支持数字字面量下划线分隔（如 `1_000_000`），并完整实现了集合推导式 (`@{... for ...}`) 与字典推导式 (`{... for ...}`)。

### 虚拟机与底层架构
- **JCB 预编译字节码**：引入 `.jcb` 二进制字节码格式，支持常量池深度序列化。提供 `--compile`、`--module` 导出及 `--strip` 剥离调试信息功能，具备智能版本校验与源码回退机制。
- **原生类安全 (FFI 防幽灵)**：引入 `is_native` 标志与 `native_allocator` 分配器钩子，彻底封堵跨语言边界的“幽灵实例”漏洞，并使原生 C++ 模块完美支持标准 OOP 实例化语法。
- **C ABI 接口扩充**：在 C 扩展接口中深度集成类型系统（支持动态获取、类型代数、契约校验），并补全了列表、字典、集合的完整增删改查 API。

### 数学引擎与原生模块
- **高精度十进制 (`decimal`)**：新增 `decimal` 模块，提供任意精度的十进制浮点运算。内置基于泰勒级数与牛顿迭代的高精度初等数学函数（exp, log, sin, cos, sqrt 等）。
- **大数性能暴增**：通过引入视窗化操作、O(1) 位数估算及全局 10 的幂缓存等算法，彻底消除了 `BigInt` 与 `Decimal` 底层计算中的字符串转换开销，性能提升数个数量级。

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
    JunkCalculator2 script.jc2 --debug # 开启交互式步进调试器模式运行
    JunkCalculator2 script.jc2 --profile # 运行结束后输出 VM 执行火焰图指令性能报告

*脚本路径上下文：`run` 与 `import` 指令执行时，将动态地使执行文件所在目录压入路径栈。因此，在脚本内部请求相对物理路径资源时，解析规则始终基于脚本自身所在目录，而脱离终端命令执行点的影响。*

---

## 项目代码结构

    +-- src/
    |   +-- main.cpp                引擎入口，CLI 解析及工作区环境控制
    |   +-- frontend/               前端语法组件 (Lexer, Parser, AST, Highlight)
    |   +-- compiler/               中后端编译组件 (IRBuilder, Optimizer, Emitter, RegAlloc)
    |   +-- vm/                     虚拟机核心 (VM, Bytecode, BuiltinRegistry, 中断控制)
    |   +-- memory/                 内存与类型系统 (Value 动态类型, GcHeap 垃圾回收)
    |   +-- math/                   基础数学库 (BigInt, Fraction, Complex, Matrix, Base)
    |   +-- cas/                    计算机代数系统 (Symbolic, Integration, Factorization, Groebner)
    |   +-- modules/                原生 C++ 扩展模块 (Image, Probability, JSON, Socket 等)
    +-- lib/                        标准 JC2 业务层逻辑库开发区
    +-- examples/                   内置展示用项目示例
    +-- jc2-language/               配套 Visual Studio Code 插件支持

---

## 许可证 (License)

MIT License
