#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
#
# Install the Baofeng DM-32UV board model into a QEMU checkout that already has
# csky-qemu (https://github.com/awkaplan/csky-qemu) installed.
#
# Usage: ./install.sh /path/to/qemu
#
# Copies hw/ and include/ into the checkout at the same paths, then applies
# integration/top-level.diff, which registers the device files with QEMU's
# build. hw/csky/Kconfig and hw/csky/meson.build replace csky-qemu's copies:
# they are csky-qemu's files with the dm32 machine added. Safe to run again:
# the files are recopied and an already-applied patch is skipped.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${1:?usage: install.sh /path/to/qemu}"

if [[ ! -f "$DEST/VERSION" || ! -d "$DEST/target" ]]; then
    echo "error: $DEST does not look like a QEMU checkout (no VERSION or target/)" >&2
    exit 1
fi

if [[ ! -f "$DEST/target/csky/cpu.h" ]]; then
    echo "error: $DEST/target/csky/cpu.h not found." >&2
    echo "  dm32uv-qemu needs csky-qemu (the C-SKY CPU target) installed first:" >&2
    echo "    git clone https://github.com/awkaplan/csky-qemu.git" >&2
    echo "    ./csky-qemu/install.sh $DEST" >&2
    exit 1
fi

echo "copying files..."
(cd "$SRC" && find hw include -type f) | while read -r f; do
    mkdir -p "$DEST/$(dirname "$f")"
    cp "$SRC/$f" "$DEST/$f"
done

echo "applying build registrations..."
if (cd "$DEST" && git apply --check "$SRC/integration/top-level.diff" 2>/dev/null); then
    (cd "$DEST" && git apply "$SRC/integration/top-level.diff")
    echo "  applied"
elif (cd "$DEST" && git apply --reverse --check "$SRC/integration/top-level.diff" 2>/dev/null); then
    echo "  already applied, skipping"
else
    echo "error: integration/top-level.diff does not apply to $DEST." >&2
    echo "  The checkout has probably moved past the commit in QEMU_COMMIT. Each hunk adds" >&2
    echo "  one Kconfig entry or one meson.build line; add them by hand (see CONTRIBUTING.md)." >&2
    exit 1
fi

echo
echo "Done. In $DEST:"
echo "  ./configure --target-list=cskyv2-softmmu"
echo "  ninja -C build qemu-system-cskyv2"
echo "  ./build/qemu-system-cskyv2 -M help    # lists dm32"
