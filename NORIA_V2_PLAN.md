# Noria V2 — Implementation Plan

**Overview:** Take Noria from its `i32`/`bool` MVP to a V2 with floating point, I/O, unary/logical/bitwise operators, strings, arrays, structs, generics, modules, and a source-based data-structure standard library, then prove it out with a deterministic CLI dungeon game written in Noria, updating docs/tests throughout.

The work is sequenced so each phase ends in a compiling, testable state, and there's a clear "minimum shippable V2" cut line if time runs short.

## Current status (as of August 2026)

**Branch:** `mmcrae/v2` — Phases 0–4 are implemented; Phase 5 is next.

| Phase | Status | Notes |
|-------|--------|-------|
| 0 — type-refactor | **Done** | Canonical `Type` in `Types.hpp`; `llvmType` adapter |
| 1 — operators | **Done** | Unary, logical, bitwise, `%`, `else if`, `as` |
| 2 — floats-io-cast | **Done** | `f64`, print builtins, casts, `sqrt`/`pow`; FizzBuzz + hello world |
| 2.5 — architecture refactor | **Done** | Types, diagnostics, builtins, Visitor, facade, IrEmitter, Place |
| 3 — strings | **Done** | `len`, `s[i]`, `str + str`, escapes; `print(str)` |
| 4 — arrays | **Done** | `[T]`, literals, `len`, index read/write places |
| 5 — structs | **Partial** | Decl, construction, field rvalue (locals); field lvalue + params pending |
| 6–9 | **Not started** | Blocked on 5 |

**Regression gate:** `just test` (83 `examples/basic`, 54 `examples/invalid`, 6 `examples/invalid_syntax`, C++ unit tests). Use `just sanitize` after AST ownership, string storage, Place, or pointer-arithmetic changes.

**Documentation:** README/SYNTAX updated through Phase 4; keep updating after each feature phase.

## Task checklist

- [x] **Phase 0 — type-refactor:** Generalize `Type` (TypeChecker.hpp) and `IrType` (Codegen.hpp) from flat enums to kind+payload representations; keep all existing examples passing.
- [x] **Phase 1 — operators:** Add unary (`!`, `-`, `~`), logical (`&&`, `||` short-circuit), bitwise (`& | ^ << >>`), modulo (`%`), optional `else` / `else if`, and `as` cast tokens/AST/parse levels.
- [x] **Phase 2 — floats-io-cast:** Add `f64` end-to-end, expression statements, print builtins (printf/putchar), `as` casts (sitofp/fptosi), and sqrt/pow intrinsics. FizzBuzz + hello world run.
- [x] **Phase 2.5 — architecture refactor:** Canonical types, full Visitor, Compiler facade, builtin registry, CodegenContext + IrEmitter, postfix + Place foundation. No new language surface; all examples stay green after each checkpoint.
- [x] **Phase 3 — strings (finish):** Indexing, `len`, concat on top of Phase 2.5 infrastructure.
- [x] **Phase 4 — arrays:** Array types, literals, indexing, `len`; indexed places via Phase 2.5 Place path.
- [ ] **Phase 5 — structs:** Struct decls, construction, field access (rvalue + lvalue), pass by value.
- [ ] **Phase 6 — generics-modules:** Add source imports, generic structs/functions, compile-time implementation tags, constraints, and reachable-specialization monomorphization.
- [ ] **Phase 7 — data-structure-stdlib:** Ship implementation-independent `Sequence`, `Dictionary`, and `Set` APIs plus generic heap algorithms in Noria source.
- [ ] **Phase 8 — demo-dungeon:** Add `read_char()` input and write a deterministic CLI dungeon game in Noria (`examples/demos/dungeon_cli.noria`) using the standard-library ADTs, with scripted transcript tests.
- [ ] **Phase 9 — docs-tests polish:** Final harness audit, `--emit-ast` snapshot coverage, standard-library reference, dungeon sample session, resume bullet.

## Architecture today (what we're extending)

```text
.noria -> Lexer.cpp -> Parser.cpp (AST) -> TypeChecker.cpp -> Codegen.cpp (LLVM IR text) -> opt -> llc -> clang
```

After Phase 2.5 the in-memory pipeline moves behind a `Compiler` facade; CLI, file I/O, optimization, and native linking stay in `main.cpp`.

**Current pain points (motivation for Phase 2.5):**

- `Type` and `IrType` are structurally identical mirrors; both stages independently parse type name strings.
- TypeChecker, Codegen, and AstPrinter each maintain parallel `dynamic_cast` dispatch chains (~15 sites per pass).
- Builtins are duplicated as string `if` chains in TypeChecker and Codegen.
- Codegen threads `nextTemporary`/`nextLabel` through every method and uses `mutable` module state on a `const` generator.
- `AssignmentStatement.lhs` is still a `std::string`; no postfix parsing for `[` or `.`.
- `Diagnostic.hpp` is only `CompileError`; location formatting is copy-pasted in Lexer, Parser, and TypeChecker.

Codegen remains string-based with a stack/alloca model and `vector<Scope>` of bindings. We keep emitting IR text — no LLVM C++ API.

## Cross-cutting design decisions (chosen defaults)

- Memory: AOT, no GC. Heap via libc `malloc`/`free` declared as externs in emitted IR. Leak-on-exit is acceptable for demos; documented as "arena allocator / free is future work."
- Strings: null-terminated `i8*` (C strings). Literals become private LLVM globals; `len` -> libc `strlen`; concat -> `malloc`+`strcpy`/`strcat`; `s[i]` -> load `i8`, zext to `i32`.
- Arrays `[T]`: pointer to a length-prefixed heap block `[ i64 len, T elems... ]`. `len(a)` loads the header; `a[i]` is a `getelementptr` past the header. Literals `malloc` then store length + elements.
- Structs: LLVM named types (`%Point = type { i32, i32 }`). Locals/params are `alloca`'d; field read is `getelementptr`+`load`; passed by value as LLVM aggregates.
- Generics: generic structs and functions are monomorphized for reachable concrete type/tag combinations. No runtime type erasure, virtual dispatch, or implementation-tag branching.
- Standard library: container APIs and algorithms live under `stdlib/` as Noria source modules. A small private runtime ABI provides allocation and typed buffer/node operations; it is not a user-facing pointer or allocator API.
- Container semantics: `Sequence`, `Dictionary`, and `Set` are opaque heap-backed handles. Copying or passing a handle aliases the same container; all implementations of an ADT expose the same observable API while retaining implementation-specific complexity.
- `print` is a set of recognized builtins backed by libc `printf`/`putchar`: `print(str)`, `print_int(i32)`, `print_float(f64)`, `print_char(i32)`, `println()`.
- Input: Phase 8 adds `read_char() -> i32`, backed by libc `getchar`; it returns the next byte as an `i32` or `-1` at EOF. It is intentionally deferred so Phase 2.5 remains behavior-preserving.
- Casts use an `as` expression: `x as f64` -> `sitofp`, `y as i32` -> `fptosi`/`trunc`.
- Math: `sqrt`, `pow` via LLVM intrinsics (`@llvm.sqrt.f64`, `@llvm.pow.f64`).
- Testing discipline (non-negotiable): **every feature ships with its tests in the same phase, not later.** Each phase ends with a "Tests" checklist; a phase is not "done" until those tests pass and the full suite is green. Phase 9 is final polish only. Each new feature adds: (1) a positive `examples/basic` (or `examples/demos`) program; (2) at least one negative case in `examples/invalid`; and (3) where applicable, an exit-code or stdout assertion in `tests/run_examples.sh`. **All existing examples stay green after every phase and every Phase 2.5 checkpoint** as the regression gate.

## Phase 0 - Type representation refactor (foundation) — DONE

Generalized the two flat enums so later phases add cases instead of reshaping everything.

- `include/noria/TypeChecker.hpp`: `TypeKind` + `Type` with element/struct payload.
- Mirrored shape in `include/noria/Codegen.hpp` as `IrType` (to be consolidated in Phase 2.5).
- `tests/type_representation_test.cpp` covers equality and `name()`.
- Negative examples exercise `f64`/`str` type names in diagnostics.

## Phase 1 - Operators + control-flow polish — DONE

- Lexer: `!`, `&&`, `||`, `&`, `|`, `^`, `~`, `<<`, `>>`, `%`, `as`.
- AST: `UnaryExpression`, extended `BinaryOperator`, `CastExpression`.
- Parser: full precedence chain; optional `else` and `else if`.
- TypeChecker + Codegen: short-circuit `&&`/`||`, unary/bitwise/`%`.
- Tests: `examples/basic/unary_operators.noria`, `logical_operators.noria`, `bitwise.noria`, `short_circuit_*`, `else_if.noria`, and matching `examples/invalid/` cases.

## Phase 2 - Floats, I/O, casts — DONE

- `f64` end to end: float literals, `FloatLiteral`, `fadd`/`fsub`/`fmul`/`fdiv`, `fcmp`.
- `ExpressionStatement` for bare calls like `print("hi");`.
- Print builtins, `as` casts, `sqrt`/`pow` intrinsics.
- Acceptance: `hello_world.noria`, `fizzbuzz.noria` (stdout goldens); `float_math`, `cast_*`, `math_builtins`.

## Phase 2.5 - Architecture refactor (pre-strings gate) — DONE

**Goal:** Behavior-preserving structural cleanup before finishing strings, arrays, and structs. No new language surface area. Split into independently green checkpoints; run `just test` after each.

### Design patterns and touch points

| Pattern | Where | Purpose |
|---------|-------|---------|
| **Canonical Type + Adapter** | `Types.hpp`/`Types.cpp`, AST, TypeChecker, Codegen | One language-level `Type`; `llvmType(const Type&)` maps to LLVM spellings; aggregate layouts live in codegen context, not in `Type` |
| **Visitor** | `AstVisitor.hpp`, TypeChecker, Codegen, AstPrinter | Replace parallel `dynamic_cast` chains; void-returning `visit` with pass-local result wrappers |
| **Facade** | `Compiler.hpp`/`Compiler.cpp`, `main.cpp` | `compileSource()` with `StopAfter { Tokens, Ast, Typed, Ir }`; CLI/file I/O/optimization/linking stay in `main` |
| **Registry** | `Builtins.hpp` | `BuiltinId` keyed descriptor table shared by TypeChecker (validation) and Codegen (emission) |
| **Context Object + Builder** | `CodegenContext`, `IrEmitter` | Replace `mutable` module state and threaded temp/label counters; thin load/store/GEP/branch helpers |
| **Postfix + Place** | `Parser::parsePostfix`, `AssignmentStatement`, TypeChecker, Codegen | Postfix loop for calls (extracted from `parsePrimary`); assignment `lhs` becomes owned expression; `checkPlace`/`generatePlace` for identifiers initially |

### Checkpoint sequence

Each checkpoint is a green refactor: `just test` must pass before moving on.

1. **Canonical types**
   - Extract `Type`/`TypeKind` to `include/noria/Types.hpp` + `src/Types.cpp`.
   - AST declarations store canonical `Type` (replace `typeName: string` on `LetStatement`, `Parameter`, `Function.returnType`, `CastExpression` target).
   - Codegen `Value`/`LocalBinding` use `Type`; remove `IrType`/`parseIrType`; add `llvmType(const Type&)` adapter.
   - Extend `type_representation_test.cpp` for LLVM adapter spellings.
   - *Checkpoint:* all examples green; no behavior change.

2. **Shared diagnostics**
   - Add `formatDiagnostic(SourceLocation, stage, message)` to `Diagnostic.hpp`.
   - Unify Lexer/Parser/TypeChecker location formatting; preserve existing error text contracts tested by `run_examples.sh`.
   - *Checkpoint:* grep-based negative tests still pass.

3. **Builtin registry**
   - Add `Builtins.hpp` with `BuiltinId` enum and signature descriptors.
   - TypeChecker validates via table; Codegen dispatches on `BuiltinId`.
   - Migrate existing builtins: `print`, `print_int`, `print_float`, `print_char`, `println`, `sqrt`, `pow`.
   - Add C++ unit test for lookup and arity validation.
   - *Checkpoint:* all examples green.

4. **Full Visitor**
   - Add `AstVisitor` with `visit` overloads for every current statement and expression node.
   - Migrate TypeChecker, Codegen, AstPrinter to visitor implementations.
   - Remove `dynamic_cast` chains from those three files.
   - Add C++ unit test: visitor reaches every node kind in a smoke AST.
   - *Checkpoint:* all examples green; `--emit-ast` output unchanged for existing programs.

5. **Compiler facade**
   - Extract `Compiler::compileSource(source, StopAfter)` from `main.cpp`.
   - `main` retains CLI parsing, file read/write, `optimizeLlvmIr`, `buildNativeExecutable`.
   - Add C++ test: compile known-good source through `StopAfter::Typed` without subprocess.
   - *Checkpoint:* CLI behavior unchanged; `just test` green.

6. **CodegenContext + IrEmitter**
   - Replace `mutable functions_`/`moduleGlobals_`/`nextStringGlobal_` with explicit `CodegenContext`.
   - Introduce thin `IrEmitter` owning temp/label generation and common emission (`freshTemp`, `emitLoad`, `emitStore`, `emitBranch`).
   - Extract runtime declarations (`printf`, `puts`, `putchar`, `@noria_print_int`, intrinsics) into a runtime catalog consumed by preamble generation.
   - *Checkpoint:* all examples green; run `just sanitize`.

7. **Postfix + Place foundation**
   - Add `parsePostfix` loop: extract call parsing from `parsePrimary`; prepare hook for `[expr]` and `.ident` (tokens added when features land).
   - Change `AssignmentStatement::lhs` from `std::string` to `std::unique_ptr<ast::Expression>`.
   - TypeChecker: `checkPlace` / `checkRvalue` split; identifier places only for now.
   - Codegen: `generatePlace` / `generateRvalue` split; identifier places only.
   - Visitor + AstPrinter updated for new assignment shape.
   - *Checkpoint:* all `identifier = expr` examples green; run `just sanitize`.

### Phase 2.5 tests

- `just test` after every checkpoint (full regression gate).
- `just sanitize` after checkpoints 4, 6, and 7 (ownership, string globals, Place).
- New C++ targets (register in `CMakeLists.txt` + CTest):
  - Extended `type_representation_test` (LLVM adapter).
  - `builtin_registry_test`.
  - `visitor_smoke_test`.
  - `compiler_facade_test`.
- No new Noria example programs in 2.5 — behavior must not change.
- Update `README.md` and `SYNTAX.md` for Phases 0–2 features (remove stale "Limitations" entries for implemented operators, floats, strings-as-literals, print).

### Explicitly deferred (not V2)

Do not introduce these during Phase 2.5 or later V2 work unless requirements change:

- Pratt/climbing parser (explicit precedence levels are a project rule).
- `std::variant` AST rewrite.
- Pass manager or pluggable compiler passes.
- Typed AST / semantic IR between typecheck and codegen.
- Full textual IR builder (instruction/basic-block object model).
- Generic C++ scope-stack template framework (unrelated to Phase 6 language generics).
- Declarative test manifest or per-example sidecar metadata (keep explicit shell assertions in `run_examples.sh`).
- LLVM C++ API.
- User-defined traits, custom allocators, iterators, package management, and runtime-selected container implementations.

## Phase 3 - Strings (finish) — DONE

**Already done:** string literals with escapes, `StringLiteral` AST, `TypeKind::Str`, `print(str)` via `puts`, negative type-mismatch examples.

**Builds on Phase 2.5:** postfix loop + `IndexExpression`, builtin registry (`len`), runtime catalog (`malloc`/`strlen`/`strcpy`/`strcat`), IrEmitter for GEP/load/heap calls, Visitor for new nodes.

- Lexer: `[` `]` tokens (if not added in 2.5 postfix prep).
- AST: `IndexExpression { base, index }`.
- Parser: `s[i]` via postfix; string `+` overload in binary typing helper.
- TypeChecker: index rules for `str` (index `i32`, result `i32`); `len(str) -> i32`; `str + str -> str`.
- Codegen: `s[i]` GEP + load i8 + zext; `len` -> `strlen`; concat -> malloc + strcpy/strcat.
- Tests:
  - Convert `examples/future/string_output.noria` to passing `examples/basic/`.
  - Positive (stdout-asserted): `len(s)`, `s[i]` printed as int, `+` concat, escape sequence example (`"\n\t\""`).
  - Negative: `len(42)`, index with non-`i32`, concat with non-`str`.
  - `just sanitize` on concat path.
- Update `SYNTAX.md` string section.

## Phase 4 - Arrays — DONE

**Builds on Phase 2.5:** canonical `Type::array(element)`, recursive `parseType()` for `[T]`, Place/Visitor for indexed assignment, layout registry in `CodegenContext`.

- No second assignment redesign — `AssignmentStatement.lhs` is already an expression from Phase 2.5.
- AST: `ArrayLiteral { elements }`; extend `IndexExpression` for array bases.
- TypeChecker: `parseType` for `[T]`; `len([T]) -> i32`; index element type from array element type.
- Codegen: array literal `malloc(8 + n*sizeof(T))`, store header + elements; `a[i]` GEP past header; indexed `generatePlace`.
- Convert `examples/future/arrays_sum.noria` to passing example.
- Tests:
  - Positive: array literal + sum loop, `len(a)`, read-then-write `a[i] = a[i] + 1`, nested index.
  - Lvalue regression: plain `identifier = expr` still green.
  - Negative: index non-array, type-mismatched store, assign to non-lvalue (`len(a) = 3`).
  - `just sanitize` on malloc sizing and GEP offsets.
- Update `SYNTAX.md` arrays section.

## Phase 5 - Structs — NOT STARTED

**Builds on Phase 2.5 + 4:** declaration collection pass, struct layout registry, field expressions as postfix `.ident`, Place for field mutation.

- Lexer/parser: `struct Name { field: T; ... }`; `Name { field: expr, ... }` construction.
- AST: `StructDecl`, `StructLiteral`, `FieldAccessExpression`; extend `ast::Module` with struct declarations.
- TypeChecker: `collectStructDecls` before function checking; field typing; construction validation.
- Codegen: emit `%Name = type {...}`; construction via alloca+stores; field GEP as rvalue and lvalue Place; pass by value.
- Convert `examples/future/struct_point.noria` to passing example.
- Tests:
  - Positive: construct, read fields, `p.field = v`, pass-by-value copy semantics, struct with array/string field.
  - Negative: unknown field, missing/extra/mis-typed field, `.field` on non-struct.
  - `just sanitize` on field offsets and aggregate passing.
- Update `SYNTAX.md` structs section.

## Phase 6 - Generics and source modules

**Goal:** Provide the minimum language and compiler foundation required to implement reusable generic containers in Noria source. Keep specialization compile-time and explicit rather than adding a runtime object model.

- Modules and imports:
  - Add `import std::sequence::{Sequence, sequence_new, sequence_push};`-style declarations and resolve bundled modules from `stdlib/`.
  - Parse each source module once, preserve source locations across files, reject duplicate exports, report missing imports clearly, and detect import cycles.
  - Extend the `Compiler` facade to compile a resolved module graph while the CLI remains responsible for the root file and search paths.
- Generic declarations and types:
  - Parse type-parameter lists on structs and functions and generic type applications such as `Sequence<i32, arr>`.
  - Treat `arr`, `list`, `bst`, and `hashmap` as closed compile-time implementation tags, not runtime values or ordinary user types.
  - Support implementation-specific generic definitions selected by the tag while requiring every implementation of an ADT to export the same public operations.
  - Represent generic parameters and applications canonically in `Type`; substitute them during type checking and reject wrong arity, unknown tags, or unresolved parameters with source-located diagnostics.
- Constraints:
  - Check required operations when a specialization is instantiated rather than adding user-defined traits in V2.
  - Heap ordering and `bst` keys require `<` and `==`.
  - `hashmap` keys require `==` and the standard-library `hash` operation; V2 provides hashing for `i32`, `bool`, and `str`.
- Monomorphization:
  - Instantiate only reachable concrete generic structs/functions, cache specializations by canonical type arguments plus implementation tag, and use deterministic mangled LLVM names.
  - Detect recursive specialization cycles and avoid duplicate code emission when multiple modules request the same specialization.
- Private standard-library runtime ABI:
  - Add internal allocation, reallocation, release, and typed buffer/node primitives needed by Noria source containers.
  - Keep these primitives unavailable to ordinary imports; the public API remains safe ADT operations rather than pointers.
- Tests:
  - Positive multi-module import, generic function, generic struct, nested generic application, and specialization-reuse cases.
  - Negative missing/cyclic imports, duplicate exports, wrong generic arity, unknown implementation tag, unresolved type parameter, and unsatisfied operation/hash constraints.
  - C++ unit tests for substitution, specialization keys, deterministic mangling, and import-graph ordering.
  - Run `just test` after each checkpoint and `just sanitize` for specialization ownership and cross-module AST lifetimes.

## Phase 7 - Noria data-structure standard library

**Goal:** Expose stable ADT APIs whose semantics do not change when callers select a different compile-time implementation. Canonical V2 spellings use Noria's `str` type and lowercase implementation tags: `Sequence<i32, arr>`, `Sequence<i32, list>`, `Dictionary<i32, str, bst>`, `Dictionary<i32, str, hashmap>`, `Set<i32, bst>`, and `Set<i32, hashmap>`.

- Source layout:
  - `stdlib/sequence.noria`
  - `stdlib/dictionary.noria`
  - `stdlib/set.noria`
  - `stdlib/heap.noria`
- `Sequence<T, Impl>`:
  - `Sequence<T, arr>` is a growable contiguous array analogous to `vector<T>`, with length/capacity metadata and geometric growth.
  - `Sequence<T, list>` is a doubly linked list analogous to `list<T>`, with heap-backed nodes.
  - Both implementations expose the same constructor, `len`, `push`, `pop`, `get`, `set`, `insert`, and `remove` operations. Bounds and empty-container violations fail through one stable runtime diagnostic path.
- `Dictionary<K, V, Impl>`:
  - `Dictionary<K, V, bst>` is an ordered binary-search-tree map with deterministic in-order traversal; balancing is deferred beyond V2.
  - `Dictionary<K, V, hashmap>` is a flat, open-addressed hash map with linear probing, tombstones, a bounded load factor, and geometric resizing. It is analogous to a flat hash map, not the sorted C++ `std::flat_map`.
  - Both implementations expose the same constructor, `len`, `insert`/upsert, `contains`, `get`, `get_or`, and `remove` operations. `get` requires a present key; `get_or` covers absence without adding `Optional` to V2.
- `Set<T, Impl>`:
  - `Set<T, bst>` and `Set<T, hashmap>` reuse the corresponding dictionary search/storage strategy without a value type parameter.
  - Both implementations expose the same constructor, `len`, `insert`, `contains`, and `remove` operations; inserting an existing value is idempotent.
- Generic heap algorithms in `stdlib/heap.noria`:
  - `Heappush(sequence, value)` inserts into a min-heap.
  - `Heappop(sequence)` removes and returns the minimum value; an empty sequence uses the standard empty-container failure path.
  - `Heapify(sequence)` transforms an existing sequence into a min-heap in place.
  - Algorithms accept either `Sequence<T, arr>` or `Sequence<T, list>` when `T` supports `<`. Semantics are identical; document that random access makes the `arr` implementation asymptotically preferable.
- Tests:
  - Run one behavioral conformance suite against every implementation tag to prove the public ADT behavior is implementation-independent.
  - Sequence coverage: growth, front/middle/back insertion/removal, bounds, empty operations, aliasing, and nested element types.
  - Dictionary coverage: replacement, missing keys, ordered traversal for `bst`, deliberate hash collisions, tombstone reuse, resize boundaries, and supported key types.
  - Set coverage: duplicates, membership, removal, `bst` ordering, and hash collisions.
  - Heap coverage: both sequence implementations, empty/singleton inputs, duplicates, negative values, repeated pop order, and heapify invariants.
  - Negative cases cover wrong type/tag arity, operation type mismatches, unsupported hash keys, and heap elements without ordering.
  - Run the full suite and `just sanitize` across growth, node allocation, removal, collision, and heap-mutation paths.
- Documentation:
  - Add a standard-library API/complexity matrix to `SYNTAX.md` and usage examples showing that changing only the implementation tag preserves behavior.
  - Document heap algorithms as implementation-independent but performance-sensitive: `arr` has efficient indexed heap operations, while `list` pays for traversal.

## Phase 8 - Deterministic CLI dungeon game (in Noria)

**Goal:** Prove the V2 language and standard library with one substantial, reproducible native program. The game is intentionally multi-function and control-flow-heavy so it provides a larger compiler workload, but tests assert correctness rather than unstable wall-clock compile times.

- Add `read_char() -> i32` end to end:
  - Register its zero-argument signature in the shared builtin registry and validate calls in TypeChecker.
  - Declare libc `getchar` through the runtime catalog and emit the call in Codegen.
  - Preserve `-1` at EOF so programs can terminate cleanly on exhausted input.
- `examples/demos/dungeon_cli.noria`:
  - Store a fixed two-dimensional dungeon in `Sequence<i32, arr>` with walls, floor, and an exit.
  - Use `Sequence<Enemy, list>` for enemies, `Set<i32, hashmap>` for visited tiles, and a `Dictionary<i32, str, bst>` command/help table so the demo exercises each public ADT.
  - Model player and enemies with structs; use indexing, field mutation, strings, generic containers, functions, and nested control flow across movement, collision, rendering, and combat.
  - Accept `w`, `a`, `s`, `d`, and `q` through `read_char`; ignore line separators and reject unknown commands with a stable message.
  - Use fixed enemy placement, deterministic enemy movement, and deterministic damage. Do not depend on randomness, clocks, raw-terminal control, or platform-specific escape sequences.
  - End with explicit victory, defeat, or quit messages and a stable exit code.
- Tests:
  - Add a focused positive `read_char` example plus an `examples/invalid` wrong-arity case.
  - Add `dungeon_cli.input` fixtures and neighboring `.expected` transcripts for victory, defeat, and quit paths.
  - Extend `tests/run_examples.sh` with a native stdin/stdout helper that redirects a fixture to the executable and compares exact output.
  - Compile the dungeon normally and through the optimized path to exercise the full textual LLVM IR pipeline; do not assert a compile-duration threshold.
  - Run `just test` and `just sanitize` for the aggregate, indexing, and native-input paths.

## Phase 9 - Docs, tests, polish

- Per-feature tests already exist from earlier phases; Phase 9 is audit and polish only.
- Audit `tests/run_examples.sh` for coverage gaps; wire ASan/Valgrind into CI if not already.
- Refresh `--emit-ast` snapshot test for all node kinds (Visitor makes this maintainable).
- Final README/SYNTAX pass; include the standard-library API/complexity reference, a deterministic dungeon sample session and feature walkthrough, and the resume bullet.

## Timeline (milestone-based, part-time)

Obsolete calendar (end-of-July) replaced with dependency-ordered milestones:

| Milestone | Estimate | Delivers |
|-----------|----------|----------|
| Phase 2.5 complete | ~2 weeks | Visitor, types, facade, registry, emitter, Place foundation |
| Phase 3 finish | ~1 week | String index, len, concat |
| Phase 4 | ~1 week | Arrays + indexed places |
| Phase 5 | ~1 week | Structs, field places, aggregate passing |
| Phase 6 | ~2 weeks | Generic types/functions, source modules, monomorphization |
| Phase 7 | ~2–3 weeks | Sequence, Dictionary, Set, heap algorithms |
| Phase 8 | ~1–2 weeks | `read_char` input + deterministic CLI dungeon using the ADTs |
| Phase 9 | ~2–3 days | Final docs and harness audit |

**Minimum shippable V2** if time runs short: Phases 0–2 (done) + 2.5 + 3 + 4 + 5 + 6 + 7 + 8 (generic data-structure library and deterministic CLI dungeon). Phase 9 is final documentation and harness polish.

## Risks / notes

- **Phase 2.5 scope creep:** Keep checkpoints behavior-preserving; do not land new language features during the refactor. If a checkpoint grows, split it further rather than mixing feature work.
- **Visitor migration:** Migrate one pass at a time (AstPrinter first if lowest risk, then TypeChecker, then Codegen) with green tests between each.
- **Place foundation:** Phase 2.5 only supports identifier places; indexed/field places arrive in Phases 3–5 without another assignment AST change.
- **Aggregate codegen:** GEP offsets and `malloc` sizing remain the fiddliest part; layout registry in `CodegenContext` centralizes this. Keep `just sanitize` running.
- **Specialization growth:** Monomorphization can multiply emitted code. Instantiate only reachable canonical combinations, cache them across the module graph, and include specialization counts in debug output.
- **Container constraints:** `bst` and heap operations require ordering; `hashmap` requires equality and hashing. Keep V2 constraints structural and limited to supported built-in operations rather than introducing a trait system.
- **Implementation tradeoffs:** A linked-list sequence intentionally performs poorly for indexed heap operations, and the V2 `bst` is not balanced. Preserve shared ADT semantics while documenting these complexity differences.
- **Documentation drift:** README/SYNTAX lag the compiler today; update at end of Phase 2.5 and after each feature phase per testing rules.
- **Regression gate:** All existing examples (currently 69 basic + 34 invalid + 5 invalid_syntax) must stay green after every phase and every Phase 2.5 checkpoint.
