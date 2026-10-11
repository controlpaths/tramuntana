#!/usr/bin/env python3
"""Live distance from an HC-SR04 ultrasonic sensor: TRIG on DIO1, ECHO on DIO2.

Wiring: VCC -> J1.1 (3V3), GND -> J1.2, TRIG -> J1.10 (DIO1), ECHO -> J1.11
(DIO2). The classic HC-SR04 wants 5 V; at 3.3 V use an HC-SR04P / RCWL-1601
(3-5.5 V parts).

Each reading pulses TRIG as a plain GPIO and times the ECHO pulse with the
logic analyzer, armed on ECHO's rising edge: the pulse width is the sound's
round trip.

    python3 hcsr04_range.py                 # until Ctrl-C
    python3 hcsr04_range.py --temp 30       # speed of sound at 30 C
    python3 hcsr04_range.py --scale 1.075   # calibrated against a ruler
"""
import argparse
import time

from tramuntana import Tramuntana, LinkError

TRIG, ECHO = 1, 2                                    # DIO numbers
RATE_HZ = 250_000                                    # 4 us/sample = 0.7 mm
WINDOW_S = 0.040                                     # longer than the 38 ms "no echo" pulse
MAX_MM = 4000                                        # HC-SR04 range limit


def speed_of_sound(temp_c: float) -> float:
    """m/s in dry air."""
    return 331.3 + 0.606 * temp_c


def measure(t: Tramuntana, c: float, scale: float = 1.0, offset_mm: float = 0.0):
    """One reading: distance in mm, or None (no echo / out of range)."""
    n = int(RATE_HZ * WINDOW_S)
    t.logic.abort()                                  # a capture left armed would make arm() busy
    t.logic.arm(n=n, rate_hz=RATE_HZ, trigger="rising", pin=ECHO, timeout_ms=100)
    t.dio_write(1 << TRIG, 1 << TRIG)                # >= 10 us high: two USB requests
    t.dio_write(1 << TRIG, 0)                        # take far longer than that
    st = t.logic.wait(1.0)
    if st["state"] != "done":
        return None                                  # no ECHO pulse at all
    echo = t.logic.read().channel(ECHO)
    if 0 not in echo:
        return None                                  # still high: nothing in range
    width_s = echo.index(0) / RATE_HZ
    mm = width_s * c / 2 * 1000 * scale + offset_mm
    return mm if mm <= MAX_MM else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--temp", type=float, default=20.0, help="air temperature in C (default 20)")
    ap.add_argument("--scale", type=float, default=1.0, help="calibration: distance x scale (default 1)")
    ap.add_argument("--offset", type=float, default=0.0, help="calibration: cm added after scaling (default 0)")
    ap.add_argument("--period", type=float, default=0.1, help="seconds between readings (>= 0.06)")
    ap.add_argument("-n", type=int, default=0, help="number of readings, 0 = until Ctrl-C")
    a = ap.parse_args()
    c = speed_of_sound(a.temp)

    with Tramuntana() as t:
        # The sniffers share the capture memory with the logic analyzer.
        st = t.status()
        if st["i2c_sniff"]:
            print("stopping the I2C sniffer (it holds the capture memory)")
            t.i2c.sniff(False)
        if st["spi_sniff"]:
            print("stopping the SPI sniffer (it holds the capture memory)")
            t.spi.sniff(False)
        t.power(True)
        time.sleep(0.1)
        t.dio_dir((1 << TRIG) | (1 << ECHO), 1 << TRIG)
        t.dio_write(1 << TRIG, 0)
        i = 0
        try:
            while a.n == 0 or i < a.n:
                start = time.monotonic()
                try:
                    mm = measure(t, c, a.scale, a.offset * 10)
                except LinkError:
                    mm = None
                if mm is None:
                    line = "  --- no echo"
                    t.display("no echo")
                else:
                    bar = "#" * round(40 * min(mm, 1000) / 1000)
                    line = f"{mm / 10:7.1f} cm  {bar:<40}"
                    t.display(f"{mm / 10:.1f} cm")
                print(f"\r{line}", end="", flush=True)
                i += 1
                time.sleep(max(0.0, a.period - (time.monotonic() - start)))
        except KeyboardInterrupt:
            pass
        print()


if __name__ == "__main__":
    main()
