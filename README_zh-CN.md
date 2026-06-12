<div align="right">
  <a href="README.md">English</a> | <strong>简体中文</strong>
</div>

# Junk Calculator 2.4.3.0

![Version](https://img.shields.io/badge/Version-v2.4.3.0-orange.svg?style=flat-square)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B)
![Zero Dependencies](https://img.shields.io/badge/Dependencies-0-brightgreen.svg?style=flat-square)
![CMake](https://img.shields.io/badge/CMake-3.15+-064F8C.svg?style=flat-square&logo=cmake)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)

一个基于 C++20 实现的脚本语言及计算机代数系统 (CAS)。该方案采用自定义字节码编译器和基于栈的虚拟机执行，**完全无第三方依赖**。

由清华大学 Yu Liangyang 开发。

---

## 技术概览

### 底层架构
- **词法分析器 (Lexer)**：支持超过 55 种词法单元，涵盖字符串插值 (`f""`)、自定义定界符原始字符串 (`r"TAG()TAG"`)、交替单双引号、虚数后缀 (`3i`) 以及可变长参数 (`...`)。
- **语法分析器 (Parser)**：递归下降解析器，生成包含 30 余种节点类型的抽象语法树 (AST)。支持运算符优先级、块语句、逗号序列求值以及解构提取。
- **编译器 (Compiler)**：采用访问者模式将 AST 编译为字节码。负责处理词法作用域、自动局部变量声明、循环补丁 (Loop Patching) 以及闭包环境的捕获。
- **虚拟机 (Virtual Machine)**：基于栈的字节码解释器。实现了内联缓存 (Inline Caching)、尾调用优化 (TCO)、函数调用的延迟绑定 (Late-binding)、带行号回溯的异常处理、交互式步进调试器、执行性能分析器 (Profiler) 以及动态运算符分发。

### 语言语义
- **类型系统与内存管理**：由 NaN-boxing (NaN 装箱) 驱动的动态类型系统，内部包含 20 余种数据类型（含隐藏类型）。
  - *值类型*：标量（双精度浮点数、大整数、复数）与矩阵（实数矩阵、复数矩阵、字符串矩阵）采用连续内存，遵循“值传递 (pass-by-value)”语义。
  - *引用类型*：容器 (`List`, `Dict`, `Set`) 与面向对象 `Instance` 采用“引用传递”语义（底层基于 PIMPL 架构和 `std::shared_ptr`）。
- **渐进式类型 (Gradual Typing)**：支持对函数参数和返回值进行运行时类型契约校验（例如 `func(a: double, b: matrix) -> bool = ...`）。涵盖基础类型、容器类型及类的继承校验。
- **垃圾回收 (GC)**：运行于 VM 栈上的标记-清扫 (Mark-and-Sweep) 垃圾回收器 (`GcHeap`)。追踪 GC 根节点（全局变量、调用栈、闭包上值及上下文）以打破并清除循环引用。
- **面向对象 (OOP)**：支持单继承 (`extends`)、`super` 超类分发以及通过魔术方法（如 `__add__`）实现的运算符重载。实例对象支持解构赋值。
- **控制流与模式匹配**：包含 `if/else`、`while`、`for`、`for-in`、`switch/case`、`match`（支持深度解构与依赖绑定）、`break/continue/return`。
- **错误处理**：提供 `try/catch/throw` 结构以及支持栈追踪的函数式 `pcall`。
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

## v2.4.3.0 版本更新说明

v2.4.3.0 版本在底层性能优化、语法特性扩展、虚拟机稳定性及标准库重构方面进行了大量更新。

### 性能大幅提升
- **尾调用优化 (TCO)**：实现了针对深度递归的尾调用优化（在 `ref` 参数需要写回时会自动禁用以保证语义正确）。
- **内联缓存 (Inline Caching)**：为属性访问和方法调用引入了内联缓存机制，并利用常量池字符串加速属性查找。
- **内存与字符串**：实现了字符串字面量驻留（Interning），开启了指针相等性比较的快速路径；为字符串拼接、字典和集合的构建增加了内存预分配机制。
- **算术运算**：在二元运算符分发中为双精度浮点数（Double）添加了快速执行路径。

### 语法特性扩展
- **字面量语法**：新增了集合字面量 `@{...}` 和强制列表字面量 `@[...]`（通过 `@` 前缀避免被隐式解析为矩阵拼接）。
- **模式匹配增强**：引入了依赖绑定（Dependent binding）和显式动态断言 `(expr)`；在解构赋值和 `match` 表达式中增加了对矩阵模式的严格形状校验；修复了字典字面量与代码块的解析歧义。
- **列表推导式**：强制要求推导式子句使用括号包裹，并支持了交替嵌套的 `for` 和 `if` 语句。

### 虚拟机与内存安全
- **GC 稳定性**：修复了原生函数调用期间参数可能被意外垃圾回收的严重问题；延迟了调用和索引操作中的栈清理时机，确保触发 GC 时的栈状态安全；分离了用户态的 `clear()` 和 GC 使用的 `clearTotal()`。
- **执行逻辑**：统一了底层栈管理逻辑（遵循 SESE 原则）；修复了字符串修改时未正确拷贝导致的 COW（写时复制）语义破坏问题；修复了 REPL 执行后临时函数残留导致的内存泄漏。
- **闭包与状态**：在闭包捕获缺失的全局变量或未初始化变量时，改为快速失败（Fail-fast）报错。

### 数学引擎与标准库
- **数学计算**：移除了数值清理阶段的整数舍入，使矩阵输出严格遵循浮点数精度；放宽了编译期的矩阵形状检查，允许在运行时进行矩阵行拼接。
- **标准库重构**：适配了收紧的类型系统，将标准库中的 `int` 约束全面替换为 `whole`；将标准库中所有的 `list()` 函数调用替换为 `@[...]` 字面量以消除函数调用开销。
- **正则库重写**：使用最新的模式匹配语法全面重写了 `regex.jc2`，移除了旧的 `re_` 前缀，并将内部的 lambda 替换为 `for` 循环，彻底解决了闭包捕获（upvalue）丢失上下文的问题。

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
    |   +-- frontend/               前端编译组件 (Lexer, Parser, Compiler, AST, Highlight)
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
