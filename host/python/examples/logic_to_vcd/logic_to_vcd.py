#!/usr/bin/env python3
"""Capture DIO0..7 when SDA (DIO7) falls - an I2C START - and save a VCD for PulseView."""
from tramuntana import Tramuntana

with Tramuntana() as t:
    lc = t.logic.capture(n=100_000, rate_hz=10_000_000, trigger="pattern",
                         mask=0x80, value=0x00, pre=2000, timeout_ms=10_000)
    print(f"{lc.n} samples @ {lc.rate_hz} S/s, trigger at {lc.trigger_index}")
    for d in range(8):
        print(f"DIO{d}: {len(lc.edges(d))} transitions")
    lc.to_vcd("capture.vcd")
    print("saved capture.vcd")
