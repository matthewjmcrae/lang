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
- Paths must be exactly `std::<name>` (for example, `std::mathx`).
- There is no `as` renaming, glob import, or user module search path.
- Only names listed in the import braces are merged into the program; other symbols in the imported file remain unavailable.
- Diagnostics report `line:column` without a source file path for imported modules.

The bundled stdlib lives in `stdlib/` next to the project root. The compiler resolves `std::mathx` to `stdlib/mathx.noria`. Override the location with `--stdlib <dir>`.

## Functions

Functions use typed parameters and a typed return value.

```noria
fn add(a: i32, b: i32) -> i32 {
  return a + b;
}

fn main() -> i32 {
  return add(3, 4);
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

Type parameters are bare identifiers with no bounds or defaults. At a call site, concrete type arguments are inferred from argument types only (`id(7)` specializes to `i32`). Explicit type application (turbofish) is not supported. If a type parameter cannot be inferred from arguments — for example, when it appears only in the return type — the compiler reports a type error.

Each distinct specialization is monomorphized into a concrete function with a deterministic mangled name such as `id$s.i32` (type kinds are encoded: scalars as `s.i32`, structs as `st.Point`). Calling the same generic twice with the same type reuses one specialization. Implementation tags and constraints are not supported in the current compiler.

## Generic Structs

Structs may declare type parameters after the name:

```noria
struct Box<T> {
  value: T;
}
```

Use type applications in annotations and struct literals: `Box<i32>`, `Box<i32> { value: 42 }`. When type arguments are omitted from a literal (`Box { value: 42 }`), the compiler infers them from field values. Each concrete application is monomorphized into a specialized struct type such as `Box$s.i32`. Uncalled generic struct templates are not emitted in LLVM IR.

Implementation tags `arr`, `list`, `bst`, and `hashmap` are closed compile-time selectors used only inside type-argument lists (for example `Box<i32, arr>`). They are not runtime types and cannot appear as standalone value types. Each tag participates in specialization keys and mangling (`tag.arr`). Tagged specializations enforce key-type constraints at instantiation time: `bst` keys require `<` and `==` on `i32` or `f64`; `hashmap` keys require `==` on `i32` or `f64` and a V2 `hash` on `i32`, `bool`, or `str`. `arr` and `list` impose no key constraints.

## Variables

Local variables are declared with `let`.

```noria
let x: i32 = 42;
let flag: bool = x > 0;
```

Variables can be reassigned:

```noria
x = x + 1;
```

## Expressions

Integer literals:

```noria
42
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

Division is signed integer division.

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
```

Comparisons work on `i32` and `f64` operands of the same type.

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

Use the `print` builtin to write a string to stdout. Use `len(s)` to get the byte length of a string as an `i32`. Index a string with `s[i]` where `i` is an `i32`; the result is an `i32` byte value (0–255). Concatenate strings with `+`; the result is a newly allocated C string (`malloc` + `strcpy`/`strcat`). Noria does not reclaim concatenated strings — they are leaked on program exit, consistent with the MVP allocator stance.

## Arrays

Array types are written `[T]` where `T` is any known element type (for example, `[i32]`, `[str]`, `[[i32]]`). Array literals use square brackets with comma-separated elements:

```noria
let values: [i32] = [3, 4, 5, 6];
let names: [str] = ["alice", "bob"];
```

Use `len(a)` on an array to read its element count as an `i32`. Index an array with `a[i]` where `i` is an `i32`; the result has the element type. Assign through an array index with `a[i] = expr` when the right-hand side matches the element type. Arrays are heap-allocated: a literal calls `malloc(8 + n * sizeof(T))`, stores the element count in an `i64` header at offset 0, and stores elements contiguously starting at offset 8. An array value is the malloc base pointer. Passing an array to a function copies the pointer (shared buffer). There is no bounds checking; out-of-range indexing is undefined behavior. Arrays are not freed — they leak on program exit, consistent with the MVP allocator stance.

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

Declare a struct with named fields and semicolon-terminated field types:

```noria
struct Point {
  x: i32;
  y: i32;
}
```

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

Noria does not currently support:

- imports or modules
- container stdlib
- `break` or `continue`
- `for` loops
- global variables
- type inference
- implicit conversions between types
- additional integer types (`i64`, unsigned, or character types)
- float exponent literals (for example, `1e3` does not parse)

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
