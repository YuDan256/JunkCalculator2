# 编译器 / VM TODO

> 基线：`a788dc77`。工作区干净。解释器与 `--jit` 测试通过。
>
> 本文是完整待办，不只包含 dunder 显式帧。旧 `7a9cd95` 清单的行号已变化，
> 只记录可核验的代码位置。
>
> 包含：
>
> ```text
> P0 dunder 显式帧剩余部分
> P1 for-in 重复 IS_UNINIT
> P1 ListPattern 元组脱糖
> P2 合成索引 double -> int
> P2 state 叶子重复检查
> P3 private static / trait private static
> P3 初始化器无限定名
> P3 state / static 合并
> P3 缺失测试
> ```

## 已完成

```text
P1  static local X = none 误判
P2  类体初始化器访问 private
dict/set 可重入索引
dict/set O(1) 删除
sys.disasm EXTRAARG 修复/verbose 合并
空 AST 操作数崩溃
Token 帮助修正
for-in / comprehension 重复 IS_UNINIT
合成索引 double -> int
```

VM 操作码协议主路径已改为显式帧：

```text
算术 / 位运算 / 比较
bool / str / len
iter / next
getitem / setitem
getattr / setattr
mapping / unpack
cmp / rcmp
INVOKE 的 __getattr__
```

递归 `__eq__` / `__add__` / `__getitem__` 的 opcode 回归存在。

---

# P0：dunder 显式帧剩余部分

状态：**未完成**。不是“全部完成”。

## 1. Value::equals / hash

```text
src/memory/Value.h:2894    Value::equals
src/memory/Value.h:3022    __eq__
src/memory/Value.h:3106    __eq__
src/memory/Value.h:3110    __eq__
src/memory/Value.h:3743    ValueHasher::operator()
src/memory/Value.h:3957    __hash__
```

这些路径调用：

```text
invokeDunder
    -> BuiltinRegistry::invokeDunder
        -> VM::callDunder
            -> invoke
                -> run
```

仍然是 C++ 栈上的同步递归。

影响：

```text
dict/set 的 key 比较
dict/set 的 key 哈希
Value::equals 直接调用
```

验收：

```text
递归 __eq__ 作为 dict key 触发
    -> JC2 CallFrame stack overflow
    -> 不依赖原生栈余量
    -> 不出现固定深度魔法数字
```

## 2. dict / set 原生索引

```text
ValueHasher
ValueEqual
ObjDict::find
ObjSet 查找
```

目前通过 `Value::equals` / `ValueHasher` 间接同步调用 dunder。
键索引必须改为：

```text
请求 dunder
挂起当前执行体
计算 hash/equals
恢复查找
```

不能只在 opcode 层迁移。

## 3. native builtin

```text
src/vm/BuiltinRegistry.cpp:298   invokeDunder
src/vm/BuiltinRegistry.cpp:334   callDunder
src/vm/BuiltinRegistry.cpp:388   __hash__
src/vm/BuiltinRegistry.cpp:1039  __abs__
src/vm/BuiltinRegistry.cpp:3011  __str__
src/vm/BuiltinRegistry.cpp:3164  __str__
src/vm/BuiltinRegistry.cpp:3178  __len__
src/vm/BuiltinRegistry.cpp:4225  __iter__
src/vm/BuiltinRegistry.cpp:4239  __next__
src/vm/BuiltinRegistry.cpp:4715  __len__
src/vm/BuiltinRegistry.cpp:4863  __str__
src/vm/BuiltinRegistry.cpp:6914  __iter__
src/vm/BuiltinRegistry.cpp:6926  __next__
```

`str` / `len` / `print` / `abs` 等 native 路径仍同步等待。

## 4. helper

```text
src/vm/BuiltinRegistry.h:231   spreadPositional -> __unpack__
src/vm/BuiltinRegistry.h:306   __iter__
src/vm/BuiltinRegistry.h:323   __next__
src/vm/VM.cpp:11674            compareValues
src/vm/VM.cpp:10514            opIterInit
src/vm/VM.cpp:10648            opIterNext
```

`compareValues` 还被 native `min/max/sort` 使用：

```text
src/vm/BuiltinRegistry.cpp:2339 ...
src/vm/BuiltinRegistry.cpp:5325 ...
```

VM 操作码在无法调度时仍回退到 `compareValues`：

```text
src/vm/VM.cpp:6238 EQ
src/vm/VM.cpp:6300 NEQ
src/vm/VM.cpp:6321 LT
src/vm/VM.cpp:6392 LE
src/vm/VM.cpp:6451 GT
```

## 5. JIT callout

```text
src/vm/VM.cpp:11981  jc2_jit_*
src/vm/VM.cpp:12012 ...
src/vm/VM.cpp:12095 ...
src/vm/VM.cpp:12331 ...
src/vm/VM.cpp:12371 ...
```

JIT 比较/真值/调佣 helper 仍同步 `callDunder`。

## 6. 错误与 finalize

```text
formatException __str__
delete/finalize __del__
PredefinedClasses __str__
```

这些是错误/析构边界，必须明确是否属于允许的同步边界。

---

# P1：for-in 重复 IS_UNINIT

状态：**已完成**（`1610da3f`）。

```text
for-in / comprehension 的 buildPatternMatch 不再重复发射 IS_UNINIT。
反汇编只剩循环守卫的一位 IS_UNINIT。
```

---

# P1：ListPattern 元组脱糖

状态：未做。

```jc2
@[a, b] = @[b, a + b]
```

目标不再生成：

```text
BUILD_LIST
MATCH_INIT
MATCH_SHAPE
INDEX_GET
```

改为直接：

```text
读旧 a, b
计算 a + b
写回 a, b
```

必须保留：

```text
RHS 先求值
state 首次初始化
失败原子性
const/ref/state 写入规则
自定义 __match__
```

---

# P2：合成索引 double -> int

状态：**已完成**（`a788dc77`）。

```text
ListPattern / MatrixPattern / rest 的合成索引已改为 int。
反汇编显示 0 / 1 / -1。
```

---

# P2：state 叶子重复检查

状态：需要先定语义。

已知：

```text
0001  外层检查 up[0]
0019  再检查 up[0]
0024  检查 up[1]
```

需要决定：

```text
state 模式是否保证所有目标一起初始化
```

再决定能否删除叶子检查。

---

# P2/P3：语义决定

## private static 的 self 访问

```jc2
self.A
self.A = 9
self.g()
```

当前可用，但帮助写“Static members are NOT reachable through an instance”。

需要决定：

```text
禁止
或允许并文档化 private 例外
```

## private static 与实例方法同名

```jc2
static local X = 1
local X() = 2
```

当前报重复；公开版本可共存。需要统一规则。

## trait private static 的宿主可见性

```jc2
trait T {
    static local const X = 1
    getT() = class.X
}
class U with T {
    getU() = class.X
}
```

需要决定：

```text
trait-private
或复制/提升到宿主
```

## 类体初始化器无限定名

已支持：

```jc2
static local B = class.A
local D = class.A
```

未支持：

```jc2
static local B = A
local D = A
```

需要决定是否把类成员名作为初始化器无限定名。

## state / static 合并

只有设计讨论，未实现。

前置规则：

```text
函数内 static local 非法
类内 static local 合法
```

---

# 缺失测试

```text
self 访问 private static
private static 字段 / 实例方法同名
trait private static 可见性
初始化器无限定名
dunder 显式帧（dict/set key）
native len/str/print 递归 dunder
JIT callout 递归 dunder
```

---

# 建议顺序

```text
1. for-in 重复 IS_UNINIT              小、确定
2. 合成索引 double -> int             小、低风险
3. ListPattern 元组脱糖               收益最大，需设计
4. P3/P4/P5 语义决定                  先定规范
5. Value::equals / hash               核心 P0
6. dict/set 索引                     依赖 5
7. native builtin / JIT               依赖 5
```

---

# 已确认的预期行为

`LOAD_NONE` 不是缺陷；它对应 `FreeReg` 的 GC 生命周期清理。
