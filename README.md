<div align="right">
  <strong>English</strong> | <a href="README_zh-CN.md">简体中文</a>
</div>

# Junk Calculator 2.6.1.0

![Version](https://img.shields.io/badge/Version-v2.6.1.0-orange.svg?style=flat-square)
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
- **Arbitrary-Precision**: Base-2³² limb `BigInt` layout. Implements high-base division, GCD/LCM, and modular exponentiation.
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

## What's New in v2.6.1.0

### Type System
- **`any` and `never`**: The top type `any_type` is shortened to `any`, and a new bottom type `never` (the empty type holding no values) completes the type lattice — `any` accepts everything, `never` accepts nothing. The empty type now renders as lowercase `never`.
- **Class Promotion in `<:`**: A class on the right of the subset operator auto-promotes to its typedef, so `A <: MyClass` works directly without an explicit type conversion.
- **TypeDef Converters**: Callable types (`int`, `float`, `string`, `matrix`, `dict`, `list`, `set`, `complex`, `bool`, `fraction`) now carry their converter on the typedef itself, eliminating the string-based name-lookup hack across six call sites.
- **`matrix()` Restoration**: Dynamic matrix construction works again via a union-type converter whose converter check runs before the single-element path, with automatic symbolic detection (`hasSymbolic` → `asSymbolic`).
- **`__subsets__`**: The `<:` subset operator now dispatches through a `__subsets__` dunder, documented alongside the other protocols.

### Dict Higher-Order Functions
- **`map` / `filter` / `reduce` / `any` / `all` / `countIf`**: `Dict` gains the full higher-order suite. Callbacks receive an `@[k, v]` pair (a frozen list), matching `for ([k, v] in d)` and `entries()`.
- **Full-mapping `map`**: `d.map(f)` lets `f` return `@[new_k, new_v]`, producing `{new_k: new_v}` — both keys and values are remappable, like Python's `dict(map(...))` or Rust's `map().collect()`.
- **Single-side shorthands**: `mapKeys(f)` and `mapValues(f)` map only one side, mirroring Ruby's `transform_keys` / `transform_values`.
- **GC safety**: Pair arguments and result dicts are pinned with `GcValueGuard`/`GcObjGuard`, since containers survive by marking, not reference counts.

### Semantics
- **`as` as a postfix assertion**: `as` now sits between `call` and `power`, applying `call()` to both operands, and chains left-associatively (`a as int as double` == `(a as int) as double`).
- **Or-pattern consistency**: or-patterns binding different variable names are rejected at compile time, while same-named bindings across alternatives merge through phi nodes, and a guard applies to the whole or-pattern.

### Fixes
- Matrix 1D index assignment (`A[1] = [1, 2]`) no longer forces `asDouble` on the value.
- REPL no longer continues on slash-prefixed commands (`/help`, `//`), and keeps `/*` multiline comments open.
- An empty program evaluates to `none`, not `0`.
- BigInt layout documented as base 2³² limbs.

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
