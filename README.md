# Noria | Statically Typed Custom Programming Language

Noria is a small statically typed language and C++ compiler project created by Matthew McRae. The goal is a compact, defensible compiler MVP that demonstrates the core pieces of a real language implementation without overbuilding.

Noria is actively in development. The `examples/future/` directory holds design sketches for upcoming work; some entries are superseded stubs that point at passing programs in `examples/basic/`.

## Current Status

The compiler currently supports `i32`, `bool`, `f64`, and `str` values; local variables; assignment; arithmetic; comparisons; unary operators (`!`, `-`, `~`); short-circuit logical operators (`&&`, `||`); bitwise operators and `%` on integers; `if` / `else` / `else if`; `while` loops; `as` casts between `i32`, `f64`, and `bool`; builtins (`print`, `print_int`, `print_char`, `println`, `sqrt`, `pow`, `len`); expression statements that call void builtins; functions; recursion; lexical scoping; `import std::<name>::{...}` from the bundled `stdlib/`; static type checking; LLVM IR generation; LLVM optimization; and native macOS executable output:

```noria
fn main() -> i32 {
  print("Hello, world!");
  return 0;
}
```

Recursion example:

```noria
fn factorial(n: i32) -> i32 {
  if n <= 1 {
    return 1;
  } else {
    return n * factorial(n - 1);
  }
}

fn main() -> i32 {
  return factorial(5);
}
```

Compiler pipeline:

```text
Noria source
  -> lexer
  -> parser / AST
  -> static type checker
  -> LLVM IR generation
  -> optional LLVM optimization
  -> native executable
```

## Requirements

- CMake 3.20+
- A C++20 compiler
- LLVM tools for native executable tests, especially `llc`
- `clang` for linking generated object files

## Build

```bash
cmake -S . -B build
cmake --build build
```

If you use `just`, common workflows are wrapped in `Justfile`:

```bash
just build
just test
just tokens examples/basic/lexer_smoke.noria
just ast examples/basic/factorial.noria
just ir examples/basic/factorial.noria build/factorial.ll -O2
just run examples/basic/factorial.noria build/factorial -O2
```

## Run Tests

The test script builds the compiler, compiles every program in `examples/basic`, and runs native executable checks for examples with shell-friendly exit codes.

```bash
./tests/run_examples.sh
```

You can also run it through CTest after configuring the project:

```bash
cmake -S . -B build
ctest --test-dir build --output-on-failure
```

## Memory Safety Checks

On macOS, use Clang sanitizers for memory and undefined-behavior checks:

```bash
just sanitize
```

This configures a separate `build-sanitize` directory with AddressSanitizer and UndefinedBehaviorSanitizer enabled, then runs the normal compiler test suite.

On Linux, or any machine with Valgrind installed, you can also run the compiler suite under Valgrind:

```bash
just valgrind
```

That wraps every compiler invocation in `valgrind --leak-check=full --error-exitcode=1`, so leaks or memory errors fail the test run.

## Continuous Integration

GitHub Actions runs the same compiler test suite on every push and pull request. The workflow installs LLVM, configures CMake, builds the compiler, and runs:

```bash
BUILD_DIR=build ./tests/run_examples.sh
```

## Compile A Program To LLVM IR

```bash
./build/noria examples/basic/return_answer.noria -o build/return_answer.ll
```

The generated LLVM IR will look like:

```llvm
define i32 @main() {
entry:
  ret i32 42
}
```

## Debug Tokens

Use `--emit-tokens` to inspect the lexer without running the parser:

```bash
./build/noria --emit-tokens examples/basic/lexer_smoke.noria
```

## Debug AST

Use `--emit-ast` to inspect the parser output before type checking or code generation:

```bash
./build/noria --emit-ast examples/basic/factorial.noria
```

Example output:

```text
Module
  Function factorial(n: i32) -> i32
    Block
      If
        Condition
          Binary <=
            Identifier n
            Integer 1
```

## Diagnostics

Parser and type checker errors include source line and column information:

```text
noria: error: 2:10: typecheck: unknown local variable 'missing'
```

## Compile LLVM IR To A Native macOS Executable

Use the direct build command:

```bash
./build/noria build -O2 examples/basic/factorial.noria -o build/factorial
./build/factorial
echo $?
```

Use `-O1`, `-O2`, or `-O3` to run LLVM's `opt` pipeline before emitting LLVM IR or building a native executable:

```bash
./build/noria -O2 examples/basic/variables.noria -o build/variables.opt.ll
```

Or compile emitted LLVM IR manually with Homebrew LLVM:

```bash
LLVM_BIN=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
"${LLVM_BIN}/llc" -filetype=obj build/return_answer.ll -o build/return_answer.o
clang build/return_answer.o -o build/return_answer
./build/return_answer
echo $?
```

Expected result:

```text
42
```

## Examples

Passing examples live in `examples/basic` (93 programs).

Negative type-checking examples live in `examples/invalid` (61 programs).

Lexer and parser failure examples live in `examples/invalid_syntax` (10 programs).

Design sketches and superseded stubs live in `examples/future`.

## Architecture

Compiler pipeline:

```text
source
  -> lexer
  -> parser / AST
  -> static type checking
  -> LLVM IR generation / optimization
  -> native executable
```

LLVM is used because it lets Noria focus on language implementation while delegating machine-code generation, object files, platform calling conventions, and macOS target details to mature backend tools.

## Limitations

Noria is an intentionally small MVP language. It does not yet support:

- multi-segment / non-`std` imports and generics (basic `import std::module::{names};` works)
- container stdlib
- `break` or `continue`
- `for` loops
- type inference
- global variables
- implicit conversions between types
- float exponent literals (for example, `1e3` does not parse)
- additional integer types (`i64`, unsigned, or character types)

Known issue: `print_float` is registered but prints incorrect values on arm64 due to a variadic `printf` calling-convention mismatch in codegen; use `print` with string literals for output until that is fixed.

## Future Work

See `SYNTAX.md` for the currently supported Noria language syntax.
