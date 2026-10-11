#!/usr/bin/env python3
"""Read distance from a VL53L0X time-of-flight sensor over I2C (DIO6 SCL, DIO7 SDA).

Wiring: VIN -> J1.1 (3V3), GND -> J1.2, SCL -> J1.15 (DIO6), SDA -> J1.16
(DIO7), XSHUT left open (pulled up on most modules). The sensor answers
at 0x29. Breakouts carry their own SDA/SCL pull-ups, so the board's stay
off by default: with them on and the 3V3 rail off, the module is
back-powered through SCL/SDA and never really resets.

The VL53L0X does not range out of reset: it needs ST's init sequence
(stop variable, SPAD setup, tuning settings, VHV/phase calibration).
This is a port of Pololu's VL53L0X Arduino library, which follows ST's
API, trimmed to single-shot ranging with the default timing budget.

    python3 vl53l0x_range.py               # 10 readings
    python3 vl53l0x_range.py -n 0          # until Ctrl-C
"""
import argparse
import time

from tramuntana import Tramuntana

SYSRANGE_START = 0x00
SYSTEM_SEQUENCE_CONFIG = 0x01
SYSTEM_INTERRUPT_CONFIG_GPIO = 0x0A
SYSTEM_INTERRUPT_CLEAR = 0x0B
RESULT_INTERRUPT_STATUS = 0x13
RESULT_RANGE_STATUS = 0x14
FINAL_RANGE_SIGNAL_RATE_LIMIT = 0x44
MSRC_CONFIG_CONTROL = 0x60
GPIO_HV_MUX_ACTIVE_HIGH = 0x84
VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV = 0x89
GLOBAL_CONFIG_SPAD_ENABLES_REF_0 = 0xB0
GLOBAL_CONFIG_REF_EN_START_SELECT = 0xB6
DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD = 0x4E
DYNAMIC_SPAD_REF_EN_START_OFFSET = 0x4F
IDENTIFICATION_MODEL_ID = 0xC0

RANGE_VALID = 11

# ST's default tuning settings (VL53L0X_DefaultTuningSettings), as (reg, value).
TUNING = [
    (0xFF, 0x01), (0x00, 0x00), (0xFF, 0x00), (0x09, 0x00), (0x10, 0x00), (0x11, 0x00),
    (0x24, 0x01), (0x25, 0xFF), (0x75, 0x00), (0xFF, 0x01), (0x4E, 0x2C), (0x48, 0x00),
    (0x30, 0x20), (0xFF, 0x00), (0x30, 0x09), (0x54, 0x00), (0x31, 0x04), (0x32, 0x03),
    (0x40, 0x83), (0x46, 0x25), (0x60, 0x00), (0x27, 0x00), (0x50, 0x06), (0x51, 0x00),
    (0x52, 0x96), (0x56, 0x08), (0x57, 0x30), (0x61, 0x00), (0x62, 0x00), (0x64, 0x00),
    (0x65, 0x00), (0x66, 0xA0), (0xFF, 0x01), (0x22, 0x32), (0x47, 0x14), (0x49, 0xFF),
    (0x4A, 0x00), (0xFF, 0x00), (0x7A, 0x0A), (0x7B, 0x00), (0x78, 0x21), (0xFF, 0x01),
    (0x23, 0x34), (0x42, 0x00), (0x44, 0xFF), (0x45, 0x26), (0x46, 0x05), (0x40, 0x40),
    (0x0E, 0x06), (0x20, 0x1A), (0x43, 0x40), (0xFF, 0x00), (0x34, 0x03), (0x35, 0x44),
    (0xFF, 0x01), (0x31, 0x04), (0x4B, 0x09), (0x4C, 0x05), (0x4D, 0x04), (0xFF, 0x00),
    (0x44, 0x00), (0x45, 0x20), (0x47, 0x08), (0x48, 0x28), (0x67, 0x00), (0x70, 0x04),
    (0x71, 0x01), (0x72, 0xFE), (0x76, 0x00), (0x77, 0x00), (0xFF, 0x01), (0x0D, 0x01),
    (0xFF, 0x00), (0x80, 0x01), (0x01, 0xF8), (0xFF, 0x01), (0x8E, 0x01), (0x00, 0x01),
    (0xFF, 0x00), (0x80, 0x00),
]


class Vl53l0x:
    def __init__(self, t: Tramuntana, addr: int = 0x29):
        self.i2c = t.i2c
        self.addr = addr
        self.stop_variable = 0

    def wr(self, reg: int, *values: int):
        self.i2c.write(self.addr, bytes([reg, *values]))

    def rd(self, reg: int, n: int = 1) -> bytes:
        return self.i2c.reg_read(self.addr, reg, n)

    def rd8(self, reg: int) -> int:
        return self.rd(reg)[0]

    def wait(self, cond, what: str, timeout: float = 0.5):
        end = time.monotonic() + timeout
        while not cond():
            if time.monotonic() > end:
                raise TimeoutError(f"VL53L0X: timeout waiting for {what}")

    def spad_info(self):
        """Reference SPAD count and type, from the sensor's NVM."""
        for reg, val in [(0x80, 0x01), (0xFF, 0x01), (0x00, 0x00), (0xFF, 0x06)]:
            self.wr(reg, val)
        self.wr(0x83, self.rd8(0x83) | 0x04)
        for reg, val in [(0xFF, 0x07), (0x81, 0x01), (0x80, 0x01), (0x94, 0x6B), (0x83, 0x00)]:
            self.wr(reg, val)
        self.wait(lambda: self.rd8(0x83) != 0x00, "SPAD info")
        self.wr(0x83, 0x01)
        tmp = self.rd8(0x92)
        self.wr(0x81, 0x00)
        self.wr(0xFF, 0x06)
        self.wr(0x83, self.rd8(0x83) & ~0x04 & 0xFF)
        for reg, val in [(0xFF, 0x01), (0x00, 0x01), (0xFF, 0x00), (0x80, 0x00)]:
            self.wr(reg, val)
        return tmp & 0x7F, bool(tmp & 0x80)

    def ref_calibration(self, vhv_init_byte: int):
        self.wr(SYSRANGE_START, 0x01 | vhv_init_byte)
        self.wait(lambda: self.rd8(RESULT_INTERRUPT_STATUS) & 0x07, "calibration")
        self.wr(SYSTEM_INTERRUPT_CLEAR, 0x01)
        self.wr(SYSRANGE_START, 0x00)

    def init(self, io_2v8: bool = True):
        # Genuine parts read 0xEE; clones may report another id.
        model = self.rd8(IDENTIFICATION_MODEL_ID)
        if model != 0xEE:
            print(f"warning: model id 0x{model:02x} (not 0xee), carrying on as a VL53L0X")

        if io_2v8:
            self.wr(VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV, self.rd8(VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV) | 0x01)
        self.wr(0x88, 0x00)                                   # I2C standard mode
        for reg, val in [(0x80, 0x01), (0xFF, 0x01), (0x00, 0x00)]:
            self.wr(reg, val)
        self.stop_variable = self.rd8(0x91)
        for reg, val in [(0x00, 0x01), (0xFF, 0x00), (0x80, 0x00)]:
            self.wr(reg, val)

        # Disable the MSRC and pre-range signal rate limit checks,
        # final range limit 0.25 MCPS (Q9.7).
        self.wr(MSRC_CONFIG_CONTROL, self.rd8(MSRC_CONFIG_CONTROL) | 0x12)
        self.wr(FINAL_RANGE_SIGNAL_RATE_LIMIT, 0x00, 0x20)
        self.wr(SYSTEM_SEQUENCE_CONFIG, 0xFF)

        # Reference SPADs: keep the first `count` good ones of the right type.
        count, aperture = self.spad_info()
        spad_map = bytearray(self.rd(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, 6))
        self.wr(0xFF, 0x01)
        self.wr(DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00)
        self.wr(DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C)
        self.wr(0xFF, 0x00)
        self.wr(GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4)
        first = 12 if aperture else 0
        enabled = 0
        for i in range(48):
            if i < first or enabled == count:
                spad_map[i // 8] &= ~(1 << (i % 8)) & 0xFF
            elif (spad_map[i // 8] >> (i % 8)) & 1:
                enabled += 1
        self.wr(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, *spad_map)

        for reg, val in TUNING:
            self.wr(reg, val)

        # "New sample ready" interrupt, active low.
        self.wr(SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04)
        self.wr(GPIO_HV_MUX_ACTIVE_HIGH, self.rd8(GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10 & 0xFF)
        self.wr(SYSTEM_INTERRUPT_CLEAR, 0x01)

        # Skip MSRC and TCC, then VHV and phase calibration.
        self.wr(SYSTEM_SEQUENCE_CONFIG, 0xE8)
        self.wr(SYSTEM_SEQUENCE_CONFIG, 0x01)
        self.ref_calibration(0x40)
        self.wr(SYSTEM_SEQUENCE_CONFIG, 0x02)
        self.ref_calibration(0x00)
        self.wr(SYSTEM_SEQUENCE_CONFIG, 0xE8)

    def range_mm(self):
        """One single-shot measurement: (distance in mm, range status)."""
        for reg, val in [(0x80, 0x01), (0xFF, 0x01), (0x00, 0x00)]:
            self.wr(reg, val)
        self.wr(0x91, self.stop_variable)
        for reg, val in [(0x00, 0x01), (0xFF, 0x00), (0x80, 0x00)]:
            self.wr(reg, val)
        self.wr(SYSRANGE_START, 0x01)
        self.wait(lambda: not (self.rd8(SYSRANGE_START) & 0x01), "range start")
        self.wait(lambda: self.rd8(RESULT_INTERRUPT_STATUS) & 0x07, "range result")
        r = self.rd(RESULT_RANGE_STATUS, 12)
        self.wr(SYSTEM_INTERRUPT_CLEAR, 0x01)
        return (r[10] << 8) | r[11], (r[0] >> 3) & 0x0F


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", type=lambda s: int(s, 16), default=0x29, help="7-bit address, hex (default 29)")
    ap.add_argument("--freq", type=int, default=100_000, help="SCL frequency in Hz (default 100000)")
    ap.add_argument("--pullups", action="store_true", help="enable the board's I2C pull-ups")
    ap.add_argument("-n", type=int, default=10, help="number of readings, 0 = until Ctrl-C")
    ap.add_argument("--period", type=float, default=0.2, help="seconds between readings")
    a = ap.parse_args()

    with Tramuntana() as t:
        t.pullups(False)                            # rail off + pull-ups on = back-powered
        t.power(False)
        time.sleep(0.5)                             # full power-down, clean boot
        t.power(True)
        time.sleep(0.1)
        t.i2c.config(a.freq, pullups=a.pullups)
        if a.addr not in t.i2c.scan():
            raise SystemExit(f"no device at 0x{a.addr:02x} (check wiring and power)")

        tof = Vl53l0x(t, a.addr)
        tof.init()
        i = 0
        try:
            while a.n == 0 or i < a.n:
                mm, status = tof.range_mm()
                note = "" if status == RANGE_VALID else f"  (status {status}, not valid)"
                print(f"{mm:5d} mm{note}")
                t.display(f"{mm} mm")
                i += 1
                time.sleep(a.period)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
