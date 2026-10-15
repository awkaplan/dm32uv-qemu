# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Unit tests for scripts/gen_svd_regs.py, run against tests/svd/fixture.svd.

Set DM32_SVD to the real HR_C7000-DM32.svd to also check that the committed
include/hw/csky/dm32_svd.h is current.
"""

import hashlib
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
GEN = os.path.join(REPO, "scripts", "gen_svd_regs.py")
FIXTURE = os.path.join(HERE, "fixture.svd")
COMMITTED = os.path.join(REPO, "include", "hw", "csky", "dm32_svd.h")
DM32_SVD = os.environ.get("DM32_SVD")

FIELDS = ("name", "offset", "size", "reset", "flags", "ro", "wo", "w1c",
          "w1s", "rc")


def run_gen(*args):
    return subprocess.run([sys.executable, GEN] + list(args),
                          capture_output=True, text=True)


def read_text(path):
    with open(path) as f:
        return f.read()


def reg_entries(text, periph):
    """Parse the X() entries of DM32_SVD_<periph>_REGS(X) in a header."""
    lines = text.split("\n")
    start = [i for i, ln in enumerate(lines)
             if ln.startswith(f"#define DM32_SVD_{periph}_REGS(X)")]
    if len(start) != 1:
        raise AssertionError(f"no unique REGS macro for {periph}")
    body = []
    for ln in lines[start[0]:]:
        body.append(ln.rstrip().rstrip("\\"))
        if not ln.rstrip().endswith("\\"):
            break
    entries = []
    for args in re.findall(r"X\(([^)]*)\)", " ".join(body)):
        parts = [p.strip() for p in args.split(",")]
        vals = [int(p, 0) if i else p for i, p in enumerate(parts)]
        entries.append(dict(zip(FIELDS, vals)))
    return entries


def entry_map(text, periph):
    return {e["name"]: e for e in reg_entries(text, periph)}


class GenTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = os.path.join(cls.tmp.name, "fixture.h")
        r = run_gen(FIXTURE, "-o", cls.out)
        if r.returncode != 0:
            raise AssertionError(r.stderr)
        cls.text = read_text(cls.out)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def tmp_svd(self, name, text):
        path = os.path.join(self.tmp.name, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def fixture_text(self):
        return read_text(FIXTURE)

    # Exact X() entries, one row per register, in offset order.

    def test_testa_entries_exact(self):
        want = [
            # name, offset, size, reset, flags, ro, wo, w1c, w1s, rc
            ("STAT", 0x000, 32, 0x000000aa, 0x0,
             0xfffffffd, 0, 0, 0, 0),
            ("CTRL", 0x004, 32, 0x00000010, 0x1,
             0x00000030, 0x0000000e, 0, 0, 0),
            ("INT_ST", 0x008, 32, 0x000000aa, 0x0,
             0, 0, 0x00000019, 0x00000002, 0x00000004),
            ("RC_REG", 0x00c, 32, 0, 0x1, 0, 0, 0, 0, 0xffffffff),
            ("BYTE", 0x010, 8, 0x000000a5, 0x1, 0x0f, 0, 0, 0, 0),
            ("DATA_A", 0x020, 32, 0x00000011, 0x1, 0, 0, 0, 0, 0),
            ("DATA_B", 0x020, 32, 0x000000aa, 0x2, 0, 0, 0, 0, 0),
            ("CH0_CFG", 0x040, 32, 0x00000005, 0x0,
             0xffffffff, 0, 0, 0, 0),
            ("CH0_CNT", 0x044, 32, 0, 0x1, 0, 0, 0, 0, 0),
            ("CH1_CFG", 0x050, 32, 0x00000005, 0x0,
             0xffffffff, 0, 0, 0, 0),
            ("CH1_CNT", 0x054, 32, 0, 0x1, 0, 0, 0, 0, 0),
            ("CH2_CFG", 0x060, 32, 0x00000005, 0x0,
             0xffffffff, 0, 0, 0, 0),
            ("CH2_CNT", 0x064, 32, 0, 0x1, 0, 0, 0, 0, 0),
        ]
        got = [tuple(e[f] for f in FIELDS)
               for e in reg_entries(self.text, "TESTA")]
        self.assertEqual(got, want)

    def test_entries_sorted_by_offset_alternate_last(self):
        offs = [(e["offset"], e["flags"] & 0x2)
                for e in reg_entries(self.text, "TESTA")]
        self.assertEqual(offs, sorted(offs))

    # Inheritance and resets.

    def test_device_reset_inherited_without_flag(self):
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["STAT"]["reset"], 0xaa)
        self.assertEqual(e["STAT"]["flags"] & 0x1, 0)
        self.assertEqual(e["INT_ST"]["reset"], 0xaa)
        self.assertEqual(e["INT_ST"]["flags"] & 0x1, 0)

    def test_explicit_reset_flagged(self):
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["CTRL"]["reset"], 0x10)
        self.assertEqual(e["CTRL"]["flags"] & 0x1, 0x1)
        self.assertEqual(e["RC_REG"]["flags"] & 0x1, 0x1)

    def test_cluster_reset_inherited(self):
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["CH0_CFG"]["reset"], 5)
        self.assertEqual(e["CH0_CFG"]["flags"] & 0x1, 0)
        self.assertEqual(e["CH0_CNT"]["flags"] & 0x1, 0x1)

    # Access: peripheral, register and field levels.

    def test_peripheral_access_inherited_by_register(self):
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["STAT"]["ro"] & 0xfffffffd, 0xfffffffd)
        self.assertEqual(e["CH0_CFG"]["ro"], 0xffffffff)

    def test_register_access_overrides_peripheral(self):
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["CTRL"]["ro"], 0x30)
        self.assertEqual(e["CTRL"]["wo"], 0x0e)

    def test_field_access_inherits_register(self):
        e = entry_map(self.text, "TESTA")
        # BUSY has no access: read-only from STAT. ERR overrides to rw.
        self.assertEqual(e["STAT"]["ro"] & 0x3, 0x1)
        # LEVEL has no access: read-write from CTRL, so no mask bits.
        self.assertEqual(e["CTRL"]["ro"] & 0xc0, 0)
        self.assertEqual(e["CTRL"]["wo"] & 0xc0, 0)

    def test_field_read_only_and_write_only(self):
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["CTRL"]["ro"], 0x30)     # STATUS [5:4]
        self.assertEqual(e["CTRL"]["wo"], 0x0e)     # MODE [3:1]

    # Modified write values and read actions.

    def test_one_to_clear_and_one_to_set(self):
        e = entry_map(self.text, "TESTA")["INT_ST"]
        self.assertEqual(e["w1c"], 0x19)  # FLAG_A bit 0, FLAG_D [4:3]
        self.assertEqual(e["w1s"], 0x02)  # FLAG_B bit 1

    def test_read_action_clear_on_field(self):
        e = entry_map(self.text, "TESTA")["INT_ST"]
        self.assertEqual(e["rc"], 0x04)  # FLAG_C bit 2

    def test_read_action_clear_on_register(self):
        e = entry_map(self.text, "TESTA")["RC_REG"]
        self.assertEqual(e["rc"], 0xffffffff)

    # Register size.

    def test_8bit_register(self):
        e = entry_map(self.text, "TESTA")["BYTE"]
        self.assertEqual(e["size"], 8)
        self.assertEqual(e["reset"], 0xa5)
        self.assertEqual(e["ro"], 0x0f)  # LOW [3:0] read-only

    # Alternate registers and clusters.

    def test_alternate_register_flagged(self):
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["DATA_A"]["flags"] & 0x2, 0)
        self.assertEqual(e["DATA_B"]["flags"] & 0x2, 0x2)
        self.assertEqual(e["DATA_A"]["offset"], e["DATA_B"]["offset"])

    def test_cluster_dim_expansion(self):
        names = [e["name"] for e in reg_entries(self.text, "TESTA")]
        self.assertEqual([n for n in names if n.startswith("CH")],
                         ["CH0_CFG", "CH0_CNT", "CH1_CFG", "CH1_CNT",
                          "CH2_CFG", "CH2_CNT"])
        e = entry_map(self.text, "TESTA")
        self.assertEqual(e["CH2_CNT"]["offset"], 0x064)

    # Peripheral macros and the derivedFrom alias.

    def test_peripheral_macros(self):
        self.assertIn("#define DM32_SVD_TESTA_BASE 0x40000000u", self.text)
        self.assertIn("#define DM32_SVD_TESTA_SIZE 0x100u", self.text)
        self.assertIn("#define DM32_SVD_TESTA_IRQ 5", self.text)
        self.assertIn("#define DM32_SVD_RESET_EXPLICIT 0x1", self.text)
        self.assertIn("#define DM32_SVD_ALTERNATE 0x2", self.text)

    def test_derived_peripheral(self):
        self.assertIn("#define DM32_SVD_TESTB_BASE 0x50000000u", self.text)
        self.assertIn("#define DM32_SVD_TESTB_SIZE 0x100u", self.text)
        self.assertIn("#define DM32_SVD_TESTB_IRQ 9", self.text)
        self.assertIn("#define DM32_SVD_TESTB_REGS(X) DM32_SVD_TESTA_REGS(X)",
                      self.text)

    def test_sha256_recorded(self):
        with open(FIXTURE, "rb") as f:
            want = hashlib.sha256(f.read()).hexdigest()
        self.assertIn(f'"{want}"', self.text)

    # --check.

    def test_check_fresh_then_stale(self):
        r = run_gen(FIXTURE, "--check", "-o", self.out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.out, "a") as f:
            f.write("/* edited */\n")
        r = run_gen(FIXTURE, "--check", "-o", self.out)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("stale", r.stderr)

    # Unsupported input.

    def test_register_dim_is_unsupported(self):
        text = self.fixture_text().replace(
            "<name>STAT</name>",
            "<name>STAT</name><dim>2</dim><dimIncrement>4</dimIncrement>", 1)
        svd = self.tmp_svd("regdim.svd", text)
        r = run_gen(svd, "-o", os.path.join(self.tmp.name, "x.h"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("dim not supported", r.stderr)

    def test_register_dim_inside_cluster_is_unsupported(self):
        """A register-level <dim> inside a <cluster> is rejected too (exit 2)."""
        text = self.fixture_text().replace(
            "<name>CNT</name>",
            "<name>CNT%s</name><dim>2</dim><dimIncrement>4</dimIncrement>",
            1)
        svd = self.tmp_svd("clusterdim.svd", text)
        r = run_gen(svd, "-o", os.path.join(self.tmp.name, "y.h"))
        self.assertEqual(r.returncode, 2, r.stderr)


@unittest.skipUnless(DM32_SVD, "set DM32_SVD to the real HR_C7000-DM32.svd")
class CommittedHeaderTest(unittest.TestCase):
    def test_committed_header_is_current(self):
        r = run_gen(DM32_SVD, "--check")
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
