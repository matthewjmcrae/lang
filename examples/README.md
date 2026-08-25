# Noria Examples

This directory contains the programs used to exercise the compiler pipeline. The main test suite compiles every passing example, checks every invalid example fails during type checking, and runs many examples as native executables.

Run all examples:

```bash
just test
```

Or without `just`:

```bash
BUILD_DIR=build ./tests/run_examples.sh
```

## `basic`

These are valid Noria programs supported by the current compiler.

They cover:

- Integer and boolean literals
- Arithmetic and comparisons
- Local variables and assignment
- `if` / `else`
- `while` loops
- Functions and function calls
- Void procedures and explicit returns
- Recursion and mutual recursion
- Lexical scoping and shadowing
- Direct native executable generation
- Optimized LLVM IR / native builds through `-O1`, `-O2`, or `-O3`
- Arrays: `[T]` types, literals, `len`, and index read

Good starter examples:

```text
examples/basic/return_answer.noria
examples/basic/arithmetic.noria
examples/basic/variables.noria
examples/basic/if_assignment_true.noria
examples/basic/while_sum.noria
examples/basic/function_call.noria
examples/basic/factorial.noria
```

Larger showcase examples:

```text
examples/basic/showcase_recursive_loop.noria
examples/basic/showcase_bool_scope_loop.noria
examples/basic/showcase_nested_control.noria
examples/basic/showcase_recursion_branching.noria
```

Compile one to LLVM IR:

```bash
./build/noria examples/basic/factorial.noria -o build/factorial.ll
```

Compile one to an optimized native executable:

```bash
./build/noria build -O2 examples/basic/factorial.noria -o build/factorial
./build/factorial
echo $?
```

## `invalid`

These programs are intentionally invalid. They should fail during static checking and are part of the negative test suite.

They cover:

- Unknown variables and functions
- Out-of-scope variables
- Duplicate functions, parameters, and locals
- Functions named after builtins
- Assignment type mismatches
- Return type mismatches
- Missing return paths and invalid void returns
- Non-boolean `if` / `while` conditions
- Wrong function argument counts
- Wrong function argument types
- Invalid boolean arithmetic
- Nested call, recursion, branch, and loop scope type failures

Example:

```bash
./build/noria examples/invalid/assignment_type_mismatch.noria -o build/bad.ll
```

Expected result: a nonzero compiler exit with a `typecheck:` diagnostic.

## `invalid_syntax`

These programs are intentionally invalid at the lexer/parser level. They should fail before type checking and are used to verify source-position-aware syntax diagnostics.

Example:

```bash
./build/noria examples/invalid_syntax/missing_semicolon.noria -o build/bad.ll
```

Expected result: a nonzero compiler exit with a line/column parser or lexer diagnostic.

## `future`

These files are sketches of future features for the langauge
