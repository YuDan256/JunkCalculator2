Version 2.6.4.0 completes the object member model, adds TLS 1.2 to the hand-written TLS stack, and fixes a
long-standing register-liveness defect in catch handlers. It also lands traits and interfaces, class-based
built-in error types, a new `http` / `tls` / `crypto` / `inflate` standard-library set, a linear-time regex
engine, and a slimmer release package that ships the always-on test suite only.

### OOP Member Model Completed
* Members may omit their initializer: a class member can be declared with no value (`const id`, `local balance`, `name`). It must then be assigned in `init`, otherwise construction throws `member 'x' was not initialized in init()`.
* Assign-once `const`: a `const` member declared without an initializer accepts exactly one assignment (in `init`); later writes from a method or from outside are rejected.
* Private fields are per-instance: initializer-less `local` members used to be stored on the class and shared between instances; each instance now owns its own copy.
* Introspection: new `hasField(obj, key)` and `getFields(obj)` globals work on instances, dicts and namespaces (public members only).

### Traits & Interfaces
* `trait` / `with` composition with abstract-method enforcement and frozen constraints, plus the `istrait(x)` predicate.
* Full support in the syntax highlighters, the language server and the help.

### Errors
* Built-in error types are classes: `TypeError`, `ValueError` and friends can be caught by type and subclassed like user classes.
* Uninitialized members are reported at construction time, before the instance can escape.

### TLS 1.2 Alongside TLS 1.3
* The client now offers TLS 1.3 **and** TLS 1.2 (ECDHE-RSA / ECDHE-ECDSA with AES-128-GCM and extended master secret) and follows the server's choice; `TlsConfig::enableTls12` switches it off for a byte-identical 1.3-only ClientHello.
* Sites that only speak TLS 1.2 (`www.baidu.com`, `www.qq.com`, `baike.baidu.com`) now work over `https`; TLS 1.3 sites are unaffected.

### Standard Library & Modules
* `http`: chunked bodies, redirects, gzip and `https`.
* `tls.connect` / `TcpStream`: raw TCP and TLS sockets for scripts.
* `crypto`: SHA-256/384/512, AES-GCM, X25519, P-256/384/521 and X.509 chain verification.
* `inflate`: zero-dependency gzip / zlib decoding.
* `Set` aggregates: `sum`, `prod`, `max`, `min`, `span`, `any` and `all` now work on sets.
* **`isapprox` for matrices**: `isapprox(A, B[, rtol[, atol]])` compares matrices of the same shape elementwise, with the same tolerance rule as for numbers; a shape mismatch yields `false`.


### Performance & Fixes
* Regex is linear: the per-position dedup table is reused and a literal-prefix prefilter skips non-matching regions — a real 160 KB page dropped from about 86 s to 0.013 s.
* Catch handlers keep live values: a parameter read inside `catch` no longer comes back as `none` after a call in the `try` body.
* GC: dangling-object fixes for instances, class member tables and closure copies.
* **Matrix integer powers**: `A^n` no longer comes back transposed — a row-major/column-major mismatch in the real-to-complex conversion made `A^1` differ from `A` and `A^-1` the transpose of `inv(A)`. The sibling symbolic conversions were fixed as well.
* **Native class methods**: `math.BaseNum.fromString(s, r)` works again (native class methods were registered without the `is_static` flag the class-call dispatch requires).


### CAS
* Broader factoring and solving (perfect powers, multivariate forms, `RootOf` for degree >= 4), branch-safe rewrite guards, and canonical results from `diff` / `integ`.

### Tooling & Docs
* Semantic highlighting: token spans are taken from the source text, so multi-line strings and comments no longer bleed into the next line, and f-string `{...}` interpolations are coloured as expressions.
* VS Code extension 6.4.0, with its bundled help re-synced.
* Help now documents initializer-less members; both READMEs were refreshed for 2.6.4.0.

### Renames & Breaking Changes
* `double` -> `float` (with `sqrtF` / `cbrtF` / `rootF`); the `is*` predicates are all lowercase; `pi` / `e` / `i` live in `math`; the `switch` expression is replaced by `match`; iteration ends with `StopIteration` instead of returning `none`.
* Tests moved: the suites now live under `test/` — `jc2 test test/tests`, `test/jit_tests` and `test/http_tests`.

### Packaging
* The release zip ships the always-on suite only (43 tests); the vector/network suite and its fixtures are a separate on-demand download. The main package shrank from 8.59 MB to 4.57 MB.
* The always-on suite was consolidated from 57 to 43 files with no test lost (822 test functions and 1621 assertions before and after).


***

Asset Notes
* JunkCalculator2-2.6.4.0-Windows-x64.zip: Statically-linked standalone Windows executable. Includes bin, lib (Native DLL extensions), modules (JC2 standard scripts), data, examples, and test/tests (43 always-on tests).
* JunkCalculator2-2.6.4.0-http_tests.zip: Optional bundle holding test/http_tests (6 vector and network tests plus their fixtures). Unpack it at the package root and run `jc2 test test/http_tests`.
* jc2-language-6.4.0.vsix: VS Code extension (language server client) with its bundled help re-synced.

Full Changelog: https://github.com/YuDan256/JunkCalculator2/compare/v2.6.3.2...v2.6.4.0
