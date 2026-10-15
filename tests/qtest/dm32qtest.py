# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Register-level test client for the dm32 board, over QEMU's qtest protocol.

Each Machine is one `qemu-system-cskyv2 -M dm32 -accel qtest` process with no firmware. The CPU
does not run. The test reads and writes MMIO, advances the virtual clock and watches interrupt
lines directly, through two Unix sockets: the qtest socket (the protocol in QEMU's
system/qtest.c) and a QMP socket (used for QOM lookups and for a clean quit).

The binary comes from the DM32_QEMU environment variable. Inherited DM32_* variables are removed
from the child's environment, so each test sets exactly the ones it needs.

Standard library only.
"""

import base64
import json
import os
import re
import shutil
import socket
import subprocess
import tempfile
import unittest

QEMU_ENV = "DM32_QEMU"
BASE_ARGS = ["-M", "dm32", "-cpu", "ck803", "-display", "none", "-monitor", "none",
             "-serial", "null", "-accel", "qtest"]
EVENTS_RE = re.compile(r"^dm32-events: (.*)$", re.M)


class QTestError(Exception):
    """A qtest or QMP command failed."""


def qemu_binary():
    path = os.environ.get(QEMU_ENV)
    if not path:
        raise unittest.SkipTest(f"{QEMU_ENV} is not set (path to qemu-system-cskyv2)")
    return path


def child_env(env):
    """The parent's environment without DM32_* variables, plus `env`."""
    out = {k: v for k, v in os.environ.items() if not k.startswith("DM32_")}
    out.update({k: str(v) for k, v in (env or {}).items()})
    return out


def parse_events(text):
    """The counts from the last `dm32-events:` line in `text`, as a dict.

    Integer counts are ints; `strict_stop` stays a string.
    """
    lines = EVENTS_RE.findall(text)
    if not lines:
        return None
    out = {}
    for tok in lines[-1].split():
        key, _, val = tok.partition("=")
        out[key] = int(val) if val.isdigit() else val
    return out


def run_plain(env=None, args=(), timeout=10):
    """Run the board without a qtest connection. Returns (exit status, combined output).

    For start-up checks, such as a rejected DM32_STRICT value, and for runs that must end on their
    own. A run still alive after `timeout` seconds is killed and reported as status None.
    """
    cmd = [qemu_binary()] + BASE_ARGS[:-2] + list(args)
    try:
        p = subprocess.run(cmd, env=child_env(env), stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as e:
        return None, (e.output or b"").decode(errors="replace")


class Machine:
    """One dm32 board under qtest.

    irq_level maps an intercepted line number to its last level (True = high). irq_log lists every
    (level, line) change in order. Both are only filled after irq_intercept_in() or
    irq_intercept_out().
    """

    def __init__(self, env=None, args=(), timeout=10.0):
        self._dir = tempfile.mkdtemp(prefix="dm32qt-")
        self._timeout = timeout
        self._buf = b""
        self._qmp_buf = b""
        self._closed = False
        self.output = ""
        self.irq_level = {}
        self.irq_log = []

        qpath = os.path.join(self._dir, "q")
        mpath = os.path.join(self._dir, "m")
        listeners = []
        for path in (qpath, mpath):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.bind(path)
            s.listen(1)
            s.settimeout(timeout)
            listeners.append(s)

        self._out_path = os.path.join(self._dir, "output.txt")
        self._out = open(self._out_path, "wb")
        cmd = ([qemu_binary()] + BASE_ARGS
               + ["-qtest", "unix:" + qpath, "-qtest-log", os.devnull, "-qmp", "unix:" + mpath]
               + list(args))
        self.proc = subprocess.Popen(cmd, env=child_env(env), stdin=subprocess.DEVNULL,
                                     stdout=self._out, stderr=subprocess.STDOUT)
        try:
            self._q, _ = listeners[0].accept()
            self._m, _ = listeners[1].accept()
        except socket.timeout:
            self.close()
            raise QTestError("QEMU did not connect; output:\n" + self.output)
        finally:
            for s in listeners:
                s.close()
        self._q.settimeout(timeout)
        self._m.settimeout(timeout)
        self._qmp_read()                              # greeting
        self.qmp("qmp_capabilities")

    # ---- qtest -------------------------------------------------------------------------------

    def _readline(self):
        while b"\n" not in self._buf:
            chunk = self._q.recv(4096)
            if not chunk:
                raise QTestError("qtest connection closed; output:\n" + self._read_output())
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return line.decode()

    def cmd(self, line):
        """Send one qtest command. Returns the text after OK, or raises QTestError."""
        self._q.sendall((line + "\n").encode())
        while True:
            resp = self._readline()
            if resp.startswith("IRQ "):
                _, kind, n = resp.split()
                level = kind == "raise"
                self.irq_level[int(n)] = level
                self.irq_log.append((level, int(n)))
                continue
            if resp == "OK" or resp.startswith("OK "):
                return resp[3:]
            raise QTestError(f"{line!r}: {resp}")

    def readb(self, addr):
        return int(self.cmd(f"readb 0x{addr:x}"), 16)

    def readw(self, addr):
        return int(self.cmd(f"readw 0x{addr:x}"), 16)

    def readl(self, addr):
        return int(self.cmd(f"readl 0x{addr:x}"), 16)

    def writeb(self, addr, val):
        self.cmd(f"writeb 0x{addr:x} 0x{val & 0xff:x}")

    def writew(self, addr, val):
        self.cmd(f"writew 0x{addr:x} 0x{val & 0xffff:x}")

    def writel(self, addr, val):
        self.cmd(f"writel 0x{addr:x} 0x{val & 0xffffffff:x}")

    def read(self, addr, size):
        """`size` bytes from guest memory, as bytes."""
        return base64.b64decode(self.cmd(f"b64read 0x{addr:x} 0x{size:x}"))

    def write(self, addr, data):
        """Write bytes to guest memory."""
        enc = base64.b64encode(bytes(data)).decode()
        self.cmd(f"b64write 0x{addr:x} 0x{len(data):x} {enc}")

    def clock_step(self, ns=None):
        """Advance virtual time by `ns`, or to the next timer deadline. Returns the new time."""
        return int(self.cmd("clock_step" if ns is None else f"clock_step {int(ns)}"))

    def clock_set(self, ns):
        return int(self.cmd(f"clock_set {int(ns)}"))

    def irq_intercept_in(self, path):
        """Observe every input line of the device at `path`. One interception per Machine."""
        self.cmd(f"irq_intercept_in {path}")

    def irq_intercept_out(self, path, name=None):
        """Observe the device's output lines (`name` = a named GPIO list, e.g. sysbus-irq)."""
        self.cmd(f"irq_intercept_out {path}" + (f" {name}" if name else ""))

    def set_irq_in(self, path, n, level, name="unnamed-gpio-in"):
        """Drive input line `n` of the device at `path`."""
        self.cmd(f"set_irq_in {path} {name} {n} {int(level)}")

    # ---- QMP ---------------------------------------------------------------------------------

    def _qmp_read(self):
        while b"\n" not in self._qmp_buf:
            chunk = self._m.recv(65536)
            if not chunk:
                raise QTestError("QMP connection closed")
            self._qmp_buf += chunk
        line, _, self._qmp_buf = self._qmp_buf.partition(b"\n")
        return json.loads(line)

    def qmp(self, command, **arguments):
        msg = {"execute": command}
        if arguments:
            msg["arguments"] = arguments
        self._m.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            resp = self._qmp_read()
            if "return" in resp:
                return resp["return"]
            if "error" in resp:
                raise QTestError(f"QMP {command}: {resp['error']}")

    def find_devices(self, typename):
        """QOM paths of every device of `typename`, in creation order."""
        found = []
        for prop in self.qmp("qom-list", path="/machine/unattached"):
            if prop["type"] == f"child<{typename}>":
                m = re.search(r"\[(\d+)\]$", prop["name"])
                found.append((int(m.group(1)) if m else 0,
                              "/machine/unattached/" + prop["name"]))
        return [p for _, p in sorted(found)]

    def pic_path(self):
        (path,) = self.find_devices("dm32_pic")
        return path

    def pic_cpu_line(self):
        """The value the PIC drives on the core interrupt input (its read-only "cpu-line"
        property): the vector in bits 7:0 ORed with bit 10, or 0 when nothing is in service."""
        return self.qmp("qom-get", path=self.pic_path(), property="cpu-line")

    # ---- shutdown ----------------------------------------------------------------------------

    def _read_output(self):
        if not self._out.closed:
            self._out.flush()
        with open(self._out_path, "rb") as f:
            return f.read().decode(errors="replace")

    def peek_output(self):
        """Everything QEMU has printed so far, without stopping it.

        Device messages written with fprintf(stderr) are unbuffered, so they are visible as soon as
        the qtest command that caused them has returned.
        """
        return self._read_output()

    def close(self):
        """Quit QEMU cleanly (exit handlers run) and return everything it printed."""
        if self._closed:
            return self.output
        self._closed = True
        try:
            self.qmp("quit")
        except (QTestError, OSError, AttributeError):
            pass
        try:
            self.proc.wait(timeout=self._timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        for s in ("_q", "_m"):
            if hasattr(self, s):
                getattr(self, s).close()
        self._out.close()
        self.output = self._read_output()
        shutil.rmtree(self._dir, ignore_errors=True)
        return self.output

    def events(self):
        """Close the machine and return its dm32-events counts (see parse_events)."""
        return parse_events(self.close())


class DM32TestCase(unittest.TestCase):
    """Base class: self.start() returns a fresh Machine that is closed after the test."""

    def start(self, env=None, args=()):
        m = Machine(env=env, args=args)
        self.addCleanup(m.close)
        return m

    def make_tempdir(self):
        """A new empty directory, removed after the test. Returns its path as a str."""
        path = tempfile.mkdtemp(prefix="dm32-qtest-")
        self.addCleanup(shutil.rmtree, path, ignore_errors=True)
        return path


# ---- Bit-banged I2C on GPIOA (from test_gpio_i2c; also used by test_rda5807m) ----

GPIOA = 0x14020000
DR, DDR, EXT = GPIOA + 0x00, GPIOA + 0x04, GPIOA + 0x50
EEPROM_ADDR = 0x50


class BitBang:
    """An I2C master on two GPIOA bits, push-pull like stock: the SoC drives both lines and
    releases SDA (SWPORTA_DDR bit clear) only to read it."""

    def __init__(self, m, scl, sda):
        self.m, self.scl, self.sda = m, 1 << scl, 1 << sda
        self.dr = self.scl | self.sda
        self.ddr = self.scl | self.sda
        m.writel(DR, self.dr)
        m.writel(DDR, self.ddr)

    def _line(self, mask, level):
        self.dr = (self.dr | mask) if level else (self.dr & ~mask)
        self.m.writel(DR, self.dr)

    def _drive_sda(self, on):
        self.ddr = (self.ddr | self.sda) if on else (self.ddr & ~self.sda)
        self.m.writel(DDR, self.ddr)

    def sda_in(self):
        return 1 if self.m.readl(EXT) & self.sda else 0

    def start(self):
        self._drive_sda(True)
        self._line(self.sda, 1)
        self._line(self.scl, 1)
        self._line(self.sda, 0)
        self._line(self.scl, 0)

    def stop(self):
        self._drive_sda(True)
        self._line(self.sda, 0)
        self._line(self.scl, 1)
        self._line(self.sda, 1)

    def write_byte(self, byte, release_for_ack=True):
        """Shift out one byte; return True if the slave ACKed."""
        self._drive_sda(True)
        for i in range(7, -1, -1):
            self._line(self.sda, (byte >> i) & 1)
            self._line(self.scl, 1)
            self._line(self.scl, 0)
        if release_for_ack:
            self._drive_sda(False)
        else:
            self._line(self.sda, 1)
        self._line(self.scl, 1)
        ack = self.sda_in() == 0
        self._line(self.scl, 0)
        return ack

    def read_byte(self, ack):
        self._drive_sda(False)
        byte = 0
        for _ in range(8):
            self._line(self.scl, 1)
            byte = (byte << 1) | self.sda_in()
            self._line(self.scl, 0)
        self._drive_sda(True)
        self._line(self.sda, 0 if ack else 1)
        self._line(self.scl, 1)
        self._line(self.scl, 0)
        return byte

    def eeprom_write(self, addr, data):
        self.start()
        acks = [self.write_byte(EEPROM_ADDR << 1), self.write_byte(addr)]
        acks += [self.write_byte(b) for b in data]
        self.stop()
        return all(acks)

    def eeprom_read(self, addr, n):
        self.start()
        if not (self.write_byte(EEPROM_ADDR << 1) and self.write_byte(addr)):
            self.stop()
            return None
        self.start()
        if not self.write_byte((EEPROM_ADDR << 1) | 1):
            self.stop()
            return None
        data = [self.read_byte(ack=i < n - 1) for i in range(n)]
        self.stop()
        return bytes(data)


# ---- I2C controller and RTC helpers (from test_i2c; also used by test_reset, test_pinmux) ----

I2C0, I2C1, I2C2 = 0x14060000, 0x14070000, 0x14080000
CON, TAR, DATA_CMD, FS_HCNT, FS_LCNT = 0x00, 0x04, 0x10, 0x1C, 0x20
CLR_INTR, CLR_RX_UNDER, CLR_RX_OVER, CLR_TX_OVER, CLR_TX_ABRT = 0x40, 0x44, 0x48, 0x4C, 0x54
ENABLE, STATUS, TXFLR, RXFLR, ABRT_SRC, EN_STATUS, START = (0x6C, 0x70, 0x74, 0x78, 0x80, 0x9C,
                                                           0xA0)
READ = 0x100                     # IC_DATA_CMD.CMD
# Stock's I2C2 set-up (i2c2_master_init_400khz @0x0301dacc with its 84 MHz clock): master,
# fast mode, RESTART enabled, 7-bit; SCL counts 97/104, a 210-cycle (2.5 us) period.
STOCK_CON, STOCK_HCNT, STOCK_LCNT = 0x65, 97, 104
PERIOD_NS = 2500
RTC = 0x70
VAL_S, VAL_M, VAL_H, VAL_D_L, VAL_D_H = 0x00, 0x01, 0x02, 0x03, 0x04
LOAD_S = 0x0C
CCR = 0x11
CCR_INTR_EN, CCR_ENABLE, CCR_LOAD = 0x1, 0x2, 0x4


def setup(m, base=I2C2, con=STOCK_CON, hcnt=STOCK_HCNT, lcnt=STOCK_LCNT):
    """Stock's initialisation order (DS 5.1.4.1): disable, then IC_CON and the SCL counts."""
    m.writel(base + ENABLE, 0)
    m.writel(base + CON, con)
    m.writel(base + FS_HCNT, hcnt)
    m.writel(base + FS_LCNT, lcnt)


def begin(m, addr, base=I2C2):
    """Stock's transfer preamble: disable, clear interrupts, target, enable."""
    m.writel(base + ENABLE, 0)
    m.readl(base + CLR_INTR)
    m.writel(base + TAR, addr)
    m.writel(base + ENABLE, 1)


def finish(m, base=I2C2):
    m.writel(base + START, 0)
    m.writel(base + ENABLE, 0)
    m.clock_step(10 * PERIOD_NS)


def i2c_write(m, addr, data, base=I2C2):
    """i2c2_master_write_reg's shape: queue the bytes, set IC_START, wait for idle."""
    begin(m, addr, base)
    for b in data:
        m.writel(base + DATA_CMD, b)
    m.writel(base + START, 1)
    m.clock_step(1_000_000)
    finish(m, base)


def i2c_read(m, addr, reg, n, base=I2C2):
    """i2c2_master_read_reg's shape: register index, n read commands, IC_START, pop n."""
    begin(m, addr, base)
    m.writel(base + DATA_CMD, reg)
    for _ in range(n):
        m.writel(base + DATA_CMD, READ)
    m.writel(base + START, 1)
    m.clock_step(1_000_000)
    data = [m.readl(base + DATA_CMD) for _ in range(n)]
    finish(m, base)
    return data


def rtc_write(m, reg, data):
    i2c_write(m, RTC, [reg] + list(data))


def rtc_read(m, reg, n):
    return i2c_read(m, RTC, reg, n)
