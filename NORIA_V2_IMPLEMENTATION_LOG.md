# Noria V2 — Implementation Log

Entries are append-only; newest entries go at the bottom.

## Phase 2.5, checkpoint 1 — Canonical types

Phase 2.5, checkpoint 1 — Canonical types. Baseline commit `b0ff702`.

### Objective and acceptance

Extract `Type`/`TypeKind` into `Types.hpp`/`Types.cpp`; AST and codegen store canonical `Type` instead of type-name strings; parser uses one parse-type helper; delete `IrType`/`parseIrType`; add `llvmType(const Type&)` adapter; extend type-representation tests with LLVM spellings; preserve all emitted IR, `--emit-ast` output, and diagnostic text contracts.

### Files and behavior changed

New `include/noria/Types.hpp` and `src/Types.cpp` (`TypeKind`, `Type`, free `llvmType`). AST nodes (`LetStatement`, `Parameter`, `Function::returnType`, `CastExpression`) hold `Type`. Parser: single `parseTypeAnnotation` replaces four inline expect sites. TypeChecker: `requireKnownType`; local name parsing removed. Codegen: dropped `IrType`, `parseIrType`, private `llvmType`. AstPrinter prints `Type::name()`. CMakeLists links `src/Types.cpp` into noria and `type_representation_test`.

### Decisions

Unknown spellings parse as `Type::structType`; rejection stays in type checker, preserving `typecheck: unknown type 'x'` and location. `llvmType` is a free function in `Types.cpp` so the representation test links one file, not `Codegen.cpp`. LLVM spellings unchanged from prior codegen, including `ptr` for str/array/struct (plan brief originally said `i8*`; corrected before implementation).

### Tests, sanitizer, results

Extended `type_representation_test.cpp` with `llvmType` assertions (i32, double, i1, void, ptr). All gates green: warning-clean `-Wall -Wextra -Wpedantic`; byte-identical `-O0` IR, `--emit-ast`, and negative-example stderr; `just format` with unrelated hunks reverted; `git diff --check`; `just test` (69 basic, 34 invalid, 5 invalid_syntax) plus C++ tests. Sanitizer not required (mechanical field replacement, no AST ownership change).

### Review findings and resolutions

Reviewer APPROVED with two non-blocking findings, both fixed and re-reviewed clean: removed avoidable `SourceLocation*` out-parameter from `parseTypeAnnotation` (capture `peek().location` at cast site); added missing `<utility>` include in `src/Types.cpp`.

### Limitations and risks

Deferred by design: `isBuiltinName` duplication (TypeChecker/Codegen) → checkpoint 3; `atLocation` formatting (TypeChecker/Parser) → checkpoint 2; `dynamic_cast` dispatch chains → checkpoint 4. `Type` passed by value in some codegen paths; revisit only if profiling shows cost.

### Next unit

Phase 2.5, checkpoint 2 — shared diagnostics (`formatDiagnostic` in `Diagnostic.hpp`, unify Lexer/Parser/TypeChecker location formatting, preserve error text contracts pinned by `tests/run_examples.sh`).

## Phase 2.5, checkpoint 2 — Shared diagnostics

Baseline commit `6955c80`.

### Objective and acceptance

Add `formatDiagnostic` to `Diagnostic.hpp` and unify the three duplicated location-formatting helpers across Lexer, Parser, and TypeChecker while preserving every diagnostic byte.

### Files and behavior changed

`include/noria/Diagnostic.hpp` gained a `DiagnosticStage` scoped enum (Lexer, TypeCheck) and two `formatDiagnostic` overloads; new `src/Diagnostic.cpp` holds the formatter and a file-local `stageLabel` switch; `CMakeLists.txt` registers it on the noria target; `src/Lexer.cpp`, `src/Parser.cpp`, and `src/TypeChecker.cpp` each lost their private anonymous-namespace `atLocation` copy. 47 call sites migrated: 4 lexer, 5 parser, 38 typechecker.

### Semantic and architectural decisions

The stage prefix moved out of ~42 message literals into the `DiagnosticStage` argument rather than staying baked into strings, so the parameter carries real meaning. Parser diagnostics have no stage label today, so the API uses a two-argument overload rather than a `None` enumerator or `std::optional` — no enumerator maps to an empty label, and the parser's five sites stay clean. `Diagnostic.hpp` knows only `SourceLocation`, not `Token`, so the parser's old Token-taking helper was deleted rather than moved. Codegen's location-free `CompileError` throws are internal impossible-state errors and were deliberately left untouched.

### Tests, sanitizer, results

No new C++ target — `formatDiagnostic` is stateless and all three branches (lexer label, typecheck label, no label) are covered by existing negative examples. Acceptance proof was a byte-level stderr snapshot over every `examples/invalid` and `examples/invalid_syntax` program before and after, since `run_examples.sh` uses substring greps that would not catch a doubled label or stray separator. All gates green: warning-clean build, empty IR/AST/stderr snapshot diffs, zero leftover `"typecheck:` or `"lexer:` literals in `src/`, `just format`, `git diff --check`, `just test` at 69 basic / 34 invalid / 5 invalid_syntax. Sanitizer not required.

### Review findings and resolutions

APPROVED with zero findings. Reviewer's extensibility note: adding a Parser stage label would take seven edit sites (enum, label switch, five call sites).

### Limitations and risks

Most of the 38 typecheck strings are pinned only by generic greps, so the stderr snapshot is the real regression net for future diagnostic refactors. Still deferred: `isBuiltinName` duplication → checkpoint 3; `dynamic_cast` dispatch chains → checkpoint 4.

### Next unit

Phase 2.5, checkpoint 3 — builtin registry (`Builtins.hpp` with `BuiltinId` and signature descriptors, TypeChecker validates via the table, Codegen dispatches on `BuiltinId`, migrate print/print_int/print_float/print_char/println/sqrt/pow, add a C++ unit test for lookup and arity).

## Phase 2.5, checkpoint 3 — Builtin registry

Baseline commit `ffd6059`.

### Objective and acceptance

Introduce a header-only builtin registry; TypeChecker validates calls via the table and shared formatters; Codegen dispatches on `BuiltinId`; migrate print family and sqrt/pow; remove duplicated `isBuiltinName` chains; add unit test; all gates green including sanitizer.

### Files and behavior changed

New `include/noria/Builtins.hpp`: `BuiltinId`, `BuiltinSignature` table, `lookupBuiltin`, and shared helpers `builtinArityMatches`, `formatBuiltinArityError`, `formatBuiltinPerArgumentMismatch`, `formatBuiltinAllArgumentsMismatch`. TypeChecker validates via table + helpers; Codegen switches on `BuiltinId`. Migrated: `print`, `print_int`, `print_float`, `print_char`, `println`, `sqrt`, `pow`. Removed duplicated `isBuiltinName` chains and unused `isBuiltin` wrapper after review. `tests/builtin_registry_test.cpp` registered in `CMakeLists.txt`, CTest, and `run_examples.sh`.

### Semantic and architectural decisions

Header-only registry centralizes builtin identity, signatures, and diagnostic formatting so TypeChecker and tests share one source of truth; Codegen maps `BuiltinId` to IR without string matching.

### Tests, sanitizer, results

All gates green: warning-clean build; empty IR/AST diffs (base3 vs after3); `just test`; `just sanitize`.

### Review findings and resolutions

First review BLOCKED — test duplicated formatters; fixed by moving helpers into `Builtins.hpp`. Re-review APPROVED with zero findings. Reviewer extensibility note: adding `len` needs four production edit sites (enum, descriptor, `strlen` decl, Codegen case).

### Limitations and risks

Still deferred: `dynamic_cast` dispatch chains → checkpoint 4.

### Next unit

Phase 2.5, checkpoint 4 — Full Visitor.

## Phase 2.5, checkpoint 4 — Full Visitor

Baseline commit `ac69be9`.

### Objective and acceptance

Introduce a full AST visitor; migrate AstPrinter, TypeChecker, and Codegen off `dynamic_cast` dispatch; smoke-test every node kind; all gates green including sanitizer.

### Files and behavior changed

New `include/noria/AstVisitor.hpp`: `visit` overloads for all 15 current stmt/expr nodes; `accept` on `Expression`/`Statement` and each final node in `Ast.hpp`. Migrated AstPrinter, TypeChecker, Codegen to visitor implementations; removed their `dynamic_cast` chains. Nested `StatementVisitor`/`ExpressionVisitor` (and small probe visitors) in TypeChecker and Codegen; `AstPrintVisitor` in `AstPrinter.cpp`. `tests/visitor_smoke_test.cpp`: smoke AST hits every node kind; registered in `CMakeLists.txt`, CTest, `run_examples.sh`.

### Semantic and architectural decisions

Double dispatch via `accept`/`visit` replaces ad-hoc `dynamic_cast` chains in three compiler stages, giving one traversal contract for printing, type checking, and codegen.

### Tests, sanitizer, results

All gates green: warning-clean build; empty IR/AST diffs (base4 vs after4); `--emit-ast` unchanged; `just test`; `just sanitize`.

### Review findings and resolutions

APPROVED with one non-blocking extensibility note: exhaustive visitors add boilerplate for new nodes.

### Limitations and risks

Extensibility cost: adding `IndexExpression` needs ~16 visitor-propagation edits across seven production files.

### Next unit

Phase 2.5, checkpoint 5 — Compiler facade.

## Phase 2.5, checkpoint 5 — Compiler facade

Baseline commit `1edaa93`.

### Objective and acceptance

Add a compiler facade: `compileSource(source, StopAfter)` returning `CompileOutput`; `StopAfter` stages `{Tokens, Ast, Typed, Ir}`. Preserve early exits (Ast skips typecheck; Tokens skips parse); all gates green.

### Files and behavior changed

New `include/noria/Compiler.hpp` and `src/Compiler.cpp`. `main.cpp` retains CLI, file I/O, `optimizeLlvmIr`, and `buildNativeExecutable`. CMake: `noria_core` OBJECT library shared by noria and tests; `compiler_facade_test` in CMake, CTest, and `run_examples.sh`.

### Semantic and architectural decisions

Facade wraps the existing pipeline without changing stage semantics; main remains the CLI driver for I/O, LLVM optimization, and native executable linking.

### Tests, sanitizer, results

All gates green: empty IR/AST/tokens diffs (base5 vs after5); `just test`. Sanitizer skipped — by-value ownership, no codegen/AST ownership change.

### Review findings and resolutions

APPROVED. Non-blocking: `noria_core` lacks warning/sanitizer compile flags (only `main.cpp` receives them).

### Limitations and risks

OBJECT library compile flags for warnings/sanitizer not yet unified with main target.

### Next unit

Phase 2.5, checkpoint 6 — CodegenContext + IrEmitter.

## Phase 2.5, checkpoint 6 — CodegenContext + IrEmitter

Baseline commit `2fdfb32`.

### Objective and acceptance

Extract mutable codegen module state into `CodegenContext` and LLVM IR emission into `IrEmitter`; preserve byte-identical emitted IR; all gates green including sanitizer.

### Files and behavior changed

New `include/noria/Runtime.hpp` catalog. New `include/noria/IrEmitter.hpp` and `src/IrEmitter.cpp`: `freshTemp`, `freshLabel`, `emitLoad`/`Store`/`Branch`/`Alloca`, `line`. `CodegenContext` replaces mutable `functions_`, `moduleGlobals_`, `nextStringGlobal_` on the generator. `freshTempCounter()` shares the temp counter for let-slot naming. CMake: `IrEmitter.cpp` in `noria_core`; warning and sanitizer compile flags moved onto `noria_core` (fixes checkpoint 5 gap); sanitizer link added to `compiler_facade_test`.

### Semantic and architectural decisions

Codegen state and IR emission split: `CodegenContext` holds function bindings and module globals; `IrEmitter` owns temp/label allocation and common emit helpers, keeping visitors thin.

### Tests, sanitizer, results

All gates green: empty IR/AST diffs (base6 vs after6); `just test`; `just sanitize`.

### Review findings and resolutions

APPROVED. Non-blocking: warnings no longer only on `main.cpp`; `freshTempCounter` exposes the internal counter for let-slot naming.

### Limitations and risks

`freshTempCounter` leaks emitter internals; acceptable until postfix/place refactors slot naming.

### Next unit

Phase 2.5, checkpoint 7 — Postfix + Place foundation.
