# Noria

Noria is a statically typed, ahead-of-time compiled language created by Matthew McRae. Its C++20 compiler owns the pipeline from source text to LLVM IR, and its standard library is written largely in Noria itself. The CLI can emit inspectable textual LLVM IR or drive LLVM object emission and host linking to produce native executables on macOS and Linux.

This repository is deliberately scoped as a focused language implementation rather than a production ecosystem. Within that scope it tackles the parts that make compiler work interesting: type and return inference, source modules, generic specialization, representation-independent data structures, ownership-aware code generation, deterministic diagnostics, runtime safety checks, and cross-platform validation.

| Document | Use it for |
| --- | --- |
| **This page** | Project scope, current status, build instructions, and a guided code tour |
| [Engineering](ENGINEERING.md) | Pipeline invariants, ownership, runtime layouts, design decisions, and tradeoffs |
| [Performance](PERFORMANCE.md) | Benchmark methodology, optimization attribution, cache design, and measurement limits |
| [Language reference](SYNTAX.md) | Implemented syntax, semantics, standard-library APIs, traps, and limitations |

## Project Highlights

- **A complete compiler, not a transpiler shell.** The pipeline owns lexing, parsing, AST design, module resolution, semantic analysis, monomorphization, LLVM IR emission, optimization handoff, object emission, native linking, and diagnostics.
- **Language features are backed by architecture.** Canonical types, visitor-based AST passes, shared semantic registries, place/rvalue separation, and a compiler facade keep later features from becoming one-off branches.
- **Generics have real compile-time semantics.** Noria infers type arguments, checks tag-specific constraints, emits only reachable concrete specializations, detects recursive specialization cycles, and gives specializations deterministic names.
- **Managed values have defined ownership behavior.** Strings, arrays, structs containing managed fields, and standard-library ADTs are cloned, borrowed, moved, and dropped explicitly by generated code—without a garbage collector.
- **Compiler performance work was driven by phase data.** A controlled in-process workload covering **19,600 compilations** improved from **27.69s to 7.05s** of aggregate compiler phase time—**74.5% less time** or **3.93× faster**—after process-local AST caching, selective cache admission, and frontier-only generic work. The [performance case study](PERFORMANCE.md) separates the effects and records the measurement limits.
- **Failure behavior is part of the contract.** The current regression suite compiles 277 accepted programs, rejects 148 semantic failures and 22 lexer/parser failures, runs native exit/stdout/trap checks, and includes 13 focused C++ test executables.
- **Memory safety and resilience get dedicated workflows.** Compiler and generated-code sanitizers, portable leak checks, deterministic container reference models, and a weekly WIP libFuzzer job exercise risks that success-only examples miss.

## Current snapshot

| Area | Evidence in the current tree                                                                                                                                                                          |
| --- |-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Implementation | Approximately 15.5k lines of C++ compiler/header code and 1.6k lines of Noria standard-library code                                                                                                   |
| Compiler stages | Lexer and parser, module resolution, fixed-point return inference, type checking, reachable monomorphization, ownership-aware LLVM IR generation, optional LLVM optimization, object emission, native linking |
| Validation corpus | 277 accepted programs, 148 semantic failures, and 22 lexer/parser failures; a guard test fails when these documented counts drift                                                                     |
| Focused tests | 13 C++ test executables for types, visitors/cloning, semantic registries, constraints, modules, generics, caches, diagnostics, and the compiler facade                                                |
| End-to-end checks | IR assertions, native exit/stdout/trap behavior, `-O2` regression cases, install/stdlib discovery, sanitizer instrumentation, leak checking, and four checked-in 300-operation container model traces |
| Automation | macOS and Ubuntu CI for normal, sanitizer, and leak lanes; scheduled WIP Clang/libFuzzer coverage of the in-memory compiler facade                                                                    |

`examples/basic`, `examples/invalid`, and `examples/invalid_syntax` are the implemented executable contract. Files under `examples/future` are design sketches and are intentionally excluded from current feature claims and regression counts.

## A taste of Noria

```noria
import std::sequence::{Sequence, sequence_len, sequence_push};

struct Score {
  i32: value; // Type:name and name:Type declarations are both valid.
}

helper sum(values: Sequence<i32>) { // Return type is inferred as i32.
  let i: i32;
  let total: i32; // Defaults to 0.

  while i < sequence_len(values) {
    total = total + values[i];
    i = i + 1;
  }
  return total;
}

fn main() {
  let values: Sequence<i32>; // Empty Sequence<i32, arr> by default.
  sequence_push(values, 10);
  sequence_push(values, 32);
  return sum(values);
}
```

Several choices are intentionally unusual:

- The public abstraction is the **ADT name** (`Sequence`, `Dictionary`, or `Set`), not the backing-container name. `arr`, `list`, `bst`, `hashmap`, and `hashset` are compile-time implementation tags: changing a tag changes representation and complexity, not the API.
- The last implementation argument has an ADT-specific default from the shared container registry: `Sequence<T>` uses `arr`; `Dictionary<K, V>` and `Set<T>` use `hashmap` (`hashset` is an alias for Set).
- Typed declarations can put the name or type first: `value: i32` and `i32: value` are equivalent.
- `fn`, `helper`, `util`, and `recfn` are function declaration keywords used to describe behaviour from the function body in the function header.
- A trailing `-> Type` is optional in function declarations when returns provide enough information, but can still be best practice to include to make function headers more descriptive.
- Top-level structs and functions are collected before bodies are checked, so declarations do not need to be ordered around their uses. Imports are the exception: they must come first.

## Language overview

Noria currently includes:

- `i32`, `f64`, `bool`, `str`, `void`, heap arrays `[T]`, structs, and generic structs/functions;
- explicit or inferred local types, type/name order-independent declarations, recursive calls, and inferred return types;
- arithmetic, comparison, bitwise, short-circuit logical, unary, indexing, field access, and explicit `as` casts;
- `if` / `else if` / `else`, `while`, lexical scopes, and explicit returns on all completing paths;
- module-private struct fields and selective `import std::<path>::{...}` declarations;
- compile-time implementation tags and constraints for generic implementation families;
- builtins for output, math, and length, plus a private stdlib-only allocation/buffer ABI;
- checked bounds, allocation, integer division/remainder, shift, and float-to-integer conversion failures.

Identifiers and keywords are case-insensitive; string contents retain their case. Noria does not perform implicit numeric conversions.

See [SYNTAX.md](SYNTAX.md) for the grammar, precedence, defaults, ownership rules, ADT APIs, diagnostics, and limitations.

## Standard library

The standard library demonstrates one stable ADT interface over multiple compile-time-selected implementations. A shared container registry records each ADT's module, canonical name, full type-argument arity, and default implementation tag; ADT defaulting consults that metadata after import resolution.

| Module | Abstraction | Implementations and default         | Representative operations |
| --- | --- |-------------------------------------| --- |
| `std::sequence` | `Sequence<T, I>` | `arr`, `list`; default `arr`        | `push`, `pop`, `get`, `set`, `insert`, `remove`, `[]`, `+` |
| `std::dictionary` | `Dictionary<K, V, I>` | `bst`, `hashmap`; default `hashmap` | `insert`, `contains`, `get`, `get_or`, `remove`, `[]` |
| `std::set` | `Set<T, I>` | `bst`, `hashmap` (`hashset` alias); default `hashmap` | `insert`, `contains`, `remove`, membership `[]` |
| `std::heap` | algorithms over `Sequence<T, I>` | inherits the sequence tag           | `heappush`, `heappop`, `heapify` |

For example, `Sequence<i32, arr>` provides amortized O(1) append and O(1) indexed access, while `Sequence<i32, list>` provides O(1) append but O(n) indexed access. Both expose the same source-level operations. Dictionary and Set follow the same model: an unbalanced BST offers O(h) operations. `hashmap` and `hashset` are open addressed implementations which targets O(1) average lookup and resize at 75% load.

The implementation tags are erased by monomorphization—there is no runtime branch or virtual dispatch to select a representation.

## Compiler architecture

```text
.noria source
    │
    ├─ Lexer ─────────────── tokens + file/line/column locations
    ├─ Parser ────────────── owned AST with canonical language types
    ├─ ModuleResolver ────── selective stdlib imports + symbol origins
    ├─ ADT defaulting ────── registry expands omitted implementation tags
    ├─ TypeChecker ───────── declarations, inference, constraints, places
    ├─ Monomorphizer ─────── reachable, deduplicated specializations
    ├─ LLVMGenerator ─────── ownership-aware textual LLVM IR
    ├─ opt (optional) ────── LLVM -O1/-O2/-O3 pipeline
    ├─ llc (when found) ──── target object file
    └─ host clang ────────── final link; can consume IR if llc is unavailable
```

The public `compileSource()` facade can stop after tokens, AST, typed AST, or LLVM IR. The CLI remains responsible for files, options, optimization, and native linking; compiler stages remain usable directly from C++ tests.

Key directories:

| Path | Responsibility |
| --- | --- |
| [`include/noria/`](../include/noria/) | AST, canonical types, public compiler facade, shared semantic metadata, runtime definitions |
| [`src/typecheck/`](../src/typecheck/) | `TypeChecker` Pimpl façade; context-owned declarations, scopes, sessions, specialization registry, and focused checking components |
| [`src/monomorphize/`](../src/monomorphize/) | Specialization discovery, cloning, rewriting, caching, cycle and expansion guards |
| [`src/codegen/`](../src/codegen/) | Module/function contexts, expressions, statements, places, builtins, structs, ownership/drop emission |
| [`stdlib/`](../stdlib/) | Public Noria ADTs and private implementation/runtime modules |
| [`tests/`](../tests/) | Focused C++ tests, the end-to-end compiler/native-execution harness, WIP fuzz target, and corpus seeds |
| [`examples/`](../examples/) | Passing programs, semantic failures, syntax failures, and clearly separated future sketches |

## Build and run

Requirements:

- CMake 3.20+
- a C++20 compiler
- host `clang` for the CLI's native-executable mode
- LLVM `opt` for `-O1` through `-O3`
- LLVM `llc` for matched LLVM object emission and the full CI path; native builds fall back to letting host Clang consume IR when `llc` is unavailable
- unoptimized IR emission requires neither LLVM libraries nor external LLVM tools
- optionally `just` for convenience recipes and Valgrind for the Linux leak lane

```bash
cmake -S . -B build
cmake --build build
```

Install the compiler and bundled standard library into a prefix:

```bash
cmake --install build --prefix /usr/local
```

`noria` can then be invoked from `PATH`. It locates `stdlib` next to the executable, at `../stdlib` (in-tree builds), or at `../share/noria/stdlib` (the CMake install layout). Override with `--stdlib <dir>` or `NORIA_STDLIB`.

Emit LLVM IR:

```bash
./build/noria examples/basic/factorial.noria -o build/factorial.ll
```

Build and run a native executable:

```bash
./build/noria build -O2 examples/basic/factorial.noria -o build/factorial
./build/factorial
echo $?
```

Inspect front-end output:

```bash
./build/noria --emit-tokens examples/basic/lexer_smoke.noria
./build/noria --emit-ast examples/basic/factorial.noria
```

Set `LLVM_BIN` to the LLVM tool directory when `opt` and `llc` are not in their default locations. Keeping both tools from the same LLVM installation avoids dialect mismatches between optimized IR and object emission; final linking still uses the host Clang driver so platform SDK and system-library discovery remain native to the host.

## Verification

CTest is the canonical entry point:

```bash
ctest --test-dir build --output-on-failure
```

Or use the repository workflows:

```bash
just test       # build + C++ tests + end-to-end compiler suite (no leak checkers)
just sanitize   # ASan + UBSan for the compiler, plus generated-code ASan on native checks
just leak       # required portable leak checks on the container corpus only
just full       # normal + sanitizer + required leak suites
just valgrind   # wrap compiler invocations under Valgrind
```

| Workflow | What it checks |
| --- | --- |
| `just test` | All 16 CTest entries: the end-to-end corpus, 13 C++ executables, a macOS leak-output classifier test, and a documentation/corpus-count guard |
| `just sanitize` | ASan/UBSan on the compiler and ASan instrumentation of generated LLVM IR before native linking |
| `just leak` | Container-focused leak fixtures using Valgrind when available, otherwise Linux ASan/LSan or macOS `leaks`; fails if no checker can run |

The end-to-end harness validates more than successful compilation. It checks located diagnostics, emitted IR patterns, native exit codes and stdout, stable runtime trap status/messages, ownership drops, specialization reuse, and ADT conformance across implementation tags. A named high-risk manifest reruns ownership and container cases at `-O2`. Container fixtures cover both Sequence implementations, both Dictionary/Set representations, mixed scalar widths, heap-allocated strings, arrays, and heap-over-Sequence. Four generated-but-checked-in reference models each replay a deterministic 300-operation trace against expected state, including clone divergence, resize/tombstone behavior, and alternate representations.

The harness also exercises `noria --help`, stdlib discovery through `PATH` from another directory, the `cmake --install` layout, and the same-LLVM `llc` object path used before host linking. GitHub Actions requires both `opt` and `llc` and runs normal, sanitizer, and required leak lanes on macOS and Ubuntu.

Fuzzing is WIP and is not part of the canonical test suite or a benchmark. A separate scheduled workflow currently runs the Clang/libFuzzer target weekly and uploads crash artifacts on failure.

See [PERFORMANCE.md](PERFORMANCE.md) for the historical 19,600-run measurement. It is documented separately because the repository does not currently ship a benchmark target or enforce performance thresholds in CI.

## Intentional scope

Noria is an engineering-focused language project, not yet a general-purpose production toolchain. The main current boundaries are:

- only bundled `std::` modules can be imported;
- no `for`, `break`, `continue`, globals, user-defined traits, or implicit conversions;
- no additional integer widths or exponent-form float literals;
- implementation tags and constraints are closed compiler concepts rather than a user-extensible trait system;
- containers support scalar elements/keys/values, not arbitrary nested ADTs or struct elements;
- the BST is currently not self-balancing, and the list-backed heap exposes the cost of poor random access;
- the compiler emits textual LLVM IR instead of using LLVM's C++ API.
