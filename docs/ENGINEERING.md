# Engineering Noria

This document explains how Noria is built: the compiler architecture, the invariants carried between stages, the design patterns used to keep features coherent, the performance model, the difficult implementation problems, and the tradeoffs that define the current scope.

It is intentionally about decisions visible in the code. Start with the [project overview](README.md) for build and usage instructions, use the [language reference](SYNTAX.md) for source semantics, and see the [performance case study](PERFORMANCE.md) for measurement details.

## Engineering profile

Noria is an ahead-of-time compiler written in C++20. It owns the language front end and semantic pipeline, emits LLVM IR as text, optionally runs LLVM's optimizer, lowers IR to an object with `llc` when available, and delegates the final native link to the host Clang driver. The standard data-structure library is implemented in Noria source over a deliberately small private runtime ABI; the compiler invokes LLVM tools as subprocesses rather than linking against LLVM libraries.

The current tree contains approximately 15,500 lines of compiler/header code and 1,600 lines of Noria standard-library code. Its validation corpus includes 277 accepted programs and 170 negative programs split between 148 semantic and 22 syntax failures. CTest registers 16 checks: one end-to-end language harness, 13 focused C++ test executables, and two shell contract tests. Those numbers matter less than the coverage shape: compiler stages, generated IR, native behavior, optimizer behavior, diagnostics, traps, ownership, generics, ADT conformance, install layout, and documented corpus counts are all exercised.

### Engineering Highlights

| Engineering concern | Primary implementation | Contract/evidence |
| --- | --- | --- |
| Pipeline composition | [`Compiler.cpp`](../src/Compiler.cpp), [`Compiler.hpp`](../include/noria/Compiler.hpp) | [`compiler_facade_test.cpp`](../tests/compiler_facade_test.cpp) |
| Type rules and inference | [`src/typecheck/`](../src/typecheck/), [`Types.hpp`](../include/noria/Types.hpp), [`SemanticTables.hpp`](../include/noria/SemanticTables.hpp) | Negative corpus plus type, constraint, semantic-table, and generic tests |
| Generic specialization | [`src/monomorphize/`](../src/monomorphize/) | [`generics_test.cpp`](../tests/generics_test.cpp) and IR/dedup assertions in the end-to-end harness |
| Ownership and lowering | [`src/codegen/`](../src/codegen/) | Native copy/move/drop cases, leak fixtures, generated-code ASan, and optimized regression cases |
| Source-written ADTs | [`stdlib/`](../stdlib/) and the private ABI in [`Builtins.hpp`](../include/noria/Builtins.hpp) | Cross-tag conformance cases and deterministic container model traces |
| Compiler throughput | [`CompilerCache.cpp`](../src/CompilerCache.cpp), [`LfuCache.hpp`](../include/noria/LfuCache.hpp), frontier monomorphization | Focused cache tests and the documented historical [performance experiment](PERFORMANCE.md) |
| Robustness | [`run_examples.sh`](../tests/run_examples.sh), [`compile_fuzzer.cpp`](../tests/fuzz/compile_fuzzer.cpp) | Cross-platform CI, sanitizers, leak tools, and WIP weekly fuzzing with crash artifact upload |

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
   │
   ├──► llc ──► target object (when the resolved tool exists)
   ▼
host clang ──► final native link (or direct IR compilation fallback)
```

### Stage boundaries and invariants

| Stage | Primary responsibility | Invariant on success |
| --- | --- | --- |
| Lexer | Normalize identifiers, recognize literals/operators, preserve locations | Every token has file/line/column data; unknown characters fail locally |
| Parser | Build declarations and expression/statement trees with explicit precedence | AST ownership is unique; imports precede declarations; syntax is structurally valid |
| Module resolver | Load bundled `std::` sources, reject cycles/private imports, merge selected exports | Every merged symbol retains its module of origin |
| ADT defaulting | Use the standard-container registry to expand omitted implementation arguments | `Sequence`, `Dictionary`, and `Set` reach semantic analysis with concrete tags |
| Type checker | Collect declarations, infer returns/type arguments, enforce visibility and constraints | Every concrete function has a return type and every expression/place use is valid |
| Monomorphizer | Materialize reachable generic functions/structs and rewrite applications | No generic template reaches code generation; specializations are deterministic and deduplicated |
| Code generator | Lower checked AST, runtime checks, ownership, and layouts to LLVM | Emitted functions have explicit terminators and managed paths carry correct clone/drop behavior |
| CLI/toolchain | File I/O, executable/stdlib discovery, `opt`, `llc`, native linking | Front-end logic remains callable without subprocesses through `compileSource()`; native linking uses an explicit host target triple |

The compiler facade in `include/noria/Compiler.hpp` exposes `StopAfter::Tokens`, `StopAfter::Ast`, `StopAfter::Typed`, and `StopAfter::Ir` checkpoints. This separation is useful in two ways: the CLI is a thin integration layer, and C++ tests can exercise the compiler in memory without writing files or launching the binary.

## Architectural decisions

### One canonical language type

`Type` in `include/noria/Types.hpp` is the shared representation used by the AST, type checker, monomorphizer, and code generator. It is a private `std::variant` whose active alternative defines its `TypeKind`; callers cannot create a type with a mismatched kind and payload.

- scalar kinds carry no payload;
- arrays own one recursive element type through `std::unique_ptr`;
- structs carry a name and concrete type arguments;
- type parameters carry their source name;
- implementation tags carry a closed `ImplementationTag` value.

`Type` remains copyable even though arrays are recursively owned: its explicit copy operations clone nested array elements, so AST and cache clones can normalize their types independently. Typed accessors expose only the payload that matches the active alternative and reject mismatched access.

`LLVMType(const Type&)` is an adapter at the backend boundary. This avoids a common compiler failure mode where parser, checker, and backend maintain subtly different type enums or repeatedly parse type-name strings. Language identity stays independent from physical layout: the type says “struct Point,” while codegen owns field order and LLVM layout.

### Owned AST plus visitor-based passes

AST child nodes use `std::unique_ptr`, making tree ownership explicit and allowing whole modules/functions to be moved through the pipeline. `AstVisitor` and `AstMutator` provide shared dispatch for printing, cloning, semantic passes, code generation, specialization rewriting, and cache sizing.

The important choice is not “visitor everywhere”; it is one exhaustively checked node vocabulary. Pass-local adapters such as expression-only and statement-only visitors keep invalid node categories obvious, while targeted structural algorithms—return-flow inspection, for example—can still use direct traversal when that is clearer.

Deep clone support is a first-class operation rather than incidental copy construction. The module resolver and compiler cache need isolated AST copies because later phases mutate return annotations and rewrite generic applications.

### Thin facades and focused compiler services

The public compiler surface is a single `compileSource()` facade. `TypeChecker` and `LLVMGenerator` follow the same boundary at the stage level: each public header contains the stable stage API plus one `std::unique_ptr<Impl>`. `Impl` is a composition root of direct, non-polymorphic collaborators, not a second stage API. Move construction and assignment move that pointer, so no internal collaborator needs rebinding.

Pimpl is the right boundary here for three reasons. It keeps the include surface small and stable, so clients do not pull visitors, emitter headers, or per-function lowering types. It lets the internal file split grow without changing the public header. And it makes moves a pointer steal instead of rewriting parent back-pointers after relocating heap proxies.

These stages are not a GoF State machine. Checkers and emitters all exist at once and call each other during one check or one `generate()`. Naming those objects “state” produced heap proxies and a service locator, not exclusive modes that transition.

`TypeChecker::Impl` constructs collaborators around a shared `TypeCheckContext`:

- `TypeEnvironment` owns active-module declaration metadata, callable signatures, generic families, struct metadata, and symbol origins;
- `TypeCheckSession` owns per-check transient data such as the current function name;
- `ScopeStack` owns lexical declarations and lookup, and exposes an RAII frame to keep scope exit balanced;
- `SpecializationRegistry` owns registered type arguments and requested function/struct specializations.

Semantic work remains in the component that owns it. `TypeCheckDriver` sequences complete and frontier checks plus return inference; `DeclarationChecker`, `TypeRelations`, `CallChecker`, `ExpressionChecker`, `PlaceChecker`, `StatementChecker`, and `StructChecker` own their respective rules. Recursive expression work is passed explicitly to call and struct operations, rather than routed through a broad parent interface.

The implementation is correspondingly split across `src/typecheck/TypeCheckerDriver.cpp`, `TypeCheckerDeclarations.cpp`, `TypeCheckerCalls.cpp`, `TypeCheckerExpressions.cpp`, `TypeCheckerPlaces.cpp`, `TypeCheckerStatements.cpp`, `TypeCheckerStructs.cpp`, `TypeRelations.cpp`, and `TypeCheckerContext.cpp`. The façade implementation contains lifecycle and public-API forwarding only.

`LLVMGenerator::Impl` owns specialization maps plus `ModuleEmitter`, `BuiltinEmitter`, `ExpressionEmitter`, `PlaceEmitter`, `StatementEmitter`, `StructEmitter`, `MemoryEmitter`, and `OwnershipEmitter`. Recursive expression emission is passed into builtin and struct methods. Module and function contexts are per-call so one `generate()` cannot leak locals, bindings, globals, or counters into another. The façade implementation again contains lifecycle and public-API forwarding only.

### Semantic registries instead of parallel switch logic

`Builtins.hpp` and `SemanticTables.hpp` centralize metadata shared across phases:

- builtin name, visibility, arity, parameter kinds, return kind, and mismatch style;
- binary/unary operator type rules and LLVM lowering metadata;
- type display/LLVM/mangling data;
- implementation-tag constraints;
- standard ADT identity, full type-argument arity, default implementation tags, and hidden ownership operations.

The module resolver uses the same container metadata with imported symbol origins to expand an omitted final implementation tag, so a same-named user type is not defaulted as a standard ADT. The type checker consumes this metadata for validation, and codegen consumes the same identities for lowering. Adding a builtin or operator still requires implementation work, but its semantic identity is not duplicated as unrelated string chains.

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

Codegen tracks this with an `owned` bit on generated values and an ownership slot for managed locals/parameters. Managed locals get unique LLVM names for both the value slot (`%name.slotN`) and the owned flag (`%name.ownedN`), independent of source names reused across sibling scopes. Parameters begin borrowed. Returns clear a moved local's ownership flag or clone borrowed storage. Borrow-mode expression lowering avoids cloning a managed local merely to pass it, while still marking a newly allocated temporary as owned; after the callee or consuming builtin returns, that temporary is released. Drop emission walks scopes in reverse and recursively handles managed array elements and struct fields.

The same model covers:

- heap allocated strings, with immortal literals/default empty strings distinguished by a header tag;
- length-prefixed arrays, including nested arrays and string elements;
- ordinary structs containing managed fields;
- standard ADTs through compiler-requested hidden `clone` and `drop` specializations.

Deep copy is intentionally favored over reference counting. It gives simple, deterministic value semantics and avoids aliasing/double-free hazards at the cost of O(n) copies for managed aggregates. Function borrowing prevents every call from paying that cost.

## Runtime representation and safety

### Strings

Strings are null-terminated byte strings for libc interoperability. Literal globals carry an immortal header marker. Heap allocated strings include a small header before the returned bytes; concatenation and cloning allocate checked storage. `len` maps to `strlen`, indexing loads an unsigned byte, and equality maps to byte-string comparison.

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

A historical in-process macrobenchmark ran 196 Noria inputs for 100 rounds and timed compiler phases through LLVM IR generation. It reduced aggregate phase time from 27.69s to 7.05s (74.5%, about 3.9×) across the full optimization sequence. Importantly, the initial module/specialization cache accounted for a 27% improvement; the final result also includes selective admission and frontier-only generic checking/rewriting. [PERFORMANCE.md](PERFORMANCE.md) records the phase breakdown, methodology, attribution, and limitations. The repository does not currently ship the timing harness or gate CI on performance, so the number is a historical controlled result rather than a claim about current wall time on arbitrary hardware.

### Compile-time work

- **Only reachable generics are emitted.** Unused templates never reach LLVM IR.
- **Specializations are deduplicated.** Canonical mangling makes repeated calls and cross-import requests converge on one emitted body.
- **Requests are sorted.** Deterministic output improves reproducibility and makes IR assertions stable.
- **Stdlib parsing/specialization is cached.** A LFU cache retains up to 64 parsed modules and 256 specializations.
- **Cache boundaries clone ASTs.** Reuse does not leak mutations from type inference or rewrite phases into later compilations.
- **Admission is selective.** Function specializations below a computed 1 KiB AST weight and structs below eight fields are regenerated rather than retained.
- **The cache uses custom data structures.** `LFUCache` uses frequency buckets plus direct key/frequency indexes; those indexes use the repository's contiguous open-addressed `HashTable` with double hashing and tombstones.

The process cache assumes stdlib contents under a given canonical root stay stable during the process lifetime; `clear()` is available for explicit invalidation. That is reasonable for the one-shot CLI and in-process tests, but a long-running language server would need content- or metadata-aware keys.

### Generated-code work

- Implementation tags disappear during specialization, so ADT selection adds no runtime dispatch.
- Array Sequence append is amortized O(1); hashmap operations target O(1) average time.
- Scope metadata records whether a scope contains managed pointers, avoiding drop traversal work for scalar-only scopes.
- LLVM optimization is optional. The simple alloca/load/store lowering is readable at `-O0`, while LLVM can promote stack slots and simplify control flow at higher levels.
- Native builds prefer `llc` from the same LLVM installation used for `opt`, then use the host Clang driver only for final linking. This avoids optimized-IR dialect mismatches without giving up the host SDK and system-library search path; direct Clang consumption of textual IR remains the fallback.

## Testing and quality strategy

CTest combines focused host-language tests with an end-to-end shell harness and two repository-contract checks. The normal configured build currently exposes 16 CTest entries.

### Focused C++ tests

The 13 C++ test executables cover canonical types, builtin and semantic registries, AST visitation/cloning, constraints, module resolution, compiler facade stages, diagnostics, generics, the custom hash table, LFU behavior, compiler caching, and semantic tables. Two additional shell checks validate macOS `leaks` output classification and fail when the checked-in corpus counts drift from the claims in this documentation.

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
- container leak programs cover every supported Sequence/Dictionary/Set backing tag plus representative scalar layouts, mixed key/value widths, heap-over-Sequence, `[T]`, and heap-allocated strings created by concatenation;
- checked-in reference-model fixtures (`container_model_*.noria`) replay 300 deterministic operations against Python oracles for Sequence, Dictionary, Set, and heap;
- a named high-risk `-O2` manifest re-runs ownership and container programs after optimization;
- `noria --help` and stdlib discovery are checked when the compiler is invoked through `PATH` and after `cmake --install`.

ASan/UBSan run through `just sanitize`, which also sets `NORIA_NATIVE_ASAN=1` so generated LLVM IR is instrumented before native link. Portable leak checks (`run_leak_check`) run only when `NORIA_RUN_LEAK_CHECKS=1` (via `just leak`, which also sets `NORIA_REQUIRE_LEAK_CHECKS=1`) with Valgrind when present, otherwise Linux ASan/LSan or macOS `/usr/bin/leaks`. Ordinary `just test` and `just sanitize` skip leak checkers so the expensive leak corpus has one explicit lane. `just valgrind` can also wrap all compiler invocations under Valgrind.

### Fuzzing (WIP)

`noria_compile_fuzzer` sends arbitrary byte strings through `compileSource(..., StopAfter::Ir)` and clears the process cache between inputs. Located `CompileError` failures are expected for invalid programs; unexpected standard or non-standard exceptions cause a fuzzer finding. The target is built with Clang libFuzzer and ASan and has a small seed corpus spanning valid, generic, container, ownership, lexer-invalid, and parser-invalid inputs.

Fuzzing is WIP, not part of the canonical test suite, and not a benchmark. It is currently exercised only by a separate scheduled Ubuntu workflow, which runs the compile fuzzer weekly for 120 seconds and uploads crash artifacts when the job fails.

The main CI matrix builds and tests on macOS and Ubuntu with both `opt` and `llc` required. Both jobs run the normal, sanitizer, and required leak lanes; Ubuntu installs Valgrind and macOS falls back to `/usr/bin/leaks`. Jobs have read-only repository permissions, a 45-minute timeout, and cancellation for superseded runs.

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
