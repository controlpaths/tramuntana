# 11. Sequencer: microsecond-timed step lists

[← Emulated devices](10-emulated-devices.md) · [Contents](README.md) · [Next: Timeline →](12-timeline.md)

## 11.1 Why

Every command from the host is a USB round trip plus the host's own
scheduling. Two calls from a script are anywhere from one to tens of
milliseconds apart, and the gap varies from run to run. That is fine for
most work, but not for:

- a reset pulse of exactly 100 µs;
- "release reset, then talk to the sensor 2 ms later";
- "wait for the IRQ line, then read the data register immediately";
- measuring how long the target takes to raise a pin after reset;
- arming a capture right before the stimulus that triggers it.

The **sequencer** takes a list of steps from the host and runs it on the
board itself, one step after the other within microseconds. Each step
leaves a **record** with its result and the board time at which it
finished.

## 11.2 Steps

| Step | Arguments | Records |
|---|---|---|
| `delay_us` | microseconds | — (busy wait below 2 ms; the step adds ~14 µs, see [11.6](#116-timing-and-side-effects)) |
| `delay_ms` | milliseconds | — (sleeps, then busy-waits the last 2 ms) |
| `power` | on | — |
| `pullups` | on | — |
| `dio_dir` | mask, out_mask | — |
| `dio_write` | mask, value | — |
| `dio_read` | — | the eight levels |
| `wait_dio` | mask, value, timeout_us | microseconds it waited (a timeout is a failure) |
| `i2c_write` | address, data | bytes ACKed |
| `i2c_read` | address, n | the bytes |
| `i2c_write_read` | address, write, n | the bytes |
| `spi_txn` | write, read_n | the bytes |
| `uart_send` | data | — (the UART bridge must be active) |
| `awg_dc` | volts | — |
| `adc_read` | channel | volts |
| `mark` | id | the id (a timestamp to measure against) |
| `watch` | mask | — (same as `dio_watch`) |
| `loop` … `end_loop` | count | — (one level of nesting) |
| `logic_arm` | the logic analyzer's arm arguments | sample rate; read the capture afterwards |
| `scope_arm` | the scope's arm arguments | sample rate; read the capture afterwards |

A failing step (NAK, timeout, busy pin…) **stops the run**, unless it was
added with `keep_going=True`: then its failure is recorded and the run
continues. A run also stops when its overall timeout expires or when it is
aborted.

Limits: a program is up to 2 KB (several hundred steps); results up to
4 KB; one run at a time.

## 11.3 From Python

```python
from tramuntana import Tramuntana, Sequence

RESET, IRQ = 1 << 3, 1 << 4

s = (Sequence()
     .dio_dir(RESET, RESET)
     .dio_write(RESET, 0).delay_us(100)
     .mark(1)
     .dio_write(RESET, RESET)                     # release reset
     .wait_dio(IRQ, 0, timeout_us=500_000)        # until the target pulls IRQ low
     .i2c_write_read(0x76, b"\xd0", 1, keep_going=True)
     .dio_dir(RESET, 0))                          # release the line

with Tramuntana() as t:
    r = t.seq.run(s, timeout_s=10)
    print(r.state, r.error)                       # done / failed / timeout / aborted / invalid
    for step in r.steps:
        print(step)
```

Example output:

```
done None
+       0 us  dio_dir mask 0x08 out 0x08            ok
+      17 us  dio_write mask 0x08 = 0x00            ok
+     130 us  delay 100 us                          ok
+     141 us  mark 1                                ok  -> 1
+     146 us  dio_write mask 0x08 = 0x08            ok
+   21480 us  wait_dio mask 0x10 == 0x00            ok  -> 21321
+   21752 us  i2c_write_read 0x76 [d0] x1           ok  -> 60
+   21770 us  dio_dir mask 0x08 out 0x00            ok
```

In this example the target took 21.3 ms from reset release to pulling IRQ low, and
the sensor answered 270 µs later.

Each `SeqStep` has `op`, `label`, `ok`, `status`, `t_us` (board time),
`dt_us` (since the first step), `data` (raw) and `value` (decoded). The
`SeqResult` has `state`, `error`, `pc` (where it stopped) and `steps`.

To run without waiting: `t.seq.start(s)`, then `t.seq.status()` and
`t.seq.result(s)`; `t.seq.abort()` stops it. A `SeqDone` event is sent when
a run finishes.

### Arming a capture inside a sequence

```python
s = (Sequence()
     .logic_arm(n=20000, rate_hz=2e6, trigger="falling", pin=1, pre=200)
     .delay_us(100)
     .uart_send(b"AT\r\n"))
t.seq.run(s)
t.logic.wait()
cap = t.logic.read()
```

### Polling for readiness

How soon after reset does a sensor ACK? Loop a read with `keep_going` and
a delay; the first successful step tells you:

```python
s = Sequence().power(False).delay_ms(50).power(True).loop(100)
s.i2c_read(0x76, 1, keep_going=True).delay_us(500)
s.end_loop()
r = t.seq.run(s)
first_ok = next(st for st in r.steps if st.op == "i2c_read" and st.ok)
print("ready after", first_ok.dt_us, "us")
```

## 11.4 From the webapp

The **SEQ** tab takes the program as text, one step per line (`dio_write 3
0`, `delay_us 100`, `i2c_write_read 76 d0 read 1`, `loop 3` … `end_loop`, a
trailing `?` for keep-going), runs it and lists each step's time, status and
value. See [Web application](18-webapp.md#183-the-seq-tabs-step-syntax).

## 11.5 From the AI tools

`sequence_run` takes the steps as a list of dictionaries, with friendlier
pin syntax:

```json
[
  {"op": "dio_dir", "pin": 3, "output": true},
  {"op": "dio_write", "pin": 3, "level": 0},
  {"op": "delay_us", "us": 100},
  {"op": "dio_write", "pin": 3, "level": 1},
  {"op": "wait_dio", "pin": 4, "level": 0, "timeout_us": 500000},
  {"op": "i2c_write_read", "address": 118, "write": "d0", "n": 1, "keep_going": true},
  {"op": "loop", "count": 10, "steps": [{"op": "adc_read", "channel": 0}, {"op": "delay_ms", "ms": 1}]},
  {"op": "dio_dir", "pin": 3, "output": false}
]
```

It returns each step's time (µs from the first), result and status. See
[AI tools](19-ai-tools.md).

## 11.6 Timing and side effects

- Steps follow each other within about 5–15 µs (bus steps take as long as
  the transfer itself).
- A delay step lasts longer than requested by a fixed overhead, on top of
  the ~4 µs every step costs. Measured on a Tramuntana Lite (fw 1.1.0)
  between two `dio_write` steps with the logic analyzer:

  | Requested delay | Actual delay step |
  |---|---|
  | 0 – 1 ms | requested + 13–16 µs |
  | 1 – 2 ms | requested + 17–23 µs |
  | > 2 ms (sleeps, then busy-waits) | requested + ~37 µs |

  So a 500 µs `delay_us` between two pin writes gives a ~518 µs pulse
  (delay overhead plus the ~4 µs of the second write). For a precise
  interval, subtract both from the request (`delay_us(482)` for a 500 µs
  pulse), or check the result against the step records, which carry the
  real board time. Interrupts add jitter: some
  runs came out 15–50 µs longer than the rest, so don't rely on a
  sequence for timing tighter than that.
- The sequencer runs above most of the firmware's threads, so other work
  doesn't stretch its busy waits (interrupts still can, see above). While it busy-waits, lower-priority work catches up
  afterwards: sniffer decoding, event delivery and the display. Nothing is
  lost, and board timestamps stay exact. The UART bridge runs above the
  sequencer, so the target's bytes, baud rate changes and DTR/RTS keep
  flowing during a run.
- Steps that drive pins behave exactly like the corresponding commands:
  check what is connected before driving a pin the target might drive too.
- Records carry the same board clock as the events, so a run lines up with
  a [timeline](12-timeline.md); the run's end appears there as `SEQ done`.

The bytecode format is documented in
[Binary protocol](16-binary-protocol.md#sequencer-0x680x6c) and in
`fw/apps/tramuntana_fw/src/seq.h`.

[← Emulated devices](10-emulated-devices.md) · [Contents](README.md) · [Next: Timeline →](12-timeline.md)
