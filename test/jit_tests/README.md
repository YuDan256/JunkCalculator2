# JIT Test Suite (Junk Calculator 2 / JC2)

Black-box test suite for the Just-In-Time (JIT) compiler introduced in
Junk Calculator 2.6.0.0. It exercises the JIT through the public CLI and REPL
only — no source changes are required.

## Requirements

- Windows x64 build of `JunkCalculator2.exe` (the JIT is x86-64 only).
- PowerShell 7+ for the runner. If the script is launched from Windows
  PowerShell 5.1 it automatically re-invokes itself with `pwsh` when available.
- ~5–10 minutes of wall time for a full run.

## What it covers

- **Correctness oracle** — runs every existing test script in the repository's
  `tests/` directory (except `test_window.jc2`, which opens a real GUI window)
  under both the interpreter and `--jit`, and requires identical stdout and
  exit codes.
- **Benchmarks** — 10 scenarios (int32/float hotspots, big-integer
  accumulation, monomorphic/polymorphic calls, closures, string/dict, matrix,
  nested loops, `for-in range`). Each runs 3 times per mode and reports the
  median speedup.
- **Edge cases** — mid-loop type switching, int64 overflow, exceptions inside
  hot loops, recursion, closure state, complex arithmetic, f-string
  interpolation.
- **Toolchain & introspection** — `--hir`/`--mc`/`--profile` output, REPL
  `/jit|/hir|/mc` toggles, `-e` vs `--eval`, and the `__dbg_is_jitted` /
  `__dbg_type_feedback` debug functions.

## Usage

Packaged layout (binary sits next to `bin/` in the project root):

```powershell
.\jit-tests\run_jit_tests.ps1
```

Source repository layout (after building, e.g. `cmake -B build
-DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release`):

```powershell
.\tests\jit-tests\run_jit_tests.ps1 `
  -ExePath .\build\Release\JunkCalculator2.exe `
  -TestsDir .\tests `
  -ModulesDir .\modules
```

Results are written to `jit-tests/results/` (CSV summaries plus raw samples:
HIR graph, machine-code disassembly, profiler output, REPL transcript, debug
probes). `JIT_TEST_REPORT.md` in this directory is a full Chinese-language
report of a reference run.

## Acceptance criteria used by this suite

- Correctness: JIT output must be byte-identical to the interpreter for every
  automated scenario.
- Performance (soft): a pure int32 `while` hotspot should show ≥3x speedup;
  no benchmark should be more than 2x slower under `--jit`.
- Stability: every edge scenario must complete without crashing, hanging, or
  leaking an uncaught VM error; deoptimization/fallback must be correct.

## Known observations

- JIT acceleration is large (16–25x in the reference run) only for pure scalar
  `while` loops. Big-integer accumulation, function calls, nested loops and
  `for-in range` typically fall back to the interpreter and run at
  interpreter speed (0.94–1.23x).
- With `--jit --profile`, the runtime prints the full OSR lifecycle:
  `OSR Compilation successful` → `Executing OSR machine code` → `OSR
  Deoptimized! Falling back to interpreter`.
- `__dbg_is_jitted(f)` returned `false` in every shape tested (including hot
  loops), so it does not reflect OSR state; `__dbg_type_feedback(f)` returns a
  type-profile array that does change with call patterns.
- GUI-based tests (`test_window.jc2` and the `engine` examples) are excluded
  from automated assertions.
