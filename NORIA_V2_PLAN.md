# Noria V2 — Implementation Plan

**Overview:** Take Noria from its `i32`/`bool` MVP to a V2 with floating point, I/O, unary/logical/bitwise operators, strings, arrays, and structs, then prove it out with two flagship demos written in Noria (a Brainfuck interpreter and a PPM ray tracer), updating docs/tests throughout.

Target: end of July, part-time. The work is sequenced so each phase ends in a compiling, testable state, and there's a clear "minimum shippable V2" cut line if time runs short.

## Task checklist

- [ ] **Phase 0 — type-refactor:** Generalize `Type` (TypeChecker.hpp) and `IrType` (Codegen.hpp) from flat enums to kind+payload representations; keep all 53 existing examples passing.
- [ ] **Phase 1 — operators:** Add unary (`!`, `-`, `~`), logical (`&&`, `||` short-circuit), bitwise (`& | ^ << >>`), modulo (`%`), optional `else` / `else if`, and `as` cast tokens/AST/parse levels.
- [ ] **Phase 2 — floats-io-cast:** Add `f64` end-to-end, expression statements, print builtins (printf/putchar), `as` casts (sitofp/fptosi), and sqrt/pow intrinsics. FizzBuzz + hello world run.
- [ ] **Phase 3 — strings:** Add `str` type, string literals as globals, indexing, `len` (strlen), concat (malloc+strcpy), `print(str)`.
- [ ] **Phase 4 — arrays-lvalue:** Add arrays `[T]` (length-prefixed heap block), array literals, indexing, `len`; generalize `AssignmentStatement` to lvalue/place expressions.
- [ ] **Phase 5 — structs:** Add struct decls, named LLVM types, construction, field access (rvalue+lvalue via Phase 4 path), pass by value.
- [ ] **Phase 6 — demo-brainfuck:** Write a Brainfuck interpreter in Noria (`examples/demos/brainfuck.noria`) and assert its output in tests.
- [ ] **Phase 7 — demo-raytracer:** Write a PPM ray tracer in Noria (vec3 struct, sphere intersection, sqrt shading); render to PNG and embed in README.
- [ ] **Phase 8 — docs-tests:** Update README/SYNTAX, extend run_examples.sh with new positive/negative/output tests, update AstPrinter, write resume bullet.

## Architecture today (what we're extending)

```text
.noria -> Lexer.cpp -> Parser.cpp (AST) -> TypeChecker.cpp -> Codegen.cpp (LLVM IR text) -> opt -> llc -> clang
```

The two things every feature touches:
- `include/noria/TypeChecker.hpp` `enum class Type { I32, Bool }` — flat enum, no payload.
- `include/noria/Codegen.hpp` `enum class IrType { I32, Bool }` and `struct Value { string text; IrType type; }` — text-based emission.

Codegen is string-based with a stack/alloca model and `vector<Scope>` of `{slot, IrType}` bindings (`src/Codegen.cpp`). This is fine to extend; we keep emitting IR text.

## Cross-cutting design decisions (chosen defaults)

- Memory: AOT, no GC. Heap via libc `malloc`/`free` declared as externs in emitted IR. Leak-on-exit is acceptable for demos; documented as "arena allocator / free is future work" (an honest, mature answer in interviews).
- Strings: null-terminated `i8*` (C strings). Literals become private LLVM globals; `len` -> libc `strlen`; concat -> `malloc`+`strcpy`/`strcat`; `s[i]` -> load `i8`, zext to `i32`.
- Arrays `[T]`: pointer to a length-prefixed heap block `[ i64 len, T elems... ]`. `len(a)` loads the header; `a[i]` is a `getelementptr` past the header. Literals `malloc` then store length + elements.
- Structs: LLVM named types (`%Point = type { i32, i32 }`). Locals/params are `alloca`'d (consistent with current param handling); field read is `getelementptr`+`load`; passed by value as LLVM aggregates.
- `print` is a set of recognized builtins backed by libc `printf`/`putchar`, not user functions: `print(str)`, `print_int(i32)`, `print_float(f64)`, `print_char(i32)`, `println()`.
- Casts use an `as` expression: `x as f64` -> `sitofp`, `y as i32` -> `fptosi`/`trunc`.
- Math: `sqrt`, `pow` via LLVM intrinsics (`@llvm.sqrt.f64`, `@llvm.pow.f64`).
- Testing discipline (non-negotiable): **every feature ships with its tests in the same phase, not later.** Each phase below ends with a "Tests" checklist, and a phase is not "done" until those tests pass and the full suite is still green. Phase 8 is only final polish/docs and harness-level additions, never the first time a feature gets a test. Concretely, each new feature adds: (1) a positive `examples/basic` (or `examples/demos`) program that compiles, runs, and asserts behavior; (2) at least one negative case in `examples/invalid` proving the TypeChecker/Parser rejects misuse with a sensible error; and (3) where the feature produces runtime output or an exit code, an output-assertion or native exit-code test in `tests/run_examples.sh`. The existing 53 examples stay green after every phase as the regression gate.

## Phase 0 - Type representation refactor (foundation)

Generalize the two flat enums so later phases just add cases instead of reshaping everything.

- In `include/noria/TypeChecker.hpp`: replace `enum class Type` with a small value type:

```cpp
enum class TypeKind { I32, F64, Bool, Str, Array, Struct, Void };
struct Type {
  TypeKind kind = TypeKind::I32;
  std::shared_ptr<Type> element;   // Array element type
  std::string structName;          // Struct name
  // equality + a name() helper for diagnostics
};
```

- Mirror the same shape in `include/noria/Codegen.hpp` `IrType` (carry element type / struct name so `getelementptr` knows layouts). `struct Value` keeps `{ text, IrType }`.
- Update `parseTypeName`/`typeName`/`isAssignable` in `src/TypeChecker.cpp` and `parseIrType`/`llvmType` in `src/Codegen.cpp` to switch on kind.
- No language behavior change yet; existing 53 examples must still pass. This phase is "green refactor."
- Tests:
  - Run the full existing suite (`tests/run_examples.sh`) before and after; all 53 examples must pass unchanged — this is the entire acceptance bar for Phase 0.
  - Add unit-style coverage for `Type`/`IrType` equality and the `name()` diagnostic helper (a tiny `examples/invalid` case whose error message exercises `name()` for a non-`I32`/`Bool` kind is enough if there's no C++ unit harness).

## Phase 1 - Operators + control-flow polish (Tier 0, cheap wins)

- Lexer (`src/Lexer.cpp`, `include/noria/Token.hpp`): add `Bang(!)`, `AmpAmp(&&)`, `PipePipe(||)`, `Amp(&)`, `Pipe(|)`, `Caret(^)`, `Tilde(~)`, `Shl(<<)`, `Shr(>>)`, `Percent(%)`, and the `as` keyword.
- AST (`include/noria/Ast.hpp`): add `UnaryExpression { op, operand }`; extend `BinaryOperator` with `Modulo, And, Or, BitAnd, BitOr, BitXor, Shl, Shr`; add `CastExpression { expr, targetTypeName }`.
- Parser (`src/Parser.cpp`): add precedence levels — `parseLogicalOr -> parseLogicalAnd -> parseEquality -> parseComparison -> parseBitOr -> parseBitXor -> parseBitAnd -> parseShift -> parseAddition -> parseMultiplication(+ %) -> parseUnary(!,-,~) -> parseCast(as) -> parsePrimary`. Make `else` optional and support `else if` (after a `then` block, optionally consume `else` followed by either a block or another `if`).
- TypeChecker + Codegen: `&&`/`||` short-circuit via branches/phi (great interview talking point); unary `-` -> `sub 0`, `!` -> `xor true`, `~` -> `xor -1`; bitwise/`%` -> `and/or/xor/shl/ashr/srem`.
- Tests:
  - Convert `examples/future/unary_operators.noria` and `logical_operators.noria` into passing `examples/basic` tests.
  - Positive: one example per operator group — `%`, bitwise (`& | ^ ~ << >>`), unary (`! - ~`), `&&`/`||` short-circuit (assert the RHS side effect does *not* run when short-circuited), and `else if` / optional-`else` control flow.
  - Negative (`examples/invalid`): `&&`/`||` on non-`Bool`, bitwise ops on non-integers, and a cast-precedence/`as`-on-bad-type case rejected by the TypeChecker.
  - Add native exit-code assertions for at least the short-circuit and `else if` programs in `tests/run_examples.sh`.

## Phase 2 - Floats, I/O, casts (Tier 0, the "now it's demoable" milestone)

- f64 end to end: lexer float literals (`3.14`), AST `FloatLiteral`, typechecker `TypeKind::F64`, codegen `double` with `fadd/fsub/fmul/fdiv` and `fcmp`.
- Expression statements: add `ExpressionStatement` to the AST + parser so a bare call like `print("hi");` is a valid statement (today only return/let/assign/if/while exist). Needed before `print` works.
- `print` builtins recognized in TypeChecker + Codegen, emitting libc calls (declare `printf`, `putchar`).
- `as` casts: `sitofp`/`fptosi`/`trunc`/`zext`.
- Math builtins `sqrt`, `pow` via intrinsics.
- Tests:
  - Acceptance examples (positive, with output assertions in `tests/run_examples.sh`): FizzBuzz (`print_int` + `%`), a float-math program (`fadd/fmul/fdiv`, `fcmp`), and a "hello world" via `print`.
  - `as` casts: round-trip example (`i32 -> f64 -> i32`) asserting the expected value; float-precision-loss case documented in the example.
  - Math builtins: `sqrt`/`pow` example checking known values (e.g. `sqrt(2.0)`, `pow(2.0, 10.0)`).
  - Negative (`examples/invalid`): `print_int("str")` (wrong builtin arg type), bare non-call expression statement that should still be rejected if disallowed, and an invalid cast (`Bool as f64` if disallowed).
  - This is the "demoable milestone" gate: the three acceptance programs must produce exact expected stdout under the test harness.

## Phase 3 - Strings (Tier 1)

- Lexer: string literals `"..."` with escapes -> `String` token.
- AST `StringLiteral`; emit as private global `i8` constant. Type `TypeKind::Str`.
- Operations: `s[i]` (load `i8` -> zext `i32`), `len(s)` (strlen), `+` concat (malloc + strcpy/strcat), `print(s)`.
- Tests:
  - Convert `examples/future/string_output.noria` to a passing example.
  - Positive (output-asserted): `print(s)`, `len(s)` (compare against expected length), `s[i]` indexing into an `i32` and printing it, and `+` concat producing an asserted combined string. Include an escape-sequence example (`"\n\t\""`).
  - Negative (`examples/invalid`): `len(42)` on a non-string/array, indexing a string with a non-`i32`, and concatenating a string with a non-string.
  - Run under ASan/Valgrind in CI for the concat path (malloc + strcpy/strcat) to catch overflow/off-by-one before merging.

## Phase 4 - Arrays + lvalue assignment (Tier 1)

- The key generalization: assignment currently only handles `identifier = expr` (`src/Parser.cpp` `parseStatement`). Introduce place/lvalue expressions so `arr[i] = v` and (Phase 5) `p.field = v` work. `AssignmentStatement.lhs` becomes an lvalue `Expression` instead of a `string`.
- AST: `IndexExpression { array, index }`, `ArrayLiteral { elements }`; type `[T]`.
- Codegen: array literal `malloc(8 + n*sizeof(T))`, store `len` then elements; `a[i]` getelementptr+load (rvalue) or address (lvalue); `len(a)` loads header.
- Convert `examples/future/arrays_sum.noria` to a passing example; add bounds-check-free indexing (document as a known limitation, or add an optional checked mode).
- Tests:
  - Positive (output-asserted): array literal + sum loop, `len(a)` from the header, read-then-write `a[i] = a[i] + 1` proving the new lvalue path, and a nested-index expression.
  - Lvalue regression: keep an existing `identifier = expr` example green after `AssignmentStatement.lhs` becomes an lvalue `Expression` (this is the riskiest refactor — guard it explicitly).
  - Negative (`examples/invalid`): indexing a non-array, `a[i] = v` with a type-mismatched `v`, and assigning to a non-lvalue (e.g. `len(a) = 3`).
  - ASan/Valgrind run for array-literal `malloc` sizing and getelementptr offsets.

## Phase 5 - Structs (Tier 1)

- Lexer/parser: `struct Name { field: T; ... }` declarations; `Name { field: expr, ... }` construction; `expr.field` access.
- AST: `StructDecl`, `StructLiteral`, `FieldAccessExpression`; extend `ast::Module` to hold struct declarations.
- TypeChecker: register struct types/fields; check construction + field types; field access typing.
- Codegen: emit `%Name = type {...}`; construction via alloca+stores; `p.field` via getelementptr (works as both rvalue and lvalue, reusing Phase 4 lvalue path); pass structs by value.
- Convert `examples/future/struct_point.noria` to a passing example.
- Tests:
  - Positive (output-asserted): construct a struct, read fields, mutate via `p.field = v` (reuses Phase 4 lvalue path), pass a struct by value into a function and confirm caller's copy is unchanged, and a struct containing an array/string field.
  - Negative (`examples/invalid`): unknown field access, construction with a missing/extra/mis-typed field, and accessing `.field` on a non-struct.
  - ASan/Valgrind run for `getelementptr` field offsets and by-value aggregate passing.

## Phase 6 - Demo 1: Brainfuck interpreter (in Noria)

- `examples/demos/brainfuck.noria`: byte-array tape, data pointer, instruction pointer, a `while` loop dispatching on `program[ip]`; `.`/`,` use `print_char`/getchar, `[`/`]` use bracket matching.
- Exercises strings (program source), arrays (tape), indexing, loops, `print_char`. Ship a "Hello World!" program string and assert output.
- Tests:
  - Add the Brainfuck demo to the suite as an output-assertion test: run the bundled "Hello World!" program and assert stdout equals `Hello World!\n` (or exact expected bytes).
  - Add a second tiny BF program (e.g. a `+`/`.` counter) to exercise the `[`/`]` bracket-matching path and assert its output.

## Phase 7 - Demo 2: PPM ray tracer (in Noria)

- `examples/demos/raytracer.noria`: vec3 `struct`, ray-sphere intersection (needs `sqrt`), a few spheres, simple diffuse shading, output `.ppm` via `print`/`print_int`. Render -> convert to PNG -> embed image in README.
- If structs slip, fallback: represent vectors as functions over three `f64` params (keeps the demo unblocked).
- Tests:
  - Add the ray tracer to the suite as a deterministic golden-output test: render at a fixed small resolution and assert the emitted `.ppm` bytes match a checked-in golden file (regenerate the golden only via an explicit, reviewed update).
  - Keep render resolution tiny in CI so the test runs fast; the README image can be a separate higher-res render.

## Phase 8 - Docs, tests, polish

- By this phase the per-feature tests already exist (each phase added them). Phase 8 is final polish, not the first time anything gets tested.
- Update `README.md` and `SYNTAX.md` (move features out of "Limitations"; add new sections + the ray-tracer image).
- Harness-level test work only: audit `tests/run_examples.sh` for coverage gaps across all phases, add any missing cross-feature integration examples, ensure the ASan/Valgrind and golden-output runs are wired into CI, and confirm the full positive + negative + output-assertion suite passes from a clean checkout.
- Update `src/AstPrinter.cpp` for the new node kinds so `--emit-ast` stays accurate, and add/refresh an `--emit-ast` snapshot test covering the new node kinds.
- Write the resume bullet describing the V2 feature set.

## Timeline (now -> end of July, ~6 weeks part-time)

- Week 1: Phase 0 + Phase 1 (refactor + operators/control-flow).
- Week 2: Phase 2 (floats, print, casts) -> demoable milestone.
- Week 3: Phase 3 (strings).
- Week 4: Phase 4 (arrays + lvalues).
- Week 5: Phase 5 (structs) + Phase 6 (Brainfuck).
- Week 6: Phase 7 (ray tracer) + Phase 8 (docs/tests). Buffer built in.

Minimum shippable V2 if time runs short (still a major leap, all Tier 0 + most Tier 1 + one demo): Phases 0-4 + Phase 6 (Brainfuck). Stretch: Phases 5 + 7 (structs + ray tracer).

## Risks / notes

- Biggest risk is the lvalue generalization (Phase 4) since it reshapes `AssignmentStatement`; doing it once cleanly unblocks both arrays and struct-field mutation.
- Text-based codegen makes aggregates (structs/arrays) the fiddliest part — getelementptr offsets and `malloc` sizing must be exact. Keep ASan/Valgrind workflows running.
- Keep the existing 53 examples green after every phase as a regression gate.
