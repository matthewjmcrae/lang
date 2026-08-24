# Noria Syntax

Noria is a small statically typed language that currently compiles to LLVM IR and native macOS executables.

## Supported Types

Noria currently supports:

```noria
i32
bool
f64
str
```

`i32` is a signed 32-bit integer.

`bool` is a boolean value:

```noria
true
false
```

`f64` is a 64-bit floating-point value. Literals use decimal notation such as `2.0` or `3.14` (see [Floats](#floats)).

`str` is a string value. String literals use double quotes (see [Strings](#strings)).

Noria does not perform implicit conversions among `i32`, `bool`, `f64`, or `str`. Use `as` casts where conversions are supported (see [Casts](#casts)).

## Program Structure

A Noria program is a list of optional import declarations followed by struct and function declarations.

```noria
fn main() -> i32 {
  return 0;
}
```

The compiler expects an executable program to contain:

```noria
fn main() -> i32
```

Struct types are declared by name and referenced in annotations (for example, `Point`). Struct-typed parameters and returns are supported with pass-by-value copy semantics.

## Modules

Wake 1 supports importing selected symbols from bundled stdlib modules. Import declarations must appear before any struct or function declarations.

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

The bundled stdlib lives in `stdlib/` next to the project root. The compiler resolves `std::mathx` to `stdlib/mathx.noria` and nested paths such as `std::internal::rt` to `stdlib/internal/rt.noria`. Override the location with `--stdlib <dir>`.

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
| `sequence_push` | `fn sequence_push<T, I>(s: Sequence<T, I>, value: T) -> Sequence<T, I>` | O(1) amortized; grow/reallocate when full | O(1); append before sentinel |
| `sequence_get` | `fn sequence_get<T, I>(s: Sequence<T, I>, index: i32) -> T` | O(1); traps on out-of-bounds | O(n); walk from front; traps on out-of-bounds |
| `sequence_set` | `fn sequence_set<T, I>(s: Sequence<T, I>, index: i32, value: T) -> Sequence<T, I>` | O(1); traps on out-of-bounds | O(n); walk from front; traps on out-of-bounds |
| `sequence_pop` | `fn sequence_pop<T, I>(s: Sequence<T, I>) -> T` | O(1); remove last; traps on empty | O(1); remove last before sentinel; traps on empty |
| `sequence_insert` | `fn sequence_insert<T, I>(s: Sequence<T, I>, index: i32, value: T) -> Sequence<T, I>` | O(n) shift; insert at index in `[0, len]`; traps otherwise | O(n); walk to index + link; insert at index in `[0, len]`; traps otherwise |
| `sequence_remove` | `fn sequence_remove<T, I>(s: Sequence<T, I>, index: i32) -> T` | O(n) shift; remove at index in `[0, len)`; traps otherwise | O(n); walk + unlink; remove at index in `[0, len)`; traps otherwise |

Callers select the backing implementation with the second type argument (`Sequence<i32, arr>` vs `Sequence<i32, list>`). A `let` binding's declared type seeds constructor tag inference for the initializer's root call, such as `sequence_new(0)`. Nested expressions under that root do not inherit the declared type as an inference hint.

`bst` and other implementation tags are not implemented for `Sequence` yet; selecting them is a compile-time error.

```noria
import std::sequence::{Sequence, sequence_get, sequence_new, sequence_push};

fn main() -> i32 {
  let s: Sequence<i32, arr> = sequence_new(0);
  s = sequence_push(s, 10);
  s = sequence_push(s, 20);
  return sequence_get(s, 0) + sequence_get(s, 1);
}
```

```noria
import std::sequence::{Sequence, sequence_get, sequence_len, sequence_new, sequence_push};

fn main() -> i32 {
  let s: Sequence<i32, list> = sequence_new(0);
  s = sequence_push(s, 10);
  s = sequence_push(s, 20);
  if sequence_len(s) != 2 {
    return 1;
  }
  return sequence_get(s, 0) + sequence_get(s, 1);
}
```

## Dictionary (bst and hashmap)

`std::dictionary` exports a generic `Dictionary<K, V, I>` struct and tag-selected operation families. `bst` keys require `<` and `==`; `hashmap` keys require `==` and V2 `hash` (`i32`, `bool`, `str`).

| Operation | Signature | bst | hashmap |
| --- | --- | --- | --- |
| `dictionary_new` | `fn dictionary_new<K, V, I>(kSample: K, vSample: V) -> Dictionary<K, V, I>` | Empty tree; samples seed type inference | Empty table (cap 8); samples seed type inference |
| `dictionary_len` | `fn dictionary_len<K, V, I>(d: Dictionary<K, V, I>) -> i32` | O(1) | O(1) |
| `dictionary_insert` | `fn dictionary_insert<K, V, I>(d: Dictionary<K, V, I>, key: K, value: V) -> Dictionary<K, V, I>` | Upsert; O(h) | Upsert; O(1) avg; resize at 75% load |
| `dictionary_contains` | `fn dictionary_contains<K, V, I>(d: Dictionary<K, V, I>, key: K) -> bool` | O(h) | O(1) avg |
| `dictionary_get` | `fn dictionary_get<K, V, I>(d: Dictionary<K, V, I>, key: K) -> V` | O(h); traps on missing key | O(1) avg; traps on missing key |
| `dictionary_get_or` | `fn dictionary_get_or<K, V, I>(d: Dictionary<K, V, I>, key: K, default: V) -> V` | O(h) | O(1) avg |
| `dictionary_remove` | `fn dictionary_remove<K, V, I>(d: Dictionary<K, V, I>, key: K) -> V` | O(h); traps on missing key | O(1) avg; tombstone; traps on missing key |

Internal runtime helpers `null_ptr`, `ptr_eq`, `hash_of`, and `byte_offset` in `std::internal::rt` support dictionary implementations.

```noria
import std::dictionary::{Dictionary, dictionary_get, dictionary_insert, dictionary_new};

fn main() -> i32 {
  let d: Dictionary<i32, i32, hashmap> = dictionary_new(0, 0);
  d = dictionary_insert(d, 10, 100);
  return dictionary_get(d, 10);
}
```

```noria
import std::dictionary::{Dictionary, dictionary_get, dictionary_insert, dictionary_new};

fn main() -> i32 {
  let d: Dictionary<i32, i32, bst> = dictionary_new(0, 0);
  d = dictionary_insert(d, 10, 100);
  return dictionary_get(d, 10);
}
```

## Set (bst and hashmap)

`std::set` exports a generic `Set<T, I>` struct and tag-selected operation families. Implementations reuse the dictionary BST/hashmap storage layout with a dummy `i32` value (same header, keys, and internal search paths as `Dictionary<T, i32, I>`). `bst` elements require `<` and `==`; `hashmap` elements require `==` and V2 `hash` (`i32`, `bool`, `str`).

| Operation | Signature | bst | hashmap |
| --- | --- | --- | --- |
| `set_new` | `fn set_new<T, I>(sample: T) -> Set<T, I>` | Empty tree; sample seeds type inference | Empty table (cap 8); sample seeds type inference |
| `set_len` | `fn set_len<T, I>(s: Set<T, I>) -> i32` | O(1) | O(1) |
| `set_insert` | `fn set_insert<T, I>(s: Set<T, I>, elem: T) -> Set<T, I>` | Idempotent insert; O(h) | Idempotent insert; O(1) avg |
| `set_contains` | `fn set_contains<T, I>(s: Set<T, I>, elem: T) -> bool` | O(h) | O(1) avg |
| `set_remove` | `fn set_remove<T, I>(s: Set<T, I>, elem: T) -> Set<T, I>` | O(h); traps on missing element | O(1) avg; tombstone; traps on missing element |

```noria
import std::set::{Set, set_contains, set_insert, set_new, set_len};

fn main() -> i32 {
  let s: Set<i32, hashmap> = set_new(0);
  s = set_insert(s, 10);
  s = set_insert(s, 10);
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
| `heappush` | `fn heappush<T, I>(s: Sequence<T, I>, value: T) -> Sequence<T, I>` | O(log n) | O(n log n) |
| `heappop` | `fn heappop<T, I>(s: Sequence<T, I>) -> T` | O(log n); traps on empty | O(n log n); traps on empty |
| `heapify` | `fn heapify<T, I>(s: Sequence<T, I>) -> Sequence<T, I>` | O(n) | O(n² log n) |

```noria
import std::heap::{heappop, heappush};
import std::sequence::{Sequence, sequence_new};

fn main() -> i32 {
  let s: Sequence<i32, arr> = sequence_new(0);
  s = heappush(s, 5);
  s = heappush(s, 3);
  s = heappush(s, 7);
  return heappop(s);
}
```

```noria
import std::heap::{heappop, heappush};
import std::sequence::{Sequence, sequence_new};

fn main() -> i32 {
  let s: Sequence<i32, list> = sequence_new(0);
  s = heappush(s, 5);
  s = heappush(s, 3);
  s = heappush(s, 7);
  return heappop(s);
}
```

## Functions

Functions use typed parameters and a typed return value. Parameters may be written
name-first or type-first; both forms produce the same function signature.

```noria
fn add(a: i32, b: i32) -> i32 {
  return a + b;
}

fn add_swapped(i32: a, b: i32) -> i32 {
  return a + b;
}

fn main() -> i32 {
  return add(3, 4);
}
```

If a function reaches the end of its body without a `return`, code generation emits the type's zero value (`0`, `false`, `0.0`, `null`, or `zeroinitializer`). Missing returns are not a type error. Statements that follow an `if`/`else` where both branches `return` are still type-checked, but codegen does not emit them (unreachable).

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

Generic functions may declare tag-selected implementations after the return type. Each implementation shares the same public signature and is chosen at specialization time from the inferred implementation tag in the type arguments:

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

Implementation tags `arr`, `list`, `bst`, and `hashmap` are closed compile-time selectors used only inside type-argument lists (for example `Box<i32, arr>`). They are not runtime types and cannot appear as standalone value types. Each tag participates in specialization keys and mangling (`tag.arr`). Tagged specializations enforce key-type constraints at instantiation time: `bst` keys require `<` and `==` on `i32` or `f64`; `hashmap` keys require `==` and a V2 `hash` on `i32`, `bool`, or `str`. `arr` and `list` impose no key constraints.

## Variables

Local variables may use the original `let name: Type = expr;` form, a shorthand
typed form without `let`, or `let name = expr;` when the type can be inferred
from the initializer. Bare `name = expr;` remains assignment. Declarations
without an initializer must include an explicit type and are default-initialized.

```noria
let x: i32 = 42;
let flag: bool = x > 0;
x: i32 = 1;
i32: y = 2;
let inferred = x + y;
z: i32;
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

Division is signed integer division that truncates toward zero. Integer `+`, `-`, and `*` wrap on overflow. Division or remainder by zero and shifts by 32 or more are LLVM poison; they are not trapped.

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
  if false && 1 / 0 == 1 {
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

Arithmetic on `f64` values uses `+`, `-`, `*`, and `/`. Comparisons use the same operators as integers.

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

Use the `print` builtin to write a string to stdout. Use `len(s)` to get the byte length of a string as an `i32`. Index a string with `s[i]` where `i` is an `i32`; the result is an `i32` byte value (0–255). Out-of-range indexes, including negatives, trap at runtime. Concatenate strings with `+`; the result is a newly allocated C string (`malloc` + `strcpy`/`strcat`). Failed allocations trap. Noria does not reclaim concatenated strings — they are leaked on program exit, consistent with the MVP allocator stance. `str` values compare with `==` and `!=`.

## Arrays

Array types are written `[T]` where `T` is a scalar or array element type (for example, `[i32]`, `[str]`, `[[i32]]`). Struct types cannot be used as array elements.

```noria
let values: [i32] = [3, 4, 5, 6];
let names: [str] = ["alice", "bob"];
```

Use `len(a)` on an array to read its element count as an `i32`. Index an array with `a[i]` where `i` is an `i32`; the result has the element type. Assign through an array index with `a[i] = expr` when the right-hand side matches the element type. Arrays are heap-allocated: a literal calls `malloc(8 + n * sizeof(T))`, stores the element count in an `i64` header at offset 0, and stores elements contiguously starting at offset 8. An array value is the malloc base pointer. Passing an array to a function copies the pointer (shared buffer). Out-of-range indexes, including negatives, trap at runtime. Failed allocations trap. `[bool]` elements are stored with byte stride even though SSA `bool` values are `i1`. Arrays are not freed — they leak on program exit, consistent with the MVP allocator stance.

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

Struct values are first-class aggregates stored in local slots. Copying a struct (`let b: Point = a;`) copies the aggregate value. Passing a struct to a function or returning one from a function also copies the aggregate; callee mutations to parameter fields do not affect the caller's local. Mutate a field through a local with postfix assignment:

```noria
p.x = 10;
p.y = p.y + 1;
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
| `print_char` | 1 | `i32` | `void` |
| `println` | 0 | — | `void` |
| `sqrt` | 1 | `f64` | `f64` |
| `pow` | 2 | `f64`, `f64` | `f64` |
| `len` | 1 | `str` or array | `i32` |

Example:

```noria
fn main() -> i32 {
  print("Hello, world!");
  print_int(42);
  print_char(65);
  println();
  let root: f64 = sqrt(2.0);
  let power: f64 = pow(2.0, 10.0);
  return 0;
}
```

`print_float` is registered in the compiler but currently prints incorrect values on arm64; prefer `print` for output until that is fixed.

## Expression Statements

A statement may be a call to a void-returning builtin followed by `;`. User-defined functions and builtins that return a value (such as `sqrt` or `pow`) cannot be used as standalone statements. Other bare expressions are rejected:

```text
noria: error: 2:3: typecheck: expression statement must be a function call
noria: error: 2:3: typecheck: expression statement must call a void builtin
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
- reclaiming `str` concatenations or array/`malloc` buffers (leak-on-exit)

Runtime traps (exit status 70) cover Sequence/Dictionary/Set misuse, array and string index OOB, and failed `malloc`/`realloc`. Integer overflow wraps; `sdiv`/`srem` by zero and shifts of 32 or more are LLVM poison.

## Commands

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
