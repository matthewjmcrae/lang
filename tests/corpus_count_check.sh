#!/usr/bin/env bash
# Fail when docs corpus counts drift from examples/{basic,invalid,invalid_syntax}.
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

count_noria() {
  local dir="$1"
  find "${dir}" -maxdepth 1 -type f -name '*.noria' | wc -l | tr -d ' '
}

accepted="$(count_noria "${ROOT_DIR}/examples/basic")"
semantic="$(count_noria "${ROOT_DIR}/examples/invalid")"
syntax="$(count_noria "${ROOT_DIR}/examples/invalid_syntax")"
negative=$((semantic + syntax))

echo "corpus: accepted=${accepted} semantic=${semantic} syntax=${syntax} negative=${negative}"

fail=0
check_doc() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if ! grep -Eq "${pattern}" "${file}"; then
    echo "FAIL: ${file} missing ${label} claim matching /${pattern}/" >&2
    fail=1
  fi
}

# docs/README.md: "277 accepted ... 148 semantic ... 22 lexer/parser"
check_doc "${ROOT_DIR}/docs/README.md" \
  "compiles ${accepted} accepted programs" \
  "accepted count"
check_doc "${ROOT_DIR}/docs/README.md" \
  "rejects ${semantic} semantic failures and ${syntax} lexer/parser failures" \
  "negative split"

# docs/ENGINEERING.md: "277 accepted ... 170 negative"
check_doc "${ROOT_DIR}/docs/ENGINEERING.md" \
  "includes ${accepted} accepted programs" \
  "accepted count"
check_doc "${ROOT_DIR}/docs/ENGINEERING.md" \
  "${negative} negative programs" \
  "negative total"

if [[ "${fail}" -ne 0 ]]; then
  exit 1
fi

echo "corpus_count_check: ok"
