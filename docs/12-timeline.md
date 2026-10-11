# 12. Timeline, DIO watch and timestamps

[← Sequencer](11-sequencer.md) · [Contents](README.md) · [Next: Python library →](13-python-library.md)

## 12.1 The idea

Most embedded bugs are of the form "when X happens in the firmware, Y
happens on the wires". To see that, you need both on one time axis. The
**timeline** records everything the board observes, continuously, and
merges it into one ordered list:

```
  0.000102  host    power on
  0.012311  dio     RESET ^  (levels 00001001)
  0.150204  uart    Booting v1.2...
  0.152870  uart    I2C init
  0.153190  i2c     S 76W- P
  0.154087  uart    sensor not found
  0.160442  i2c     S 76W+ D0+  S 76R+ 60- P
```

In this example the firmware tries the sensor 3 ms after printing
"I2C init". The sensor doesn't answer yet (NAK). The firmware gives up and
prints "sensor not found", and 7 ms later the sensor answers fine. The bug
is a missing start-up delay or retry, and it is visible at a glance.

Recording is continuous. Nothing that happens between your commands, or
while you are thinking, is lost.

## 12.2 Sources

| Source | What | Timestamp |
|---|---|---|
| `uart` | lines the target printed on its UART (through the bridge) | host arrival |
| `uart_tx` | what the host sent to the target | host |
| `i2c`, `spi` | messages from the sniffers | board: the START / CS edge |
| `dio` | edges on watched DIOs | board: the edge |
| `emu` | transfers between the target and an [emulated device](10-emulated-devices.md) | board |
| `line` | DTR/RTS changes on the bridge port ([control lines](09-uart-bridge.md#92-control-lines-resetboot)) | board |
| `seq` | end of a [sequencer](11-sequencer.md) run | board |
| `capture` | a scope / logic capture finished | host arrival |
| `host` | actions taken by the host (`note()`, and every acting tool call with the AI tools) | host |
| `mark` | markers you add | host |

## 12.3 Using it from Python

```python
import time
from tramuntana import Tramuntana, Timeline

with Tramuntana() as t:
    tl = Timeline(t, dio_names={3: "RESET", 4: "IRQ"})
    tl.start(watch=(1 << 3) | (1 << 4))       # record edges on DIO3 and DIO4
    tap = tl.open_uart(115200)                # the target's console, recorded line by line
    t.i2c.sniff(True)                         # sniffed messages are recorded too

    tl.mark("power-up test")
    tl.note("power on"); t.power(True)
    time.sleep(2)

    print(tl.format())                                  # everything
    print(tl.format(sources=["uart", "i2c"]))           # only some sources
    print(tl.format(since=0.1, until=0.2))              # a time window (seconds)
    print(tl.format(contains="error"))                  # a text search
    for e in tl.select(sources=["dio"]):                # Entry objects
        print(e.t, e.text, e.board_us, e.obj)

    t.i2c.sniff(False)
    tap.close()
    tl.stop()
```

| Method | What it does |
|---|---|
| `Timeline(t, keep=20000, dio_names=None)` | create; `keep` = entries kept (oldest dropped) |
| `start(watch=0, clear=True)` | start recording; `watch` = DIO mask of edges to record. Turns on sniffer timestamps |
| `watch(mask)` | change the watched DIOs |
| `open_uart(baud, ...)` | open the bridge port with a `UartTap` feeding the timeline; returns it |
| `feed_uart(data)` | feed UART bytes yourself, if you read the port another way |
| `note(text, source="host")` | add a host entry (an action) |
| `mark(label)` | add a marker |
| `select(since, until, sources, contains, last)` | `Entry` objects (`t`, `source`, `text`, `board_us`, `obj`) |
| `format(...)` | the same, as text lines |
| `stop()` | stop recording; entries stay available |

Times are **seconds since `start()`**. The sniffers must be started
separately (`t.i2c.sniff(True)`, `t.spi.sniff(True, mode)`), because only
one can run at a time and you choose which.

## 12.4 Using it from the CLI

```bash
tramuntana timeline --watch 0x18 --uart 115200 --i2c
tramuntana timeline --uart 9600 --spi 0
```

It prints entries as they arrive until Ctrl-C.

## 12.5 Using it from the webapp

The **TIMELINE** tab: tick the DIOs to watch (and name them), press START,
start sniffers on the SPI/I2C tabs and open the UART terminal if needed.
Entries appear live, coloured by source; filter them by source or text,
add markers, and save the list as a text file.

## 12.6 Using it from the AI tools

`timeline_start(watch_dios=[3, 4], dio_names={"3": "RESET"})`,
`uart_open(115200)`, `sniff_start("i2c")`, then act, then
`timeline_read(since_s=..., sources=[...], contains=...)`. Every tool
call that does something (power, writes, sequences…) is noted as a `host`
entry automatically, so the agent sees its own actions next to their
effects. See [AI tools](19-ai-tools.md).

## 12.7 DIO watch

The DIO watch reports **every edge** on chosen DIOs as an event with the
board's microsecond timestamp, and the levels of all eight pins after the
edge. It only listens: it works on pins that an instrument, the bridge or
the target drives.

```bash
tramuntana watch 0x09               # DIO0 and DIO3, until Ctrl-C
```
```
   12.004512  DIO3^  levels 00001001
   12.025831  DIO0v  levels 00001000
```

```python
t.dio_watch(0x09)
for ev in t.events(timeout=5):      # DioEdge(t_us, levels, changed, lost)
    print(ev)
t.dio_watch(0)
```

Shell: `watch 0x09`, `watch off`.

It is meant for **control lines**: reset, enable, interrupt, chip select
of a slow device, status LEDs. A pin toggling faster than about 50 edges
per millisecond is muted for 100 ms, so a clock can't flood the board. The
number of edges lost is reported in the next event. For buses and fast
signals, use the [logic analyzer](07-logic-analyzer.md).

## 12.8 The board clock

All board timestamps come from one clock: microseconds since boot, 32
bits, wrapping every ~71 minutes. Read it with `tramuntana`'s
`t.time_us()` or the shell's `time`.

The `Timeline` maps board time onto host time with a `time_us()` request
(the fastest of three, every 5 s), and unwraps the 32-bit counter.

**Sniffer timestamps** are optional. `t.event_stamps(True)` makes the
firmware add the board time of each message's START (I2C) or CS falling
edge (SPI); a `Timeline` turns this on. The sniffer's PIO program raises an
interrupt at that edge, and the firmware records the time together with
the position in the capture buffer. The stamp is therefore exact even if
the decoding runs much later.

## 12.9 Precision

| Entries | Precision |
|---|---|
| `dio`, `emu`, `line`, `seq`, `i2c`, `spi` | the board's clock: microseconds |
| sequencer records | microseconds |
| `uart` | host arrival time of the line's first byte: ~1 ms of USB latency, plus the UART's receive FIFO, which hands bytes over in groups (up to ~1.5 ms at 115200 baud, proportionally more at low baud rates) |
| `host`, `mark`, `capture` | host time when it happened or arrived |

For microsecond timing of something the target prints, capture its TX
line with the logic analyzer and decode it: the decoder gives each
character's time.

[← Sequencer](11-sequencer.md) · [Contents](README.md) · [Next: Python library →](13-python-library.md)
