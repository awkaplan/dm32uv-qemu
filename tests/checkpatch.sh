#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
#
# Run QEMU's scripts/checkpatch.pl over every C file in this repository.
#
# Usage: tests/checkpatch.sh /path/to/qemu
#
# Fails if checkpatch reports anything not listed in tests/checkpatch-allow.txt. Each line there
# is "<path>: <exact checkpatch message>" ("*" as the path matches any file); keep a comment
# above each entry saying why it stays.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU="$(cd "${1:?usage: tests/checkpatch.sh /path/to/qemu}" && pwd)"
CHECKPATCH="$QEMU/scripts/checkpatch.pl"
ALLOW="$ROOT/tests/checkpatch-allow.txt"

[[ -x "$CHECKPATCH" ]] || { echo "error: $CHECKPATCH not found" >&2; exit 1; }

found="$(mktemp)"
anypath="$(mktemp)"
trap 'rm -f "$found" "$anypath"' EXIT
cd "$ROOT"
# checkpatch exits 0 (clean) or 1 (findings). Anything else, or output that is not a finding or
# the "total:" line, means it did not run properly (e.g. missing Perl modules): fail, never pass.
while read -r file; do
    rc=0
    out="$("$CHECKPATCH" --no-tree --terse -f "$file" 2>&1)" || rc=$?
    other="$(grep -vE '^[^:]+:[0-9]+: (ERROR|WARNING|CHECK): |^total: ' <<< "$out" | grep . || true)"
    if (( rc > 1 )) || [[ -n "$other" ]]; then
        echo "error: checkpatch failed on $file (exit $rc):" >&2
        echo "$out" >&2
        exit 1
    fi
    grep -E ': (ERROR|WARNING|CHECK): ' <<< "$out" || true
done < <(find hw include tests -name '*.[ch]' | sort) > "$found"

# --terse lines look like "path:line: ERROR: message"; drop the line number, then drop every
# line that matches an allowlist entry exactly or, for "*: message" entries, by message alone.
# The "*: " messages go to awk as a file, not through -v: BSD awk rejects newlines in -v values.
# Only grep's "no lines selected" status (1) is tolerated; any other failure stops the script.
exact="$(grep -v '^#' "$ALLOW" | grep -v '^\*: ' | grep . || true)"
grep '^\*: ' "$ALLOW" | sed 's/^\*: //' > "$anypath" || true
unexpected="$(sed -E 's/^([^:]+):[0-9]+: /\1: /' "$found" \
    | { grep -vxF -e "${exact:-\x00}" || [[ $? -eq 1 ]]; } \
    | awk 'FILENAME == ARGV[1] { allowed[$0] = 1; next }
           { msg = $0; sub(/^[^:]+: /, "", msg); if (!(msg in allowed)) print }' "$anypath" -)"
if [[ -n "$unexpected" ]]; then
    echo "checkpatch findings:"
    echo "$unexpected"
    exit 1
fi
echo "checkpatch: clean ($(wc -l < "$found" | tr -d ' ') allowed findings)"
