#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi
TEST_OUT_DIR="${BUILD_DIR}/test-output"
NORIA="${BUILD_DIR}/noria"
NORIA_PREFIX="${NORIA_PREFIX:-}"
read -r -a NORIA_PREFIX_ARGS <<<"${NORIA_PREFIX}"
NORIA_REQUIRE_LLVM_TOOLS="${NORIA_REQUIRE_LLVM_TOOLS:-0}"

CURRENT_PHASE="initialization"
CURRENT_CASE=""

print_failure_context() {
  printf '[noria-tests] phase: %s\n' "${CURRENT_PHASE}" >&2
  if [[ -n "${CURRENT_CASE}" ]]; then
    printf '[noria-tests] case: %s\n' "${CURRENT_CASE}" >&2
  fi
  printf '[noria-tests] artifacts: %s\n' "${TEST_OUT_DIR}" >&2
}

report_failure() {
  local status="$1"
  local command="$2"
  local line="$3"

  trap - ERR
  printf '[noria-tests] failed command (line %s): %s\n' "${line}" "${command}" >&2
  print_failure_context
  exit "${status}"
}

fail() {
  printf '[noria-tests] error: %s\n' "$1" >&2
  print_failure_context
  exit 1
}

phase() {
  CURRENT_PHASE="$1"
  CURRENT_CASE=""
  echo "[noria-tests] ${CURRENT_PHASE}"
}

set_case() {
  CURRENT_CASE="$1"
}

trap 'report_failure "$?" "$BASH_COMMAND" "$LINENO"' ERR

LLVM_BIN="${LLVM_BIN:-}"
if [[ -n "${LLVM_BIN}" ]]; then
  LLVM_BIN="${LLVM_BIN%/}"
  if [[ -d "${LLVM_BIN}" ]]; then
    export PATH="${LLVM_BIN}:${PATH}"
  elif [[ "${NORIA_REQUIRE_LLVM_TOOLS}" != "0" ]]; then
    fail "LLVM_BIN does not name a directory: ${LLVM_BIN}"
  else
    echo "[noria-tests] warning: LLVM_BIN does not name a directory: ${LLVM_BIN}" >&2
  fi
fi

resolve_tool() {
  local tool="$1"

  if [[ -n "${LLVM_BIN}" && -x "${LLVM_BIN}/${tool}" ]]; then
    printf '%s\n' "${LLVM_BIN}/${tool}"
    return
  fi

  if command -v "${tool}"; then
    return
  fi

  return 0
}

resolve_host_clang() {
  if [[ -x /usr/bin/clang ]]; then
    printf '%s\n' /usr/bin/clang
    return
  fi

  resolve_tool clang
}

CLANG="$(resolve_host_clang)"
OPT="$(resolve_tool opt)"

# Linux needs libm for llvm.sqrt.f64 / llvm.pow.f64; macOS provides them via libSystem.
link_ir() {
  local llvm_ir="$1"
  local executable="$2"
  "${CLANG}" "${llvm_ir}" -lm -o "${executable}"
}

if [[ ! -x "${NORIA}" ]]; then
  fail "compiler not found at ${NORIA}; configure and build before running this harness"
fi

if [[ "${NORIA_REQUIRE_LLVM_TOOLS}" != "0" && -z "${CLANG}" ]]; then
  fail "clang is required when NORIA_REQUIRE_LLVM_TOOLS is enabled"
fi

if [[ "${NORIA_REQUIRE_LLVM_TOOLS}" != "0" && -z "${OPT}" ]]; then
  fail "opt is required when NORIA_REQUIRE_LLVM_TOOLS is enabled"
fi

mkdir -p "${TEST_OUT_DIR}"

run_noria() {
  if [[ -n "${NORIA_PREFIX}" ]]; then
    "${NORIA_PREFIX_ARGS[@]}" "${NORIA}" "$@"
  else
    "${NORIA}" "$@"
  fi
}

compile_example() {
  local source="$1"
  local name
  name="$(basename "${source}" .noria)"
  local llvm_ir="${TEST_OUT_DIR}/${name}.ll"

  set_case "compile ${source#${ROOT_DIR}/}"
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

  set_case "native ${source#${ROOT_DIR}/}"
  if [[ -z "${CLANG}" ]]; then
    echo "[noria-tests] skip native ${source#${ROOT_DIR}/}: clang not found"
    return
  fi

  echo "[noria-tests] native ${source#${ROOT_DIR}/} -> exit ${expected_exit}"
  link_ir "${llvm_ir}" "${executable}"

  local actual_exit
  if "${executable}"; then
    actual_exit=0
  else
    actual_exit="$?"
  fi

  if [[ "${actual_exit}" != "${expected_exit}" ]]; then
    fail "expected exit ${expected_exit}, got ${actual_exit} for ${source}"
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

  set_case "native failure ${source#${ROOT_DIR}/}"
  if [[ -z "${CLANG}" ]]; then
    echo "[noria-tests] skip native failure ${source#${ROOT_DIR}/}: clang not found"
    return
  fi

  echo "[noria-tests] native failure ${source#${ROOT_DIR}/} -> exit ${expected_exit}"
  link_ir "${llvm_ir}" "${executable}"

  local actual_exit
  if [[ -n "${expected_stderr}" ]]; then
    if "${executable}" >/dev/null 2>"${stderr_file}"; then
      actual_exit=0
    else
      actual_exit="$?"
    fi
  else
    if "${executable}"; then
      actual_exit=0
    else
      actual_exit="$?"
    fi
  fi

  if [[ "${actual_exit}" != "${expected_exit}" ]]; then
    fail "expected exit ${expected_exit}, got ${actual_exit} for ${source}"
  fi

  if [[ -n "${expected_stderr}" ]]; then
    grep -q "${expected_stderr}" "${stderr_file}"
  fi
}

run_optimized_native_failure_test() {
  local source="$1"
  local expected_exit="$2"
  local expected_stderr="$3"
  local name
  name="$(basename "${source}" .noria)"
  local executable="${TEST_OUT_DIR}/${name}_optimized"
  local stderr_file="${TEST_OUT_DIR}/${name}.optimized.runtime.stderr"

  set_case "optimized native failure ${source#${ROOT_DIR}/}"
  echo "[noria-tests] optimized native failure ${source#${ROOT_DIR}/} -> exit ${expected_exit}"
  run_noria build -O2 "${source}" -o "${executable}"

  local actual_exit
  if "${executable}" >/dev/null 2>"${stderr_file}"; then
    actual_exit=0
  else
    actual_exit="$?"
  fi

  if [[ "${actual_exit}" != "${expected_exit}" ]]; then
    fail "expected exit ${expected_exit}, got ${actual_exit} for optimized ${source}"
  fi

  grep -q "${expected_stderr}" "${stderr_file}"
}

run_optimized_native_exit_test() {
  local source="$1"
  local expected_exit="$2"
  local name
  name="$(basename "${source}" .noria)"
  local executable="${TEST_OUT_DIR}/${name}_optimized"

  set_case "optimized native ${source#${ROOT_DIR}/}"
  echo "[noria-tests] optimized native ${source#${ROOT_DIR}/} -> exit ${expected_exit}"
  run_noria build -O2 "${source}" -o "${executable}"

  local actual_exit
  if "${executable}"; then
    actual_exit=0
  else
    actual_exit="$?"
  fi

  if [[ "${actual_exit}" != "${expected_exit}" ]]; then
    fail "expected exit ${expected_exit}, got ${actual_exit} for optimized ${source}"
  fi
}

run_optimized_native_stdout_test() {
  local source="$1"
  local expected_file="$2"
  local name
  name="$(basename "${source}" .noria)"
  local executable="${TEST_OUT_DIR}/${name}_optimized_stdout"
  local actual_file="${TEST_OUT_DIR}/${name}.optimized.stdout"

  set_case "optimized native stdout ${source#${ROOT_DIR}/}"
  echo "[noria-tests] optimized native stdout ${source#${ROOT_DIR}/}"
  run_noria build -O2 "${source}" -o "${executable}"
  "${executable}" >"${actual_file}"

  if ! diff -u "${expected_file}" "${actual_file}"; then
    fail "optimized stdout mismatch for ${source}"
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

  set_case "native stdout ${source#${ROOT_DIR}/}"
  if [[ -z "${CLANG}" ]]; then
    echo "[noria-tests] skip stdout ${source#${ROOT_DIR}/}: clang not found"
    return
  fi

  echo "[noria-tests] stdout ${source#${ROOT_DIR}/}"
  link_ir "${llvm_ir}" "${executable}"
  "${executable}" >"${actual_file}"

  if ! diff -u "${expected_file}" "${actual_file}"; then
    fail "stdout mismatch for ${source}"
  fi
}

expect_compile_failure() {
  local source="$1"
  local name
  name="$(basename "${source}" .noria)"
  local stderr_file="${TEST_OUT_DIR}/${name}.stderr"

  set_case "compile failure ${source#${ROOT_DIR}/}"
  echo "[noria-tests] expect failure ${source#${ROOT_DIR}/}"
  local status
  if run_noria "${source}" -o "${TEST_OUT_DIR}/${name}.ll" >"${TEST_OUT_DIR}/${name}.stdout" 2>"${stderr_file}"; then
    status=0
  else
    status="$?"
  fi

  if [[ "${status}" == "0" ]]; then
    fail "expected compile failure for ${source}"
  fi

  grep -q "typecheck:" "${stderr_file}"
}

expect_compile_failure_contains() {
  local source="$1"
  local expected="$2"
  local name
  name="$(basename "${source}" .noria)"
  local stderr_file="${TEST_OUT_DIR}/${name}.stderr"

  set_case "compile failure ${source#${ROOT_DIR}/}"
  echo "[noria-tests] expect failure ${source#${ROOT_DIR}/}"
  local status
  if run_noria "${source}" -o "${TEST_OUT_DIR}/${name}.ll" >"${TEST_OUT_DIR}/${name}.stdout" 2>"${stderr_file}"; then
    status=0
  else
    status="$?"
  fi

  if [[ "${status}" == "0" ]]; then
    fail "expected compile failure for ${source}"
  fi

  grep -q "${expected}" "${stderr_file}"
}

expect_entry_point_failure() {
  local source="$1"
  local expected="$2"
  local name
  name="$(basename "${source}" .noria)"
  local ir_path="${TEST_OUT_DIR}/entry-${name}.ll"
  local executable_path="${TEST_OUT_DIR}/entry-${name}"
  local ir_stderr="${TEST_OUT_DIR}/entry-${name}.ir.stderr"
  local native_stderr="${TEST_OUT_DIR}/entry-${name}.native.stderr"

  rm -f "${ir_path}" "${executable_path}" "${executable_path}.ll"

  set_case "entry-point IR failure ${source#${ROOT_DIR}/}"
  echo "[noria-tests] expect entry-point IR failure ${source#${ROOT_DIR}/}"
  local status
  if run_noria "${source}" -o "${ir_path}" >/dev/null 2>"${ir_stderr}"; then
    status=0
  else
    status="$?"
  fi
  if [[ "${status}" == "0" ]]; then
    fail "expected entry-point compilation failure for ${source}"
  fi
  [[ ! -e "${ir_path}" ]] || fail "compiler wrote IR for invalid entry point ${source}"
  grep -Fq "${source}:" "${ir_stderr}"
  grep -Fq "typecheck: ${expected}" "${ir_stderr}"

  set_case "entry-point native failure ${source#${ROOT_DIR}/}"
  echo "[noria-tests] expect entry-point native failure ${source#${ROOT_DIR}/}"
  if run_noria build "${source}" -o "${executable_path}" >/dev/null 2>"${native_stderr}"; then
    status=0
  else
    status="$?"
  fi
  if [[ "${status}" == "0" ]]; then
    fail "expected entry-point native build failure for ${source}"
  fi
  [[ ! -e "${executable_path}" ]] || fail "compiler linked invalid entry point ${source}"
  [[ ! -e "${executable_path}.ll" ]] || fail "compiler wrote native LLVM IR for ${source}"
  grep -Fq "${source}:" "${native_stderr}"
  grep -Fq "typecheck: ${expected}" "${native_stderr}"
}

phase "compile basic examples"
for source in "${ROOT_DIR}"/examples/basic/*.noria; do
  compile_example "${source}"
done

phase "emit tokens examples/basic/lexer_smoke.noria"
set_case "examples/basic/lexer_smoke.noria"
run_noria --emit-tokens "${ROOT_DIR}/examples/basic/lexer_smoke.noria" \
  -o "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q 'let "let"' "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q 'fn "fn"' "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q 'identifier "x"' "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q '>= ">="' "${TEST_OUT_DIR}/lexer_smoke.tokens"
grep -q '!= "!="' "${TEST_OUT_DIR}/lexer_smoke.tokens"

phase "emit ast examples/basic/ast_smoke.noria"
set_case "examples/basic/ast_smoke.noria"
run_noria --emit-ast "${ROOT_DIR}/examples/basic/ast_smoke.noria" \
  -o "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Module" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Function factorial(n: i32) -> i32" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Let value: i32" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "If" "${TEST_OUT_DIR}/ast_smoke.ast"
grep -q "Call factorial" "${TEST_OUT_DIR}/ast_smoke.ast"

phase "type future params examples/basic/type_future_params_smoke.noria"
set_case "examples/basic/type_future_params_smoke.noria"
grep -q "define i32 @consume_f64(double" "${TEST_OUT_DIR}/type_future_params_smoke.ll"
grep -q "define i32 @consume_str(ptr" "${TEST_OUT_DIR}/type_future_params_smoke.ll"

phase "negative type-checking examples"
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
grep -q "typecheck: integer literal out of i32 range" \
  "${TEST_OUT_DIR}/integer_literal_overflow.stderr"
grep -q "typecheck: ordered comparison requires matching numeric operands, got bool and bool" \
  "${TEST_OUT_DIR}/ordered_compare_bool.stderr"
grep -q "typecheck: array element type cannot be a struct" \
  "${TEST_OUT_DIR}/array_struct_element.stderr"
grep -q "typecheck: unknown function 'missing'" \
  "${TEST_OUT_DIR}/unreachable_after_exhaustive_if.stderr"
grep -q "typecheck: not all control-flow paths in function 'status' return" \
  "${TEST_OUT_DIR}/missing_return_non_void.stderr"
grep -q "typecheck: not all control-flow paths in function 'announce' return" \
  "${TEST_OUT_DIR}/missing_return_void.stderr"
grep -q "typecheck: non-void function must return a value" \
  "${TEST_OUT_DIR}/bare_return_non_void.stderr"
grep -q "typecheck: void function cannot return a value" \
  "${TEST_OUT_DIR}/value_return_void.stderr"
grep -q "typecheck: not all control-flow paths in function 'spin' return" \
  "${TEST_OUT_DIR}/missing_return_while_true.stderr"
grep -q "typecheck: not all control-flow paths in function 'pending' return" \
  "${TEST_OUT_DIR}/missing_return_generic.stderr"
grep -q "typecheck: not all control-flow paths in function 'zero' return" \
  "${TEST_OUT_DIR}/struct_default_return.stderr"
grep -q "typecheck: return type bool does not match expected i32" \
  "${TEST_OUT_DIR}/inferred_return_type_conflict.stderr"
grep -q "typecheck: void function cannot return a value" \
  "${TEST_OUT_DIR}/inferred_return_mixed_forms.stderr"
grep -q "typecheck: cannot infer element type of empty array literal" \
  "${TEST_OUT_DIR}/inferred_return_empty_array.stderr"
grep -q "typecheck: cannot infer return type for function 'loop'; add an explicit '-> Type'" \
  "${TEST_OUT_DIR}/inferred_return_unanchored_recursion.stderr"
for name in void_parameter void_local void_struct_field void_array void_generic_argument void_cast; do
  grep -q "typecheck: void is only valid as a function return type" \
    "${TEST_OUT_DIR}/${name}.stderr"
done

phase "entry-point diagnostics"
expect_entry_point_failure "${ROOT_DIR}/qa_programs/invalid_main_signature.noria" \
  "entry point 'main' must accept no parameters; expected 'fn main() -> i32'"
expect_entry_point_failure "${ROOT_DIR}/qa_programs/invalid_main_return_type.noria" \
  "entry point 'main' must return i32, got bool"
expect_entry_point_failure "${ROOT_DIR}/qa_programs/void_main.noria" \
  "entry point 'main' must return i32, got void"
expect_entry_point_failure "${ROOT_DIR}/qa_programs/str_main.noria" \
  "entry point 'main' must return i32, got str"
expect_entry_point_failure "${ROOT_DIR}/qa_programs/generic_main.noria" \
  "entry point 'main' must not be generic; expected 'fn main() -> i32'"
expect_entry_point_failure "${ROOT_DIR}/qa_programs/missing_main.noria" \
  "missing entry point; expected 'fn main() -> i32'"

phase "future type name() diagnostics"
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

grep -q "typecheck: cannot infer local variable 'x' from void initializer" \
  "${TEST_OUT_DIR}/void_inferred_local.stderr"
grep -q "typecheck: unknown local variable 'a'" \
  "${TEST_OUT_DIR}/bare_inferred_declaration.stderr"
grep -q "typecheck: cannot initialize 'x' of type i32 with bool" \
  "${TEST_OUT_DIR}/declaration_shorthand_mismatch.stderr"
grep -q "typecheck: cannot initialize 'x' of type i32 with bool" \
  "${TEST_OUT_DIR}/declaration_type_first_mismatch.stderr"

grep -q "typecheck: unknown type 'widget'" \
  "${TEST_OUT_DIR}/unknown_future_type.stderr"

phase "phase 1 operator diagnostics"
grep -q "typecheck: logical operator requires bool operands, got i32 and bool" \
  "${TEST_OUT_DIR}/logical_non_bool.stderr"
grep -q "typecheck: integer operator requires i32 operands, got bool and bool" \
  "${TEST_OUT_DIR}/bitwise_non_integer.stderr"
grep -q "typecheck: cannot cast bool to f64" \
  "${TEST_OUT_DIR}/cast_bad_type.stderr"
grep -q "typecheck: integer division by zero" \
  "${TEST_OUT_DIR}/integer_divide_zero_literal.stderr"
grep -q "typecheck: integer remainder by zero" \
  "${TEST_OUT_DIR}/integer_remainder_zero_literal.stderr"
grep -q "typecheck: integer division overflow" \
  "${TEST_OUT_DIR}/integer_divide_overflow_literal.stderr"
grep -q "typecheck: integer remainder overflow" \
  "${TEST_OUT_DIR}/integer_remainder_overflow_literal.stderr"
grep -q "typecheck: integer shift count out of range (expected 0..31)" \
  "${TEST_OUT_DIR}/integer_shift_left_negative_literal.stderr"
grep -q "typecheck: integer shift count out of range (expected 0..31)" \
  "${TEST_OUT_DIR}/integer_shift_left_too_large_literal.stderr"
grep -q "typecheck: integer shift count out of range (expected 0..31)" \
  "${TEST_OUT_DIR}/integer_shift_right_negative_literal.stderr"
grep -q "typecheck: integer shift count out of range (expected 0..31)" \
  "${TEST_OUT_DIR}/integer_shift_right_too_large_literal.stderr"

phase "phase 2 io and cast diagnostics"
grep -q "typecheck: print_int expects i32, got str" \
  "${TEST_OUT_DIR}/print_int_wrong_type.stderr"
grep -q "typecheck: expression statement must be a function call" \
  "${TEST_OUT_DIR}/bare_expression_statement.stderr"

phase "builtin name reservation diagnostics"
grep -q "1:1: typecheck: cannot define function 'print': name is a builtin" \
  "${TEST_OUT_DIR}/user_print.stderr"
grep -q "1:1: typecheck: cannot define function 'println': name is a builtin" \
  "${TEST_OUT_DIR}/user_println.stderr"
grep -q "1:1: typecheck: cannot define function 'len': name is a builtin" \
  "${TEST_OUT_DIR}/user_generic_len.stderr"

phase "phase 3 string length diagnostics"
grep -q "typecheck: len expects str or array, got i32" \
  "${TEST_OUT_DIR}/len_wrong_type.stderr"

phase "phase 6 generic diagnostics"
grep -q "typecheck: function 'id' expects 1 argument(s), got 2" \
  "${TEST_OUT_DIR}/generic_wrong_arity.stderr"
grep -q "typecheck: cannot infer type parameter 't'" \
  "${TEST_OUT_DIR}/generic_uninferable.stderr"
grep -q "typecheck: unknown type 't'" \
  "${TEST_OUT_DIR}/generic_unresolved_type_param.stderr"
grep -q "typecheck: conflicting types i32 and bool for type parameter 't'" \
  "${TEST_OUT_DIR}/generic_conflicting_inference.stderr"
grep -q "typecheck: arithmetic operator requires matching numeric operands, got bool and i32" \
  "${TEST_OUT_DIR}/generic_instantiation_body_error.stderr"
grep -q "typecheck: specialization expansion limit exceeded" \
  "${TEST_OUT_DIR}/generic_recursive_specialization.stderr"
grep -q "typecheck: type 'box<i32, bool>' expects 1 type argument(s), got 2" \
  "${TEST_OUT_DIR}/generic_struct_wrong_arity.stderr"
grep -q "typecheck: unknown type 'missing<i32>'" \
  "${TEST_OUT_DIR}/generic_struct_unknown.stderr"
grep -q "typecheck: cannot infer type parameter 'b'" \
  "${TEST_OUT_DIR}/generic_struct_uninferred.stderr"
grep -q "typecheck: type 'point<i32>' is not generic and cannot take type arguments" \
  "${TEST_OUT_DIR}/generic_struct_non_generic_args.stderr"
grep -q "typecheck: field 'value' of 'box' expects i32, got bool" \
  "${TEST_OUT_DIR}/generic_struct_field_mismatch.stderr"
grep -q "typecheck: implementation tag 'arr' cannot be used as a type" \
  "${TEST_OUT_DIR}/impl_tag_as_type.stderr"
grep -q "typecheck: type 'box<i32>' expects 2 type argument(s), got 1" \
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

phase "phase 3 string index diagnostics"
grep -q "typecheck: index requires str, array, Sequence, Dictionary, or Set base, got i32" \
  "${TEST_OUT_DIR}/index_non_str_base.stderr"
grep -q "typecheck: index requires i32 index, got bool" \
  "${TEST_OUT_DIR}/index_non_i32.stderr"

phase "invalid syntax examples"
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
    let_missing_type_initializer.noria)
      expect_compile_failure_contains "${source}" \
        "local declaration 'x' requires a type or initializer"
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
        "duplicate type parameter 't'"
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
        "duplicate type parameter 't'"
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
    integer_literal_underflow.noria)
      expect_compile_failure_contains "${source}" \
        "integer literal out of i32 range"
      ;;
    float_literal_overflow.noria)
      expect_compile_failure_contains "${source}" \
        "float literal out of range"
      ;;
  esac
done

run_native_exit_test "${ROOT_DIR}/examples/basic/return_zero.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/return_one.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/return_answer.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/local_variable.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/function_parameter.noria" 5
run_native_exit_test "${ROOT_DIR}/examples/basic/return_large_integer.noria" 0
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
run_native_exit_test "${ROOT_DIR}/examples/basic/function_keywords.noria" 18
run_native_exit_test "${ROOT_DIR}/examples/basic/factorial.noria" 120
run_native_exit_test "${ROOT_DIR}/examples/basic/inferred_return_types.noria" 8
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
run_native_exit_test "${ROOT_DIR}/examples/basic/declaration_shorthand_locals.noria" 30
run_native_exit_test "${ROOT_DIR}/examples/basic/declaration_inferred_locals.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/declaration_mixed_params_fields.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/iterative_factorial.noria" 120
run_native_exit_test "${ROOT_DIR}/examples/basic/iterative_fibonacci.noria" 13
run_native_exit_test "${ROOT_DIR}/examples/basic/while_count.noria" 5
run_native_exit_test "${ROOT_DIR}/examples/basic/while_sum.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/while_with_if.noria" 14
run_native_exit_test "${ROOT_DIR}/examples/basic/unary_operators.noria" 12
run_native_exit_test "${ROOT_DIR}/examples/basic/logical_operators.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/modulo.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/bitwise.noria" 37
run_native_exit_test "${ROOT_DIR}/examples/basic/i32_min_max.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/i32_wrapping.noria" 0

phase "void procedure acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/void_procedure.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_void_procedure.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/conditional_return_followup.noria" 0
grep -q "define void @announce" "${TEST_OUT_DIR}/void_procedure.ll"
grep -q "call void @announce" "${TEST_OUT_DIR}/void_procedure.ll"
grep -q "ret void" "${TEST_OUT_DIR}/void_procedure.ll"
grep -Fq 'define void @consume$s.i32' "${TEST_OUT_DIR}/generic_void_procedure.ll"
phase "integer safety acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/integer_checked_boundaries.noria" \
  "${ROOT_DIR}/examples/basic/integer_checked_boundaries.expected"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_divide_zero_runtime.noria" 70 \
  "integer division by zero"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_remainder_zero_runtime.noria" 70 \
  "integer remainder by zero"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_divide_overflow_runtime.noria" 70 \
  "integer division overflow"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_remainder_overflow_runtime.noria" 70 \
  "integer remainder overflow"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_shift_left_negative_runtime.noria" 70 \
  "integer shift count out of range"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_shift_left_too_large_runtime.noria" 70 \
  "integer shift count out of range"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_shift_right_negative_runtime.noria" 70 \
  "integer shift count out of range"
run_native_failure_test "${ROOT_DIR}/examples/basic/integer_shift_right_too_large_runtime.noria" 70 \
  "integer shift count out of range"
run_native_exit_test "${ROOT_DIR}/examples/basic/bool_equality.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/short_circuit_and.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/short_circuit_or.noria" 7
run_native_stdout_test "${ROOT_DIR}/examples/basic/short_circuit_no_print.noria" \
  "${ROOT_DIR}/examples/basic/short_circuit_no_print.expected"
run_native_exit_test "${ROOT_DIR}/examples/basic/else_if.noria" 6
run_native_exit_test "${ROOT_DIR}/examples/basic/if_without_else.noria" 5
run_native_exit_test "${ROOT_DIR}/examples/basic/cast_identity.noria" 8
run_native_exit_test "${ROOT_DIR}/examples/basic/float_math.noria" 4
run_native_exit_test "${ROOT_DIR}/examples/basic/float_nan_not_equal.noria" 0
grep -q "fcmp une double" "${TEST_OUT_DIR}/float_nan_not_equal.ll"
grep -q "fcmp oeq double" "${TEST_OUT_DIR}/float_nan_not_equal.ll"
run_native_exit_test "${ROOT_DIR}/examples/basic/cast_roundtrip.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/cast_precision_loss.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/math_builtins.noria" 1

phase "checked f64 to i32 casts"
run_native_stdout_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_valid.noria" \
  "${ROOT_DIR}/examples/basic/cast_f64_to_i32_valid.expected"
run_native_failure_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_too_large.noria" 70 \
  "invalid f64 to i32 cast"
run_native_failure_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_too_small.noria" 70 \
  "invalid f64 to i32 cast"
run_native_failure_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_nan.noria" 70 \
  "invalid f64 to i32 cast"
run_native_failure_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_positive_infinity.noria" 70 \
  "invalid f64 to i32 cast"
run_native_failure_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_negative_infinity.noria" 70 \
  "invalid f64 to i32 cast"
grep -q "fcmp ogt double" "${TEST_OUT_DIR}/cast_f64_to_i32_too_large.ll"
grep -q "fcmp olt double" "${TEST_OUT_DIR}/cast_f64_to_i32_too_large.ll"
grep -q "cast.fail" "${TEST_OUT_DIR}/cast_f64_to_i32_too_large.ll"

phase "phase 2 stdout acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/print_adjacent.noria" \
  "${ROOT_DIR}/examples/basic/print_adjacent.expected"
grep -Fq "@.fmt.str = private unnamed_addr constant [3 x i8] c\"%s\\00\"" \
  "${TEST_OUT_DIR}/print_adjacent.ll"
if grep -Fq "@puts" "${TEST_OUT_DIR}/print_adjacent.ll"; then
  fail "print IR must not call puts"
fi
run_native_stdout_test "${ROOT_DIR}/examples/basic/print_numeric_adjacent.noria" \
  "${ROOT_DIR}/examples/basic/print_numeric_adjacent.expected"
grep -Fq "@.fmt.float = private unnamed_addr constant [3 x i8] c\"%g\\00\"" \
  "${TEST_OUT_DIR}/print_numeric_adjacent.ll"
if grep -Fq "c\"%g\\0A\\00\"" "${TEST_OUT_DIR}/print_numeric_adjacent.ll"; then
  fail "print_float IR must not include a newline in the format string"
fi
if ! grep -A1 "call i32 @putchar(i32 48)" "${TEST_OUT_DIR}/print_numeric_adjacent.ll" |
  grep -q "ret void"; then
  fail "print_int zero path must not append a newline"
fi
run_native_stdout_test "${ROOT_DIR}/examples/basic/hello_world.noria" \
  "${ROOT_DIR}/examples/basic/hello_world.expected"
run_native_stdout_test "${ROOT_DIR}/examples/basic/fizzbuzz.noria" \
  "${ROOT_DIR}/examples/basic/fizzbuzz.expected"
run_native_stdout_test "${ROOT_DIR}/examples/basic/float_output.noria" \
  "${ROOT_DIR}/examples/basic/float_output.expected"
grep -Fq "call i32 (ptr, ...) @printf(" "${TEST_OUT_DIR}/float_output.ll"
grep -Fq "@.fmt.float = private unnamed_addr constant [3 x i8] c\"%g\\00\"" \
  "${TEST_OUT_DIR}/float_output.ll"

phase "phase 3 string length acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_length.noria" \
  "${ROOT_DIR}/examples/basic/string_length.expected"
grep -q "call i64 @strlen" "${TEST_OUT_DIR}/string_length.ll"

phase "phase 3 string index acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_index.noria" \
  "${ROOT_DIR}/examples/basic/string_index.expected"
grep -q "getelementptr inbounds i8" "${TEST_OUT_DIR}/string_index.ll"
grep -q "zext i8" "${TEST_OUT_DIR}/string_index.ll"

phase "phase 3 string concat acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_concat.noria" \
  "${ROOT_DIR}/examples/basic/string_concat.expected"
grep -q "call ptr @malloc" "${TEST_OUT_DIR}/string_concat.ll"
grep -q "call ptr @strcpy" "${TEST_OUT_DIR}/string_concat.ll"
grep -q "call ptr @strcat" "${TEST_OUT_DIR}/string_concat.ll"

phase "phase 3 string escape acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_escapes.noria" \
  "${ROOT_DIR}/examples/basic/string_escapes.expected"

phase "phase 3 string output acceptance programs"
run_native_stdout_test "${ROOT_DIR}/examples/basic/string_output.noria" \
  "${ROOT_DIR}/examples/basic/string_output.expected"
run_native_exit_test "${ROOT_DIR}/examples/basic/str_equality.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/empty_string.noria" 0
run_native_stdout_test "${ROOT_DIR}/examples/basic/default_initialized_string_print.noria" \
  "${ROOT_DIR}/examples/basic/default_initialized_string_print.expected"
run_native_failure_test "${ROOT_DIR}/examples/basic/string_index_oob.noria" 70 \
  "string index out of bounds"

phase "phase 4 array acceptance programs"
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
run_native_exit_test "${ROOT_DIR}/examples/basic/array_bool_roundtrip.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/array_empty_contexts.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/array_nested_empty.noria" 10
run_native_exit_test "${ROOT_DIR}/examples/basic/default_initialized_payload.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/array_index_oob.noria" 70 \
  "array index out of bounds"
run_native_failure_test "${ROOT_DIR}/examples/basic/array_index_negative.noria" 70 \
  "array index out of bounds"
run_native_failure_test "${ROOT_DIR}/examples/basic/default_initialized_array_oob.noria" 70 \
  "array index out of bounds"
grep -q "call ptr @malloc" "${TEST_OUT_DIR}/arrays_sum.ll"
grep -q "store i64 4" "${TEST_OUT_DIR}/arrays_sum.ll"
grep -q "getelementptr inbounds i8, ptr .*, i64 8" "${TEST_OUT_DIR}/arrays_sum.ll"
grep -q "call ptr @malloc(i64 8)" "${TEST_OUT_DIR}/array_empty_contexts.ll"
grep -q "store i64 0" "${TEST_OUT_DIR}/array_empty_contexts.ll"
grep -q "call ptr @malloc(i64 8)" "${TEST_OUT_DIR}/default_initialized_payload.ll"
grep -q "store i64 0" "${TEST_OUT_DIR}/default_initialized_payload.ll"
if grep -Fq "store ptr null" "${TEST_OUT_DIR}/default_initialized_payload.ll"; then
  fail "default-initialized str and array values must not store null"
fi
if grep -Fq "store ptr null" "${TEST_OUT_DIR}/default_initialized_string_print.ll"; then
  fail "default-initialized str must not store null"
fi

phase "managed str and array auto-free acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/string_concat_loop.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/array_if_scope.noria" 20
run_native_exit_test "${ROOT_DIR}/examples/basic/array_copy_independence.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/array_param_mutation.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/return_owned_array.noria" 6
run_native_exit_test "${ROOT_DIR}/examples/basic/return_owned_str.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/str_array_reassign_scope.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_array_field.noria" 3
grep -q "call void @__noria.rt.drop_str" "${TEST_OUT_DIR}/string_concat_loop.ll"
grep -q "call void @free" "${TEST_OUT_DIR}/string_concat_loop.ll"
grep -q "call void @free" "${TEST_OUT_DIR}/array_if_scope.ll"
grep -q "call void @free" "${TEST_OUT_DIR}/array_copy_independence.ll"
if command -v valgrind >/dev/null 2>&1 && [[ -n "${CLANG}" ]]; then
  set_case "valgrind examples/basic/string_concat_loop.noria"
  echo "[noria-tests] valgrind examples/basic/string_concat_loop.noria"
  link_ir "${TEST_OUT_DIR}/string_concat_loop.ll" "${TEST_OUT_DIR}/string_concat_loop_valgrind"
  valgrind --leak-check=full --error-exitcode=1 \
    "${TEST_OUT_DIR}/string_concat_loop_valgrind" >/dev/null
  set_case "valgrind examples/basic/sequence_push_loop.noria"
  echo "[noria-tests] valgrind examples/basic/sequence_push_loop.noria"
  link_ir "${TEST_OUT_DIR}/sequence_push_loop.ll" "${TEST_OUT_DIR}/sequence_push_loop_valgrind"
  valgrind --leak-check=full --error-exitcode=1 \
    "${TEST_OUT_DIR}/sequence_push_loop_valgrind" >/dev/null
fi

phase "phase 3 string concat diagnostics"
grep -q "typecheck: string concatenation requires str operands, got str and i32" \
  "${TEST_OUT_DIR}/concat_str_i32.stderr"
grep -q "typecheck: string concatenation requires str operands, got i32 and str" \
  "${TEST_OUT_DIR}/concat_i32_str.stderr"

phase "phase 4 array diagnostics"
grep -q "typecheck: array literal element 2 has type bool, expected i32" \
  "${TEST_OUT_DIR}/array_literal_mixed_types.stderr"
grep -q "typecheck: cannot infer element type of empty array literal" \
  "${TEST_OUT_DIR}/array_literal_empty.stderr"
grep -q "typecheck: cannot infer element type of empty array literal" \
  "${TEST_OUT_DIR}/array_literal_nested_empty.stderr"
grep -Fq "typecheck: cannot initialize 'a' of type [i32] with [f64]" \
  "${TEST_OUT_DIR}/array_element_type_mismatch.stderr"
grep -q "typecheck: index requires str, array, Sequence, Dictionary, or Set base, got i32" \
  "${TEST_OUT_DIR}/array_index_non_array_base.stderr"
grep -q "typecheck: cannot assign f64 to variable 'a' of type i32" \
  "${TEST_OUT_DIR}/array_indexed_store_type_mismatch.stderr"
grep -q "typecheck: str index is not assignable" \
  "${TEST_OUT_DIR}/string_index_assignment.stderr"
grep -q "typecheck: index requires i32 index, got bool" \
  "${TEST_OUT_DIR}/array_indexed_non_i32_index.stderr"
grep -q "mul i32 .*, 4" "${TEST_OUT_DIR}/array_indexed_assignment.ll"
grep -q "getelementptr i8, ptr %t[0-9]*, i32" "${TEST_OUT_DIR}/array_indexed_assignment.ll"
grep -q "store i32 [^,]*, ptr %t[0-9]*" "${TEST_OUT_DIR}/array_indexed_assignment.ll"
grep -q "store i32 99, ptr %t[0-9]*" "${TEST_OUT_DIR}/array_indexed_assignment.ll"
grep -q "typecheck: len expects str or array, got i32" \
  "${TEST_OUT_DIR}/array_len_of_element.stderr"

phase "phase 5 struct acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_point.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_param_by_value.noria" 106
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_param_aggregate_fields.noria" 23
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_literal_argument_in_condition.noria" 1
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_copy.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_field_assign.noria" 34
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_field_assign_nested.noria" 5
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_array_field_assign.noria" 68
run_native_stdout_test "${ROOT_DIR}/examples/basic/struct_field_assign_str.noria" \
  "${ROOT_DIR}/examples/basic/struct_field_assign_str.expected"
run_native_stdout_test "${ROOT_DIR}/examples/basic/struct_field_order.noria" \
  "${ROOT_DIR}/examples/basic/struct_field_order.expected"
grep -q "%point = type { i32, i32 }" "${TEST_OUT_DIR}/struct_point.ll"
grep -q "getelementptr inbounds %point, ptr %t[0-9]*, i32 0, i32 1" \
  "${TEST_OUT_DIR}/struct_field_order.ll"
grep -q "getelementptr inbounds %point, ptr %t[0-9]*, i32 0, i32 1" \
  "${TEST_OUT_DIR}/struct_field_assign.ll"
grep -q "store i32 [^,]*, ptr %t[0-9]*" "${TEST_OUT_DIR}/struct_field_assign.ll"
grep -q "alloca %point" "${TEST_OUT_DIR}/struct_point.ll"
grep -q "define %point @" "${TEST_OUT_DIR}/struct_param_by_value.ll"
grep -q "call %point @" "${TEST_OUT_DIR}/struct_param_by_value.ll"
grep -q "store %point %.*\.param" "${TEST_OUT_DIR}/struct_param_by_value.ll"

phase "emit ast examples/basic/struct_point.noria"
set_case "examples/basic/struct_point.noria"
run_noria --emit-ast "${ROOT_DIR}/examples/basic/struct_point.noria" \
  -o "${TEST_OUT_DIR}/struct_point.ast"
grep -q "Struct point" "${TEST_OUT_DIR}/struct_point.ast"

phase "phase 5 struct diagnostics"
grep -q "typecheck: struct 'point' has no field 'z'" \
  "${TEST_OUT_DIR}/struct_unknown_field.stderr"
grep -q "typecheck: struct literal for 'point' is missing field 'y'" \
  "${TEST_OUT_DIR}/struct_missing_field.stderr"
grep -q "typecheck: duplicate field 'x' in struct literal for 'point'" \
  "${TEST_OUT_DIR}/struct_duplicate_field.stderr"
grep -q "typecheck: field 'x' of 'point' expects i32, got bool" \
  "${TEST_OUT_DIR}/struct_field_type_mismatch.stderr"
grep -q "typecheck: field access requires struct base, got i32" \
  "${TEST_OUT_DIR}/struct_field_on_non_struct.stderr"
grep -q "typecheck: struct 'point' has no field 'z'" \
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
grep -q "store i32 50, ptr %t[0-9]*" "${TEST_OUT_DIR}/struct_array_field_assign.ll"
grep -q "store i32 9, ptr %t[0-9]*" "${TEST_OUT_DIR}/struct_array_field_assign.ll"
grep -q "typecheck: invalid assignment target" \
  "${TEST_OUT_DIR}/struct_array_field_assign_temporary.stderr"
grep -q "typecheck: cannot assign bool to variable 'h' of type i32" \
  "${TEST_OUT_DIR}/struct_array_field_assign_type_mismatch.stderr"
grep -q "typecheck: unknown type 'nope'" \
  "${TEST_OUT_DIR}/struct_unknown_type.stderr"
grep -q "typecheck: duplicate struct 'point'" \
  "${TEST_OUT_DIR}/struct_duplicate_decl.stderr"
grep -q "typecheck: argument 1 of 'm' expects point, got other" \
  "${TEST_OUT_DIR}/struct_argument_type_mismatch.stderr"
grep -q "typecheck: argument 1 of 'm' expects point, got i32" \
  "${TEST_OUT_DIR}/struct_argument_non_struct.stderr"
grep -q "typecheck: return type i32 does not match expected point" \
  "${TEST_OUT_DIR}/struct_return_type_mismatch.stderr"

phase "phase 6 import acceptance programs"
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

phase "phase 6 private runtime ABI"
run_native_exit_test "${ROOT_DIR}/examples/basic/stdlib_memory_probe.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/stdlib_generic_alloc.noria" 1

phase "phase 7 sequence acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/adt_default_impls.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/adt_default_init.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/array_add.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/array_add_length_mismatch.noria" 70 \
  "array addition requires equal lengths"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_push_get.noria" 60
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_if_scope.noria" 20
run_native_exit_test "${ROOT_DIR}/examples/basic/container_mixed_scope_drop.noria" 8
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_copy_independence.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/set_copy_independence.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_param_mutate.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_str_reassign_scope.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/struct_sequence_field.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_push_loop.noria" 0
grep -q "call void @__noria_sequence_drop" "${TEST_OUT_DIR}/sequence_if_scope.ll"
grep -q "call void @__noria_sequence_drop" "${TEST_OUT_DIR}/container_mixed_scope_drop.ll"
grep -q "call void @__noria_dictionary_drop" "${TEST_OUT_DIR}/container_mixed_scope_drop.ll"
grep -q "call void @__noria.rt.drop_str" "${TEST_OUT_DIR}/container_mixed_scope_drop.ll"
grep -q "call void @free" "${TEST_OUT_DIR}/container_mixed_scope_drop.ll"
grep -q "call void @__noria_sequence_drop" "${TEST_OUT_DIR}/sequence_push_loop.ll"
run_native_exit_test "${ROOT_DIR}/examples/basic/declaration_container_type_first.noria" 42
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
grep -c '%sequence$s.i32$tag.arr = type' "${TEST_OUT_DIR}/sequence_push_get.ll" | grep -q "^1$"
grep -c 'define %sequence$s.i32$tag.arr @sequence_new$s.i32$tag.arr' "${TEST_OUT_DIR}/sequence_push_get.ll" | grep -q "^1$"
grep -c '%sequence$s.i32$tag.arr = type' "${TEST_OUT_DIR}/adt_default_impls.ll" | grep -q "^1$"
grep -c '%set$s.i32$tag.hashmap = type' "${TEST_OUT_DIR}/adt_default_impls.ll" | grep -q "^1$"
grep -c '%dictionary$s.i32$s.i32$tag.hashmap = type' "${TEST_OUT_DIR}/adt_default_impls.ll" | grep -q "^1$"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_push_get.noria" 40
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_pop_set.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_insert_remove.noria" 55
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_insert_oob.noria" 70 \
  "sequence_insert: index out of bounds"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_remove_oob.noria" 70 \
  "sequence_remove: index out of bounds"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_arr_list_conformance.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_nested_id.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_str_arr.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_str_list.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_bool_arr.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_bool_list.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_set_oob.noria" 70 \
  "sequence_set: index out of bounds"
grep -c '%sequence$s.i32$tag.list = type' "${TEST_OUT_DIR}/sequence_list_push_get.ll" | grep -q "^1$"
grep -c 'define %sequence$s.i32$tag.list @sequence_new$s.i32$tag.list' "${TEST_OUT_DIR}/sequence_list_push_get.ll" | grep -q "^1$"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_pop_empty.noria" 70 \
  "sequence_pop: empty sequence"
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_list_get_oob.noria" 70 \
  "sequence_get: index out of bounds"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_add.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_add.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_add_length_mismatch.noria" 70 \
  "sequence addition requires equal lengths"
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_index.noria" 40
run_native_exit_test "${ROOT_DIR}/examples/basic/sequence_list_index.noria" 51
run_native_failure_test "${ROOT_DIR}/examples/basic/sequence_index_oob.noria" 70 \
  "sequence_get: index out of bounds"

run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_insert_get.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_contains_remove.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_get_or.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_insert_get.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_contains_remove.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_get_or.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_resize.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_tombstone.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_dense.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_i32_str.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_str_i32.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_bool_i32.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashmap_str_tombstone.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_hashset_alias.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_i32_str.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_f64.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_bst_sorted_delete.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/dictionary_get_missing.noria" 70 \
  "dictionary_get: key not found"
run_native_failure_test "${ROOT_DIR}/examples/basic/dictionary_remove_missing.noria" 70 \
  "dictionary_remove: key not found"
grep -c '%dictionary$s.i32$s.i32$tag.bst = type' "${TEST_OUT_DIR}/dictionary_bst_insert_get.ll" | grep -q "^1$"
grep -c 'define %dictionary$s.i32$s.i32$tag.bst @dictionary_new$s.i32$s.i32$tag.bst' \
  "${TEST_OUT_DIR}/dictionary_bst_insert_get.ll" | grep -q "^1$"
grep -c '%dictionary$s.i32$s.i32$tag.hashmap = type' \
  "${TEST_OUT_DIR}/dictionary_hashmap_insert_get.ll" | grep -q "^1$"
grep -c 'define %dictionary$s.i32$s.i32$tag.hashmap @dictionary_new$s.i32$s.i32$tag.hashmap' \
  "${TEST_OUT_DIR}/dictionary_hashmap_insert_get.ll" | grep -q "^1$"
grep -c '%dictionary$s.i32$s.i32$tag.hashmap = type' \
  "${TEST_OUT_DIR}/dictionary_hashset_alias.ll" | grep -q "^1$"
grep -c 'define %dictionary$s.i32$s.i32$tag.hashmap @dictionary_new$s.i32$s.i32$tag.hashmap' \
  "${TEST_OUT_DIR}/dictionary_hashset_alias.ll" | grep -q "^1$"

run_native_exit_test "${ROOT_DIR}/examples/basic/set_bst_ops.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/set_hashmap_ops.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/set_hashmap_str.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/set_hashmap_bool.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/set_hashmap_alias.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/set_index.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/dictionary_index.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/set_remove_missing.noria" 70 \
  "set_remove: key not found"
grep -c '%set$s.i32$tag.bst = type' "${TEST_OUT_DIR}/set_bst_ops.ll" | grep -q "^1$"
grep -c 'define %set$s.i32$tag.bst @set_new$s.i32$tag.bst' \
  "${TEST_OUT_DIR}/set_bst_ops.ll" | grep -q "^1$"
grep -c '%set$s.i32$tag.hashmap = type' "${TEST_OUT_DIR}/set_hashmap_ops.ll" | grep -q "^1$"
grep -c 'define %set$s.i32$tag.hashmap @set_new$s.i32$tag.hashmap' \
  "${TEST_OUT_DIR}/set_hashmap_ops.ll" | grep -q "^1$"
grep -c '%set$s.i32$tag.hashmap = type' "${TEST_OUT_DIR}/set_hashmap_alias.ll" | grep -q "^1$"
grep -c 'define %set$s.i32$tag.hashmap @set_new$s.i32$tag.hashmap' \
  "${TEST_OUT_DIR}/set_hashmap_alias.ll" | grep -q "^1$"

run_native_exit_test "${ROOT_DIR}/examples/basic/heap_arr_ops.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/heap_list_ops.noria" 0
run_native_failure_test "${ROOT_DIR}/examples/basic/heap_pop_empty.noria" 70 \
  "heappop: empty heap"
grep -c 'define i32 @heappop$s.i32$tag.arr' "${TEST_OUT_DIR}/heap_arr_ops.ll" | grep -q "^1$"
grep -c 'define i32 @heappop$s.i32$tag.list' "${TEST_OUT_DIR}/heap_list_ops.ll" | grep -q "^1$"

phase "phase 7 sequence diagnostics"
grep -q "typecheck: no implementation of 'sequence_new' for tag 'bst'" \
  "${TEST_OUT_DIR}/sequence_bst_unsupported.stderr"
grep -q "typecheck: implementation tag 'hashmap' requires 'hash' for key type f64" \
  "${TEST_OUT_DIR}/dictionary_hashmap_key_unhashable.stderr"
grep -q "typecheck: implementation tag 'bst' requires '<' for key type str" \
  "${TEST_OUT_DIR}/set_bst_key_unordered.stderr"
grep -q "typecheck: implementation tag 'hashmap' requires 'hash' for key type f64; V2 hashes i32, bool, str" \
  "${TEST_OUT_DIR}/set_hashmap_key_unhashable.stderr"
grep -q "typecheck: ordered comparison requires matching numeric operands, got str and str" \
  "${TEST_OUT_DIR}/heap_key_unordered.stderr"
grep -q "typecheck: internal runtime builtin '__rt_load' is unavailable outside the standard library" \
  "${TEST_OUT_DIR}/use_private_rt_load.stderr"
grep -q "typecheck: internal runtime builtin '__rt_trap' is unavailable outside the standard library" \
  "${TEST_OUT_DIR}/use_private_rt_trap.stderr"
grep -q "typecheck: conflicting types i32 and bool for type parameter 't'" \
  "${TEST_OUT_DIR}/sequence_set_type_mismatch.stderr"
grep -q "typecheck: __rt_drop requires a scalar element type, got point" \
  "${TEST_OUT_DIR}/sequence_struct_element.stderr"
grep -Fq "typecheck: collection addition requires matching array types, got [i32] and [bool]" \
  "${TEST_OUT_DIR}/array_add_type_mismatch.stderr"
grep -q "typecheck: collection addition requires matching Sequence operands, got sequence<i32, arr> and sequence<bool, arr>" \
  "${TEST_OUT_DIR}/sequence_add_type_mismatch.stderr"
grep -q "typecheck: set index is not assignable" \
  "${TEST_OUT_DIR}/set_index_assign.stderr"
grep -q "typecheck: type 'sequence<i32>' expects 2 type argument(s), got 1" \
  "${TEST_OUT_DIR}/user_sequence_default_not_applied.stderr"

phase "phase 7.0 private struct field diagnostics"
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

phase "phase 7 witness runtime bad stdlib diagnostics"
RT_LOAD_ARITY_STDERR="${TEST_OUT_DIR}/import_rt_load_wrong_arity.stderr"
set_case "tests/fixtures/bad_stdlib/import_rt_load_wrong_arity.noria"
if run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_rt_load_wrong_arity.noria" \
  -o "${TEST_OUT_DIR}/import_rt_load_wrong_arity.ll" \
  >"${TEST_OUT_DIR}/import_rt_load_wrong_arity.stdout" 2>"${RT_LOAD_ARITY_STDERR}"; then
  rt_load_arity_status=0
else
  rt_load_arity_status="$?"
fi
if [[ "${rt_load_arity_status}" == "0" ]]; then
  fail "expected compile failure for import_rt_load_wrong_arity.noria"
fi
grep -q "typecheck: __rt_load expects 2 arguments" "${RT_LOAD_ARITY_STDERR}"

RT_STORE_MISMATCH_STDERR="${TEST_OUT_DIR}/import_rt_store_mismatch.stderr"
set_case "tests/fixtures/bad_stdlib/import_rt_store_mismatch.noria"
if run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_rt_store_mismatch.noria" \
  -o "${TEST_OUT_DIR}/import_rt_store_mismatch.ll" \
  >"${TEST_OUT_DIR}/import_rt_store_mismatch.stdout" 2>"${RT_STORE_MISMATCH_STDERR}"; then
  rt_store_mismatch_status=0
else
  rt_store_mismatch_status="$?"
fi
if [[ "${rt_store_mismatch_status}" == "0" ]]; then
  fail "expected compile failure for import_rt_store_mismatch.noria"
fi
grep -q "typecheck: __rt_store expects i32, got bool" "${RT_STORE_MISMATCH_STDERR}"

phase "phase 6 import diagnostic file attribution"
BAD_TYPE_STDERR="${TEST_OUT_DIR}/import_bad_type.stderr"
set_case "tests/fixtures/bad_stdlib/import_bad_type.noria"
if run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_bad_type.noria" \
  -o "${TEST_OUT_DIR}/import_bad_type.ll" >"${TEST_OUT_DIR}/import_bad_type.stdout" 2>"${BAD_TYPE_STDERR}"; then
  bad_type_status=0
else
  bad_type_status="$?"
fi
if [[ "${bad_type_status}" == "0" ]]; then
  fail "expected compile failure for import_bad_type.noria"
fi
grep -q "std::badmath:2:10: typecheck: return type bool does not match expected i32" \
  "${BAD_TYPE_STDERR}"

phase "phase 6 duplicate export diagnostic"
DUPEXPORT_STDERR="${TEST_OUT_DIR}/import_dupexport.stderr"
set_case "tests/fixtures/bad_stdlib/import_dupexport.noria"
if run_noria --stdlib "${ROOT_DIR}/tests/fixtures/bad_stdlib" \
  "${ROOT_DIR}/tests/fixtures/bad_stdlib/import_dupexport.noria" \
  -o "${TEST_OUT_DIR}/import_dupexport.ll" >"${TEST_OUT_DIR}/import_dupexport.stdout" 2>"${DUPEXPORT_STDERR}"; then
  dupexport_status=0
else
  dupexport_status="$?"
fi
if [[ "${dupexport_status}" == "0" ]]; then
  fail "expected compile failure for import_dupexport.noria"
fi
grep -q "std::dupexport:5:1: import: duplicate function 'dup'" "${DUPEXPORT_STDERR}"

phase "phase 6 generic acceptance programs"
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

phase "phase 6 generic struct acceptance programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_box.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_two_params.noria" 7
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_nested.noria" 15
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_reuse.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_array_field.noria" 6
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_struct_infer.noria" 42
grep -c '%box$s.i32 = type' "${TEST_OUT_DIR}/generic_struct_reuse.ll" | grep -q "^1$"
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_impl_tag.noria" 42
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_impl_tag_distinct.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_impl_select_tag.noria" 3
run_native_exit_test "${ROOT_DIR}/examples/basic/generic_tag_constraint_ok.noria" 43

phase "production review leetcode and mixed-adt programs"
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_two_sum.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_valid_parentheses.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_max_subarray.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_climbing_stairs.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_merge_sorted_array.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_palindrome_number.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_roman_to_int.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_contains_duplicate.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_best_time_stock.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_binary_search.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_single_number.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/leetcode_kth_largest.noria" 0
run_native_exit_test "${ROOT_DIR}/examples/basic/complex_sequence_set_pair.noria" 0
grep -c '%box$s.i32$tag.arr = type' "${TEST_OUT_DIR}/generic_impl_tag_distinct.ll" | grep -q "^1$"
grep -c '%box$s.i32$tag.list = type' "${TEST_OUT_DIR}/generic_impl_tag_distinct.ll" | grep -q "^1$"
grep -c 'define i32 @kind$s.i32$tag.arr' "${TEST_OUT_DIR}/generic_impl_select_tag.ll" | grep -q "^1$"
grep -c 'define i32 @kind$s.i32$tag.list' "${TEST_OUT_DIR}/generic_impl_select_tag.ll" | grep -q "^1$"

if [[ -n "${CLANG}" ]]; then
  phase "direct build examples/basic/factorial.noria"
  set_case "examples/basic/factorial.noria"
  DIRECT_BUILD_STDOUT="${TEST_OUT_DIR}/factorial_direct.stdout"
  DIRECT_BUILD_STDERR="${TEST_OUT_DIR}/factorial_direct.stderr"
  run_noria build "${ROOT_DIR}/examples/basic/factorial.noria" -o "${TEST_OUT_DIR}/factorial_direct" \
    >"${DIRECT_BUILD_STDOUT}" 2>"${DIRECT_BUILD_STDERR}"
  [[ ! -s "${DIRECT_BUILD_STDOUT}" ]]
  [[ ! -s "${DIRECT_BUILD_STDERR}" ]]
  if "${TEST_OUT_DIR}/factorial_direct"; then
    actual_exit=0
  else
    actual_exit="$?"
  fi
  if [[ "${actual_exit}" != "120" ]]; then
    fail "expected exit 120, got ${actual_exit} for direct build"
  fi

  if [[ -n "${OPT}" ]]; then
    phase "optimized llvm examples/basic/variables.noria"
    set_case "examples/basic/variables.noria"
    run_noria -O2 "${ROOT_DIR}/examples/basic/variables.noria" -o "${TEST_OUT_DIR}/variables.opt.ll"
    grep -q "ret i32 7" "${TEST_OUT_DIR}/variables.opt.ll"

    phase "optimized direct build examples/basic/factorial.noria"
    set_case "examples/basic/factorial.noria"
    OPTIMIZED_DIRECT_BUILD_STDOUT="${TEST_OUT_DIR}/factorial_optimized.stdout"
    OPTIMIZED_DIRECT_BUILD_STDERR="${TEST_OUT_DIR}/factorial_optimized.stderr"
    run_noria build -O2 "${ROOT_DIR}/examples/basic/factorial.noria" \
      -o "${TEST_OUT_DIR}/factorial_optimized" \
      >"${OPTIMIZED_DIRECT_BUILD_STDOUT}" 2>"${OPTIMIZED_DIRECT_BUILD_STDERR}"
    [[ ! -s "${OPTIMIZED_DIRECT_BUILD_STDOUT}" ]]
    [[ ! -s "${OPTIMIZED_DIRECT_BUILD_STDERR}" ]]
    if "${TEST_OUT_DIR}/factorial_optimized"; then
      actual_exit=0
    else
      actual_exit="$?"
    fi
    if [[ "${actual_exit}" != "120" ]]; then
      fail "expected exit 120, got ${actual_exit} for optimized direct build"
    fi

    phase "optimized integer safety acceptance programs"
    run_optimized_native_stdout_test "${ROOT_DIR}/examples/basic/integer_checked_boundaries.noria" \
      "${ROOT_DIR}/examples/basic/integer_checked_boundaries.expected"
    run_optimized_native_failure_test "${ROOT_DIR}/examples/basic/integer_divide_zero_runtime.noria" \
      70 "integer division by zero"
    run_optimized_native_failure_test "${ROOT_DIR}/examples/basic/integer_remainder_zero_runtime.noria" \
      70 "integer remainder by zero"
    run_optimized_native_failure_test "${ROOT_DIR}/examples/basic/integer_divide_overflow_runtime.noria" \
      70 "integer division overflow"
    run_optimized_native_failure_test "${ROOT_DIR}/examples/basic/integer_remainder_overflow_runtime.noria" \
      70 "integer remainder overflow"
    run_optimized_native_failure_test \
      "${ROOT_DIR}/examples/basic/integer_shift_left_negative_runtime.noria" \
      70 "integer shift count out of range"
    run_optimized_native_failure_test \
      "${ROOT_DIR}/examples/basic/integer_shift_left_too_large_runtime.noria" \
      70 "integer shift count out of range"
    run_optimized_native_failure_test \
      "${ROOT_DIR}/examples/basic/integer_shift_right_negative_runtime.noria" \
      70 "integer shift count out of range"
    run_optimized_native_failure_test \
      "${ROOT_DIR}/examples/basic/integer_shift_right_too_large_runtime.noria" \
      70 "integer shift count out of range"

    phase "optimized f64 NaN inequality"
    run_optimized_native_exit_test "${ROOT_DIR}/examples/basic/float_nan_not_equal.noria" 0

    phase "optimized checked f64 to i32 casts"
    run_optimized_native_failure_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_too_large.noria" \
      70 "invalid f64 to i32 cast"
    run_optimized_native_failure_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_nan.noria" \
      70 "invalid f64 to i32 cast"
    run_optimized_native_stdout_test "${ROOT_DIR}/examples/basic/cast_f64_to_i32_valid.noria" \
      "${ROOT_DIR}/examples/basic/cast_f64_to_i32_valid.expected"
  else
    echo "[noria-tests] skip optimizer checks: opt not found; set LLVM_BIN or add opt to PATH" >&2
  fi
else
  echo "[noria-tests] skip direct native and optimizer checks: clang not found" >&2
fi

echo "[noria-tests] ok"
