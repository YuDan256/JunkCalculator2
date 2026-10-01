# 并发方案备忘：协程优先，线程承其上（M:N）

> **状态：方案稿，未实施。** 供语言作者审阅，本文不改任何源码。
> **性质：内部设计记录。** 所有"事实"条目都来自本机读码 + 实测（`out/build/x64-release/jc2.exe`，2.6.3.2），
> 每条给 `文件:行号`。查不到或没验的另列 §6，**不猜**。
> 涉及但不属于本文范围：`docs/register_vm_design.md`（寄存器机调用约定）、`docs/JIT_DESIGN.md`（JIT/GC 协作）。

---

## 1. 结论摘要

1. **协程可行，而且比预期便宜。** 帧与寄存器**整体在堆上**（`VM.h:53-57`，`VM.cpp:3543-3544`），
   JC2→JC2 的字节码调用**不同 C++ 递归**（`execCall` 只压 `frames[]` 后返回，`VM.cpp:1436`），
   所以"换个上下文继续跑"只需要让 `run()` 退出、重进时按 `frames[i].ip` 续跑——
   实测 4000 层递归只报 VM 自己的 `CallFrame stack overflow`（§6 实验 E1），C++ 栈不参与。
2. **但 `run()` 的续跑状态目前全是 C++ 局部量**（`frame`/`chunk`/`code`/`frameRegs`/`ip`，`VM.cpp:4270-4274`）。
   要么"每个协程一次 `run()` 调用、靠 C++ 栈保存续跑点"，要么把这些量搬进上下文对象。
   **这是第 1 期的全部工作量所在**，也是本方案唯一的真实架构决策点（见 §5 问题 1）。
3. **线程门槛高得多的根因不是 VM，是 GC 与全局单例。** `GcHeap` 是进程单例（`GcHeap.h:50-53`）、
   双向链表无锁（`GcHeap.h:74-81`）、`markCallback` 是**全局唯一一个**且绑在最后构造的 VM 上
   （`VM.cpp:3547` 覆盖式赋值），而 `VM::activeVM` 也由构造函数写入（`VM.cpp:3541`）——
   **今天"建第二个 VM"就会把第一个 VM 的根扫描打断**。再加上 `DeoptRegistry`、
   `g_internedStrings/g_internedTypes`、`nativeSelfStack`、`jitInvokeInfos()` 等一批进程级可变全局。
4. **建议分期：1 协程地基 → 2 协程接 I/O → 3 线程 + GC 协调。**
   第 1 期不碰 GC、不碰 JIT、不碰线程；第 2 期才有"用户可见收益"（多连接 / 连接复用）；
   第 3 期是一次真正的运行时重构，**不建议与 1/2 期并行**。
5. **最大风险两条**：① JIT 把 `registers` 指针缓存在 **R14**（`CodeEmitter.h:30-35`），
   协程切换后 R14 指向旧上下文——可绕过（切换只发生在原生 callout 边界），但必须在第 1 期就写进约束里；
   ② GC 的时间点是"任何分配点"（`GcHeap.h:71-73`），多线程下等于**任意位置都可能停世界**，
   在没有安全点的解释器里无法收敛，必须显式设计（§3 第 3 期）。

---

## 2. 实测事实

### A. VM 执行模型

**A1. 帧与值栈的表示。**两者都是**堆上的一次性大数组**，不是 `std::vector`：
`registers = new Value[MAX_REGISTERS]`，`MAX_REGISTERS = 1024*1024`（`src/vm/VM.h:53,56`；分配 `src/vm/VM.cpp:3543`）；
`frames = new CallFrame[MAX_FRAMES]`，`MAX_FRAMES = 1024`（`src/vm/VM.h:54,57`；分配 `src/vm/VM.cpp:3544`）。
`CallFrame` 只有索引与裸指针字段（`ip`、`registerBase`、`returnRegister`、`deferBase`、`closure`、`selfContext`…），
**没有任何指向 C++ 栈的引用**（`src/vm/VM.h:22-38`）。`registerBase` 是"全局寄存器数组的滑动窗口基址"
（`src/vm/VM.h:26`），返回值靠父帧记录的 `returnRegister` 回写（`src/vm/VM.h:27`）。

**A2. 调用/返回只改这两个数组。**
入帧：算 `newBase = 父帧 registerBase + localCount + refCount`，把实参写进 `registers[newBase+i]`，
`frames[frameCount++] = newFrame`（`src/vm/VM.cpp:1419-1436`，`execCall` 路径；
`callVMFunction` 路径同构：`src/vm/VM.cpp:4041-4113`）。
出帧：`runDefersDownTo` → `closeUpvalues` → 结果写 `getReg(returnRegister)` → 把该窗口整段清成 `none` →
`frameCount--`（`src/vm/VM.cpp:4331-4356`，OSR 返回分支；解释器返回分支同构）。
**关键：`execCall` 压完帧就 `return`，不递归 `run()`**——
所以 `f()` 调 `f()` 一万层只吃 VM 的 `MAX_FRAMES`，不吃 C++ 栈。

**A3. 但 `run()` 的续跑点住在 C++ 栈上（本方案的核心约束）。**
`run()` 开头把帧状态取成局部量一次，此后整个分发循环只用这些局部量：
`CallFrame* frame = &frames[frameCount-1]`、`const Instruction* code = chunk->code.data()`、
`Value* frameRegs = &registers[frame->registerBase]`、`int ip = frame->ip`（`src/vm/VM.cpp:4270-4274`）；
`#define getReg(idx) frameRegs[idx]`（`:4297`）；主循环取指/分发在 `:4522-4535`。
**"接着上次的 ip 跑"这一能力目前只由 `run()` 这层 C++ 栈帧提供**：协程切换 = 让 `run()` 返回 +
调度器另起一次 `run(targetDepth)`。一次 `run()` 只能服务一个协程的**连续执行段**，
不能中途被别的协程插入后再"回到原 `run()` 内部"——除非把这 5 个局部量整体搬进上下文结构（§5 问题 1）。

**A4. `run()` 的边界 == 协程可切换点。**全仓 `run(targetDepth)` 自递归只有 7 处，全在"边界/回调"上：
`src/vm/VM.cpp:1356`（JIT callout 调起字节码）、`2974`、`3105`（模块初始化体）、`4207`（`execute()` 脚本入口）、
`4800`（`jc2_jit_call_helper`）、`9766`、`9793`（JIT invoke/super_invoke helper）。
`callVMFunction`（`:4034`，末行 `return run(targetDepth)` `:4156`）是另一入口，供原生回调使用
（`src/main.cpp:539`）。**今天所有"嵌套进入 VM"都发生在原生调用点（native callout）上**——
这正是"换栈只发生在原生边界"的先决条件。

**A5. 全局状态清单（协程/线程必须一起换的东西）。**只列"可变的"；`helpers::evalCallback` 等注入回调
（`src/vm/BuiltinRegistry.h:112-123`；`src/main.cpp:530-586`）全局只读，不列。
| 状态 | 位置 | 协程期 | 线程期 |
|---|---|---|---|
| `VM::activeVM` | `src/vm/VM.h:368`；`VM.cpp:3541` 构造时写；`main.cpp:507` | 随上下文切换 | `thread_local` |
| `registers` / `frames` / `frameCount` | `src/vm/VM.h:56-57,98` | 每上下文一套，或共享+换出 | 每 VM 一套 |
| `openUpvalues` | `src/vm/VM.h:93`；`captureUpvalue` `VM.cpp:865` | 每上下文（**悬挂在寄存器窗口上**） | 每 VM 一套 |
| `deferStack` + 每帧 `deferBase` | `src/vm/VM.h:77,30`；`VM.cpp:891-894` | 每上下文（**按全局下标裁剪**） | 每 VM 一套 |
| `exceptionHandlers` | `src/vm/VM.h:102-109` | 每上下文（存 `frameIndex`） | 每 VM 一套 |
| `currentTargetFrameDepth`、`pendingCallRefs`、`pendingFrameBase/Count` | `src/vm/VM.h:76,111,113-114`；`VM.cpp:4268` | 每上下文一套 | 每 VM 一套 |
| `jit_exception_value` / `jitPinList` | `src/vm/VM.h:202-204` | 每上下文/每 VM | 每 VM |
| `helpers::nativeSelfStack`、`nativeClassStack` | `src/vm/BuiltinRegistry.h:244-245`（`inline` 全局） | 全局够用 | 必须 `thread_local` |
| `g_internedStrings` / `g_internedTypes` | `src/memory/Value.h:102,106` | 共享可以 | 需锁 |
| `DeoptRegistry`（`maps_`, `nextBase_`） | `src/jit/runtime/Deoptimization.h:39-60` | 共享可以 | 需锁 |
| `g_jc2_jit_deoptimized` / `g_jit_pending_exception` | `src/jit/runtime/Deoptimization.h:68-69` | 随上下文（JIT 返回码） | `thread_local` |
| `jitInvokeInfos()` / `jitSuperInvokeInfos()` | `src/vm/VM.h:400-413`；只在编译期 `push_back` 后取 `back()` 地址烘进机器码（`src/jit/frontend/BytecodeToHIR.h:1165-1166`） | 可共享 | 并发编译需锁 |
| `g_autoDebug`、断点、`currentDebuggerFrame` | `src/main.cpp:137`；`src/vm/VM.h:372,386` | **跨协程互相打断** | `thread_local` 或禁调试 |
| `g_interruptRequested` / `g_isWaitingForInput` | `src/vm/EngineInterrupt.h:16-17`（已 `std::atomic`） | 全局语义正确 | 全局语义正确 |

**A6. `--jit` 对栈/帧的假设。**
① 机器码把"本帧寄存器基址"缓存在 **R14**：prologue `movq r14, rcx/rdi`（`CodeEmitter.h:30-35`），
此后所有寄存器访问走 `[r14 + idx*8]`（`src/jit/pass/InstructionSelector.h:521-529`），
`LinearScan` 把 R14 排除出分配池（`src/jit/pass/LinearScan.h:37`）。
**含义：JIT 帧一旦进入，就期望 R14 在整段执行期不变。**
② 入口是 C++ 侧函数指针直调：`func(&registers[newBase])`（`src/vm/VM.cpp:4119`）、OSR `func(frameRegs)`（`:4310`）
→ JIT 帧**住在原生 C++ 栈上**。
③ GC 协调靠 StackMap + 主动同步：`jc2_jit_gc_pin`/`unpin`（`src/jit/runtime/Deoptimization.h:76-123`）、
`jc2_jit_sync_frame`（同文件 `127-179`）。
④ 全局变量地址经 `globalsDataPtr` 二次间接：`movabs r11, &globalsDataPtr` 再解引用
（`src/jit/backend/CodeEmitter.h:972-975`），VM 侧在 `globals.push_back` 后刷新
（`src/vm/VM.cpp:3545,4586,4638,4903`）并 `clearAllGlobalICs()`（`src/vm/VM.h:227-234`）。
⑤ 调原生函数走 callout，参数带上 R14 当前值（`src/jit/backend/CodeEmitter.h:1359,1370`）。
（本段 `CodeEmitter.h` 均指 `src/jit/backend/CodeEmitter.h`。）

**A7. 实测（E1）**：4000 层 JC2 递归 → `RuntimeError: CallFrame stack overflow.`，
进程不崩、异常可被 `try/catch` 捕获（脚本与输出见 §6）→ VM 栈的边界是**受控报错**，不是 C++ 栈溢出。

### B. GC

**B1. 算法与触发点。**标记-清除 + 引用计数混合：`Obj` 带 `isMarked`/`refCount`/双向链表指针（`GcHeap.h:34-44`）。
触发在**分配点**：`allocate<T>()` 先 `if (shouldCollect()) collectGarbage();`（`GcHeap.h:66-74`），
`shouldCollect()` 是**纯计数阈值** `allocsSinceGc_ >= gcThreshold_`（初值 65536，`GcHeap.h:119-121,250`），
每轮 GC 后 `gcThreshold_ = max(65536, surviving*2)`（`GcHeap.h:206`，本文件引用均指 `src/memory/GcHeap.h`）。
**没有时间/内存量触发，没有安全点，没有写屏障。**

**B2. 存活判定（最重要的两句）。**
`bool isAlive = curr->isMarked || (!isContainer && curr->refCount > 0);`（`GcHeap.h:159`）。
配套 `freeObj` 只允许**非容器且非驻留字符串**的纯数据对象被立即 `delete`，
LIST/DICT/SET/CLOSURE/CLASS/INSTANCE/SUPER_PROXY/NAMESPACE/UPVALUE/STRING/TYPE_DEF 一律"交给 GC 统一回收"
（`GcHeap.h:96-117`）。**含义：容器只有"被 mark 到"才能活；非容器只要 C++ 栈上还有 `Value` 就活着。**

**B3. 根扫描扫什么。**唯一入口是 `GcHeap::markCallback`，在 `VM` 构造函数里**整体赋值**（覆盖式）：
`src/vm/VM.cpp:3547`。它扫：`globals[]`（`3548-3550`）→ 逐帧的 `selfContext/classContext/jitReturnSlot/closure/chunk->constants/chunk->inlineCaches`
（`3552-3565`）→ **`registers[0 .. maxReg]`，其中 `maxReg` 只算到"最深帧的
`registerBase + localCount + refCount`"**（`3567-3572`）→ `pendingFrameBase` 窗口（`3574-3578`）→
`openUpvalues` 链（`3580-3584`）→ `pendingCallRefs`（`3586-3588`）→ `deferStack`（`3590-3592`）→
`loadedModules`/`builtinClosures`/`builtinValues`（`3594-3604`）→ 各 proto（`3605-3609`）→
`helpers::nativeSelfStack/nativeClassStack/jc::nativeTempRefs`（`3611-3613`）→
`compiledFunctions` 的常量池与 IC（`3615-3623`）。
另外两条根通道：`tempObjRoots_`/`tempValueRoots_`（`GcObjGuard`/`GcValueGuard`，
`src/memory/GcHeap.h:132-138,253-265`，在 `collectGarbage` 里 mark：`src/memory/Value.h:3476-3479`），
以及 `jitPinList`（JIT 期间 pin，`src/jit/runtime/Deoptimization.h:98-113`）。
**没有任何保守的 C++ 栈扫描**（全仓无 `setjmp`/栈范围注册；`GcHeap` 无栈边界字段）。
`gc_locked_` 这个字段存在且 `collectGarbage` 首尾会翻转（`src/memory/Value.h:3471-3472,3507`），
但全仓**没有任何地方读它做同步**（grep 仅命中定义与这两行）。

**B4. 与原生模块交错。**可以交错，且机制是被刻意设计过的：
原生调用点在 `registers[...] = fn(args)`（`src/vm/VM.cpp:1502`），
实参在 C++ 侧是一个 `std::vector<Value>` 局部量（`src/vm/VM.cpp:1439-1461`）——
容器元素靠"调用者寄存器仍在根集里"活着；**新造的容器必须自己上 `GcObjGuard`**，
这就是 `BuiltinRegistry.cpp` 里上百处 guard 的原因（如 `src/vm/BuiltinRegistry.cpp:4270,4288,4299`）。
扩展 SDK 侧的对应设施是 `nativeTempRefs` + `protect()`（`src/lib/ExtensionBridge.cpp:9,18-21`），
且 `host_make_list` 等分配器一律 `protect` 返回值（`src/lib/ExtensionBridge.cpp:335-338`）。
**实测（E7）**：在原生高阶函数 `list.sort`（C++ `std::stable_sort`）的比较器里每 20 次回调就
"分配 300 个 dict + `sys.gc()`"，200 元素排序仍完全正确（`cmpCalls=2292 sortedOk=true`，`--jit` 下同，见 §6）。
该路径经过 `helpers::safeCallValue` → `callValueCallback` → `callVMFunction` → `run()`
（`src/vm/BuiltinRegistry.h:123-129`；`src/vm/BuiltinRegistry.cpp:4301-4303`；`src/main.cpp:559,535-539`），
即**在原生调用内部重入 VM 是既有且被支持的用法**（这条正是第 1 期"在原生边界让出"的立足点）。

### C. 阻塞与 I/O

**C1. 现在全是阻塞系统调用，且没有超时。**
`socket` 模块的 `::send`（`socket_module.cpp:81`）、`::recv`（`:93`）、`::accept`（`:114`）、
`::connect`（`:142`）、`::bind`/`::listen`（`:181,188`）全部阻塞，**send/recv/accept 未设任何超时**
（以下均指 `src/lib/socket/socket_module.cpp`）。
TLS 传输层 `TcpStream` 是"最小阻塞式 TCP 客户端"（`src/lib/internal/net/TcpStream.h:1-2`），
用阻塞 socket + `SO_RCVTIMEO`/`SO_SNDTIMEO`（`TcpStream.cpp:109-111,133,149`），
握手靠 `while (!handshakeComplete()) recv(...)` 忙等（`src/lib/tls/tls_module.cpp:165-167`）。
另有 `sys.sleep` = `std::this_thread::sleep_for`（`src/vm/BuiltinRegistry.cpp:2449-2451`），
是**最容易变成 yield 点的现有原语**。

**C2. `net.TcpServer.accept` 的位置与行为。**JC2 侧 `modules/net.jc2:58-65` 直通原生 `sock_accept`
（`socket_module.cpp:111-119`）→ **线程级阻塞，无法中断、无超时**；帮助文本自述 "Blocks until a browser/client connects"（`modules/net.jc2:125-135`）。

**C3. 有没有现成的非阻塞/事件循环可复用？——有一半。**
- **非阻塞 connect + `select` 已存在**，但只在 `TcpStream::connect` 里，且用完立刻切回阻塞模式：
  `ioctlsocket(FIONBIO, 1)` → `::connect` → `WSAEWOULDBLOCK` 时 `select(0,nullptr,&writeSet,...)`
  → `getsockopt(SO_ERROR)` → `ioctlsocket(FIONBIO, 0)`（`src/lib/internal/net/TcpStream.cpp:70-108`）。
  **这段是第 2 期事件循环的直接可复用件。**
- **没有任何可用的通用事件循环**：全仓无 `WSAPoll`/`epoll`/`WSAEventSelect`/`IOCP`；
  `select` 只出现在上述一处；`Win32` 消息循环只在 `window` 模块的 UI 线程里
  （`src/lib/window/window_module.cpp:70-75`）。
- `modules/http.jc2` **每请求都主动断连**：请求头硬写 `Connection: close`（`modules/http.jc2:331`），
  注释明确"每跳都要关"（`modules/http.jc2:373`）。→ 第 2 期的"http 复用连接"要从零做（chunked 解码已存在，§6-8）。

**C4. 线程/原子/锁的现状。**
`std::thread`：**仅 1 处** —— `window` 模块的 UI 线程 `winThread = std::thread(&NativeWindow::threadFunc,...)`
（`src/lib/window/window_module.cpp:32,154`）。
`std::mutex` + `std::lock_guard`：**仅 4 处** —— 同文件的 `eventQueue`/`displayBuffer` 保护
（`src/lib/window/window_module.cpp:36,40,43,109,197,219`）。
`std::atomic`：`g_isWaitingForInput`/`g_sigintCount`（`src/main.cpp:33,48`）、
`g_interruptRequested`（`src/cas/Symbolic.cpp:25`，声明 `src/vm/EngineInterrupt.h:16`）、
window 的 `hwnd`/`running`/`cursorVisible`（`src/lib/window/window_module.cpp:28,30,31`）。
`std::async`：只用于**计算并行**（矩阵/BigInt/Decimal/Tensor，如 `src/math/Matrix.h:302-333`）——
是"另起线程做纯计算"，不共享 VM 状态。
`thread_local`：`nativeTempRefs`（`src/lib/ExtensionBridge.h:13`）、
CAS 的记忆化表与符号池（`src/cas/Symbolic.cpp:135,230`、`src/cas/Factorization.cpp:1625,1644`）、
Tensor 的 RNG（`src/lib/tensor/Tensor.h:152`）、若干递归深度计数。
**JIT 编译线程：不存在。** `JIT_DESIGN.md` 里写的"后台线程编译"未实现；
`compileForOSR` 是同步的（`src/vm/VM.cpp:3320` 起，调用点在 `run()` 循环内）。
`checkInterrupt()` 只在**辅助/库循环**里被轮询（CAS、矩阵、BigInt、内置高阶函数等，约 100 处），
**主分发循环里没有**（`src/vm/EngineInterrupt.h:19-23` 的定义与 `src/vm/VM.cpp:4535` 的 `switch` 之间无调用）。

### D. 语义面

**D1. 全局表结构与可变性。**`std::vector<Value> globals` + `std::unordered_map<std::string,uint32_t> globalNames`
+ `std::unordered_set<std::string> constGlobals`（`src/vm/VM.h:59,61,62`）都是 **VM 实例成员**。
可变性：`setGlobal` 在**名字不存在时追加**新槽并刷新 `globalsDataPtr`（`src/vm/VM.h:303-312`）；
`removeGlobal` 置 `none` 并**从名字表删除**，槽位不复用（`src/vm/VM.h:259-267`）；
`getGlobals()` **整表拷贝**（`src/vm/VM.h:269-273`）。IC 缓存的是**槽位编号**（`src/vm/VM.cpp:4562-4577`），
任何增删都配 `clearAllGlobalICs()`（`src/vm/VM.h:266`、`src/vm/VM.cpp:4587`）。
→ 共享一张全局表意味着：任意协程**首次读一个尚不存在的内置名**就会改写 `globalNames`、
让 `globals` 重新分配（`src/vm/VM.cpp:4583-4588`）并让**所有**已编译函数的 IC 失效——共享语义的硬冲突点。

**D2. 异常/错误状态：进程级全局 + VM 实例级，二者都有。**
错误类指针是**进程级全局**：`err::TypeErrorClass` 等（声明 `src/memory/Exceptions.h:19-39`，
定义 `src/memory/Exceptions.cpp:8-26`，注册时写入 `src/vm/PredefinedClasses.cpp:911-947`）。
VM 侧另有 `exceptionClass`（`src/vm/VM.h:366`）与 `jit_exception_value`（`src/vm/VM.h:202`）。
`throw_error*` 抛 `Jc2Error{ObjClass*, typeName, message}`（`src/memory/Exceptions.h:44-60`；
host 实现 `src/lib/ExtensionBridge.cpp:325-333`），运行期由 `VM::wrapException` 包成异常实例
（`src/vm/VM.cpp:3220-3261`）。挂起的 handler 是 **VM 实例成员**：`exceptionHandlers` 记录
`frameIndex/ip/registerBase/errReg/deferBase`（`src/vm/VM.h:102-109`），展开见 `handleExceptionUnwind`
（`src/vm/VM.cpp:3183-3218`）。→ 类对象跨线程只读可共享；`exceptionClass`/`jit_exception_value`/`exceptionHandlers` 必须随 VM 走。

**D3. `defer` / `try` / 迭代器 / 闭包在"换栈"时的风险点。**
- **`defer` 是 VM 级单调栈**：`deferStack`（`src/vm/VM.h:77`）由 `DEFER` 压、`RUN_DEFERS` 弹
  （`src/vm/VM.cpp:7576-7587`），`runDefersDownTo(targetBase)` 按 `deferStack.size()` **裁剪**（`:891-894`），
  每帧只记整数 `deferBase`（`src/vm/VM.h:30`，入帧如 `:1605`）。
  **风险：共享一个 `deferStack` 时，A 的 `runDefersDownTo` 会按全局下标弹出并执行 B 的 defer。**
  实测（E8）确认 LIFO + 异常展开：`g-defer → f-defer-2 → f-defer-1`。
- **`try/catch`**：handler 记的是 `frameIndex`（`src/vm/VM.h:103`）→ **跨栈切换后指向别的上下文的帧**。
- **迭代器**：`opIterInit`/`opIterNext` 的迭代状态是**普通 `Value` 对象**（`src/vm/VM.cpp:8884,9005`），
  没有 C++ 侧迭代器；`for-in` 的隐式状态本就在 `registers[]` 里，跟着上下文走即可 → 换栈本身安全。
- **闭包/upvalue**：未闭合 upvalue 直接**指向寄存器槽**（`captureUpvalue` `src/vm/VM.cpp:865`；
  链头 `openUpvalues` `src/vm/VM.h:93`），闭合靠 `closeUpvalues` 按**全局寄存器下标**比较（`:946`）。
  **风险：共享 `registers` 数组时，一个协程的 `closeUpvalues(n)` 会闭合另一个协程的 upvalue**
  → 这是"共享值栈 vs 每协程独立值栈"的核心取舍（§5 问题 2）。
- **`state`**：`tests/core/test_state.jc2:3-22` 表明它是**按函数名绑定的持久存储**，与调用帧无关
  → 多协程**共享**同一 `state`。必须由作者拍板（§5 问题 5）。

---

## 3. 建议分期

### 第 1 期：协程地基（VM 上下文可切换 + 最小 `spawn`/`yield`）

**目标**：单个 OS 线程内跑多个 JC2 执行上下文，能确定性交替。
**改动清单（文件级）**
| 文件 | 改动 |
|---|---|
| `src/vm/VM.h` / `src/vm/VM.cpp` | 把 §A5 表里标"每上下文一套"的字段收进 `VMContext` 结构；`VM` 持有 `VMContext* current` + 就绪队列 |
| 同上 | `run()`（`VM.cpp:4261`）改为"绑定上下文后取局部量"；新增"让出"出口：保存 `frame->ip` 后 `return` 哨兵值（或 `YieldSignal`），由 `VM::execute`/`callVMFunction` 的调用者识别 |
| 同上 | `activeVM` 语义从"进程唯一 VM"改为"当前 VM"（协程内不变，`VM.h:368`） |
| `src/vm/BuiltinRegistry.cpp` | 新增 `coro` 命名空间：`spawn(f)`、`yield(v)`、`resume(h, v)`、`status(h)`、`join(h)`；`spawn` 走 `callVMFunction` 创建新上下文 |
| `src/main.cpp` | 顶层 `execute()` 后跑一个**极简调度器**（单线程、FIFO、run-until-yield） |
| `tests/core/test_coroutine.jc2`（新） | 断言：交错顺序、返回码、异常穿透、`defer` 顺序、嵌套 spawn |

**验收**：`jc2.exe test tests` + `jc2.exe --jit test tests` 全绿；新测试断言"交错序列 == 期望序列"；
`E1`（深递归）与 `E7`（原生回调里 GC+重入）行为不变。
**工作量粗估**：6–10 天（其中约一半花在把 `run()` 的局部量搬进上下文并保证快路径不变慢）。
**这一期不做什么**：不碰 GC（不新增根扫描，只保证"上下文换出后其 `registers[]` 仍在现有 `maxReg` 扫描范围内"）、
不碰 JIT（明确约束：`yield` 只允许出现在原生 callout 边界，见 §5 问题 1）、
不做线程、不做非阻塞 I/O、不改 `defer`/`state` 语义。

### 第 2 期：协程接 I/O（非阻塞 socket + 事件循环）

**目标**：`TcpServer` 单线程同时服务多连接；`http` 能复用连接。
**改动清单（文件级）**
| 文件 | 改动 |
|---|---|
| `src/lib/socket/socket_module.cpp` | 给 socket 加非阻塞模式开关与 `wouldBlock` 语义（现有阻塞点：`:81,:93,:114,:142`）；新增 `select`/`poll` 原语（可复用 `TcpStream.cpp:70-108` 的 `FIONBIO`+`select`+`SO_ERROR` 手法） |
| `src/lib/internal/net/TcpStream.cpp` | 抽出"非阻塞 connect/read/write 的可挂起版本"，供 `tls` 与 `socket` 共用 |
| `src/lib/tls/tls_module.cpp` | 握手循环（`:165-167`）与 `pumpIn`（`:103-113`）改为可让出 |
| 新增 `src/lib/async/`（或并入 `BuiltinRegistry`） | 单线程事件循环 + 定时器轮（`sys.sleep` 改走定时器，`BuiltinRegistry.cpp:2449-2451`） |
| `modules/net.jc2` | `TcpServer.accept`（`:58-65`）在事件循环下让出；保持阻塞语义兼容（无循环时退化为阻塞） |
| `modules/http.jc2` | 去掉硬编码 `Connection: close`（`:331`）与逐跳关闭（`:373`），加连接池 |
| `tests/lib/test_net_async.jc2`、`tests/lib/test_http_keepalive.jc2`（新） | 单线程 8 连接并发 echo；同连接连续两次请求 |

**验收**：`test tests` 三套全绿；新增测试在**单线程**下证明并发（断言"连接 B 在 A 未读完时已完成响应"）。
**工作量粗估**：10–15 天（其中 `http` 改动小、`socket` 与 TLS 改造是大头）。
**这一期不做什么**：不引入任何线程；不做 UDP/HTTP2；不改 GC；不动 JIT。

### 第 3 期：线程（安全点 + GC 协调 + 全局语义定案 + M:N）

**目标**：N 个 OS 线程跑 M 个协程上下文。
**改动清单（文件级）**
| 文件 | 改动 |
|---|---|
| `src/memory/GcHeap.h` | `GcHeap` 从单例（`:50-53`）改为可按 VM 持有**或**加全局 GC 锁；`objects_` 链表加锁；`gc_locked_`（`:63`，现无任何读者）真正投入使用 |
| `src/memory/Value.h` | `g_internedStrings`/`g_internedTypes`（`:102,106`）加锁或改每 VM；`sweepCallback` 的 intern 表清理（`src/vm/VM.cpp:3626-3641`）同步 |
| `src/vm/VM.cpp` | `markCallback`（`:3547`）从"覆盖式单例赋值"改为**注册表**（每 VM 一个扫根器）；`activeVM`（`:3541`）改 `thread_local`；`g_autoDebug`/断点（`src/main.cpp:137`、`VM.h:372,386`）改 `thread_local`；解释器主循环加**安全点轮询**（`:4522-4525` 的回边/调用点），复用 `g_interruptRequested` 的既有模式（`src/vm/EngineInterrupt.h:19-23`）作"停世界"信号 |
| `src/jit/runtime/Deoptimization.h` | `DeoptRegistry`（`:39-60`）加锁；`g_jc2_jit_deoptimized`/`g_jit_pending_exception`（`:68-69`）改 `thread_local` |
| `src/vm/BuiltinRegistry.h` | `nativeSelfStack`/`nativeClassStack`（`:244-245`）改 `thread_local` |
| `src/main.cpp`、`src/vm/VM.h` | M:N 调度器：N 线程 × 每线程就绪队列 + 工作窃取；跨线程唤醒用 `std::condition_variable` |

**验收**：`test tests` 三套全绿；新增 `tests/threads/` 压力测试（多线程各跑 GC 密集循环，
连续 10 分钟无崩溃 + 对象计数守恒）+ `--jit` × 线程组合专项。
**工作量粗估**：25–40 天，**且必须先由作者定案 §5 的语义问题**，否则会返工。
**这一期不做什么**：不做并行 GC（先停世界）、不做无锁、不做 JIT 内联缓存跨线程共享。

---

## 4. 第一刀垂直切片（最小可验证 demo）

**目标**：证明"帧可以整体换出、能确定性交替、异常与 defer 语义不乱"，且**不引入 GC/JIT/线程**。

```jc2
// 设想 API（第 1 期落地形态，待作者定名）
import coro

trace = @[]
worker(name, n) = {
    defer trace.push(name + ":defer")
    i = 0
    while (i < n) {
        trace.push(name + ":" + str(i))
        coro.yield(i)          // 让出，把 i 交给 resume 的返回值
        i = i + 1
    }
    return name + ":done"
}

h1 = coro.spawn(worker, "A", 3)
h2 = coro.spawn(worker, "B", 2)
r1 = coro.resume(h1)      // 期望 A:0
r2 = coro.resume(h2)      // 期望 B:0
r3 = coro.resume(h1)      // 期望 A:1 …（手工 round-robin，确定性）
assert(coro.status(h1) == "done", "h1 必须跑完")
assert(coro.join(h1) == "A:done", "返回值必须原样穿透")
```

**断言清单（每条对应本节一个证据）**
1. **顺序**：`trace` 精确等于手工 round-robin 推演序列 → 证明帧真的被整体换出（A2/A3）。
2. **返回值**：`coro.yield(v)` 的值成为 `resume` 的返回值，`join` 拿到 `return` 值 → 证明返回值通道（`VM.h:27`）换栈后不错位。
3. **`defer` 隔离**：h1/h2 的 `*:defer` 只在自己结束时出现且 LIFO → 证明每上下文独立 `deferStack`（D3、`VM.cpp:891-894`）。
4. **异常隔离**：h1 内部 `throw` 只让 `resume(h1)` 抛出，h2 不受影响 → 证明每上下文独立 `exceptionHandlers`（D2、`VM.cpp:3183-3218`）。
5. **不回归**：`E1`（深递归仍报 `CallFrame stack overflow`）、`E7`（原生回调里 GC+重入仍正确）在**带协程**的构建上结果不变 → 证明第 1 期没偷偷改 GC 与 JIT 的契约。

---

## 5. 待作者拍板的语义问题

**问题 1：`yield` 允许出现在哪里？**（决定第 1 期是"改 `run()`"还是"只加调度器"）
- **A（推荐）**：只在**原生调用边界**让出（`yield`/I/O 原语本身即 native function）。代价：纯计算循环插不进让出点；
  收益：JIT 完全不用动（R14 缓存在 callout 时天然失效，`CodeEmitter.h:30-35`），实现量最小。
- **B**：任意位置可让出（含 JIT 内部）。代价：要为机器码生成栈图/续跑点，等于重做一套 JIT 级协程支持
  （现有 StackMap 只服务去优化）。
- **C**：只有字节码可让出，JIT 帧内遇 `yield` 先强制去优化。代价：性能抖动，需 `invalidateJIT` 配合。

**问题 2：值栈"每协程一套"还是"共享 + 换出"？**（决定内存与 upvalue 正确性）
- **A（推荐）**：每协程独立 `registers[]`。收益：`closeUpvalues`（`VM.cpp:946`）不会误闭合别人的 upvalue，
  `openUpvalues`（`VM.h:93`）天然隔离；代价：每协程 1M 槽 × 16B ≈ **16 MB**（`VM.h:53`）→
  必须**同期**把 `MAX_REGISTERS` 改成按需增长，否则 1000 连接 = 16 GB。
- **B**：共享一块值栈、切换时 memcpy 换出。收益：不改 `MAX_REGISTERS`；代价：换栈 O(活跃窗口)，
  且 upvalue/`deferStack` 的全局下标全要改成"上下文相对"。
- **C**：共享 + 每协程只留自己那段窗口的栈式分配器（B 的优化版）。代价：最复杂，收益中后期才显现。

**问题 3：`globalNames`/`globals` 多线程下共享还是隔离？**
- **A**：每线程一份（隔离）。收益：零语义争议，`globals` 重分配与 IC 失效的竞态（D1）直接消失；
  代价：跨线程共享计数器要显式传通道。
- **B**：共享 + 全局锁。收益：符合直觉；代价：**首次读一个未定义全局名就会改表并 `clearAllGlobalICs()`**
  （`VM.cpp:4583-4588`、`VM.h:227-234`），等于反复给所有线程的 JIT 代码失效。
- **C**：默认隔离 + 显式 `shared` 声明（需要语言层新语法）。

**问题 4：GC 的停世界策略。**
- **A（推荐）**：全局 GC 锁 + 安全点轮询（解释器回边/调用点插 poll，复用 `EngineInterrupt` 模式）。
  代价：所有线程必须能到安全点；长原生调用要靠现有 `checkInterrupt` 轮询点（约 100 处）兜住。
- **B**：每 VM 独立堆 + 独立 GC。收益：无全局停世界；代价：跨线程传对象要跨堆，GC 复杂度暴涨。
- **C**：线程期禁止触发 GC，只在主线程 GC。代价：语义奇怪，只适合过渡。

**问题 5：`state`（`tests/core/test_state.jc2:3-22`）在协程/线程间是否可见？**
- **A**：每上下文一份 → 最"并发友好"，但与现有"同名函数共享状态"的直觉冲突。
- **B**：全局（当前语义）→ 兼容现状，但并发下就是共享可变状态，需文档明确警告。
- **C**：全局 + 提供 `local state` 变体。

**问题 6：`VM::activeVM` 的最终定位。**
- **A**：`thread_local`，协程切换时不改。代价：JIT helper 的 `VM::activeVM->getCurrentFrame()`（如 `VM.cpp:610`）
  在协程切换后会指向另一个上下文 → 必须保证"JIT 帧执行期间不发生协程切换"（与问题 1 选项 A 一致）。
- **B**：协程切换时同步改 `activeVM`。更安全，但**`VM.cpp` 里 `VM::activeVM` 出现 200+ 次**，A/B 工程量差距很大。

---

## 6. 未查清 / 需要实验的部分

**已跑的实验（脚本在 `D:\Temp\jc2-conc-exp\`，不在仓库内；均为 `out/build/x64-release/jc2.exe` 实跑）**
| 编号 | 内容 | 结果 |
|---|---|---|
| E1 | 4000 层 JC2 递归；900 层 × 16 局部量递归（E4） | 前者 `RuntimeError: CallFrame stack overflow.`（可 `try/catch`，进程不崩）；后者正常返回 → 证 A2/A7 |
| E7 | `list.sort` 的比较器里每 20 次回调分配 300 dict + `sys.gc()`，200 元素 | `cmpCalls=2292 sortedOk=true`；`--jit` 下同 → 证 B4 的"原生回调内可重入 GC" |
| E8 | `defer` + 异常跨帧展开 | `@["g-defer", "f-defer-2", "f-defer-1", "caught:from g"]` → LIFO + 异常路径执行，证 D3 |

**未查清（明确不猜，需要作者或后续实验确认）**
1. **"每协程一次 `run()`"在深度混合调用下的 C++ 栈消耗**：`run()` 自递归（A4 的 7 处）在"协程 A 的
   JIT 帧 callout 进协程 B"的极端交错下会不会叠 C++ 栈？需先有协程原型才能实测。
2. **`registers[]` 改按需增长后地址是否仍稳定**：`MAX_REGISTERS`（`VM.h:53`）动态化后 JIT 的 R14 假设
   （`CodeEmitter.h:30-35`）会不会被破坏？`globals` 有 `globalsDataPtr` 刷新 + `clearAllGlobalICs` 先例
   （`VM.cpp:3545,4586`）可参照，但寄存器数组更热，需实测。
3. **非 `execute()` 的 `run()` 入口能否安全让出并重入**：`execImport`（`VM.cpp:2703`）与模块初始化体
   （`2974,3105`）让出后重入时，`loadedModules`/`builtinModules` 的半初始化状态没有验证。
4. **`exceptionHandlers` 的 `frameIndex` 是否稳定**：协程是"独立 `frames[]`"还是"共享数组 + 分区"未定，
   这决定 `handleExceptionUnwind`（`VM.cpp:3190`）与 `buildStackTrace`（`VM.cpp:3278`）要不要改。
5. **`GcHeap` 单例保留时的多 VM 注册语义**：现为覆盖式赋值（`VM.cpp:3547`），改注册表的具体形态
   （callback 列表？还是 GcHeap 持有 VM 列表？）未评估。
6. **原生扩展 ABI 的线程可重入性**：`ExtensionBridge` 的 `Env::ctx` 是每扩展一份静态上下文；
   多线程同调一个扩展函数是否会互踩？只做了 grep（未见锁），**未逐一审计 `src/lib/*/`**。
7. **`window` 模块的 UI 线程算不算"必须到安全点的线程"**：它已有 `std::mutex` + 事件队列
   （`window_module.cpp:36-47`），但没有与 GC 交接的约定；第 3 期停世界时必须回答。
8. **chunked 已查清：已实现**（`decode_chunked` `modules/http.jc2:102-137`，调用 `:259-261`）
   → 第 2 期 http 只剩"连接池 + 去掉 `Connection: close`"，比原估计小。列此留痕。
