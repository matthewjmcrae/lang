# Noria V2 — Implementation Log

Entries are append-only; newest entries go at the bottom.

## Unit

Phase 2.5, checkpoint 1 — Canonical types. Baseline commit `b0ff702`.

## Objective and acceptance

Extract `Type`/`TypeKind` into `Types.hpp`/`Types.cpp`; AST and codegen store canonical `Type` instead of type-name strings; parser uses one parse-type helper; delete `IrType`/`parseIrType`; add `llvmType(const Type&)` adapter; extend type-representation tests with LLVM spellings; preserve all emitted IR, `--emit-ast` output, and diagnostic text contracts.

## Files and behavior changed

New `include/noria/Types.hpp` and `src/Types.cpp` (`TypeKind`, `Type`, free `llvmType`). AST nodes (`LetStatement`, `Parameter`, `Function::returnType`, `CastExpression`) hold `Type`. Parser: single `parseTypeAnnotation` replaces four inline expect sites. TypeChecker: `requireKnownType`; local name parsing removed. Codegen: dropped `IrType`, `parseIrType`, private `llvmType`. AstPrinter prints `Type::name()`. CMakeLists links `src/Types.cpp` into noria and `type_representation_test`.

## Decisions

Unknown spellings parse as `Type::structType`; rejection stays in type checker, preserving `typecheck: unknown type 'x'` and location. `llvmType` is a free function in `Types.cpp` so the representation test links one file, not `Codegen.cpp`. LLVM spellings unchanged from prior codegen, including `ptr` for str/array/struct (plan brief originally said `i8*`; corrected before implementation).

## Tests, sanitizer, results

Extended `type_representation_test.cpp` with `llvmType` assertions (i32, double, i1, void, ptr). All gates green: warning-clean `-Wall -Wextra -Wpedantic`; byte-identical `-O0` IR, `--emit-ast`, and negative-example stderr; `just format` with unrelated hunks reverted; `git diff --check`; `just test` (69 basic, 34 invalid, 5 invalid_syntax) plus C++ tests. Sanitizer not required (mechanical field replacement, no AST ownership change).

## Review findings and resolutions

Reviewer APPROVED with two non-blocking findings, both fixed and re-reviewed clean: removed avoidable `SourceLocation*` out-parameter from `parseTypeAnnotation` (capture `peek().location` at cast site); added missing `<utility>` include in `src/Types.cpp`.

## Limitations and risks

Deferred by design: `isBuiltinName` duplication (TypeChecker/Codegen) → checkpoint 3; `atLocation` formatting (TypeChecker/Parser) → checkpoint 2; `dynamic_cast` dispatch chains → checkpoint 4. `Type` passed by value in some codegen paths; revisit only if profiling shows cost.

## Next unit

Phase 2.5, checkpoint 2 — shared diagnostics (`formatDiagnostic` in `Diagnostic.hpp`, unify Lexer/Parser/TypeChecker location formatting, preserve error text contracts pinned by `tests/run_examples.sh`).
