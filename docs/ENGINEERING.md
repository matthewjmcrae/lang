# Engineering Noria

This document explains how Noria is built: the compiler architecture, the invariants carried between stages, the design patterns used to keep features coherent, the performance model, the difficult implementation problems, and the tradeoffs that define the current scope.


## Engineering profile

Noria is a statically typed language with a compiler written in C++20. The compiler owns the language front end and semantic pipeline, emits LLVM IR as text, optionally runs LLVM's optimizer, lowers IR to an object with `llc` when available, and delegates the final native link to the host Clang driver. The compiler invokes LLVM tools as subprocesses rather than linking against LLVM libraries.


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

- scalar kinds carry no payload
- arrays own one recursive element type through `std::unique_ptr`
- structs carry a name and concrete type arguments
- type parameters carry their source name
- implementation tags carry a closed `ImplementationTag` value

`Type` remains copyable even though arrays are recursively owned: its explicit copy operations clone nested array elements, so AST and cache clones can normalize their types independently. Typed accessors expose only the payload that matches the active alternative and reject mismatched access.

`LLVMType(const Type&)` is an adapter at the backend boundary. This avoids a common compiler failure mode where parser, checker, and backend maintain subtly different type enums or repeatedly parse type-name strings. Language identity stays independent from physical layout: the type says “struct Point,” while codegen owns field order and LLVM layout.

### Owned AST plus visitor-based passes

AST child nodes use `std::unique_ptr`, making tree ownership explicit and allowing whole modules/functions to be moved through the pipeline. `AstVisitor` and `AstMutator` provide shared dispatch for printing, cloning, semantic passes, code generation, specialization rewriting, and cache sizing.

The important choice is not “visitor everywhere”; it is one exhaustively checked node vocabulary. Pass-local adapters such as expression-only and statement-only visitors keep invalid node categories obvious, while targeted structural algorithms, return-flow inspection, for example, can still use direct traversal when that is clearer.

Deep clone support is a first-class operation rather than incidental copy construction. The module resolver and compiler cache need isolated AST copies because later phases mutate return annotations and rewrite generic applications.

### Facades and focused compiler services

The public compiler surface is a single `compileSource()` facade. `TypeChecker` and `LLVMGenerator` follow the same boundary at the stage level: each public header contains the stable stage API plus one `std::unique_ptr<Impl>`. `Impl` is a composition root of direct, non-polymorphic collaborators, not a second stage API. Move construction and assignment move that pointer, so no internal collaborator needs rebinding.

Pimpl is the right boundary here for two main reasons:
1. It keeps the include surface small and stable, so clients do not pull visitors, emitter headers, or per-function lowering types. 
2. It lets the internal file split grow without changing the public header. 


`TypeChecker::Impl` constructs collaborators around a shared `TypeCheckContext`:

- `TypeEnvironment` owns active-module declaration metadata, callable signatures, generic families, struct metadata, and symbol origins
- `TypeCheckSession` owns per-check transient data such as the current function name
- `ScopeStack` owns lexical declarations and lookup, and exposes an RAII frame to keep scope exit balanced
- `SpecializationRegistry` owns registered type arguments and requested function/struct specializations

Semantic work remains in the component that owns it. `TypeCheckDriver` sequences complete and frontier checks plus return inference; `DeclarationChecker`, `TypeRelations`, `CallChecker`, `ExpressionChecker`, `PlaceChecker`, `StatementChecker`, and `StructChecker` own their respective rules. Recursive expression work is passed explicitly to call and struct operations, rather than routed through a broad parent interface.

The implementation is correspondingly split across `src/typecheck/TypeCheckerDriver.cpp`, `TypeCheckerDeclarations.cpp`, `TypeCheckerCalls.cpp`, `TypeCheckerExpressions.cpp`, `TypeCheckerPlaces.cpp`, `TypeCheckerStatements.cpp`, `TypeCheckerStructs.cpp`, `TypeRelations.cpp`, and `TypeCheckerContext.cpp`. The façade implementation contains lifecycle and public-API forwarding only.

`LLVMGenerator::Impl` owns specialization maps plus `ModuleEmitter`, `BuiltinEmitter`, `ExpressionEmitter`, `PlaceEmitter`, `StatementEmitter`, `StructEmitter`, `MemoryEmitter`, and `OwnershipEmitter`. Recursive expression emission is passed into builtin and struct methods. Module and function contexts are per-call so one `generate()` cannot leak locals, bindings, globals, or counters into another. The façade implementation again contains lifecycle and public-API forwarding only.

### Semantic registries instead of parallel switch logic

`Builtins.hpp` and `SemanticTables.hpp` centralize metadata shared across phases:

- builtin name, visibility, arity, parameter kinds, return kind, and mismatch style
- binary/unary operator type rules and LLVM lowering metadata
- type display/LLVM/mangling data
- implementation-tag constraints
- standard ADT identity, full type-argument arity, default implementation tags, and hidden ownership operations

The module resolver uses the same container metadata with imported symbol origins to expand an omitted final implementation tag, so a same-named user type is not defaulted as a standard ADT. The type checker consumes this metadata for validation, and codegen consumes the same identities for lowering. Adding a builtin or operator still requires implementation work, but its semantic identity is not duplicated as unrelated string chains.

### Places are different from values

An assignable location is not just a string variable name. Noria supports locals, struct fields, array elements, and nested combinations such as `holder.grid[0][1]`.

The parser therefore stores assignment targets as expression trees. Type checking separates `checkPlace()` from `checkRvalue()`, and codegen separates `generatePlace()` from `generateRvalue()`. A place resolves to an address plus type/layout information; an rvalue resolves to a value plus ownership state.

This abstraction made field and index mutation composable. It also gives string indexing a clean read-only rule and lets the checker attach special container-index semantics without hardcoding every assignment shape.

### Declaration collection and fixed-point return inference

Noria does not make source order determine whether a function can be called. The checker first collects struct declarations, then resolves return types, then collects callable signatures and checks concrete bodies.

Optional return annotations make this more involved than a single pass. Unannotated functions are resolved as a fixed point:

1. explicitly typed function families are registered first
2. the checker attempts each pending unannotated family
3. a family becomes callable when all of its bodies infer consistently
4. newly available signatures may unblock forward callers
5. no-progress recursion produces a located request for an explicit `-> Type`

All value returns must converge on the same type, bare and value returns cannot mix, and every control-flow path that can complete must contain an explicit return. Loops are treated conservatively as able to terminate, even `while true`, which favors sound, predictable checking over clever reachability proofs.

### Reachable monomorphization as a checked worklist

Generic functions and structs are specialized rather than type-erased. A call unifies concrete argument and expected-result types against template types, checks implementation-tag constraints, and records a specialization request.

Monomorphization then runs a frontier loop:

1. sort requests by deterministic mangled name and source location
2. deduplicate and clone the requested templates
3. substitute canonical type arguments and propagate symbol origins
4. type-check only the newly emitted frontier
5. enqueue generic calls discovered inside those specializations
6. rewrite call sites and type applications after the worklist closes
7. strip generic templates before codegen

Specialization links detect recursive generic expansion, and hard limits of 64 rounds/total specializations prevent pathological growth from hanging the compiler. Deterministic names such as `id$s.i32` make emitted IR testable and make specializations reusable across different import paths.

Implementation tags participate in the same specialization key as ordinary types. A tagged generic family can provide separate `impl arr`, `impl list`, `impl bst`, or `impl hashmap` bodies while presenting one source-level name. Selection happens entirely at compile time.

### Modules are resolved with provenance, not simple text inclusion

The current module system intentionally supports only bundled `std::` modules, but it still enforces meaningful boundaries:

- imports are selective
- import cycles and missing modules/exports are rejected
- `std::internal::*` cannot be imported by user code
- duplicate or conflicting exports are diagnosed
- every function and struct retains a module origin
- private fields and private runtime builtins are checked against that origin

Resolved declarations are flattened into one module for the later single-module pipeline. `SymbolOrigins` preserves the information that flattening would otherwise erase, including through generated specializations.

### Source standard library with a narrow private ABI

`Sequence`, `Dictionary`, `Set`, and heap algorithms live under `stdlib/` as Noria. This forces the generic, module, privacy, mutation, and specialization systems to support real reusable code.

The compiler exposes raw allocation and typed-buffer primitives only to internal stdlib modules. Public ADTs store an opaque `__rt_ptr` in module-private fields; user code cannot name that pointer type, call the primitives, or construct the opaque structs directly.

This boundary keeps policy in Noria source and mechanism in the runtime:

- ADT algorithms, growth, probing, tree manipulation, and conformance live in Noria;
- allocation, raw pointer arithmetic, typed loads/stores, hashing witnesses, and traps form the minimal trusted base.

The ADT name defines behavior. The implementation tag chooses representation. Code using `Sequence<T, arr>` and `Sequence<T, list>` calls the same public operations even though the asymptotic costs differ.

## Ownership and memory model

Noria has no garbage collector. Managed values use value semantics at bindings and explicit ownership behavior at calls/returns.

| Operation | Managed-value behavior |
| --- | --- |
| `let b = a` | Deep clone; `a` and `b` can be dropped independently |
| Ordinary struct argument | Copy the aggregate and recursively clone managed fields; the callee owns an independent parameter value |
| Direct `str`, `[T]`, or standard-ADT argument | Borrow the caller's storage; in-place container/array mutation remains visible |
| Return owned local/temporary | Move ownership to the caller |
| Return borrowed direct managed parameter | Clone before returning |
| Reassignment of a local, field, or index | Drop the previous occupant, then store/move or clone the replacement |
| Scope exit / early return | Drop every still-owned local in exited scopes |
| Default-initialized managed local | Construct the default; mark owned when `typeNeedsDrop` |
| Temporary used by `print`/`len`, a call, an index, or a field read | Clone a managed result if needed, then `emitReleaseIfOwned` the temporary |
| `+` on `str`, `[T]`, or Sequence | New owned result; `emitReleaseIfOwned` both operands |
| Container index `Get` of `str` | Independent clone (`__rt_load`); owned |
| Container index `Get` of `[T]` | Borrow into the container; not owned |

Codegen tracks this with an `owned` bit on generated values and an ownership slot for managed locals/parameters. `OwnershipEmitter` is the single place that decides whether a type needs a drop (`str`, `[T]`, standard ADTs, and structs that contain those), clones, drops a value, assigns into a place, or releases a temporary. `emitDefaultValue` sets `owned` from `typeNeedsDrop` on every path. Managed locals get unique LLVM names for both the value slot (`%name.slotN`) and the owned flag (`%name.ownedN`), independent of source names reused across sibling scopes. Direct `str`, array, and standard-ADT parameters begin borrowed. Ordinary struct parameters are aggregate copies; when they contain managed data, the callee recursively clones their fields and owns the resulting parameter value. Returns clear a moved local's ownership flag or clone borrowed storage. Borrow-mode expression lowering avoids cloning a direct managed local merely to pass it, while still marking a newly allocated temporary as owned; the ordinary-struct callee prologue establishes the independent copy required by the language contract. After the callee or consuming builtin returns, an owned temporary argument is released. `emitAssignPlace` is the only assignment store for managed types: it drops the occupant (an `ownedSlot` when present, otherwise load-and-drop) before storing. Index and field reads clone a managed result when the mode is Own or the base is owned, then release the base. Drop emission walks scopes in reverse and recursively handles managed array elements and struct fields. The language-facing table is in [SYNTAX.md](SYNTAX.md#ownership); residuals and the leak class are in [Closing forgotten drops on independent heap values](#closing-forgotten-drops-on-independent-heap-values).

The same model covers:

- heap allocated strings, with immortal literals/default empty strings distinguished by a header tag
- length-prefixed arrays, including nested arrays and string elements
- ordinary structs containing managed fields
- standard ADTs through compiler-requested hidden `clone` and `drop` specializations

Deep copy is intentionally favored over reference counting. It gives simple, deterministic value semantics and avoids aliasing/double-free hazards at the cost of O(n) copies for managed aggregates. Direct strings, arrays, and standard ADTs are borrowed at calls to preserve their established call semantics; ordinary struct calls pay the recursive copy cost needed to isolate the callee from the caller.

## Runtime representation and safety

### Strings

Strings are null-terminated byte strings for libc interoperability. Literal globals carry an immortal header marker. Heap allocated strings include a small header before the returned bytes; concatenation and cloning allocate checked storage. `len` maps to `strlen`, indexing loads an unsigned byte, and equality maps to byte-string comparison. Concatenation returns `owned=true` and releases owned operands; `len`, `print`, and string index then `emitReleaseIfOwned` a temporary base after copying the result.

This is compact and makes printing straightforward, but it means `len` is O(n), strings cannot contain embedded nulls as first-class data, and indexing is byte-based rather than Unicode-aware.

### Arrays

An array value points to one allocation:

```text
+0   i64 element_count
+8   element 0
     element 1
     ...
```

Element stride comes from shared type metadata; `[bool]` deliberately uses byte stride even though SSA booleans are LLVM `i1`. Bounds checks zero-extend the signed index and compare it unsigned with length, so negative indexes fail without a separate branch. Nested managed elements are cloned/dropped recursively. Empty defaults and array `+` results are owned; index of an owned temporary clones a managed element if needed and then drops the base.

### Structs

Structs lower to named LLVM aggregate types. Fields retain source declaration order even when a literal supplies them in another order. Locals and parameters use stack slots, field access lowers through GEP, and ordinary structs are passed/returned by value. An ordinary struct parameter containing managed data is recursively cloned into callee-owned storage before its body executes; nested structs, arrays and their managed elements, strings, and standard-ADT fields participate in that clone. Callee field assignment therefore drops only the callee's occupant. Default-initialized structs with managed fields are marked owned. Field assignment goes through `emitAssignPlace`. Field reads of an owned temporary clone a managed result and then drop the aggregate. `Sequence`, `Dictionary`, and `Set` are represented as structs but retain borrowed semantics when passed directly to their operations.

### Standard ADTs

- Array-backed Sequence uses geometric capacity growth and contiguous indexed storage
- List-backed Sequence uses a circular sentinel doubly linked list
- Hashmap uses open addressing, tombstones, and resize at 75% load; expected lookup is O(1)
- BST is intentionally unbalanced; operations are O(h)
- Set reuses Dictionary storage/search logic with a dummy value
- Heap is expressed over the Sequence interface, making the list implementation's random-access penalty visible rather than special-casing it

Mixed-size dictionary key/value slots use aligned byte offsets. This avoids the classic error of applying `sizeof(T)`-scaled indexing to a heterogeneous packed layout.

### Stable failures

The compiler rejects invalid constant operations where possible and emits runtime guards when operands are dynamic. Checked cases include:

- integer divide/remainder by zero and `INT_MIN / -1` overflow
- shift counts outside `0..31`
- array, string, Sequence, Dictionary, and Set bounds/missing-value misuse
- allocation/reallocation failure
- NaN, infinity, or out-of-range `f64 as i32` conversion

Runtime failures use a stable trap path with exit status 70 and diagnostic text. Platform-specific trap definitions cover macOS and Linux on x86-64 and ARM64.

## Performance engineering

A historical in-process macrobenchmark ran 196 Noria inputs for 100 rounds and timed compiler phases through LLVM IR generation. It reduced aggregate phase time from 27.69s to 7.05s (74.5%, about 3.9×) across the full optimization sequence. Importantly, the initial module/specialization cache accounted for a 27% improvement; the final result also includes selective admission and frontier-only generic checking/rewriting. [PERFORMANCE.md](PERFORMANCE.md) records the phase breakdown, methodology, attribution, and limitations. The repository does not currently ship the timing harness or gate CI on performance, so the number is a historical controlled result rather than a claim about current wall time on arbitrary hardware.

### Compile-time work

- **Only reachable generics are emitted.** Unused templates never reach LLVM IR
- **Specializations are deduplicated.** Canonical mangling makes repeated calls and cross-import requests converge on one emitted body
- **Requests are sorted.** Deterministic output improves reproducibility and makes IR assertions stable
- **Stdlib parsing/specialization is cached.** A LFU cache retains up to 64 parsed modules and 256 specializations
- **Cache boundaries clone ASTs.** Reuse does not leak mutations from type inference or rewrite phases into later compilations
- **Admission is selective.** Function specializations below a computed 1 KiB AST weight and structs below eight fields are regenerated rather than retained
- **The cache uses custom data structures.** `LFUCache` uses frequency buckets plus direct key/frequency indexes; those indexes use the repository's contiguous open-addressed `HashTable` with double hashing and tombstones

The process cache assumes stdlib contents under a given canonical root stay stable during the process lifetime; `clear()` is available for explicit invalidation. That is reasonable for the one-shot CLI and in-process tests, but a long-running language server would need content- or metadata-aware keys.

## Testing and quality strategy

CTest combines focused host-language tests with an end-to-end shell harness and two repository-contract checks. The normal configured build currently exposes 16 CTest entries.

### Focused C++ tests

The 13 C++ test executables cover canonical types, builtin and semantic registries, AST visitation/cloning, constraints, module resolution, compiler facade stages, diagnostics, generics, the custom hash table, LFU behavior, compiler caching, and semantic tables. Two additional shell checks validate macOS `leaks` output classification and fail when the checked-in corpus counts drift from the claims in this documentation.

### Language corpus

The validation corpus includes 277 accepted programs and 170 negative programs (148 semantic failures and 22 lexer/parser failures). `tests/run_examples.sh` treats those examples as executable specifications:

- every `examples/basic/*.noria` program must emit non-empty LLVM IR
- every `examples/invalid/*.noria` program must fail semantic analysis
- every `examples/invalid_syntax/*.noria` program must fail lexing/parsing
- selected cases are linked and checked for exact exit status or stdout
- runtime-failure cases assert status 70 and diagnostic text
- emitted IR is inspected for bounds checks, drops, layouts, mangled specializations, and deduplication
- safety-sensitive programs are rerun through optimized native builds
- ADT operations are exercised across every supported implementation tag
- container leak programs cover every supported Sequence/Dictionary/Set backing tag plus representative scalar layouts, mixed key/value widths, heap-over-Sequence, `[T]`, and heap-allocated strings created by concatenation
- checked-in reference-model fixtures (`container_model_*.noria`) replay 300 deterministic operations against Python oracles for Sequence, Dictionary, Set, and heap
- a named high-risk `-O2` manifest re-runs ownership and container programs after optimization
- `noria --help` and stdlib discovery are checked when the compiler is invoked through `PATH` and after `cmake --install`

ASan/UBSan run through `just sanitize`, which also sets `NORIA_NATIVE_ASAN=1`. Linux instruments generated IR with LLVM ASan passes, clang-links that IR, and runs generated natives with `detect_leaks=1`. Darwin one-step compiles the original IR with Apple clang `-fsanitize=address -c` (Homebrew `opt` IR that Apple clang cannot parse falls back to `llc` without ASan hooks) and sets `detect_leaks=0` because Apple's ASan has no usable LSan. Portable leak checks (`run_leak_check`) run only when `NORIA_RUN_LEAK_CHECKS=1` (via `just leak`, which also sets `NORIA_REQUIRE_LEAK_CHECKS=1`) with Valgrind when present, otherwise Linux ASan/LSan or macOS `/usr/bin/leaks`. Ordinary `just test` and `just sanitize` skip leak checkers so the expensive leak corpus has one explicit lane. `just valgrind` can also wrap all compiler invocations under Valgrind. Ubuntu LSan on generated natives is therefore the leak gate for forgotten drops; Darwin generated-code ASan is for use-after-free and overflow.

## Notable engineering challenges

### Specializing source-written generic ADTs

The standard library creates nested generic calls: a heap specialization calls Sequence operations; Set calls Dictionary internals; a Dictionary specialization calls witness-polymorphic typed-buffer operations. The compiler must carry enclosing type arguments, seed matching callee parameters, preserve module origins, and continue type checking until no new specializations appear.

### Reconciling mutation with value ownership

Container parameters need borrowed handles so `sequence_push(s, x)` mutates caller-visible storage, while `let copy = s` must produce an independently droppable value. The solution separates binding copy semantics from call semantics and generates hidden container clone/drop operations as ordinary monomorphized stdlib requests. 

### Managing raw layout without exposing raw pointers

Source-written containers need allocation and typed access, but a public pointer type would undermine the language's safety boundary. Module provenance, private struct fields, internal builtin visibility, and a narrow runtime ABI combine to keep raw operations inside trusted stdlib modules.

### Closing forgotten drops on independent heap values

Noria has no garbage collector. Every independent heap object the compiler creates, empty or nonempty `[T]`, a heap `str` from concat or `clone_str`, a Sequence/Dictionary/Set handle, or a struct that owns any of those, must be dropped exactly once. The language rule is simple; the lowering is not. Allocation is scattered across default values, literals, concat, collection `+`, clones, stdlib `New`/`Get`, and struct/array construction. Consumption is equally scattered: named locals, field and element places, projections (`.field`, `[i]`), builtins (`len`, `print`), user calls, comparisons, and scope exit. A path that mallocs and then leaves `Value.owned == false` is invisible to `emitReleaseIfOwned` and to `emitDropLocal` (which also no-ops when a place has no `ownedSlot`). Ubuntu ASan runs generated natives with `detect_leaks=1`; Darwin sets `detect_leaks=0` because Apple's ASan has no usable LSan. Ordinary `just test` does not set `NORIA_NATIVE_ASAN`. So a missing drop is a real bug on every OS and a red CI job only on Linux sanitize, and only after earlier leaks in the same harness phase are gone.

The architecture that has to hold together is the one in [Ownership and memory model](#ownership-and-memory-model):

- `typeNeedsDrop` is the recursive predicate: `str`, `[T]`, standard containers, and any struct containing those.
- A generated `Value` is either a borrow (`owned=false`) or an independent heap object (`owned=true`). Immortal string literals use a negative header tag; `drop_str` is a no-op on them, so marking a default `""` owned is safe.
- Named managed locals have an `ownedSlot`. Field and array-element places do not; the aggregate owns the occupant.
- Direct `str`, array, and standard-ADT parameters are borrowed. Ordinary struct parameters are independent aggregate copies and own recursive clones of their managed fields.
- Borrow-mode rvalues avoid cloning a local just to read it. Own mode, or a projection out of an owned temporary, must clone a managed result before the aggregate is dropped.
- `__rt_load` clones only `str`. Index `Get` of `str` is therefore an independent heap value; index `Get` of `[T]` is a borrow into the container.

The failures that showed up, and the ones the same rule predicted, were one class: **independent heap, then forgotten drop**.

| Hole | What allocated | Why it leaked |
| --- | --- | --- |
| `print` / `len` of a concat temp | heap `str` | Borrow-eval, then no `emitReleaseIfOwned` |
| `("xy" + "z")[2]` | heap `str` | Index copied a byte and discarded the owned base |
| `holder.nested = []` | empty `[T]` header | Field place had no `ownedSlot`; assignment used a raw store |
| `(Holder { nested: [] }).nested` | empty `[T]` in a temp struct | Field read never dropped the owned aggregate |
| `payload: Payload` / `wrapper: Wrapper` | empty `[T]` fields | `emitDefaultValue` returned the struct `owned=false`; let default stored that flag |
| `let numbers: Sequence<i32>` (and Set/Dictionary) | container `New` handle | `emitStandardContainerCall` always returned `owned=false` |
| Sequence `+` | new handle and payload | Result unowned; operands not released (array `+` already did both) |
| `s[i]` / `d[k]` of `str` | `clone_str` inside `__rt_load` | Function-call `sequence_get` was marked owned; index `Get` was not |

The overarching fix is one rule, centralized rather than another per-site `if`:

1. **If this `Value` is an independent heap object, it is `owned=true`.** `emitDefaultValue` sets `owned` from `typeNeedsDrop` on every path. Let-without-initializer uses the same claim as let-with-initializer. Sequence `+` returns owned and `emitReleaseIfOwned`s both operands. `New`/`Clone` are owned. Index `Get` is owned only when the return type is `str`.
2. **A managed place owns its occupant.** `emitAssignPlace` is the only assignment store: drop the current value (`ownedSlot` gated, otherwise load-and-drop), clone an unowned rvalue, then store. No raw `emitStore` fallback for managed types.
3. **A projection out of an owned aggregate drops the aggregate after the result is independently owned.** Index and field reads clone a managed result when the mode is Own or the base is owned, then `emitReleaseIfOwned(base)`. Identifier bases stay unowned, so `len(holder.nested)` does not drop the local.

That is not a proof of ownership soundness. Left closed only if a later sanitizer names them: `New` samples that are themselves `[T]`; `sequence_get` of `[T]` as a `Call` (marked owned even though `__rt_load` does not clone arrays, use-after-free, the opposite flavour); index-assign of `[T]` where `store_at` does not clone and then `emitReleaseIfOwned` frees the buffer still in the container; `__rt_alloc` raw pointers. The leak gate remains Ubuntu LSan on generated natives, plus IR assertions that owned slots are `true` and that overwritten or temporary headers are `free`d.
