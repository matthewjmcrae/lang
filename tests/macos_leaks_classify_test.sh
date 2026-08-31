#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLASSIFY="${ROOT_DIR}/tests/macos_leaks_classify.sh"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

expect_class() {
  local status="$1"
  local report_body="$2"
  local expected="$3"
  local report="${TMP_DIR}/report.txt"
  printf '%s\n' "${report_body}" >"${report}"
  local actual
  actual="$("${CLASSIFY}" "${status}" "${report}")"
  if [[ "${actual}" != "${expected}" ]]; then
    fail "status=${status} expected ${expected}, got ${actual} (body: ${report_body})"
  fi
}

# Clean zero-leak summary.
expect_class 0 $'Process 1: 0 leaks for 0 total leaked bytes.\n' pass
expect_class 1 $'Process 1: 0 leaks for 0 total leaked bytes.\n' pass

# Nonzero leak summary (program leak), regardless of exit code.
expect_class 0 $'Process 1: 2 leaks for 64 total leaked bytes.\n' program_leak
expect_class 1 $'Process 1: 1 leaks for 16 total leaked bytes.\n' program_leak

# Exit 255 without a summary: checker unavailable.
expect_class 255 $'leaks: could not attach to process\n' checker_unavailable
expect_class 255 $'' checker_unavailable

# Other failures without a summary: checker invocation error.
expect_class 1 $'leaks: unexpected failure\n' checker_error
expect_class 42 $'' checker_error

# Exit 0 without a summary is also a checker error.
expect_class 0 $'no summary here\n' checker_error

# Missing report file.
actual="$("${CLASSIFY}" 0 "${TMP_DIR}/missing.txt")"
if [[ "${actual}" != "checker_error" ]]; then
  fail "missing report expected checker_error, got ${actual}"
fi

echo "macos_leaks_classify_test: ok"
