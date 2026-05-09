# Noria Syntax

Noria is a small statically typed language that currently compiles to LLVM IR and native macOS executables.

## Supported Types

Noria currently supports:

```noria
i32
bool
```

`i32` is a signed 32-bit integer.

`bool` is a boolean value:

```noria
true
false
```

Noria does not perform implicit conversions between `i32` and `bool`.

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

- strings
- arrays
- structs or classes
- imports or modules
- standard library calls
- floating-point numbers
- unary operators
- logical operators such as `&&`, `||`, or `!`
- `break` or `continue`
- `else if` syntax
- global variables
- type inference
- generics

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
