Version 2.6.5.0 is a language-consistency and stability release. It adds compile-time directives,
package imports, improved namespace/include handling, comparison-driven natural ordering, and
several standard-library additions, while fixing binding, destructuring, include, and matrix
edge cases. This note covers all 248 commits since v2.6.4.0: 63 features, 75 fixes,
23 refactors, 29 test additions, 44 documentation changes, and packaging/utility work.

### Language
* **Compile-time directives**: `#version`, `#platform`, `#warning`, and `#error` with version and platform checks.
* **Package imports and include**: package entry points, submodules, `include import`, and namespace field merging.
* **Comparison and ordering**: `<=>` is available; `sort` and `sorted` are separate; natural ordering falls back to `__lt__` + `__eq__` when `__cmp__` is absent.
* **Definition names**: raw and quoted definition names are accepted; f-strings remain rejected as definition names.
* **Binding and patterns**: atomic destructuring commits, staged `state` targets, duplicate enum rejection, and stricter `ref` / modifier checks.
* **Comprehensions and match**: comprehension variables stay local; match branch targets are predeclared.
* **Matrix patterns**: default targets and rest targets interact correctly; trailing semicolons pin the number of rows.
* **Smaller language additions**: `reverse` / `reversed`, `getName` reflection, constant enum folding, and `ast` literal conversion helpers.

### Standard Library & Modules
* **`macros` package**: platform gates and compile-time helpers.
* **`math`**: numerical ODE solver, callable dense solutions, and `math.e()` / `math.E` namespace forms.
* **`cre` module**: chemical reaction engineering helpers.
* **`collections`**: LRU cache, UnionFind, and binary-tree helpers.
* **Graph algorithms**: MST, shortest paths, and strongly connected components.
* **`discrete`**: `Prop` / `Relation` text handling, CNF/DNF, satisfiability search, and Hasse diagrams.
* **Runtime helpers**: `exec` / `system`, `process` / environment access, `sys.globals`, bytecode dumping, and annotated disassembly.
* **AST and help APIs**: literal conversion helpers, script-side help registration, nested help lookup, extension LSP indexing/outline support, and `include` semantic highlighting.
* **Containers and reflection**: `toList`, iteration protocol, additional dunders, `getName`, and `getFieldInfo`.

### Fixes
* Base-class fields initialize before derived fields.
* `parseNum` rejects partial numeric input; `ord` and `parseNum` are available as globals.
* Include merging is atomic; include conflicts are rejected without partial mutation.
* Include merges the final public fields into the target namespace.
* `ref` arguments reject non-lvalues and remain safe across closures.
* Comprehension variables are local; match branch targets are visible during guard evaluation.
* Matrix default/rest destructuring matches smaller shapes correctly.
* Empty containers are falsy; private methods are hidden from `getFields`; workspace/bytecode writes are atomic.
* GC and JIT fixes cover shutdown, stack safety, slot flags, and member-name resolution.
* Diagnostics and scoping: use-before-initialization diagnostics, function-scoped block assignments, and tail-call restoration.

### Tooling & Docs
* **JC2 Language Specification, Volume 1** Chinese draft: 63 pages, with a self-contained grammar appendix and conformance matrix.
* Help topics were synchronized with runtime behavior: ordered containers, matrix columns, slice assignment, `getFieldInfo`, `#version =`, and `_` partial application.
* Test suite expanded to 73 files; `unit` and `unit --jit` both pass.
* Developer tooling: commit-message checker, PowerShell build wrapper, and JC2 regression runner.
* CAS: positive-radical merging and irreducible-cubic integration.

### Compatibility
* `math.i` is now a function; use `math.i()` or the literal `1i`. `math.I` remains the constant form.
* The global `e` is gone; use `math.e()` (function) or `math.E` (constant).
* Duplicate enum members now raise `SyntaxError`.
* `sort` and `sorted` remain distinct; `sort` mutates a list and returns it, `sorted` returns a copy.

***

Asset Notes
* JunkCalculator2-2.6.5.0-Windows-x64.zip: statically linked standalone Windows executable with bin, native libraries, modules, data, examples, and the always-on test suite (73 test files).
* JunkCalculator2-2.6.5.0-http_tests.zip: optional vector and network suite with fixtures.
* jc2-language-6.5.0.vsix: VS Code extension with synchronized bundled help.

Full Changelog: https://github.com/YuDan256/JunkCalculator2/compare/v2.6.4.0...v2.6.5.0
