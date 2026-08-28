# Noria

Created by Matthew McRae, Noria is a statically typed, ahead-of-time compiled language with a C++20 compiler and a standard library written largely in Noria itself. It compiles source through a hand-built lexer, parser, type checker, generic monomorphizer, and textual LLVM IR backend, then produces native executables on macOS and Linux.

This repository is deliberately scoped as a focused language implementation rather than a production ecosystem. Within that scope it tackles the parts that make compiler work interesting: type and return inference, source modules, generic specialization, representation-independent data structures, ownership-aware code generation, deterministic diagnostics, runtime safety checks, and cross-platform validation.

## Project Highlights

- **A complete compiler, not a transpiler shell.** The pipeline owns lexing, parsing, AST design, semantic analysis, monomorphization, LLVM IR emission, optimization handoff, native linking, and diagnostics.
- **Language features are backed by architecture.** Canonical types, visitor-based AST passes, shared semantic registries, place/rvalue separation, and a compiler facade keep later features from becoming one-off branches.
- **Generics have real compile-time semantics.** Noria infers type arguments, checks tag-specific constraints, emits only reachable concrete specializations, detects recursive specialization cycles, and gives specializations deterministic names.
- **The standard library exercises the language.** `Sequence`, `Dictionary`, `Set`, and heap algorithms are Noria source modules over a small private runtime ABI, rather than C++ containers exposed as builtins.
- **Managed values have defined ownership behavior.** Strings, arrays, structs containing managed fields, and standard-library ADTs are cloned, borrowed, moved, and dropped explicitly by generated code—without a garbage collector.
- **Compiler performance work is measured end to end.** After profiling **19,600 compilation runs**, bounded LFU caching for large, frequently reused AST components reduced the full-suite compilation benchmark from **27.7s to 7.1s**—a **74% reduction** and approximately **3.9× speedup**.
- **The test suite covers failure behavior as a contract.** The current regression suite compiles 255 accepted programs, rejects 142 semantic failures and 22 lexer/parser failures, runs native exit/stdout/trap checks, and includes 13 focused C++ test executables.

For the design rationale behind these choices, see [engineering.md](engineering.md). For the complete language reference and edge cases, see [SYNTAX.md](SYNTAX.md).

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

- The public abstraction is the **ADT name** (`Sequence`, `Dictionary`, or `Set`), not the backing-container name. `arr`, `list`, `bst`, and `hashmap` are compile-time implementation tags: changing a tag changes representation and complexity, not the API.
- The last implementation argument has an ADT-specific default: `Sequence<T>` uses `arr`; `Dictionary<K, V>` and `Set<T>` use `hashmap` (`hashset` is an alias).
- Typed declarations can put the name or type first: `value: i32` and `i32: value` are equivalent.
- `fn`, `helper`, `util`, and `recfn` are equivalent declaration keywords, and a trailing `-> Type` is optional when returns provide enough information.
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

The standard library demonstrates one stable ADT interface over multiple compile-time-selected implementations.

| Module | Abstraction | Implementations and default | Representative operations |
| --- | --- | --- | --- |
| `std::sequence` | `Sequence<T, I>` | `arr`, `list`; default `arr` | `push`, `pop`, `get`, `set`, `insert`, `remove`, `[]`, `+` |
| `std::dictionary` | `Dictionary<K, V, I>` | `bst`, `hashmap`; default `hashmap` | `insert`, `contains`, `get`, `get_or`, `remove`, `[]` |
| `std::set` | `Set<T, I>` | `bst`, `hashmap`/`hashset`; default `hashmap` | `insert`, `contains`, `remove`, membership `[]` |
| `std::heap` | algorithms over `Sequence<T, I>` | inherits the sequence tag | `heappush`, `heappop`, `heapify` |

For example, `Sequence<i32, arr>` provides amortized O(1) append and O(1) indexed access, while `Sequence<i32, list>` provides O(1) append but O(n) indexed access. Both expose the same source-level operations. Dictionary and Set follow the same model: an unbalanced BST offers O(h) operations, while the open-addressed hashmap targets O(1) average lookup and resizes at 75% load.

The implementation tags are erased by monomorphization—there is no runtime branch or virtual dispatch to select a representation.

## Compiler architecture

```text
.noria source
    │
    ├─ Lexer ─────────────── tokens + file/line/column locations
    ├─ Parser ────────────── owned AST with canonical language types
    ├─ ModuleResolver ────── selective stdlib imports + symbol origins
    ├─ ADT defaulting ────── omitted implementation tags become explicit
    ├─ TypeChecker ───────── declarations, inference, constraints, places
    ├─ Monomorphizer ─────── reachable, deduplicated specializations
    ├─ LLVMGenerator ─────── ownership-aware textual LLVM IR
    ├─ opt (optional) ────── LLVM -O1/-O2/-O3 pipeline
    └─ host clang ────────── native executable
```

The public `compileSource()` facade can stop after tokens, AST, typed AST, or LLVM IR. The CLI remains responsible for files, options, optimization, and native linking; compiler stages remain usable directly from C++ tests.

Key directories:

| Path | Responsibility |
| --- | --- |
| `include/noria/` | AST, canonical types, public compiler facade, shared semantic metadata, runtime definitions |
| `src/typecheck/` | declaration collection, fixed-point return inference, expression/place checking, constraints |
| `src/monomorphize/` | specialization discovery, cloning, rewriting, caching, cycle and expansion guards |
| `src/codegen/` | module/function state, expressions, statements, places, builtins, structs, ownership/drop emission |
| `stdlib/` | public Noria ADTs and private implementation/runtime modules |
| `tests/` | focused C++ unit tests and the end-to-end compiler/native-execution harness |
| `examples/` | passing programs, semantic failures, syntax failures, and future design sketches |

Read [engineering.md](engineering.md) for the invariants, design patterns, memory layouts, performance decisions, challenges, and accepted tradeoffs behind this structure.

## Build and run

Requirements:

- CMake 3.20+
- a C++20 compiler
- host `clang` for native executable output
- LLVM `opt` for `-O1` through `-O3` and full CI coverage
- optionally `just`, Valgrind, and Clang sanitizers for the convenience workflows

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

Set `LLVM_BIN` when LLVM tools are not on `PATH`.

## Verification

CTest is the canonical entry point:

```bash
ctest --test-dir build --output-on-failure
```

Or use the repository workflows:

```bash
just test       # build + C++ tests + end-to-end compiler suite
just sanitize   # ASan + UBSan in a separate build directory
just valgrind   # compiler invocations and generated string stress test
```

The end-to-end harness validates more than successful compilation. It checks located diagnostic text, emitted IR patterns, native exit codes and stdout, optimization-sensitive safety behavior, runtime trap status/messages, ownership drops, specialization reuse, and ADT conformance across implementation tags. It also exercises the installed CLI: `noria --help` and stdlib discovery when the compiler is invoked through `PATH` from another directory, including after `cmake --install`. GitHub Actions runs the suite on macOS and Ubuntu with LLVM tooling required.

The same full-suite compilation workload serves as the compiler performance benchmark. Profiling across 19,600 compilation runs identified repeated reconstruction of reusable AST components; bounded LFU caches reduced the recorded workload from 27.7 seconds to 7.1 seconds. The raw timings are environment-specific; the comparable-workload result is a 20.6-second / 74.4% reduction, or roughly 3.9× faster.

## Intentional scope

Noria is an engineering-focused language project, not yet a general-purpose production toolchain. The main current boundaries are:

- only bundled `std::` modules can be imported;
- no `for`, `break`, `continue`, globals, user-defined traits, or implicit conversions;
- no additional integer widths or exponent-form float literals;
- implementation tags and constraints are closed compiler concepts rather than a user-extensible trait system;
- containers support scalar elements/keys/values, not arbitrary nested ADTs or struct elements;
- the BST is currently not self-balancing, and the list-backed heap exposes the cost of poor random access;
- the compiler emits textual LLVM IR instead of using LLVM's C++ API.
