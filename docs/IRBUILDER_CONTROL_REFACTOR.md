# IRBuilder 控制流 / 指令顺序重构方案

## 背景

当前 `IRBuilder` 用同一个 `currentControl` 表达两件事：

1. 最近的真实 CFG 节点；
2. 下一条指令的插入位置。

`RegisterAllocator::isControlSpine()` 也把大量**有副作用的指令**当成控制骨架：

```text
Call / Invoke
GetGlobal / SetGlobal
GetProperty / SetProperty
BuildNamespace / Class / Method
Import / Export
ListAppend / DictAppend
...
```

结果是“副作用 = 控制块”，导致：

- 条件分支里的 `include namespace` 两个 `BUILD_NAMESPACE` 顺序执行；
- `Export` 被当成 branch terminator；
- `BuildNamespace` 既像值节点又像 CFG 节点；
- 需要通过回溯 `Export.controlInput`、跳过 `FreeReg` 等补丁维持正确性。

## 目标

分离三个概念：

```text
currentSeq      最近生成的任意指令/值节点，用于线性顺序
currentControl  最近的真实 CFG 节点，用于分支/汇合/循环边
hasSideEffects  优化器属性，只影响 DCE/重排，不影响 CFG
```

成功标准：

- `isControlSpine` 只包含真正的 CFG 节点；
- `Export` / `Import` / `Call` / `BuildNamespace` 都是基本块内指令；
- `If` 的 `Merge` 只使用真实控制节点，不再回溯 `Export`；
- 条件 `include`、`A.B`、`A.c`、`collections`、`discrete` 全部通过；
- 四套判据与 70 文件 compile 通过。

## 一、IRBuilder 字段

```cpp
IRNode* currentControl; // 只指向 CFG 节点
IRNode* currentSeq;     // 最近一条指令/值节点
```

构造函数：

```cpp
currentControl = graph->startNode;
currentSeq = graph->startNode;
```

## 二、普通节点规则

所有非 CFG 节点统一：

```cpp
node->setControl(currentSeq);
currentSeq = node;
```

不再修改 `currentControl`。

包括：

- Constant / Move / 算术 / 比较；
- GetGlobal / SetGlobal；
- Call / Invoke；
- GetProperty / SetProperty；
- Import / Export；
- BuildList / BuildDict / BuildSet / BuildMatrix；
- BuildNamespace / NamespaceBegin；
- Class / Method / DefineProp；
- ListAppend / DictAppend；
- Defer / RunDefers。

`Export` 和 `Import` 只更新 `currentSeq`，不更新 `currentControl`。

## 三、CFG 节点规则

CFG 节点创建时同时更新两个字段：

```cpp
node->setControl(currentSeq);
currentControl = node;
currentSeq = node;
```

包括：

- Start
- If / IfTrue / IfFalse
- Merge
- Loop
- TryBegin / Catch / TryEnd
- Return / Throw / ThrowTyped

分支入口：

```cpp
currentControl = ifTrue;
currentSeq = ifTrue;
```

分支末尾：

```cpp
IRNode* thenControl = currentControl; // 不再从 Export 回溯
```

Merge：

```cpp
mergeNode->addData(thenControl);
mergeNode->addData(elseControl);
currentControl = mergeNode;
currentSeq = mergeNode;
```

循环、try/catch、match 采用同一规则。

## 四、RegisterAllocator

`isControlSpine()` 收缩为：

```cpp
Start
Return
If / IfTrue / IfFalse
Merge
Loop
TryBegin / Catch / TryEnd
Throw / ThrowTyped
```

移除：

```text
Call / Invoke
GetGlobal / SetGlobal / DeleteGlobal
GetProperty / SetProperty
BuildNamespace / NamespaceBegin
Import / Export
Class / Method
ListAppend / DictAppend
...
```

这些节点作为普通指令进入 `bb->instructions`，通过 `currentSeq` 的 `controlInput` 保证顺序。

CFG 连边不再需要：

```cpp
while (predCtrl && !nodeToBB.count(predCtrl))
    predCtrl = predCtrl->controlInput;
```

因为 `BuildNamespace` 已不在 `isControlSpine`，不会再制造“controlInput 是数据节点”的控制块。

## 五、Optimizer / Emitter

- `hasSideEffects()` 保持现状：决定 DCE 是否保留节点；
- 不再用 `hasSideEffects` 推导 CFG；
- Emitter 的 block 只由真正的 CFG 节点产生；
- 分支末尾的 `JMP` 由 CFG 后继决定，不由 `Export` 决定；
- JIT 继续对 `NAMESPACE_BEGIN` / `BUILD_NAMESPACE` / `EXPORT` deopt，直到调用约定稳定。

## 六、Namespace 收集

当前已完成：

- `namespaceMemberScopes`：嵌套 namespace 最终节点登记到父层；
- `isLocalBinding`：local namespace 不导出；
- uninit placeholder 解析到 class / 具名值 / global；
- `include` 关键字替代 `export`。

重构后保留这些，但删除：

- `visitIfExpr` 中针对 `Export` / `Import` 的 controlInput 回溯；
- 任何为了补偿 `BuildNamespace` 控制块而存在的特殊判断。

## 七、实施顺序

### Phase 1：加 currentSeq，不改变 isControlSpine

1. `IRBuilder.h` 增加 `currentSeq`；
2. 构造函数初始化；
3. 把所有 `node->setControl(currentControl)` 改为 `setControl(currentSeq)`；
4. 每次创建普通节点后 `currentSeq = node`；
5. 控制节点位置同时更新 `currentControl/currentSeq`；
6. 全量测试，行为应不变。

### Phase 2：收缩 isControlSpine

按组迁移并每次跑全量测试：

1. Import / Export；
2. GetGlobal / SetGlobal / 属性访问；
3. Call / Invoke；
4. BuildNamespace / NamespaceBegin / Class / Method；
5. 容器 append 类。

每组迁移后确认：

- CFG 块数下降；
- 无分支跳转错误；
- register liveness/FreeReg 正常；
- 69/70 文件测试通过。

### Phase 3：删除补丁

- 删除 `visitIfExpr` 的 `Export/Import` 回溯；
- 确认 `RegisterAllocator` 无 `namespaceTargetRegs` 之类的 FreeReg 跳过；
- 确认 Emitter 无针对 namespace 的临时寄存器补丁。

### Phase 4：文档与回归

- 更新 `docs/register_vm_design.md`，明确 CFG/指令/副作用三层；
- 回归：
  - `namespace A { include namespace B { c = 1 } }` → `A.B` / `A.c`
  - 条件 `include`
  - `collections.Stack/Queue`
  - `discrete.parseProp`
  - class 自引用
  - local namespace 不导出

## 八、风险

- `currentControl` 在 IRBuilder 中出现频繁，必须全局搜索并逐处判断是“插入点”还是“CFG”；
- Call/Invoke 变成普通指令后，寄存器分配和异常处理需要确认顺序；
- JIT 依赖 `isControlSpine` 的块结构，需跑 `unit --jit` 与 `jit`；
- Bytecode 序列化 / 反汇编假设块结构，需 compile-check；
- 任何阶段失败，保留 `currentSeq` 改动但回退该阶段 `isControlSpine` 条目，不要一次性删除所有条目。

## 九、验收命令

```powershell
taskkill /F /IM jc2.exe
cmake --build out\build\x64-release
out\build\x64-release\jc2.exe run tools\test.jc2
```

必须看到：

```text
unit: PASS
unit --jit: PASS
http: PASS
jit: PASS
compile-check: 70 files, 0 failures
ALL GATES PASSED
```

## 十、当前状态

已完成并提交：

```text
83d0810 fix(lang): connect conditional namespace branches
ea832d6 refactor(lang): rename export keyword to include
8cbc579 docs(lang): document namespace self name
a6840ea fix(lang): reconnect namespace CFG through export
feeb287 fix(lang): keep exported namespace members
```

当前 `currentControl` 仍承担双重职责；本方案是后续彻底重构的起点。

## 诊断记录（2026-10-29）

在 helper-only 阶段给 isitIfExpr 加 compare：

`	ext
IFDIFF then seq=SetProperty id=27 ctl=IfFalse id=23
`

说明在 isControlSpine 仍然包含 SetProperty 等副作用节点时：

- 分支真正的最后控制块是 SetProperty 这类节点；
- currentControl 仍停在 IfTrue/IfFalse；
- 直接把 Merge 边改成 currentControl 会跳过分支末尾的副作用块，导致
  8 个测试失败。

因此 If 迁移必须与 isControlSpine 收缩、数据节点 owner 解析一起做，
不能单独先改 Merge 边。

## 诊断记录（Round 6）

尝试 shrink + owner 解析 + Merge 边改 currentControl，并给块内指令加
stable_sort(id)：

- 最小 (x)=x*2 仍出现 Unimplemented opcode；
- 数学测试出现 Expected a string / Target is not callable。

结论：块内顺序不能靠 id 排序恢复。RegisterAllocator 目前依赖
isControlSpine 中的副作用节点作为隐式顺序边界；收缩 spine 必须同时引入
显式的“前一指令”依赖，否则数据节点会被排到错误位置。

下一步需要给 IRNode 增加序列边（或等价的 scheduler 输入），而不是继续
在 spine 列表和 Merge 边上局部调整。

