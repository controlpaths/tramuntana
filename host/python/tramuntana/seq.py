"""
The board's sequencer: build a list of steps on the host, run them on the
board with microsecond timing, get a timestamped record of every step.

    from tramuntana import Tramuntana, Sequence

    with Tramuntana() as t:
        s = (Sequence()
             .dio_dir(1 << 3, 1 << 3)          # DIO3 = target RESET
             .dio_write(1 << 3, 0).delay_us(100).dio_write(1 << 3, 1)
             .wait_dio(1 << 0, 0, timeout_us=500_000)   # until TX (DIO0) goes low
             .i2c_write_read(0x76, b"\\xd0", 1))
        r = t.seq.run(s)
        for step in r.steps:
            print(step)

Why: every host request is a USB round trip plus the host's scheduling,
so "release reset, then read the sensor 2 ms later" from a script lands
anywhere from 1 to 20 ms. On the board it lands within microseconds, and
each record carries the board time at which the step finished - the same
clock as the events (``DioEdge``, ``EmuTransfer``...), so a run lines up
with a ``Timeline``.

The wire format of each step is documented in fw/apps/tramuntana_fw/src/seq.h.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field
from typing import List, Optional

from . import proto as P
from .proto import Link, LinkError, STATUS_NAMES

MAX_CODE = 2048
CONTINUE = 0x80

OP_NAMES = {
    0x01: "delay_us", 0x02: "power", 0x03: "pullups", 0x04: "dio_dir", 0x05: "dio_write",
    0x06: "dio_read", 0x07: "wait_dio", 0x08: "i2c_write", 0x09: "i2c_read",
    0x0A: "i2c_write_read", 0x0B: "spi_txn", 0x0C: "uart_send", 0x0D: "awg_dc", 0x0E: "adc_read",
    0x0F: "mark", 0x10: "loop", 0x11: "end_loop", 0x12: "logic_arm", 0x13: "scope_arm",
    0x14: "watch", 0x15: "delay_ms",
}
STATES = ["idle", "running", "done", "failed", "aborted", "timeout", "invalid"]


class Sequence:
    """Builder for a sequencer program. Every method appends a step and
    returns the sequence, so calls chain. ``keep_going=True`` on a step
    records its failure and carries on instead of stopping the run.
    """

    def __init__(self):
        self.code = bytearray()
        self.labels = {}          # pc -> description, for readable results
        self._loop_open = False

    def _add(self, op: int, args: bytes, desc: str, keep_going: bool = False) -> "Sequence":
        pc = len(self.code)
        self.code += bytes([op | (CONTINUE if keep_going else 0)]) + args
        if len(self.code) > MAX_CODE:
            raise ValueError(f"program longer than {MAX_CODE} bytes")
        self.labels[pc] = desc
        return self

    # --- timing ---
    def delay_us(self, us: int) -> "Sequence":
        """Waits ``us`` microseconds (busy wait below 2 ms). The step lasts
        ~14 us longer than requested below 1 ms (~20 us up to 2 ms, ~37 us
        above); subtract it for a precise interval."""
        return self._add(0x01, struct.pack("<I", int(us)), f"delay {int(us)} us")

    def delay_ms(self, ms: float) -> "Sequence":
        """Waits ``ms`` milliseconds."""
        return self._add(0x01, struct.pack("<I", int(ms * 1000)), f"delay {ms:g} ms")

    def mark(self, mark_id: int) -> "Sequence":
        """Records a timestamp with an id, to measure between points."""
        return self._add(0x0F, bytes([mark_id & 0xFF]), f"mark {mark_id}")

    def loop(self, count: int) -> "Sequence":
        """Repeats the steps up to ``end_loop()`` ``count`` times (one level)."""
        if self._loop_open:
            raise ValueError("loops don't nest")
        self._loop_open = True
        return self._add(0x10, struct.pack("<H", max(1, int(count))), f"loop x{count}")

    def end_loop(self) -> "Sequence":
        self._loop_open = False
        return self._add(0x11, b"", "end loop")

    # --- board ---
    def power(self, on: bool) -> "Sequence":
        return self._add(0x02, bytes([int(on)]), f"power {'on' if on else 'off'}")

    def pullups(self, on: bool) -> "Sequence":
        return self._add(0x03, bytes([int(on)]), f"pullups {'on' if on else 'off'}")

    def dio_dir(self, mask: int, out_mask: int) -> "Sequence":
        return self._add(0x04, bytes([mask, out_mask]), f"dio_dir mask {mask:#04x} out {out_mask:#04x}")

    def dio_write(self, mask: int, value: int, keep_going: bool = False) -> "Sequence":
        return self._add(0x05, bytes([mask, value]), f"dio_write mask {mask:#04x} = {value:#04x}",
                         keep_going)

    def dio_read(self) -> "Sequence":
        return self._add(0x06, b"", "dio_read")

    def wait_dio(self, mask: int, value: int, timeout_us: int = 1_000_000,
                 keep_going: bool = False) -> "Sequence":
        """Waits until ``dio & mask == value & mask``; records how long it took."""
        return self._add(0x07, bytes([mask, value]) + struct.pack("<I", int(timeout_us)),
                         f"wait_dio mask {mask:#04x} == {value & mask:#04x}", keep_going)

    def watch(self, mask: int) -> "Sequence":
        """Same as ``Tramuntana.dio_watch()``, from inside the run."""
        return self._add(0x14, bytes([mask & 0xFF]), f"watch {mask:#04x}")

    def awg_dc(self, volts: float) -> "Sequence":
        return self._add(0x0D, struct.pack("<H", int(round(volts * 1000))), f"awg_dc {volts:g} V")

    def adc_read(self, channel: int = 0) -> "Sequence":
        return self._add(0x0E, bytes([channel]), f"adc_read AIN{channel}")

    # --- buses ---
    def i2c_write(self, address: int, data: bytes, keep_going: bool = False) -> "Sequence":
        data = bytes(data)
        return self._add(0x08, bytes([address, len(data)]) + data,
                         f"i2c_write {address:#04x} [{data.hex(' ')}]", keep_going)

    def i2c_read(self, address: int, n: int, keep_going: bool = False) -> "Sequence":
        return self._add(0x09, bytes([address, n]), f"i2c_read {address:#04x} x{n}", keep_going)

    def i2c_write_read(self, address: int, write: bytes, n: int, keep_going: bool = False) -> "Sequence":
        write = bytes(write)
        return self._add(0x0A, bytes([address, len(write)]) + write + bytes([n]),
                         f"i2c_write_read {address:#04x} [{write.hex(' ')}] x{n}", keep_going)

    def spi_txn(self, write: bytes, read_n: int = 0, keep_going: bool = False) -> "Sequence":
        write = bytes(write)
        return self._add(0x0B, bytes([len(write)]) + write + bytes([read_n]),
                         f"spi_txn [{write.hex(' ')}] x{read_n}", keep_going)

    def uart_send(self, data: bytes, keep_going: bool = False) -> "Sequence":
        """Sends bytes to the target's RX; the bridge must be active."""
        data = bytes(data)
        return self._add(0x0C, bytes([len(data)]) + data, f"uart_send {data!r}", keep_going)

    # --- captures ---
    def logic_arm(self, n: int = 4096, rate_hz: float = 10e6, trigger: str = "none", pin: int = 0,
                  mask: int = 0, value: int = 0, pre: int = 0, timeout_ms: int = 5000) -> "Sequence":
        """Arms the logic analyzer from inside the run (same arguments as
        ``Logic.arm()``); collect it afterwards with ``t.logic.read()``."""
        clkdiv = max(1, min(65535, round(150e6 / rate_hz)))
        trig = {"none": 0, "rising": 1, "falling": 2, "pattern": 3}[trigger]
        return self._add(0x12, struct.pack("<IHBBBBIH", n, clkdiv, trig, pin, mask, value, pre, timeout_ms),
                         f"logic_arm n={n} @ {150e6 / clkdiv:g} S/s {trigger}")

    def scope_arm(self, n: int = 1024, channels: int = 1, rate_hz: Optional[float] = None,
                  trigger: str = "none", level: float = 1.65, pre: int = 0,
                  timeout_ms: int = 5000) -> "Sequence":
        """Arms the scope from inside the run (same arguments as
        ``Scope.arm()``); collect it afterwards with ``t.scope.read()``."""
        clkdiv = 96 if rate_hz is None else max(96, min(65535, round(48e6 / (rate_hz * channels))))
        lvl = max(0, min(4095, round(level * 4095 / 3.3)))
        trig = {"none": 0, "rising": 1, "falling": 2}[trigger]
        return self._add(0x13, struct.pack("<BHHBHHH", channels, n, clkdiv, trig, lvl, pre, timeout_ms),
                         f"scope_arm n={n} {trigger}")


@dataclass
class SeqStep:
    """One executed step of a run.

    Attributes:
        pc: byte offset of the step in the program.
        op: step name.
        ok: whether it succeeded.
        status: "ok" or the error (e.g. "bus error / NAK", "timeout").
        t_us: board time when it finished.
        dt_us: microseconds since the first step finished.
        data: raw record bytes; ``value`` decodes them (levels, waited us,
            millivolts, bytes read...).
        label: what the step was, from the ``Sequence``.
    """
    pc: int
    op: str
    ok: bool
    status: str
    t_us: int
    dt_us: int
    data: bytes
    label: str = ""

    @property
    def value(self):
        d = self.data
        if self.op in ("dio_read", "mark"):
            return d[0] if d else None
        if self.op == "wait_dio" and len(d) >= 4:
            return struct.unpack("<I", d)[0]
        if self.op == "adc_read" and len(d) >= 2:
            return struct.unpack("<H", d)[0] / 1000
        if self.op in ("logic_arm", "scope_arm") and len(d) >= 4:
            return struct.unpack("<I", d)[0]
        if self.op == "i2c_write" and d:
            return d[0]
        return d if d else None

    def __str__(self):
        v = self.value
        if isinstance(v, (bytes, bytearray)):
            v = v.hex(" ")
        return (f"+{self.dt_us:8d} us  {self.label or self.op:<40} "
                f"{'ok' if self.ok else self.status}" + (f"  -> {v}" if v is not None else ""))


@dataclass
class SeqResult:
    """Outcome of ``Sequencer.run()``: the final state ("done", "failed",
    "aborted", "timeout", "invalid"), the error and where it stopped, and
    every executed step."""
    state: str
    error: Optional[str]
    pc: int
    steps: List[SeqStep] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.state == "done"


class Sequencer:
    """Runs ``Sequence`` programs on the board. Access it as ``t.seq``."""

    def __init__(self, link: Link):
        self._l = link

    def load(self, seq: Sequence):
        """Uploads the program (in chunks)."""
        code = bytes(seq.code)
        for off in range(0, len(code), 1024):
            self._l.request(P.CMD_SEQ_LOAD, struct.pack("<H", off) + code[off:off + 1024])

    def start(self, seq: Sequence, timeout_s: float = 10.0):
        """Uploads and starts the program without waiting (see ``status()`` /
        ``result()``)."""
        if not seq.code:
            raise ValueError("empty sequence")
        self.load(seq)
        self._l.request(P.CMD_SEQ_RUN, struct.pack("<HI", len(seq.code), int(timeout_s * 1000)))

    def status(self) -> dict:
        """Returns ``{"state", "error", "pc", "result_len"}``."""
        st, err, pc, rl = struct.unpack("<BBHH", self._l.request(P.CMD_SEQ_STATUS))
        return {"state": STATES[st] if st < len(STATES) else str(st),
                "error": STATUS_NAMES.get(err, err) if err else None, "pc": pc, "result_len": rl}

    def abort(self):
        self._l.request(P.CMD_SEQ_ABORT)

    def result(self, seq: Optional[Sequence] = None) -> SeqResult:
        """Downloads and decodes the records of the last run (``seq`` adds
        the step labels)."""
        st = self.status()
        raw = bytearray()
        while len(raw) < st["result_len"]:
            chunk = self._l.request(P.CMD_SEQ_RESULT, struct.pack("<HH", len(raw), 2048))
            if not chunk:
                break
            raw += chunk
        steps = []
        i = 0
        t0 = None
        while i + 9 <= len(raw):
            pc, op, status, t_us, n = struct.unpack_from("<HBBIB", raw, i)
            data = bytes(raw[i + 9:i + 9 + n])
            i += 9 + n
            if t0 is None:
                t0 = t_us
            name = OP_NAMES.get(op & ~CONTINUE, f"op{op:#04x}")
            steps.append(SeqStep(pc, name, status == 0, "ok" if status == 0 else
                                 STATUS_NAMES.get(status, str(status)), t_us,
                                 (t_us - t0) & 0xFFFFFFFF, data,
                                 seq.labels.get(pc, "") if seq else ""))
        return SeqResult(st["state"], st["error"], st["pc"], steps)

    def run(self, seq: Sequence, timeout_s: float = 10.0) -> SeqResult:
        """Uploads, runs and waits for the program; returns every step's
        record. Raises ``LinkError`` if it doesn't finish in ``timeout_s``
        plus a margin."""
        self.start(seq, timeout_s)
        deadline = time.monotonic() + timeout_s + 2
        while True:
            st = self.status()
            if st["state"] != "running":
                break
            if time.monotonic() > deadline:
                self.abort()
                raise LinkError("sequence did not finish")
            time.sleep(0.005)
        return self.result(seq)
