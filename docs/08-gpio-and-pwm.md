# 8. GPIO and PWM outputs

[← Logic analyzer](07-logic-analyzer.md) · [Contents](README.md) · [Next: UART bridge →](09-uart-bridge.md)

## 8.1 DIO as plain GPIO

Every DIO can be a plain input or output. Directions and levels are set
with masks (bit i = DIOi):

| Operation | CLI | Python | Shell |
|---|---|---|---|
| set direction | `tramuntana dio dir <mask> <out_mask>` | `t.dio_dir(mask, out_mask)` | `dio dir <mask> <out>` |
| drive outputs | `tramuntana dio write <mask> <value>` | `t.dio_write(mask, value)` | `dio write <mask> <value>` |
| read all eight | `tramuntana dio read` | `t.dio_read()` | `dio read` |

```bash
tramuntana dio dir 0x08 0x08          # DIO3 output
tramuntana dio write 0x08 0x00        # DIO3 low
tramuntana dio write 0x08 0x08        # DIO3 high
tramuntana dio dir 0x08 0x00          # release it (input)
tramuntana dio read                   # e.g. 0xfe
```

```python
RESET = 1 << 3
t.dio_dir(RESET, RESET)
t.dio_write(RESET, 0)
t.dio_write(RESET, RESET)
t.dio_dir(RESET, 0)                   # back to input: let the target's pull-up take over
print(f"{t.dio_read():08b}")
```

- `dio_read()` reads the pads **whoever drives them**: it works on pins
  that the UART bridge, a bus master or the target owns.
- `dio_dir()` takes the pins as owner `gpio`. It fails with **busy** on a
  pin held exclusively (the active UART bridge, an emulated device).
- `dio_write()` only drives pins that are currently plain GPIO outputs
  (status **not permitted** otherwise).
- There are no internal pull-ups on the DIOs: an input with nothing
  connected floats. Only DIO6/DIO7 have the switchable I2C pull-ups.

**Driving a reset line:** most reset pins are open-drain with a pull-up on
the target. Drive low to reset, then set the pin back to an input to
release it, rather than driving it high: that never fights a reset button
or a supervisor chip. For an exact pulse width, do it in a
[sequence](11-sequencer.md).

**Watching inputs over time:** `dio_watch` reports each edge on chosen pins
as a timestamped event. See [Timeline](12-timeline.md#127-dio-watch).

## 8.2 PWM outputs

Any DIO can output a hardware PWM. The RP2350 has one PWM slice per pair of
pins, so **the two pins of a pair share a frequency** (and a phase), each
with its own duty cycle and polarity:

| Slice | Pins |
|---|---|
| 0 | DIO7, DIO6 |
| 1 | DIO5, DIO4 |
| 2 | DIO3, DIO2 |
| 3 | DIO1, DIO0 |

That gives up to four independent frequencies.

| | |
|---|---|
| Frequency | ~10 Hz – 75 MHz (integer clock divider × 16-bit counter) |
| Duty | 0.01 % steps; actual resolution 1/(TOP+1), coarser at high frequencies |
| Glitch-free changes | duty and polarity (double-buffered); a frequency change may produce one odd period |
| Phase | per pair, applied when the pairs are synchronised |

`sync` restarts the selected pairs on the same clock edge, each delayed by
its phase. Pairs at the same (or integer-ratio) frequency then keep their
phase relationship indefinitely: multiphase signals, quadrature
encoders, interleaved drives.

PWM pins are claimed softly: another instrument can take a pin over. The
slice keeps running, but the pin no longer shows it, and the output is
then reported as "lost".

### From Python

```python
r = t.pwm.set(4, 10_000, 25)          # DIO4: 10 kHz, 25 % duty
print(r["freq_hz"])                   # achieved frequency: 10000.0
t.pwm.set(5, 10_000, 75, invert=True) # DIO5, same pair: same frequency, own duty and polarity

t.pwm.set(3, 10_000, 50)              # pair DIO3/DIO2 at the same frequency...
t.pwm.phase(3, 90)                    # ...delayed by 90 degrees
t.pwm.sync()                          # restart every live pair together (0 = all)

for dio, s in enumerate(t.pwm.status()):
    if s["active"]:
        print(dio, s["actual_hz"], s["duty"], s["phase"], s["lost"])

t.pwm.stop(1 << 4)                    # one pin (a DIO mask)
t.pwm.stop()                          # all of them
```

| Method | What it does |
|---|---|
| `set(dio, freq_hz, duty=50.0, invert=False)` | start or update one pin; returns `{freq_hz (achieved), level, top, div}`. **busy** if the pair partner runs at another frequency: stop it first |
| `phase(dio, degrees)` | phase delay of the pin's pair, 0..359°, applied by `sync()` |
| `sync(dio_mask=0)` | restart the pairs in the mask (0 = every live pair) on the same clock edge |
| `stop(dio_mask=0xFF)` | stop and release the pins |
| `status()` | 8 dicts, DIO0 first: `active`, `lost`, `invert`, `freq_hz` (requested), `actual_hz`, `duty`, `phase` |
| `Pwm.slice_of(dio)`, `Pwm.partner(dio)` | which slice drives a pin, and its pair partner |

### From the CLI

```bash
tramuntana pwm set 4 10k 25               # DIO4: 10 kHz, 25 %
tramuntana pwm set 5 10k 75 --invert      # DIO5 (same pair)
tramuntana pwm phase 2 90                 # pair DIO3/DIO2: 90 degrees
tramuntana pwm sync                       # restart every live pair together
tramuntana pwm status
tramuntana pwm stop all                   # or: pwm stop 4
```

### From the shell

```
pwm set 4 10000 25          # DIO4: 10 kHz, 25 % duty
pwm set 5 10000 75 inv      # DIO5 (same pair): 75 %, inverted
pwm phase 2 90              # pair DIO3/DIO2: 90 degrees
pwm sync                    # restart every live pair together
pwm status
pwm stop all
```

### From the webapp

The **PWM** tab shows one card per pair, with frequency and phase for the
pair, duty and polarity per pin, and **SYNC ALL**.

### Checking the result

Measure the outputs with the logic analyzer: the duty of DIO4 is
`sum(cap.channel(4)) / cap.n` on a capture, and the delay between the
rising edges of two pairs gives their phase. On a slow PWM filtered by the
target (an RC, a motor), `decode.measure` on a scope capture shows the
analog result.

[← Logic analyzer](07-logic-analyzer.md) · [Contents](README.md) · [Next: UART bridge →](09-uart-bridge.md)
