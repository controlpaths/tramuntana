#!/usr/bin/env python3
"""DS3231M real-time clock (Maxim DS3231MPMB1 Pmod) over I2C: read, set, watch.

Wiring (module J1 -> Tramuntana J1):

    J1.6 VCC -> J1.1 (3V3)      J1.5 GND -> J1.2
    J1.3 SCL -> J1.15 (DIO6)    J1.4 SDA -> J1.16 (DIO7)
    J1.2 INT/SQW -> J1.9 (DIO0), optional: 1 Hz output, used by 'sqw'
    J1.1 RST     -> not connected

The module has its own 4.7k pull-ups, so the board's stay off, and it
needs its coin cell fitted to work. The DS3231M answers at 0x68.

    python3 ds3231m_rtc.py read            # time, temperature, flags
    python3 ds3231m_rtc.py set             # set it from this computer's clock
    python3 ds3231m_rtc.py set 2026-10-03T18:30:00
    python3 ds3231m_rtc.py watch           # live, until Ctrl-C
    python3 ds3231m_rtc.py sqw             # check the 1 Hz output on DIO0
"""
import argparse
import datetime
import time

from tramuntana import Tramuntana, LinkError

ADDR = 0x68

REG_TIME = 0x00          # seconds .. year, 7 BCD registers
REG_CONTROL = 0x0E
REG_STATUS = 0x0F
REG_TEMP = 0x11          # MSB (signed degrees) + LSB (bits 7:6 = 0.25 C)

CTRL_CONV = 0x20         # start a temperature conversion
CTRL_INTCN = 0x04        # 1: INT/SQW is the alarm interrupt, 0: 1 Hz square wave
STAT_OSF = 0x80          # oscillator stopped since the flag was cleared
STAT_EN32KHZ = 0x08
STAT_BSY = 0x04

DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]


def bcd2bin(v: int) -> int:
    return (v >> 4) * 10 + (v & 0x0F)


def bin2bcd(v: int) -> int:
    return ((v // 10) << 4) | (v % 10)


class Ds3231m:
    def __init__(self, t: Tramuntana, addr: int = ADDR):
        self.i2c = t.i2c
        self.addr = addr

    def rd(self, reg: int, n: int = 1) -> bytes:
        return self.i2c.reg_read(self.addr, reg, n)

    def wr(self, reg: int, *values: int):
        self.i2c.write(self.addr, bytes([reg, *values]))

    def now(self) -> datetime.datetime:
        """Reads all seven time registers in one burst, so they are coherent."""
        r = self.rd(REG_TIME, 7)
        sec = bcd2bin(r[0] & 0x7F)
        minute = bcd2bin(r[1] & 0x7F)
        if r[2] & 0x40:                                  # 12-hour mode
            hour = bcd2bin(r[2] & 0x1F) % 12 + (12 if r[2] & 0x20 else 0)
        else:
            hour = bcd2bin(r[2] & 0x3F)
        date = bcd2bin(r[4] & 0x3F)
        month = bcd2bin(r[5] & 0x1F)
        year = 2000 + bcd2bin(r[6]) + (100 if r[5] & 0x80 else 0)
        return datetime.datetime(year, month, date, hour, minute, sec)

    def set(self, dt: datetime.datetime):
        """Writes the time (24-hour mode) and clears the oscillator-stop flag."""
        year = dt.year - 2000
        century = 0x80 if year >= 100 else 0
        self.wr(REG_TIME,
                bin2bcd(dt.second), bin2bcd(dt.minute), bin2bcd(dt.hour),
                dt.isoweekday(),                         # 1 = Monday .. 7 = Sunday
                bin2bcd(dt.day), century | bin2bcd(dt.month), bin2bcd(year % 100))
        self.wr(REG_STATUS, self.rd8(REG_STATUS) & ~STAT_OSF & 0xFF)

    def rd8(self, reg: int) -> int:
        return self.rd(reg)[0]

    def temperature(self, convert: bool = True) -> float:
        """Die temperature in C. The chip converts every 64 s on its own;
        ``convert`` forces a fresh one first (about 125 ms)."""
        if convert:
            while self.rd8(REG_STATUS) & STAT_BSY:
                time.sleep(0.01)
            self.wr(REG_CONTROL, self.rd8(REG_CONTROL) | CTRL_CONV)
            while self.rd8(REG_CONTROL) & CTRL_CONV:
                time.sleep(0.01)
        msb, lsb = self.rd(REG_TEMP, 2)
        return (msb - 256 if msb & 0x80 else msb) + (lsb >> 6) * 0.25

    def oscillator_stopped(self) -> bool:
        """True if the time can't be trusted (first power-up, battery out...)."""
        return bool(self.rd8(REG_STATUS) & STAT_OSF)

    def sqw(self, on: bool):
        """INT/SQW as a 1 Hz square wave (on) or as the alarm interrupt (off)."""
        ctrl = self.rd8(REG_CONTROL)
        self.wr(REG_CONTROL, (ctrl & ~CTRL_INTCN & 0xFF) if on else (ctrl | CTRL_INTCN))


def show(rtc: Ds3231m):
    dt = rtc.now()
    host = datetime.datetime.now().replace(microsecond=0)
    print(f"rtc:   {DAYS[dt.weekday()]} {dt:%Y-%m-%d %H:%M:%S}")
    print(f"host:  {DAYS[host.weekday()]} {host:%Y-%m-%d %H:%M:%S}  "
          f"(rtc {(dt - host).total_seconds():+.0f} s)")
    print(f"temp:  {rtc.temperature():.2f} C")
    if rtc.oscillator_stopped():
        print("warning: oscillator-stop flag set, the time is not valid - run 'set'")


def watch(t: Tramuntana, rtc: Ds3231m):
    last = None
    try:
        while True:
            dt = rtc.now()
            if dt != last:
                temp = rtc.temperature(convert=False)
                print(f"\r{dt:%Y-%m-%d %H:%M:%S}  {temp:5.2f} C", end="", flush=True)
                t.display(f"{dt:%H:%M:%S}")
                last = dt
            time.sleep(0.1)
    except KeyboardInterrupt:
        print()


def check_sqw(t: Tramuntana, rtc: Ds3231m, dio: int):
    """Enables the 1 Hz output and times it with the logic analyzer."""
    rtc.sqw(True)
    rate = 10_000
    try:
        # 3.5 s at 10 kS/s; the firmware timeout covers the whole capture.
        lc = t.logic.capture(n=35_000, rate_hz=rate, trigger="falling", pin=dio, timeout_ms=6000)
    except LinkError:
        lc = None
    finally:
        rtc.sqw(False)
    falls = [i for i, level in lc.edges(dio) if level == 0] if lc else []
    if not falls:
        raise SystemExit(f"no square wave on DIO{dio} (wired to the module's INT/SQW?)")
    period = falls[0] / rate                         # sample 0 is the trigger edge
    print(f"INT/SQW on DIO{dio}: period {period:.4f} s ({1 / period:.3f} Hz)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("op", choices=["read", "set", "watch", "sqw"])
    ap.add_argument("when", nargs="?", help="for 'set': ISO date/time (default: this computer's clock)")
    ap.add_argument("--sqw-dio", type=int, default=0, help="DIO wired to INT/SQW (default 0)")
    ap.add_argument("--pullups", action="store_true", help="enable the board's I2C pull-ups")
    a = ap.parse_args()

    with Tramuntana() as t:
        t.power(True)
        time.sleep(0.1)
        t.i2c.config(100_000, pullups=a.pullups)
        if ADDR not in t.i2c.scan():
            raise SystemExit("no DS3231M at 0x68 (check wiring, power and the coin cell)")
        rtc = Ds3231m(t)

        if a.op == "read":
            show(rtc)
        elif a.op == "set":
            if a.when:
                dt = datetime.datetime.fromisoformat(a.when)
            else:
                # Write on the host's second boundary, so the RTC starts in step.
                now = datetime.datetime.now()
                time.sleep(1 - now.microsecond / 1e6)
                dt = datetime.datetime.now().replace(microsecond=0)
            rtc.set(dt)
            print(f"set to {dt:%Y-%m-%d %H:%M:%S}")
            show(rtc)
        elif a.op == "watch":
            watch(t, rtc)
        else:
            check_sqw(t, rtc, a.sqw_dio)


if __name__ == "__main__":
    main()
