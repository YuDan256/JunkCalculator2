# GC 悬垂对象缺陷（UAF）—— 证据与待决问题

> 状态：**未修复**（需要 GC 设计决策，作者拍板）。记录 2026-10-01。
> 影响：release 下约 7% 概率随机 `0xC0000005`（`--jit test tests` 约 93% 通过）；ASan 构建下 100% 命中。

---

## 1. 症状与复现

- **确定性复现（ASan）**：`out\build\x64-asan\jc2.exe test tests` → `heap-use-after-free`，退出码 1。
- **release 随机复现**：`out\build\x64-release\jc2.exe --jit test tests` 约 1/15 崩（`0xC0000005`），无 PASS/FAIL 输出。
- **与新增模块无关（决定性对照）**：把 `tests/` 复制一份、只删掉 `test_inflate.jc2`，其余 61 个原始测试照样崩，而且是另一条路径：
  `GcHeap::sweep` ← `GcHeap::allocate<ObjList>` ← `host_make_list`(ExtensionBridge.cpp:336) ← `crypto_x509Parse`(crypto_module.cpp:439)。
  新测试只是分配压力更大，把潜伏缺陷从"偶发"变成"必现"。

## 2. ASan 栈（实测）

```
#0 jc::Value::releaseObj    src/memory/Value.h:315
#1 jc::Value::~Value        src/memory/Value.h:347
#2 jc::ObjInstance::~…      (scalar deleting destructor)
#3 jc::GcHeap::sweep        src/memory/GcHeap.h:321
```

## 3. 被破坏的不变量（插桩实测）

在 sweep 的释放阶段加断言抓到：

```
[GcDiag] SWEEP_LIVE_CONTAINER obj=… type=10(CLOSURE) refCount=1
```

`GcHeap::sweep`（`GcHeap.h:196` 一带）的存活判定是：

```cpp
bool isAlive = curr->isMarked || (!isContainer && curr->refCount > 0);
```

即 **容器（CLOSURE / CLASS / INSTANCE / LIST / DICT …）只要没被 mark 就回收，完全不看 `refCount`**。
于是一个"被活 `Value` 持有（`refCount ≥ 1`）、但 mark 阶段扫不到"的容器被释放，留下悬垂 `Value`；后续任何一次析构/访问就是 UAF。

## 4. 已定位的一处具体窗口

把 ASan 报的分配点返回地址换算 RVA、对 `/MAP` 符号化：

```
RVA 0x005DE22D → jc::VM::copyClosureTemplate + 0x5D   (VM.cpp:477)
```

`copyClosureTemplate` 在 `allocate<ObjClosure>` 之后还有一长串赋值会**再**分配 GC 对象
（`paramTypes[i] = Value(...)`、`defaultValues = src->defaultValues`、`boundSelf/boundClass`…）。
这段窗口里副本既没进任何根（`refCount` 仍为 0，`objects_` 裸链不算根），
一旦 `collectGarbage()` 落进来就会被回收。调用方 `snapshotMembers`（VM.cpp:529）那层
`GcObjGuard` **出现得太晚**，包不住这段窗口。

已做（**局部**）修复：在 `copyClosureTemplate` 分配处立即 `GcObjGuard copyGuard(c);`（+8 行）。

## 5. A/B 对照：这只关掉了其中一个窗口

| 配置 | `--jit test tests` 通过率 |
|---|---|
| 带该 guard | 14/15 |
| 去掉该 guard | 14/15 |

⇒ 崩溃不止一处，`copyClosureTemplate` 只是 ASan 恰好抓到的那一处。

## 6. 已试过但**无效**的做法（避免重复劳动）

- 把 `refCount > 0` 对所有类型都当存活条件 → **不解决**；说明还存在"先减到 0 再回收、但别处仍有悬垂 Value"的路径。
- 用 helper 包装大对象调用（曾误以为能修）→ 只是扰动堆布局（40% → 25%），**不是修复**，已删除。

## 7. 需要作者决策的问题

1. **根集合纪律**：是否需要"分配即入根"的统一机制（例如分配后立刻登记到当前帧/临时根表，或让所有 `allocate` 返回受 `GcObjGuard` 保护的句柄），而不是逐个窗口打补丁？
2. **容器是否该看 `refCount`**：`sweep` 现在对容器一律忽略 `refCount`。是改成"`refCount > 0` 也算存活"（需评估循环引用导致的泄漏），还是坚持"容器必须可 mark"并补全所有入根路径？
3. **`objects_` 裸链的语义**：新分配对象在进入任何根之前，是否应有一个"新生代/临时根"区间（如 nursery）？
4. **是否引入写屏障或增量标记**：当前显然假设"GC 只在安全点发生"，但实测存在分配中途触发回收的窗口 —— 是收紧触发点（只在安全点回收），还是补根？

## 8. 当前状态

- 三套测试：**62/62、62/62、18/18**（release，解释器与 `--jit`）；
- `--jit test tests` 仍有约 7% 随机崩溃；
- 相关改动：`src/vm/VM.cpp` 的 +8 行局部 guard（**不是根治**）；诊断脚手架与临时开关均已移除；
- 复现配方：ASan 构建 + `jc2.exe test tests`（100% 命中）。

## 9. 复现手册（ASan 构建）

来自本次调查（原始报告在未跟踪的 `docs/ideas/inflate-task-report.md`）。

```powershell
# 1) 建 ASan 构建（目录已存在：out/build/x64-asan）
cmake -S . -B out/build/x64-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="/fsanitize=address"
cmake --build out/build/x64-asan
# 2) 跑：100% 命中 heap-use-after-free
out\build\x64-asan\jc2.exe test tests
```

**两个已知坑（踩过）**

1. ASan 的运行时 DLL 必须与 exe 放在一起，否则 exe 起不来（把编译器目录下的
   `clang_rt.asan_dynamic-x86_64.dll` 复制到 exe 同目录）。
2. `out/build/*/.ninja_deps` 容易陈旧：改了源码却看不到重新编译时，构建输出里不会有该文件的
   `Building CXX object ...`；此时删掉 `.ninja_deps` 再构建，或对照 repo/out 两侧文件的
   hash+mtime 是否一致 —— 否则会出现"改了也没生效"的假绿（本会话已因此误判过一次）。

**排期建议**：GC 缺陷单独排期，不要挂在别的任务上顺带修；修之前 `--jit test tests` 视为已知
flaky（约 5%~25% 失败），门禁用 `test tests` 与 `--jit test jit_tests`。


---
