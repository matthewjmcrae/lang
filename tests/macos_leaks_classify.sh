#!/usr/bin/env bash
# Classify a macOS /usr/bin/leaks report.
# Usage: macos_leaks_classify.sh <exit_status> <report_path>
# Prints one of: pass | program_leak | checker_unavailable | checker_error
set -Eeuo pipefail

if [[ "$#" -ne 2 ]]; then
  echo "usage: $0 <exit_status> <report_path>" >&2
  exit 2
fi

status="$1"
report="$2"

if [[ ! -f "${report}" ]]; then
  echo "checker_error"
  exit 0
fi

if grep -q '0 leaks for 0 total leaked bytes' "${report}"; then
  echo "pass"
  exit 0
fi

if grep -q 'leaks for' "${report}"; then
  echo "program_leak"
  exit 0
fi

if [[ "${status}" -eq 255 ]]; then
  echo "checker_unavailable"
  exit 0
fi

if [[ "${status}" -ne 0 ]]; then
  echo "checker_error"
  exit 0
fi

# Exit 0 with no summary is unexpected; treat as checker error.
echo "checker_error"
