# 7. Logic analyzer and protocol decoding

[← Oscilloscope and AWG](06-scope-and-awg.md) · [Contents](README.md) · [Next: GPIO and PWM →](08-gpio-and-pwm.md)

## 7.1 What it captures

| | |
|---|---|
| Channels | 8: DIO0..DIO7, one byte per sample, bit i = DIOi |
| Sample rate | 150 MHz divided by an integer: 150 MS/s, 75, 50, 37.5… down to ~2.3 kS/s |
| Depth | 131072 samples (122880 with the pattern trigger) |
| Triggers | none, rising/falling edge on one pin, pattern on several pins |
| Pre-trigger | with the pattern trigger (and edges at ≤ 15 MS/s, which become a one-pin pattern) |
| Export | VCD (PulseView, GTKWave), CSV |

| Trigger | Max rate | Pre-trigger | How it works |
|---|---|---|---|
| `none` | 150 MS/s | — | starts at once |
| `rising` / `falling` on `pin` | 150 MS/s | — (yes at ≤ 15 MS/s) | the PIO program itself waits for the edge: zero latency |
| `pattern`: `(sample & mask)` becomes `== value` | 15 MS/s | yes | samples run continuously into a ring; the CPU scans for the first match |

The pattern trigger fires when the pins **enter** the pattern: a
non-matching sample has to be seen first. A pattern that is already true
when you arm waits for the next time it starts.

It only listens: it never drives a pin, so it can capture the target's
signals and this board's own buses at the same time.

**Choosing settings:** sample at least 4× (better 10×) the fastest signal.
Typical rates: UART 115200 → 2 MS/s; I2C 400 kHz → 4–10 MS/s; SPI 1 MHz →
10 MS/s; SPI 8 MHz → 75–150 MS/s. The window is `n / rate`: 131072 samples
at 10 MS/s is 13 ms.

## 7.2 Capturing

### From the CLI

```bash
tramuntana logic -n 65536 --rate 25M --vcd capture.vcd
tramuntana logic -n 131072 --rate 150M --trigger rising --pin 5 --vcd spi.vcd
tramuntana logic -n 100000 --rate 10M --trigger pattern --mask 0x80 --value 0 --pre 2000 --vcd i2c.vcd
tramuntana logic -n 20000 --rate 1M --plot
```

It prints the number of transitions per channel. `--vcd` / `--csv` save
the capture; `--plot` shows the eight lanes.

### From Python

```python
cap = t.logic.capture(n=100_000, rate_hz=10e6, trigger="pattern",
                      mask=0x80, value=0x00, pre=1000)    # DIO7 (SDA) low
print(cap.rate_hz, cap.trigger_index, cap.n)
bits = cap.channel(6)              # list of 0/1 for DIO6
edges = cap.edges(6)               # [(sample_index, new_level), ...]
cap.to_vcd("i2c.vcd", names=["RX", "TX", "CS", "MISO", "MOSI", "SCK", "SCL", "SDA"])
cap.to_csv("i2c.csv")
```

### Capturing this board's own traffic

Arm, generate the traffic, then wait and read:

```python
t.logic.arm(n=20000, rate_hz=10e6, trigger="pattern", mask=0x80, value=0x00, pre=200)
t.i2c.write_read(0x76, b"\xd0", 1)       # SDA falls at the START: trigger
t.logic.wait()
cap = t.logic.read()
```

The same pattern works for SPI (`trigger="falling", pin=2` on CS) and UART
(`trigger="falling", pin=1` on TX, with some `pre`).

From the [sequencer](11-sequencer.md), `logic_arm` arms the capture right
before the step that causes the event, so the trigger lines up with it
exactly.

## 7.3 Decoding UART, I2C and SPI

`tramuntana.decode` turns a `LogicCapture` into frames on the host. It
works on any DIO, so it covers buses wired to other pins, and you can
re-decode the same capture with other settings without capturing again.

```python
from tramuntana import decode

# UART: guess the baud rate from the shortest pulses, then decode
baud = decode.guess_baud(cap, pin=0)          # nearest standard rate, or the raw estimate
frames = decode.uart(cap, pin=0, baud=baud)   # parity="none"|"even"|"odd", stop_bits, invert
print(decode.uart_text(frames))               # 'Booting v1.2\r\n...'
for f in frames:
    print(f)                                  # 12.5us 42 B   (+ PARITY / FRAMING flags)

# I2C (default DIO6 SCL, DIO7 SDA)
for x in decode.i2c(cap):
    print(x)                                  # 10.0us S 76W+ D0+  S 76R+ 60- P

# SPI (default DIO2 CS, DIO5 SCK, DIO4 MOSI, DIO3 MISO)
for x in decode.spi(cap, mode=0):
    print(x)                                  # 2.0us MOSI 9F 00 00 00  MISO FF EF 40 18
```

| Function | Returns |
|---|---|
| `uart(cap, pin, baud, bits=8, parity="none", stop_bits=1, invert=False)` | `UartFrame(t_s, value, parity_error, framing_error)` list |
| `guess_baud(cap, pin)` | standard rate within 6 %, or the raw estimate; `None` with too few edges |
| `uart_text(frames)` | the characters, non-printables as `\xNN` |
| `i2c(cap, scl=6, sda=7)` | `I2cTransfer(t_s, message, stopped, partial_bits, scl_hz)` list |
| `spi(cap, cs=2, sck=5, mosi=4, miso=3, mode=0, lsb_first=False, word_bits=8)` | `SpiTransfer(t_s, mosi, miso, partial_bits, sck_hz, complete)` list; `cs=None` for a bus without CS |
| `measure(scope_cap, ch)` | oscilloscope measurements, see [Oscilloscope](06-scope-and-awg.md#measurements) |

Times (`t_s`) are seconds from the trigger.

**Reading the results:**

- **Framing errors in UART** with the right baud rate mean the wrong data
  bits, parity or stop bits, or a break. A guessed baud rate that isn't
  standard (e.g. 74880, or exactly 2× off) usually points to a
  misconfigured clock in the target.
- **`partial_bits`** in I2C/SPI means the transfer ended mid-byte: a
  master that aborted, or a CS released early.
- **Wrong SPI bytes, shifted by one bit:** wrong mode. Decode the same
  capture with `mode=0..3` and keep the one that makes sense.
- **Nothing decoded but edges present:** wrong pins, wrong baud rate or
  mode, or a sample rate too low.

The [AI tools](19-ai-tools.md) expose this as `logic_capture(decode=...)`
and `logic_decode`, plus `logic_image` / `scope_image`, which return a PNG
of the capture.

## 7.4 Memory sharing

The logic analyzer and the two sniffers share a 128 KB capture buffer: only
one runs at a time, and the others get **busy**. A finished logic capture
stays in memory until you read it, but if a sniffer needs the buffer it
takes it, and the capture is discarded (its state goes back to `"idle"`).

[← Oscilloscope and AWG](06-scope-and-awg.md) · [Contents](README.md) · [Next: GPIO and PWM →](08-gpio-and-pwm.md)
