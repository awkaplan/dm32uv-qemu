#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
# Usage: tests/run-qtest.sh /path/to/qemu-system-cskyv2 [unittest args...]
# Runs the register-level device tests in tests/qtest/ against the given binary. No firmware is
# needed. Extra arguments go to unittest, for example a single module: test_wdt.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 /path/to/qemu-system-cskyv2 [unittest args...]" >&2
    exit 2
fi

QEMU="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
DIR="$(cd "$(dirname "$0")" && pwd)/qtest"

cd "$DIR"
if [ $# -eq 0 ]; then
    set -- discover -s . -p 'test_*.py'
fi
if DM32_QEMU="$QEMU" python3 -m unittest "$@"; then
    echo "dm32uv-qemu qtest: PASS"
else
    echo "dm32uv-qemu qtest: FAILED" >&2
    exit 1
fi
