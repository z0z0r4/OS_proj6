#!/usr/bin/env bash
# Dynamic linking test (ldso / shared objects / dlopen).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XV6_DIR="$ROOT_DIR/src"
LOG_DIR="$SCRIPT_DIR/logs"
TEST_ARCH="${TEST_ARCH:-riscv}"
TEST_CPUS="${TEST_CPUS:-3}"
LOG_SUFFIX=""
CASE_NAME="dynlink"
if [ "$TEST_ARCH" != "riscv" ]; then
  LOG_SUFFIX="-$TEST_ARCH"
  CASE_NAME="${TEST_ARCH}_dynlink"
fi
RAW_LOG="$LOG_DIR/qemu-dynlink$LOG_SUFFIX.log"
RESULT_LOG="$LOG_DIR/result$LOG_SUFFIX.log"

mkdir -p "$LOG_DIR"

if [ ! -f "$XV6_DIR/fs.img" ]; then
  echo "[dynlink] fs.img not found"
  echo "Use: make dynlink"
  exit 1
fi

echo "[dynlink] running dynamic linking test on $TEST_ARCH with CPUS=$TEST_CPUS"
ARCH="$TEST_ARCH" python3 "$ROOT_DIR/tests/runner/run_qemu_case.py" \
  --case "$CASE_NAME" \
  --xv6-dir "$XV6_DIR" \
  --commands "$SCRIPT_DIR/cmds.sh" \
  --log "$RAW_LOG" \
  --cpus "$TEST_CPUS" \
  --prompt-wait \
  --step-wait 0.01 \
  --final-wait 120 \
  --expect "DYN_BEGIN" \
  --expect "STATIC_STILL_WORKS" \
  --expect "STILL_ALIVE" \
  --expect "DYN_DONE" \
  --expect-count "DYNHELLO_OK" 11 \
  --expect-count "DYNTEST_OK" 2 \
  --expect "plugin_run(5) = 31" \
  --expect-count "ldso: cannot load shared object: /libgreet.so" 2 \
  --reject "panic:" \
  --reject "kerneltrap" \
  --reject "FAIL" \
  --reject "undefined symbol" \
  --reject "unsupported relocation" \
  --reject "not a dynamic object"
test_status=$?

if [ "$test_status" -eq 0 ]; then
  result="PASS"
else
  result="FAIL"
fi

{
  echo "Result: $result"
  echo "Test Status: $test_status"
  echo "CPUs: $TEST_CPUS"
  echo "Command File: $SCRIPT_DIR/cmds.sh"
  echo "Raw Log: $RAW_LOG"
} > "$RESULT_LOG"

echo "[dynlink] result: $result"
echo "[dynlink] logs: $LOG_DIR"

if [ "$result" != "PASS" ]; then
  exit 1
fi
