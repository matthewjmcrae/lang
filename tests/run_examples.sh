#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi
TEST_OUT_DIR="${BUILD_DIR}/test-output"
NORIA="${BUILD_DIR}/noria"
NORIA_PREFIX="${NORIA_PREFIX:-}"
read -r -a NORIA_PREFIX_ARGS <<<"${NORIA_PREFIX}"

LLVM_BIN="${LLVM_BIN:-}"
LLC_TRIPLE=""
if [[ -z "${LLVM_BIN}" ]]; then
  if [[ -x "/opt/homebrew/opt/llvm/bin/llc" ]]; then
    LLVM_BIN="/opt/homebrew/opt/llvm/bin"
  elif [[ -x "/opt/homebrew/opt/llvm@15/bin/llc" ]]; then
    LLVM_BIN="/opt/homebrew/opt/llvm@15/bin"
  elif command -v llc >/dev/null 2>&1; then
    LLVM_BIN="$(dirname "$(command -v llc)")"
  else
    LLVM_BIN=""
  fi
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
  if [[ "$(uname -m)" == "arm64" ]]; then
    LLC_TRIPLE="arm64-apple-macosx"
  else
    LLC_TRIPLE="x86_64-apple-macosx"
  fi
elif [[ "$(uname -s)" == "Linux" ]]; then
  if [[ "$(uname -m)" == "aarch64" ]]; then
    LLC_TRIPLE="aarch64-unknown-linux-gnu"
  else
    LLC_TRIPLE="x86_64-unknown-linux-gnu"
  fi
fi

mkdir -p "${TEST_OUT_DIR}"

run_noria() {
  if [[ -n "${NORIA_PREFIX}" ]]; then
    "${NORIA_PREFIX_ARGS[@]}" "${NORIA}" "$@"
  else
    "${NORIA}" "$@"
  fi
}

echo "[noria-tests] configure"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" >/dev/null

echo "[noria-tests] build"
cmake --build "${BUILD_DIR}" >/dev/null

compile_example() {
  local source="$1"
  local name
  name="$(basename "${source}" .noria)"
  local llvm_ir="${TEST_OUT_DIR}/${name}.ll"

  echo "[noria-tests] compile ${source#${ROOT_DIR}/}"
  run_noria "${source}" -o "${llvm_ir}"
  test -s "${llvm_ir}"
}

run_native_exit_test() {
  local source="$1"
  local expected_exit="$2"
  local name
  name="$(basename "${source}" .noria)"
  local llvm_ir="${TEST_OUT_DIR}/${name}.ll"
  local executable="${TEST_OUT_DIR}/${name}"

  if ! command -v clang >/dev/null 2>&1; then
    echo "[noria-tests] skip native ${source#${ROOT_DIR}/}: clang not found"
    return
  fi

  echo "[noria-tests] native ${source#${ROOT_DIR}/} -> exit ${expected_exit}"
  clang "${llvm_ir}" -o "${executable}"

  set +e
  "${executable}"
  local actual_exit="$?"
  set -e

  if [[ "${actual_exit}" != "${expected_exit}" ]]; then
    echo "[noria-tests] expected exit ${expected_exit}, got ${actual_exit} for ${source}" >&2
    exit 1
  fi
}

run_native_failure_test() {
  local source="$1"
  local expected_exit="$2"
  local expected_stderr="${3:-}"
  local name
  name="$(basename "${source}" .noria)"
  local llvm_ir="${TEST_OUT_DIR}/${name}.ll"
  local executable="${TEST_OUT_DIR}/${name}"
  local stderr_file="${TEST_OUT_DIR}/${name}.runtime.stderr"

  if ! command -v clang >/dev/null 2>&1; then
    echo "[noria-tests] skip native failure ${source#${ROOT_DIR}/}: clang not found"
    return
  fi

  echo "[noria-tests] native failure ${source#${ROOT_DIR}/} -> exit ${expected_exit}"
  clang "${llvm_ir}" -o "${executable}"

  set +e
  if [[ -n "${expected_stderr}" ]]; then
    "${executable}" >/dev/null 2>"${stderr_file}"
  else
    "${executable}"
  fi
  local actual_exit="$?"
  set -e

  if [[ "${actual_exit}" != "${expected_exit}" ]]; then
    echo "[noria-tests] expected exit ${expected_exit}, got ${actual_exit} for ${source}" >&2
    exit 1
  fi

  if [[ -n "${expected_stderr}" ]]; then
    grep -q "${expected_stderr}" "${stderr_file}"
  fi
}

run_native_stdout_test() {
  local source="$1"
  local expected_file="$2"
  local name
  name="$(basename "${source}" .noria)"
  local llvm_ir="${TEST_OUT_DIR}/${name}.ll"
  local executable="${TEST_OUT_DIR}/${name}_stdout"
  local actual_file="${TEST_OUT_DIR}/${name}.stdout"

  if ! command -v clang >/dev/null 2>&1; then
    echo "[noria-tests] skip stdout ${source#${ROOT_DIR}/}: clang not found"
    return
  fi

  echo "[noria-tests] stdout ${source#${ROOT_DIR}/}"
  clang "${llvm_ir}" -o "${executable}"
  "${executable}" >"${actual_file}"

  if ! diff -u "${expected_file}" "${actual_file}"; then
    echo "[noria-tests] stdout mismatch for ${source}" >&2
    exit 1
  fi
}

expect_compile_failure() {
  local source="$1"
  local name
  name="$(basename "${source}" .noria)"
  local stderr_file="${TEST_OUT_DIR}/${name}.stderr"

  echo "[noria-tests] expect failure ${source#${ROOT_DIR}/}"
  set +e
  run_noria "${source}" -o "${TEST_OUT_DIR}/${name}.ll" >"${TEST_OUT_DIR}/${name}.stdout" 2>"${stderr_file}"
  local status="$?"
  set -e

  if [[ "${status}" == "0" ]]; then
    echo "[noria-tests] expected compile failure for ${source}" >&2
    exit 1
  fi

  grep -q "typecheck:" "${stderr_file}"
}

expect_compile_failure_contains() {
  local source="$1"
  local expected="$2"
  local name
  name="$(basename "${source}" .noria)"
  local stderr_file="${TEST_OUT_DIR}/${name}.stderr"

  echo "[noria-tests] expect failure ${source#${ROOT_DIR}/}"
  set +e
  run_noria "${source}" -o "${TEST_OUT_DIR}/${name}.ll" >"${TEST_OUT_DIR}/${name}.stdout" 2>"${stderr_file}"
  local status="$?"
  set -e

  if [[ "${status}" == "0" ]]; then
    echo "[noria-tests] expected compile failure for ${source}" >&2
    exit 1
  fi

  grep -q "${expected}" "${stderr_file}"
}

for source in "${ROOT_DIR}"/examples/basic/*.noria; do
  compile_example "${source}"
done

echo "[noria-tests] emit tokens examples/basic/lexer_smoke.noria"
run_noria --emit-tokens "${ROOT_DIR}/examples/basic/lexer_smoke.noria" \
  -o "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q 'let "let"' "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q '>= ">="' "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q '!= "!="' "${TEST_OUT_DIR}/lexer_smoke.tokens"

echo "[noria-tests] emit ast examples/basic/ast_smoke.noria"
run_noria --emit-ast "${ROOT_DIR}/examples/basic/ast_smoke.noria" \
  -o "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Module" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Function factorial(n: i32) -> i32" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Let value: i32" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "If" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Call factorial" "${TEST_OUT_DIR}/ast_smoke.ast"

echo "[noria-tests] type future params examples/basic/type_future_params_smoke.noria"
grep -q "define i32 @consume_f64(double" "${TEST_OUT_DIR}/type_future_params_smoke.ll"
grep -q "define i32 @consume_str(ptr" "${TEST_OUT_DIR}/type_future_params_smoke.ll"

echo "[noria-tests] type representation unit tests"
"${BUILD_DIR}/type_representation_test"
"${BUILD_DIR}/builtin_registry_test"
"${BUILD_DIR}/visitor_smoke_test"
"${BUILD_DIR}/compiler_facade_test"
"${BUILD_DIR}/module_resolver_test"
"${BUILD_DIR}/diagnostic_location_test"
"${BUILD_DIR}/generics_test"
"${BUILD_DIR}/constraints_test"

for source in "${ROOT_DIR}"/examples/invalid/*.noria; do
  case "$(basename "${source}")" in
    import_private_runtime.noria)
      expect_compile_failure_contains "${source}" \
        "import: module 'std::internal::rt' is internal and cannot be imported"
      continue
      ;;
  esac
  expect_compile_failure "${source}"
done

grep -q "2:10: typecheck: unknown local variable 'missing'" \
  "${TEST_OUT_DIR}/unknown_variable.stderr"

echo "[noria-tests] future type name() diagnostics"
grep -q "typecheck: cannot initialize 'x' of type f64 with bool" \
  "${TEST_OUT_DIR}/f64_bool_mismatch.stderr"
grep -q "typecheck: cannot assign bool to variable 'x' of type f64" \
  "${TEST_OUT_DIR}/f64_assignment_mismatch.stderr"
grep -q "typecheck: return type i32 does not match expected f64" \
  "${TEST_OUT_DIR}/f64_return_mismatch.stderr"
grep -q "typecheck: argument 1 of 'take' expects f64, got bool" \
  "${TEST_OUT_DIR}/f64_argument_mismatch.stderr"
grep -q "typecheck: cannot initialize 'x' of type f64 with i32" \
  "${TEST_OUT_DIR}/f64_i32_mismatch.stderr"

grep -q "typecheck: cannot initialize 's' of type str with i32" \
  "${TEST_OUT_DIR}/str_i32_mismatch.stderr"
grep -q "typecheck: cannot assign i32 to variable 'text' of type str" \
  "${TEST_OUT_DIR}/str_assignment_mismatch.stderr"
grep -q "typecheck: return type i32 does not match expected str" \
  "${TEST_OUT_DIR}/str_return_mismatch.stderr"
grep -q "typecheck: argument 1 of 'take' expects str, got i32" \
  "${TEST_OUT_DIR}/str_argument_mismatch.stderr"

grep -q "typecheck: unknown type 'widget'" \
  "${TEST_OUT_DIR}/unknown_future_type.stderr"

echo "[noria-tests] phase 1 operator diagnostics"
grep -q "typecheck: logical operator requires bool operands, got i32 and bool" \
  "${TEST_OUT_DIR}/logical_non_bool.stderr"
grep -q "typecheck: integer operator requires i32 operands, got bool and bool" \
  "${TEST_OUT_DIR}/bitwise_non_integer.stderr"
grep -q "typecheck: cannot cast bool to f64" \
  "${TEST_OUT_DIR}/cast_bad_type.stderr"

echo "[noria-tests] phase 2 io and cast diagnostics"
grep -q "typecheck: print_int expects i32, got str" \
  "${TEST_OUT_DIR}/print_int_wrong_type.stderr"
grep -q "typecheck: expression statement must be a function call" \
  "${TEST_OUT_DIR}/bare_expression_statement.stderr"

echo "[noria-tests] phase 3 string length diagnostics"
grep -q "typecheck: len expects str or array, got i32" \
  "${TEST_OUT_DIR}/len_wrong_type.stderr"

echo "[noria-tests] phase 6 generic diagnostics"
grep -q "typecheck: function 'id' expects 1 argument(s), got 2" \
  "${TEST_OUT_DIR}/generic_wrong_arity.stderr"
grep -q "typecheck: cannot infer type parameter 'T'" \
  "${TEST_OUT_DIR}/generic_uninferable.stderr"
grep -q "typecheck: unknown type 'T'" \
  "${TEST_OUT_DIR}/generic_unresolved_type_param.stderr"
grep -q "typecheck: conflicting types i32 and bool for type parameter 'T'" \
  "${TEST_OUT_DIR}/generic_conflicting_inference.stderr"
grep -q "typecheck: arithmetic operator requires matching numeric operands, got bool and i32" \
  "${TEST_OUT_DIR}/generic_instantiation_body_error.stderr"
grep -q "typecheck: specialization expansion limit exceeded" \
  "${TEST_OUT_DIR}/generic_recursive_specialization.stderr"
grep -q "typecheck: type 'Box<i32, bool>' expects 1 type argument(s), got 2" \
  "${TEST_OUT_DIR}/generic_struct_wrong_arity.stderr"
grep -q "typecheck: unknown type 'Missing<i32>'" \
  "${TEST_OUT_DIR}/generic_struct_unknown.stderr"
grep -q "typecheck: cannot infer type parameter 'B'" \
  "${TEST_OUT_DIR}/generic_struct_uninferred.stderr"
grep -q "typecheck: type 'Point<i32>' is not generic and cannot take type arguments" \
  "${TEST_OUT_DIR}/generic_struct_non_generic_args.stderr"
grep -q "typecheck: field 'value' of 'Box' expects i32, got bool" \
  "${TEST_OUT_DIR}/generic_struct_field_mismatch.stderr"
grep -q "typecheck: implementation tag 'arr' cannot be used as a type" \
  "${TEST_OUT_DIR}/impl_tag_as_type.stderr"
grep -q "typecheck: type 'Box<i32>' expects 2 type argument(s), got 1" \
  "${TEST_OUT_DIR}/impl_tag_wrong_arity.stderr"
grep -q "typecheck: implementation tag 'bst' requires '<' for key type str" \
  "${TEST_OUT_DIR}/generic_bst_key_unordered.stderr"
grep -q "typecheck: implementation tag 'hashmap' requires 'hash' for key type f64; V2 hashes i32, bool, str" \
  "${TEST_OUT_DIR}/generic_hashmap_key_unhashable.stderr"
grep -q "typecheck: no implementation of 'kind' for tag 'list'" \
  "${TEST_OUT_DIR}/generic_impl_missing_tag.stderr"
grep -q "typecheck: implementation signature of 'kind' does not match other implementations" \
  "${TEST_OUT_DIR}/generic_impl_signature_mismatch.stderr"
grep -q "typecheck: generic function 'kind' mixes tagged and untagged implementations" \
  "${TEST_OUT_DIR}/generic_impl_mixed_tagged.stderr"
grep -q "typecheck: cannot select implementation of 'kind' without an implementation tag in inferred type arguments" \
  "${TEST_OUT_DIR}/generic_impl_untagged_call.stderr"

echo "[noria-tests] phase 3 string index diagnostics"
grep -q "typecheck: index requires str or array base, got i32" \
  "${TEST_OUT_DIR}/index_non_str_base.stderr"
grep -q "typecheck: index requires i32 index, got bool" \
  "${TEST_OUT_DIR}/index_non_i32.stderr"

for source in "${ROOT_DIR}"/examples/invalid_syntax/*.noria; do
  case "$(basename "${source}")" in
    import_after_function.noria)
      expect_compile_failure_contains "${source}" \
        "5:1: imports must appear before other declarations"
      ;;
    import_missing_module.noria)
      expect_compile_failure_contains "${source}" \
        "import: unknown module 'std::nope'"
      ;;
    import_unknown_export.noria)
      expect_compile_failure_contains "${source}" \
        "import: module 'std::mathx' does not export 'cube'"
      ;;
    invalid_token.noria)
      expect_compile_failure_contains "${source}" \
        "2:11: lexer: unexpected character '$'"
      ;;
    missing_call_comma.noria)
      expect_compile_failure_contains "${source}" \
        "6:16: expected ')' after function call arguments"
      ;;
    missing_semicolon.noria)
      expect_compile_failure_contains "${source}" \
        "3:3: expected ';' after variable declaration"
      ;;
    unclosed_block.noria)
      expect_compile_failure_contains "${source}" \
        "7:1: unterminated function body"
      ;;
    unknown_character.noria)
      expect_compile_failure_contains "${source}" \
        "2:12: lexer: unexpected character '@'"
      ;;
    unclosed_index.noria)
      expect_compile_failure_contains "${source}" \
        "3:16: expected ']' after index expression"
      ;;
    field_access_statement.noria)
      expect_compile_failure_contains "${source}" \
        "8:3: expected statement"
      ;;
    generic_empty_type_params.noria)
      expect_compile_failure_contains "${source}" \
        "generic declaration requires at least one type parameter"
      ;;
    generic_duplicate_type_param.noria)
      expect_compile_failure_contains "${source}" \
        "duplicate type parameter 'T'"
      ;;
    generic_unclosed_type_params.noria)
      expect_compile_failure_contains "${source}" \
        "expected '>' after type parameters"
      ;;
    generic_struct_empty_type_params.noria)
      expect_compile_failure_contains "${source}" \
        "generic declaration requires at least one type parameter"
      ;;
    generic_struct_duplicate_type_param.noria)
      expect_compile_failure_contains "${source}" \
        "duplicate type parameter 'T'"
      ;;
    generic_struct_unclosed_type_params.noria)
      expect_compile_failure_contains "${source}" \
        "expected '>' after type parameters"
      ;;
    impl_tag_type_param.noria)
      expect_compile_failure_contains "${source}" \
        "implementation tag 'arr' cannot be a type parameter"
      ;;
    generic_impl_unknown_tag.noria)
      expect_compile_failure_contains "${source}" \
        "unknown implementation tag 'bogus'"
      ;;
    generic_impl_on_non_generic.noria)
      expect_compile_failure_contains "${source}" \
        "impl clause requires a generic function"
      ;;
  esac
done

run_native_exit_test "${ROOT_DIR}/examples/basic/return_zero.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/return_one.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/return_answer.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/assignment.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/assignment_reuse.noria" 14
run_native_exit_test "${ROOT_DIR}/examples/basic/arithmetic.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/bool_variable.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/bool_literal_condition.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/bool_assignment.noria" 5
run_native_exit_test "${ROOT_DIR}/examples/basic/bool_parameter.noria" 11
run_native_exit_test "${ROOT_DIR}/examples/basic/bool_argument_from_call.noria" 8
run_native_exit_test "${ROOT_DIR}/examples/basic/branching_complex.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/comparison_less.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/comparison_greater_equal.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/comparison_equal_false.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/comparison_arithmetic_operands.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/function_call.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/function_call_expression.noria" 14
run_native_exit_test "${ROOT_DIR}/examples/basic/function_with_locals.noria" 13
run_native_exit_test "${ROOT_DIR}/examples/basic/factorial.noria" 120
run_native_exit_test "${ROOT_DIR}/examples/basic/function_call_in_loop.noria" 30
run_native_exit_test "${ROOT_DIR}/examples/basic/if_assignment_true.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/if_assignment_false.noria" 9
run_native_exit_test "${ROOT_DIR}/examples/basic/if_branch_return.noria" 11
run_native_exit_test "${ROOT_DIR}/examples/basic/if_condition_arithmetic.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/lexical_shadowing.noria" 9
run_native_exit_test "${ROOT_DIR}/examples/basic/loop_with_nested_if.noria" 9
run_native_exit_test "${ROOT_DIR}/examples/basic/max_function.noria" 12
run_native_exit_test "${ROOT_DIR}/examples/basic/mutual_recursion_even.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/nested_scope_shadowing_deep.noria" 9
run_native_exit_test "${ROOT_DIR}/examples/basic/nested_while_sum.noria" 30
run_native_exit_test "${ROOT_DIR}/examples/basic/parenthesized_arithmetic.noria" 9
run_native_exit_test "${ROOT_DIR}/examples/basic/recursive_sum.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/showcase_recursive_loop.noria" 33
run_native_exit_test "${ROOT_DIR}/examples/basic/showcase_bool_scope_loop.noria" 48
run_native_exit_test "${ROOT_DIR}/examples/basic/showcase_nested_control.noria" 9
run_native_exit_test "${ROOT_DIR}/examples/basic/showcase_recursion_branching.noria" 9
run_native_exit_test "${ROOT_DIR}/examples/basic/two_functions.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/variables.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/variables_multiple.noria" 20
run_native_exit_test "${ROOT_DIR}/examples/basic/variables_initializer_reference.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/variables_expression_reuse.noria" 27
run_native_exit_test "${ROOT_DIR}/examples/basic/variables_division.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/iterative_factorial.noria" 120
run_native_exit_test "${ROOT_DIR}/examples/basic/iterative_fibonacci.noria" 13
run_native_exit_test "${ROOT_DIR}/examples/basic/while_count.noria" 5
run_native_exit_test "${ROOT_DIR}/examples/basic/while_sum.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/while_with_if.noria" 14
run_native_exit_test "${ROOT_DIR}/examples/basic/unary_operators.noria" 12
run_native_exit_test "${ROOT_DIR}/examples/basic/logical_operators.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/modulo.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/bitwise.noria" 37
run_native_exit_test "${ROOT_DIR}/examples/basic/short_circuit_and.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/short_circuit_or.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/else_if.noria" 6
run_native_exit_test "${ROOT_DIR}/examples/basic/if_without_else.noria" 5
run_native_exit_test "${ROOT_DIR}/examples/basic/cast_identity.noria" 8
run_native_exit_test "${ROOT_DIR}/examples/basic/float_math.noria" 4
run_native_exit_test "${ROOT_DIR}/examples/basic/cast_roundtrip.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/cast_precision_loss.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/math_builtins.noria" 1

echo "[noria-tests] phase 2 stdout acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/hello_world.noria" \
  "${ROOT_DIR}/examples/basic/hello_world.expected"
run_native_stdout_test "${ROOT_DIR}/examples/basic/fizzbuzz.noria" \
  "${ROOT_DIR}/examples/basic/fizzbuzz.expected"

echo "[noria-tests] phase 3 string length acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_length.noria" \
  "${ROOT_DIR}/examples/basic/string_length.expected"
grep -q "call i64 @strlen" "${TEST_OUT_DIR}/string_length.ll"

echo "[noria-tests] phase 3 string index acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_index.noria" \
  "${ROOT_DIR}/examples/basic/string_index.expected"
grep -q "getelementptr inbounds i8" "${TEST_OUT_DIR}/string_index.ll"
grep -q "zext i8" "${TEST_OUT_DIR}/string_index.ll"

echo "[noria-tests] phase 3 string concat acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_concat.noria" \
  "${ROOT_DIR}/examples/basic/string_concat.expected"
grep -q "call ptr @malloc" "${TEST_OUT_DIR}/string_concat.ll"
grep -q "call ptr @strcpy" "${TEST_OUT_DIR}/string_concat.ll"
grep -q "call ptr @strcat" "${TEST_OUT_DIR}/string_concat.ll"

echo "[noria-tests] phase 3 string escape acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_escapes.noria" \
  "${ROOT_DIR}/examples/basic/string_escapes.expected"

echo "[noria-tests] phase 3 string output acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_output.noria" \
  "${ROOT_DIR}/examples/basic/string_output.expected"

echo "[noria-tests] phase 4 array acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/arrays_sum.noria" 18
run_native_stdout_test "${ROOT_DIR}/examples/basic/array_length.noria" \
  "${ROOT_DIR}/examples/basic/array_length.expected"
run_native_stdout_test "${ROOT_DIR}/examples/basic/array_index_read.noria" \
  "${ROOT_DIR}/examples/basic/array_index_read.expected"
run_native_stdout_test "${ROOT_DIR}/examples/basic/array_str_elements.noria" \
  "${ROOT_DIR}/examples/basic/array_str_elements.expected"
run_native_exit_test "${ROOT_DIR}/examples/basic/array_reassign.noria" 3
run_native_stdout_test "${ROOT_DIR}/examples/basic/array_indexed_assignment.noria" \
  "${ROOT_DIR}/examples/basic/array_indexed_assignment.expected"
grep -q "call ptr @malloc" "${TEST_OUT_DIR}/arrays_sum.ll"
grep -q "store i64 4" "${TEST_OUT_DIR}/arrays_sum.ll"
grep -q "getelementptr inbounds i8, ptr .*, i64 8" "${TEST_OUT_DIR}/arrays_sum.ll"

echo "[noria-tests] phase 3 string concat diagnostics"
grep -q "typecheck: string concatenation requires str operands, got str and i32" \
  "${TEST_OUT_DIR}/concat_str_i32.stderr"
grep -q "typecheck: string concatenation requires str operands, got i32 and str" \
  "${TEST_OUT_DIR}/concat_i32_str.stderr"

echo "[noria-tests] phase 4 array diagnostics"
grep -q "typecheck: array literal element 2 has type bool, expected i32" \
  "${TEST_OUT_DIR}/array_literal_mixed_types.stderr"
grep -q "typecheck: cannot infer element type of empty array literal" \
  "${TEST_OUT_DIR}/array_literal_empty.stderr"
grep -Fq "typecheck: cannot initialize 'a' of type [i32] with [f64]" \
  "${TEST_OUT_DIR}/array_element_type_mismatch.stderr"
grep -q "typecheck: index requires str or array base, got i32" \
  "${TEST_OUT_DIR}/array_index_non_array_base.stderr"
grep -q "typecheck: cannot assign f64 to variable 'a' of type i32" \
  "${TEST_OUT_DIR}/array_indexed_store_type_mismatch.stderr"
grep -q "typecheck: str index is not assignable" \
  "${TEST_OUT_DIR}/string_index_assignment.stderr"
grep -q "typecheck: index requires i32 index, got bool" \
  "${TEST_OUT_DIR}/array_indexed_non_i32_index.stderr"
grep -q "getelementptr inbounds i32, ptr %t[0-9]*, i32 1" \
  "${TEST_OUT_DIR}/array_indexed_assignment.ll"
grep -q "store i32 [^,]*, ptr %t[0-9]*" "${TEST_OUT_DIR}/array_indexed_assignment.ll"
grep -q "store i32 99, ptr %t[0-9]*" "${TEST_OUT_DIR}/array_indexed_assignment.ll"
grep -q "typecheck: len expects str or array, got i32" \
  "${TEST_OUT_DIR}/array_len_of_element.stderr"

echo "[noria-tests] phase 5 struct acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_point.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_param_by_value.noria" 106
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_param_aggregate_fields.noria" 23
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_default_return.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_literal_argument_in_condition.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_copy.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_field_assign.noria" 34
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_field_assign_nested.noria" 5
run_native_stdout_test "${ROOT_DIR}/examples/basic/struct_field_assign_str.noria" \
  "${ROOT_DIR}/examples/basic/struct_field_assign_str.expected"
run_native_stdout_test "${ROOT_DIR}/examples/basic/struct_field_order.noria" \
  "${ROOT_DIR}/examples/basic/struct_field_order.expected"
grep -q "%Point = type { i32, i32 }" "${TEST_OUT_DIR}/struct_point.ll"
grep -q "getelementptr inbounds %Point, ptr %t[0-9]*, i32 0, i32 1" \
  "${TEST_OUT_DIR}/struct_field_order.ll"
grep -q "getelementptr inbounds %Point, ptr %t[0-9]*, i32 0, i32 1" \
  "${TEST_OUT_DIR}/struct_field_assign.ll"
grep -q "store i32 [^,]*, ptr %t[0-9]*" "${TEST_OUT_DIR}/struct_field_assign.ll"
grep -q "alloca %Point" "${TEST_OUT_DIR}/struct_point.ll"
grep -q "define %Point @" "${TEST_OUT_DIR}/struct_param_by_value.ll"
grep -q "call %Point @" "${TEST_OUT_DIR}/struct_param_by_value.ll"
grep -q "store %Point %.*\.param" "${TEST_OUT_DIR}/struct_param_by_value.ll"
grep -q "ret %Point zeroinitializer" "${TEST_OUT_DIR}/struct_default_return.ll"

echo "[noria-tests] emit ast examples/basic/struct_point.noria"
run_noria --emit-ast "${ROOT_DIR}/examples/basic/struct_point.noria" \
  -o "${TEST_OUT_DIR}/struct_point.ast"
grep -q "Struct Point" "${TEST_OUT_DIR}/struct_point.ast"

echo "[noria-tests] phase 5 struct diagnostics"
grep -q "typecheck: struct 'Point' has no field 'z'" \
  "${TEST_OUT_DIR}/struct_unknown_field.stderr"
grep -q "typecheck: struct literal for 'Point' is missing field 'y'" \
  "${TEST_OUT_DIR}/struct_missing_field.stderr"
grep -q "typecheck: duplicate field 'x' in struct literal for 'Point'" \
  "${TEST_OUT_DIR}/struct_duplicate_field.stderr"
grep -q "typecheck: field 'x' of 'Point' expects i32, got bool" \
  "${TEST_OUT_DIR}/struct_field_type_mismatch.stderr"
grep -q "typecheck: field access requires struct base, got i32" \
  "${TEST_OUT_DIR}/struct_field_on_non_struct.stderr"
grep -q "typecheck: struct 'Point' has no field 'z'" \
  "${TEST_OUT_DIR}/struct_field_assign_unknown_field.stderr"
grep -q "typecheck: __rt_ptr cannot be used outside the standard library" \
  "${TEST_OUT_DIR}/struct_import_name_collision.stderr"
grep -q "typecheck: __rt_ptr cannot be used outside the standard library" \
  "${TEST_OUT_DIR}/use_private_builtin.stderr"
grep -q "typecheck: internal runtime builtin '__rt_release' is unavailable outside the standard library" \
  "${TEST_OUT_DIR}/use_private_runtime_builtin.stderr"
grep -q "typecheck: field access requires struct base, got i32" \
  "${TEST_OUT_DIR}/struct_field_assign_non_struct.stderr"
grep -q "typecheck: cannot assign bool to variable 'p.x' of type i32" \
  "${TEST_OUT_DIR}/struct_field_assign_type_mismatch.stderr"
grep -q "typecheck: invalid assignment target" \
  "${TEST_OUT_DIR}/struct_field_assign_temporary.stderr"
grep -q "typecheck: unknown type 'Nope'" \
  "${TEST_OUT_DIR}/struct_unknown_type.stderr"
grep -q "typecheck: duplicate struct 'Point'" \
  "${TEST_OUT_DIR}/struct_duplicate_decl.stderr"
grep -q "typecheck: argument 1 of 'm' expects Point, got Other" \
  "${TEST_OUT_DIR}/struct_argument_type_mismatch.stderr"
grep -q "typecheck: argument 1 of 'm' expects Point, got i32" \
  "${TEST_OUT_DIR}/struct_argument_non_struct.stderr"
grep -q "typecheck: return type i32 does not match expected Point" \
  "${TEST_OUT_DIR}/struct_return_type_mismatch.stderr"

echo "[noria-tests] phase 6 import acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/import_math.noria" 25
run_native_exit_test "${ROOT_DIR}/examples/basic/import_shadow_comparison.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/import_two_names.noria" 17
run_native_exit_test "${ROOT_DIR}/examples/basic/import_twice_same_module.noria" 25
grep -c "define i32 @square" "${TEST_OUT_DIR}/import_twice_same_module.ll" | grep -q "^1$"
run_native_exit_test "${ROOT_DIR}/examples/basic/import_impl_family.noria" 3
grep -c 'define i32 @kind$s.i32$tag.arr' "${TEST_OUT_DIR}/import_impl_family.ll" | grep -q "^1$"
grep -c 'define i32 @kind$s.i32$tag.list' "${TEST_OUT_DIR}/import_impl_family.ll" | grep -q "^1$"
run_noria --emit-ast "${ROOT_DIR}/examples/basic/import_math.noria" \
  -o "${TEST_OUT_DIR}/import_math.ast"
grep -q "Import std::mathx {square}" "${TEST_OUT_DIR}/import_math.ast"

echo "[noria-tests] phase 6 private runtime ABI"
run_native_exit_test "${ROOT_DIR}/examples/basic/stdlib_memory_probe.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/stdlib_generic_alloc.noria" 1

echo "[noria-tests] phase 7 sequence acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_push_get.noria" 60
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_f64.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_pop_set.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_growth_mutation.noria" 51
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_pop_empty.noria" 70 \
  "sequence_pop: empty sequence"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_get_oob.noria" 70 \
  "sequence_get: index out of bounds"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_insert_remove.noria" 55
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_insert_growth.noria" 104
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_insert_oob.noria" 70 \
  "sequence_insert: index out of bounds"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_remove_oob.noria" 70 \
  "sequence_remove: index out of bounds"
grep -c '%Sequence$s.i32$tag.arr = type' "${TEST_OUT_DIR}/sequence_push_get.ll" | grep -q "^1$"
grep -c 'define %Sequence$s.i32$tag.arr @sequence_new$s.i32$tag.arr' "${TEST_OUT_DIR}/sequence_push_get.ll" | grep -q "^1$"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_push_get.noria" 40
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_pop_set.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_insert_remove.noria" 55
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_insert_oob.noria" 70 \
  "sequence_insert: index out of bounds"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_remove_oob.noria" 70 \
  "sequence_remove: index out of bounds"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_arr_list_conformance.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_nested_id.noria" 0
grep -c '%Sequence$s.i32$tag.list = type' "${TEST_OUT_DIR}/sequence_list_push_get.ll" | grep -q "^1$"
grep -c 'define %Sequence$s.i32$tag.list @sequence_new$s.i32$tag.list' "${TEST_OUT_DIR}/sequence_list_push_get.ll" | grep -q "^1$"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_pop_empty.noria" 70 \
  "sequence_pop: empty sequence"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_get_oob.noria" 70 \
  "sequence_get: index out of bounds"

run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_insert_get.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_contains_remove.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_get_or.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/dictionary_get_missing.noria" 70 \
  "dictionary_get: key not found"
grep -c '%Dictionary$s.i32$s.i32$tag.bst = type' "${TEST_OUT_DIR}/dictionary_bst_insert_get.ll" | grep -q "^1$"
grep -c 'define %Dictionary$s.i32$s.i32$tag.bst @dictionary_new$s.i32$s.i32$tag.bst' \
  "${TEST_OUT_DIR}/dictionary_bst_insert_get.ll" | grep -q "^1$"

echo "[noria-tests] phase 7 sequence diagnostics"
grep -q "typecheck: no implementation of 'sequence_new' for tag 'bst'" \
  "${TEST_OUT_DIR}/sequence_bst_unsupported.stderr"
grep -q "typecheck: no implementation of 'dictionary_new' for tag 'hashmap'" \
  "${TEST_OUT_DIR}/dictionary_hashmap_unsupported.stderr"
grep -q "typecheck: internal runtime builtin '__rt_load' is unavailable outside the standard library" \
  "${TEST_OUT_DIR}/use_private_rt_load.stderr"
grep -q "typecheck: internal runtime builtin '__rt_trap' is unavailable outside the standard library" \
  "${TEST_OUT_DIR}/use_private_rt_trap.stderr"
grep -q "typecheck: conflicting types i32 and bool for type parameter 'T'" \
  "${TEST_OUT_DIR}/sequence_set_type_mismatch.stderr"
grep -q "typecheck: __rt_sizeof requires a scalar element type, got Point" \
  "${TEST_OUT_DIR}/sequence_struct_element.stderr"

echo "[noria-tests] phase 7.0 private struct field diagnostics"
grep -q "typecheck: field 'handle' is private to module 'std::sequence'" \
  "${TEST_OUT_DIR}/private_field_read.stderr"
grep -q "typecheck: field 'handle' is private to module 'std::sequence'" \
  "${TEST_OUT_DIR}/private_field_assign.stderr"
grep -q "typecheck: field 'handle' is private to module 'std::sequence'" \
  "${TEST_OUT_DIR}/private_struct_literal.stderr"
grep -q "typecheck: field 'handle' is private to module 'std::sequence'" \
  "${TEST_OUT_DIR}/sequence_handle_swap.stderr"

grep -q "typecheck: function 'store_i32_at' is internal to module 'std::internal::rt'" \
  "${TEST_OUT_DIR}/use_transitive_internal_rt.stderr"

run_native_exit_test "${ROOT_DIR}/examples/basic/struct_private_same_module.noria" 52
run_native_exit_test "${ROOT_DIR}/examples/basic/comparison_uppercase_ident.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_lowercase.noria" 42

echo "[noria-tests] phase 7 witness runtime bad stdlib diagnostics"
RT_LOAD_ARITY_STDERR="${TEST_OUT_DIR}/import_rt_load_wrong_arity.stderr"
set +e
run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_rt_load_wrong_arity.noria" \
  -o "${TEST_OUT_DIR}/import_rt_load_wrong_arity.ll" \
  >"${TEST_OUT_DIR}/import_rt_load_wrong_arity.stdout" 2>"${RT_LOAD_ARITY_STDERR}"
rt_load_arity_status="$?"
set -e
if [[ "${rt_load_arity_status}" == "0" ]]; then
  echo "[noria-tests] expected compile failure for import_rt_load_wrong_arity.noria" >&2
  exit 1
fi
grep -q "typecheck: __rt_load expects 2 arguments" "${RT_LOAD_ARITY_STDERR}"

RT_STORE_MISMATCH_STDERR="${TEST_OUT_DIR}/import_rt_store_mismatch.stderr"
set +e
run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_rt_store_mismatch.noria" \
  -o "${TEST_OUT_DIR}/import_rt_store_mismatch.ll" \
  >"${TEST_OUT_DIR}/import_rt_store_mismatch.stdout" 2>"${RT_STORE_MISMATCH_STDERR}"
rt_store_mismatch_status="$?"
set -e
if [[ "${rt_store_mismatch_status}" == "0" ]]; then
  echo "[noria-tests] expected compile failure for import_rt_store_mismatch.noria" >&2
  exit 1
fi
grep -q "typecheck: __rt_store expects i32, got bool" "${RT_STORE_MISMATCH_STDERR}"

echo "[noria-tests] phase 6 import diagnostic file attribution"
BAD_TYPE_STDERR="${TEST_OUT_DIR}/import_bad_type.stderr"
set +e
run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_bad_type.noria" \
  -o "${TEST_OUT_DIR}/import_bad_type.ll" >"${TEST_OUT_DIR}/import_bad_type.stdout" 2>"${BAD_TYPE_STDERR}"
bad_type_status="$?"
set -e
if [[ "${bad_type_status}" == "0" ]]; then
  echo "[noria-tests] expected compile failure for import_bad_type.noria" >&2
  exit 1
fi
grep -q "std::badmath:2:10: typecheck: return type bool does not match expected i32" \
  "${BAD_TYPE_STDERR}"

echo "[noria-tests] phase 6 duplicate export diagnostic"
DUPEXPORT_STDERR="${TEST_OUT_DIR}/import_dupexport.stderr"
set +e
run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_dupexport.noria" \
  -o "${TEST_OUT_DIR}/import_dupexport.ll" >"${TEST_OUT_DIR}/import_dupexport.stdout" 2>"${DUPEXPORT_STDERR}"
dupexport_status="$?"
set -e
if [[ "${dupexport_status}" == "0" ]]; then
  echo "[noria-tests] expected compile failure for import_dupexport.noria" >&2
  exit 1
fi
grep -q "std::dupexport:5:1: import: duplicate function 'dup'" "${DUPEXPORT_STDERR}"

echo "[noria-tests] phase 6 generic acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_id_i32.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_id_cast.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_id_struct_field.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_two_instantiations.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_reuse_same_type.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_reuse_two_paths.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_two_params.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_array_param.noria" 10
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_with_comparison.noria" 42
grep -c 'define i32 @id$s.i32' "${TEST_OUT_DIR}/generic_reuse_same_type.ll" | grep -q "^1$"
grep -c 'define i32 @id$s.i32' "${TEST_OUT_DIR}/generic_reuse_two_paths.ll" | grep -q "^1$"

echo "[noria-tests] phase 6 generic struct acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_box.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_two_params.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_nested.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_reuse.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_array_field.noria" 6
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_infer.noria" 42
grep -c '%Box$s.i32 = type' "${TEST_OUT_DIR}/generic_struct_reuse.ll" | grep -q "^1$"
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_impl_tag.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_impl_tag_distinct.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_impl_select_tag.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_tag_constraint_ok.noria" 43
grep -c '%Box$s.i32$tag.arr = type' "${TEST_OUT_DIR}/generic_impl_tag_distinct.ll" | grep -q "^1$"
grep -c '%Box$s.i32$tag.list = type' "${TEST_OUT_DIR}/generic_impl_tag_distinct.ll" | grep -q "^1$"
grep -c 'define i32 @kind$s.i32$tag.arr' "${TEST_OUT_DIR}/generic_impl_select_tag.ll" | grep -q "^1$"
grep -c 'define i32 @kind$s.i32$tag.list' "${TEST_OUT_DIR}/generic_impl_select_tag.ll" | grep -q "^1$"

echo "[noria-tests] direct build examples/basic/factorial.noria"
run_noria build "${ROOT_DIR}/examples/basic/factorial.noria" -o "${TEST_OUT_DIR}/factorial_direct"
set +e
"${TEST_OUT_DIR}/factorial_direct"
actual_exit="$?"
set -e
if [[ "${actual_exit}" != "120" ]]; then
  echo "[noria-tests] expected exit 120, got ${actual_exit} for direct build" >&2
  exit 1
fi

if [[ -n "${LLVM_BIN}" && -x "${LLVM_BIN}/opt" ]]; then
  echo "[noria-tests] optimized llvm examples/basic/variables.noria"
  run_noria -O2 "${ROOT_DIR}/examples/basic/variables.noria" -o "${TEST_OUT_DIR}/variables.opt.ll"
  grep -q "ret i32 7" "${TEST_OUT_DIR}/variables.opt.ll"

  echo "[noria-tests] optimized direct build examples/basic/factorial.noria"
  run_noria build -O2 "${ROOT_DIR}/examples/basic/factorial.noria" \
    -o "${TEST_OUT_DIR}/factorial_optimized"
  set +e
  "${TEST_OUT_DIR}/factorial_optimized"
  actual_exit="$?"
  set -e
  if [[ "${actual_exit}" != "120" ]]; then
    echo "[noria-tests] expected exit 120, got ${actual_exit} for optimized direct build" >&2
    exit 1
  fi
else
  echo "[noria-tests] skip optimizer checks: opt not found"
fi

echo "[noria-tests] ok"
