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

## Phase 2.5, checkpoint 7 — Postfix + Place foundation

Baseline commit `9f99dca`.

### Objective and acceptance

Introduce postfix call parsing and place/rvalue split in typecheck and codegen; `AssignmentStatement::lhs` becomes `unique_ptr<Expression>`; preserve IR and diagnostic contracts; all gates green including sanitizer.

### Files and behavior changed

Parser: `parsePostfix` extracts calls; `CallExpression::callee` stays a string; bare-ident gate preserves `(f)(1)`/`f(1)(2)` failures; `Identifier`+`Equal` lookahead kept for assignment. TypeChecker: `checkPlace`/`checkRvalue`. Codegen: `generatePlace`/`generateRvalue`; identifier places only; `generatePlace` emits no IR/temps for identifiers. Visitor, AstPrinter, and `visitor_smoke_test` updated; Assign AST prints nested `Identifier`.

### Semantic and architectural decisions

Postfix parsing separates call chaining from primary expressions without widening callee to an expression node yet. Place/rvalue split mirrors future lvalue semantics: places are checked and generated separately from rvalues, with identifiers as the only place form today.

### Tests, sanitizer, results

All gates green: byte-identical `-O0` IR across 69 basic examples; AST Assign-shape diffs in 22 files (justified by nested lhs); `just test`; `just sanitize`.

### Review findings and resolutions

APPROVED. Non-blocking: no direct regression tests for rejected parenthesized/chained calls.

### Limitations and risks

Callee remains a string, not an expression; chained/index/field places deferred. Parenthesized and chained-call rejections rely on existing invalid examples, not dedicated C++ tests.

### Next unit

Phase 2.5, checkpoint 8 — closeout (README.md and SYNTAX.md for Phases 0–2, stale limitations removed, plan checkbox).

## Phase 2.5, checkpoint 8 — Closeout

Baseline commit `db80a0d`.

### Objective and acceptance

Close Phase 2.5 by aligning user-facing docs with implemented Phases 0–2 behavior; remove stale limitations; tick the Phase 2.5 plan checkbox. No compiler changes.

### Files and behavior changed

`README.md` and `SYNTAX.md` updated: `f64`/`str` types, operators, `else if`, `as` casts (including identity), strings-as-literals, void-builtin expression statements, and the builtins table. Stale limitations removed; remaining limitations accurate. `print_float` documented as broken (arm64 variadic `printf` ABI missing in codegen), not advertised as working.

### Semantic and architectural decisions

Docs-only closeout: semantics unchanged; documentation reflects current gates and known debt rather than aspirational features.

### Tests, sanitizer, results

`just test` green. No compiler code touched; sanitizer N/A. Orchestrator ticks Phase 2.5 plan checkbox at commit.

### Review findings and resolutions

First review BLOCKED: expression-stmt scope too broad, cast matrix incomplete, README fence issue. All fixed; re-review APPROVED.

### Limitations and risks

`print_float` remains broken pending arm64 variadic ABI fix in codegen. Phase 3 string operations not yet documented as supported.

### Next unit

Phase 3 — strings finish; smallest end-to-end first (recommend `len(str)` or indexing). Track `print_float` ABI fix as known debt.

## Phase 3 — len(str) -> i32

Baseline commit `a3d5bc3`.

### Objective and acceptance

Add `len(str) -> i32` as the first Phase 3 string builtin: table-driven typecheck, runtime `strlen`, codegen truncates i64 to i32; preserve preexisting AST; all gates green including sanitizer.

### Files and behavior changed

`Builtins.hpp`: `BuiltinId::Len` last in table with `(str) -> i32`. `Runtime.hpp`: declare `i64 @strlen(ptr)`. `Codegen.cpp`: call `strlen`, `trunc i64 to i32`. TypeChecker unchanged (registry-driven). New `examples/basic/string_length.noria` + `.expected`; `examples/invalid/len_wrong_type.noria`. Updated `run_examples.sh`, `builtin_registry_test.cpp`, `SYNTAX.md`, `README.md`. Emitted IR gains an unconditional `strlen` declare in the preamble; AST byte-identical on existing programs.

### Semantic and architectural decisions

`len` follows the checkpoint 3 registry pattern: one descriptor drives typecheck; Codegen maps `BuiltinId::Len` to libc `strlen` with explicit i64→i32 narrowing to match Noria's i32 int model.

### Tests, sanitizer, results

All gates green: `just test` (70 basic, 35 invalid); `just sanitize`.

### Review findings and resolutions

APPROVED. Non-blocking: README example counts stale (70/35); fixed before commit.

### Limitations and risks

Only `str` accepted; no generic `len` for arrays yet. Unconditional `strlen` in module preamble even when unused (harmless linkage). `print_float` ABI debt unchanged.

### Next unit

Phase 3 indexing `s[i]` (`IndexExpression` + `[` `]` tokens) or escape-sequence stdout example; string concat deferred.

## Phase 3 — String indexing s[i]

Baseline commit `3607698`.

### Objective and acceptance

Add `s[i]` via `IndexExpression` and `[`/`]` tokens; postfix index on any postfix base; typecheck `str`/`i32`→`i32`; codegen `getelementptr inbounds i8`, `load i8`, `zext`; reject index places; preserve preexisting IR/AST; all gates green including sanitizer.

### Files and behavior changed

Lexer: `LeftBracket`/`RightBracket`. AST: `IndexExpression`; all `AstVisitor` impls updated. Parser: `parsePostfix` index. TypeChecker/Codegen: place visitors reject; GEP+load+zext. Examples: `string_index` (+expected), `index_non_str_base`, `index_non_i32`, `unclosed_index`. Updated `visitor_smoke_test`, `SYNTAX.md`, `README.md`.

### Semantic and architectural decisions

Postfix indexing on any postfix base; only `str[i32]` accepted, result `i32` (byte). `IndexExpression` not a place until indexed assignment.

### Tests, sanitizer, results

Preexisting IR/AST identical. `just test`; `just sanitize` green.

### Review findings and resolutions

APPROVED. Non-blocking README counts/limitations; fixed before commit.

### Limitations and risks

No indexed assignment — parser retains `Identifier`+`=` lookahead. No bounds checks.

### Next unit

Phase 3 string concat (`str + str`) or escape-sequence stdout example.

## Phase 3 — String concat str + str

Baseline commit `d97e3c3`.

### Objective and acceptance

Add `str + str` string concatenation: typecheck both operands as `str`, result `str`; runtime heap allocation via libc; mixed-type `+` rejected with clear diagnostics; preserve preexisting AST; all gates green including sanitizer; close Phase 3.

### Files and behavior changed

`Runtime.hpp`: declare `malloc`, `strcpy`, `strcat`. TypeChecker: `Add` for `str+str`; mixed-type diagnostic for `str`/`i32` combinations. Codegen: `strlen`×2, `malloc(n+m+1)` as i64, `strcpy` then `strcat`. Examples: `string_concat`, `string_escapes`, `concat_str_i32`, `concat_i32_str`; promoted `string_output` from future. Updated `SYNTAX.md`, `README.md`; Phase 3 plan checkbox ticked by orchestrator. Emitted IR gains three runtime declares; preexisting AST unchanged.

### Semantic and architectural decisions

Concat follows the Phase 3 pattern: type rules in TypeChecker, heap copy in codegen via libc helpers rather than a custom runtime allocator. Mixed-type `+` is a hard error, not implicit coercion.

### Tests, sanitizer, results

All gates green: `just test`; `just sanitize`.

### Review findings and resolutions

APPROVED. Non-blocking: libc name collision risk if Noria later defines its own `malloc`/`strcpy`/`strcat`; README example counts stale — fixed before commit.

### Limitations and risks

Heap-allocated result; no ownership/lifetime model yet. Unconditional runtime declares in module preamble when concat is linked. `print_float` ABI debt unchanged.

### Next unit

Phase 4 — arrays.

## Phase 4 — Arrays read-only ([T], literal, len, index read)

Baseline commit `5f32852`.

### Objective and acceptance

Add read-only arrays: `[T]` type syntax, array literals, `len([T])`, and index read `a[i]`; heap layout `i64` count plus elements at +8; preserve preexisting IR/AST; all gates green including sanitizer.

### Files and behavior changed

AST: `ArrayLiteral`. Parser: `[T]` via `parseType`; postfix index on array bases. TypeChecker: `len([T])` special-case; `IndexExpression` for array bases. Codegen: heap count+elements layout. Promoted `arrays_sum` (exit 18). New `examples/basic/array_*` and `examples/invalid/array_*`; updated `visitor_smoke_test`, `SYNTAX.md`, `README.md`.

### Semantic and architectural decisions

Read-only first: literals allocate count+elements on the heap; `len` and index read follow string-index patterns. Parser deviation: `identifier[…] =` parses so indexed assign fails at typecheck as invalid assignment target, not a parse error.

### Tests, sanitizer, results

Preexisting IR/AST identical. `just test`; `just sanitize`; ASan/UBSan on array IR clean.

### Review findings and resolutions

APPROVED. Non-blocking README arrays limitation; fixed before commit.

### Limitations and risks

No indexed assignment or bounds checks. Heap-allocated literals; no ownership model.

### Next unit

Phase 4 indexed place assignment `a[i] = …`.

## Phase 4 — Indexed place assignment a[i] = …

Baseline commit `f83ec2b`.

### Objective and acceptance

Enable indexed place assignment `a[i] = …` for arrays; preserve string index as rvalue-only; preserve preexisting IR/AST; all gates green including sanitizer; close Phase 4.

### Files and behavior changed

TypeChecker: `PlaceVisitor` accepts array `IndexExpression`; string index remains non-place. Codegen: `generatePlace` emits element GEP via shared `emitArrayElementPointer` (no load); store after RHS. Promoted `array_indexed_assignment` to basic + `.expected`. New negatives: `array_indexed_store_type_mismatch`, `array_indexed_non_i32_index`, `string_index_assignment`. Orchestrator ticks Phase 4 plan checkbox.

### Semantic and architectural decisions

Array index is a place; string index stays rvalue-only. Place codegen reuses `emitArrayElementPointer` for GEP without load, then stores the RHS — symmetric to index read but without zext/load.

### Tests, sanitizer, results

Preexisting IR/AST identical. `just test`; `just sanitize` green.

### Review findings and resolutions

APPROVED. Non-blocking: `len(a)=3` syntax coverage; `[str]` store coverage.

### Limitations and risks

No bounds checks. String indexed assignment still rejected. Heap array literals; no ownership model.

### Next unit

Phase 5 — structs.

## Phase 5 — Struct decl, construction, field rvalue

Baseline commit `a5e9e9d`.

### Objective and acceptance

Add struct declarations, struct literals, and field rvalue access (`p.x`); locals only; preserve preexisting IR/AST; all gates green including sanitizer.

### Files and behavior changed

Lexer: `Struct`/`Dot` tokens. AST: `StructDecl`, `StructLiteral`, `FieldAccessExpression`; `Module::structs`; `collectStructDecls`. Types: `llvmType(Struct)` → `%Name`. Parser: `structLiteralAllowed_` disambiguates struct literals from `if`/`while` bodies. TypeChecker/Codegen: struct decl collection; alloca+stores construction; field GEP+load. Examples: `struct_point`, `struct_field_order`, `struct_copy` + seven invalid; updated `SYNTAX.md`, `visitor_smoke_test`, `type_representation_test`.

### Semantic and architectural decisions

Struct types are nominal LLVM named structs; construction uses stack alloca and per-field stores; field read is GEP+load. Struct literals gated where statement/expr ambiguity exists.

### Tests, sanitizer, results

Preexisting IR/AST byte-identical. `just test`; `just sanitize` green.

### Review findings and resolutions

APPROVED after usage-limit delay. Non-blocking: initializer evaluation order follows declaration order; struct parameters accepted early; `p.x =` unreachable at parse; missing cycle/duplicate-declaration negative examples.

### Limitations and risks

Locals only — no struct params/returns, field assignment, or heap structs. No struct cycle or duplicate-decl negatives yet.

### Next unit

Phase 5 field lvalue `p.x = v`.

## Phase 5 — Field lvalue p.x = v

Baseline commit `a16f327`.

### Objective and acceptance

Enable struct field place assignment `p.x = v`; preserve preexisting IR/AST; all gates green including sanitizer.

### Files and behavior changed

Parser: `tryParseAssignmentStatement` backtracks postfix LHS so `p.x = v` parses as assignment. TypeChecker: `PlaceVisitor` accepts `FieldAccessExpression`. Codegen: `generatePlace` emits field GEP via `emitStructFieldPointer` (no load), then stores RHS. Examples: `struct_field_assign`, `struct_field_assign_nested`, `struct_field_assign_str`; negatives for non-struct, temporary, type mismatch, unknown field; `field_access_statement` invalid_syntax. Updated `SYNTAX.md`, `README.md`.

### Semantic and architectural decisions

Field access on a placeable base is an lvalue; field GEP reuses `emitStructFieldPointer` without load, symmetric to indexed array assignment.

### Tests, sanitizer, results

Preexisting IR/AST identical. `just test`; `just sanitize` green.

### Review findings and resolutions

APPROVED. Non-blocking: IR greps loose; README counts (fixed); alternating `a[i].inner.x` root extraction gap.

### Limitations and risks

No struct params/returns yet. Chained/alternating array+field place roots not fully covered.

### Next unit

Phase 5 struct params/returns + promote full `struct_point`.

## Phase 5 — Struct params/returns and Phase 5 closeout

Baseline commit `2566f9a`.

### Objective and acceptance

Close Phase 5 with struct by-value params/returns, regression examples, and docs; preserve preexisting IR/AST; all gates green including sanitizer.

### Files and behavior changed

Codegen: `defaultIrValue` returns `zeroinitializer` for structs, `null` for str/array. Parser: `parseCallArguments` restores `structLiteralAllowed_` so struct literals parse as call arguments. Promoted full `struct_point` (exit 7). New basics: `struct_param_by_value` (exit 106), `struct_default_return`, `struct_param_aggregate_fields`, `struct_literal_argument_in_condition`. New invalid: `struct_argument_non_struct`, `struct_argument_type_mismatch`, `struct_return_type_mismatch`. Updated `SYNTAX.md`, `README.md`, `run_examples.sh`. Orchestrator ticks Phase 5 plan checkbox.

### Semantic and architectural decisions

Params/returns already largely worked from prior struct work; this wake locked tests and small fixes. Struct passing remains by-value aggregate copy; callee field mutations do not affect caller locals.

### Tests, sanitizer, results

Preexisting IR/AST byte-identical. `just test`; `just sanitize` green.

### Review findings and resolutions

APPROVED. Non-blocking: by-value isolation test expected exit corrected to 106 before commit.

### Limitations and risks

Still no heap structs. Chained array+field place roots partially covered. Struct cycle/duplicate-decl negatives unchanged from prior checkpoint.

### Next unit

Phase 6 — generics/modules; smallest end-to-end slice first.

## Phase 6 — Source imports (stdlib modules)

Baseline commit `fb772c6`.

### Objective and acceptance

Add `import std::name::{exports};` at module top; resolve stdlib `.noria` files via `ModuleResolver`, merge imported declarations into one `Module`; preserve preexisting IR/AST and diagnostic contracts; all gates green including sanitizer.

### Files and behavior changed

New `ModuleResolver.hpp`/`ModuleResolver.cpp`: path lookup under `CompileOptions::stdlibRoot`, parse cache for diamond imports, merge exports into caller module. Lexer/Parser: `Import` token and import statement parsing (top-level only). `Compiler.hpp`/`Compiler.cpp`: `stdlibRoot`; CLI `--stdlib`. `stdlib/mathx.noria` sample module. Examples: `import_math`, `import_two_names`, `import_twice_same_module`; invalid_syntax: missing module, unknown export, import after function. `tests/module_resolver_test.cpp`; updated `SYNTAX.md`, `README.md`, `run_examples.sh`.

### Semantic and architectural decisions

Imports are compile-time merge only — TypeChecker and Codegen unchanged. `std::` prefix selects stdlib root; named export list required. Diamond imports parse each file once via resolver cache.

### Tests, sanitizer, results

Preexisting IR/AST byte-identical. `just test`; `just sanitize` green.

### Review findings and resolutions

APPROVED. Non-blocking: duplicate declaration in imported module not rejected; `argv[0]`-relative stdlib path should canonicalize; parse-once instrumentation deferred.

### Limitations and risks

No user module paths yet; stdlib-only. Import placement and export validation only partially covered by negatives. Duplicate symbols across imports unresolved.

### Next unit

Phase 6 generics scaffold or `SourceLocation` file attribution for imported diagnostics.
