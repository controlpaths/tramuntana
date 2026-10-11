#!/usr/bin/env python3
"""Functional test of a Tramuntana Lite board.

Flashes the firmware from fw/build (if there is one), asks for a few wires
on J1, then tests every instrument against those wires: GPIO on all eight
DIO pins, UART bridge, SPI, I2C, PWM, logic analyzer, waveform generator,
ADC, scope and the switched 3.3 V rail. The RGB LED and the screen can't
check themselves, so the script lights them and asks what you see.

    python3 board_test.py                  # flash + test
    python3 board_test.py --no-flash       # test the firmware already on it
    python3 board_test.py --uf2 my.uf2     # flash another file

Exit code 0 when everything passes.
"""
import argparse
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import time

import serial

from tramuntana import Tramuntana, find_ports, ProtoError, LinkError
from tramuntana import proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_UF2 = os.path.normpath(os.path.join(HERE, "../../../../fw/build/tramuntana_fw/zephyr/zephyr.uf2"))

# Jumpers on J1: (pin, name, pin, name, what it tests).
WIRES = [
    (7, "AOUT0", 3, "AIN0", "waveform generator -> ADC / scope ch0"),
    (1, "3V3", 5, "AIN1", "target power rail -> ADC / scope ch1"),
    (9, "DIO0", 10, "DIO1", "GPIO, UART bridge loopback, PWM"),
    (12, "DIO3", 13, "DIO4", "GPIO, SPI MISO <- MOSI loopback"),
    (11, "DIO2", 15, "DIO6", "GPIO, SPI CS / I2C SCL"),
    (14, "DIO5", 16, "DIO7", "GPIO, SPI SCK / I2C SDA"),
]
DIO_PAIRS = [(0, 1), (3, 4), (2, 6), (5, 7)]

USE_COLOUR = sys.stdout.isatty()


def c(text, code):
    return f"\033[{code}m{text}\033[0m" if USE_COLOUR else text


class Report:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail=""):
        """ok: True = pass, False = fail, None = skipped."""
        self.rows.append((name, ok, detail))
        tag = c("PASS", "1;32") if ok else c("SKIP", "1;33") if ok is None else c("FAIL", "1;31")
        print(f"  [{tag}] {name:<24} {detail}")

    def failed(self):
        return [r for r in self.rows if r[1] is False]


def ask_yn(question):
    while True:
        try:
            a = input(f"  {question} [Y/N] ").strip().lower()
        except EOFError:
            return False
        if a in ("y", "yes", "s", "si"):
            return True
        if a in ("n", "no"):
            return False


# --- flashing ---------------------------------------------------------------------

def find_bootloader_mount():
    with open("/proc/mounts") as f:
        for line in f:
            mnt = line.split()[1].replace("\\040", " ")
            if re.search(r"RP2350|RPI-RP2", os.path.basename(mnt), re.I):
                return mnt
    return None


def wait_bootloader_mount(timeout=20.0):
    t0 = time.time()
    tried_mount = False
    while time.time() - t0 < timeout:
        mnt = find_bootloader_mount()
        if mnt:
            return mnt
        # No automounter: mount it ourselves once the block device shows up.
        for label in ("RP2350", "RPI-RP2"):
            dev = f"/dev/disk/by-label/{label}"
            if os.path.exists(dev) and not tried_mount and time.time() - t0 > 3:
                tried_mount = True
                subprocess.run(["udisksctl", "mount", "-b", dev], capture_output=True)
        time.sleep(0.25)
    return None


def wait_api_port(serial_number, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for b in find_ports():
            if b["api"] and (serial_number is None or b["serial"] == serial_number):
                time.sleep(0.5)                 # let the port settle
                return b
        time.sleep(0.25)
    return None


def flash(uf2, serial_number):
    print(f"Flashing {uf2}")
    with Tramuntana(serial_number=serial_number) as t:
        print(f"  running firmware: {t.info()['fw_version']}, rebooting into the bootloader...")
        t.bootloader()
    mnt = wait_bootloader_mount()
    if not mnt:
        sys.exit("  the RP2350 drive did not show up (mount it, or hold BOOTSEL and copy the UF2 by hand)")
    print(f"  copying to {mnt} ...")
    dst = os.path.join(mnt, os.path.basename(uf2))
    try:
        with open(uf2, "rb") as src, open(dst, "wb") as out:
            shutil.copyfileobj(src, out)
            out.flush()
            os.fsync(out.fileno())
    except OSError:
        pass                                    # the drive vanishes as soon as the last block lands
    if not wait_api_port(serial_number):
        sys.exit("  the board did not come back after flashing")
    print("  done")


def follows(cap, a, b):
    """True if pin b mirrors pin a, allowing one sample of skew at the edges
    (open-drain lines rise slowly through the pull-ups)."""
    A, B = cap.channel(a), cap.channel(b)
    for i, (x, y) in enumerate(zip(A, B)):
        if x != y and x not in B[max(0, i - 1):i + 2]:
            return False
    return True


# --- tests -----------------------------------------------------------------------------

def test_info(t, rep):
    i = t.info()
    rep.add("info", True, f"fw {i['fw_version']}, {i['board']}, chip {i['chip_id']}, "
                          f"proto v{i['proto_version']}")


def test_gpio(t, rep):
    t.pullups(False)
    for a, b in DIO_PAIRS:
        errors = []
        for drv, rcv in ((a, b), (b, a)):
            t.dio_dir((1 << a) | (1 << b), 1 << drv)
            for level in (0, 1, 0):
                t.dio_write(1 << drv, level << drv)
                time.sleep(0.002)
                got = (t.dio_read() >> rcv) & 1
                if got != level:
                    errors.append(f"DIO{drv}={level} -> DIO{rcv}={got}")
        t.dio_dir((1 << a) | (1 << b), 0)
        rep.add(f"gpio DIO{a}<->DIO{b}", not errors, "; ".join(errors) or "both directions, both levels")


def test_uart(t, rep, uart_port):
    if not uart_port:
        rep.add("uart bridge", False, "no bridge port found")
        return
    t.uart.config("auto")
    data = bytes(random.getrandbits(8) for _ in range(256))
    with serial.Serial(uart_port, 115200, timeout=1.0) as u:
        time.sleep(0.2)                         # bridge activates on DTR
        u.reset_input_buffer()
        u.write(data)
        got = u.read(len(data))
    time.sleep(0.1)
    ok = got == data
    rep.add("uart bridge", ok, f"256 B @ 115200 echoed via DIO1->DIO0" if ok
            else f"sent 256 B, got {len(got)} B{' (different)' if len(got) == len(data) else ''}")


def test_spi(t, rep):
    t.dio_dir(0xC0, 0x00)                       # DIO6/DIO7 (wired to CS/SCK) as inputs
    t.spi.config(1_000_000, mode=0)
    data = bytes(random.getrandbits(8) for _ in range(64))
    t.spi.sel()
    got = t.spi.xfer(data)
    t.spi.unsel()
    ok = got == data
    rep.add("spi loopback", ok, "64 B full duplex MOSI->MISO @ 1 MHz" if ok
            else f"sent {data[:8].hex(' ')}.. got {got[:8].hex(' ')}..")

    # Same traffic through the logic analyzer: CS, SCK and the MOSI bits.
    pkt = bytes([0xA5, 0x3C, 0x0F, 0xF0])
    t.logic.arm(n=8192, rate_hz=10_000_000, trigger="falling", pin=2)
    t.spi.txn(pkt)
    t.logic.wait()
    cap = t.logic.read()
    sck_rise = [i for i, lvl in cap.edges(5) if lvl == 1]
    mosi = cap.channel(4)
    bits = [mosi[i - 1] for i in sck_rise]       # sample just before the edge
    rx = bytes(int("".join(map(str, bits[k:k + 8])), 2) for k in range(0, len(bits) - 7, 8))
    cs_ok = 0 in cap.channel(2) and cap.channel(2)[-1] == 1
    errors = []
    if not cs_ok:
        errors.append("CS did not go low/high")
    if len(sck_rise) != 8 * len(pkt):
        errors.append(f"{len(sck_rise)} SCK edges, expected {8 * len(pkt)}")
    elif rx != pkt:
        errors.append(f"MOSI decoded {rx.hex(' ')}, sent {pkt.hex(' ')}")
    if cap.channel(3) != mosi:
        errors.append("MISO does not follow MOSI")
    if not follows(cap, 2, 6) or not follows(cap, 5, 7):
        errors.append("DIO6/DIO7 do not follow CS/SCK")
    rep.add("logic: spi capture", not errors, "; ".join(errors) or
            f"CS, 32 SCK edges, MOSI = {pkt.hex(' ')}")


def test_i2c(t, rep):
    t.dio_dir(0x24, 0x00)                       # DIO2/DIO5 (wired to SCL/SDA) as inputs
    t.i2c.config(100_000, pullups=True)
    time.sleep(0.01)
    idle = t.dio_read()
    if (idle & 0xC0) != 0xC0:
        rep.add("i2c pull-ups", False, f"SCL/SDA idle at {(idle >> 6) & 1}/{(idle >> 7) & 1}, expected 1/1")
        return
    rep.add("i2c pull-ups", True, "SCL/SDA idle high")
    t.logic.arm(n=20000, rate_hz=2_000_000, trigger="pattern", mask=0x80, value=0x00, pre=100)
    try:
        found = t.i2c.scan()
    except ProtoError as e:
        t.logic.abort()
        rep.add("i2c scan", False, str(e))
        return
    rep.add("i2c scan", True, f"no bus errors, {len(found)} device(s) {[hex(a) for a in found]}")
    t.logic.wait()
    cap = t.logic.read()
    scl_edges = len(cap.edges(6))
    errors = []
    if scl_edges < 100:
        errors.append(f"only {scl_edges} SCL edges")
    if not follows(cap, 6, 2) or not follows(cap, 7, 5):
        errors.append("DIO2/DIO5 do not follow SCL/SDA")
    rep.add("logic: i2c capture", not errors, "; ".join(errors) or f"{scl_edges} SCL edges, START seen")
    t.pullups(False)


def test_pwm(t, rep):
    freq, duty = 10_000, 25.0
    try:
        t.link.request(P.CMD_PWM_SET, struct.pack("<BIHB", 0, freq, int(duty * 100), 0))
    except ProtoError as e:
        rep.add("pwm", None if e.status == 8 else False, str(e))
        return
    try:
        cap = t.logic.capture(n=20000, rate_hz=10_000_000)
    finally:
        t.link.request(P.CMD_PWM_STOP, bytes([0x01]))
    rises = [i for i, lvl in cap.edges(1) if lvl == 1]
    falls = [i for i, lvl in cap.edges(1) if lvl == 0]
    if len(rises) < 3:
        rep.add("pwm", False, f"{len(rises)} rising edges on DIO1")
        return
    period = (rises[-1] - rises[0]) / (len(rises) - 1)
    f_meas = cap.rate_hz / period
    highs = [next(f for f in falls if f > r) - r for r in rises[:-1] if any(f > r for f in falls)]
    d_meas = 100 * sum(highs) / len(highs) / period
    ok = abs(f_meas - freq) < 0.01 * freq and abs(d_meas - duty) < 2
    rep.add("pwm DIO0 -> DIO1", ok, f"{f_meas:.0f} Hz, {d_meas:.1f} % (set {freq} Hz, {duty} %)")


def test_awg_adc(t, rep):
    errors = []
    for v in (0.3, 1.65, 3.0):
        t.awg.dc(v)
        time.sleep(0.05)
        got = sum(t.adc(0) for _ in range(4)) / 4
        if abs(got - v) > 0.1:
            errors.append(f"{v:.2f} V -> {got:.3f} V")
    rep.add("awg dc -> adc0", not errors, "; ".join(errors) or "0.30 / 1.65 / 3.00 V within 100 mV")

    t.awg.sine(1000, amp=1.0, offset=1.65)
    time.sleep(0.05)
    cap = t.scope.capture(n=4000, channels=1, rate_hz=100_000)
    t.awg.off()
    s = cap.stats(0)
    f = s["freq"] or 0
    ok = abs(f - 1000) < 20 and 1.7 < s["vpp"] < 2.2 and abs(s["mean"] - 1.65) < 0.1
    rep.add("awg sine -> scope ch0", ok, f"{f:.0f} Hz, {s['vpp']:.2f} Vpp, mean {s['mean']:.2f} V "
                                         f"(set 1000 Hz, 2.00 Vpp, 1.65 V)")


def test_power(t, rep, visual):
    t.power(True)
    time.sleep(0.2)
    on = t.adc(1)
    cap = t.scope.capture(n=1000, channels=2, rate_hz=100_000)
    s1 = cap.stats(1)
    if visual:
        rep.add("power led on", ask_yn("Target power is ON: is the power LED (D7) lit?"))
    t.power(False)
    time.sleep(0.5)
    off = t.adc(1)
    if visual:
        rep.add("power led off", ask_yn("Target power is OFF: is the power LED (D7) off?"))
    # With nothing but AIN1 and D7 on it, the switched-off rail only drains
    # through D7, which stops conducting below ~1.8 V: check that it dropped.
    ok = on > 3.1 and off < on - 0.8
    rep.add("power rail -> adc1", ok, f"on {on:.2f} V, off {off:.2f} V (unloaded)")
    rep.add("scope ch1", s1["mean"] > 3.1, f"rail on: mean {s1['mean']:.2f} V")


def test_visual(t, rep):
    for name, rgb in (("red", (1, 0, 0)), ("green", (0, 1, 0)), ("blue", (0, 0, 1))):
        t.led(*map(bool, rgb))
        rep.add(f"led {name}", ask_yn(f"Is the RGB LED {c(name.upper(), '1')}?"))
    t.led(False, False, False)
    rep.add("led off", ask_yn("Is the RGB LED off now?"))
    t.led()
    if not ask_yn("Is a display fitted?"):
        rep.add("display", None, "no display fitted")
        return
    code = f"{random.randint(0, 9999):04d}"
    t.display(f"BOARD TEST {code}")
    rep.add("display", ask_yn(f"Does the screen show {c('BOARD TEST ' + code, '1')}?"))
    t.display("")


def restore(t):
    for fn in (t.awg.off, lambda: t.power(False), lambda: t.pullups(False), lambda: t.led(),
               lambda: t.uart.config("auto"), lambda: t.dio_dir(0xFF, 0x00)):
        try:
            fn()
        except (ProtoError, LinkError):
            pass


# --- main ---------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--uf2", default=DEFAULT_UF2, help="firmware to flash first (default: fw/build)")
    ap.add_argument("--no-flash", action="store_true", help="test the firmware already on the board")
    ap.add_argument("--serial", help="USB serial number of the board (several plugged in)")
    ap.add_argument("--yes", action="store_true", help="don't wait for Enter after the wiring step")
    ap.add_argument("--no-visual", action="store_true", help="skip the LED / screen questions")
    args = ap.parse_args()

    boards = [b for b in find_ports() if args.serial is None or b["serial"] == args.serial]
    if not boards:
        sys.exit("No Tramuntana board found. Plug it in (or flash the firmware by hand with BOOTSEL).")
    board = boards[0]
    print(f"Board {board['serial']}: api {board['api']}, uart {board['uart']}")

    if args.no_flash:
        pass
    elif os.path.isfile(args.uf2):
        flash(args.uf2, board["serial"])
        board = wait_api_port(board["serial"])
    else:
        print(f"No UF2 at {args.uf2}, testing the firmware already on the board")

    print("\nWire these pins of J1 together (pin 1 = square pad):")
    for p1, n1, p2, n2, what in WIRES:
        print(f"  J1.{p1:<2} {n1:<5} <-> J1.{p2:<2} {n2:<5}  {c(what, '2')}")
    print("Nothing else connected to J1.")

    with Tramuntana(serial_number=board["serial"], timeout=3.0) as t:
        t.display_pinout(None)
        if not args.yes:
            try:
                input("Press Enter when the wires are in place... ")
            except EOFError:
                pass
        t.display_pinout(0)

        rep = Report()
        print("\nAutomatic tests")
        steps = [("info", lambda: test_info(t, rep)),
                 ("gpio", lambda: test_gpio(t, rep)),
                 ("uart bridge", lambda: test_uart(t, rep, board["uart"])),
                 ("spi", lambda: test_spi(t, rep)),
                 ("i2c", lambda: test_i2c(t, rep)),
                 ("pwm", lambda: test_pwm(t, rep)),
                 ("awg / scope", lambda: test_awg_adc(t, rep)),
                 ("power rail", lambda: test_power(t, rep, not args.no_visual))]
        try:
            for name, fn in steps:
                try:
                    fn()
                except (ProtoError, LinkError, serial.SerialException) as e:
                    rep.add(name, False, f"{type(e).__name__}: {e}")
                    restore(t)

            if not args.no_visual:
                print("\nVisual checks")
                test_visual(t, rep)
        finally:
            restore(t)

    bad = rep.failed()
    print()
    if bad:
        print(c(f"{len(bad)} test(s) FAILED: ", "1;31") + ", ".join(r[0] for r in bad))
        sys.exit(1)
    print(c(f"All {len(rep.rows)} tests passed.", "1;32"))


if __name__ == "__main__":
    main()
