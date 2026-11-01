<div align="right">
  <a href="README.md">English</a> | <strong>简体中文</strong>
</div>

# Junk Calculator 2.6.5.0

![Version](https://img.shields.io/badge/Version-v2.6.5.0-orange.svg?style=flat-square)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B)
![Zero Dependencies](https://img.shields.io/badge/Dependencies-0-brightgreen.svg?style=flat-square)
![CMake](https://img.shields.io/badge/CMake-3.17+-064F8C.svg?style=flat-square&logo=cmake)
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
- **渐进式类型 (Gradual Typing)**：支持对函数参数和返回值进行运行时类型契约校验（例如 `func(a: float, b: matrix) -> bool = ...`）。涵盖基础类型、容器类型及类的继承校验。
- **垃圾回收 (GC)**：运行于 VM 栈上的标记-清扫 (Mark-and-Sweep) 垃圾回收器 (`GcHeap`)。追踪 GC 根节点（全局变量、调用栈、闭包上值及上下文）以打破并清除循环引用。
- **面向对象 (OOP)**：支持单继承 (`extends`)、`super` 超类分发以及通过魔术方法（如 `__add__`）实现的运算符重载。实例对象支持解构赋值。
- **控制流与模式匹配**：包含 `if/else`、`while`、`for`、`for-in`、`match`（支持深度解构与依赖绑定）、`break/continue/return` 以及用于资源清理的 `defer`。
- **错误处理**：提供 `try/catch/throw` 结构与支持栈追踪的结构化 `Exception` 对象，并支持 match 风格 catch 链（`catch { pattern => body, ... }`），包含软类型匹配与 guard。
- **元编程 (Metaprogramming)**：支持基于 AST 的编译时宏系统 (`macro`)，提供代码引用 (`quote`)、解引用 (`$`) 以及卫生宏 (`gensym`) 能力，允许在编译阶段进行代码生成。
- **执行控制**：具备强大的 `Ctrl+C` 中断机制，可在不崩溃虚拟机的前提下安全暂停死循环或重型 CAS 计算。连续三次 `Ctrl+C` 将触发强制退出。
- **函数特性**：支持闭包、Lambda 表达式 `(x) => expr`、默认参数、关键字参数 (`f(a=1, b=2)`)、仅关键字参数 (`f(a; b=0)`)、关键字收集 (`f(; ...kw)`)、可变长参数 (`...args`)、参数解包 (`f(...args)`) 以及 `ref` 引用参数绑定。
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
- `io`：文件流类，零拷贝二进制 I/O、RFC 4180 CSV 引擎，以及支持 UTF-8 路径的文件系统操作。
- `prob`：概率论与统计模块。提供面向对象的分布类（支持 PDF、CDF 及基于牛顿迭代的分位数逆函数），以及建设性的假设检验功能。
- `json`：高性能 JSON 序列化与反序列化引擎。
- `socket`：操作系统底层的 TCP/IP 网络栈控制引擎（封装了 WinSock2 / POSIX）。
- `bytes`：原生内存缓冲区，Hex/Base64 编解码、零拷贝 view/slice、链式类型化读写方法。
- `window`：原生 GUI 窗口渲染引擎。支持第一人称鼠标指针捕获 (Mouse-Look) 和独立的输入法 (IME) 状态接管 (Win32)。
- `latex`：双向 LaTeX 引擎。可将 JC2 数学对象序列化为 LaTeX 代码，或将原始 LaTeX 公式解析并即时编译为可执行的 JC2 闭包函数。
- `ffi`：零依赖的外部函数接口（跨平台：Windows/Linux/macOS）。支持动态加载共享库、直接调用 C ABI 函数、零拷贝多维数组视图、嵌套结构体支持。
- `regex`：高性能原生正则表达式引擎（字节码虚拟机 + 全状态记忆化）。
- `crypto`：SHA-256/384/512、AES-GCM、X25519、P-256/384/521 与 X.509 证书链校验。
- `tls`：基于 TCP 的 TLS 1.2 / 1.3 客户端。
- `inflate`：gzip / zlib 解码。
- `tensor`：支持自动求导的 N 维张量引擎。
- `decimal`：任意精度十进制运算。

通过 `import` 加载的 JC2 标准库：
- `collections`：栈、队列、双端队列、优先队列（堆）、搜索树、LRU 缓存与 UnionFind。
- `macros`：带平台门控的编译期宏。
- `cre`：化学反应工程辅助库。
- `discrete`：组合数学、`Prop` / `Relation`、CNF/DNF、可满足性、图算法与 Hasse 图。
- `engine`：面向对象的可视化/游戏框架，在 `window` 模块基础上抽象了渲染主循环与事件状态机。
- `net`：TCP 数据流的高级 OOP 封装（`TcpSocket` 和 `TcpServer`）。
- `http`：HTTP/1.1 与 HTTPS 客户端，支持 URL 解析、GET/POST、重定向与分块传输。

---

## v2.6.5.0 版本更新说明

### 语言
- **编译期指令**：`#version`、`#platform`、`#warning` 与 `#error`，支持版本和平台检查。
- **包导入与 include**：package 入口、子模块、`include import` 与 namespace 字段合并。
- **比较与排序**：`<=>`、拆分 `sort` / `sorted`；缺少 `__cmp__` 时由 `__lt__` + `__eq__` 提供自然序。
- **定义名**：支持 raw 与反引号定义名；继续拒绝 f-string 定义名。
- **绑定安全**：解构原子提交、`state` 目标 staging、enum 重复成员拒绝，以及更严格的 `ref` / 修饰符检查。
- **小型补充**：`reverse` / `reversed`、`getName` 反射、常量 enum 折叠与 `ast` 字面量辅助。

### 标准库与模块
- **`macros` 包**：平台门控与编译期辅助。
- **`math`**：数值 ODE 求解器、可调用稠密解，以及 `math.e()` / `math.E` 形式。
- **`collections`**：LRU 缓存、UnionFind 与二叉树辅助。
- **图算法**：最小生成树、最短路径与强连通分量。
- **`discrete`**：`Prop` / `Relation` 文本、CNF/DNF、可满足性与 Hasse 图。
- **运行时辅助**：`exec` / `system`、`process` / 环境访问、`sys.globals`、字节码导出与带注释反汇编。
- **反射 API**：`getName` 与 `getFieldInfo`。
- **帮助与 LSP**：脚本侧帮助注册、嵌套帮助查找，扩展 LSP 索引/大纲支持，以及 `include` 语义高亮。

### 修复
- 基类字段先于派生类字段初始化。
- `parseNum` 拒绝部分输入；`ord` 与 `parseNum` 作为全局函数可用。
- 矩阵默认值/rest 解构与 include 合并保持原子性。
- comprehension 变量保持局部；match 分支目标预先声明。
- 空容器为假；private 方法从 `getFields` 隐藏；GC 与 JIT 关闭/栈修复落地。
- 使用前初始化诊断、块内赋值作用域与 workspace 原子写入得到加强。

### 工具与文档
- 新增《JC2 语言规范》第一卷中文草案。
- 帮助文档与运行时同步，覆盖有序容器、矩阵迭代、slice 赋值与 `math.i()`。
- 开发工具：提交消息检查器、PowerShell 构建包装器与 JC2 回归运行器。
- CAS：正根式合并与不可约三次积分。

### 兼容性
- `math.i` 改为函数；使用 `math.i()` 或字面量 `1i`。`math.I` 保留常量形式。
- 全局 `e` 移除；使用 `math.e()` 或 `math.E`。
- enum 重复成员现在产生 `SyntaxError`。

## 构建指南

本项目要求使用支持 C++20 的主流编译器，以及 CMake 3.17 或以上版本进行构建。

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

*注意：在使用 MSVC 构建时，底层 CMake 脚本将默认启用 `/MT` 运行时静态链接与全局链接时代码生成 (LTCG / `/GL`)，以压榨极限性能。*

---

## 命令行接口使用方法

    jc2                                # 运行交互式 REPL 会话（默认）
    jc2 load [name/path]               # 加载工作区并进入 REPL
    jc2 [path].jcw                     # 自动回退为加载工作区
    jc2 script.jc2                     # 执行脚本（回退到 'run'）
    jc2 run script.jc2                 # 显式执行脚本
    jc2 compile in.jc2 [out.jcb]       # 编译脚本为字节码
    jc2 compile in.jc2 -m              # 编译为模块（供 'import' 使用）
    jc2 compile in.jc2 -s              # 编译并剥离调试信息
    jc2 fmt [path]                     # 格式化脚本或目录
    jc2 fmt --check [path]             # 检查脚本是否已格式化（未格式化则返回 1）
    jc2 test [dir]                     # 运行目录中的测试脚本
    jc2 eval "expr"                    # 求值表达式并退出
    jc2 help [topic]                   # 显示帮助概览或特定主题
    jc2 version                        # 显示当前版本
    jc2 lsp                            # 启动 LSP 服务器（stdio）

全局 flag（可附加到任意命令）：

    -d, --ir, --hir, --mc              # 显示反汇编 / IR / 机器码
    --debug, --profile, --jit          # 启用调试器 / 性能分析器 / JIT
    -q, --quiet                        # 静默模式（不显示 banner/提示符）

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
    |   +-- utils/                  共享工具 (lsp, fmt, json, deflate)
    +-- modules/                    标准 JC2 库
    +-- docs/                       设计文档 (JIT, VM, Extension API 等)
    +-- data/                       捆绑数据 (帮助文档、图标、质数表)
    +-- examples/                   内置展示用项目示例
    +-- test/                       测试脚本（见下）
        +-- tests/                  常驻套件（随包发布）
        +-- jit_tests/              JIT 专项套件（可选）
        +-- http_tests/             向量/网络套件及其夹具（按需下载）
    +-- jc2-language/               配套 Visual Studio Code 插件支持

---

## 许可证 (License)

MIT License
