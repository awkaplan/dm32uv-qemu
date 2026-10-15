#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
"""Print a hash of the compiled code of every C file in this overlay.

Usage: tests/objhash.py /path/to/qemu [> hashes.txt]

Use it to show that a change is behaviour-neutral (comments, formatting, removing code that the
preprocessor already drops): run it on a built QEMU checkout before the change, install the changed
overlay, run it again, and diff the two outputs. Identical hashes mean identical machine code.

Each file is recompiled with the exact command from build/compile_commands.json, plus -g0 (no
debug info) and __LINE__ defined to 0 (so moving code between lines does not change constants in
assertions), and ELF objects are stripped before hashing (see strip()). Objects go to a temporary
directory; the QEMU build tree is not modified. Hashes are comparable only between runs on the same
host with the same compiler.
"""

import hashlib
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile

OVERLAY = pathlib.Path(__file__).resolve().parent.parent
SOURCE_DIRS = ("hw",)


def overlay_sources():
    """Paths, relative to the overlay root, of the C files this overlay installs."""
    return {str(p.relative_to(OVERLAY))
            for d in SOURCE_DIRS for p in (OVERLAY / d).rglob("*.c")}


def strip(obj):
    """Remove debug sections and the compiler-identification comment from an ELF object.

    Mach-O objects (macOS) are left as they are: with -g0 they carry no debug sections and no
    .comment, and GNU strip, if it comes first on PATH, cannot always rewrite them.
    """
    if obj.read_bytes()[:4] != b"\x7fELF":
        return
    subprocess.run(["strip", "--strip-debug", "-R", ".comment", str(obj)], check=True)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[2])
    qemu = pathlib.Path(sys.argv[1]).resolve()
    commands = json.loads((qemu / "build" / "compile_commands.json").read_text())
    wanted = overlay_sources()
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        for entry in commands:
            src = (pathlib.Path(entry["directory"]) / entry["file"]).resolve()
            try:
                rel = str(src.relative_to(qemu))
            except ValueError:
                continue
            if rel not in wanted:
                continue
            obj = pathlib.Path(tmp) / (rel.replace("/", "_") + ".o")
            args, skip = [], False
            for arg in shlex.split(entry["command"]):
                if skip:
                    skip = False
                elif arg in ("-o", "-MQ", "-MF"):
                    skip = True
                elif arg not in ("-MD", "-g"):
                    args.append(arg)
            args += ["-g0", "-D__LINE__=0", "-Wno-builtin-macro-redefined",
                     "-Wno-error", "-o", str(obj)]
            result = subprocess.run(args, cwd=entry["directory"],
                                    capture_output=True, text=True)
            if result.returncode:
                print(f"FAILED {rel}\n{result.stderr}", file=sys.stderr)
                failed = True
                continue
            strip(obj)
            print(hashlib.sha256(obj.read_bytes()).hexdigest()[:16], rel)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
