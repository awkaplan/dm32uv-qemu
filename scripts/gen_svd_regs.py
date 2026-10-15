#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Generate include/hw/csky/dm32_svd.h from the HR_C7000 CMSIS-SVD.

Usage:
  scripts/gen_svd_regs.py SVD                write include/hw/csky/dm32_svd.h
  scripts/gen_svd_regs.py SVD -o FILE        write FILE instead
  scripts/gen_svd_regs.py SVD --check        exit 1 if the committed header is stale

SVD is dm32uv-re's data/registers/HR_C7000-DM32.svd. The SVD is not part of this repository; the
generated header is, and records the SVD's sha256.

For every peripheral the header gives its base, address-block size, PIC source and one X-macro
entry per register: offset, size, reset value and five bit masks (read-only, write-only,
write-1-to-clear, write-1-to-set, clear-on-read). include/hw/csky/dm32_regfile.h turns a list into
a register file that enforces them.

CMSIS-SVD rules applied:
- size, access and resetValue inherit device -> peripheral -> cluster -> register; a field's access
  inherits from its register. A register without its own resetValue is flagged as inherited.
- modifiedWriteValues oneToClear / oneToSet and readAction clear, on a field or a register.
- A peripheral with derivedFrom reuses its parent's registers and address-block size.
- A cluster with dim is expanded: TIMER_CH with dim 6 becomes TIMER_CH0_..TIMER_CH5_.
- Registers that share an offset with an alternateRegister are flagged; the register file uses the
  first unflagged one.

Standard library only.
"""

import argparse
import hashlib
import os
import re
import sys
import xml.etree.ElementTree as ET

DEFAULT_OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                            "include", "hw", "csky", "dm32_svd.h"))

FLAG_RESET_EXPLICIT = 0x1
FLAG_ALTERNATE = 0x2


class SvdError(Exception):
    pass


def num(text):
    text = text.strip().lower().replace("_", "")
    if text.startswith("#"):
        raise SvdError(f"binary literal with don't-care bits not supported: {text}")
    return int(text, 0)


def child(el, tag):
    e = el.find(tag)
    return None if e is None or e.text is None else e.text.strip()


def ident(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def field_range(f):
    if child(f, "bitOffset") is not None:
        lsb = num(child(f, "bitOffset"))
        width = num(child(f, "bitWidth") or "1")
        return lsb, lsb + width - 1
    if child(f, "lsb") is not None:
        return num(child(f, "lsb")), num(child(f, "msb"))
    m = re.fullmatch(r"\[(\d+):(\d+)\]", child(f, "bitRange") or "")
    if not m:
        raise SvdError(f"field {child(f, 'name')}: no bit position")
    return int(m.group(2)), int(m.group(1))


def bits(lo, hi):
    return ((1 << (hi - lo + 1)) - 1) << lo


class Props:
    """The inheritable register properties at one level of the hierarchy."""

    def __init__(self, el=None, parent=None):
        base = parent.__dict__ if parent else {"size": 32, "access": "read-write", "reset": 0,
                                               "modify": None, "read_action": None}
        self.__dict__.update(base)
        if el is None:
            return
        if child(el, "size") is not None:
            self.size = num(child(el, "size"))
        if child(el, "access") is not None:
            self.access = child(el, "access")
        if child(el, "resetValue") is not None:
            self.reset = num(child(el, "resetValue"))


def register_entry(reg, offset, name, props, alternate):
    p = Props(reg, props)
    size = p.size
    if size not in (8, 16, 32):
        raise SvdError(f"register {name}: size {size} not supported")
    full = (1 << size) - 1
    explicit = child(reg, "resetValue") is not None
    access = {i: p.access for i in range(size)}
    modify = {i: child(reg, "modifiedWriteValues") for i in range(size)}
    read_action = {i: child(reg, "readAction") for i in range(size)}
    for f in reg.findall("fields/field"):
        lo, hi = field_range(f)
        for i in range(lo, min(hi, size - 1) + 1):
            access[i] = child(f, "access") or p.access
            modify[i] = child(f, "modifiedWriteValues") or modify[i]
            read_action[i] = child(f, "readAction") or read_action[i]

    def mask(pred):
        return sum(1 << i for i in range(size) if pred(i))

    w1c = mask(lambda i: modify[i] == "oneToClear")
    w1s = mask(lambda i: modify[i] == "oneToSet")
    ro = mask(lambda i: access[i] == "read-only") & ~(w1c | w1s)
    wo = mask(lambda i: access[i] in ("write-only", "writeOnce"))
    rc = mask(lambda i: read_action[i] == "clear")
    flags = (FLAG_RESET_EXPLICIT if explicit else 0) | (FLAG_ALTERNATE if alternate else 0)
    return {"name": ident(name), "offset": offset, "size": size, "reset": p.reset & full,
            "flags": flags, "ro": ro & full, "wo": wo & full, "w1c": w1c & full,
            "w1s": w1s & full, "rc": rc & full}


def peripheral_registers(periph, props):
    out = []
    regs_el = periph.find("registers")
    if regs_el is None:
        return out
    for el in regs_el:
        if el.tag == "register":
            if child(el, "dim") is not None:
                raise SvdError(f"register {child(el, 'name')}: dim not supported")
            out.append(register_entry(el, num(child(el, "addressOffset")), child(el, "name"),
                                      props, child(el, "alternateRegister") is not None))
        elif el.tag == "cluster":
            cprops = Props(el, props)
            cname = child(el, "name").replace("[%s]", "").replace("%s", "")
            dim = num(child(el, "dim") or "1")
            step = num(child(el, "dimIncrement") or "0")
            base = num(child(el, "addressOffset"))
            for n in range(dim):
                prefix = f"{cname}{n}_" if child(el, "dim") is not None else f"{cname}_"
                for reg in el.findall("register"):
                    if child(reg, "dim") is not None:
                        raise SvdError(f"register {child(reg, 'name')}: dim not supported")
                    out.append(register_entry(
                        reg, base + n * step + num(child(reg, "addressOffset")),
                        prefix + child(reg, "name"), cprops,
                        child(reg, "alternateRegister") is not None))
    out.sort(key=lambda r: (r["offset"], r["flags"] & FLAG_ALTERNATE))
    return out


def generate(svd_path):
    data = open(svd_path, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    root = ET.fromstring(data)
    dprops = Props(root)
    periphs = {}
    order = []
    for p in root.iter("peripheral"):
        name = child(p, "name")
        periphs[name] = p
        order.append(name)

    lines = [
        "/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */",
        "/*",
        " * HR_C7000 register reset values and access masks, from the CMSIS-SVD",
        " *",
        " * Copyright (c) 2026 Alex Kaplan",
        " *",
        " * GENERATED by scripts/gen_svd_regs.py from dm32uv-re",
        " * data/registers/HR_C7000-DM32.svd (sha256 in DM32_SVD_SHA256). Do not edit",
        " * by hand: regenerate, and check with --check.",
        " *",
        " * Per peripheral P:",
        " *   DM32_SVD_P_BASE, DM32_SVD_P_SIZE   address and address-block size",
        " *   DM32_SVD_P_IRQ                    PIC source, when the SVD gives one",
        " *   DM32_SVD_P_REGS(X)                one X() per register, in offset order:",
        " *     X(name, offset, size, reset, flags, ro, wo, w1c, w1s, rc)",
        " * size is in bits. flags: DM32_SVD_RESET_EXPLICIT when the register states",
        " * its own resetValue (otherwise the value is inherited, usually 0, and may",
        " * not be the silicon's), DM32_SVD_ALTERNATE when the register shares its",
        " * offset with another (the UART's DLAB-selected registers). The masks are",
        " * read-only, write-only, write-1-to-clear, write-1-to-set and clear-on-read",
        " * bits. include/hw/csky/dm32_regfile.h applies them.",
        " *",
        " * DM32_SVD_PERIPHERALS(X) lists every peripheral as X(name, base, size).",
        " */",
        "",
        "#ifndef HW_CSKY_DM32_SVD_H",
        "#define HW_CSKY_DM32_SVD_H",
        "",
        "#define DM32_SVD_SHA256 \\",
        f'    "{sha}"',
        "",
        f"#define DM32_SVD_RESET_EXPLICIT 0x{FLAG_RESET_EXPLICIT:x}",
        f"#define DM32_SVD_ALTERNATE 0x{FLAG_ALTERNATE:x}",
    ]
    for name in order:
        p = periphs[name]
        macro = "DM32_SVD_" + ident(name).upper()
        parent_name = p.get("derivedFrom")
        src = periphs[parent_name] if parent_name else p
        if parent_name and p.find("registers") is not None:
            raise SvdError(f"{name}: derivedFrom with its own registers not supported")
        block = p.find("addressBlock") if p.find("addressBlock") is not None \
            else src.find("addressBlock")
        lines += ["", f"/* {name}" + (f", derived from {parent_name}" if parent_name else "")
                  + " */",
                  f"#define {macro}_BASE 0x{num(child(p, 'baseAddress')):08x}u"]
        if block is not None:
            lines.append(f"#define {macro}_SIZE 0x{num(child(block, 'size')):x}u")
        irqs = p.findall("interrupt")
        for irq in irqs:
            suffix = "_IRQ" if len(irqs) == 1 else "_IRQ_" + ident(child(irq, "name")).upper()
            lines.append(f"#define {macro}{suffix} {num(child(irq, 'value'))}")
        if parent_name:
            lines.append(f"#define {macro}_REGS(X) DM32_SVD_{ident(parent_name).upper()}"
                         "_REGS(X)")
            continue
        regs = peripheral_registers(p, Props(p, dprops))
        if not regs:
            lines.append(f"#define {macro}_REGS(X)")
            continue
        width = max(len(r["name"]) for r in regs)
        body = [f"#define {macro}_REGS(X)"]
        for r in regs:
            body.append(f"    X({r['name'] + ',':<{width + 1}} 0x{r['offset']:03x}, "
                        f"{r['size']:2d}, 0x{r['reset']:08x}, 0x{r['flags']:x},")
            body.append(f"      0x{r['ro']:08x}, 0x{r['wo']:08x}, 0x{r['w1c']:08x}, "
                        f"0x{r['w1s']:08x}, 0x{r['rc']:08x})")
        lines.append(" \\\n".join(f"{b:<77}" if i < len(body) - 1 else b
                                  for i, b in enumerate(body)).replace(" " * 1 + "\\", " \\"))
    blocks = []
    for name in order:
        p = periphs[name]
        src = periphs[p.get("derivedFrom")] if p.get("derivedFrom") else p
        block = p.find("addressBlock")
        block = block if block is not None else src.find("addressBlock")
        if block is not None:
            blocks.append(f"    X({ident(name).upper()}, 0x{num(child(p, 'baseAddress')):08x}u, "
                          f"0x{num(child(block, 'size')):x}u)")
    lines += ["", "/* Every peripheral's address block: X(name, base, size). */",
              " \\\n".join(["#define DM32_SVD_PERIPHERALS(X)"] + blocks)]
    lines += ["", "#endif /* HW_CSKY_DM32_SVD_H */", ""]
    text = "\n".join(lines)
    return re.sub(r" +\\$", " \\\\", text, flags=re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("svd", help="path to HR_C7000-DM32.svd")
    ap.add_argument("-o", "--out", default=DEFAULT_OUT, help="output header")
    ap.add_argument("--check", action="store_true",
                    help="compare with the existing header instead of writing it")
    args = ap.parse_args()
    try:
        text = generate(args.svd)
    except (SvdError, ET.ParseError, OSError) as e:
        print(f"gen_svd_regs: {e}", file=sys.stderr)
        return 2
    if args.check:
        try:
            current = open(args.out).read()
        except OSError:
            current = None
        if current != text:
            print(f"gen_svd_regs: {args.out} is stale; regenerate it with "
                  f"scripts/gen_svd_regs.py {args.svd}", file=sys.stderr)
            return 1
        print(f"gen_svd_regs: {args.out} is up to date")
        return 0
    with open(args.out, "w") as f:
        f.write(text)
    print(f"gen_svd_regs: wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
