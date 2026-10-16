<div align="right">
  <strong>English</strong> | <a href="README_zh-CN.md">简体中文</a>
</div>

# Junk Calculator 2.6.4.0

![Version](https://img.shields.io/badge/Version-v2.6.4.0-orange.svg?style=flat-square)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B)
![Zero Dependencies](https://img.shields.io/badge/Dependencies-0-brightgreen.svg?style=flat-square)
![CMake](https://img.shields.io/badge/CMake-3.17+-064F8C.svg?style=flat-square&logo=cmake)
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

## What's New in v2.6.4.0

### Language
- **Initializer-less members**: a class member may be declared without a value (`const id`, `local balance`, `name`). It must then be assigned in `init`; otherwise construction throws `member 'x' was not initialized in init()`. A `const` member accepts exactly one such assignment.
- **Traits and interfaces**: `trait` / `with` composition, abstract methods, frozen constraints, and the `istrait(x)` predicate.
- **Built-in error types are classes**: `TypeError`, `ValueError` and friends can be caught by type and subclassed like user classes.
- **Iteration ends with `StopIteration`** instead of returning `none`.
- **Renames (breaking)**: `double` → `float` (with `sqrtF` / `cbrtF` / `rootF`); the `is*` predicates are all lowercase; `pi` / `e` / `i` live in `math`; the `switch` expression is replaced by `match`.
- **CAS**: broader factoring and solving (perfect powers, multivariate forms, `RootOf` for degree ≥ 4) and branch-safe rewrite guards.

### Standard Library & Networking
- **`http` module**: `httpGet` plus request helpers, chunked bodies, redirects, and `https` (TLS 1.3 **and** TLS 1.2).
- **`tls.connect` / `TcpStream`**: raw TCP and TLS sockets for scripts.
- **`crypto` module**: SHA-256/384/512, AES-GCM, X25519, P-256/384/521 and X.509 chain verification.
- **`gzip` / `zlib` decoding** for compressed bodies.

### Fixes & Performance
- **Catch handlers keep live values**: a parameter read inside `catch` no longer comes back as `none` after a call in the `try` body.
- **`regex` is linear**: the dedup table is reused and a literal-prefix prefilter skips non-matching regions (a real 160 KB page: ~86 s → 0.013 s).
- **`Set` aggregates and predicates**: `sum`, `prod`, `max`, `min`, `span`, `any`, `all` now work on sets.
- **Initializer-less private fields are per-instance**, no longer shared between instances.
- **GC fixes** for instances, class member tables and closure copies.

### Tooling
- **Semantic highlighting**: token spans now come from the source text (multi-line strings and comments no longer bleed into the next line) and f-string `{...}` interpolations are coloured as expressions.
- **VS Code extension 6.4.0**, bundled help re-synced.
- **Test layout**: suites now live under `test/`; `jc2 test test/tests` ships in the package, while `test/http_tests` (vector and network tests plus their fixtures) is an on-demand download.

## Building

Requires a C++20 compliant compiler and CMake 3.17+.

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
    +-- test/                       Test scripts (see below)
        +-- tests/                  Always-on suite (shipped in the package)
        +-- jit_tests/              JIT-specific suite (optional)
        +-- http_tests/             Vector/network suite + fixtures (optional download)
    +-- jc2-language/               VS Code Language Support Extension

---

## License

MIT
