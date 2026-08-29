# Noria language reference

This document is the reference for Noria's implemented syntax and semantics. It includes the language's deliberate quirks, generic ADTs, ownership behavior, runtime traps, and current limitations. For project status and build instructions, start with the [README](README.md); for compiler internals and design rationale, see [ENGINEERING.md](ENGINEERING.md); for the optimization case study, see [PERFORMANCE.md](PERFORMANCE.md).

The executable language contract is the checked-in corpus under `examples/basic`, `examples/invalid`, and `examples/invalid_syntax`. Files under `examples/future` are design sketches, are not included in regression counts, and must not be read as implemented syntax.

## Syntax that is intentionally different

These rules are easy to miss if you approach Noria from C++, Rust, or TypeScript:

| Choice | Noria rule                                                                                                                                                                                                                                                                                                                                            | Example |
| --- |-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------| --- |
| **The ADT is named, not the backing container** | `Sequence`, `Dictionary`, and `Set` are the public abstractions. `arr`, `list`, `bst`, `hashmap`, and `hashset` are compile-time implementation tags, never standalone runtime types.                                                                                                                                                                 | `Sequence<i32, list>` is still a `Sequence`, with the Sequence API. |
| **ADT implementation defaults** | An omitted final tag expands before type checking: `Sequence<T>` defaults to `arr`; `Dictionary<K, V>` and `Set<T>` default to `hashmap`. The shared standard-container registry defines these defaults by imported module, canonical ADT name, and full arity. `hashset` aliases `hashmap` and is considered idiomatic for Set implementations. | `let seen: Set<str>;` creates an empty hashmap-backed set. |
| **Default initialization is real initialization** | A typed declaration may omit `= expr`; the compiler constructs the type's default value rather than leaving uninitialized storage.                                                                                                                                                                                                                    | `let n: i32;`, `let text: str;`, `let values: [i32];` |
| **Trailing return types are optional** | Omitting `-> Type` asks the checker to infer one type from all returns. Recursive or otherwise underconstrained functions need an annotation. Annotations are recommended for functions frequently called by other parts of code so that developers can infer the return type from the function header, but can be omitted for internal helper logic. | `fn answer() { return 42; }` infers `i32`. |
| **Function keywords are aliases** | `fn`, `util`, `helper`, and `recfn` lex as the same declaration. The spelling communicates intent but has no semantic effect. It is advised to annotate function headers using these keywords to increase code readability.                                                                                                                           | `recfn factorial(...) -> i32 { ... }` |
| **Type/name order is independent** | Parameters, struct fields, and typed locals accept either `name: Type` or `Type: name`.                                                                                                                                                                                                                                                               | `left: i32` and `i32: left` are equivalent. |
| **Top-level declaration order is independent** | Struct and function declarations are collected before bodies are checked, enabling forward calls and mutual references where the types can be resolved. Imports alone must precede declarations.                                                                                                                                                      | `main` may call a later declared helper. |
| **Names are case-insensitive** | The lexer lowercases identifiers and keywords. String contents retain their case.                                                                                                                                                                                                                                                                     | `Main`, `main`, and `MAIN` are the same identifier. |
| **Returns are explicit on every completing path** | Inferred return types do not imply an implicit final expression. Value functions use `return expr;`; void functions use `return;`.                                                                                                                                                                                                                    | Both branches of an exhaustive `if` may return. |

### Default values

| Type | Default                           |
| --- |-----------------------------------|
| `i32` | `0`                               |
| `f64` | `0.0`                             |
| `bool` | `false`                           |
| `str` | immortal empty string `""`        |
| `[T]` | allocated, length-zero array      |
| ordinary struct | field-wise defaults               |
| `Sequence<T>` | empty `Sequence<T, arr>`          |
| `Dictionary<K, V>` | empty `Dictionary<K, V, hashmap>` |
| `Set<T>` | empty `Set<T, hashmap>`           |

`void`, raw runtime pointers, and implementation tags cannot be local value types and therefore have no user-visible default.

## Supported types

Noria currently supports:

```noria
i32
bool
f64
str
void
```

`i32` is a signed 32-bit integer.

`bool` is a boolean value:

```noria
true
false
```

`f64` is a 64-bit floating-point value. Literals use decimal notation such as `2.0` or `3.14` (see [Floats](#floats)).

`str` is a string value. String literals use double quotes (see [Strings](#strings)).

`void` is available only as a function return type. A void function must end every potentially
completing control-flow path with a bare `return;`.

Noria does not perform implicit conversions among `i32`, `bool`, `f64`, or `str`. Use `as` casts where conversions are supported (see [Casts](#casts)).

Names in Noria are case-insensitive. Identifiers and keywords are normalized to lowercase during
lexing, so `Main`, `main`, and `MAIN` refer to the same name. String literal contents retain their
original casing. Because of that folding, a type parameter `I` is the same name as a local `i`;
write `name: Type` in that situation (`let i: i32`) rather than type-first `Type: name`.

## Program structure

A Noria program is a list of optional import declarations followed by struct and function declarations.

```noria
fn main() -> i32 {
  return 0;
}
```

After return inference, an executable program must contain a non-generic, zero-parameter entry point with this signature:

```noria
fn main() -> i32
```

Struct types are declared by name and referenced in annotations (for example, `Point`). Struct-typed parameters and returns are supported with pass-by-value copy semantics.

## Modules

Noria supports importing selected symbols from bundled stdlib modules. Import declarations must appear before any struct or function declarations.

```noria
import std::mathx::{square};

fn main() -> i32 {
  return square(5);
}
```

Syntax:

```noria
import <module-path>::{<name> (, <name>)*};
```

Current limitations:

- Only the `std` module root is supported.
- Paths use `std::` followed by one or more segments (for example, `std::mathx` or `std::internal::rt`).
- There is no `as` renaming, glob import, or user module search path.
- Only names listed in the import braces are merged into the program; other symbols in the imported file remain unavailable.
- Modules under `std::internal::` are available only to other standard-library modules, not to user programs.

The bundled stdlib lives in `stdlib/` next to the project root. The compiler resolves `std::mathx` to `stdlib/mathx.noria` and nested paths such as `std::internal::rt` to `stdlib/internal/rt.noria`.

The CLI finds that directory from the running executable: next to the binary, at `../stdlib` for in-tree `build/noria`, or at `../share/noria/stdlib` after `cmake --install`. Override with `--stdlib <dir>` or the `NORIA_STDLIB` environment variable.

## Private runtime ABI

Standard-library container implementations may use an internal pointer type and runtime builtins that are unavailable to ordinary programs:

- Type `__rt_ptr` — internal raw pointer type (maps to LLVM `ptr`).
- Builtins `__rt_alloc`, `__rt_realloc`, and `__rt_release` — wrap `malloc`, `realloc`, and `free`.
- Witness-polymorphic builtins `__rt_sizeof`, `__rt_load`, and `__rt_store` — typed buffer element size, load, and store. These resolve the element type from the enclosing generic specialization context and accept scalar elements only (`i32`, `f64`, `bool`, `str`).

These names are reserved with the `__rt_` prefix. User code cannot import `std::internal::*` modules, annotate variables with `__rt_ptr`, or call internal runtime builtins. Public stdlib modules such as `std::memory` expose safe entry points instead.

## Sequence (arr and list)

`std::sequence` exports a generic `Sequence<T, I>` struct and tag-selected `impl arr` / `impl list` operation families:

| Operation | Signature | arr | list |
| --- | --- | --- | --- |
| `sequence_new` | `fn sequence_new<T, I>(sample: T) -> Sequence<T, I>` | Geometric initial capacity; `sample` seeds element type inference | Circular sentinel doubly linked list; empty list |
| `sequence_len` | `fn sequence_len<T, I>(s: Sequence<T, I>) -> i32` | O(1) | O(1) |
| `sequence_push` | `fn sequence_push<T, I>(s: Sequence<T, I>, value: T) -> void` | O(1) amortized; grow/reallocate when full | O(1); append before sentinel |
| `sequence_get` | `fn sequence_get<T, I>(s: Sequence<T, I>, index: i32) -> T` | O(1); traps on out-of-bounds | O(n); walk from front; traps on out-of-bounds |
| `sequence_set` | `fn sequence_set<T, I>(s: Sequence<T, I>, index: i32, value: T) -> void` | O(1); traps on out-of-bounds | O(n); walk from front; traps on out-of-bounds |
| `sequence_pop` | `fn sequence_pop<T, I>(s: Sequence<T, I>) -> T` | O(1); remove last; traps on empty | O(1); remove last before sentinel; traps on empty |
| `sequence_insert` | `fn sequence_insert<T, I>(s: Sequence<T, I>, index: i32, value: T) -> void` | O(n) shift; insert at index in `[0, len]`; traps otherwise | O(n); walk to index + link; insert at index in `[0, len]`; traps otherwise |
| `sequence_remove` | `fn sequence_remove<T, I>(s: Sequence<T, I>, index: i32) -> T` | O(n) shift; remove at index in `[0, len)`; traps otherwise | O(n); walk + unlink; remove at index in `[0, len)`; traps otherwise |

Callers may select the backing implementation with the second type argument (`Sequence<i32, arr>` vs `Sequence<i32, list>`); omitting it defaults to `arr` (`Sequence<i32>`). A typed `Sequence<T>` local without an initializer is an empty sequence, equivalent to `sequence_new` with a default sample. Index a sequence with `s[i]` like a C++ `vector`: the result has type `T`, and `s[i] = expr` updates in place. Equal-length sequences of the same type can be added with `+` when `T` supports `+`; length mismatches trap at runtime. A `let` binding's declared type seeds constructor tag inference for the initializer's root call, such as `sequence_new(0)`. Nested expressions under that root do not inherit the declared type as an inference hint, and an entirely unannotated constructor call still cannot infer its implementation tag.

`bst` and other implementation tags are not implemented for `Sequence` yet; selecting them is a compile-time error.

```noria
import std::sequence::{Sequence, sequence_get, sequence_new, sequence_push};

fn main() -> i32 {
  let s: Sequence<i32, arr> = sequence_new(0);
  sequence_push(s, 10);
  sequence_push(s, 20);
  return sequence_get(s, 0) + sequence_get(s, 1);
}
```

```noria
import std::sequence::{Sequence, sequence_get, sequence_len, sequence_new, sequence_push};

fn main() -> i32 {
  let s: Sequence<i32, list> = sequence_new(0);
  sequence_push(s, 10);
  sequence_push(s, 20);
  if sequence_len(s) != 2 {
    return 1;
  }
  return sequence_get(s, 0) + sequence_get(s, 1);
}
```

## Dictionary (bst and hashmap)

`std::dictionary` exports a generic `Dictionary<K, V, I>` struct and tag-selected operation families. Omitting `I` defaults to `hashmap`, so `Dictionary<K, V>` is equivalent to `Dictionary<K, V, hashmap>`. `bst` keys require `<` and `==`; `hashmap` keys require `==` and V2 `hash` (`i32`, `bool`, `str`). A typed `Dictionary<K, V>` local without an initializer is an empty dictionary. Indexing `d[k]` behaves like C++ `unordered_map`: a present key yields the value, a missing key inserts a default `V` and returns it, and `d[k] = v` inserts or updates.

| Operation | Signature | bst | hashmap |
| --- | --- | --- | --- |
| `dictionary_new` | `fn dictionary_new<K, V, I>(kSample: K, vSample: V) -> Dictionary<K, V, I>` | Empty tree; samples seed type inference | Empty table (cap 8); samples seed type inference |
| `dictionary_len` | `fn dictionary_len<K, V, I>(d: Dictionary<K, V, I>) -> i32` | O(1) | O(1) |
| `dictionary_insert` | `fn dictionary_insert<K, V, I>(d: Dictionary<K, V, I>, key: K, value: V) -> void` | Upsert; O(h) | Upsert; O(1) avg; resize at 75% load |
| `dictionary_contains` | `fn dictionary_contains<K, V, I>(d: Dictionary<K, V, I>, key: K) -> bool` | O(h) | O(1) avg |
| `dictionary_get` | `fn dictionary_get<K, V, I>(d: Dictionary<K, V, I>, key: K) -> V` | O(h); traps on missing key | O(1) avg; traps on missing key |
| `dictionary_get_or` | `fn dictionary_get_or<K, V, I>(d: Dictionary<K, V, I>, key: K, default: V) -> V` | O(h) | O(1) avg |
| `dictionary_remove` | `fn dictionary_remove<K, V, I>(d: Dictionary<K, V, I>, key: K) -> V` | O(h); traps on missing key | O(1) avg; tombstone; traps on missing key |

Internal runtime helpers `null_ptr`, `ptr_eq`, `hash_of`, and `byte_offset` in `std::internal::rt` support dictionary implementations.

```noria
import std::dictionary::{Dictionary, dictionary_get, dictionary_insert, dictionary_new};

fn main() -> i32 {
  let d: Dictionary<i32, i32, hashmap> = dictionary_new(0, 0);
  dictionary_insert(d, 10, 100);
  return dictionary_get(d, 10);
}
```

```noria
import std::dictionary::{Dictionary, dictionary_get, dictionary_insert, dictionary_new};

fn main() -> i32 {
  let d: Dictionary<i32, i32, bst> = dictionary_new(0, 0);
  dictionary_insert(d, 10, 100);
  return dictionary_get(d, 10);
}
```

## Set (bst and hashmap)

`std::set` exports a generic `Set<T, I>` struct and tag-selected operation families. Omitting `I` defaults to the hashmap implementation, so `Set<T>` is equivalent to `Set<T, hashmap>`; `hashset` is an alias. Implementations reuse the dictionary BST/hashmap storage layout with a dummy `i32` value (same header, keys, and internal search paths as `Dictionary<T, i32, I>`). `bst` elements require `<` and `==`; `hashmap` elements require `==` and V2 `hash` (`i32`, `bool`, `str`). A typed `Set<T>` local without an initializer is an empty set. Indexing `s[x]` is a membership test (`bool`) and does not insert; `s[x] = expr` is rejected.

| Operation | Signature | bst | hashmap |
| --- | --- | --- | --- |
| `set_new` | `fn set_new<T, I>(sample: T) -> Set<T, I>` | Empty tree; sample seeds type inference | Empty table (cap 8); sample seeds type inference |
| `set_len` | `fn set_len<T, I>(s: Set<T, I>) -> i32` | O(1) | O(1) |
| `set_insert` | `fn set_insert<T, I>(s: Set<T, I>, elem: T) -> void` | Idempotent insert; O(h) | Idempotent insert; O(1) avg |
| `set_contains` | `fn set_contains<T, I>(s: Set<T, I>, elem: T) -> bool` | O(h) | O(1) avg |
| `set_remove` | `fn set_remove<T, I>(s: Set<T, I>, elem: T) -> void` | O(h); traps on missing element | O(1) avg; tombstone; traps on missing element |

```noria
import std::set::{Set, set_contains, set_insert, set_new, set_len};

fn main() -> i32 {
  let s: Set<i32, hashmap> = set_new(0);
  set_insert(s, 10);
  set_insert(s, 10);
  if set_len(s) != 1 {
    return 1;
  }
  return set_contains(s, 10);
}
```

## Heap (min-heap over Sequence)

`std::heap` exports tag-generic min-heap algorithms over `Sequence<T, I>`. Element type `T` must support `<` at use sites. Sift-up and sift-down use `sequence_get` / `sequence_set` to swap parent/child elements in place. Observable min-heap semantics are identical when only the sequence implementation tag changes (`arr` vs `list`); random access makes `arr` asymptotically preferable because each swap pays O(1) indexed access instead of O(n) list traversal.

| Operation | Signature | arr | list |
| --- | --- | --- | --- |
| `heappush` | `fn heappush<T, I>(s: Sequence<T, I>, value: T) -> void` | O(log n) | O(n log n) |
| `heappop` | `fn heappop<T, I>(s: Sequence<T, I>) -> T` | O(log n); traps on empty | O(n log n); traps on empty |
| `heapify` | `fn heapify<T, I>(s: Sequence<T, I>) -> void` | O(n) | O(n² log n) |

```noria
import std::heap::{heappop, heappush};
import std::sequence::{Sequence, sequence_new};

fn main() -> i32 {
  let s: Sequence<i32, arr> = sequence_new(0);
  heappush(s, 5);
  heappush(s, 3);
  heappush(s, 7);
  return heappop(s);
}
```

```noria
import std::heap::{heappop, heappush};
import std::sequence::{Sequence, sequence_new};

fn main() -> i32 {
  let s: Sequence<i32, list> = sequence_new(0);
  heappush(s, 5);
  heappush(s, 3);
  heappush(s, 7);
  return heappop(s);
}
```

`Sequence<T, I>`, `Dictionary<K, V, I>`, and `Set<T, I>` follow the same ownership model as `str` and `[T]`. Container handles are uniquely owned: reassignment, scope exit, and `main` return drop the previous value. `let b = a` deep-copies into independent heap storage. Function parameters borrow the caller's handle; in-place mutators such as `sequence_push(s, x)` take `-> void` and update through the borrowed handle. Returning an owned local moves its handle; returning a borrowed parameter clones so the caller keeps a valid value. Element types remain scalars (`i32`, `f64`, `bool`, `str`); nested containers are unsupported.

## Functions

Functions use typed parameters. A trailing return annotation is optional: when omitted, the
compiler infers one type from every `return` statement. Parameters may be written name-first or
type-first; both forms produce the same function signature. An explicit annotation remains useful
as documentation and is checked against the inferred body type.

`fn`, `util`, `helper`, and `recfn` are interchangeable function keywords. They all parse as the
same declaration; the spellings exist only for readability (`util` for small helpers, `recfn` for
recursive functions, `helper` for supporting methods).

```noria
fn add(a: i32, b: i32) -> i32 {
  return a + b;
}

fn add_swapped(i32: a, b: i32) -> i32 {
  return a + b;
}

fn main() {
  return add(3, 4);
}
```

Bare `return;` infers `void`. All value returns must infer the same type. If a recursive cycle or
an empty literal leaves the result underconstrained, write an explicit `-> Type` annotation.
Empty array literals still need an expected element type, so `return [];` cannot seed inference.

Every function must explicitly return on each control-flow path that can reach the end of its body. Non-void functions use `return <value>;`; void procedures use `return;`. A `while` loop is conservatively considered capable of ending, including `while true`. Statements that follow an `if`/`else` where both branches return are still type-checked, but codegen does not emit them (unreachable).

```noria
fn announce(value: i32) -> void {
  print_int(value);
  return;
}

fn main() -> i32 {
  announce(42);
  return 0;
}
```

Recursion is supported:

```noria
fn factorial(n: i32) -> i32 {
  if n <= 1 {
    return 1;
  } else {
    return n * factorial(n - 1);
  }
}
```

## Generic Functions

Functions may declare one or more type parameters in angle brackets after the name:

```noria
fn id<T>(x: T) -> T {
  return x;
}

fn main() -> i32 {
  return id(7);
}
```

Type parameters are bare identifiers with no bounds or defaults. At a call site, concrete type arguments are inferred from argument types (`id(7)` specializes to `i32`). When the call is the root of a `let` initializer, the binding's declared type also seeds still-unbound type parameters (for example `let s: Sequence<i32, list> = sequence_new(0)`). Nested expressions under that root — call arguments, casts, struct fields, and similar — do not inherit the declared type. Explicit type application (turbofish) is not supported. If a type parameter cannot be inferred, the compiler reports a type error.

Each distinct specialization is monomorphized into a concrete function with a deterministic mangled name such as `id$s.i32` (type kinds are encoded: scalars as `s.i32`, structs as `st.Point`). Calling the same generic twice with the same type reuses one specialization, including when the same concrete specialization is requested from multiple import paths.

When a generic call sits inside another specialization, type parameters that share a name with the caller (`V` in `dictionary_get<K, V>` calling `bst_load_value<V>`) are reused. Remaining parameters are inferred from arguments, then from a `let`/return expected type, then from leftover caller type arguments.

Generic functions may declare tag-selected implementations after an optional return type. Each
implementation shares the same public signature and is chosen at specialization time from the
inferred implementation tag in the type arguments:

```noria
fn kind<T, I>(b: Box<T, I>) -> i32 impl arr { return 1; }
fn kind<T, I>(b: Box<T, I>) -> i32 impl list { return 2; }
```

A tagged generic family must provide an implementation for every tag used at call sites. Mixing tagged and untagged bodies for the same name is rejected.

## Generic Structs

Structs may declare type parameters after the name:

```noria
struct Box<T> {
  value: T;
}
```

Use type applications in annotations and struct literals: `Box<i32>`, `Box<i32> { value: 42 }`. When type arguments are omitted from a literal (`Box { value: 42 }`), the compiler infers them from field values. Each concrete application is monomorphized into a specialized struct type such as `Box$s.i32`. Uncalled generic struct templates are not emitted in LLVM IR.

Implementation tags `arr`, `list`, `bst`, and `hashmap` are closed compile-time selectors used only inside type-argument lists (for example `Box<i32, arr>`). `hashset` is an additional spelling for `hashmap`. They are not runtime types and cannot appear as standalone value types. Each tag participates in specialization keys and mangling (`tag.arr`). Tagged specializations enforce key-type constraints at instantiation time: `bst` keys require `<` and `==` on `i32` or `f64`; `hashmap` keys require `==` and a V2 `hash` on `i32`, `bool`, or `str`. `arr` and `list` impose no key constraints.

## Variables

Local variables may use the original `let name: Type = expr;` form, a shorthand
typed form without `let`, or `let name = expr;` when the type can be inferred
from the initializer. Bare `name = expr;` is treated as assignment, so the `let` keyword is required for initialization in this case. Declarations
without an initializer must include an explicit type and are default-initialized.
Defaults are `0` for `i32`, `false` for `bool`, `0.0` for `f64`, `""` for `str`,
a length-0 heap array for `[T]`, `sequence_new` / `set_new` / `dictionary_new` for the
standard ADTs, and field-wise defaults for other structs.

```noria
let x: i32 = 42;
let flag: bool = x > 0;
x: i32 = 1;
i32: y = 2;
let inferred = x + y;
z: i32;
s: str;
values: [i32];
```

Variables can be reassigned:

```noria
x = x + 1;
```

## Expressions

Integer literals are signed 32-bit values in the range `[-2147483648, 2147483647]`. The literal `-2147483648` is accepted. Values outside that range are rejected with a located diagnostic.

```noria
42
-2147483648
```

Boolean literals:

```noria
true
false
```

Identifier expressions:

```noria
x
```

Function calls:

```noria
add(1, 2)
factorial(n - 1)
```

Parenthesized expressions:

```noria
(1 + 2) * 3
```

## Arithmetic

Supported arithmetic operators:

```noria
+
-
*
/
```

Example:

```noria
fn main() -> i32 {
  return 1 + 2 * 3;
}
```

Operator precedence is supported:

```noria
1 + 2 * 3     // 7
(1 + 2) * 3   // 9
```

Division is signed integer division that truncates toward zero. Integer `+`, `-`, and `*` wrap on overflow. Direct literal division or remainder by zero, `-2147483648 / -1`, `-2147483648 % -1`, and shifts with counts outside `0..31` are type errors. The same invalid operations with computed operands trap at runtime with exit status 70. Valid shifts use counts `0..31` and preserve LLVM's wrapping shift behavior.

## Comparisons

Supported comparison operators:

```noria
==
!=
<
<=
>
>=
```

Comparisons produce `bool`.

```noria
let flag: bool = x >= 10;
let same: bool = "noria" == "noria";
```

`==` and `!=` work on matching `i32`, `f64`, `bool`, or `str` operands. Ordered comparisons (`<`, `<=`, `>`, `>=`) work only on matching `i32` or `f64` operands.

`f64` `==` is IEEE-754 ordered equality: it is false if either operand is NaN.
`f64` `!=` is unordered not-equal: it is true if the values differ or if either
operand is NaN, so `x != x` detects NaN. Ordered comparisons stay false when
either operand is NaN.

## Unary Operators

Unary operators apply to a single operand:

```noria
!   // logical not (bool -> bool)
-   // negation (i32 -> i32 or f64 -> f64)
~   // bitwise not (i32 -> i32)
```

Example:

```noria
fn main() -> i32 {
  let enabled: bool = !false;
  let negated: i32 = -7;
  let inverted: i32 = ~0;
  if enabled {
    return negated + inverted;
  }
  return 0;
}
```

## Logical Operators

Logical operators require `bool` operands and short-circuit:

```noria
&&   // logical and
||   // logical or
```

Example:

```noria
fn main() -> i32 {
  let zero: i32 = 0;
  if false && 1 / zero == 1 {
    return 99;
  }
  return 0;
}
```

The right-hand side is not evaluated when the result is already determined.

## Bitwise Operators and Modulo

Bitwise operators and modulo apply to `i32` operands only:

```noria
&    // bitwise and
|    // bitwise or
^    // bitwise xor
<<   // left shift
>>   // right shift
%    // remainder
```

Example:

```noria
fn main() -> i32 {
  return (60 & 13) | (60 ^ 13);
}
```

## If / Else

`if` conditions must be `bool`.

```noria
fn main() -> i32 {
  let x: i32 = 7;

  if x > 5 {
    return 1;
  } else {
    return 0;
  }
}
```

Branches can also assign variables and continue:

```noria
fn main() -> i32 {
  let result: i32 = 0;

  if true {
    result = 7;
  } else {
    result = 9;
  }

  return result;
}
```

`else if` chains additional conditions:

```noria
fn classify(value: i32) -> i32 {
  if value < 0 {
    return 1;
  } else if value == 0 {
    return 2;
  } else {
    return 3;
  }
}
```

## While Loops

`while` conditions must be `bool`.

```noria
fn main() -> i32 {
  let i: i32 = 0;
  let sum: i32 = 0;

  while i <= 5 {
    sum = sum + i;
    i = i + 1;
  }

  return sum;
}
```

## Casts

Use `as` to convert between supported types. Identity casts (`T as T`) are allowed. Cross-type casts are limited to:

- `i32` to `f64`
- `f64` to `i32`
- `i32` to `bool`
- `bool` to `i32`

Example:

```noria
fn main() -> i32 {
  return (7 as i32) + (1 as i32);
}
```

Chained cross-type casts:

```noria
fn main() -> i32 {
  let value: i32 = 42;
  return value as f64 as i32;
}
```

An `f64 as i32` cast truncates toward zero. It traps at runtime if the input is
NaN or infinite, or if its truncated value cannot be represented by `i32`.
For example, `2147483647.5 as i32` produces `2147483647`, while
`2147483648.0 as i32` traps.

Other combinations are rejected:

```text
noria: error: 3:10: typecheck: cannot cast bool to f64
```

## Floats

Floating-point literals use decimal notation with a fractional part:

```noria
2.0
3.14
0.5
```

Arithmetic on `f64` values uses `+`, `-`, `*`, and `/`. Comparisons use the same
operators as integers. `f64` `!=` is true when either operand is NaN; `==` and
ordered comparisons are false when either operand is NaN.

Exponent syntax such as `1e3` is not supported.

Example:

```noria
fn main() -> i32 {
  let result: f64 = 6.0 / 2.0 + 1.0;
  return result as i32;
}
```

## Strings

String literals use double quotes. Supported escape sequences are `\n`, `\t`, `\"`, and `\\`.

```noria
"Hello, world!"
"line one\nline two"
"quote: \" backslash: \\"
```

`str` values can be stored in locals and passed as function parameters:

```noria
fn greet(name: str) -> i32 {
  print(name);
  return 0;
}

fn main() -> i32 {
  let message: str = "Hello";
  return greet(message);
}
```

Use `print`, `print_int`, `print_float`, and `print_char` to write to stdout without adding a newline; call `println()` when a newline is needed. For example, `print("A"); print("B");` writes `AB`, and `print_int(7); print(" items");` writes `7 items`. Use `len(s)` to get the byte length of a string as an `i32`. Index a string with `s[i]` where `i` is an `i32`; the result is an `i32` byte value (0–255). Out-of-range indexes, including negatives, trap at runtime. Concatenate strings with `+`; the result is a newly allocated C string (`malloc` + `strcpy`/`strcat`). Failed allocations trap. String literals and the default empty string are immortal and are never freed. Heap strings from concatenation are uniquely owned: reassignment, scope exit, and `main` return drop the previous value. `let b = a` on `str` locals deep-copies; function parameters borrow the caller's pointer without cloning. Returning a borrowed parameter clones so the caller keeps a valid value. Managed arguments are borrowed only for the duration of a call: `print(a + "!")`, for example, releases the newly created concatenation after printing without consuming `a`. `str` values compare with `==` and `!=`. A typed `str` local without an initializer is the empty string, not a null pointer.

## Arrays

Array types are written `[T]` where `T` is a scalar or array element type (for example, `[i32]`, `[str]`, `[[i32]]`). Struct types cannot be used as array elements.

```noria
let values: [i32] = [3, 4, 5, 6];
let names: [str] = ["alice", "bob"];
let empty: [i32] = [];
let grid: [[i32]] = [[]];
```

An empty literal uses a direct expected array type, such as a typed local declaration, function parameter, return value, assignment target, or struct-literal field. When an array literal is checked against `[T]`, each element is checked against `T`, so a nested `[]` is allowed when `T` is itself an array type (`let grid: [[i32]] = [[]];` or `let grid: [[i32]] = [[1], []];`). Unannotated declarations such as `let values = [];` and `let grid = [[]];` are rejected because no element type is known. A typed `[T]` local without an initializer is an empty array, equivalent to `[]`. Use `len(a)` on an array to read its element count as an `i32`. Index an array with `a[i]` where `i` is an `i32`; the result has the element type. Assign through an array index with `a[i] = expr` when the right-hand side matches the element type. Field and nested index chains rooted at a local are the same kind of assignment place: `h.items[i] = expr` and `h.grid[0][1] = expr` store through the shared heap buffer. Arrays of equal length can be added with `+` when the element type itself supports `+` (`i32`, `f64`, `str`, or nested arrays of those). Length mismatches trap at runtime. Arrays are heap-allocated: a literal calls `malloc(8 + n * sizeof(T))`, stores the element count in an `i64` header at offset 0, and stores elements contiguously starting at offset 8. An array value is the malloc base pointer. Passing an array to a function borrows the caller's buffer; callee mutation through `a[i] = ...` is visible to the caller. `let b = a` deep-copies managed arrays (including nested `[str]` / `[[T]]`). Returning an owned local moves its buffer; returning a borrowed parameter clones. Reassignment, scope exit, and `main` return drop the previous owned buffer. Out-of-range indexes, including negatives, trap at runtime. Failed allocations trap. `[bool]` elements are stored with byte stride even though SSA `bool` values are `i1`.

String indexing is read-only; `s[i] = expr` is rejected at type check.

Example:

```noria
fn sum(values: [i32], count: i32) -> i32 {
  let total: i32 = 0;
  let index: i32 = 0;

  while index < count {
    total = total + values[index];
    index = index + 1;
  }

  return total;
}

fn main() -> i32 {
  let values: [i32] = [3, 4, 5, 6];
  return sum(values, len(values));
}
```

## Structs

Declare a struct with named fields and semicolon-terminated field types. Field
declarations may be written name-first or type-first. Fields are public by
default; use `private:` and `public:` section labels inside the struct body to
control visibility. A private field is readable, assignable, and initializable
only from functions in the same module as the struct:

```noria
struct Point {
  x: i32;
  i32: y;
}

struct Sequence<T, I> {
  private:
  handle: __rt_ptr;
}
```

Module-private fields are enforced at typecheck time. Access from another module is rejected with a diagnostic such as `typecheck: field 'handle' is private to module 'std::sequence'`. Struct literals must respect the same rule, so ADTs with private fields are constructible only inside their declaring module — callers use exported constructors such as `sequence_new`.

Construct a struct value with a literal: the struct name followed by `{ field: expr, ... }`. Fields may appear in any order; the compiler stores them in declaration order:

```noria
let origin: Point = Point { x: 3, y: 4 };
let flipped: Point = Point { y: 4, x: 3 };
```

Read a field as an rvalue with postfix `.ident`:

```noria
origin.x + origin.y
```

Struct values are first-class aggregates stored in local slots. A typed struct local without an initializer default-initializes each field. Copying a struct (`let b: Point = a;`) copies the aggregate value. Passing a struct to a function or returning one from a function also copies the aggregate; callee mutations to parameter fields do not affect the caller's local. Mutate a field through a local with postfix assignment:

```noria
p.x = 10;
p.y = p.y + 1;
h.items[0] = 50;
```

Example:

```noria
struct Point {
  x: i32;
  y: i32;
}

fn main() -> i32 {
  let origin: Point = Point { x: 3, y: 4 };
  origin.x = 10;
  return origin.x + origin.y;
}
```

## Builtins

Noria provides a small set of builtin functions:

| Name | Arity | Parameters | Return |
|------|-------|------------|--------|
| `print` | 1 | `str` | `void` |
| `print_int` | 1 | `i32` | `void` |
| `print_float` | 1 | `f64` | `void` |
| `print_char` | 1 | `i32` | `void` |
| `println` | 0 | — | `void` |
| `sqrt` | 1 | `f64` | `f64` |
| `pow` | 2 | `f64`, `f64` | `f64` |
| `len` | 1 | `str` or array | `i32` |

Builtin names are reserved. Declaring a user function whose name matches a builtin
(`print`, `print_int`, `len`, and the rest of the table) is a type error at the
`fn` keyword. Calls always resolve to the builtin; user declarations cannot shadow
them.

`print`, `print_int`, `print_float`, and `print_char` write to stdout without a trailing newline. `println()` is the only builtin that writes a newline.

Example:

```noria
fn main() -> i32 {
  print("Hello, world!");
  println();
  print_int(42);
  print(" items");
  println();
  print_float(3.14);
  println();
  print_char(65);
  println();
  let root: f64 = sqrt(2.0);
  let power: f64 = pow(2.0, 10.0);
  return 0;
}
```

## Expression Statements

A statement may be a call to any void-returning function followed by `;`. Functions and builtins that return a value (such as `sqrt` or `pow`) cannot be used as standalone statements. Other bare expressions are rejected:

```text
noria: error: 2:3: typecheck: expression statement must be a function call
noria: error: 2:3: typecheck: expression statement must call a void function
```

Example:

```noria
fn main() -> i32 {
  print("Hello");
  println();
  return 0;
}
```

## Lexical Scoping

Blocks introduce lexical scopes. Variables declared inside a block are not visible outside it.

```noria
fn main() -> i32 {
  let x: i32 = 1;

  if true {
    let y: i32 = 2;
    x = x + y;
  } else {
    x = 0;
  }

  return x;
}
```

This is invalid because `y` is out of scope:

```noria
fn main() -> i32 {
  if true {
    let y: i32 = 2;
  } else {
    let y: i32 = 3;
  }

  return y;
}
```

Shadowing in nested blocks is supported:

```noria
fn main() -> i32 {
  let x: i32 = 1;

  if true {
    let x: i32 = 9;
    return x;
  } else {
    return x;
  }
}
```

## Comments

Line comments start with `//`.

```noria
// this is a comment
fn main() -> i32 {
  return 0;
}
```

## Complete Example

```noria
fn adjust(value: i32) -> i32 {
  let result: i32 = value;
  let small: bool = result < 10;

  if small {
    let bonus: i32 = 5;
    result = result + bonus;
  } else {
    let penalty: i32 = 2;
    result = result - penalty;
  }

  return result;
}

fn main() -> i32 {
  let i: i32 = 0;
  let total: i32 = 0;

  while i < 5 {
    total = total + adjust(i * 3);
    i = i + 1;
  }

  return total;
}
```

## Current Limitations

Noria currently does not support:

- user-defined modules or a module search path (only bundled `std::` imports)
- `as` import renaming or glob imports
- `break` or `continue`
- `for` loops
- global variables
- implicit conversions between types
- additional integer types (`i64`, unsigned, or character types)
- float exponent literals (for example, `1e3` does not parse)
- boxed recursive types (trees/graphs as user structs)
- `Sequence<T>` elements that are structs
- `[T]` arrays whose element type `T` is a struct
- nested `Sequence`, `Dictionary`, or `Set` element types


## Commands

See the [project overview](README.md#build-and-run) for prerequisites, installation, test, sanitizer, leak, and WIP fuzzing workflows. The compiler-facing commands are summarized here for reference.

Install into a prefix, then invoke `noria` from `PATH`:

```bash
cmake --install build --prefix /usr/local
noria --help
```

Emit LLVM IR:

```bash
./build/noria examples/basic/factorial.noria -o build/factorial.ll
```

Build a native executable:

```bash
./build/noria build -O2 examples/basic/factorial.noria -o build/factorial
./build/factorial
echo $?
```

Emit optimized LLVM IR:

```bash
./build/noria -O2 examples/basic/variables.noria -o build/variables.opt.ll
```

Debug tokens:

```bash
./build/noria --emit-tokens examples/basic/lexer_smoke.noria
```

Debug the parsed AST:

```bash
./build/noria --emit-ast examples/basic/ast_smoke.noria
```

Type-checking errors include source line and column information:

```text
noria: error: 2:10: typecheck: unknown local variable 'missing'
```
