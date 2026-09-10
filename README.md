<div align="right">
  <strong>English</strong> | <a href="README_zh-CN.md">简体中文</a>
</div>

# Junk Calculator 2.6.3.0

![Version](https://img.shields.io/badge/Version-v2.6.3.0-orange.svg?style=flat-square)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B)
![Zero Dependencies](https://img.shields.io/badge/Dependencies-0-brightgreen.svg?style=flat-square)
![CMake](https://img.shields.io/badge/CMake-3.15+-064F8C.svg?style=flat-square&logo=cmake)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)

A scripting language and computer algebra system (CAS) implemented in C++20. It relies on a custom bytecode compiler and a register-based virtual machine, requiring no third-party dependencies.

Developed by Yu Liangyang, Tsinghua University.

---

## Technical Overview

### Architecture
- **Lexer**: Tokenizer supporting over 55 token types, including string interpolation (`f""`), raw strings with custom delimiters (`r"TAG()TAG"`), alternating single/double quotes, imaginary suffixes (`3i`), and variadic ellipsis (`...`).
- **Parser**: Recursive descent parser producing an AST (Abstract Syntax Tree) with over 30 node types. Supports operator precedence, block statements, comma sequence evaluation, and destructuring.
- **Compiler**: Features a Sea of Nodes Intermediate Representation (IR) and Static Single Assignment (SSA) form. Includes a multi-pass optimization pipeline (CSE, DCE, Constant Folding) and utilizes Graph Coloring for register allocation.
- **Just-In-Time Compiler**: A type-specializing JIT with HIR lifting, LIR instruction selection, linear-scan register allocation, x86-64 code generation, on-stack replacement (OSR), and stack-map based deoptimization.
- **Virtual Machine**: Register-based bytecode interpreter. Implements inline caching, low-level fast paths, tail call optimization (TCO), exception handling with line-number unwinding, an interactive step-debugger, execution profiling, and dynamic operator dispatching.

### Language Semantics
- **Identifiers**: Full UTF-8 identifier support, plus backtick-quoted identifiers (`` `text` ``) that wrap keywords, spaces, or punctuation as a name (`` `if` = 1 ``, `` obj.`key name` ``).
- **Type System & Memory Management**: NaN-boxing backed dynamic typing supporting 20+ internal types (including hidden types). 
  - *Value Types*: Scalars (double, BigInt, Complex) and Matrices (Real, Complex) use contiguous memory and pass-by-value semantics.
  - *Reference Types*: Containers (`List`, `Dict`, `Set`) and OOP `Instance`s use pass-by-reference semantics (backed by PIMPL architecture and `std::shared_ptr`).
- **Gradual Typing**: Optional runtime type contracts for function parameters and return values (e.g., `func(a: double, b: matrix) -> bool = ...`). Supports base types, containers, and class inheritance definitions.
- **Garbage Collection (GC)**: Mark-and-Sweep Garbage Collector (`GcHeap`) executing on top of the VM stack. Traces GC roots (Globals, Stack, Upvalues, and Contexts) to resolve cyclic references.
- **Object-Oriented Programming**: Single inheritance (`extends`), `super` dispatching, and operator overloading via dunder methods (e.g., `__add__`). Instances support destructuring assignment.
- **Control Flow & Pattern Matching**: `if/else`, `while`, `for`, `for-in`, `switch/case`, `match` (with deep destructuring and dependent binding), `break/continue/return`, and `defer` for resource cleanup.
- **Error Handling**: `try/catch/throw` constructs with structured `Exception` objects, stack tracebacks, and match-style catch chains (`catch { pattern => body, ... }`) with soft type matching and guards.
- **Metaprogramming**: AST-based compile-time macro system (`macro`) with code quoting (`quote`), unquoting (`$`), and hygienic macros (`gensym`) for code generation.
- **Execution Control**: Robust `Ctrl+C` interrupt mechanism to safely halt infinite loops or heavy CAS computations without crashing the VM. Pressing `Ctrl+C` three times consecutively triggers an immediate hard exit.
- **Functions**: Closures, lambdas `(x) => expr`, default parameters, keyword arguments (`f(a=1, b=2)`), keyword-only params (`f(a; b=0)`), kwargs collection (`f(; ...kw)`), variadic arguments (`...args`), argument unpacking (`f(...args)`), and `ref` parameter binding.
- **Generic Container API**: Array manipulation methods (`push`, `slice`, `map`, `filter`, `reduce`, `sort`, `join`, `zip`, etc.) are attached to the prototypes of `List`, `Matrix`, `String`, `Dict`, and `Set`, enabling UFCS-style pipelines (`data |> .sort() |> .unique()`). Heterogeneous literals use `@[...]`.
- **Set Algebra**: `Set` type (with `@{...}` literal syntax) providing O(1) membership testing (`in`). Supports operators for union (`|`), intersection (`&`), difference (`-`), and Cartesian product (`*`). Includes powerset generation (`powerSet`) and relation predicates.

### Mathematics & CAS Engine
- **Computer Algebra System (CAS)**: A symbolic mathematics engine operating on Directed Acyclic Graphs (DAG). Features simplification (`simplify`, `expand`, `contract`, `factor`, `trigsimp`), symbolic calculus (`diff`, `integ`, `limit`, `taylor`), and exact analytic root finding (`solveEq`).
- **Polynomial Algebra**: Uses Subresultant Pseudo-Remainder Sequences for polynomial GCD, and Finite Field $\mathbb{Z}_p$ mapping (Cantor-Zassenhaus algorithm) for multivariate factorization. 
- **Integration Engine**: Implements subsets of the Risch algorithm, including Hermite reduction, the Rothstein-Trager algorithm, and Liouvillian differential field extensions.
- **Arbitrary-Precision**: Base-2³² limb `BigInt` layout. Implements high-base division, GCD/LCM, and modular exponentiation.
- **Exact Rationals & Promotion**: `Fraction` types recursively cross-reduce. Exact rational powers (e.g., `(1/2)^(1/2)`) that cannot be resolved numerically auto-promote into `SymExpr` CAS trees to prevent floating-point precision loss.
- **Linear Algebra**: `Matrix<T>` template supporting Gaussian-Jordan elimination, QR decomposition (Modified Gram-Schmidt), LU decomposition (Doolittle partial pivoting), and Eigenvalues (Hessenberg + Givens QR iteration).

### Native Modules & Standard Library
Native C++ extensions exposed to the execution context:
- `image`: OOP-based BMP generation, drawing primitives with SDF (Signed Distance Field) sub-pixel anti-aliasing, and ASCII font rendering.
- `io`: File stream class with zero-copy binary I/O, an RFC 4180 CSV engine, and filesystem operations with UTF-8 path handling.
- `prob`: OOP-based statistical distributions (PDF, CDF, Quantile via Newton iteration) and hypothesis tests.
- `json`: JSON serialization and deserialization.
- `socket`: Low-level TCP/IP networking stack (WinSock2/POSIX bindings).
- `bytes`: Native memory buffer with Hex/Base64 encode/decode, zero-copy view/slice, and chained typed read/write methods.
- `window`: Native GUI window rendering engine. Supports Mouse-Look pointer capturing and independent IME toggling (Win32).
- `latex`: Bi-directional LaTeX engine. Serializes JC2 objects to LaTeX, and parses raw LaTeX formulas into executable closures.
- `ffi`: Zero-dependency Foreign Function Interface (cross-platform: Windows/Linux/macOS). Supports dynamic loading of shared libraries, direct C ABI invocation, zero-copy multi-dimensional array views, and nested struct support.
- `regex`: High-performance native regular expression engine (bytecode VM with full-state memoization).
- `tensor`: N-dimensional tensor engine with autograd.
- `decimal`: Arbitrary-precision decimal arithmetic.

JC2 standard libraries loaded via `import`:
- `collections`: Data structures including `Stack`, `Queue`, `Deque`, `PriorityQueue` (Heap), and Search Trees.
- `discrete`: Discrete mathematics toolkit covering combinatorics, binary relations, and graph traversal.
- `engine`: Game framework abstraction over the `window` module for render loops and event state management.
- `net`: OOP wrapper for TCP streams (`TcpSocket` and `TcpServer`).
- `http`: HTTP/1.1 client supporting URL parsing and GET/POST requests.

---

## What's New in v2.6.3.0

### Try/Catch Reworked — Catch Chains
- **Catch reuses `match` syntax**: `catch { pattern => body, ... }` supports multiple branches, tried in order until one matches.
- **Soft type matching**: a type annotation in a catch pattern (`catch { e: TypeError => ... }`) skips the branch and re-throws if the error's type doesn't match.
- **Or-patterns & guards**: `catch { e: A, e: B => ... }` matches either type; `catch { e if (cond) => ... }` adds a guard.
- **Shorthand**: `catch(pattern) body` is sugar for `catch { pattern => body }`, and supports or-patterns (comma) but not guards.

### Typed Errors
- **Explicit exception types**: `Jc2Error` + `JC2_THROW(TypeError, ...)` replace string-prefix encoding; native extensions throw typed errors via `throw_error_typed` (`JC2_EXT_VERSION` → 6).
- **Exception subclasses keep their type**: `throw MyError("x")` is caught by `catch { e: MyError => ... }` thanks to pointer-based `isExceptionInstance`.
- **Richer messages**: index/out-of-range/dimension/not-found errors include the offending values.

### `repr()` & str/repr Separation
- **`repr(x)`**: returns the full, round-trippable string — containers fully expanded, strings quoted, matrices in 1D `[1,2;3,4]` form.
- **`str(x)` / `print`**: readable form — large containers truncated, matrices in 2D layout.
- **Print truncation**: large `List`/`Dict`/`Set` print the first 50 elements + `...`; matrices/SymMatrix truncate to 10×10; tensors truncate each dimension to 10. `repr()` still returns the full content.
- **`match` raises `MatchError`** when no branch matches, instead of silently returning `none`.

### LSP & IDE Integration
- **Built-in LSP server**: JC2 now ships a full language server (`jc2 lsp`) with hover, signature help, completion, and semantic tokens.
- **Semantic tokens**: token-level highlighting driven by `BuiltinIndex` + `NameResolver` + `TypeChecker`, with `semanticTokenScopes` for precise scoping.
- **Type inference**: a context-aware engine infers variable/expression types, powers hover and completion, and resolves DLL module return types via sidecar JSON.
- **VS Code extension**: the plugin is upgraded to an industrial-grade LSP client (bundled with esbuild).

### Formatter
- **AST-based formatter**: a `jc2 fmt` command reformats source with 1TBS style, smart line wrapping, unary spacing, and comment/trivia preservation.
- **Token-stream layout engine**: rewritten as a token-stream layout engine preserving literals, comments, directives/shebangs, and return spacing.

### Language & Parser
- **List vs matrix patterns**: `@[...]` destructures lists, `[...]` destructures matrices — no more ambiguity.
- **Destructured parameters**: list-pattern parameters with defaults (`f(@[a, b = 1]) = ...`).
- **Bare `try`**: `try { ... }` without `catch` swallows errors and yields `none`.
- **Format specs**: Python-aligned format specs with `::` separator (`f"{x::.2f}"`).

### Matrix
- **Column-major storage**: the matrix model is unified on a column-major 1D view; `toList`/`toMatrix` drop the vector-flatten special case; `getItem`/`setItem` are removed.

### Mathematics & Performance
- **BigInt**: FFT multiplication, Newton-Raphson inverse/sqrt, divide-and-conquer conversion, and shift operators.
- **π & e**: Gauss-Legendre pi replaced by Chudnovsky + Binary Splitting; multi-threaded `e` computation.
- **Decimal**: Base-10⁹ `DecInt` limb layout, AGM-accelerated `ln`/`exp`, a global high-precision constant pool, and small-size optimization.

### Workspace & CLI
- **Subcommand CLI**: `jc2 run/eval/repl/fmt/lsp/compile/...` replaces flag soup.
- **Workspace snapshots**: `.jcw` workspace persistence as a binary memory snapshot, with incremental merge and introspection.
- **Compression**: a zero-dependency DEFLATE compressor compresses `.jcw`/`.jcb` archives.
- **REPL**: `/run` shortcut, consolidated `/set` cluster, and `/gc` `/gcinfo`.

### JIT
- **Full opcode coverage**: `BytecodeToHIR` now covers all 112 opcodes, with fixes for GCM phi back-edges, merge phi ordering, and type-converter routing.

### Fixes
- `_: int` (discard with type hint) and `..._: int` now enforce the type check instead of silently skipping it.
- Catch shorthand supports or-patterns (`catch(p1, p2) body`).
- Formatter preserves catch braces and branch commas.
- `switch` case values parse as expressions, not type annotations.

---

## Building

Requires a C++20 compliant compiler and CMake 3.15+.

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

*Note: On MSVC, the CMake script uses `/MT` static linkage and Link Time Code Generation (`/GL`, `/LTCG`).*

---

## Command-Line Interface

    jc2                                # Interactive REPL (default)
    jc2 load [name/path]               # Load a workspace and enter REPL
    jc2 [path].jcw                     # Auto-fallback to load workspace
    jc2 script.jc2                     # Run a script (fallback to 'run')
    jc2 run script.jc2                 # Run a script explicitly
    jc2 compile in.jc2 [out.jcb]       # Compile script to bytecode
    jc2 compile in.jc2 -m              # Compile as a module (for 'import')
    jc2 compile in.jc2 -s              # Compile and strip debug info
    jc2 fmt [path]                     # Format a script or directory of scripts
    jc2 fmt --check [path]             # Check if scripts are formatted (returns 1 if not)
    jc2 test [dir]                     # Run test scripts in directory
    jc2 eval "expr"                    # Evaluate expression and exit
    jc2 help [topic]                   # Show help overview or specific topic
    jc2 version                        # Show current version
    jc2 lsp                            # Start the LSP server (stdio)

Global flags (appendable to any command):

    -d, --ir, --hir, --mc              # Show disassembly / IR / Machine Code
    --debug, --profile, --jit          # Enable debugger / profiler / JIT
    -q, --quiet                        # Quiet mode (no banner/prompt)

*Script Path Context: The `run` and `import` instructions push the executing script's directory onto a paths stack, resolving relative I/O based on the current file's location rather than the terminal's working directory.*

---

## Project Layout

    +-- src/
    |   +-- main.cpp                Entry point, CLI parser, and Workspace I/O
    |   +-- resource.rc             Windows resource file (Icon and Version info)
    |   +-- frontend/               Frontend syntax components (Lexer, Parser, AST, Highlight)
    |   +-- compiler/               Compiler backend (IRBuilder, Optimizer, Emitter, Resolver)
    |   +-- jit/                    Just-In-Time compiler (HIR/LIR, RegAlloc, CodeGen, OSR)
    |   +-- vm/                     Virtual Machine core (VM, Bytecode, Builtins, Interrupts)
    |   +-- memory/                 Memory & Type System (Value variant, GcHeap)
    |   +-- math/                   Math primitives (BigInt, Fraction, Complex, Matrix, Base)
    |   +-- cas/                    Computer Algebra System (Symbolic, Integration, Factorization)
    |   +-- lib/                    Native C++ extensions & C ABI (Image, JSON, FFI, Regex, ...)
    |   +-- utils/                  Shared utilities (lsp, fmt, json, deflate)
    +-- modules/                    Standard JC2 libraries
    +-- docs/                       Design documentation (JIT, VM, Extension API, ...)
    +-- data/                       Bundled data (help documentation, icon, prime table)
    +-- examples/                   Showcase scripts
    +-- tests/                      Automated test script suite
    +-- jc2-language/               VS Code Language Support Extension

---

## License

MIT
