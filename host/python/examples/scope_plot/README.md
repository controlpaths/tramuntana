# scope_plot

Generates a 1 kHz sine with the waveform generator, captures it with the
scope and plots it with matplotlib. A self-test of the analog side that
needs a single wire.

## Wiring

| From | To |
|---|---|
| J1.7 (AOUT0) | J1.3 (AIN0) |

## Usage

```bash
pip install matplotlib
python3 scope_plot.py
```

## How it works

1. `t.awg.sine(1000, amp=1.0, offset=1.65)` puts a 1 kHz sine of 1 V
   amplitude centred at 1.65 V on AOUT0.
2. `t.scope.capture()` takes 4096 samples of AIN0 at 500 kS/s (about 8 ms,
   8 periods), triggered on a rising edge through 1.65 V, with 1024 samples
   of pre-trigger.
3. The generator is switched off and the capture statistics (min, max,
   mean, Vpp...) are printed.
4. The trace is plotted in volts against milliseconds from the trigger,
   with the trigger instant marked by a dashed line.
