"""
One timeline for everything the board sees: the target's UART output, the
I2C/SPI sniffers, edges on watched DIOs, transfers with an emulated
device, the host's control lines, sequencer runs - and the host's own
actions and marks - all on one clock, so cause and effect line up:

     0.000  host   power on
     0.012  dio    DIO3^                         (reset released)
     0.150  uart   Booting v1.2...
     0.153  i2c    S 76W- P                      (sensor not answering yet)
     0.154  uart   sensor not found

    from tramuntana import Tramuntana, Timeline

    with Tramuntana() as t:
        tl = Timeline(t)
        tl.start(watch=1 << 3)          # DIO3 edges too
        t.i2c.sniff(True)
        tap = tl.open_uart(115200)      # the target's console
        t.power(True)
        time.sleep(2)
        print(tl.format())

Times are seconds from ``start()``. Events stamped by the board keep its
microsecond clock, mapped onto the host's with a periodic CMD_TIME sync:
DIO edges, emulator transfers, control lines and sequencer runs exactly,
sniffer messages exactly too (the sniffer's PIO stamps the START / CS
edge). UART lines and host entries carry the host time at which they
arrived or happened: a millisecond or so late, plus the UART's receive
FIFO, which hands bytes over in groups (up to ~1.5 ms at 115200 baud,
proportionally more at lower rates).
"""
from __future__ import annotations

import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable, Deque, Iterable, List, Optional

import serial

from . import proto as P
from .device import (DioEdge, EmuTransfer, I2cMessage, LineChange, SeqDone, SpiMessage,
                     Tramuntana, decode_event, find_ports)

SYNC_PERIOD_S = 5.0
UART_LINE_IDLE_S = 0.05
DEFAULT_KEEP = 20000


@dataclass
class Entry:
    """One line of the timeline.

    Attributes:
        t: seconds since ``Timeline.start()``.
        source: "uart" (target -> host), "uart_tx", "i2c", "spi", "dio",
            "emu", "line", "seq", "capture", "host" (actions) or "mark".
        text: human-readable description.
        board_us: the board's timestamp when the board stamped it.
        obj: the decoded event (``DioEdge``, ``I2cMessage``...) or None.
    """
    t: float
    source: str
    text: str
    board_us: Optional[int] = None
    obj: object = None

    def __str__(self):
        return f"{self.t:10.6f}  {self.source:<7} {self.text}"


class _Clock:
    """Maps the board's 32-bit microsecond clock onto host monotonic time."""

    def __init__(self):
        self.host_ref = None
        self.board_ref = None
        self.rtt = None

    def sync(self, t: Tramuntana):
        best = None
        for _ in range(3):
            a = time.monotonic()
            b = t.time_us()
            c = time.monotonic()
            if best is None or c - a < best[0]:
                best = (c - a, (a + c) / 2, b)
        self.rtt, self.host_ref, self.board_ref = best

    def to_host(self, board_us: int) -> Optional[float]:
        if self.host_ref is None:
            return None
        d = (board_us - self.board_ref) & 0xFFFFFFFF
        if d >= 0x80000000:          # before the reference
            d -= 0x100000000
        return self.host_ref + d / 1e6


class Timeline:
    """Collects events from a board (and its UART bridge, through
    ``open_uart()``/``feed_uart()``) into a time-ordered list.

    Args:
        t: the board.
        keep: entries kept (oldest dropped first).
        dio_names: optional names for DIO edges, e.g. ``{3: "RESET"}``.
    """

    def __init__(self, t: Tramuntana, keep: int = DEFAULT_KEEP, dio_names: Optional[dict] = None):
        self.t = t
        self.entries: Deque[Entry] = deque(maxlen=keep)
        self.dio_names = dict(dio_names or {})
        self.t0: Optional[float] = None
        self.running = False
        self._clock = _Clock()
        self._lock = threading.Lock()
        self._uart_line = bytearray()
        self._uart_line_t: Optional[float] = None
        self._uart_last: float = 0.0
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._tap: Optional["UartTap"] = None
        self._watch = 0

    # --- lifecycle ---
    def start(self, watch: int = 0, clear: bool = True):
        """Starts recording. ``watch``: DIO mask whose edges to record
        (``Tramuntana.dio_watch``)."""
        if clear:
            self.entries.clear()
        self._clock.sync(self.t)
        if self.t0 is None or clear:
            self.t0 = time.monotonic()
        self.t.event_stamps(True)
        if not self.running:
            self.t.link.add_listener(self._on_event)
            self.running = True
            self._stop.clear()
            self._thread = threading.Thread(target=self._housekeeping, daemon=True)
            self._thread.start()
        self.watch(watch)

    def watch(self, mask: int):
        """Changes which DIOs' edges are recorded."""
        self._watch = mask
        self.t.dio_watch(mask)

    def stop(self):
        """Stops recording (entries are kept) and the DIO watch."""
        if not self.running:
            return
        self.running = False
        self._stop.set()
        self.t.link.remove_listener(self._on_event)
        self._flush_uart_line()
        try:
            self.t.dio_watch(0)
            self.t.event_stamps(False)
        except Exception:
            pass

    # --- inputs ---
    def _now(self) -> float:
        return time.monotonic() - (self.t0 or time.monotonic())

    def _add(self, e: Entry):
        with self._lock:
            # board-stamped events can be a hair older than the latest entry:
            # keep the list ordered
            if self.entries and e.t < self.entries[-1].t:
                items = list(self.entries)
                i = len(items)
                while i > 0 and items[i - 1].t > e.t:
                    i -= 1
                items.insert(i, e)
                self.entries.clear()
                self.entries.extend(items)
            else:
                self.entries.append(e)

    def _board_t(self, board_us: int) -> float:
        h = self._clock.to_host(board_us)
        return (h - self.t0) if h is not None else self._now()

    def _on_event(self, cmd: int, payload: bytes):
        if not self.running:
            return
        ev = decode_event(cmd, payload)
        now = self._now()
        if isinstance(ev, DioEdge):
            parts = []
            for i in range(8):
                if (ev.changed >> i) & 1:
                    name = self.dio_names.get(i, f"DIO{i}")
                    parts.append(f"{name} {'^' if (ev.levels >> i) & 1 else 'v'}")
            # no changed pin: it toggled back before the levels were read (a glitch)
            text = (", ".join(parts) or "glitch") + f"  (levels {ev.levels:08b})" + \
                (f" {ev.lost} lost" if ev.lost else "")
            self._add(Entry(self._board_t(ev.t_us), "dio", text, ev.t_us, ev))
        elif isinstance(ev, EmuTransfer):
            self._add(Entry(self._board_t(ev.t_us), "emu", str(ev)[4:], ev.t_us, ev))
        elif isinstance(ev, LineChange):
            self._add(Entry(self._board_t(ev.t_us), "line", f"DTR={int(ev.dtr)} RTS={int(ev.rts)}",
                            ev.t_us, ev))
        elif isinstance(ev, SeqDone):
            self._add(Entry(self._board_t(ev.t_us), "seq", str(ev), ev.t_us, ev))
        elif isinstance(ev, I2cMessage):
            t = self._board_t(ev.t_us) if ev.t_us is not None else now
            self._add(Entry(t, "i2c", str(ev)[4:], ev.t_us, ev))
        elif isinstance(ev, SpiMessage):
            t = self._board_t(ev.t_us) if ev.t_us is not None else now
            self._add(Entry(t, "spi", str(ev)[4:], ev.t_us, ev))
        elif isinstance(ev, dict) and ev.get("event") == "capture_done":
            self._add(Entry(now, "capture", f"{ev['instrument']} {ev['state']}", None, ev))

    def feed_uart(self, data: bytes, at: Optional[float] = None):
        """Bytes received from the target's UART (whoever reads the bridge
        port calls this). Grouped into lines; a line is stamped with the
        arrival of its first byte."""
        if not self.running or not data:
            return
        now = self._now() if at is None else at
        with self._lock:
            for b in data:
                if self._uart_line_t is None:
                    self._uart_line_t = now
                if b == 0x0A:
                    self._emit_uart_line()
                elif b != 0x0D:
                    self._uart_line.append(b)
            self._uart_last = now

    def _emit_uart_line(self):
        # called with the lock held
        text = self._uart_line.decode("utf-8", "replace")
        t = self._uart_line_t if self._uart_line_t is not None else self._now()
        self._uart_line.clear()
        self._uart_line_t = None
        self.entries.append(Entry(t, "uart", text))

    def _flush_uart_line(self):
        with self._lock:
            if self._uart_line:
                self._emit_uart_line()
            self._uart_line_t = None

    def note(self, text: str, source: str = "host"):
        """Adds a host-side entry (an action taken, e.g. "power on")."""
        if self.running:
            self._add(Entry(self._now(), source, text))

    def mark(self, label: str):
        """Adds a marker, to find a moment later ("pressed the button")."""
        self.note(label, "mark")

    def _housekeeping(self):
        last_sync = time.monotonic()
        while not self._stop.wait(0.02):
            if self._uart_line and self._now() - self._uart_last > UART_LINE_IDLE_S:
                self._flush_uart_line()
            if time.monotonic() - last_sync > SYNC_PERIOD_S:
                try:
                    self._clock.sync(self.t)
                except Exception:
                    pass
                last_sync = time.monotonic()

    # --- UART ---
    def open_uart(self, baud: int = 115200, **kwargs) -> "UartTap":
        """Opens the board's bridge port with a ``UartTap`` that feeds this
        timeline; returns it (use it to read and send)."""
        self._tap = UartTap(self.t, baud, timeline=self, **kwargs)
        return self._tap

    # --- output ---
    def select(self, since: Optional[float] = None, until: Optional[float] = None,
               sources: Optional[Iterable[str]] = None, contains: Optional[str] = None,
               last: Optional[int] = None) -> List[Entry]:
        """Entries in a time window (seconds since start), from some sources,
        containing some text (case-insensitive); ``last`` keeps the last N."""
        srcs = set(sources) if sources else None
        needle = contains.lower() if contains else None
        with self._lock:
            items = list(self.entries)
        out = [e for e in items
               if (since is None or e.t >= since) and (until is None or e.t <= until)
               and (srcs is None or e.source in srcs)
               and (needle is None or needle in e.text.lower())]
        return out[-last:] if last else out

    def format(self, **kwargs) -> str:
        """``select()`` as text, one entry per line."""
        return "\n".join(str(e) for e in self.select(**kwargs))


class UartTap:
    """The target's UART through the board's bridge port, read continuously
    in the background: bytes are buffered for ``read()`` and, with a
    ``Timeline``, recorded on it as they arrive.

    Args:
        t: the board (its bridge port is found by serial number).
        baud, parity ("N"/"E"/"O"), stop_bits: framing on the target side.
        dtr, rts: initial control-line states (they matter with
            ``Uart.ctrl()``); None leaves the OS default (both asserted).
        timeline: record on this timeline (can be set later: ``tap.timeline``).
        port: the bridge port's path instead of finding it from ``t``.
    """

    def __init__(self, t: Optional[Tramuntana], baud: int = 115200, parity: str = "N", stop_bits: int = 1,
                 dtr: Optional[bool] = None, rts: Optional[bool] = None,
                 timeline: Optional[Timeline] = None, port: Optional[str] = None):
        if port is None:
            if t is None:
                raise ValueError("give the board or the port")
            boards = [b for b in find_ports() if b["uart"] and b["api"] == t.port] or \
                     [b for b in find_ports() if b["uart"]]
            if not boards:
                raise P.LinkError("no Tramuntana UART bridge port found")
            port = boards[0]["uart"]
        self.port = port
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN,
                           "O": serial.PARITY_ODD}[parity[0].upper()]
        self.ser.stopbits = stop_bits
        self.ser.timeout = 0.05
        if dtr is not None:
            self.ser.dtr = dtr
        if rts is not None:
            self.ser.rts = rts
        self.ser.open()
        self.ser.reset_input_buffer()
        self.timeline = timeline
        self._buf = bytearray()
        self._cv = threading.Condition()
        self._stop = False
        self.rx_total = 0
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self):
        while not self._stop:
            try:
                data = self.ser.read(max(1, self.ser.in_waiting))
            except Exception:
                break
            if not data:
                continue
            at = self.timeline._now() if self.timeline else None
            with self._cv:
                self._buf += data
                if len(self._buf) > 1 << 20:
                    del self._buf[:len(self._buf) - (1 << 20)]
                self.rx_total += len(data)
                self._cv.notify_all()
            if self.timeline:
                self.timeline.feed_uart(data, at)

    def read(self, timeout: float = 1.0, until: Optional[bytes] = None, max_bytes: int = 65536) -> bytes:
        """Returns buffered bytes, waiting up to ``timeout`` seconds - or less
        once ``until`` shows up (the bytes up to and including it are
        returned, the rest stay buffered) or ``max_bytes`` are there."""
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                if until:
                    i = self._buf.find(until)
                    if i >= 0:
                        n = i + len(until)
                        out = bytes(self._buf[:n])
                        del self._buf[:n]
                        return out
                if len(self._buf) >= max_bytes:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._cv.wait(remaining)
            out = bytes(self._buf[:max_bytes])
            del self._buf[:len(out)]
            return out

    def pending(self) -> int:
        with self._cv:
            return len(self._buf)

    def write(self, data: bytes):
        self.ser.write(data)
        self.ser.flush()
        if self.timeline:
            self.timeline.note(repr(bytes(data))[2:-1], "uart_tx")

    def set_lines(self, dtr: Optional[bool] = None, rts: Optional[bool] = None):
        """Sets DTR/RTS (with ``Uart.ctrl()`` these drive the target's
        reset/boot pins)."""
        if dtr is not None:
            self.ser.dtr = dtr
        if rts is not None:
            self.ser.rts = rts

    def close(self):
        self._stop = True
        try:
            self._thread.join(0.5)
        except Exception:
            pass
        try:
            self.ser.close()
        except Exception:
            pass
        if self.timeline:
            self.timeline._flush_uart_line()
