# Junk Calculator 2.6.0.0 — JIT 特性全面测试报告

## 1. 环境与方法

| 项目 | 内容 |
|---|---|
| 被测程序 | [JunkCalculator2.exe](../bin/JunkCalculator2.exe)，v2.6.0.0，Windows x64 |
| 程序元数据 | Yu Liangyang / MIT / "Code is the ultimate shelter. Open and Free." |
| 测试方式 | 纯黑盒：CLI、REPL、脚本文件；不构建源码 |
| 运行环境 | Windows PowerShell 7.6.4（运行器自动用 pwsh 重跑），测试在临时目录执行 |
| 测试资产 | `jit-tests/`：10 个基准脚本、7 个边界脚本、1 个 REPL 载荷脚本、[run_jit_tests.ps1](run_jit_tests.ps1) 运行器 |
| 数据产物 | `results/oracle_results.csv`、`bench_results.csv`、`edge_results.csv`、`samples/` |

方法：

- 正确性 oracle：`bin/tests/` 下 33 个非 GUI 测试脚本，分别用解释器与 `--jit` 运行，比较 stdout 与退出码（必须完全一致）。排除 `test_window.jc2`（创建真实窗口）与 4 个 GUI 示例（tetris / life_oop / zombie / sorting_visualizer），仅记录排除原因。
- 性能基准：10 个场景 × (解释器、`--jit`) × 3 次，取中位数。
- 边界场景：类型切换、int64 溢出、循环内异常、递归、闭包状态、复数循环、f-string 循环，各 1 次解释器/JIT 对比。
- 工具链：`--hir`、`--mc`、`--profile`、REPL `/jit|/hir|/mc` 开关、`-e` 与 `--eval` 别名、`__dbg_is_jitted` / `__dbg_type_feedback` 探针。

## 2. 正确性结论

- **oracle：33/33 通过**。全部用例在 `--jit` 下的 stdout 与退出码和解释器完全一致，无崩溃、无死循环、无额外输出。
- **边界场景：7/7 通过**，包括：
  - `edge01` 循环变量 int→double 类型切换；
  - `edge02` int64 溢出（`9223372036854775807 + 10000000`，结果与解释器一致）；
  - `edge03` 循环内每 10 万次抛一次异常（try/catch）；
  - `edge04` 递归热点 fib(26)；
  - `edge05` 闭包 upvalue 状态累计；
  - `edge06` 复数运算循环（JIT 未覆盖路径，正确回退）；
  - `edge07` f-string 插值循环。
- **基准结果一致性：10/10**，JIT 与解释器输出逐字节一致。

## 3. 性能基准（3 次中位数）

| 基准 | 解释器 (ms) | JIT (ms) | 加速比 | 结论 |
|---|---|---|---|---|
| bench01 int32 安全 while 热点 | 4521.6 | 179.5 | **25.19x** | 达标（≥3x） |
| bench02 大整数累加热点 | 2787.3 | 2971.4 | 0.94x | 反优化回退，无加速 |
| bench03 double 热点 | 2579.7 | 153.8 | **16.78x** | 达标（≥3x） |
| bench04 单态函数调用热点 | 1288.9 | 1311.6 | 0.98x | 与解释器相当 |
| bench05 多态分派热点 | 640.6 | 590.1 | 1.09x | 无显著加速 |
| bench06 闭包/upvalue 热点 | 1090.1 | 1006.5 | 1.08x | 无显著加速 |
| bench07 字符串/dict 热点 | 1039.3 | 842.5 | 1.23x | 小幅加速 |
| bench08 矩阵运算循环 | 82.5 | 82.4 | 1.00x | 与解释器相当 |
| bench09 嵌套双循环 | 839.3 | 822.8 | 1.02x | 与解释器相当 |
| bench10 for-range 循环 | 3344.4 | 3519.6 | 0.95x | JIT 略慢，未达 2x 红线 |

**软性验收全部满足**：int32 热点 25.19x ≥ 3x；没有任何基准 JIT 比解释器慢 2 倍以上（最差 0.94x）。

**观察**：JIT 对“纯标量 + 简单 while 循环”的收益巨大（17–25x）；涉及大整数累加、函数调用、嵌套循环、for-range 迭代器协议、字典/字符串等场景时，JIT 未进入快路径或很快反优化回退，加速比约 0.94–1.23x。`--profile` 下可见这些场景均发生 `OSR Compilation successful` 后于 IP 4 反优化回退解释器。

## 4. 工具链与 JIT 内省

| 验证项 | 结果 | 证据 |
|---|---|---|
| `--jit --hir` | 通过：282 个节点；Unoptimized 与 Optimized 两幅 digraph；含 OSREntry、GuardIsInt32、Phi、FrameState | [tool_hir.txt](results/samples/tool_hir.txt) |
| `--jit --mc` | 通过：`OSR Machine Code [fn=f osrIp=2] (Size: 800 bytes)`，合法 x86-64 反汇编 | [tool_mc.txt](results/samples/tool_mc.txt) |
| `--jit --profile` | 通过：输出 `Profiler Results`；同时可观测到 `[JIT] OSR Compilation successful` → `Executing OSR machine code` → `OSR Deoptimized! Falling back to interpreter at IP: 4` | [tool_profile.txt](results/samples/tool_profile.txt) |
| REPL `/jit on/off`、`/hir`、`/mc` | 通过：开关即时生效，热循环触发 OSR HIR/MC 输出 | [tool_repl.txt](results/samples/tool_repl.txt) |
| `-e` 与 `--eval` 别名 | 通过：`--jit` 下输出一致（均为 `7`） | — |
| 纯脚本模式 JIT 日志 | 不开启 `/hir` `/mc` `--profile` 时，stdout/stderr 无任何 `[JIT]` 日志（0 行），输出与解释器完全一致 | [jit_logs.txt](results/samples/jit_logs.txt) |

## 5. 调试探针与观察

- `__dbg_is_jitted(f)`：在无 JIT、热循环（500 万次）、函数内 200 万次热调用等所有形态下均返回 `false`。该函数不反映 OSR 编译状态，推测只反映函数级 Tier-2 编译（本次未观测到触发），或为调试占位，**语义需源码确认**。
- `__dbg_type_feedback(f)`：随场景变化，实测 `@[145, 0]`（单态）、`@[0, 0, 1, 8, 145, 1, 0, 0]`（热循环）、`@[1, 0]`（多态分派后），可作为类型画像观测点。[probes.txt](results/samples/probes.txt)
- 语言语义注记：在 `if/else` 块内给函数变量赋值不会逃逸出块（报 `Target is not callable`）；该行为在解释器与 JIT 下完全一致，**不是 JIT 缺陷**。

## 6. 缺陷与风险清单

| 级别 | 内容 | 复现/证据 |
|---|---|---|
| 严重 | 无（0 正确性缺陷） | oracle 33/33、edge 7/7、bench 10/10 |
| 一般（性能观察项） | 大整数累加、单态调用、嵌套循环、for-range 场景 JIT 无加速（0.91–0.98x） | bench02/04/09/10 |
| 文档缺口 | `__dbg_is_jitted` 语义未在 `/help` 与 README 中说明，实测恒为 false | probes.txt |

## 7. 验收结论

**通过（软性标准）**：正确性 100% 一致；int32 热点加速 25.19x（≥3x）；所有基准均未慢于解释器 2 倍；所有边界场景稳定完成反优化/回退。性能观察项与文档缺口不阻断验收，建议后续版本关注大整数与迭代器协议的 JIT 快路径。

## 8. 复现方式

```powershell
# 完整复跑（PowerShell 7；PS 5.1 会自动调用 pwsh）
& .\jit-tests\run_jit_tests.ps1

# 关键单条命令
.\bin\JunkCalculator2.exe jit-tests\bench01_int32_while.jc2 --jit --mc
.\bin\JunkCalculator2.exe jit-tests\edge01_type_switch.jc2 --jit --profile
.\bin\JunkCalculator2.exe -e "1+2*3" --jit
```
