<div align="right">
  <strong>English</strong> | <a href="README_zh-CN.md">简体中文</a>
</div>

# Junk Calculator 2.4.4.1

![Version](https://img.shields.io/badge/Version-v2.4.4.1-orange.svg?style=flat-square)
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

## What's New in v2.4.4.1

### Compiler & Mutual Recursion Support
- **Single-pass Compilation with Function Hoisting**: Implemented single-pass compilation based on function hoisting and forced reference capture. This fundamentally resolves forward reference issues for mutually recursive functions within the same scope (especially inside modules) at the compiler level, eliminating the need for multi-pass compilation.
- **Standard Library Cleanup**: Leveraged this mechanism to clean up standard libraries (`regex`, `discrete`, `http`). Removed redundant module prefixes (e.g., `regex._re_parse`) previously added to bypass compiler limitations, restoring clean lexical scope calls.
- **Module Encapsulation**: Optimized the encapsulation of modules like `http` by moving internal functions to private namespaces like `_Http`, preventing internal implementations from polluting public APIs.

### Parsing Optimization (Dict vs. Block Disambiguation)
- **Lookahead Logic**: Completely optimized the lookahead detection logic for curly braces `{ ... }` (`isDictLiteralLookahead`). Accurately distinguishes between block statements and dictionary literals based on internal features like colons, semicolons, compound assignments (e.g., `+=`), and newlines.
- **Bug Fixes**: Fixed issues where pure code blocks (e.g., `{ max_str += self.nx() }` or `{ f() }`) were misidentified as dictionary literals, causing syntax errors.

### Destructuring Assignment & Pattern Matching Refactoring
- **Early Sniffing**: Removed the unstable "expression-to-pattern" patch. Introduced early lookahead sniffing to identify destructuring assignments (e.g., `[a] = ...`) during the initial parsing phase, directly constructing pattern nodes to prevent dict or block nodes from being mislabeled.
- **Enhanced Destructuring**: Supports complex lvalues in destructuring; perfectly supports the combination of local scope modifiers (`local`, `const`, `ref`, `state`) with default values (`= value`); supports placing modifiers before rest parameters (`...rest`).
- **Const Semantics**: Decoupled `const` from `local` and reintroduced the `ConstDecl` node to prevent global constants from being incorrectly downgraded to local constants. Merged the `const` flag for overall patterns and single variables. Prohibited the VM from redefining or overwriting existing constants globally. Strictly prohibited assignment or compound assignment initialization for `const ref`.
- **Syntax Rules**: Clarified syntax rules: when default values exist in the same destructuring match line, the rest parameter `...rest` must be placed at the end. The compiler will report errors for violations to prevent semantic ambiguity.

### Matrix System & VM Optimization
- **Unified Empty Matrix**: Standardized that numeric and string matrices (`StringMatrix`) with 0 total elements will uniformly collapse into a standard `0x0` shape during construction. This resolves equality and output inconsistencies caused by differently shaped empty matrices (e.g., `1x0`).
- **Bounds Checking**: Added out-of-bounds exception checks for matrix indexing, ensuring that destructuring can correctly catch out-of-bounds errors and fallback to default value logic.
- **Pattern Matching Bytecode**: Upgraded the `OP_MATCH_SHAPE` instruction, moving elastic 2D shape validation (supporting optional rows/columns with default values) into the VM's C++ backend for one-shot execution.

### Tests & Documentation Synchronization
- **Documentation**: Updated help documentation with advanced examples of function destructuring parameters and explicitly documented the ordering rules for `...rest` and default values.
- **Tests**: Synchronized math modules and destructuring test cases to adapt expected values to the unified `.0` suffixed floating-point format and the latest empty matrix matching logic.

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
