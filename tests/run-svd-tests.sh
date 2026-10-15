#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
# Usage: tests/run-svd-tests.sh [/path/to/HR_C7000-DM32.svd]
# Host-side tests for scripts/gen_svd_regs.py and the register-file helper
# include/hw/csky/dm32_regfile.h. Runs against a synthetic SVD by default.
# With an SVD argument, also checks that the committed dm32_svd.h is current.
# Set CC to override the C compiler (default: cc).
set -euo pipefail

if [ $# -gt 1 ]; then
    echo "usage: $0 [/path/to/HR_C7000-DM32.svd]" >&2
    exit 2
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/dm32-svd-tests.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

if [ $# -eq 1 ]; then
    export DM32_SVD="$1"
fi

${CC:-cc} -std=c11 -Wall -Wextra -Werror -I "$REPO/include" \
    "$DIR/svd/test_regfile.c" -o "$TMP/test_regfile"
"$TMP/test_regfile"

python3 -m unittest discover -v -s "$DIR/svd" -p test_gen.py \
    -t "$DIR/svd"

echo "dm32uv-qemu svd tests: PASS"
