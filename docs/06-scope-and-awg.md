# 6. Oscilloscope and waveform generator

[← I2C](05-i2c.md) · [Contents](README.md) · [Next: Logic analyzer →](07-logic-analyzer.md)

## 6.1 The oscilloscope

| | |
|---|---|
| Inputs | AIN0 (J1.3), AIN1 (J1.5) |
| Range | 0..3.3 V, 12 bits (0.8 mV per step) |
| Sample rate | up to 500 kS/s in total: one channel at 500 kS/s, or two at 250 kS/s each |
| Depth | 16384 samples in total (8192 per channel with two channels) |
| Trigger | none, rising or falling edge on AIN0, at any level |
| Pre-trigger | any number of samples up to the capture length |

The ADC runs freely into a ring buffer and the trigger is searched for in
the buffer, like a real oscilloscope. A capture can therefore include
samples from *before* the trigger, and an edge is never missed between
"waiting for the trigger" and "capturing". The sample rate is 48 MHz
divided by an integer ≥ 96, so the rate you ask for is rounded to the
nearest one possible; the actual rate is returned.

**Choosing settings:** the window is `n / rate` seconds. To see a 1 kHz
signal, 10 periods are 10 ms: `n = 2000` at 200 kS/s, for example. For a
slow event (a supply ramping up over 50 ms), lower the rate: `n = 5000` at
100 kS/s gives 50 ms.

### From the CLI

```bash
tramuntana scope -n 2048                              # free-running, max rate
tramuntana scope -n 4096 --trigger rising --level 1.65 --pre 1024 --plot
tramuntana scope --ch 2 --rate 100k --csv two_channels.csv
```

It prints min / max / Vpp / mean / RMS / frequency per channel.
`--csv` saves `t,ch0[,ch1]` in seconds and volts (t = 0 at the trigger);
`--plot` opens a matplotlib window.

### From Python

```python
cap = t.scope.capture(n=4096, channels=1, rate_hz=500_000,
                      trigger="rising", level=1.65, pre=1024, timeout_ms=5000)
print(cap.rate_hz, cap.trigger_index)
print(cap.stats())               # {'min', 'max', 'vpp', 'mean', 'rms', 'freq'}
volts = cap.volts(0)             # list of floats
times = cap.times()              # seconds from the trigger
cap.to_csv("trace.csv")
```

`capture()` blocks until done. To do something while the capture is armed
(that something usually being what triggers it), split it:

```python
t.scope.arm(n=2000, rate_hz=100_000, trigger="rising", level=1.0, pre=200)
t.awg.step(0.5, 2.5, delay_ms=5)     # AOUT0 wired to AIN0: the edge to catch
t.scope.wait()
cap = t.scope.read()
```

A trigger that never comes ends in state `"timeout"` (`capture()` raises
`LinkError`, `wait()` returns the state).

### Measurements

`tramuntana.decode.measure(cap, ch)` gives oscilloscope-style measurements
computed on the host:

```python
from tramuntana import decode
m = decode.measure(cap, 0)
# low, high, amplitude, overshoot_pct, undershoot_pct,
# rise_time_s, fall_time_s (10-90 %), freq_hz, period_s, duty_pct,
# first_rise_s, first_fall_s, rising_edges, falling_edges, sample_period_s
```

The levels come from the histogram (so overshoot doesn't move them) and the
edges from 10/50/90 % thresholds with hysteresis. `edge_at_resolution_limit`
is set when an edge is faster than one sample: the scope can't measure it,
so use the logic analyzer for timing. These measurements only make sense
for pulse-like signals. For a sine, use `stats()`.

### Watching a power-up

Wire the target's supply to AIN0, arm with a rising trigger at about a
third of the final voltage, then switch the rail on:

```python
t.scope.arm(n=4096, rate_hz=100_000, trigger="rising", level=1.0, pre=400)
t.power(True)
t.scope.wait(); cap = t.scope.read()
m = decode.measure(cap, 0)
print(m["high"], m["rise_time_s"], m["overshoot_pct"])
```

A dip after the first rise means inrush current or the protection tripping;
a non-monotonic ramp can stop some MCUs from starting.

### Quick voltmeter

`tramuntana adc 0`, `adc 0` in the shell, or `t.adc(0)` take one reading in
volts. It fails with **busy** while a scope capture is using the ADC.

## 6.2 The waveform generator

| | |
|---|---|
| Output | AOUT0 (J1.7) |
| Range | 0..3.3 V, 1 mV resolution in the settings |
| Frequency | 1 Hz – 100 kHz (the achieved frequency is returned, usually within 0.1 %) |
| Shapes | sine, triangle, sawtooth, square (duty), stairs (steps), DC, step, arbitrary |
| Arbitrary | one period of up to 4096 points, 8 bits |

There is no DAC: AOUT0 is a PWM at ~586 kHz behind a 2.7 kΩ / 1 nF RC filter
(≈59 kHz corner). A DMA loop plays a precomputed table into the PWM, so the
output keeps running while everything else works, until you turn it off.
High frequencies are smoothed by the filter (a 100 kHz square comes out
rounded), and the output can't drive a load, only a high-impedance input.

The output is `offset + amp × shape(t)`, with `shape` between −1 and 1, and
`amp` the **peak** amplitude (half of peak-to-peak). Anything outside
0..3.3 V is clipped.

### From the CLI

```bash
tramuntana awg sine 1k --amp 1 --offset 1.65          # 1 kHz, 0.65..2.65 V
tramuntana awg square 10k --amp 1.65 --offset 1.65 --param 25   # 25 % duty
tramuntana awg stairs 100 --param 8                   # 8 steps
tramuntana awg dc 1.2
tramuntana awg step 0.5 2.5 100                      # 0.5 V, then 2.5 V after 100 ms
tramuntana awg status
tramuntana awg off
```

### From Python

```python
t.awg.sine(1000, amp=1.0, offset=1.65)
t.awg.square(10_000, amp=1.65, offset=1.65, duty=25)
t.awg.set("tri", 500, amp=0.5, offset=1.0)
t.awg.set("stairs", 100, param=16)

import math
period = [math.sin(2 * math.pi * i / 100) ** 3 for i in range(100)]   # values in -1..1
t.awg.arbitrary(period, 200, amp=1.5, offset=1.65)

t.awg.dc(1.2)
t.awg.step(0.5, 2.5, delay_ms=50)     # returns after the step
print(t.awg.status())
t.awg.off()
```

### Typical uses

- **Self-test:** AOUT0 to AIN0, generate, capture, compare.
- **Stimulating an ADC input** of the target with a known signal or DC
  level, then reading what the firmware reports.
- **Finding a threshold:** sweep `dc()` and read a digital input with
  `dio_read()` to find where it flips.
- **Step response:** arm the scope on the edge, then `step()`, then read
  the capture to see how an input filter or comparator settles.

[← I2C](05-i2c.md) · [Contents](README.md) · [Next: Logic analyzer →](07-logic-analyzer.md)
