"""
The Tramuntana device object and its instruments.

    from tramuntana import Tramuntana

    with Tramuntana() as t:            # finds the board by USB id
        t.power(True)
        print(t.i2c.scan())
        cap = t.scope.capture(n=2048, trigger="rising", level=1.65)
        cap.to_csv("trace.csv")

Every instrument is a thin, well-typed wrapper over the commands in
proto.py; nothing here does more than pack arguments and unpack replies,
so what the CLI, the webapp and your scripts do is exactly the same.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Sequence, Tuple

import serial
from serial.tools import list_ports

from . import proto as P
from .proto import Link, LinkError, ProtoError

USB_VID = 0x2FE3
USB_PID = 0x0100
VREF_MV = 3300
ADC_CLOCK_HZ = 48_000_000
SYSCLK_HZ = 150_000_000


# --- port discovery ------------------------------------------------------------

def find_ports() -> List[dict]:
    """Lists the Tramuntana boards plugged in.

    Returns:
        One dict per board: ``{"serial": str, "shell": str | None,
        "api": str | None, "uart": str | None}`` - the USB serial number
        (the chip id) and the device path of each of its three ports.
        Empty if there is none.
    """
    boards = {}
    for p in list_ports.comports():
        if p.vid != USB_VID or p.pid != USB_PID:
            continue
        sn = p.serial_number or "?"
        entry = boards.setdefault(sn, {"serial": sn, "shell": None, "api": None, "uart": None})
        loc = p.location or ""
        # Linux: "1-2:1.0" / "1-2:1.2" / "1-2:1.4"; macOS/Windows differ,
        # so fall back to the interface string or the enumeration order.
        iface = None
        if ":" in loc and "." in loc.split(":")[-1]:
            try:
                iface = int(loc.split(":")[-1].split(".")[-1])
            except ValueError:
                iface = None
        role = {0: "shell", 2: "api", 4: "uart"}.get(iface)
        if role is None and p.interface:
            name = p.interface.lower()
            role = "api" if "api" in name else "uart" if "bridge" in name else "shell" if "shell" in name else None
        if role is None:
            for r in ("shell", "api", "uart"):
                if entry[r] is None:
                    role = r
                    break
        entry[role] = p.device
    return list(boards.values())


def find_api_port(serial_number: Optional[str] = None) -> str:
    """Device path of a board's binary API port.

    Args:
        serial_number: pick the board with this USB serial number; None
            takes the first one found.

    Raises:
        LinkError: no (matching) board is plugged in.
    """
    for b in find_ports():
        if serial_number and b["serial"] != serial_number:
            continue
        if b["api"]:
            return b["api"]
    raise LinkError("no Tramuntana board found (is it plugged in? is the new firmware flashed?)")


# --- result types ---------------------------------------------------------------

@dataclass
class ScopeCapture:
    """A finished scope capture, as returned by ``Scope.read()`` / ``capture()``.

    Attributes:
        channels: 1 (AIN0) or 2 (AIN0 and AIN1).
        rate_hz: sample rate of each channel, in samples/s.
        trigger_index: index of the trigger sample within each channel
            (0 without a trigger; equals ``pre`` with one).
        raw: ``raw[ch][i]``, 12-bit ADC codes (0..4095 = 0..3.3 V).
    """
    channels: int
    rate_hz: int            # samples/s per channel
    trigger_index: int      # sample index of the trigger within each channel
    raw: List[List[int]]    # raw[ch][i], 12-bit

    @property
    def n(self) -> int:
        """Samples per channel."""
        return len(self.raw[0]) if self.raw else 0

    def volts(self, ch: int = 0) -> List[float]:
        """One channel converted to volts (3.3 V reference).

        Args:
            ch: 0 = AIN0, 1 = AIN1.
        """
        return [v * VREF_MV / 4095 / 1000 for v in self.raw[ch]]

    def times(self) -> List[float]:
        """Time of each sample in seconds, relative to the trigger (negative = pre-trigger)."""
        dt = 1.0 / self.rate_hz
        return [(i - self.trigger_index) * dt for i in range(self.n)]

    def stats(self, ch: int = 0) -> dict:
        """Measurements of one channel, computed on the host.

        Args:
            ch: 0 = AIN0, 1 = AIN1.

        Returns:
            ``{"min", "max", "vpp", "mean", "rms"}`` in volts, and ``"freq"`` in
            Hz estimated from mean-level crossings (None with fewer than two).
        """
        v = self.volts(ch)
        n = len(v)
        mean = sum(v) / n
        rms = (sum(x * x for x in v) / n) ** 0.5
        # mean-crossing frequency estimate
        crossings = [i for i in range(1, n) if v[i - 1] < mean <= v[i]]
        freq = None
        if len(crossings) >= 2:
            freq = self.rate_hz * (len(crossings) - 1) / (crossings[-1] - crossings[0])
        return {"min": min(v), "max": max(v), "vpp": max(v) - min(v), "mean": mean, "rms": rms,
                "freq": freq}

    def to_csv(self, path: str):
        """Writes the capture as CSV: a ``t`` column (seconds from the trigger)
        and one ``chN`` column per channel in volts.
        """
        with open(path, "w") as f:
            f.write("t," + ",".join(f"ch{c}" for c in range(self.channels)) + "\n")
            t = self.times()
            for i in range(self.n):
                f.write(f"{t[i]:.9g}," + ",".join(f"{self.raw[c][i] * VREF_MV / 4095 / 1000:.4f}"
                                                  for c in range(self.channels)) + "\n")


@dataclass
class LogicCapture:
    """A finished logic analyzer capture, as returned by ``Logic.read()`` /
    ``capture()``.

    Attributes:
        rate_hz: sample rate in samples/s.
        trigger_index: index of the trigger sample (the pre-trigger length
            for a pattern trigger, 0 otherwise).
        samples: one byte per sample, bit i = level of DIOi.
    """
    rate_hz: int
    trigger_index: int
    samples: bytes          # one byte per sample, bit i = DIOi

    @property
    def n(self) -> int:
        """Number of samples."""
        return len(self.samples)

    def channel(self, dio: int) -> List[int]:
        """Levels (0/1) of one pin over the whole capture.

        Args:
            dio: 0..7.
        """
        return [(s >> dio) & 1 for s in self.samples]

    def edges(self, dio: int) -> List[Tuple[int, int]]:
        """Transitions of one pin.

        Args:
            dio: 0..7.

        Returns:
            ``[(sample_index, new_level), ...]``; divide an index by ``rate_hz``
            for seconds from the start of the capture.
        """
        out = []
        prev = None
        for i, s in enumerate(self.samples):
            b = (s >> dio) & 1
            if prev is not None and b != prev:
                out.append((i, b))
            prev = b
        return out

    def to_vcd(self, path: str, names: Optional[Sequence[str]] = None):
        """Writes a VCD file (PulseView, GTKWave...) with a 1 ps timescale and
        one 1-bit signal per DIO.

        Args:
            path: output file.
            names: 8 signal names, DIO0 first (default ``DIO0``..``DIO7``).
        """
        names = list(names or [f"DIO{i}" for i in range(8)])
        ps_per_sample = int(round(1e12 / self.rate_hz))
        with open(path, "w") as f:
            f.write("$timescale 1ps $end\n$scope module tramuntana $end\n")
            for i in range(8):
                f.write(f"$var wire 1 {chr(33 + i)} {names[i]} $end\n")
            f.write("$upscope $end\n$enddefinitions $end\n")
            prev = None
            for i, s in enumerate(self.samples):
                if prev is None or s != prev:
                    f.write(f"#{i * ps_per_sample}\n")
                    for b in range(8):
                        if prev is None or ((s ^ prev) >> b) & 1:
                            f.write(f"{(s >> b) & 1}{chr(33 + b)}\n")
                    prev = s
            f.write(f"#{self.n * ps_per_sample}\n")

    def to_csv(self, path: str):
        """Writes the capture as CSV: a ``t`` column (seconds from the trigger)
        and one ``DIOn`` column per pin, one row per sample.
        """
        with open(path, "w") as f:
            f.write("t," + ",".join(f"DIO{i}" for i in range(8)) + "\n")
            dt = 1.0 / self.rate_hz
            for i, s in enumerate(self.samples):
                f.write(f"{(i - self.trigger_index) * dt:.9g}," +
                        ",".join(str((s >> b) & 1) for b in range(8)) + "\n")


@dataclass
class SpiMessage:
    """One CS-low..CS-high transaction seen by the SPI sniffer.

    Attributes:
        total: bytes in the transaction.
        mosi, miso: the first bytes of each direction (up to 256); both
            are shorter than ``total`` for longer transactions.
        overrun: the sniffer lost data (bus too fast or too busy).
        t_us: board time of the first byte, when event stamps are on
            (``Tramuntana.event_stamps()``, a ``Timeline`` turns them on).
    """
    total: int
    mosi: bytes
    miso: bytes
    overrun: bool = False
    t_us: Optional[int] = None

    def __str__(self):
        more = " .." if self.total > len(self.mosi) else ""
        return (f"SPI {self.total}B  MOSI {self.mosi.hex(' ')}{more}  MISO {self.miso.hex(' ')}{more}"
                + (" OVERRUN" if self.overrun else ""))


@dataclass
class I2cFrame:
    """One START..(repeated START | STOP) segment of an I2C message.

    Attributes:
        address: 7-bit address.
        read: True for a read (R/W bit set).
        ack: whether the address was ACKed (None if the bus stopped first).
        data: ``[(byte, acked), ...]`` in bus order.
    """
    address: int
    read: bool
    ack: Optional[bool]
    data: List[Tuple[int, bool]] = field(default_factory=list)   # (byte, acked)


@dataclass
class I2cMessage:
    """One START..STOP message seen by the I2C sniffer, repeated STARTs
    included (one ``I2cFrame`` each). ``str()`` gives a compact line such
    as ``I2C S 68W+ 75+  S 68R+ 71- P`` (+ = ACK, - = NAK).

    Attributes:
        frames: the segments, in order.
        overrun: the sniffer lost data.
        truncated: the message was longer than one event and was cut.
        t_us: board time of the START, when event stamps are on.
    """
    frames: List[I2cFrame]
    overrun: bool = False
    truncated: bool = False
    t_us: Optional[int] = None

    def __str__(self):
        parts = []
        for fr in self.frames:
            s = f"S {fr.address:02X}{'R' if fr.read else 'W'}{'+' if fr.ack else '-'}"
            for b, a in fr.data:
                s += f" {b:02X}{'+' if a else '-'}"
            parts.append(s)
        return "I2C " + "  ".join(parts) + " P" + (" OVERRUN" if self.overrun else "") + \
               (" (truncated)" if self.truncated else "")


STAMPED = 0x80   # sniffer event flags: a board timestamp follows the flags byte


def decode_spi_event(payload: bytes) -> SpiMessage:
    """Decodes the payload of an ``EVT_SPI_MSG`` event into a ``SpiMessage``."""
    flags = payload[0]
    t_us = None
    if flags & STAMPED:
        t_us = struct.unpack_from("<I", payload, 1)[0]
        payload = payload[:1] + payload[5:]
    total = struct.unpack_from("<H", payload, 1)[0]
    k = (len(payload) - 3) // 2
    return SpiMessage(total, payload[3:3 + k], payload[3 + k:3 + 2 * k], bool(flags & 1), t_us)


def decode_i2c_event(payload: bytes) -> I2cMessage:
    """Decodes the payload of an ``EVT_I2C_MSG`` event into an ``I2cMessage``."""
    flags = payload[0]
    msg = I2cMessage([], bool(flags & 1), bool(flags & 2))
    if flags & STAMPED:
        msg.t_us = struct.unpack_from("<I", payload, 1)[0]
        payload = payload[:1] + payload[5:]
    cur = None
    pending_ack_for = None   # "addr" or "data"
    for i in range(1, len(payload) - 1, 2):
        t, v = payload[i], payload[i + 1]
        if t == 1:
            cur = I2cFrame(v >> 1, bool(v & 1), None)
            msg.frames.append(cur)
            pending_ack_for = "addr"
        elif t == 3 and cur is not None:
            cur.data.append((v, False))
            pending_ack_for = "data"
        elif t == 2 and cur is not None:
            if pending_ack_for == "addr":
                cur.ack = bool(v)
            elif pending_ack_for == "data" and cur.data:
                b, _ = cur.data[-1]
                cur.data[-1] = (b, bool(v))
            pending_ack_for = None
    return msg


@dataclass
class DioEdge:
    """An edge on a watched DIO (``Tramuntana.dio_watch()``).

    Attributes:
        t_us: board time in microseconds (wraps every ~71 minutes).
        levels: all eight DIO levels right after the edge (bit i = DIOi).
        changed: watched pins that changed (bit i = DIOi).
        lost: edges not reported before this one (queue full, or a pin
            toggling too fast and muted for a while).
    """
    t_us: int
    levels: int
    changed: int
    lost: int = 0

    def __str__(self):
        parts = [f"DIO{i}{'^' if (self.levels >> i) & 1 else 'v'}" for i in range(8) if (self.changed >> i) & 1]
        return " ".join(parts) + (f" ({self.lost} lost)" if self.lost else "")


@dataclass
class EmuTransfer:
    """One transfer the target made with an emulated device (``t.emu``).

    Attributes:
        t_us: board time at its start.
        bus: "i2c" or "spi".
        read: True if the target read (data = what it got), False if it
            wrote (data = what it sent).
        address: I2C address (0 for SPI).
        reg: first register.
        data: the bytes, at most 32 (``truncated`` if there were more).
        nak: an I2C address byte the emulator didn't answer (another
            device's address, or one without a START: ``no_start``); then
            ``address`` is the address it carried and ``reg`` the raw byte.
    """
    t_us: int
    bus: str
    read: bool
    address: int
    reg: int
    data: bytes
    truncated: bool = False
    nak: bool = False
    no_start: bool = False

    def __str__(self):
        if self.nak:
            return (f"EMU I2C NAK address {self.address:02X} (byte {self.reg:02X}"
                    + (", no START before it" if self.no_start else "") + ")")
        who = f"{self.bus.upper()} {self.address:02X}" if self.bus == "i2c" else "SPI"
        return (f"EMU {who} {'R' if self.read else 'W'} reg {self.reg:02X}: {self.data.hex(' ')}"
                + (" .." if self.truncated else ""))


@dataclass
class LineChange:
    """The host changed DTR/RTS on the bridge port (``EVT_LINE``)."""
    t_us: int
    dtr: bool
    rts: bool

    def __str__(self):
        return f"UART lines DTR={int(self.dtr)} RTS={int(self.rts)}"


SEQ_STATES = ["idle", "running", "done", "failed", "aborted", "timeout", "invalid"]


@dataclass
class SeqDone:
    """A sequencer program finished (``EVT_SEQ_DONE``)."""
    t_us: int
    state: str
    error: int
    pc: int
    result_len: int

    def __str__(self):
        return f"SEQ {self.state}" + (f" at pc {self.pc}: {P.STATUS_NAMES.get(self.error, self.error)}"
                                      if self.state != "done" else "")


def decode_event(cmd: int, payload: bytes):
    """Decodes any event frame into its dataclass (or dict), as
    ``Tramuntana.events()`` yields it."""
    if cmd == P.EVT_SPI_MSG:
        return decode_spi_event(payload)
    if cmd == P.EVT_I2C_MSG:
        return decode_i2c_event(payload)
    if cmd == P.EVT_CAPTURE_DONE:
        return {"event": "capture_done", "instrument": ["scope", "logic"][payload[0]],
                "state": P.CAP_NAMES[payload[1]]}
    if cmd == P.EVT_DIO:
        t, levels, changed, lost = struct.unpack_from("<IBBB", payload)
        return DioEdge(t, levels, changed, lost)
    if cmd == P.EVT_EMU:
        t, kind, flags, addr, reg = struct.unpack_from("<IBBBB", payload)
        return EmuTransfer(t, "i2c" if kind == 1 else "spi", bool(flags & 1), addr, reg,
                           bytes(payload[8:]), bool(flags & 4), bool(flags & 2), bool(flags & 8))
    if cmd == P.EVT_LINE:
        t, dtr, rts = struct.unpack_from("<IBB", payload)
        return LineChange(t, bool(dtr), bool(rts))
    if cmd == P.EVT_SEQ_DONE:
        t, st, err, pc, rl = struct.unpack_from("<IBBHH", payload)
        return SeqDone(t, SEQ_STATES[st] if st < len(SEQ_STATES) else str(st), err, pc, rl)
    if cmd == P.EVT_LOG:
        return {"event": "log", "text": payload.decode("utf-8", "replace")}
    return {"event": cmd, "payload": payload}


# --- instruments -------------------------------------------------------------------

class Spi:
    """SPI master, SPIDriver style: DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK.
    Access it as ``t.spi``.

    CS is driven by hand (``sel`` / ``unsel``), so a transaction can be
    built from several calls; ``txn`` does a whole one in a single
    request. Transfers are 8-bit words. The pins are taken over from plain
    GPIO use automatically; the bus idles with CS high.
    """

    def __init__(self, link: Link):
        self._l = link

    def config(self, freq_hz: int = 1_000_000, mode: int = 0, lsb_first: bool = False) -> int:
        """Sets clock, mode and bit order (kept until the next call).

        Args:
            freq_hz: SCK frequency, 1 kHz or more; anything above the maximum
                (75 MHz) is clamped. The PL022 divider may run slightly slower.
            mode: 0..3 (CPOL = bit 1, CPHA = bit 0).
            lsb_first: send the least significant bit first.

        Returns:
            The frequency in use after clamping, in Hz.

        Raises:
            ProtoError: invalid argument (mode > 3, frequency < 1 kHz).
        """
        r = self._l.request(P.CMD_SPI_CONF, struct.pack("<IBB", freq_hz, mode, int(lsb_first)))
        return struct.unpack("<I", r)[0]

    def sel(self):
        """Drives CS (DIO2) low."""
        self._l.request(P.CMD_SPI_SEL)

    def unsel(self):
        """Drives CS (DIO2) high."""
        self._l.request(P.CMD_SPI_UNSEL)

    def write(self, data: bytes):
        """Sends bytes on MOSI without touching CS; MISO is discarded.
        Any length: sent in 4096-byte requests.
        """
        for i in range(0, len(data), 4096):
            self._l.request(P.CMD_SPI_WRITE, bytes(data[i:i + 4096]))

    def read(self, n: int, fill: int = 0x00) -> bytes:
        """Reads n bytes from MISO without touching CS.

        Args:
            n: bytes to read, any length (4096-byte requests).
            fill: byte shifted out on MOSI while reading.
        """
        out = b""
        while len(out) < n:
            k = min(4096, n - len(out))
            out += self._l.request(P.CMD_SPI_READ, struct.pack("<HB", k, fill))
        return out

    def xfer(self, data: bytes) -> bytes:
        """Full-duplex transfer without touching CS.

        Returns:
            The bytes clocked in on MISO, one per byte of ``data``.
        """
        out = b""
        for i in range(0, len(data), 4096):
            out += self._l.request(P.CMD_SPI_XFER, bytes(data[i:i + 4096]))
        return out

    def txn(self, data: bytes, read_n: int = 0) -> bytes:
        """A whole transaction in one request: CS low, write ``data``, read
        ``read_n`` bytes (clocking 0x00), CS high.

        Args:
            data: up to 4096 bytes to send first (may be empty).
            read_n: 0..4096 bytes to read afterwards.

        Returns:
            The ``read_n`` bytes read (empty if 0).

        Raises:
            ProtoError: invalid argument (more than 4096 bytes either way).

        Example:
            ``t.spi.txn(b"\\x9f", read_n=3)`` reads a flash's JEDEC id.
        """
        return self._l.request(P.CMD_SPI_TXN, struct.pack("<H", read_n) + bytes(data))

    def sniff(self, on: bool = True, mode: Optional[int] = None):
        """Starts or stops the passive SPI sniffer on the same pins.

        It only listens (PIO, no pin claim), so it can watch a target's master
        or this board's own. Each CS-low..CS-high transaction arrives as a
        ``SpiMessage`` from ``Tramuntana.events()``. Works up to ~15 MHz SCK.

        Args:
            on: True to start, False to stop.
            mode: SPI mode of the watched bus; None uses the master's mode
                from ``config()``.

        Raises:
            ProtoError: busy (already running, or the capture memory is held
                by the logic analyzer / the other sniffer).
        """
        payload = bytes([int(on)]) + (bytes([mode]) if mode is not None else b"")
        self._l.request(P.CMD_SPI_SNIFF, payload)


class I2c:
    """I2C master, I2CDriver style: DIO6 SCL, DIO7 SDA, bit-banged,
    open-drain, with clock stretching (10 ms limit). Access it as ``t.i2c``.

    Addresses are 7-bit. High level: ``scan``, ``write``, ``read``,
    ``write_read``/``reg_read``/``reg_write``. Low level: ``start``,
    ``raw_write``, ``raw_read``, ``stop`` build any sequence a datasheet
    asks for. A NAK raises ``ProtoError`` with status 4 ("bus error / NAK");
    a stuck SCL gives status 3 (timeout).
    """

    def __init__(self, link: Link):
        self._l = link

    def config(self, hz: int = 100_000, pullups: bool = True) -> int:
        """Sets the bus speed and the board's pull-ups on SCL/SDA.

        Args:
            hz: SCL frequency, 1 kHz..1 MHz. The real speed is never higher,
                and lower if the pull-ups make the edges slow.
            pullups: switch the board's pull-up bias on (needed when the target
                has none of its own). Same switch as ``Tramuntana.pullups()``.

        Returns:
            The configured frequency in Hz.

        Raises:
            ProtoError: invalid argument (frequency out of range).
        """
        r = self._l.request(P.CMD_I2C_CONF, struct.pack("<IB", hz, int(pullups)))
        return struct.unpack("<I", r)[0]

    def scan(self) -> List[int]:
        """Probes addresses 0x08..0x77 (an address + W and a STOP each).

        Returns:
            The 7-bit addresses that ACKed, in increasing order.
        """
        bm = self._l.request(P.CMD_I2C_SCAN)
        return [a for a in range(128) if (bm[a // 8] >> (a % 8)) & 1]

    def write(self, addr: int, data: bytes) -> int:
        """START, address + W, ``data``, STOP.

        Args:
            addr: 7-bit address.
            data: up to 4096 bytes.

        Returns:
            Number of data bytes ACKed (all of them on success).

        Raises:
            ProtoError: status 4 on a NAK (address or data).
        """
        r = self._l.request(P.CMD_I2C_WRITE, bytes([addr]) + bytes(data))
        return struct.unpack("<H", r)[0]

    def read(self, addr: int, n: int) -> bytes:
        """START, address + R, read ``n`` bytes (NAK on the last), STOP.

        Args:
            addr: 7-bit address.
            n: 1..4096.
        """
        return self._l.request(P.CMD_I2C_READ, struct.pack("<BH", addr, n))

    def write_read(self, addr: int, data: bytes, n: int) -> bytes:
        """Write, repeated START, read: the usual register access.

        Args:
            addr: 7-bit address.
            data: bytes to write first (at most 255), usually the register.
            n: 1..4096 bytes to read.

        Returns:
            The bytes read.
        """
        return self._l.request(P.CMD_I2C_WRITE_READ,
                               struct.pack("<BB", addr, len(data)) + bytes(data) + struct.pack("<H", n))

    def reg_read(self, addr: int, reg: int, n: int = 1) -> bytes:
        """Reads ``n`` bytes starting at 8-bit register ``reg`` (``write_read(addr, [reg], n)``)."""
        return self.write_read(addr, bytes([reg]), n)

    def reg_write(self, addr: int, reg: int, data: bytes) -> int:
        """Writes ``data`` starting at 8-bit register ``reg``; returns the bytes ACKed."""
        return self.write(addr, bytes([reg]) + bytes(data))

    # low level
    def start(self, addr: int, read: bool = False) -> bool:
        """START - a repeated START if the bus is already open - and the address
        byte. Leaves the bus open for ``raw_write`` / ``raw_read`` / ``stop``.

        Args:
            addr: 7-bit address.
            read: set the R/W bit.

        Returns:
            True if the address was ACKed. A NAK does not raise here.
        """
        r = self._l.request(P.CMD_I2C_START, bytes([(addr << 1) | int(read)]))
        return bool(r[0])

    def stop(self):
        """STOP condition; closes the bus opened by ``start``."""
        self._l.request(P.CMD_I2C_STOP)

    def raw_write(self, data: bytes) -> int:
        """Writes bytes on an open bus (after ``start``), no START/STOP.

        Args:
            data: 1..4096 bytes.

        Returns:
            Number of bytes ACKed.

        Raises:
            ProtoError: status 4 on a NAK, status 6 if the bus is not open.
        """
        r = self._l.request(P.CMD_I2C_RAW_WRITE, bytes(data))
        return struct.unpack("<H", r)[0]

    def raw_read(self, n: int, nack_last: bool = True) -> bytes:
        """Reads bytes on an open bus (after ``start`` with ``read=True``).

        Args:
            n: bytes to read.
            nack_last: NAK the last byte (what a master does before STOP);
                False keeps ACKing to continue with another ``raw_read``.

        Raises:
            ProtoError: status 6 if the bus is not open.
        """
        return self._l.request(P.CMD_I2C_RAW_READ, struct.pack("<HB", n, int(nack_last)))

    def reset_bus(self):
        """Clocks SCL (9 pulses) and sends a STOP to free a slave stuck holding SDA low."""
        self._l.request(P.CMD_I2C_RESET)

    def sniff(self, on: bool = True, pullups: bool = False):
        """Starts or stops the passive I2C sniffer on DIO6/DIO7.

        It only listens, so it can watch a target's master or this board's
        own. Each START..STOP message arrives as an ``I2cMessage`` from
        ``Tramuntana.events()``.

        Args:
            on: True to start, False to stop.
            pullups: switch the board's pull-ups on while sniffing.

        Raises:
            ProtoError: busy (already running, or the capture memory is held
                by the logic analyzer / the other sniffer).
        """
        self._l.request(P.CMD_I2C_SNIFF, bytes([int(on), int(pullups)]))


class Scope:
    """Oscilloscope on AIN0 (J1.3) and AIN1 (J1.5): 12 bits, 0..3.3 V, up to
    500 kS/s in total (a 2-channel capture shares it: 250 kS/s each), up to
    16384 samples in total. Access it as ``t.scope``.

    The ADC free-runs into a ring buffer and the trigger is found in it, so
    captures can include pre-trigger samples. ``capture()`` does the whole
    thing; ``arm()`` / ``wait()`` / ``read()`` split it so other commands
    (an ``awg.step()``, a bus transaction...) can run while it is armed.
    """

    TRIGGERS = {"none": 0, "rising": 1, "falling": 2}

    def __init__(self, link: Link):
        self._l = link

    def arm(self, n: int = 1024, channels: int = 1, rate_hz: Optional[int] = None,
            trigger: str = "none", level: float = 1.65, pre: int = 0,
            timeout_ms: int = 5000) -> int:
        """Starts a capture and returns at once.

        Args:
            n: samples per channel; ``n * channels`` <= 16384.
            channels: 1 (AIN0) or 2 (AIN0 and AIN1, sampled alternately).
            rate_hz: sample rate per channel; None or anything above the
                maximum (500 kS/s / channels) gives the maximum. The ADC clock
                (48 MHz) is divided by an integer, so the rate is rounded.
            trigger: "none", "rising" or "falling", always on AIN0.
            level: trigger level in volts (0..3.3).
            pre: samples kept before the trigger (<= n).
            timeout_ms: give up if the trigger doesn't come (state "timeout").

        Returns:
            The actual sample rate per channel, in samples/s.

        Raises:
            ProtoError: invalid argument (sizes), or busy (a capture is
                already armed or running).
        """
        clkdiv = 96 if rate_hz is None else max(96, min(65535, round(ADC_CLOCK_HZ / (rate_hz * channels))))
        lvl = max(0, min(4095, round(level * 4095 / (VREF_MV / 1000))))
        r = self._l.request(P.CMD_SCOPE_ARM, struct.pack("<BHHBHHH", channels, n, clkdiv,
                                                          self.TRIGGERS[trigger], lvl, pre, timeout_ms))
        return struct.unpack("<I", r)[0] // channels

    def status(self) -> dict:
        """State of the current/last capture.

        Returns:
            ``{"state", "channels", "n", "rate_hz", "trigger_index"}``; state is
            one of "idle", "armed" (waiting for the trigger), "running", "done",
            "timeout", "aborted". ``rate_hz`` is the total for all channels.
        """
        r = self._l.request(P.CMD_SCOPE_STATUS)
        st, ch, n, rate, ti = struct.unpack("<BBHIH", r)
        return {"state": P.CAP_NAMES[st], "channels": ch, "n": n, "rate_hz": rate, "trigger_index": ti}

    def abort(self):
        """Stops an armed or running capture (state becomes "aborted")."""
        self._l.request(P.CMD_SCOPE_ABORT)

    def wait(self, timeout: float = 10.0) -> dict:
        """Polls ``status()`` until the capture leaves armed/running.

        Args:
            timeout: seconds to wait on the host side.

        Returns:
            The final status dict (check ``state`` for "done").

        Raises:
            LinkError: still armed/running after ``timeout``.
        """
        deadline = time.monotonic() + timeout
        while True:
            st = self.status()
            if st["state"] in ("done", "timeout", "aborted", "idle"):
                return st
            if time.monotonic() > deadline:
                raise LinkError("scope: capture did not finish")
            time.sleep(0.01)

    def read(self) -> ScopeCapture:
        """Downloads a finished capture (2048 samples per request).

        Raises:
            LinkError: the state is not "done".
        """
        st = self.status()
        if st["state"] != "done":
            raise LinkError(f"scope: no capture ({st['state']})")
        total = st["n"] * st["channels"]
        raw = []
        off = 0
        while off < total:
            k = min(2048, total - off)
            r = self._l.request(P.CMD_SCOPE_READ, struct.pack("<HH", off, k))
            raw += list(struct.unpack(f"<{len(r) // 2}H", r))
            off += len(r) // 2
        ch = st["channels"]
        per = [raw[c::ch] for c in range(ch)]
        return ScopeCapture(ch, st["rate_hz"] // ch, st["trigger_index"], per)

    def capture(self, **kwargs) -> ScopeCapture:
        """``arm(**kwargs)`` + ``wait()`` + ``read()``; takes the arguments of ``arm``.

        Raises:
            LinkError: the capture ended in "timeout" (no trigger) or "aborted".

        Example:
            ``t.scope.capture(n=4096, trigger="rising", level=1.65, pre=1024)``
        """
        self.arm(**kwargs)
        st = self.wait(timeout=kwargs.get("timeout_ms", 5000) / 1000 + 5)
        if st["state"] != "done":
            raise LinkError(f"scope: {st['state']}")
        return self.read()


class Awg:
    """Waveform generator on AOUT0 (J1.7): a PWM through an RC filter, 0..3.3 V,
    1 Hz..100 kHz. Access it as ``t.awg``.

    The output is ``offset + amp * shape(t)`` with shape(t) in -1..1,
    clipped to 0..3.3 V. It keeps running (DMA loop) while other commands
    are used, until ``off()`` or another shape is set.
    """

    SHAPES = {"sine": 0, "tri": 1, "saw": 2, "square": 3, "stairs": 4, "arb": 5}

    def __init__(self, link: Link):
        self._l = link

    def set(self, shape: str, freq_hz: float, amp: float = 1.0, offset: float = 1.65,
            param: int = 0) -> dict:
        """Starts a periodic waveform.

        Args:
            shape: "sine", "tri", "saw", "square", "stairs" or "arb" (the table
                last loaded by ``arbitrary()``).
            freq_hz: 1..100000, rounded to an integer.
            amp: peak amplitude in volts (half of peak-to-peak), resolution 1 mV.
            offset: centre voltage in volts.
            param: square: duty cycle in % (0 or > 100 means 50);
                stairs: number of steps (2..256, default 8).

        Returns:
            ``{"freq_hz": achieved frequency, "table": samples per period,
            "top": PWM counter top}``.

        Raises:
            ProtoError: invalid argument (frequency or voltages out of range,
                "arb" with no table loaded).
        """
        r = self._l.request(P.CMD_AWG_SET, struct.pack("<BIHHH", self.SHAPES[shape], int(round(freq_hz)),
                                                        int(amp * 1000), int(offset * 1000), param))
        f_mhz, n, top = struct.unpack("<IHH", r)
        return {"freq_hz": f_mhz / 1000, "table": n, "top": top}

    def sine(self, freq_hz, amp=1.0, offset=1.65):
        """Sine wave: ``set("sine", freq_hz, amp, offset)``."""
        return self.set("sine", freq_hz, amp, offset)

    def square(self, freq_hz, amp=1.0, offset=1.65, duty=50):
        """Square wave with ``duty`` % high: ``set("square", freq_hz, amp, offset, duty)``."""
        return self.set("square", freq_hz, amp, offset, duty)

    def dc(self, volts: float):
        """Constant output voltage.

        Args:
            volts: 0..3.3, resolution 1 mV.
        """
        self._l.request(P.CMD_AWG_DC, struct.pack("<H", int(volts * 1000)))

    def step(self, from_v: float, to_v: float, delay_ms: int):
        """Outputs ``from_v``, waits ``delay_ms``, then switches to ``to_v`` and
        stays there - for step-response tests.

        The board waits inside the command, so the call returns after the step
        and must fit in the link timeout (2 s unless ``Tramuntana(timeout=)``
        says otherwise). To catch the edge, ``scope.arm()`` with a trigger
        first, then ``step()``, then ``scope.wait()`` / ``read()``.

        Args:
            from_v, to_v: 0..3.3 V.
            delay_ms: 0..65535.
        """
        self._l.request(P.CMD_AWG_STEP, struct.pack("<HHH", int(from_v * 1000), int(to_v * 1000), delay_ms))

    def arbitrary(self, samples: Sequence[float], freq_hz: float, amp: float = 1.65, offset: float = 1.65):
        """Loads one period of an arbitrary waveform and plays it.

        Args:
            samples: up to 4096 values in -1..1 (quantized to 8 bits); the
                board resamples the table to its own number of points.
            freq_hz: periods per second, 1..100000.
            amp: peak amplitude in volts (a sample of 1 gives offset + amp).
            offset: centre voltage in volts.

        Returns:
            Same as ``set()``.
        """
        data = bytes(max(0, min(255, int(round((s + 1) * 127.5)))) for s in samples)
        for off in range(0, len(data), 4096):
            self._l.request(P.CMD_AWG_ARB_LOAD, struct.pack("<H", off) + data[off:off + 4096])
        return self.set("arb", freq_hz, amp, offset)

    def off(self):
        """Stops the output."""
        self._l.request(P.CMD_AWG_OFF)

    def status(self) -> dict:
        """Current state.

        Returns:
            ``{"running": bool, "shape": str, "freq_hz": float, "amp": V,
            "offset": V}``; shape is one of the ``set()`` shapes, "dc", "step"
            or "off".
        """
        r = self._l.request(P.CMD_AWG_STATUS)
        running, shape, f_mhz, amp, off = struct.unpack("<BBIHH", r)
        names = {v: k for k, v in self.SHAPES.items()}
        names.update({6: "dc", 7: "step", 8: "off"})
        return {"running": bool(running), "shape": names.get(shape, shape), "freq_hz": f_mhz / 1000,
                "amp": amp / 1000, "offset": off / 1000}


class Pwm:
    """Hardware PWM outputs on the DIO header. Access it as ``t.pwm``.

    The RP2350 has one PWM slice per pair of pins - DIO7/6, DIO5/4,
    DIO3/2, DIO1/0 - so the two pins of a pair always share a frequency
    and a phase; each has its own duty cycle and polarity. That gives up
    to four independent frequencies, from about 10 Hz to 75 MHz.

    Duty and polarity changes are glitch-free; a frequency change on a
    running pair may produce one odd period. ``phase()`` sets a pair's
    delay, applied by ``sync()``, which restarts the pairs on the same
    clock edge: pairs at the same or integer-ratio frequencies then keep
    their phase relationship indefinitely.

    Pins are claimed softly: another instrument can take one over. The
    slice keeps running, the pin no longer shows it, and ``status()``
    reports the output as ``lost``.
    """

    PAIRS = [(7, 6), (5, 4), (3, 2), (1, 0)]   # slice 0..3

    def __init__(self, link: Link):
        self._l = link

    @staticmethod
    def slice_of(dio: int) -> int:
        """PWM slice (0..3) that drives a DIO."""
        return (7 - dio) >> 1

    @staticmethod
    def partner(dio: int) -> int:
        """The other DIO of the same pair (same slice, same frequency)."""
        return dio ^ 1

    def set(self, dio: int, freq_hz: float, duty: float = 50.0, invert: bool = False) -> dict:
        """Starts (or updates) the PWM on one DIO.

        Args:
            dio: 0..7.
            freq_hz: frequency in Hz (rounded to an integer).
            duty: duty cycle in %, 0..100, 0.01 % steps (the real resolution is
                1/(top+1), coarser at high frequencies).
            invert: inverted polarity (low for ``duty`` % of the period).

        Returns:
            ``{"freq_hz": achieved frequency, "level": compare value,
            "top": counter top, "div": clock divider}``.

        Raises:
            ProtoError: busy if the pair partner is a live PWM at another
                frequency (stop it first) or the pin is held exclusively;
                invalid argument for a frequency out of range.
        """
        r = self._l.request(P.CMD_PWM_SET, struct.pack("<BIHB", dio, int(round(freq_hz)),
                                                        int(round(duty * 100)), int(invert)))
        f_mhz, level, top, div = struct.unpack("<IHHB", r)
        return {"freq_hz": f_mhz / 1000, "level": level, "top": top, "div": div}

    def phase(self, dio: int, degrees: int):
        """Phase delay (0..359 degrees) of the pair that ``dio`` belongs to;
        it takes effect on the next ``sync()``."""
        self._l.request(P.CMD_PWM_PHASE, struct.pack("<BH", dio, int(degrees) % 360))

    def sync(self, dio_mask: int = 0):
        """Restarts the pairs behind ``dio_mask`` (0 = every live pair) on the
        same clock edge, each delayed by its phase."""
        self._l.request(P.CMD_PWM_SYNC, bytes([dio_mask & 0xFF]))

    def stop(self, dio_mask: int = 0xFF):
        """Stops the PWM on the DIOs in ``dio_mask`` and gives the pins back
        as plain GPIO inputs."""
        self._l.request(P.CMD_PWM_STOP, bytes([dio_mask & 0xFF]))

    def status(self) -> List[dict]:
        """Returns:
            One dict per DIO, DIO0 first: ``{"active", "lost", "invert",
            "freq_hz" (requested), "actual_hz", "duty" (%), "phase"
            (degrees)}``. ``lost`` means another instrument took the pin.
        """
        r = self._l.request(P.CMD_PWM_STATUS)
        out = []
        for i in range(8):
            flags, f_req, f_mhz, duty, phase = struct.unpack_from("<BIIHH", r, i * 13)
            out.append({"active": bool(flags & 1), "lost": bool(flags & 2), "invert": bool(flags & 4),
                        "freq_hz": f_req, "actual_hz": f_mhz / 1000, "duty": duty / 100,
                        "phase": phase})
        return out


class Logic:
    """8-channel logic analyzer on DIO0..DIO7 (bit i of each sample = DIOi),
    up to 150 MS/s and 131072 samples. Access it as ``t.logic``.

    It only listens, so it can capture a target's buses or this board's
    own SPI/I2C/UART traffic. Triggers: none, a rising/falling edge on one
    pin (any rate, no pre-trigger), or the pins entering a pattern
    ``(sample & mask) == value`` (up to 15 MS/s and 122880 samples, with
    pre-trigger; a pattern already true when armed waits for its next start). ``arm()``
    / ``wait()`` / ``read()`` split ``capture()`` so other commands can
    generate the traffic while it is armed.
    """

    TRIGGERS = {"none": 0, "rising": 1, "falling": 2, "pattern": 3}

    def __init__(self, link: Link):
        self._l = link

    def arm(self, n: int = 4096, rate_hz: int = SYSCLK_HZ, trigger: str = "none", pin: int = 0,
            mask: int = 0, value: int = 0, pre: int = 0, timeout_ms: int = 5000) -> int:
        """Starts a capture and returns at once.

        Args:
            n: samples, 1..131072 (rounded up to a multiple of 4).
            rate_hz: sample rate; 150 MHz is divided by an integer, so the rate
                is rounded to 150e6 / round(150e6 / rate_hz).
            trigger: "none", "rising", "falling" or "pattern".
            pin: DIO for the edge triggers (0..7).
            mask, value: pattern trigger: pins compared and their levels
                (bit i = DIOi); mask must not be 0.
            pre: samples kept before the trigger (pattern, or an edge at up to 15 MS/s).
            timeout_ms: give up if the trigger doesn't come (state "timeout").

        Returns:
            The actual sample rate in samples/s.

        Raises:
            ValueError: arguments the pattern trigger can't do (checked on the
                host: rate above 15 MS/s, mask 0, more than 122880 samples).
            ProtoError: invalid argument, or busy (a capture or a sniffer is
                using the capture memory).
        """
        clkdiv = max(1, min(65535, round(SYSCLK_HZ / rate_hz)))
        if n < 1 or n > 131072:
            raise ValueError("n must be 1..131072 samples")
        if trigger in ("rising", "falling") and pre > 0 and clkdiv < 10:
            raise ValueError("an edge trigger with pre-trigger is limited to 15 MS/s (rate_hz <= 15e6); "
                             "use pre=0 for faster captures")
        if trigger == "pattern":
            if clkdiv < 10:
                raise ValueError("pattern trigger is limited to 15 MS/s (rate_hz <= 15e6); "
                                 "use an edge trigger for faster captures")
            if mask == 0:
                raise ValueError("pattern trigger needs a non-zero mask")
            if n > 131072 - 8192:
                raise ValueError("pattern trigger captures are limited to 122880 samples")
        r = self._l.request(P.CMD_LOGIC_ARM, struct.pack("<IHBBBBIH", n, clkdiv, self.TRIGGERS[trigger],
                                                          pin, mask, value, pre, timeout_ms))
        return struct.unpack("<I", r)[0]

    def status(self) -> dict:
        """State of the current/last capture.

        Returns:
            ``{"state", "n", "rate_hz", "trigger_index"}``; state is one of
            "idle", "armed", "running", "done", "timeout", "aborted".
        """
        r = self._l.request(P.CMD_LOGIC_STATUS)
        st, n, rate, ti = struct.unpack("<BIII", r)
        return {"state": P.CAP_NAMES[st], "n": n, "rate_hz": rate, "trigger_index": ti}

    def abort(self):
        """Stops an armed or running capture (state becomes "aborted")."""
        self._l.request(P.CMD_LOGIC_ABORT)

    def wait(self, timeout: float = 10.0) -> dict:
        """Polls ``status()`` until the capture leaves armed/running.

        Args:
            timeout: seconds to wait on the host side.

        Returns:
            The final status dict (check ``state`` for "done").

        Raises:
            LinkError: still armed/running after ``timeout``.
        """
        deadline = time.monotonic() + timeout
        while True:
            st = self.status()
            if st["state"] in ("done", "timeout", "aborted", "idle"):
                return st
            if time.monotonic() > deadline:
                raise LinkError("logic: capture did not finish")
            time.sleep(0.01)

    def read(self) -> LogicCapture:
        """Downloads a finished capture (4096 samples per request).

        Raises:
            LinkError: the state is not "done".
        """
        st = self.status()
        if st["state"] != "done":
            raise LinkError(f"logic: no capture ({st['state']})")
        out = bytearray()
        while len(out) < st["n"]:
            k = min(4096, st["n"] - len(out))
            out += self._l.request(P.CMD_LOGIC_READ, struct.pack("<IH", len(out), k))
        return LogicCapture(st["rate_hz"], st["trigger_index"], bytes(out))

    def capture(self, **kwargs) -> LogicCapture:
        """``arm(**kwargs)`` + ``wait()`` + ``read()``; takes the arguments of ``arm``.

        Raises:
            LinkError: the capture ended in "timeout" (no trigger) or "aborted".

        Example:
            ``t.logic.capture(n=100_000, rate_hz=10e6, trigger="pattern",
            mask=0x80, value=0x00, pre=1000)`` - an I2C START (SDA low).
        """
        self.arm(**kwargs)
        st = self.wait(timeout=kwargs.get("timeout_ms", 5000) / 1000 + 5)
        if st["state"] != "done":
            raise LinkError(f"logic: {st['state']}")
        return self.read()


class Uart:
    """Control of the USB-UART bridge: DIO1 TX (to the target's RX), DIO0 RX
    (from the target's TX). Access it as ``t.uart``.

    The data does not go through this object: open the board's third
    serial port (``find_ports()[0]["uart"]``) with pyserial or a terminal.
    Its baud rate, parity and stop bits are applied to the target side.
    While the bridge is active DIO0/DIO1 belong to it, and GPIO use of
    them fails with "busy".
    """

    MODES = {"auto": 0, "on": 1, "off": 2}

    def __init__(self, link: Link):
        self._l = link

    def config(self, mode: str = "auto", baud: int = 0):
        """Bridge mode.

        Args:
            mode: "auto" (default: active while a program holds the bridge
                port open), "on" (always) or "off" (never).
            baud: force this baud rate on the target side; 0 follows the rate
                the host program sets on the port.
        """
        self._l.request(P.CMD_UART_CONF, struct.pack("<BI", self.MODES[mode], baud))

    CTRL_MODES = {"off": 0, "direct": 1, "esp": 2}

    def status(self) -> dict:
        """Returns:
            ``{"active": bool, "baud": int, "rx_bytes": int, "tx_bytes": int}``
            (byte counters since boot, target side), plus with firmware 1.1
            ``"mode"``, ``"ctrl"`` (control-line mode), ``"reset_dio"``,
            ``"boot_dio"``, ``"dtr"``, ``"rts"``, ``"reset_low"`` and
            ``"boot_low"``.
        """
        r = self._l.request(P.CMD_UART_STATUS)
        active, baud, rx, tx = struct.unpack_from("<BIII", r)
        out = {"active": bool(active), "baud": baud, "rx_bytes": rx, "tx_bytes": tx}
        if len(r) >= 18:
            modes = {v: k for k, v in self.MODES.items()}
            ctrls = {v: k for k, v in self.CTRL_MODES.items()}
            pin = lambda d: None if d == 0xFF else d
            out.update({"mode": modes.get(r[13], r[13]), "ctrl": ctrls.get(r[14], r[14]),
                        "reset_dio": pin(r[15]), "boot_dio": pin(r[16]),
                        "dtr": bool(r[17] & 1), "rts": bool(r[17] & 2),
                        "reset_low": bool(r[17] & 4), "boot_low": bool(r[17] & 8)})
        return out

    def ctrl(self, mode: str = "esp", reset_dio: Optional[int] = None, boot_dio: Optional[int] = None):
        """Maps the bridge port's RTS/DTR to two DIOs (open-drain: asserted =
        pulled low), so tools that reset a target through a USB-serial
        adapter's control lines - esptool, for one - work through the
        bridge. The pins only pull low (open-drain).

        Args:
            mode: "esp" (the ESP32/ESP8266 two-transistor auto-reset logic:
                reset low only with RTS and not DTR, boot low only with DTR
                and not RTS), "direct" (reset = RTS, boot = DTR, like an
                adapter wired straight to the pins; opening the port asserts
                both) or "off".
            reset_dio, boot_dio: DIO2..DIO7, or None for no pin.

        While it is on, the bridge stays active (DIO0/DIO1 are the UART)
        whether or not the port is open.
        """
        pin = lambda d: 0xFF if d is None else d
        self._l.request(P.CMD_UART_CTRL, bytes([self.CTRL_MODES[mode], pin(reset_dio), pin(boot_dio)]))


class Emu:
    """The board as an I2C or SPI target: a 256-byte register file the
    target's master reads and writes, which this object loads and reads
    back. Access it as ``t.emu``. One emulator at a time; its pins are held
    exclusively (owner "emu") until ``stop()``.

    Register model: a pointer that auto-increments on every byte; reads
    return ``regs[ptr++]``, writes store into ``regs[ptr++]`` unless the
    register is read-only. Every transfer is reported as an
    ``EmuTransfer`` event. The register file survives ``stop()`` and
    switching bus.
    """

    def __init__(self, link: Link):
        self._l = link

    def i2c(self, address: int, scl: int = 6, sda: int = 7, pullups: bool = False):
        """Answers as an I2C device at a 7-bit address. The first byte the
        master writes sets the register pointer, the rest are data; a read
        starts at the pointer. The board stretches SCL briefly after each
        byte, so the master must allow clock stretching.

        Args:
            address: 7-bit address.
            scl, sda: DIOs (default the J1 I2C pins DIO6/DIO7).
            pullups: turn on the board's pull-ups (on DIO6/DIO7 only).
        """
        self._l.request(P.CMD_EMU_I2C, bytes([1, address, scl, sda, int(pullups)]))

    def spi(self, mode: int = 0, cs: int = 2, sck: int = 5, mosi: int = 4, miso: int = 3,
            addr_mask: int = 0x7F, read_mask: int = 0x80, read_value: int = 0x80, status: int = 0x00):
        """Answers as a SPI device. The first byte of each CS-low window is
        a command: a read when ``cmd & read_mask == read_value``, of the
        register ``cmd & addr_mask`` (MISO then returns regs[reg], regs[reg+1]
        ... up to 32 bytes); otherwise the following MOSI bytes are written
        from that register on. MISO carries ``status`` during the command
        byte. The defaults fit most sensors (bit 7 set = read).

        Args:
            mode: SPI mode 0..3.
            cs, sck, mosi, miso: DIOs, named from the target's side - the
                target's MOSI goes to ``mosi``. The defaults are the J1 SPI
                pins, so the wiring is the same as for the sniffer.
            addr_mask, read_mask, read_value, status: the command format.

        Limits: the first response bit has to be ready half a clock after
        the command, so keep SCK <= ~2 MHz (modes 0/2 tolerate more), and
        CS high for >= 10 us between transactions.
        """
        self._l.request(P.CMD_EMU_SPI, bytes([1, mode, cs, sck, mosi, miso, addr_mask, read_mask,
                                              read_value, status]))

    def stop(self):
        """Stops emulating and releases the pins."""
        self._l.request(P.CMD_EMU_I2C, bytes([0]))

    def set_regs(self, offset: int, data: bytes):
        """Loads ``data`` into the register file from ``offset`` (wraps at 256)."""
        data = bytes(data)
        if not 0 < len(data) <= 256:
            raise ValueError("1..256 bytes")
        self._l.request(P.CMD_EMU_REGS, bytes([0, offset & 0xFF]) + data)

    def regs(self, offset: int = 0, n: int = 256) -> bytes:
        """Reads ``n`` registers back from ``offset`` (e.g. what the target wrote)."""
        return self._l.request(P.CMD_EMU_REGS, struct.pack("<BBH", 1, offset & 0xFF, n))

    def read_only(self, registers: Sequence[int]):
        """Marks registers as read-only (writes to them are ACKed and ignored);
        an empty list makes them all writable again."""
        mask = bytearray(32)
        for r in registers:
            mask[r >> 3] |= 1 << (r & 7)
        self._l.request(P.CMD_EMU_REGS, bytes([2]) + bytes(mask))

    def status(self) -> dict:
        """Returns:
            ``{"bus": "off" | "i2c" | "spi", "transfers", "bytes_read",
            "bytes_written", "naks"}`` (counters since the emulator started;
            ``naks`` counts I2C transfers to other addresses).
        """
        r = self._l.request(P.CMD_EMU_STATUS)
        kind, txns, reads, writes, naks = struct.unpack_from("<BIIII", r)
        out = {"bus": ["off", "i2c", "spi"][kind], "transfers": txns, "bytes_read": reads,
               "bytes_written": writes, "naks": naks}
        if len(r) >= 29:
            starts, stops, nbytes = struct.unpack_from("<III", r, 17)
            out.update({"starts": starts, "stops": stops, "bytes": nbytes})
        return out


# --- the device --------------------------------------------------------------------

class Tramuntana:
    """A connected board: system commands plus the instruments as attributes
    ``spi``, ``i2c``, ``scope``, ``awg``, ``pwm``, ``logic``, ``uart``,
    ``emu`` and ``seq``.

    Use it as a context manager (``with Tramuntana() as t:``) or call
    ``close()``. One background thread reads the port; requests are
    serialized, so the object can be shared between threads.

    Args:
        port: API serial port (e.g. ``/dev/ttyACM1``); None finds it by
            USB id.
        serial_number: with several boards, pick this one (see
            ``find_ports()``).
        timeout: seconds to wait for each reply.

    Raises:
        LinkError: no board found, or the port is not a Tramuntana API port.
    """

    OWNERS = ["gpio", "spi", "i2c", "uart", "pio", "pwm", "emu", "?"]

    def __init__(self, port: Optional[str] = None, serial_number: Optional[str] = None,
                 timeout: float = 2.0):
        self.port = port or find_api_port(serial_number)
        self.ser = serial.Serial(self.port, 115200, timeout=0.05)
        self.link = Link(self.ser, timeout)
        self.spi = Spi(self.link)
        self.i2c = I2c(self.link)
        self.scope = Scope(self.link)
        self.awg = Awg(self.link)
        self.pwm = Pwm(self.link)
        self.logic = Logic(self.link)
        self.uart = Uart(self.link)
        self.emu = Emu(self.link)
        from .seq import Sequencer
        self.seq = Sequencer(self.link)
        r = self.link.request(P.CMD_PING)
        if r[:4] != P.MAGIC:
            raise LinkError(f"{self.port} is not a Tramuntana API port")
        self.proto_version = r[4]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        """Stops the reader thread and closes the port."""
        self.link.close()
        self.ser.close()

    # --- system ---
    def info(self) -> dict:
        """Returns:
            ``{"fw_version", "board", "sysclk_hz", "chip_id" (hex),
            "features" (bitmask), "proto_version"}``.
        """
        r = self.link.request(P.CMD_INFO)
        return {"fw_version": r[:16].split(b"\0")[0].decode(),
                "board": r[16:32].split(b"\0")[0].decode(),
                "sysclk_hz": struct.unpack_from("<I", r, 32)[0],
                "chip_id": r[36:44].hex(),
                "features": struct.unpack_from("<I", r, 44)[0],
                "proto_version": self.proto_version}

    def bootloader(self):
        """Reboots into the RP2350's UF2 bootloader (the ``RP2350`` drive appears; this port goes away)."""
        try:
            self.link.request(P.CMD_BOOTLOADER, timeout=1.0)
        except LinkError:
            pass

    def led(self, r: Optional[bool] = None, g: bool = False, b: bool = False):
        """Sets the RGB status LED; with no arguments it goes back to automatic
        (the firmware shows activity on it).

        Args:
            r, g, b: channels on/off; ``led(True, False, False)`` = red,
                ``led(None)`` = automatic.
        """
        if r is None:
            self.link.request(P.CMD_LED, bytes([0xFF, 0, 0]))
        else:
            self.link.request(P.CMD_LED, bytes([int(r), int(g), int(b)]))

    def power(self, on: Optional[bool] = None) -> bool:
        """Switches (or reads) the target's 3.3 V rail on J1.1.

        Args:
            on: True/False to switch; None only reads.

        Returns:
            The rail state after the call.
        """
        r = self.link.request(P.CMD_POWER, b"" if on is None else bytes([int(on)]))
        return bool(r[0])

    def pullups(self, on: Optional[bool] = None) -> bool:
        """Switches (or reads) the board's I2C pull-up bias on DIO6/DIO7.

        Args:
            on: True/False to switch; None only reads.

        Returns:
            The state after the call.
        """
        r = self.link.request(P.CMD_PULLUPS, b"" if on is None else bytes([int(on)]))
        return bool(r[0])

    def dio_dir(self, mask: int, out_mask: int):
        """Makes DIO pins plain GPIO and sets their direction.

        Args:
            mask: pins to configure (bit i = DIOi).
            out_mask: of those, 1 = output, 0 = input.

        Raises:
            ProtoError: busy if a pin is held exclusively (the UART bridge on
                DIO0/DIO1).
        """
        self.link.request(P.CMD_DIO_DIR, bytes([mask, out_mask]))

    def dio_write(self, mask: int, value: int):
        """Drives output pins.

        Args:
            mask: pins to set (bit i = DIOi); they must be outputs from
                ``dio_dir()``.
            value: their levels.

        Raises:
            ProtoError: not permitted if a pin in ``mask`` is not a plain GPIO
                (an instrument took it over).
        """
        self.link.request(P.CMD_DIO_WRITE, bytes([mask, value]))

    def dio_read(self) -> int:
        """Levels of DIO7..DIO0 as a byte (bit i = DIOi), whoever drives the pins."""
        return self.link.request(P.CMD_DIO_READ)[0]

    def dio_watch(self, mask: int):
        """Reports every edge on the DIOs in ``mask`` as a ``DioEdge`` event
        with the board's microsecond timestamp; 0 stops. It only listens,
        so it works on pins an instrument or the target drives. For control
        lines (reset, IRQ, LEDs): a pin toggling faster than ~50 edges/ms is
        muted for 100 ms - use the logic analyzer for buses.
        """
        self.link.request(P.CMD_DIO_WATCH, bytes([mask & 0xFF]))

    def time_us(self) -> int:
        """The board clock in microseconds (32 bits, wraps every ~71 min):
        the one every event timestamp and sequencer record uses."""
        return struct.unpack("<I", self.link.request(P.CMD_TIME))[0]

    def event_stamps(self, on: bool):
        """Makes the SPI/I2C sniffer events carry the board time too
        (``SpiMessage.t_us`` / ``I2cMessage.t_us``): taken when the sniffer
        decodes the first byte / the START, within about a millisecond."""
        self.link.request(P.CMD_TIME, bytes([int(on)]))

    def adc(self, channel: int = 0) -> float:
        """One reading of an analog input, in volts (0..3.3).

        Args:
            channel: 0 = AIN0 (J1.3), 1 = AIN1 (J1.5).

        Raises:
            ProtoError: busy while a scope capture uses the ADC.
        """
        r = self.link.request(P.CMD_ADC_READ, bytes([channel]))
        return struct.unpack("<H", r)[0] / 1000

    def display(self, text: str):
        """Shows a line of up to 24 ASCII characters on the dashboard's note row."""
        self.link.request(P.CMD_DISPLAY_TEXT, text.encode("ascii", "replace")[:24])

    def display_pinout(self, seconds: Optional[int] = 60):
        """Draws the J1 header on the board's screen - every pin's name and
        function - as a wiring aid, instead of the dashboard.

        Args:
            seconds: how long (0..65534); None keeps it until the next call;
                0 puts the dashboard back now.
        """
        s = 0xFFFF if seconds is None else max(0, min(0xFFFE, int(seconds)))
        self.link.request(P.CMD_DISPLAY_PINOUT, struct.pack("<H", s))

    def status(self) -> dict:
        """Snapshot of the whole board.

        Returns:
            ``{"pins": [owner of DIO0..DIO7], "power", "pullups", "scope",
            "logic", "awg", "spi_sniff", "i2c_sniff", "uart_bridge",
            "rx_dropped"}``. Owners are "gpio", "spi", "i2c", "uart", "pio", "pwm" or "emu",
            with "!" when held exclusively; scope/logic are capture states;
            ``rx_dropped`` counts API bytes the board had to drop.
        """
        r = self.link.request(P.CMD_STATUS)
        pins = struct.unpack_from("<I", r, 0)[0]
        owners = []
        for i in range(8):
            nib = (pins >> (4 * i)) & 0xF
            owners.append(self.OWNERS[nib & 7] + ("!" if nib & 8 else ""))
        return {"pins": owners, "power": bool(r[4]), "pullups": bool(r[5]),
                "scope": P.CAP_NAMES[r[6]], "logic": P.CAP_NAMES[r[7]], "awg": bool(r[8]),
                "spi_sniff": bool(r[9]), "i2c_sniff": bool(r[10]), "uart_bridge": bool(r[11]),
                "rx_dropped": struct.unpack_from("<I", r, 12)[0] if len(r) >= 16 else 0}

    # --- events ---
    def on_event(self, callback: Optional[Callable[[int, bytes], None]]):
        """Sets a callback ``fn(cmd, payload)`` for every event as it arrives
        (raw, undecoded), or None to remove it. It runs in the reader thread:
        keep it short and don't send requests from it.
        """
        self.link.on_event = callback

    def events(self, timeout: float = 0.0):
        """Yields the queued events, decoded, waiting up to ``timeout`` seconds
        for each next one; returns once ``timeout`` passes without any.

        Yields:
            ``SpiMessage``, ``I2cMessage``, ``DioEdge``, ``EmuTransfer``,
            ``LineChange``, ``SeqDone``, ``{"event": "capture_done",
            "instrument": "scope" | "logic", "state": str}``, or
            ``{"event": cmd, "payload": bytes}`` for anything else.

        The queue keeps the last 1000 events; with ``timeout=0`` this just
        drains it.
        """
        deadline = time.monotonic() + timeout
        while True:
            if self.link.events:
                cmd, payload = self.link.events.popleft()
                yield decode_event(cmd, payload)
                deadline = time.monotonic() + timeout
                continue
            if time.monotonic() >= deadline:
                return
            time.sleep(0.005)
