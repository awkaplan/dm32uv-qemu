#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
# Usage: tests/run-smoke.sh /path/to/qemu-system-cskyv2
# Boots tests/smoke/smoke.bin on the dm32 board and checks for the PASS line.
# Exit 0 only if PASS was printed. The program halts by design, so QEMU is
# stopped after 10 seconds and the timeout status 124 is expected.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 /path/to/qemu-system-cskyv2" >&2
    exit 2
fi

QEMU="$1"
DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE="$DIR/smoke/smoke.bin"
PASS="dm32uv-qemu smoke test: PASS"

# GNU timeout where available; otherwise (macOS) a shell watchdog that
# reports the same status 124.
with_timeout() {
    local secs="$1"
    shift
    if command -v timeout > /dev/null; then
        timeout "$secs" "$@"
        return
    fi
    "$@" &
    local pid=$! flag
    flag="$(mktemp)"
    ( sleep "$secs" && kill "$pid" 2> /dev/null && echo x > "$flag" ) > /dev/null 2>&1 &
    local watchdog=$! rc=0
    wait "$pid" || rc=$?
    kill "$watchdog" 2> /dev/null || true
    wait "$watchdog" 2> /dev/null || true
    [[ -s "$flag" ]] && rc=124
    rm -f "$flag"
    return "$rc"
}

rc=0
out="$(with_timeout 10 "$QEMU" -M dm32 -cpu ck803 -display none -monitor none \
    -serial stdio -kernel "$IMAGE" < /dev/null 2>&1)" || rc=$?

echo "$out"
if grep -qF "$PASS" <<< "$out"; then
    echo "smoke test: OK (qemu exit status $rc, 124 = timeout, expected)"
    exit 0
fi
echo "smoke test: FAILED - no PASS line from $QEMU (qemu exit status $rc)" >&2
exit 1
