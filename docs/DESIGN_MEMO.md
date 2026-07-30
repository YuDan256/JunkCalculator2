# JC2 核心设计须知备忘录 (Core Design Memo)

本文档记录了 Junk Calculator 2 (JC2) 架构开发中的核心设计哲学、语义界定与踩坑记录，供后续 C++ 底层开发与重构参考。

## 1. 核心语义与内存模型 (Core Semantics & Memory Model)
*   **字符串化**：`toString()` (对应 `__str__`) 目标是“人类可读”，不带引号；`toRepr()` (对应 `__repr__`) 目标是“开发者无歧义”，严格区分类型（如 `"1"` vs `1`）。
*   **内存语义**：`Matrix` 和 `Array` 采用**值语义**（深拷贝，无副作用）；`List`, `Dict`, `Set`, `Instance` 采用**引用语义**（共享内存，由 GC 追踪）。
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
*   **数值与符号边界**：标准数学函数允许符号节点提升为 `SymFunc`，不兼容函数遇符号变量立即求值或报错。
*   **视图提取器**：`__match__` 解耦对象内部结构与外部匹配接口。返回 `self` 触发平凡拦截，回退至默认字段匹配。

## 6. 底层规范与序列化 (Low-Level Rules & Serialization)
*   **零依赖与标准 C++ 哲学 (Zero-Dependency & Standard C++ Philosophy)**：JC2 严格禁止使用任何非标准 C++ 特性（如编译器特有扩展），且**绝对禁止**引入任何第三方库（如 libffi、Boost、GMP 等）。所有核心功能（包括大整数、正则、CAS、JIT FFI 等）必须基于 C++20 标准库从零手写，以保证极致的跨平台可移植性与代码的绝对掌控力。
*   **GC 保护**：C++ 内部操作未挂载的 GC 对象必须使用 `GcObjGuard`。
*   **JCB 字节码格式**：小端序，支持调试信息剥离。常量池深度反序列化（支持 BigInt 二进制块与冻结 Namespace）。值语义对象依赖 RC，引用语义对象依赖 GC Root 扫描。

## 7. 命名参数与调用约定 (Keyword Arguments & Calling Convention) [规划中]
*   **语法风格**：采用 Python 风格的等号传参 `f(a = 1, b = 2)`。
*   **解析器 (Parser) 规则**：上下文敏感解析。在函数调用的参数列表中，顶层的 `标识符 = 表达式` 强制解析为 `KeywordArgNode`。若需在传参时进行变量赋值，必须加括号 `f((a = 1))`。位置参数必须严格在命名参数之前。
*   **VM 预对齐 (Pre-alignment)**：VM 负责在运行时根据闭包的参数名元数据，将命名参数动态路由到正确的局部变量槽位。
*   **Native C++ 扩展无痛升级**：扩展 `bind_method` 注册 API，允许 C++ 侧提供参数名列表（如 `{"a", "b", "rtol"}`）。VM 在调用 Native 函数前，自动将位置参数和命名参数“预对齐”成一个标准的 `std::vector<Value>`，C++ 函数体逻辑无需任何修改。
*   **可变参数 (`...rest`) 隔离**：`...rest` 仅收集多余的位置参数，绝不收集命名参数。命名参数必须与函数签名严格匹配（名花有主）。对于开放式的配置项，不引入 `**kwargs`，而是推荐使用现有的字典解构 `f(opts = {})`。
