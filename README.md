<div align="right">
  <strong>English</strong> | <a href="README_zh-CN.md">简体中文</a>
</div>

# Junk Calculator 2.4.4.3

![Version](https://img.shields.io/badge/Version-v2.4.4.3-orange.svg?style=flat-square)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B)
![Zero Dependencies](https://img.shields.io/badge/Dependencies-0-brightgreen.svg?style=flat-square)
![CMake](https://img.shields.io/badge/CMake-3.15+-064F8C.svg?style=flat-square&logo=cmake)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)

A scripting language and computer algebra system (CAS) implemented in C++20. It relies on a custom bytecode compiler and a stack-based virtual machine, requiring no third-party dependencies.

Developed by Yu Liangyang, Tsinghua University.

---

## Technical Overview

### Architecture
- **Lexer**: Tokenizer supporting over 55 token types, including string interpolation (`f""`), raw strings with custom delimiters (`r"TAG()TAG"`), alternating single/double quotes, imaginary suffixes (`3i`), and variadic ellipsis (`...`).
- **Parser**: Recursive descent parser producing an AST (Abstract Syntax Tree) with over 30 node types. Supports operator precedence, block statements, comma sequence evaluation, and destructuring.
- **Compiler**: AST-to-bytecode compiler utilizing the Visitor pattern. Handles lexical scoping, auto-local variable declaration, loop patching, and closure capture.
- **Virtual Machine**: Stack-based bytecode interpreter. Implements inline caching, tail call optimization (TCO), late-binding for function calls, exception handling with line-number unwinding, an interactive step-debugger, execution profiling, and dynamic operator dispatching.

### Language Semantics
- **Type System & Memory Management**: NaN-boxing backed dynamic typing supporting 20+ internal types (including hidden types). 
  - *Value Types*: Scalars (double, BigInt, Complex) and Matrices (Real, Complex, String) use contiguous memory and pass-by-value semantics.
  - *Reference Types*: Containers (`List`, `Dict`, `Set`) and OOP `Instance`s use pass-by-reference semantics (backed by PIMPL architecture and `std::shared_ptr`).
- **Gradual Typing**: Optional runtime type contracts for function parameters and return values (e.g., `func(a: double, b: matrix) -> bool = ...`). Supports base types, containers, and class inheritance definitions.
- **Garbage Collection (GC)**: Mark-and-Sweep Garbage Collector (`GcHeap`) executing on top of the VM stack. Traces GC roots (Globals, Stack, Upvalues, and Contexts) to resolve cyclic references.
- **Object-Oriented Programming**: Single inheritance (`extends`), `super` dispatching, and operator overloading via dunder methods (e.g., `__add__`). Instances support destructuring assignment.
- **Control Flow & Pattern Matching**: `if/else`, `while`, `for`, `for-in`, `switch/case`, `match` (with deep destructuring and dependent binding), `break/continue/return`.
- **Error Handling**: `try/catch/throw` constructs and functional `pcall` with stack tracebacks.
- **Execution Control**: Robust `Ctrl+C` interrupt mechanism to safely halt infinite loops or heavy CAS computations without crashing the VM. Pressing `Ctrl+C` three times consecutively triggers an immediate hard exit.
- **Functions**: Closures, lambdas `(x) => expr`, default parameters, variadic arguments (`...args`), and `ref` parameter binding.
- **Generic Container API**: Array manipulation functions (`push`, `slice`, `map`, `filter`, `reduce`, `sort`, `join`, `zip`, etc.) operate across four container types: `RealMatrix`, `ComplexMatrix`, `StringMatrix`, and `List` (with `@[...]` forced literal syntax).
- **Set Algebra**: `Set` type (with `@{...}` literal syntax) providing O(1) membership testing (`in`). Supports operators for union (`|`), intersection (`&`), difference (`-`), and Cartesian product (`*`). Includes powerset generation (`setPow`) and relation predicates.

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
- `ffi` (experimental): Zero-dependency Foreign Function Interface. Supports dynamic loading of shared libraries (DLL/SO), direct C ABI function invocation, and raw memory/pointer manipulation.

JC2 standard libraries loaded via `import`:
- `collections`: Data structures including `Stack`, `Queue`, `Deque`, `PriorityQueue` (Heap), and Search Trees.
- `regex`: Object-Oriented NFA regex engine with capture groups, alternation, and quantifiers.
- `discrete`: Discrete mathematics toolkit covering combinatorics, binary relations, and graph traversal.
- `engine`: Game framework abstraction over the `window` module for render loops and event state management.
- `net`: OOP wrapper for TCP streams (`TcpSocket` and `TcpServer`).
- `http`: HTTP/1.1 client supporting URL parsing and GET/POST requests.
- `buffer`: Binary manipulation API with cursor support.

---

## What's New in v2.4.4.3

### Language Features & Syntax
- **Pattern Matching & Destructuring Enhancements**:
  - Unified `for-in` and list comprehension bindings with the Pattern engine.
  - Supported pattern destructuring in `catch` bindings.
  - Allowed type annotations and default values on destructuring and rest parameters (`...rest`).
  - Supported destructuring parameters in class methods.
  - Allowed destructuring parameters to accept `Instance` and `Matrix` types.
  - Implemented deep orthogonalization of placeholder penetration and pattern modifier binding, restricting the placeholder `_` to direct function call arguments only.
- **Const Semantics Enforcement**:
  - Supported the `const` modifier on function parameters and destructuring patterns.
  - Const assignment violations now throw compile-time errors instead of generating runtime exceptions.
  - Prevented the deletion of global constants via the `delete` command.
- **Exception Handling**: Allowed arbitrary `Value` types to be thrown and caught in exception handling, no longer restricted to specific error objects.
- **Operators & Literals**:
  - Elevated the precedence of bitwise and set operators above comparison operators.
  - Added full C-style and octal escape sequences in string literals.
  - Booleans `true` and `false` are now treated as exact integers `1` and `0` in arithmetic contexts.
- **UFCS Lexical Fallback**: Implemented a lexical fallback mechanism for Uniform Function Call Syntax (UFCS) method resolution.

### Compiler & Frontend Optimizations
- **Dead Code Elimination & Constant Propagation**:
  - Implemented Dead Code Elimination (DCE) for blocks, loops, and short-circuit operators.
  - Implemented constant propagation for `const` variables, omitting `OP_SET_LOCAL` instructions for compile-time constants.
- **AST Arena Allocator**: Implemented an arena allocator for AST nodes using a bump pointer and free list, significantly improving parsing performance.
- **Compile-time Errors**: Compile-time errors now include precise file names and line numbers.
- **Bytecode Operand Extension**: Introduced the `OP_EXTEND` prefix instruction to support 32-bit operands on demand, along with compile-time overflow checks for bytecode operand limits.

### Virtual Machine & Memory Management
- **Closure Memory Leak Fix**: Replaced `std::shared_ptr`-based `UpVal` with GC-managed `ObjUpVal`, completely fixing memory leaks caused by cyclic closure references.
- **Global Variable Access Optimization**: Replaced the global variable `std::unordered_map` with a `std::vector` and cached slot indices directly in the Inline Cache (IC), drastically improving global variable access speed.
- **Instruction Compression**:
  - Compressed inline cache instructions by moving `nameIdx` into the cache slot.
  - Compressed `OP_PASS_REFS` to a fixed 3-byte instruction using a call signature pool.
  - Compressed `OP_BUILD_MATRIX` to a fixed 3-byte instruction using a matrix shape pool.
  - Compressed `OP_MATCH_SHAPE` to 3 bytes using a shape pattern pool.
- **Heap Allocation Elimination**:
  - Precomputed reference counts to eliminate string allocations in built-in dunder calls.
  - Eliminated implicit `std::vector` and heap allocations in `invokeDunder`, `callVMFunction`, `callDunder` parameter packing, and slice index building.
  - Reduced heap allocations and optimized stack operations using `memmove`.
- **TCO Refinement**: Refined the Tail Call Optimization (TCO) disable condition to only block TCO when references (`ref`) to local variables are passed.

### Mathematics & CAS Engine
- **Strassen Matrix Multiplication**:
  - Implemented the Strassen algorithm for matrix multiplication with a fallback threshold.
  - Parallelized Strassen sub-matrix multiplications with depth-limited concurrency.
  - Reduced matrix allocations in Strassen using zero-copy views and dynamic peeling.
- **BigInt Algorithmic Improvements**:
  - Implemented the Karatsuba algorithm for BigInt multiplication.
  - Optimized BigInt division (Knuth D) with unsigned merging and branch elimination.
  - Optimized BigInt factorial, GCD, and modular exponentiation (`modPow`) algorithms.
- **Other Math Optimizations**:
  - Optimized Fibonacci sequence generation using the bit-scanning fast doubling algorithm.
  - Improved the Miller-Rabin primality test to use deterministic bases for ≤81-bit numbers and random bases for larger numbers.

### Test Framework
- **Test Runner**: Added a new test runner with isolated execution and summary reporting.
- **Test Suite Refactoring**: Reorganized tests into `core`, `modules`, `features`, and `syntax` directories, and added `TEST_SPEC.md` to standardize the test suite.

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
    JunkCalculator2 script.jc2 --debug # Execute with interactive step-debugger
    JunkCalculator2 script.jc2 --profile # Execute and print performance report

*Script Path Context: The `run` and `import` instructions push the executing script's directory onto a paths stack, resolving relative I/O based on the current file's location rather than the terminal's working directory.*

---

## Project Layout

    +-- src/
    |   +-- main.cpp                Entry point, CLI parser, and Workspace I/O
    |   +-- frontend/               Frontend components (Lexer, Parser, Compiler, AST, Highlight)
    |   +-- vm/                     Virtual Machine core (VM, Bytecode, Builtins, Interrupts)
    |   +-- memory/                 Memory & Type System (Value variant, GcHeap)
    |   +-- math/                   Math primitives (BigInt, Fraction, Complex, Matrix, Base)
    |   +-- cas/                    Computer Algebra System (Symbolic, Integration, Factorization)
    |   +-- modules/                Native C++ extensions (Image, Probability, JSON, Socket, etc.)
    +-- lib/                        Standard JC2 libraries
    +-- examples/                   Showcase scripts
    +-- jc2-language/               VS Code Language Support Extension

---

## License

MIT
