<div align="right">
  <strong>English</strong> | <a href="README_zh-CN.md">简体中文</a>
</div>

# Junk Calculator 2.6.2.0

![Version](https://img.shields.io/badge/Version-v2.6.2.0-orange.svg?style=flat-square)
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
- **Error Handling**: `try/catch/throw` constructs with structured `Exception` objects and stack tracebacks.
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

## What's New in v2.6.2.0

### Functions & Argument Unpacking
- **Keyword-only parameters**: A `;` splits the parameter list — everything after it is keyword-only (`f(a; b, c=0)`), and `...kw` after the `;` collects leftover keyword arguments into a dict (`f(a, ...rest; b, ...kw)`). Parameter metadata is split into four independent fields (`paramNames`/`restName`/`kwargNames`/`kwargsName`), replacing the old `...`-prefix hack.
- **Argument unpacking (spread)**: `...expr` expands inline — `f(...args)` spreads a list/set/matrix/string positionally, `f(1; ...opts)` spreads a dict into keyword arguments. Spread may appear anywhere and multiple times.
- **Literal unpacking**: `@[...a]`, `@{...s}`, `{...d}` unpack into list/set/dict literals. Dict spread merges left-to-right with later keys winning; explicit keys always take precedence.
- **`__unpack__` / `__mapping__`**: custom types opt into unpacking by returning a list (positional) or a dict (keyword) from these dunders; `apply` now uses `__unpack__` instead of `__iter__` to avoid infinite iterators.
- **Unified native call convention**: `rest` always arrives as a list in native functions regardless of how the call is written, eliminating the old expand-vs-collect split.
- **Richer signatures**: `toString` now shows `const`/`ref` modifiers and builtin keyword defaults — `print` renders as `<function print(...args; sep = " ", end = "\n")>`.

### `print` / `println` Merge
- **`print(...args; sep = " ", end = "\n")`**: `print` gains Python-style keyword-only `sep`/`end` and defaults to a trailing newline. `println` is removed; `print("no newline", end = "")` covers the old no-newline behavior. This is the first builtin to exercise keyword-only defaults.

### Matrix
- **Immutable matrices**: matrices no longer copy-on-write; hashes are cached because values never change.
- **Zero-copy views**: slicing, `trans`, `getRow`, `getCol` return stride-based views instead of copying.
- **2D slicing**: `getItem`/`getSlice` and an upgraded `setSlice(sr, sc, val)` support row/column ranges.

### Tensor
- **Performance**: template-based dtype dispatch, contiguous fast paths, cache-blocked matmul, a Strassen fast path for batched matmul, and a zero-overhead `TensorImpl` handle architecture.
- **Autograd**: topological-sort backward pass with a true `no_grad` context.
- **Broadcasting & reductions**: broadcast stride iteration, 1D/batched `matmul`, `sum`/`mean` axis, `clamp`, `argmax`; `DType::Bool` for strict index dispatch; `keepdim` on reductions; nested-list initialization with shape inference.

### Standard Library
- **`bytes`**: a native `Buffer` replaces the old `buffer.jc2` script — Hex/Base64 encode/decode, zero-copy view/slice, and chained typed read/write methods.
- **`ffi`**: cross-platform (Linux/macOS), zero-copy multi-dimensional array views, nested struct support, and inline-array index assignment.
- **`io`**: a `File` stream class with zero-copy binary I/O, an RFC 4180 CSV engine, filesystem operations, and UTF-8 path handling on Windows.

### CAS
- **Simplification**: same-exponent powers contract on multiplication, and matrix operation simplification / power folding improved.

### Fixes
- Macro call arguments are AST nodes again — `@m(a=1)` parses `a=1` as an `Assign` statement, not a named argument.
- `maxArity` checks use the post-spread positional count, so `f(1, 2, ...@[], 3, 4)` works.
- Rest/kwargs must be the last parameter; `;` is allowed at the start of a parameter list (all keyword-only).

---

## Building

Requires a C++20 compliant compiler and CMake 3.15+.

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

*Note: On MSVC, the CMake script uses `/MT` static linkage and Link Time Code Generation (`/GL`, `/LTCG`).*

---

## Command-Line Interface

    JunkCalculator2                    # Interactive REPL session
    JunkCalculator2 script.jc2         # Execute a script
    JunkCalculator2 --run script.jc2   # Execute a script (explicit flag)
    JunkCalculator2 script.jc2 -d      # Execute and print bytecode disassembly
    JunkCalculator2 script.jc2 --ir    # Execute and print IR graph
    JunkCalculator2 script.jc2 --hir   # Execute and print HIR graph
    JunkCalculator2 script.jc2 --mc    # Execute and print machine code disassembly
    JunkCalculator2 script.jc2 --jit   # Execute with JIT compilation
    JunkCalculator2 script.jc2 --debug # Execute with interactive step-debugger
    JunkCalculator2 script.jc2 --profile # Execute and print performance report

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
    +-- modules/                    Standard JC2 libraries
    +-- docs/                       Design documentation (JIT, VM, Extension API, ...)
    +-- data/                       Bundled data (help documentation, icon, prime table)
    +-- examples/                   Showcase scripts
    +-- tests/                      Automated test script suite
    +-- jc2-language/               VS Code Language Support Extension

---

## License

MIT
