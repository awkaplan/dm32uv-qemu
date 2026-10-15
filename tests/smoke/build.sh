#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
# Rebuild smoke/smoke.bin (a raw program-flash image) with the C-SKY ABIv2
# toolchain. Override the toolchain prefix with CROSS, e.g.
#   CROSS=/opt/csky-elfabiv2-toolchain/bin/csky-abiv2-elf- tests/smoke/build.sh
set -euo pipefail

cd "$(dirname "$0")"
CROSS="${CROSS:-csky-abiv2-elf-}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"${CROSS}gcc" -mcpu=ck803 -nostdlib -nostartfiles -T smoke.ld \
    -o "$tmp/smoke.elf" smoke.S
"${CROSS}objcopy" -O binary "$tmp/smoke.elf" smoke.bin
echo "built $(pwd)/smoke.bin"
