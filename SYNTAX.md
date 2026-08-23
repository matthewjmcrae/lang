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

A Noria program is a list of function declarations.

```noria
fn main() -> i32 {
  return 0;
}
```

The compiler expects an executable program to contain:

```noria
fn main() -> i32
```

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
| `len` | 1 | `str` | `i32` |

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

- arrays
- structs or classes
- imports or modules
- generics
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
