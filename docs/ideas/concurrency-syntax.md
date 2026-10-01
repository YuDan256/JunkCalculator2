# 并发（协程 + 线程）语法与行为规范（定稿）

> 状态：**已定稿**（2026-10-01，作者拍板：关键字升格、`yield expr` 双向、无红蓝函数、全量形态）。
> 本稿只定**语法与行为**；实现分期与文件级改动见 `concurrency-plan.md`。
> 起草过程中用到的既有事实都标了出处；凡我未能亲自复核的，标注"待实现期确认"。

---

## 1. 范围

- **回答**：语言表面长什么样（关键字、形态、句柄方法）、程序员的可见行为（何时让出、取消怎么发生、线程之间能传什么）。
- **不回答**：VM/GC/JIT 怎么改、调度算法、性能。
- **原则**：能定的都定死；只有确实需要作者再拍板的才留在 §8。

---

## 2. 既有风格基线（提案必须相容）

| # | 既有事实 | 出处 |
|---|---|---|
| 1 | 关键字表存在且集中：`local` / `state` / `defer` … | `src/frontend/Lexer.cpp:11-48` |
| 2 | `state` 是按闭包绑定的持久槽，同闭包的子闭包共享；顶层写 `state` 是可捕获的 `SyntaxError` | `tests/core/test_state.jc2` |
| 3 | `defer` 是块/帧级 LIFO，异常展开时按序执行，并有 `e.suppressed` | `tests/features/test_defer.jc2` |
| 4 | 修饰符是**前缀链**（`local` / `const` / `ref` / `state`） | `tests/core/test_modifiers.jc2` |
| 5 | `namespace` 是一等对象 | `tests/features/test_namespace.jc2` |
| 6 | 命名参数（kwargs）用 `;` 分隔 | `docs/DESIGN_MEMO.md`（kwargs 一节） |
| 7 | 测试必须静默：不得 `print` | `docs/TEST_SPEC.md` |
| 8 | 闭包共享捕获（非快照） | `tests/features/test_functions.jc2` |
| 9 | `sys.sleep` 现在是**真阻塞** | 待实现期确认具体调用点 |

**破坏面（升格关键字前必须先处理）**：`spawn` 现被 `examples/tetris.jc2` 用作标识符（2 处）；`yield` 被 `tests/core/test_io.jc2`、`tests/features/test_member_model.jc2` 用作标识符（各 1 处）。`join`（7 处）、`cancel`（1 处）也被占用，因此它们**不**升格为关键字（见 §3）。

---

## 3. 定稿：语法形态

### 3.1 新增关键字（三个）

| 关键字 | 形态 | 说明 |
|---|---|---|
| `spawn` | `spawn f(args)` / `spawn name f(args)` | 新建协程；带名字的形式把 `name`（标识符）绑定为句柄的名字，供调试与内省 |
| `yield` | `yield` / `yield expr` | 让出当前执行体；**双向**（见 §4.3） |
| `thread` | `thread f(args)` / `thread name f(args)` | 新建线程；名字语义同 `spawn` |

**不新增**：`async`、`await`、`join`、`cancel`、`resume`、`pause`、`select`（前两个明确被否决；后几个保持为句柄方法/不引入）。

### 3.2 句柄（`spawn` / `thread` 的返回值）

句柄是值，可赋值、可放进列表/字典；**不可跨线程传递**（§4.7）。

| 方法 | 行为 |
|---|---|
| `h.resume(value = none)` | 恢复一个挂起的执行体，`value` 成为对应 `yield` 表达式的结果；返回该执行体**本次让出**产出的值（`yield expr` 的 `expr`），若它已结束则返回其结果。对已结束的句柄调用 → 报错（不是 `none`，避免与"产出了 none"混淆） |
| `h.join(timeout = none)` | 等它跑完并返回结果；体内未捕获异常在此**重抛**；`timeout` 到期 → 抛超时错误，执行体不受影响 |
| `h.cancel()` | 请求协作式取消（§4.5） |
| `h.isAlive()` | 是否仍在运行 |

`h.join()` 与 `h.resume()` **自身就是让出点**（当前执行体挂起）。

### 3.3 最小样子

```jc2
// 协程：并发抓两个地址再汇总
a = spawn fetch("https://example.org/")
b = spawn fetch("https://www.apple.com/")
ra = a.join()
rb = b.join()
print(ra.status, rb.status)

// 双向 yield：把中间值交给消费者，恢复时拿回新输入
gen = spawn producer()
v = gen.resume(none)          // 拿到 producer 第一次 yield 的值
w = gen.resume(v + 1)         // 传回去，成为那次 yield 表达式的结果

// 线程：独立全局表，函数的返回值/异常通过 join 传递
t = thread work(42)
r = t.join()                  // work 里未捕获的异常在这里重抛
```

---

## 4. 定稿：行为规范

### 4.1 无红蓝函数

- **任何函数**都可以 `yield`，也可以调用会让出的操作；没有函数着色、没有 `await`、没有"必须在协程里才能调用"的限制。
- 普通调用 `f()` 仍是**同步**执行：它在当前执行体里跑完。**只有 `spawn` / `thread` 新建执行体** —— 读代码时必须能靠这两个关键字判断哪里会产生新的执行流。
- 顶层脚本本身是一个**隐式主执行体**，顶层 `yield` 合法。
- `yield` 在 `defer` 块、`catch` 块、闭包体内**都合法**（无着色 ⇒ 无需作用域限制）。

### 4.2 让出点

- **能 yield 的函数**：全部。
- **真正切换执行体的位置**：只在**原生调用边界**（JIT 把寄存器基址缓存在 R14，换栈只能发生在原生 callout 处）。
- **会让出的调用清单**（第 1 期随实现落地，之后必须与帮助文本同步维护）：
  - 会让出：socket 的 `connect/send/recv/accept`、`h.join()`、`h.resume(value)`、新增的 `sleep(ms)`、`channel.send/recv`（第 2 期起）。
  - 不会让出：`print`、算术与比较、`list.sort`、`json.parse`、字符串操作、`sys.gc()`。
- **持锁/持事务期间让出由使用者负责**：语言不做自动保护，也不会因此报错。
- **`sys.sleep(ms)` 语义不变（真阻塞）**；需要"不阻塞其他执行体地等待"用新增的 **`sleep(ms)`**（在协程内让出并在到期后由调度器恢复；在顶层主执行体里同样只是让出）。这条是我按"不静默改变既有脚本行为"的原则定的。

### 4.3 `yield expr` 是双向的

- `yield x`：产出 `x`；恢复时若无值传入，该表达式结果为 `none`。
- `v = yield x`：产出 `x`，恢复时传入的值成为表达式结果（`h.resume(v)`）。
- `yield`：不产出值（等价于产出 `none`）。
- 产出/传入的值遵循普通值语义；跨线程的那一套限制（§4.7）**不**适用于协程之间（同一线程、同一全局表）。

### 4.4 调度与 `defer` / upvalue / `state` / `try`

| 项 | 定稿行为 |
|---|---|
| 调度顺序 | 就绪队列 FIFO（第 1 期）；优先级以后再加 |
| `defer` 与让出 | **让出时不执行** `defer`（让出 ≠ 作用域退出）；`defer` 只在作用域正常退出、异常展开、协程结束/被取消时按 LIFO 执行 |
| upvalue 跨让出 | **活的**（与既有共享捕获一致）：让出/恢复不产生快照，恢复后看到的是最新值 |
| `state` 协程间 | **共享**（保持现状；隔离会静默改变既有 `state` 代码在并发下的结果） |
| `try/catch` 跨让出 | **合法**；`catch` 能捕获取消异常（§4.5）并可以再抛 |
| `yield` 的内存序 | 语言不做任何承诺（单线程下无意义；多线程下的可见性由第 3 期规范另定） |

### 4.5 取消（协作式）

- `h.cancel()` 只是**置请求**；取消在**下一个让出点**生效。
- 生效时：该执行体内抛 `CancelledError`，可被 `catch` 看见、可再抛；`defer` 按 LIFO 照常执行。
- 取消后：`h.isAlive()` 为 false；`h.join()` **重抛 `CancelledError`**（不返回 `none`，避免与正常返回 none 混淆）。
- 线程取消与协程**同一口径**（不让用户去记"我在协程里还是线程里"）。

### 4.6 线程：共享模型

- **每个线程一套独立全局表**（线程里看不到主线程的全局变量，反之亦然；`state` 因此自然各自独立）。
- **跨线程只传值**：没有共享可变状态、没有锁模型（共享可变 + 锁与 JIT 的全局内联缓存存在硬冲突，见计划稿）。
- **通信靠**：`thread f(args)` 的参数与 `h.join()` 的返回值，以及第 2 期起的 `channel`。
- 线程里未捕获的异常：**存到句柄上**，`join()` 时重抛；**不自动打印**（测试必须静默）。

### 4.7 跨线程传值规则（运行期检查，违反抛 `ThreadError`）

| 类别 | 结论 |
|---|---|
| 数值、字符串、布尔、`none`、`bytes` | 可传（按值） |
| 列表、字典 | 可传（**深拷贝**：对面拿到的是副本） |
| 闭包/函数值 | **不可传**（它们捕获的环境属于原线程） |
| 协程/线程句柄 | **不可传** |
| `channel` 句柄 | 可传（它本身就是为跨线程通信设计的） |
| 原生句柄（socket、TLS、文件、窗口） | **不可传**（线程亲和） |
| 类实例 | 仅当其成员全部为"可传值"时可传，否则运行期 `ThreadError` |

### 4.8 结构性要求

- 第 1 期只做**句柄式**（`spawn`/`thread` + 句柄）；**结构化并发块**（如"作用域结束时自动 join 所有子执行体"）不进第 1 期，作为第 3 期的候选，见 §8。
- 线程承载协程（M:N）是**第 3 期目标**；第 1、2 期是单线程内的多协程。

---

## 5. 完整示例

同一件事（并发抓 3 个 URL + 汇总）在两种模型下：

```jc2
// 模型一：单线程多协程（第 1–2 期）
local import http

urls = @["https://example.org/", "https://www.apple.com/", "https://www.cloudflare.com/"]
hs = @[]
for (u in urls) { hs.push(spawn http.httpGet(u)) }

total = 0
for (h in hs) {
    res = h.join()          // 让出点：等这个协程完成
    total += res.body.len()
}
print(f"total={total}")

// 模型二：线程 + 值传递（第 3 期）
th = @[]
for (u in urls) { th.push(thread fetch_len(u)) }   // u 是字符串：按值传
sum = 0
for (t in th) { sum += t.join() }                  // 返回的是数值：按值传
print(f"total={sum}")

fetch_len(u: string) -> int = {
    local import http
    return http.httpGet(u).body.len()
}
```

**反例（会被拒绝）**：

```jc2
t = thread work(sock)        // ThreadError：原生句柄不可跨线程
t2 = thread work(f)          // ThreadError：闭包捕获了原线程的环境
```

---

## 6. 未采用的候选（备查）

- 库对象式 API（`coro.spawn(...)`）——与"语言底层原语"的定位不符，作者否决。
- `async` / `await` 与**函数着色**——明确否决；`await` 的功能由 `h.join()` 承担。
- `yield` 作为库调用（`coro.yield(v)`）——被关键字取代。
- 让出位置放宽到"任意语句之间/任意表达式内部"——第 1 期不做（JIT 的 R14 约束）；将来若放宽属于增量改动。
- 共享可变 + `mutex`/`atomic`、`select` ——不做（与"只传值"模型冲突；`select` 代价大于收益）。
- 全共享全局表 + 锁 ——否决（与 JIT 全局内联缓存冲突）。

---

## 7. 第 1 期实施清单

**第 0 步：消除关键字冲突（纯改名，行为不变）**
1. `examples/tetris.jc2`：`spawn` 标识符改名（2 处）。
2. `tests/core/test_io.jc2`：`yield` 标识符改名（1 处）。
3. `tests/features/test_member_model.jc2`：`yield` 标识符改名（1 处）。
4. 验收：三套测试仍 60/60、60/60、18/18；brainfuck 示例输出不变。

**第 1 步：关键字升格**
5. `src/frontend/Lexer.cpp` 关键字表加入 `spawn` / `yield` / `thread`。
6. 同步更新所有列举关键字的位置：语言文档、模块帮助文本、语法高亮/编辑器配置（实现时用 `grep -rn` 找全，逐个核对）。

**第 2 步：语法与解析**
7. `src/frontend/`（Parser/AST）：`spawn [name] call`、`thread [name] call`、`yield [expr]` 三种语句/表达式节点；`yield expr` 出现在表达式位置时其值可被使用（双向）。
8. `src/compiler/`：三条新指令/调用路径，能编译进字节码；`--jit` 下先**拒绝**（明确报"暂不支持"）而不是静默降级。

**第 3 步：运行时最小切片**
9. 执行体上下文（每协程一套）、`spawn`/`yield`/`thread` 跑通。
10. `h.resume(v)` / `h.join(timeout)` / `h.cancel()` / `h.isAlive()` 四个方法。
11. 取消：让出点检查 + `CancelledError` + `defer` 按 LIFO 执行。
12. `sleep(ms)`：协程内让出、到期恢复。

**第 4 步：测试计划**（全部静默）
13. 正例：`spawn` 返回值、命名 `spawn name f()`、`yield`/`yield expr`/`v = yield x` 双向、FIFO 顺序、`join` 取值、`join` 重抛、`cancel` 后 `defer` 顺序、`yield` 在 `map` 回调里合法、`yield` 在 `defer`/`catch` 块里合法。
14. 负例：已结束的句柄再 `resume` 报错、`join` 超时、跨线程传句柄/闭包/原生句柄 → `ThreadError`、类实例含不可传成员 → `ThreadError`。
15. `--jit` 下对三个关键字明确报"暂不支持"。

**第 5 步：不做（本期边界）**
16. 不做：线程安全点与 GC 协调、抢占式调度、网络非阻塞化与事件循环（第 2 期）、结构化并发块、`channel`、M:N 迁移。

---

## 8. 待定（只有确实需要作者再拍板的）

1. **中断（Ctrl+C / 三次强制退出）与执行体的关系**：中断到达时正在跑的协程/线程是否走取消路径、`defer` 是否执行。这是交互体验决定，且现有 README 已有"三次强制退出"的约定。
2. **结构化并发块**要不要进第 3 期（"作用域结束自动 join 子执行体"），以及它的语法形状。
3. **线程内未捕获异常在 REPL 与脚本下是否要有不同默认行为**（脚本已定：不打印、`join` 重抛）。
4. **`window` 模块的 UI 线程**是否升格为语言可见的守护线程。
5. **原生句柄的线程亲和**是否要可显式转移（第 1 期一律不可传）。

---

## 9. 待实现期确认的事实

- `sys.sleep` 的确切实现位置与调用点（本稿只依赖"真阻塞"这一行为）。
- 关键字被引用的全部位置（文档/帮助/高亮）——第 1 步用 `grep -rn` 找全。
- `channel` 的 `recv` 在 `close()` 之后的行为：**已定**为抛 `ClosedChannelError`（`none` 与"有人 send 了 none"不可区分），落在第 2 期实现。
