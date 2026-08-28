# Engineering Noria

This document explains how Noria is built: the compiler architecture, the invariants carried between stages, the design patterns used to keep features coherent, the performance model, the difficult implementation problems, and the tradeoffs that define the current scope.

It is intentionally about decisions visible in the code. The language reference lives in [SYNTAX.md](SYNTAX.md); build and usage instructions live in the [README](README.md).

## Engineering profile

Noria is an ahead-of-time compiler written in C++20. It owns the language front end and semantic pipeline, emits LLVM IR as text, optionally runs LLVM's optimizer, and delegates native linking to the host Clang driver. The standard data-structure library is implemented in Noria source over a deliberately small private runtime ABI.

The repository currently contains approximately 15,000 lines of compiler/header code and 1,500 lines of Noria standard-library code. Its validation corpus includes 255 accepted programs, 164 negative programs split between semantic and syntax failures, and 13 focused C++ test executables. Those numbers matter less than the coverage shape: compiler stages, generated IR, native behavior, optimizer behavior, diagnostics, traps, ownership, generics, and ADT conformance are all exercised.

The central engineering goal is to keep the project small enough to understand end to end while still making the hard semantics explicit. Noria does not hide container behavior behind C++ builtins, outsource the front end to a parser generator, or rely on a garbage collector to defer ownership questions.

## End-to-end architecture

```text
source text
   │
   ▼
Lexer ──► tokens with SourceLocation
   │
   ▼
Parser ──► owned AST using canonical Type values
   │
   ▼
ModuleResolver ──► selectively merged declarations + SymbolOrigins
   │
   ▼
ADT default expansion ──► explicit implementation tags
   │
   ▼
TypeChecker ──► checked declarations + specialization requests
   │
   ▼
Monomorphizer ◄──► TypeChecker frontier checks
   │
   ▼
LLVMGenerator ──► textual LLVM IR + runtime definitions
   │
   ├──► opt -O1/-O2/-O3 (optional)
   ▼
host clang ──► native executable
```

### Stage boundaries and invariants

| Stage | Primary responsibility | Invariant on success |
| --- | --- | --- |
| Lexer | Normalize identifiers, recognize literals/operators, preserve locations | Every token has file/line/column data; unknown characters fail locally |
| Parser | Build declarations and expression/statement trees with explicit precedence | AST ownership is unique; imports precede declarations; syntax is structurally valid |
| Module resolver | Load bundled `std::` sources, reject cycles/private imports, merge selected exports | Every merged symbol retains its module of origin |
| ADT defaulting | Expand omitted standard-ADT implementation arguments | `Sequence`, `Dictionary`, and `Set` reach semantic analysis with concrete tags |
| Type checker | Collect declarations, infer returns/type arguments, enforce visibility and constraints | Every concrete function has a return type and every expression/place use is valid |
| Monomorphizer | Materialize reachable generic functions/structs and rewrite applications | No generic template reaches code generation; specializations are deterministic and deduplicated |
| Code generator | Lower checked AST, runtime checks, ownership, and layouts to LLVM | Emitted functions have explicit terminators and managed paths carry correct clone/drop behavior |
| CLI/toolchain | File I/O, executable/stdlib discovery, `opt`, native linking | Front-end logic remains callable without subprocesses through `compileSource()` |

The compiler facade in `include/noria/Compiler.hpp` exposes `StopAfter::Tokens`, `StopAfter::Ast`, `StopAfter::Typed`, and `StopAfter::Ir` checkpoints. This separation is useful in two ways: the CLI is a thin integration layer, and C++ tests can exercise the compiler in memory without writing files or launching the binary.

## Architectural decisions

### One canonical language type

`Type` in `include/noria/Types.hpp` is the shared representation used by the AST, type checker, monomorphizer, and code generator. It is a kind-plus-payload value:

- scalar kinds carry no payload;
- arrays carry an element type;
- structs carry a name and concrete type arguments;
- type parameters carry their source name;
- implementation tags carry a closed `ImplementationTag` value.

`LLVMType(const Type&)` is an adapter at the backend boundary. This avoids a common compiler failure mode where parser, checker, and backend maintain subtly different type enums or repeatedly parse type-name strings. Language identity stays independent from physical layout: the type says “struct Point,” while codegen owns field order and LLVM layout.

### Owned AST plus visitor-based passes

AST child nodes use `std::unique_ptr`, making tree ownership explicit and allowing whole modules/functions to be moved through the pipeline. `AstVisitor` and `AstMutator` provide shared dispatch for printing, cloning, semantic passes, code generation, specialization rewriting, and cache sizing.

The important choice is not “visitor everywhere”; it is one exhaustively checked node vocabulary. Pass-local adapters such as expression-only and statement-only visitors keep invalid node categories obvious, while targeted structural algorithms—return-flow inspection, for example—can still use direct traversal when that is clearer.

Deep clone support is a first-class operation rather than incidental copy construction. The module resolver and compiler cache need isolated AST copies because later phases mutate return annotations and rewrite generic applications.

### Facade outside, state objects inside

The public compiler surface is a single `compileSource()` facade. Internally, the type checker and code generator are split by responsibility:

- declarations and structs;
- expressions and function calls;
- assignment places;
- statements/control flow;
- module construction and builtins.

Each subsystem operates through an explicit environment/session or module/function context. Long-lived declaration metadata is separated from per-check scopes and specialization requests. Codegen separates module registries/globals from per-function scopes and temporary output.

This is a pragmatic middle ground between two extremes: one monolithic pass with implicit mutable state, and a heavyweight pass manager/semantic-IR framework that would exceed the project's needs.

### Semantic registries instead of parallel switch logic

`Builtins.hpp` and `SemanticTables.hpp` centralize metadata shared across phases:

- builtin name, visibility, arity, parameter kinds, return kind, and mismatch style;
- binary/unary operator type rules and LLVM lowering metadata;
- type display/LLVM/mangling data;
- implementation-tag constraints;
- standard ADT identity and hidden ownership operations.

The type checker consumes this metadata for validation, and codegen consumes the same identities for lowering. Adding a builtin or operator still requires implementation work, but its semantic identity is not duplicated as unrelated string chains.

### Places are different from values

An assignable location is not just a string variable name. Noria supports locals, struct fields, array elements, and nested combinations such as `holder.grid[0][1]`.

The parser therefore stores assignment targets as expression trees. Type checking separates `checkPlace()` from `checkRvalue()`, and codegen separates `generatePlace()` from `generateRvalue()`. A place resolves to an address plus type/layout information; an rvalue resolves to a value plus ownership state.

This abstraction made field and index mutation composable. It also gives string indexing a clean read-only rule and lets the checker attach special container-index semantics without hardcoding every assignment shape.

### Declaration collection and fixed-point return inference

Noria does not make source order determine whether a function can be called. The checker first collects struct declarations, then resolves return types, then collects callable signatures and checks concrete bodies.

Optional return annotations make this more involved than a single pass. Unannotated functions are resolved as a fixed point:

1. explicitly typed function families are registered first;
2. the checker attempts each pending unannotated family;
3. a family becomes callable when all of its bodies infer consistently;
4. newly available signatures may unblock forward callers;
5. no-progress recursion produces a located request for an explicit `-> Type`.

All value returns must converge on the same type, bare and value returns cannot mix, and every control-flow path that can complete must contain an explicit return. Loops are treated conservatively as able to terminate—even `while true`—which favors sound, predictable checking over clever reachability proofs.

### Reachable monomorphization as a checked worklist

Generic functions and structs are specialized rather than type-erased. A call unifies concrete argument and expected-result types against template types, checks implementation-tag constraints, and records a specialization request.

Monomorphization then runs a frontier loop:

1. sort requests by deterministic mangled name and source location;
2. deduplicate and clone the requested templates;
3. substitute canonical type arguments and propagate symbol origins;
4. type-check only the newly emitted frontier;
5. enqueue generic calls discovered inside those specializations;
6. rewrite call sites and type applications after the worklist closes;
7. strip generic templates before codegen.

Specialization links detect recursive generic expansion, and hard limits of 64 rounds/total specializations prevent pathological growth from hanging the compiler. Deterministic names such as `id$s.i32` make emitted IR testable and make specializations reusable across different import paths.

Implementation tags participate in the same specialization key as ordinary types. A tagged generic family can provide separate `impl arr`, `impl list`, `impl bst`, or `impl hashmap` bodies while presenting one source-level name. Selection happens entirely at compile time.

### Modules are resolved with provenance, not simple text inclusion

The current module system intentionally supports only bundled `std::` modules, but it still enforces meaningful boundaries:

- imports are selective;
- import cycles and missing modules/exports are rejected;
- `std::internal::*` cannot be imported by user code;
- duplicate or conflicting exports are diagnosed;
- every function and struct retains a module origin;
- private fields and private runtime builtins are checked against that origin.

Resolved declarations are flattened into one module for the later single-module pipeline. `SymbolOrigins` preserves the information that flattening would otherwise erase, including through generated specializations.

### Source standard library with a narrow private ABI

`Sequence`, `Dictionary`, `Set`, and heap algorithms live under `stdlib/` as Noria. This forces the generic, module, privacy, mutation, and specialization systems to support real reusable code.

The compiler exposes raw allocation and typed-buffer primitives only to internal stdlib modules. Public ADTs store an opaque `__rt_ptr` in module-private fields; user code cannot name that pointer type, call the primitives, or construct the opaque structs directly.

This boundary keeps policy in Noria source and mechanism in the runtime:

- ADT algorithms, growth, probing, tree manipulation, and conformance live in Noria;
- allocation, raw pointer arithmetic, typed loads/stores, hashing witnesses, and traps form the minimal trusted base.

The ADT name defines behavior. The implementation tag chooses representation. Code using `Sequence<T, arr>` and `Sequence<T, list>` calls the same public operations even though the asymptotic costs differ.

## Ownership and memory model

Noria is AOT and has no garbage collector. Managed values use value semantics at bindings and explicit ownership behavior at calls/returns.

| Operation | Managed-value behavior |
| --- | --- |
| `let b = a` | Deep clone; `a` and `b` can be dropped independently |
| Function argument | Borrow the caller's value; in-place container/array mutation remains visible |
| Return owned local/temporary | Move ownership to the caller |
| Return borrowed parameter | Clone before returning |
| Reassignment | Drop the previous owned value, then store/move or clone the replacement |
| Scope exit / early return | Drop every still-owned local in exited scopes |

Codegen tracks this with an `owned` bit on generated values and an ownership slot for managed locals/parameters. Parameters begin borrowed. Returns clear a moved local's ownership flag or clone borrowed storage. Drop emission walks scopes in reverse and recursively handles managed array elements and struct fields.

The same model covers:

- heap strings, with immortal literals/default empty strings distinguished by a header tag;
- length-prefixed arrays, including nested arrays and string elements;
- ordinary structs containing managed fields;
- standard ADTs through compiler-requested hidden `clone` and `drop` specializations.

Deep copy is intentionally favored over reference counting. It gives simple, deterministic value semantics and avoids aliasing/double-free hazards at the cost of O(n) copies for managed aggregates. Function borrowing prevents every call from paying that cost.

## Runtime representation and safety

### Strings

Strings are null-terminated byte strings for libc interoperability. Literal globals carry an immortal header marker. Heap strings include a small header before the returned bytes; concatenation and cloning allocate checked storage. `len` maps to `strlen`, indexing loads an unsigned byte, and equality maps to byte-string comparison.

This is compact and makes printing straightforward, but it means `len` is O(n), strings cannot contain embedded nulls as first-class data, and indexing is byte-based rather than Unicode-aware.

### Arrays

An array value points to one allocation:

```text
+0   i64 element_count
+8   element 0
     element 1
     ...
```

Element stride comes from shared type metadata; `[bool]` deliberately uses byte stride even though SSA booleans are LLVM `i1`. Bounds checks zero-extend the signed index and compare it unsigned with length, so negative indexes fail without a separate branch. Nested managed elements are cloned/dropped recursively.

### Structs

Structs lower to named LLVM aggregate types. Fields retain source declaration order even when a literal supplies them in another order. Locals and parameters use stack slots, field access lowers through GEP, and structs are passed/returned by value. Managed fields add recursive clone/drop work.

### Standard ADTs

- Array-backed Sequence uses geometric capacity growth and contiguous indexed storage.
- List-backed Sequence uses a circular sentinel doubly linked list.
- Hashmap uses open addressing, tombstones, and resize at 75% load; expected lookup is O(1).
- BST is intentionally unbalanced; operations are O(h).
- Set reuses Dictionary storage/search logic with a dummy value.
- Heap is expressed over the Sequence interface, making the list implementation's random-access penalty visible rather than special-casing it.

Mixed-size dictionary key/value slots use aligned byte offsets. This avoids the classic error of applying `sizeof(T)`-scaled indexing to a heterogeneous packed layout.

### Stable failures

The compiler rejects invalid constant operations where possible and emits runtime guards when operands are dynamic. Checked cases include:

- integer divide/remainder by zero and `INT_MIN / -1` overflow;
- shift counts outside `0..31`;
- array, string, Sequence, Dictionary, and Set bounds/missing-value misuse;
- allocation/reallocation failure;
- NaN, infinity, or out-of-range `f64 as i32` conversion.

Runtime failures use a stable trap path with exit status 70 and diagnostic text. Platform-specific trap definitions cover macOS and Linux on x86-64 and ARM64.

## Performance engineering

The full compilation test suite doubles as an end-to-end compiler macrobenchmark. This workload is intentionally broader than a parser or single-file microbenchmark: it repeatedly exercises module loading, AST construction, semantic checking, generic specialization, and LLVM IR emission across the accepted and rejected language corpus. Profiling covered 19,600 compilation runs, providing enough repetition to identify reconstruction of reusable AST components as a material cost rather than optimizing from a single trace.

Process-level caching of reusable AST components—parsed stdlib modules and eligible cloned stdlib specializations—produced the following result in the same benchmark environment:

| Full-suite compilation benchmark | Time |
| --- | ---: |
| Before AST-component caching | 27.7s |
| After AST-component caching | 7.1s |
| Improvement | 20.6s / 74.4% reduction / ~3.9× faster |

The absolute times are machine- and environment-specific. The engineering signal is the controlled before/after result on the same real project workload, not a claim that every machine will complete the suite in 7.1 seconds.

### Compile-time work

- **Only reachable generics are emitted.** Unused templates never reach LLVM IR.
- **Specializations are deduplicated.** Canonical mangling makes repeated calls and cross-import requests converge on one emitted body.
- **Requests are sorted.** Deterministic output improves reproducibility and makes IR assertions stable.
- **Stdlib parsing/specialization is cached.** A process-local, mutex-protected LFU cache retains up to 64 parsed modules and 256 specializations.
- **Cache boundaries clone ASTs.** Reuse does not leak mutations from type inference or rewrite phases into later compilations.
- **Admission is selective.** Small function/struct specializations are cheaper to regenerate than retain, so only sufficiently weighted entries enter the specialization cache.
- **The cache uses project data structures.** `LFUCache` uses frequency buckets plus direct key/frequency indexes; those indexes use the repository's contiguous open-addressed `HashTable` with double hashing and tombstones.

The process cache assumes stdlib contents under a given canonical root stay stable during the process lifetime; `clear()` is available for explicit invalidation. That is reasonable for the one-shot CLI and tests, but a long-running language server would need content/mtime-aware keys.

### Generated-code work

- Implementation tags disappear during specialization, so ADT selection adds no runtime dispatch.
- Array Sequence append is amortized O(1); hashmap operations target O(1) average time.
- Scope metadata records whether a scope contains managed pointers, avoiding drop traversal work for scalar-only scopes.
- LLVM optimization is optional. The simple alloca/load/store lowering is readable at `-O0`, while LLVM can promote stack slots and simplify control flow at higher levels.

The main performance tradeoff is deliberate: deep-copy value semantics can be expensive for large managed aggregates. Borrowed parameters and moved owned returns reduce unnecessary copies without introducing reference counts or a borrow checker.

## Testing and quality strategy

CTest combines focused host-language tests with an end-to-end shell harness.

### Focused C++ tests

The 13 unit targets cover canonical types, builtin and semantic registries, AST visitation/cloning, constraints, module resolution, compiler facade stages, diagnostics, generics, the custom hash table, LFU behavior, compiler caching, and semantic tables.

### Language corpus

`tests/run_examples.sh` treats examples as executable specifications:

- every `examples/basic/*.noria` program must emit non-empty LLVM IR;
- every `examples/invalid/*.noria` program must fail semantic analysis;
- every `examples/invalid_syntax/*.noria` program must fail lexing/parsing;
- selected cases are linked and checked for exact exit status or stdout;
- runtime-failure cases assert status 70 and diagnostic text;
- emitted IR is inspected for bounds checks, drops, layouts, mangled specializations, and deduplication;
- safety-sensitive programs are rerun through optimized native builds;
- ADT operations are exercised across every supported implementation tag;
- `noria --help` and stdlib discovery are checked when the compiler is invoked through `PATH` and after `cmake --install`.

ASan/UBSan run through `just sanitize`. Valgrind can wrap compiler invocations and a generated string stress executable. CI builds and tests on both macOS and Ubuntu, requires LLVM tools, uses read-only repository permissions, and cancels superseded runs.

## Notable engineering challenges

### Keeping inference independent of declaration order

Forward calls, recursion, optional return annotations, generic families, and expected-type inference interact. A single left-to-right checker either rejects valid programs or creates implicit order dependencies. The fixed-point inference pass makes progress explicit and gives underconstrained recursive cycles a deterministic escape hatch: add a return annotation.

### Specializing source-written generic ADTs

The standard library creates nested generic calls: a heap specialization calls Sequence operations; Set calls Dictionary internals; a Dictionary specialization calls witness-polymorphic typed-buffer operations. The compiler must carry enclosing type arguments, seed matching callee parameters, preserve module origins, and continue type checking until no new specializations appear.

### Reconciling mutation with value ownership

Container parameters need borrowed handles so `sequence_push(s, x)` mutates caller-visible storage, while `let copy = s` must produce an independently droppable value. The solution separates binding copy semantics from call semantics and generates hidden container clone/drop operations as ordinary monomorphized stdlib requests.

### Making nested assignment composable

Adding arrays and structs changes assignment from “look up a local slot” into recursive address computation. The Place abstraction prevents each feature from inventing its own lvalue path and lets type checking and codegen enforce the same assignability boundary.

### Preserving abstraction across representation choices

The ADT API must behave the same for array/list and BST/hashmap implementations while retaining honest complexity differences. Conformance examples run the same operations across tags; heap intentionally demonstrates how an abstraction can preserve correctness while still exposing representation-dependent performance.

### Managing raw layout without exposing raw pointers

Source-written containers need allocation and typed access, but a public pointer type would undermine the language's safety boundary. Module provenance, private struct fields, internal builtin visibility, and a narrow runtime ABI combine to keep raw operations inside trusted stdlib modules.

## Tradeoffs and alternatives considered

| Decision | Benefit | Cost / accepted limitation |
| --- | --- | --- |
| Textual LLVM IR instead of LLVM C++ API | Small build surface, readable output, easy IR assertions | String construction is less structurally safe; target/tool integration is manual |
| Recursive-descent precedence functions instead of Pratt parsing | Grammar and diagnostics remain explicit for a modest operator set | Adding operators touches the precedence ladder |
| Checked AST directly to LLVM instead of a semantic IR | Fewer layers in a compact compiler | Some type/layout knowledge is revisited in codegen; advanced optimization belongs to LLVM |
| Stack-slot lowering instead of early SSA construction | Simple locals, places, and control-flow joins | Verbose `-O0` IR; relies on LLVM optimization for promotion |
| Monomorphization instead of erasure/boxing | Concrete layouts, no generic dispatch, straightforward constraints | Code-size growth; defensive specialization limits are required |
| Closed implementation tags instead of traits | Predictable selection and finite constraint rules | Users cannot define new tags or overload operations |
| Source stdlib over private primitives | Exercises the language and keeps algorithms inspectable | Compiler/runtime and stdlib still share a trusted ABI contract |
| Deep-copy values + borrowed parameters | Deterministic ownership without GC/reference counting | Copying large aggregates is O(n); aliasing semantics require explanation |
| Flatten imported declarations + preserve origins | Keeps later phases single-module and simple | No separately compiled user modules or package graph yet |
| C strings | Direct libc interoperability and compact runtime | O(n) length, byte indexing, no embedded-null/Unicode model |
| Unbalanced BST and list-backed heap | Keeps alternate representations inspectable and validates abstraction | Worst-case BST O(n); list heap is intentionally inefficient |
| Explicit shell corpus rather than generated test manifests | Test intent and expected diagnostics remain visible | Harness is long and requires manual registration for detailed native assertions |

## Known boundaries and extension points

The current architecture is intentionally not a production compiler framework. There is no user module search path, separate compilation, incremental dependency invalidation, user-defined trait system, semantic IR, general borrow checker, package manager, or debugger metadata. The benchmark is an end-to-end compilation macrobenchmark rather than a granular suite of per-stage or runtime benchmarks.

The existing seams make the likely next work concrete:

- a user module system can build on `ModuleSourceProvider`, cycle detection, and `SymbolOrigins`;
- broader generics can replace closed constraint tables while retaining substitution and worklist specialization;
- a semantic IR can be inserted between checked AST and `LLVMGenerator` if optimization needs outgrow direct lowering;
- content-addressed cache keys can extend the current clone-safe cache for a long-running compiler service;
- the compilation benchmark can be extended with cold/warm-cache splits, per-stage timings, scaling curves, managed-copy costs, and runtime ADT constants;
- richer control flow can reuse lexical scopes, return-flow analysis, and place lowering.

The important constraint is that future features should preserve the current stage invariants and verification style: one canonical semantic definition, located failures, deterministic output, positive and negative language cases, and native behavior checks where code generation is involved.
