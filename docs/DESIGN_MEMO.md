# JC2 核心设计须知备忘录 (Core Design Memo)

本文档记录了 Junk Calculator 2 (JC2) 架构开发中的核心设计哲学、语义界定与踩坑记录，供后续 C++ 底层开发与重构参考。

## 1. 核心语义与内存模型 (Core Semantics & Memory Model)
*   **字符串化**：`toString()` (对应 `__str__`) 目标是“人类可读”，不带引号；`toRepr()` (对应 `__repr__`) 目标是“开发者无歧义”，严格区分类型（如 `"1"` vs `1`）。
*   **内存语义**：`Matrix` 和 `Array` 采用**值语义**（深拷贝，无副作用）；`List`, `Dict`, `Set`, `Instance` 采用**引用语义**（共享内存，由 GC 追踪）。
*   **Matrix COW 与原生对象 (Matrix COW & Native Objects)**：`Matrix` 坚决不转为 `ObjInstance`，以避免哈希表带来的内存碎片化和分发开销。底层 C++ `Matrix<T>` 保持极致连续的 `std::vector` 纯数学容器。**写时复制 (COW)** 严格实现在 VM 操作层面（通过检查 `ObjRealMatrix` 的 `refCount`），而非 C++ 类内部，从而避免双重 COW 开销和破坏值语义。为了支持 `A.inv()` 的 OOP 语法，VM 引入**虚拟原型链 (Virtual Prototype Chain)**，对原生类型的方法调用自动路由至全局静态原型字典。
*   **哈希与不可变性**：采用**原生值哈希**，`1.0`、`int(1)`、`frac(1,1)` 哈希值绝对一致。仅**被冻结 (Frozen)** 的容器可作为 Dict 键或 Set 元素。
*   **作用域修饰符**：
    *   `local`：严格块级作用域。
    *   `ref`：向上作用域解析（按名捕获/引用传递）。
    *   `state`：私有持久化状态，初始化仅执行一次。
    *   `const`：严格不可变，无法被重新赋值或 `delete`。
*   **占位符 `_`**：零开销丢弃符。不产生绑定，不增加 GC 压力，读取会抛出 `SyntaxError`。

## 2. 异常与垃圾回收 (Exceptions & Garbage Collection)
*   **异常机制**：C++ 底层**绝对禁止**抛出标准库异常，必须统一抛出 `jc::RuntimeError` 或 `jc::ValueException`。VM 会自动将其包装为 `Exception` 实例并填充 Traceback。
*   **析构与防僵尸复活**：
    *   `__del__` 仅执行一次（由 `is_finalized` 标志保证），即使对象在析构中被复活。
    *   Native 析构 (`c_nativeDtor`) 严格在 Sweep 阶段执行，彻底杜绝悬空指针。
    *   析构异常被静默隔离，且执行期间锁定 GC 重入（`gc_locked`），防止破坏堆链表。

## 3. 面向对象与扩展架构 (OOP & Extension Architecture)
*   **Native 类防幽灵机制**：`is_native` 标志拦截非法实例化，`native_allocator` 路由至 C++ 构造函数，确保底层句柄安全。
*   **C ABI 与类型代数**：支持 `is_type`, `type_union` 等接口，Native 模块可动态操作 JC2 容器与类型系统。
*   **零依赖 FFI**：基于运行时机器码生成 (JIT Trampoline)，隔离 ABI，支持任意参数传递与结构体按值传递。

## 4. 编译期与元编程 (Compile-Time & Metaprogramming)
*   **零开销枚举**：`enum` 在编译期折叠为冻结的 `ObjNamespace`，作为常量池对象，零运行时开销。匿名枚举内部 `name` 严格为空。
*   **Token 宏**：`syntax` 宏直接操作词法 Token 流，允许定义全新 DSL。支持词法容错 (`ERROR_TOKEN`) 与底层解析桥梁 (`parseExpr`)。

## 5. CAS 与模式匹配 (CAS & Pattern Matching)
*   **数值与符号边界**：标准数学函数允许符号节点提升为 `SymFunc`，不兼容函数遇符号变量立即求值或报错。符号常量 `PI`, `E`, `i` (或 `I`) 在 `evalf` 时会被精确解析为对应的浮点数或复数（如 `sym("i")` 解析为 `1i`）。
*   **视图提取器**：`__match__` 解耦对象内部结构与外部匹配接口。返回 `self` 触发平凡拦截，回退至默认字段匹配。

## 6. 底层规范与序列化 (Low-Level Rules & Serialization)
*   **零依赖与标准 C++ 哲学 (Zero-Dependency & Standard C++ Philosophy)**：JC2 严格禁止使用任何非标准 C++ 特性（如编译器特有扩展），且**绝对禁止**引入任何第三方库（如 libffi、Boost、GMP 等）。所有核心功能（包括大整数、正则、CAS、JIT FFI 等）必须基于 C++20 标准库从零手写，以保证极致的跨平台可移植性与代码的绝对掌控力。
*   **GC 保护**：C++ 内部操作未挂载的 GC 对象必须使用 `GcObjGuard`。
*   **JCB 字节码格式**：小端序，支持调试信息剥离。常量池深度反序列化（支持 BigInt 二进制块与冻结 Namespace）。值语义对象依赖 RC，引用语义对象依赖 GC Root 扫描。

## 7. 命名参数与调用约定 (Keyword Arguments & Calling Convention)
*   **语法风格**：采用 Python 风格的等号传参 `f(a = 1, b = 2)`。
*   **解析器 (Parser) 规则**：上下文敏感解析。在函数调用的参数列表中，顶层的 `标识符 = 表达式` 强制解析为 `KeywordArgNode`。若需在传参时进行变量赋值，必须加括号 `f((a = 1))`。位置参数必须严格在命名参数之前。
*   **VM 预对齐 (Pre-alignment)**：VM 负责在运行时根据闭包的参数名元数据，将命名参数动态路由到正确的局部变量槽位。
*   **Native C++ 扩展无痛升级**：扩展 `bind_method` 注册 API，允许 C++ 侧提供参数名列表（如 `{"a", "b", "rtol"}`）。VM 在调用 Native 函数前，自动将位置参数和命名参数“预对齐”成一个标准的 `std::vector<Value>`，C++ 函数体逻辑无需任何修改。
*   **可变参数 (`...rest`) 隔离**：`...rest` 仅收集多余的位置参数，绝不收集命名参数。命名参数必须与函数签名严格匹配（名花有主）。对于开放式的配置项，不引入 `**kwargs`，而是推荐使用现有的字典解构 `f(opts = {})`。

## 8. N维索引与切片架构 (N-Dimensional Indexing & Slicing)
*   **统一路由 (Unified Routing)**：废弃 `__getslice__`，将所有 `[]` 操作统一路由至 `__getitem__(...dims)` 和 `__setitem__(...dims, val)`。利用变长参数机制，原生支持任意维度的索引与切片。
*   **一等公民 Slice (First-Class Slice)**：引入 `ObjSlice` 类型，彻底消除“高级索引 (List)”与“切片 (Slice)”在多维访问中的语义歧义。
*   **零 GC 压力 (Zero GC Overhead)**：`ObjSlice` 仅包含 `start, end, step`，无循环引用风险。底层通过**专属免锁对象池 (Free-list)** 分配，并依赖**引用计数 (RC)** 瞬间回收，完全绕过 Mark-and-Sweep 扫描，性能媲美栈分配。
*   **语法糖隔离 (Parser-Friendly Syntax)**：冒号 `:` 切片语法严格限制在 `[]` 内部使用（由 Parser 直接生成 `SliceExpr`）。在外部作用域，强制使用内置函数 `slice(start, end, step)` 创建切片对象。此举彻底避免了与三元运算符 `? :`、字典 `{k: v}` 及类型注解的语法冲突。
*   **C++ 切片解析层 (C++ Slice Utility)**：在 `jc2` 核心库中提供统一的 `ShapeStrider` 或 `SliceIterator` 工具类。Native 模块（如 Matrix, Tensor）接收到变长参数后，直接交由该工具类进行越界检查、负数回绕与内存步长计算，避免重复造轮子。

## 9. 标准库模块化与命名空间重构 (Stdlib Modularization & Namespace Refactoring)
*   **痛点**：当前 `BuiltinRegistry` 注册了超过 200 个全局原生函数，导致严重的命名空间污染、UFCS（统一函数调用）语义隐患以及自动补全困难。
*   **核心保留 (Core Prelude)**：保留最常用的基础数学函数（如 `sin`, `cos`, `sqrt`, `abs`, `len`, `print`）在全局作用域，维持 JC2 作为“科学计算器”的极佳手感。
*   **内置类型原型链 (Type Prototypes)**：将容器与字符串操作（如 `push`, `map`, `filter`, `split`, `join`, `keys`）从全局函数中彻底剔除，改为挂载到 `List`, `Dict`, `String`, `Matrix` 等内置类型的 `ObjClass` 原型上。这不仅符合纯正的 OOP 语义，还能极大提升 VM 方法查找（Inline Cache）的效率与安全性。
*   **管道操作符演进 (Pipe Operator)**：采用显式方法调用语法 `data |> .sort() |> .unique()`。在编译期解构为 `data.sort().unique()`，彻底消除作用域解析歧义，并完美兼容全局归约函数（如 `data |> .filter(f) |> sum`）。
*   **内置命名空间 (Built-in Namespaces)**：边缘函数打包为小写的命名空间（如 `sys`, `io`, `cas`），必须通过 `import sys` 显式引入。这不仅实现了与用户定义类型（大写驼峰）的视觉隔离，更实现了零开销的极速冷启动。
*   **鸭子类型谓词 (Duck-Typing Predicates)**：保留 `isiterable`, `iscallable` 等行为契约检查函数，以支持鲁棒的泛型编程与多态；而具体类型检查则收敛为 `type(x) == list` 或 `isinstance`。

## 10. 符号矩阵与高级特性 (Symbolic Matrix & Advanced Features)
*   **符号矩阵的双重身份 (Dual Role of SymMatrix)**：由于 `RealMatrix` 底层强制使用 `double`（在 $2^{53}$ 后丢失精度），`SymMatrix` 不仅用于代数推导，更是 JC2 中**唯一的精确数学矩阵**。通过 `symmatrix(r, c, ...)` 构建，内部的 `SymExpr` 节点可完美无损地承载任意精度的 `BigInt` 和 `Fraction`，从而支持精确的矩阵求逆与代数计算。
*   **内存模型与生命周期 (Memory Model & Lifecycle)**：`SymExpr` 是基于 DAG 的不可变 RC 对象。`ObjSymMatrix` 底层直接采用连续的 `std::vector<SymExpr>`（而非 `std::vector<Value>`）。这从类型系统层面彻底杜绝了挂载常规 GC 对象（如 List/Dict）的可能性，完美摆脱了循环引用问题。当 RC 归零时瞬间析构，**绝对不参与** GC 的 Mark-and-Sweep 扫描，实现真正的零 GC 负担。
*   **构建与类型提升网络 (Construction & Promotion Lattice)**：字面量 `[1, 2; 3, 4]` 绝对保留为 `realmatrix` 以维持极致性能。仅当混入符号变量时触发**按需懒提升 (Lazy Promotion)**。在 `BuiltinRegistry` 中，`matrix()` 构造函数和 `toMatrix()` 转换函数会自动探测元素类型，若包含 `symbolic` 则自动提升为 `symmatrix`。
*   **基础算术与表达式膨胀控制 (Arithmetic & Swell Control)**：在矩阵乘法 ($O(N^3)$) 的最内层循环中，强制调用了 `simplifyCore(expand_core(..., SymConfig::maxExpandTerms))`，在合并同类项的同时压平 AST 树，防止表达式呈指数级爆炸。
*   **高级线性代数 (Advanced Linear Algebra)**：彻底摒弃浮点 LU 分解。行列式计算 (`determinant()`) 实现了 **Bareiss 算法 (无分母高斯消元法)**，保证中间结果始终在多项式环内，除法为精确整除；求逆 (`inverse()`) 采用伴随矩阵法 (`adjugate()`) 除以行列式；支持矩阵的精确整数次幂 (`power(n)`) 和迹 (`trace()`)。
*   **与 CAS 引擎的深度联动 (CAS Integration)**：微积分与代数操作 (`diff`, `integ`, `limit`, `subs`, `simplify`, `expand`, `evalFloat`, `evalValue`) 已在 `SymMatrix` 内部实现，并自动在矩阵所有元素上广播。在 `BuiltinRegistry` 中，`cas.diff`、`cas.integ`、`cas.limit` 等接口已完美适配 `SymMatrix` 的分发。
*   **致命的“隐藏零”判定 (Zero Equivalence Problem)**：在矩阵消元（如 Bareiss 算法）寻找非零主元时，实现了 `isSymZero` 探测器。对于疑似零的符号表达式，强制执行最重型的 `full_simplify()` 进行确定性零等价判定，确保多元复杂表达式在除法操作前的绝对正确性。
*   **VM 泛型接口适配 (VM Generic API Integration)**：`SymMatrix` 已全面接入 VM 的泛型矩阵操作，包括逐元素运算 (`addE`, `subE`, `whereE` 等)、结构操作 (`reshape`, `sub`, `hcat`, `vcat` 等)、行列操作 (`swapR`, `delC` 等) 以及高阶函数 (`map`, `filter`, `reduce`, `zip` 等)，与 `RealMatrix` 和 `ComplexMatrix` 享有同等的一等公民地位。
*   **关系运算与显示控制 (Relational Ops & Display)**：符号矩阵的大小比较运算（如 `A > B`）与数值矩阵保持一致，直接抛出 `TypeError`。打印输出 (`operator<<`) 采用标准的二维对齐格式，并受全局 `g_printMatrix2D` 标志控制。

## 11. [规划中] CAS 模块终极性能重构方案 (CAS Ultimate Performance Refactoring Plan)
为解决 CAS 模块在复杂代数运算中的性能瓶颈（如字符串哈希开销、原子锁竞争、AST 展开爆炸等），制定以下自底向上的四步重构计划：

### 第一步：重塑底层基石（内存与哈希）
1. ~~**废弃字符串签名，引入 64 位结构化哈希**：在 `SymNode` 中彻底删除 `cachedSig` 和 `computeSignature()`。引入 `uint64_t hashValue`，在节点构造时利用子节点的哈希值和节点类型直接计算出 64 位整型哈希。~~ (已完成)
2. ~~**替换全局驻留池 (Interning Pool)**：将 `g_symPool` 的 Key 从 `std::string` 替换为 `uint64_t`。这会将所有表达式比较和查表的复杂度从 $O(L)$（字符串长度）降至 $O(1)$。~~ (已完成)
3. ~~**引入 Arena 内存池**：剥离 `std::shared_ptr`，改用线程局部的内存池（Arena Allocator）和裸指针 `SymNode*`。彻底消灭数以百万计的原子锁（引用计数）开销。~~ (已完成)

### 第二步：重写核心算术引擎（加法与乘法）
1. **$O(N)$ 级别的同类项合并**：在 `operator+` 和 `operator*` 中，废弃极其沉重的 `std::sort` 和 `compareSymNodes`。直接使用 `std::unordered_map<uint64_t, TermData>`（以节点的 64 位哈希为 Key）来聚合底数相同的项或同类项。
2. **消除无意义的递归展平**：优化 `flattenAdd` 和 `flattenMul`，在构造节点时就保证其扁平化，避免每次运算都重新遍历树。

### 第三步：引入独立的多项式代数引擎
1. **阻断 AST 与多项式的混用**：废弃依赖 `expand_core` 的 `extractCoeffs`，引入专用的内部数据结构（如 `SparsePoly`，基于 `std::map<int, SymExpr>` 或哈希表）。
2. **自底向上的多项式转换器**：实现 `toPolynomial(AST, var)` 函数。遇到加法就多项式相加，遇到乘法就多项式相乘，遇到幂次就多项式快速幂。**绝不展开无关变量**。
3. **重构底层数学库**：将 `polyDiv`、`polyGCD`、`polyResultant` 等函数的内部实现全部切换为使用 `SparsePoly` 结构，仅在算法结束时转换回 AST 节点。

### 第四步：高层算法的局部极致优化
1. **矩阵乘法防爆**：修改 `SymMatrix::operator*`。在最内层循环中只做纯粹的 AST 节点拼接（构建一棵巨大的 `SymAdd` 树），在计算完一个单元格的所有项后，**只调用一次** `simplifyCore`。
2. **模式匹配零拷贝**：修改 `matchAST`。将 `std::map<std::string, SymExpr> captures` 的按值拷贝替换为引用传递。在 DFS 深入时记录状态，回溯时手动 `erase`，实现零拷贝的模式匹配。
3. **Gröbner 基栈分配**：将 `Monomial` 结构体中的 `std::vector` 替换为 `boost::small_vector` 或固定大小的 `std::array`，消除 Buchberger 算法中海量的堆内存分配。
