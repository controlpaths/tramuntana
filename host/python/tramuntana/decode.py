"""
Offline protocol decoders and waveform measurements, run on the host over
captures already downloaded from the board.

    from tramuntana import Tramuntana, decode

    with Tramuntana() as t:
        cap = t.logic.capture(n=65536, rate_hz=10e6, trigger="falling", pin=0)
        for fr in decode.uart(cap, pin=0, baud=decode.guess_baud(cap, 0)):
            print(fr)

Unlike the firmware sniffers, these work on any DIO, on any capture, and
can be re-run with other settings (baud rate, SPI mode...) without
capturing again. They only see what the capture holds: sample at least 4x
the bit rate (UART), the SCL frequency (I2C) or the SCK frequency (SPI).
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple

from .device import I2cFrame, I2cMessage, LogicCapture, ScopeCapture

STANDARD_BAUDS = (300, 1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 74880,
                  115200, 230400, 250000, 460800, 500000, 921600, 1000000, 1500000,
                  2000000, 3000000, 4000000)


def _t(cap: LogicCapture, i: float) -> float:
    """Seconds from the trigger of sample i."""
    return (i - cap.trigger_index) / cap.rate_hz


# --- UART ----------------------------------------------------------------------------

@dataclass
class UartFrame:
    """One character decoded by ``uart()``.

    Attributes:
        t_s: time of the start bit's falling edge, seconds from the trigger.
        value: the data bits (LSB first on the wire).
        parity_error: the parity bit did not match (False without parity).
        framing_error: the stop bit was low (wrong baud rate, or a break
            when ``value`` is 0).
    """
    t_s: float
    value: int
    parity_error: bool = False
    framing_error: bool = False

    def __str__(self):
        ch = chr(self.value)
        txt = repr(ch)[1:-1] if 32 <= self.value < 127 or ch in "\r\n\t" else ""
        flags = (" PARITY" if self.parity_error else "") + (" FRAMING" if self.framing_error else "")
        return f"{self.t_s * 1e6:.1f}us {self.value:02X} {txt}{flags}"


def _runs(bits: List[int]) -> List[int]:
    """Lengths of the complete constant-level runs (the first and last are cut
    by the capture window, so they are left out)."""
    out, start = [], None
    for i in range(1, len(bits)):
        if bits[i] != bits[i - 1]:
            if start is not None:
                out.append(i - start)
            start = i
    return out


def guess_baud(cap: LogicCapture, pin: int) -> Optional[int]:
    """Estimates the baud rate of a UART line from its shortest pulses.

    Args:
        cap: the capture.
        pin: DIO carrying the UART signal.

    Returns:
        The nearest standard rate when the estimate is within 6 % of one,
        the raw estimate otherwise, or None with too few edges.
    """
    runs = _runs(cap.channel(pin))
    if len(runs) < 4:
        return None
    shortest = min(runs)
    # Average the runs that are one bit long, to beat the +-1 sample jitter.
    singles = [r for r in runs if r < shortest * 1.5]
    bit = sum(singles) / len(singles)
    est = cap.rate_hz / bit
    best = min(STANDARD_BAUDS, key=lambda b: abs(b - est))
    return best if abs(best - est) / best < 0.06 else int(round(est))


def uart(cap: LogicCapture, pin: int, baud: int, bits: int = 8, parity: str = "none",
         stop_bits: int = 1, invert: bool = False) -> List[UartFrame]:
    """Decodes an asynchronous serial line.

    Args:
        cap: the capture.
        pin: DIO carrying the signal (DIO0 = target TX on J1).
        baud: bit rate (see ``guess_baud``).
        bits: data bits, 5..9.
        parity: "none", "even" or "odd".
        stop_bits: 1 or 2.
        invert: the line idles low (RS-232 levels after an inverter).

    Returns:
        The characters, in order.
    """
    spb = cap.rate_hz / baud
    if spb < 3:
        raise ValueError(f"{cap.rate_hz} S/s is too slow for {baud} baud (need >= {3 * baud} S/s)")
    line = cap.channel(pin)
    if invert:
        line = [b ^ 1 for b in line]
    n = len(line)
    npar = 0 if parity == "none" else 1
    frame_bits = 1 + bits + npar + stop_bits
    out = []
    i = 1
    # A capture triggered on the start bit's edge (no pre-trigger) begins
    # low: take sample 0 as a start bit if that frame ends in a valid stop.
    first = line[0] == 0 if n else False
    while i < n:
        if first:
            first = False
            stop_at = int((frame_bits - 1) * spb + spb / 2)
            if stop_at < n and line[stop_at] == 1:
                i = 0
            else:
                continue
        # falling edge after idle = start bit
        elif not (line[i - 1] == 1 and line[i] == 0):
            i += 1
            continue
        s = i

        def at(k: float) -> Optional[int]:
            j = int(s + k * spb + spb / 2)
            return line[j] if j < n else None

        if at(0) != 0:          # glitch, not a start bit
            i += 1
            continue
        if at(frame_bits - 1) is None:
            break               # frame cut by the end of the capture
        value = 0
        for k in range(bits):
            value |= at(1 + k) << k
        perr = False
        if npar:
            ones = bin(value).count("1") + at(1 + bits)
            perr = (ones % 2 == 1) if parity == "even" else (ones % 2 == 0)
        ferr = any(at(1 + bits + npar + k) == 0 for k in range(stop_bits))
        out.append(UartFrame(_t(cap, s), value, perr, ferr))
        # resume at the middle of the last stop bit, waiting for the next falling edge
        i = int(s + (frame_bits - 0.5) * spb)
        if ferr:
            while i < n and line[i] == 0:   # skip a break or a mis-framed tail
                i += 1
    return out


def uart_text(frames: List[UartFrame]) -> str:
    """The decoded characters as text (non-printable ones as \\xNN)."""
    out = []
    for f in frames:
        c = chr(f.value)
        out.append(c if 32 <= f.value < 127 or c in "\r\n\t" else f"\\x{f.value:02x}")
    return "".join(out)


# --- I2C -----------------------------------------------------------------------------

@dataclass
class I2cTransfer:
    """One START..STOP message decoded by ``i2c()``.

    Attributes:
        t_s: time of the START, seconds from the trigger.
        message: frames as the I2C sniffer reports them (``str()`` gives
            ``I2C S 68W+ 75+  S 68R+ 71- P``).
        stopped: a STOP ended it (False when the capture ended first).
        partial_bits: bits of an incomplete byte left when it ended.
        scl_hz: SCL frequency measured inside the message.
    """
    t_s: float
    message: I2cMessage
    stopped: bool = True
    partial_bits: int = 0
    scl_hz: Optional[float] = None

    def __str__(self):
        s = f"{self.t_s * 1e6:.1f}us {str(self.message)[4:]}"
        if not self.stopped:
            s = s[:-2] + " (no STOP)"
        if self.partial_bits:
            s += f" (+{self.partial_bits} bits)"
        return s


def i2c(cap: LogicCapture, scl: int = 6, sda: int = 7) -> List[I2cTransfer]:
    """Decodes I2C from SCL/SDA (DIO6/DIO7 on J1).

    Args:
        cap: the capture.
        scl, sda: the DIOs carrying the bus.

    Returns:
        The messages, in order. Bytes a STOP or START cut short are counted
        in ``partial_bits``.
    """
    out: List[I2cTransfer] = []
    cur: Optional[I2cTransfer] = None
    frame: Optional[I2cFrame] = None
    shift = nbits = 0
    rises: List[int] = []
    prev_c = prev_d = None

    def close(stopped: bool, i: int):
        nonlocal cur
        if cur is None:
            return
        cur.stopped = stopped
        # a STOP is always set up by one SCL rise that carries no data bit
        cur.partial_bits = max(0, nbits - 1) if stopped else nbits
        if len(rises) >= 2:
            cur.scl_hz = round(cap.rate_hz * (len(rises) - 1) / (rises[-1] - rises[0]), 1)
        out.append(cur)
        cur = None

    for i, s in enumerate(cap.samples):
        c, d = (s >> scl) & 1, (s >> sda) & 1
        if prev_c is not None:
            if c and prev_c and d != prev_d:
                if d == 0:                      # START or repeated START
                    if cur is None:
                        cur = I2cTransfer(_t(cap, i), I2cMessage([]))
                        rises = []
                    frame = None
                    shift = nbits = 0
                    cur.message.frames.append(None)   # placeholder until the address byte
                else:                           # STOP
                    close(True, i)
                    frame = None
                    shift = nbits = 0
            elif c and not prev_c and cur is not None:   # SCL rising: sample SDA
                rises.append(i)
                if nbits < 8:
                    shift = (shift << 1) | d
                    nbits += 1
                else:                           # 9th bit: ACK (low) / NAK (high)
                    ack = d == 0
                    if frame is None:
                        frame = I2cFrame(shift >> 1, bool(shift & 1), ack)
                        cur.message.frames[-1] = frame
                    else:
                        frame.data.append((shift, ack))
                    shift = nbits = 0
        prev_c, prev_d = c, d
    close(False, cap.n)
    for tr in out:   # drop START placeholders never followed by a full address byte
        tr.message.frames = [f for f in tr.message.frames if f is not None]
    return out


# --- SPI -----------------------------------------------------------------------------

@dataclass
class SpiTransfer:
    """One CS-low..CS-high transaction decoded by ``spi()``.

    Attributes:
        t_s: time CS went low (or of the first SCK edge without CS).
        mosi, miso: the words shifted each way.
        partial_bits: bits of an incomplete word left when CS rose.
        sck_hz: SCK frequency measured inside the transaction.
        complete: CS rose inside the capture (False if it was cut).
    """
    t_s: float
    mosi: List[int] = field(default_factory=list)
    miso: List[int] = field(default_factory=list)
    partial_bits: int = 0
    sck_hz: Optional[float] = None
    complete: bool = True

    def __str__(self):
        w = lambda xs: " ".join(f"{x:02X}" for x in xs)
        s = f"{self.t_s * 1e6:.1f}us MOSI {w(self.mosi)}  MISO {w(self.miso)}"
        if self.partial_bits:
            s += f" (+{self.partial_bits} bits)"
        return s + ("" if self.complete else " (cut)")


def spi(cap: LogicCapture, cs: Optional[int] = 2, sck: int = 5, mosi: Optional[int] = 4,
        miso: Optional[int] = 3, mode: int = 0, lsb_first: bool = False,
        word_bits: int = 8) -> List[SpiTransfer]:
    """Decodes SPI (J1 defaults: DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK).

    Args:
        cap: the capture.
        cs: chip-select DIO (active low), or None for the whole capture as
            one transaction.
        sck, mosi, miso: the other DIOs; mosi/miso may be None.
        mode: SPI mode 0..3 (CPOL = bit 1, CPHA = bit 0).
        lsb_first: bit order.
        word_bits: bits per word.

    Returns:
        The transactions, in order.
    """
    cpol, cpha = mode >> 1, mode & 1
    # leading edge is rising when CPOL=0; CPHA=0 samples on it, CPHA=1 on the trailing one
    sample_on_rise = (cpol == 0) == (cpha == 0)
    out: List[SpiTransfer] = []
    cur: Optional[SpiTransfer] = None
    mo = mi = nbits = 0
    edges: List[int] = []
    prev_k = prev_cs = None

    def close(complete: bool):
        nonlocal cur
        if cur is None:
            return
        cur.partial_bits = nbits
        cur.complete = complete
        if len(edges) >= 2:
            cur.sck_hz = round(cap.rate_hz * (len(edges) - 1) / (edges[-1] - edges[0]), 1)
        out.append(cur)
        cur = None

    for i, s in enumerate(cap.samples):
        k = (s >> sck) & 1
        sel = ((s >> cs) & 1) == 0 if cs is not None else True
        if cs is not None and prev_cs is not None and sel != prev_cs:
            if sel:
                cur = SpiTransfer(_t(cap, i))
                mo = mi = nbits = 0
                edges = []
            else:
                close(True)
        if sel and prev_k is not None and k != prev_k and (k == 1) == sample_on_rise:
            if cur is None:          # no CS (or CS already low when the capture started)
                cur = SpiTransfer(_t(cap, i))
                mo = mi = nbits = 0
                edges = []
            edges.append(i)
            bo = (s >> mosi) & 1 if mosi is not None else 0
            bi = (s >> miso) & 1 if miso is not None else 0
            if lsb_first:
                mo |= bo << nbits
                mi |= bi << nbits
            else:
                mo = (mo << 1) | bo
                mi = (mi << 1) | bi
            nbits += 1
            if nbits == word_bits:
                if mosi is not None:
                    cur.mosi.append(mo)
                if miso is not None:
                    cur.miso.append(mi)
                mo = mi = nbits = 0
        prev_k, prev_cs = k, sel
    close(cs is None)
    return out


# --- scope measurements --------------------------------------------------------------

def _cross(v: List[float], i: int, level: float) -> float:
    """Fractional index where v crosses level between samples i-1 and i."""
    a, b = v[i - 1], v[i]
    return i - 1 + ((level - a) / (b - a) if b != a else 0.0)


def _median(xs: List[float]) -> Optional[float]:
    if not xs:
        return None
    xs = sorted(xs)
    m = len(xs) // 2
    return xs[m] if len(xs) % 2 else (xs[m - 1] + xs[m]) / 2


def measure(cap: ScopeCapture, ch: int = 0, min_amplitude: float = 0.05) -> dict:
    """Oscilloscope-style measurements of one channel.

    Levels come from the two halves of the histogram (so overshoot and
    ringing don't move them); edges are found with 10/50/90 % thresholds
    and hysteresis.

    Args:
        cap: the capture.
        ch: 0 = AIN0, 1 = AIN1.
        min_amplitude: below this (V) the signal is treated as DC.

    Returns:
        Volts / seconds / Hz / %: ``low``, ``high``, ``amplitude``,
        ``overshoot_pct``, ``undershoot_pct``, ``rising_edges``,
        ``falling_edges``, ``rise_time_s``, ``fall_time_s`` (10-90 %,
        median), ``freq_hz``, ``period_s``, ``duty_pct``, ``first_rise_s``
        and ``first_fall_s`` (50 % crossing from the trigger), and
        ``sample_period_s``. Edges as fast as one sample are reported
        with ``edge_at_resolution_limit``.
    """
    v = cap.volts(ch)
    dt = 1.0 / cap.rate_hz
    vmin, vmax = min(v), max(v)
    mid = (vmin + vmax) / 2
    low = _median([x for x in v if x <= mid])
    high = _median([x for x in v if x > mid]) or vmax
    amp = high - low
    out = {"low": low, "high": high, "amplitude": amp, "sample_period_s": dt}
    if amp < min_amplitude:
        out.update({"dc": True, "mean": sum(v) / len(v)})
        return out
    out["overshoot_pct"] = max(0.0, (vmax - high) / amp * 100)
    out["undershoot_pct"] = max(0.0, (low - vmin) / amp * 100)
    t10, t50, t90 = low + 0.1 * amp, low + 0.5 * amp, low + 0.9 * amp

    rises, falls, r50, f50 = [], [], [], []
    state = None         # "low" / "high" once a level has been seen
    last_low = last_high = None
    for i, x in enumerate(v):
        if x <= t10:
            if state == "high" and last_high is not None:
                # falling edge: last sample >= 90 % .. first <= 10 %
                a = _cross(v, last_high + 1, t90)
                b = _cross(v, i, t10)
                falls.append((b - a) * dt)
                j = next(k for k in range(last_high + 1, i + 1) if v[k] <= t50)
                f50.append(_cross(v, j, t50))
            state, last_low = "low", i
        elif x >= t90:
            if state == "low" and last_low is not None:
                a = _cross(v, last_low + 1, t10)
                b = _cross(v, i, t90)
                rises.append((b - a) * dt)
                j = next(k for k in range(last_low + 1, i + 1) if v[k] >= t50)
                r50.append(_cross(v, j, t50))
            state, last_high = "high", i
    out["rising_edges"], out["falling_edges"] = len(rises), len(falls)
    if rises:
        out["rise_time_s"] = _median(rises)
        out["first_rise_s"] = (r50[0] - cap.trigger_index) * dt
    if falls:
        out["fall_time_s"] = _median(falls)
        out["first_fall_s"] = (f50[0] - cap.trigger_index) * dt
    if any(e <= dt * 1.01 for e in rises + falls):
        out["edge_at_resolution_limit"] = True
    if len(r50) >= 2:
        period = _median([b - a for a, b in zip(r50, r50[1:])]) * dt
        out["period_s"], out["freq_hz"] = period, 1 / period
        highs = []
        for a, b in zip(r50, r50[1:]):
            f = next((x for x in f50 if a < x < b), None)
            if f is not None:
                highs.append((f - a) / (b - a))
        if highs:
            out["duty_pct"] = 100 * sum(highs) / len(highs)
    return out
