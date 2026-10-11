#!/usr/bin/env python3
"""Generate a 1 kHz sine on AOUT0, capture AIN0 (wire them together) and plot it."""
import matplotlib.pyplot as plt
from tramuntana import Tramuntana

with Tramuntana() as t:
    t.awg.sine(1000, amp=1.0, offset=1.65)
    cap = t.scope.capture(n=4096, rate_hz=500_000, trigger="rising", level=1.65, pre=1024)
    t.awg.off()

print(cap.stats())
plt.plot([x * 1e3 for x in cap.times()], cap.volts(0))
plt.axvline(0, color="grey", ls="--")
plt.xlabel("ms from trigger"); plt.ylabel("V"); plt.grid(alpha=0.3)
plt.show()
