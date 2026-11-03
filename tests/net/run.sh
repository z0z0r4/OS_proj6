#!/usr/bin/env bash
# Network test: rewritten iperf / netperf over loopback and over the
# virtio-net device (to host peers reached through qemu user networking).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XV6_DIR="$ROOT_DIR/src"
LOG_DIR="$SCRIPT_DIR/logs"
TEST_ARCH="${TEST_ARCH:-riscv}"
TEST_CPUS="${TEST_CPUS:-3}"
LOG_SUFFIX=""
CASE_NAME="net"
if [ "$TEST_ARCH" != "riscv" ]; then
  LOG_SUFFIX="-$TEST_ARCH"
  CASE_NAME="${TEST_ARCH}_net"
fi
RAW_LOG="$LOG_DIR/qemu-net$LOG_SUFFIX.log"
RESULT_LOG="$LOG_DIR/result$LOG_SUFFIX.log"

mkdir -p "$LOG_DIR"

if [ ! -f "$XV6_DIR/fs.img" ]; then
  echo "[net] fs.img not found"
  echo "Use: make test_net"
  exit 1
fi

# host-side peers for the second half of cmds.sh
python3 "$SCRIPT_DIR/host_server.py" 5311 12875 > "$LOG_DIR/host-peers$LOG_SUFFIX.log" 2>&1 &
HOST_PID=$!
trap 'kill $HOST_PID 2>/dev/null' EXIT
sleep 1

echo "[net] running network test on $TEST_ARCH with CPUS=$TEST_CPUS"
ARCH="$TEST_ARCH" python3 "$ROOT_DIR/tests/runner/run_qemu_case.py" \
  --case "$CASE_NAME" \
  --xv6-dir "$XV6_DIR" \
  --commands "$SCRIPT_DIR/cmds.sh" \
  --log "$RAW_LOG" \
  --cpus "$TEST_CPUS" \
  --prompt-wait \
  --step-wait 0.01 \
  --final-wait 200 \
  --expect "net: virtio-net" \
  --expect "NET_LOOPBACK_DONE" \
  --expect "NET_DONE" \
  --expect-count "iperf Done." 3 \
  --expect-count "TCP STREAM TEST from" 2 \
  --expect-count "UDP UNIDIRECTIONAL SEND TEST from" 2 \
  --expect-count "TCP REQUEST/RESPONSE TEST from" 2 \
  --expect-count "UDP REQUEST/RESPONSE TEST from" 2 \
  --expect-count "TCP Connect/Request/Response TEST from" 2 \
  --expect "0/852 (0%)  receiver" \
  --expect "Server listening on 5201" \
  --reject "panic:" \
  --reject "kerneltrap" \
  --reject "unable to connect" \
  --reject "connect to" \
  --reject "no result" \
  --reject "unknown test" \
  --reject "cannot bind" \
  --reject "unexpected interrupt"
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

echo "[net] result: $result"
echo "[net] logs: $LOG_DIR"

if [ "$result" != "PASS" ]; then
  exit 1
fi
