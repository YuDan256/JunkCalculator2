# JC2 核心设计须知备忘录 (Core Design Memo)

本文档记录了 Junk Calculator 2 (JC2) 架构开发中的核心设计哲学、语义界定与踩坑记录，供后续 C++ 底层开发与重构参考。

## 1. 字符串化接口语义 (Stringification Semantics)
在 C++ 虚拟机底层，`Value` 对象的字符串化存在严格的语义区分，绝不可混用：

*   **`toString()` (对应 `__str__`)：目标是“人类可读”（Human-readable）。**
    *   **场景**：`print()` 输出、字符串拼接 (`+`)、f-string 插值 (`f"{x}"`)。
    *   **特征**：对于字符串，直接返回裸字符串内容（**不带引号**）。对于复杂对象，返回易于阅读的摘要。
*   **`toRepr()` (对应 `__repr__`)：目标是“开发者无歧义”（Developer-readable / Unambiguous）。**
    *   **场景**：REPL 顶层回显、错误信息 (Traceback)、以及集合（List/Dict/Set）内部元素的打印。
    *   **特征**：必须能严格区分类型。例如，数字 `1` 返回 `"1"`，而字符串 `"1"` 必须返回带引号的 `"\"1\""`。

## 2. 容器的内存语义 (Container Memory Semantics)
JC2 混合了值语义与引用语义，以兼顾数学计算的高效与通用编程的灵活：

*   **值语义 (Value Semantics)**：`Matrix` (包含 `realmatrix`, `complexmatrix`, `stringmatrix`)、`Array` (行向量)。
    *   赋值 `A = B` 会触发**深度拷贝**。修改 `A` 绝不会影响 `B`。
    *   适用于纯数学计算，保证无副作用。
*   **引用语义 (Reference Semantics)**：`List`, `Dict`, `Set`, `Instance` (类实例)。
    *   赋值 `L2 = L1` 仅拷贝指针，两者共享同一块底层内存。
    *   由虚拟机的 **Mark-and-Sweep 垃圾回收器 (GC)** 负责追踪和销毁，允许循环引用。

## 3. 哈希与不可变性 (Hashing & Immutability)
JC2 采用高性能的**原生值哈希 (Native Value Hashing)** 架构：

*   **值等价碰撞**：`1.0` (double)、`int(1)`、`frac(1,1)` 在语义上完全相等，它们的哈希值必须绝对一致，且在 Dict/Set 中会发生完美碰撞。
*   **容器作为键**：只有**被冻结 (Frozen)** 的容器（通过 `freeze()` 或 `val()`）才能作为 Dict 的键或 Set 的元素。未冻结的容器在插入时必须抛出 `TypeError`。
*   **类实例哈希**：类实例默认按引用（指针）比较和哈希。如果重写了 `__hash__` 和 `__eq__`，则按自定义逻辑哈希。

## 4. 作用域修饰符 (Scope Modifiers)
变量声明时的修饰符决定了其内存生命周期与捕获方式：

*   **(默认)**：函数级自动局部变量。**闭包默认按值捕获 (Capture by Value)**。
*   `local`：严格的**块级作用域**（Block Scope）。离开 `{}`、`if`、`for` 后立即销毁。
*   `ref`：**向上作用域解析**。跨越函数边界直接绑定并修改外部变量（按名捕获/引用传递）。
*   `state`：**私有持久化状态**。为闭包提供跨调用的持久内存，且初始化表达式仅在首次调用时执行一次。
*   `const`：**严格不可变**。无法被重新赋值，也无法被 `delete` 关键字删除。

## 5. 异常与 Traceback (Exceptions & Tracebacks)
*   所有通过 `throw` 抛出的值（无论类型），在虚拟机内部都会被自动包装为 `Exception` 类的实例。
*   如果抛出的已经是 `Exception` 实例且没有 `traceback`，VM 会自动为其填充当前的调用栈。
*   **C++ 底层抛出规范**：在 C++ 运行时底层（如内置函数、类型检查、VM 指令执行等），**绝对禁止**抛出裸的 `std::runtime_error`、`std::invalid_argument` 等标准库异常。必须借鉴 `VM.cpp` 的机制，统一抛出 `jc::RuntimeError("ErrorType", Value("Message"))` 或 `jc::ValueException`。这避免了 VM 在捕获时还需要低效地解析字符串前缀（如 `"Math Error: ..."`），并保证了异常类型的精确传递。
*   在 C++ 宿主层捕获的 `RuntimeError`，其 `what()` 方法应调用内部 `Exception` 实例的 `toString()`（即 `__str__`），以确保打印出格式化好的完整 Traceback，而不是简写的 `<Exception: ...>`。

## 6. CAS 与数值计算的边界 (CAS vs Numerical Boundary)
*   **数值域 (Numerical)**：变量持有具体的值，表达式立即求值。
*   **符号域 (Symbolic)**：变量持有 AST 节点（如 `sym("x")`），表达式构建代数树。
*   **跨界规则**：只有标准的数学函数（sin, cos, exp 等）允许捕获符号节点并向上提升为 `SymFunc`。用户自定义函数或不兼容的内置函数遇到符号变量时，应立即求值或抛出 `TypeError`。

## 7. C++ 底层开发与扩展规范 (C++ Low-Level Development & Extension Rules)
*   **GC 保护 (GC Guard)**：在 C++ 内部创建或操作受 GC 管理的原生对象（如 `ObjList`, `ObjDict`, `ObjInstance` 等）时，如果在将其挂载到根节点（如压入栈或存入其他已被追踪的对象）之前发生新的内存分配，**必须**使用 `GcObjGuard` 将其包裹，防止在触发 GC 时被误伤回收。
*   **AST 节点与宏系统 (AST Nodes & Macros)**：当在编译器中加入新的 AST 节点时，必须同步处理宏系统中的双向转换逻辑。即在 `ASTNode` 类（供 JC2 脚本操作的字典结构）与 C++ 底层的真 AST 节点之间，实现完整的序列化与反序列化支持。

## 8. 枚举的编译期零开销语义 (Compile-Time Zero-Overhead Enum Semantics)
*   **语法糖与底层映射**：`enum` 在 JC2 中并非引入新的运行时数据结构，而是直接映射为**被冻结的命名空间 (Frozen Namespace)**。
*   **编译期求值 (Compile-Time Evaluation)**：为了实现真正的零开销，`EnumDefExpr` 节点在 IR 构建阶段（`IRBuilder`）就会被完全求值。枚举成员的值必须是编译期常量（字面量或可常量折叠的表达式）。
*   **常量池嵌入**：构建好的 `ObjNamespace` 会在编译期直接被标记为 `is_frozen = true`，并作为 `IROp::Constant` 塞入当前函数的常量池中。运行时仅需一条 `LOADK` 指令即可加载整个枚举，没有任何动态分配或函数调用开销。
*   **匿名纯洁性**：匿名枚举（以及匿名类、匿名命名空间）的内部 `name` 字段严格保持为空字符串 `""`。为了防止与名为 `anonymous` 的变量混淆，打印时由 `Value::toJC2Expression` 拦截并格式化为 `<anonymous namespace>`。

## 9. 占位符与丢弃符的零开销语义 (Zero-Overhead Placeholder & Discard Semantics)
*   **语法降级**：`_` 在 JC2 中被彻底剥夺了“合法标识符”的身份，降级为纯粹的**占位符/丢弃符 (Placeholder/Discard)**。
*   **读取拦截**：在 Resolver 阶段，任何试图读取 `_` 的行为（如 `print(_)`）都会直接抛出 `SyntaxError`，保证了语义的绝对纯洁性。
*   **零开销绑定**：在 Resolver 阶段，`_` 永远不会被注册到任何作用域的符号表中。在 IRBuilder 阶段，目标为 `_` 的赋值（如 `_ = expr` 或 `[_, b] = [1, 2]`）会正常对右侧求值（以保证副作用），但**直接丢弃结果，不生成任何写入变量的 IR 节点**。
*   **GC 压力释放**：由于 `_` 不产生绑定，它不会增加对象的引用计数，也不会进入环境栈，从而完美实现了真正的零开销丢弃，极大减轻了 GC 压力。
*   **语义统一**：这使得 `_` 在解构丢弃、顶层裸赋值丢弃（`_ = func()`）以及偏函数应用（`f(_, 10)`）中达到了完美的逻辑自洽——它永远代表一个“洞”，而不是一个“值”。

## 10. JCB 字节码序列化格式 (JCB Bytecode Serialization Format)
为了提升大型脚本与标准库的加载速度并支持闭源分发，JC2 引入了 `.jcb` (Junk Calculator Bytecode) 格式。
*   **文件头与版本校验 (Header & Versioning)**：文件必须以魔数 (Magic Number) 开头（如 `0x4A 0x43 0x42 0x01`），紧跟严格的 VM 指令集版本号。版本不匹配时必须拒绝加载，防止底层段错误 (Segfault)。
*   **跨平台字节序 (Endianness)**：`.jcb` 文件内部的所有多字节原生数据（如 `int32_t`, `double`）必须统一采用 **小端序 (Little-Endian)** 序列化，以保证在 x64 和 ARM 架构之间的完美跨平台兼容。
*   **常量池的深度反序列化 (Deep Constant Pool Deserialization)**：常量池不仅需要支持标量（Double, Int, String, Fraction, Complex），还**必须支持特定容器的序列化**。
    *   **大整数 (BigInt) 的二进制序列化**：为了极致的加载性能，`BigInt` 必须导出其内部的 `std::vector<uint32_t>` 块，而不是转换为十进制字符串。序列化格式为：1 字节符号位 (`negative`) + 4 字节数组长度 (`size`) + 连续的小端序 `uint32_t` 数组。
    *   **枚举支持**：由于 `enum` 语法在编译期被折叠为 `ObjNamespace` 并存入常量池，`.jcb` 必须支持 `TAG_NAMESPACE` 的递归序列化。反序列化时，需读取其内部的所有键值对，并在内存中重建后严格标记为 `is_frozen = true`。
*   **常量池的序列化边界与内存管理 (Serialization Boundary & Memory Management)**：
    *   **无需序列化**：`Class`, `Instance`, `Closure`, `List`, `Dict`, `Set`。这些对象具有引用语义或依赖运行时上下文，由 VM 在运行时通过指令（如 `CLASS`, `BUILD_LIST`）动态构建，**绝不会**进入常量池。
    *   **按需序列化**：`RealMatrix`, `ComplexMatrix`, `StringMatrix` 具有值语义，未来优化器实现矩阵常量折叠后会进入常量池，因此需支持其序列化（写入维度与连续内存块）。
    *   **内存保护机制 (Memory Protection)**：常量池（`Chunk::constants`）在反序列化后会持有这些复杂对象的 `Value`。
        *   **值语义对象**（如 `String`, `BigInt`, `Matrix`）：自动接入底层的**引用计数 (RC)**，只要 `Chunk` 存活，其 `refCount > 0` 即可免于回收。
        *   **引用语义对象**（如冻结的 `Namespace`）：由于 RC 不保护容器对象，它们依赖于 **GC 标记阶段 (Mark Phase)**。VM 会将当前所有活跃 `Chunk` 的常量池作为 GC Root 进行扫描，确保其绝不会被误杀。
*   **加载优先级**：在执行 `import "module"` 时，VM 的 Resolver 必须优先探测是否存在版本匹配的 `module.jcb`。如果存在则直接反序列化绕过编译前端；如果不存在或魔数/版本号不匹配，则回退加载 `module.jc2` 源码。
