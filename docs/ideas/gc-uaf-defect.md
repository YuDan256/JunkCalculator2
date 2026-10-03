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

---

## 15. 根因与修复（flagsScratch：回收期漏掉的一条不可见边）

### 15.1 现场（A 级，全部来自同一次运行）

```
[W] ret 符号化栈：
  #1 jc::Value::~Value                          (Value.h:334)
  #2 jc::ObjInstance::`scalar deleting destructor' +0x1b
  #3 jc::GcHeap::sweep                          (删除循环)
  #5 …lambda_10 (sys.gc())                      (BuiltinRegistry.cpp:2123)
[C] clear-phase done visited=24 called=14        （清理阶段无遗漏；引爆实例 clear=1）
[T] ctor     inst=…1540 size=280 tail=7ffc000000000001 bebebe…   （构造时已是 uninit 的 Value）
[T] clearEnd inst=…1540 size=280 tail=fffc1252a87e8f80 bebebe…   （清理结束时已是闭包指针）
```

### 15.2 根因

`ObjInstance::flagsScratch`（`PropertyDescriptor`，含 `Value val`，声明在 `private:` 之后）被
`ownFlags()` 用作"槽位描述符返回值落点"：每次调用都把目标槽位的 `Value` **复制**进去。
它既不被 `markObj` 遍历（INSTANCE 分支只走 `classDef`/`properties`/`slots`，且它私有），
也不被 `clearTotal` 清空 ⇒ 回收期**这条边既不被保命也不被断开**。
受害闭包先被释放，实例析构时销毁 `flagsScratch` ⇒ 对已释放对象记账 ⇒ heap-use-after-free。

### 15.3 修复

`ObjInstance::clearTotal()` 中把 `flagsScratch.val` 置空：属两阶段回收里"先断边、后释放"漏掉的一步。
（另一可选形态：让 `ownFlags` 不再复制 `Value`、只返回标志位；改动面约 46 个调用点，安全性收益为零
—— 容器不看 `refCount`，这条 scratch 引用不会钉住任何对象，唯一害处就是没断边。）

### 15.4 判据

| 判据 | 结果 |
|---|---|
| ASan 最小复现 | 修复前 100% UAF → 修复后 `asan = 0` |
| ASan 全套 | 修复前 100% UAF → 修复后 `exit = 0`，62/62，`asan = 0` |
| release 三套 | 62/62 · 62/62 · 18/18 |
| release `--jit test tests` ×15 | 0 崩溃 |
| 反向验证 | 去掉该行 ⇒ UAF 重现 |

### 15.5 A/B

- 先测"标记它"（`markObj` 里 `markValue(inst->flagsScratch.val)`）⇒ **阴性**：实例本身不可达，
  给不可达对象的字段做标记不产生任何保护；
- 再测"断边"（`clearTotal` 中置空）⇒ **全绿**。

### 15.6 方法学

定位这一步靠三件事：① 在 `~Value` 里对"目标已被真正释放"打点并**就地符号化**调用栈（dbghelp，
放在独立 .cpp 里以免污染头文件）；② 用对象尾部的**三时点位模式采样**（ctor/clearEnd/dtor）确定
"这条 `Value` 何时出现"；③ 两次单变量 A/B 区分"标记"与"断边"。此前多轮弯路的主因是测量无效
（陈旧构建缓存、探针自解引用、补丁未生效）——这些已作为纪律写入 `local/LESSONS.md` 第一章。

---

## 16. 甲重构：`flagsScratch` 与那条断边都不复存在

`ObjInstance::flagsScratch`（`PropertyDescriptor`，内含 `Value val`）曾是 8d9a09d 需要"手动断边"的原因：
`ownFlags()` 把槽位 `Value` 复制进它并返回指针，而 `markObj` 遍历不到该私有成员 ⇒ 回收期这条边既不被
保命也不被断边 ⇒ 受害闭包先死、实例析构时记到已释放对象上，即 heap-use-after-free。

甲重构把它从源头去掉：

- `PropFlags` + `ObjInstance::propFlags(k)`：按值返回标志，不复制 `Value`、不写成员；
- `ObjInstance::ownStorage(k, Value*&, PropFlags&)`：返回**真实存储**指针（槽位 `&slots[idx]` / 溢出表 `&it->second.val`）；
- `VM::findPrivateMember` 出参改为 `Value*& foundVal, PropFlags& foundFlags`，10 个调用点同步；
- 删除旧 `ownFlags`、`flagsScratch`，以及 `clearTotal` 中那条断边行及其注释。

**验证**：ASan 最小复现与全套全绿（62/62）；release 三套 62/62 · 62/62 · 18/18；`--jit test tests` 连跑 15 次 0 崩溃；
反向验证（恢复复制 `Value` 进 scratch 的形态）重现 UAF。

**顺带修复的既存缺陷**：写私有槽位成员原来写进 scratch 副本（丢写），现在写真实存储。

**结论**：本缺陷的**成因**（一条只存在于回收期、GC 既看不见也不断开的计数引用）已从结构上移除；
`8d9a09d` 的断边改动成为历史防护，不再是必需的补丁。
