<div align="right">
  <strong>English</strong> | <a href="README_zh-CN.md">简体中文</a>
</div>

# Junk Calculator 2.6.0.0

![Version](https://img.shields.io/badge/Version-v2.6.0.0-orange.svg?style=flat-square)
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
- **Functions**: Closures, lambdas `(x) => expr`, default parameters, keyword arguments (`f(a=1, b=2)`), variadic arguments (`...args`), and `ref` parameter binding.
- **Generic Container API**: Array manipulation methods (`push`, `slice`, `map`, `filter`, `reduce`, `sort`, `join`, `zip`, etc.) are attached to the prototypes of `List`, `Matrix`, `String`, `Dict`, and `Set`, enabling UFCS-style pipelines (`data |> .sort() |> .unique()`). Heterogeneous literals use `@[...]`.
- **Set Algebra**: `Set` type (with `@{...}` literal syntax) providing O(1) membership testing (`in`). Supports operators for union (`|`), intersection (`&`), difference (`-`), and Cartesian product (`*`). Includes powerset generation (`powerSet`) and relation predicates.

### Mathematics & CAS Engine
- **Computer Algebra System (CAS)**: A symbolic mathematics engine operating on Directed Acyclic Graphs (DAG). Features simplification (`simplify`, `expand`, `contract`, `factor`, `trigsimp`), symbolic calculus (`diff`, `integ`, `limit`, `taylor`), and exact analytic root finding (`solveEq`).
- **Polynomial Algebra**: Uses Subresultant Pseudo-Remainder Sequences for polynomial GCD, and Finite Field $\mathbb{Z}_p$ mapping (Cantor-Zassenhaus algorithm) for multivariate factorization. 
- **Integration Engine**: Implements subsets of the Risch algorithm, including Hermite reduction, the Rothstein-Trager algorithm, and Liouvillian differential field extensions.
- **Arbitrary-Precision**: Base-10^9 compressed `BigInt` layout. Implements high-base division, GCD/LCM, and modular exponentiation.
- **Exact Rationals & Promotion**: `Fraction` types recursively cross-reduce. Exact rational powers (e.g., `(1/2)^(1/2)`) that cannot be resolved numerically auto-promote into `SymExpr` CAS trees to prevent floating-point precision loss.
- **Linear Algebra**: `Matrix<T>` template supporting Gaussian-Jordan elimination, QR decomposition (Modified Gram-Schmidt), LU decomposition (Doolittle partial pivoting), and Eigenvalues (Hessenberg + Givens QR iteration).

### Native Modules & Standard Library
Native C++ extensions exposed to the execution context:
- `image`: OOP-based BMP generation, drawing primitives with SDF (Signed Distance Field) sub-pixel anti-aliasing, and ASCII font rendering.
- `prob`: OOP-based statistical distributions (PDF, CDF, Quantile via Newton iteration) and hypothesis tests.
- `json`: JSON serialization and deserialization.
- `socket`: Low-level TCP/IP networking stack (WinSock2/POSIX bindings).
- `bytes`: Memory buffering and low-level binary I/O operations.
- `window`: Native GUI window rendering engine. Supports Mouse-Look pointer capturing and independent IME toggling (Win32).
- `latex`: Bi-directional LaTeX engine. Serializes JC2 objects to LaTeX, and parses raw LaTeX formulas into executable closures.
- `ffi`: Zero-dependency Foreign Function Interface (Windows x64 only). Supports dynamic loading of shared libraries (DLL), direct C ABI function invocation, and raw memory/pointer manipulation.
- `regex`: High-performance native regular expression engine (bytecode VM with full-state memoization).
- `tensor`: N-dimensional tensor engine with autograd.
- `decimal`: Arbitrary-precision decimal arithmetic.

JC2 standard libraries loaded via `import`:
- `collections`: Data structures including `Stack`, `Queue`, `Deque`, `PriorityQueue` (Heap), and Search Trees.
- `discrete`: Discrete mathematics toolkit covering combinatorics, binary relations, and graph traversal.
- `engine`: Game framework abstraction over the `window` module for render loops and event state management.
- `net`: OOP wrapper for TCP streams (`TcpSocket` and `TcpServer`).
- `http`: HTTP/1.1 client supporting URL parsing and GET/POST requests.
- `buffer`: Binary manipulation API with cursor support.

---

## What's New in v2.6.0.0

### JIT Compiler (New)
- **Just-In-Time Compiler**: A complete JIT pipeline built from scratch: executable memory allocation, a macro assembler, bytecode-to-HIR lifting with type specialization, LIR instruction selection, graph-coloring register allocation, and x86-64 machine code generation.
- **Adaptive Optimization**: Runtime type profiling feeds type guards, inline caches, function inlining, and callout fallbacks for megamorphic call sites. Integer arithmetic emits overflow checks with deoptimization.
- **On-Stack Replacement (OSR)**: Hot loops compile in place and swap execution through OSR entries, with stack-map based deoptimization for precise state recovery.
- **Tooling**: A built-in x86-64 disassembler (`--mc`), HIR graph printing (`--hir`), and the `--jit` command-line flag (off by default).

### Language Core
- **Keyword Arguments**: Named parameters callable as `f(b=5, a=3)`. Native functions expose parameter names, and `...rest` parameters only collect excess positional arguments.
- **Prototype Chain**: Container methods migrated onto `List`, `Matrix`, `String`, `Dict`, and `Set` prototypes, enabling UFCS-style pipelines like `data |> .sort() |> .unique()`.
- **Built-in Namespaces**: `sys`, `io`, `cas`, `math`, and `random` injected as native namespaces, replacing scattered global functions.
- **First-Class Slices**: `slice(start, end, step)` objects with multi-dimensional indexing (`A[i, j]`) and slicing (`A[1:3, 2:4]`), where `__getitem__` receives slice objects.
- **Lifecycle & Matching Protocols**: `finalize()` destructor hook (replacing `__del__`), and the `__match__()` protocol for custom pattern-matching views.
- **Recursive Macros**: Macros may now call themselves inside `quote` blocks for AST traversal and transformation.
- **Type Assertions**: Single-shot runtime assertions on assignment, destructuring, and standalone declarations (`x: int = 10`, `[a: int, b: string] = data`, `local x: int`).
- **Compiler Directives & Shebang**: `#` introduces line-level compiler/VM directives; `#!/usr/bin/env jc2` shebangs are recognized via a no-op `!` directive.
- **Integer Division**: New `~/` operator (truncating toward zero) with `__idiv__` overload.
- **New Builtins**: `enumerate` and `groupBy` on containers.

### CAS Engine
- **Polynomial Core Rewrite**: Sparse polynomial engine (`SparsePoly`) and multivariate engine (`MultiPoly`) with 64-bit structured hashes, an arena allocator replacing `shared_ptr`, and targeted GCD/factorReal optimizations.
- **Advanced Algorithms**: Buchberger's LCM criterion, Subresultant PRS, and a native 64-bit Cantor-Zassenhaus factorization fast path.
- **Symbolic Matrices (`SymMatrix`)**: Exact symbolic linear algebra including LU, QR, diagonalization, charPoly, eigenvalues/eigenvectors, matrix exponential, Jacobian, Hessian, Kronecker products, nullspace, rank, norm, and element-wise integration.

### API Modernization
- **Deprecated APIs Removed**: `isXXX` type predicates and `pcall` removed in favor of `isinstance`, first-class type objects, and `try/catch`.
- **Unified Deep Copy**: `copy(obj, freeze)` with a three-state freeze flag (`none`/`true`/`false`), replacing `clone`/`val`.
- **`BaseNum` Demotion**: `basenum` is now a `math.BaseNum` class supporting radix-based shifts.
- **Strict Matrix Literals**: Heterogeneous literals require `@[...]`; plain `[...]` constructs homogeneous matrices. The `StringMatrix` type was removed.
- **Type Interning**: Type objects are globally interned, making `type(5) is int` an O(1) pointer comparison.

### Memory & GC
- **Finalizers**: `finalize()` is invoked by the GC with exactly-once semantics.
- **Use-After-Free Fixes**: Resolved GC use-after-free defects surfaced by long-running scripts (zombie scenarios).

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
