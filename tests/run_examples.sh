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

for source in "${ROOT_DIR}"/examples/invalid/*.noria; do
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

for source in "${ROOT_DIR}"/examples/invalid_syntax/*.noria; do
  case "$(basename "${source}")" in
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
