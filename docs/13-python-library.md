# 13. Python library reference

[← Timeline](12-timeline.md) · [Contents](README.md) · [Next: CLI →](14-cli.md)

The `tramuntana` package (in `host/python`) drives a board over its binary
API port. Pure Python 3.8+; the only dependency is `pyserial`
(`matplotlib`/`numpy` are optional, for plots). Install it with
`pip install -e host/python` (see [Getting started](02-getting-started.md#22-install-the-host-software)).

Every method maps to one or a few [protocol commands](16-binary-protocol.md).
The docstrings carry the same information: `help(t.spi.txn)` works.

```python
from tramuntana import (Tramuntana, find_ports, Sequence, Timeline, UartTap,
                        decode, ProtoError, LinkError)
```

**Contents:**
[Module map](#131-module-map) ·
[Connecting](#132-connecting) ·
[Tramuntana](#133-tramuntana--the-board) ·
[t.spi](#134-tspi) ·
[t.i2c](#135-ti2c) ·
[t.scope](#136-tscope) ·
[t.awg](#137-tawg) ·
[t.pwm](#138-tpwm) ·
[t.logic](#139-tlogic) ·
[t.uart](#1310-tuart) ·
[t.emu](#1311-temu) ·
[t.seq and Sequence](#1312-tseq-and-sequence) ·
[Timeline and UartTap](#1313-timeline-and-uarttap) ·
[Events](#1314-events) ·
[Result types](#1315-result-types) ·
[decode](#1316-tramuntanadecode) ·
[Errors](#1317-errors) ·
[proto](#1318-tramuntanaproto) ·
[pinout](#1319-tramuntanapinout)

## 13.1 Module map

| Module | Contents |
|---|---|
| `tramuntana.device` | `Tramuntana`, the instrument classes (`Spi`, `I2c`, `Scope`, `Awg`, `Pwm`, `Logic`, `Uart`, `Emu`), result and event types, `find_ports`, event decoders |
| `tramuntana.seq` | `Sequence` (builder), `Sequencer` (`t.seq`), `SeqResult`, `SeqStep` |
| `tramuntana.timeline` | `Timeline`, `UartTap`, `Entry` |
| `tramuntana.decode` | UART/I2C/SPI decoders and scope measurements, on the host |
| `tramuntana.proto` | framing, `Link`, command/event constants, `ProtoError`, `LinkError` |
| `tramuntana.pinout` | the J1 header as data, and its terminal drawing |
| `tramuntana.cli` | the `tramuntana` command |
| `tramuntana.mcp_server` | the MCP server for AI agents (`tramuntana-mcp`; needs the `[mcp]` extra), see [AI tools](19-ai-tools.md) |
| `tramuntana.ai` | `install()` / `uninstall()`: register the MCP server and the skill with an agent (`tramuntana ai`) |

Everything commonly used is re-exported from `tramuntana`.

**Conventions:** bus data is `bytes` (anything `bytes()` accepts) in and
`bytes` out; I2C addresses are 7-bit; DIO masks have bit i = DIOi; volts
are floats; frequencies and rates are Hz.

## 13.2 Connecting

```python
from tramuntana import Tramuntana, find_ports

print(find_ports())
# [{'serial': 'FA6418C89B869936', 'shell': '/dev/ttyACM0',
#   'api': '/dev/ttyACM1', 'uart': '/dev/ttyACM2'}]

with Tramuntana() as t:                                    # first board found
    ...
with Tramuntana(serial_number="FA6418C89B869936") as t:   # a specific board
    ...
```

| Function | Returns |
|---|---|
| `find_ports()` | one dict per board: `serial` (USB serial number = chip id) and the `shell`, `api`, `uart` device paths (`None` if missing) |
| `find_api_port(serial_number=None)` | the API port's path; `LinkError` if no board matches |

## 13.3 Tramuntana — the board

```python
Tramuntana(port=None, serial_number=None, timeout=2.0)
```

| Argument | Meaning |
|---|---|
| `port` | API serial port; `None` finds it by USB VID/PID |
| `serial_number` | which board, when several are connected |
| `timeout` | seconds to wait for each reply |

Opening sends a PING and checks the answer; `LinkError` if no board is
found or the port isn't a Tramuntana API port. Use it as a context manager
or call `close()`. A background thread reads the port and requests are
serialised by a lock, so one object can be shared between threads.

**Attributes:** `spi`, `i2c`, `scope`, `awg`, `pwm`, `logic`, `uart`, `emu`, `seq`
(the instruments below), `port` (the device path), `proto_version`,
`link` (the [`Link`](#1318-tramuntanaproto)).

| Method | What it does | Returns |
|---|---|---|
| `info()` | firmware and board identity | `{fw_version, board, sysclk_hz, chip_id, features, proto_version}` |
| `status()` | snapshot of the whole board | dict, see below |
| `power(on=None)` | switch the target's 3.3 V rail; `None` only reads | state after the call |
| `pullups(on=None)` | switch the I2C pull-ups on DIO6/DIO7; `None` only reads | state after the call |
| `led(r=None, g=False, b=False)` | RGB LED (`led(True, False, False)` = red); `led()` = automatic | — |
| `adc(channel=0)` | one reading of AIN0/AIN1 | volts; `ProtoError` busy while the scope uses the ADC |
| `dio_dir(mask, out_mask)` | make pins plain GPIO; 1 in `out_mask` = output | —; busy on exclusively held pins |
| `dio_write(mask, value)` | drive output pins | —; not permitted on pins that aren't GPIO outputs |
| `dio_read()` | levels of DIO7..DIO0, whoever drives them | int |
| `dio_watch(mask)` | report every edge on these DIOs as `DioEdge` events; `0` stops | — |
| `time_us()` | the board clock (µs since boot, 32 bits) | int |
| `event_stamps(on)` | add the board time to sniffer events (`t_us`) | — |
| `display(text)` | up to 24 ASCII characters on the screen's note line | — |
| `display_pinout(seconds=60)` | J1 drawing on the screen; `None` = until the next call, `0` = back to the dashboard | — |
| `events(timeout=0.0)` | decoded events, see [Events](#1314-events) | generator |
| `on_event(callback)` | raw callback `fn(cmd, payload)` for every event, or `None` | — |
| `bootloader()` | reboot into the UF2 bootloader (the port disappears) | — |
| `close()` | stop the reader, close the port | — |

`status()` returns:

| Key | Value |
|---|---|
| `pins` | owner of DIO0..DIO7: `gpio`, `spi`, `i2c`, `uart`, `pio`, `pwm`, `emu`; `!` appended when held exclusively |
| `power`, `pullups` | bools |
| `scope`, `logic` | capture states |
| `awg`, `spi_sniff`, `i2c_sniff`, `uart_bridge` | bools |
| `rx_dropped` | API bytes the board had to drop (0 unless the host floods it) |

`info()["features"]` is a bit mask: SPI, I2C, scope, AWG, logic, UART,
PWM, DIO watch/time, UART control lines, emulated targets, sequencer
(bits 0–10).

## 13.4 t.spi

SPI master on DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK, and the SPI sniffer.

| Method | What it does | Returns |
|---|---|---|
| `config(freq_hz=1_000_000, mode=0, lsb_first=False)` | clock (≥ 1 kHz, up to 75 MHz), mode 0–3, bit order | the clock in use |
| `sel()` / `unsel()` | CS low / high | — |
| `write(data)` | send, CS untouched, MISO ignored | — |
| `read(n, fill=0x00)` | read `n` bytes sending `fill`, CS untouched | bytes |
| `xfer(data)` | full duplex, CS untouched | bytes clocked in |
| `txn(data, read_n=0)` | CS low, write `data` (≤ 4096), read `read_n` (≤ 4096), CS high | the `read_n` bytes |
| `sniff(on=True, mode=None)` | passive sniffer; `mode=None` uses the master's mode | — |

`write`, `read` and `xfer` split data longer than 4096 bytes into several
requests. See the [SPI guide](04-spi.md).

## 13.5 t.i2c

I2C master on DIO6 SCL / DIO7 SDA (bit-banged, open-drain, 1 kHz – 1 MHz,
clock stretching up to 10 ms), and the I2C sniffer.

| Method | What it does | Returns |
|---|---|---|
| `config(hz=100_000, pullups=True)` | bus speed and the board's pull-ups | the frequency |
| `scan()` | probe 0x08..0x77 | list of addresses that ACKed |
| `write(addr, data)` | START, addr+W, data (≤ 4096), STOP | bytes ACKed; `ProtoError` on NAK |
| `read(addr, n)` | START, addr+R, `n` bytes (≤ 4096, NAK on the last), STOP | bytes |
| `write_read(addr, data, n)` | write (≤ 255 bytes), repeated START, read `n` | bytes |
| `reg_read(addr, reg, n=1)` | `write_read(addr, [reg], n)` | bytes |
| `reg_write(addr, reg, data)` | `write(addr, [reg] + data)` | bytes ACKed |
| `start(addr, read=False)` | START (repeated if the bus is open) + address byte; leaves the bus open | `True` if ACKed; a NAK doesn't raise |
| `raw_write(data)` | bytes on the open bus | bytes ACKed; NAK raises |
| `raw_read(n, nack_last=True)` | bytes from the open bus | bytes |
| `stop()` | STOP | — |
| `reset_bus()` | up to 9 SCL pulses + STOP | — |
| `sniff(on=True, pullups=False)` | passive sniffer | — |

Errors: status 4 (NAK), 3 (SCL held low > 10 ms), 6 (`raw_*` without an
open bus). See the [I2C guide](05-i2c.md).

## 13.6 t.scope

AIN0/AIN1, 12 bits, 500 kS/s in total, 16384 samples in total.

| Method | What it does | Returns |
|---|---|---|
| `arm(n=1024, channels=1, rate_hz=None, trigger="none", level=1.65, pre=0, timeout_ms=5000)` | start a capture and return at once | actual rate per channel |
| `status()` | `{state, channels, n, rate_hz, trigger_index}` (`rate_hz` = total) | dict |
| `wait(timeout=10.0)` | poll until the capture leaves `armed`/`running` | final status; `LinkError` if still running |
| `read()` | download a finished capture | [`ScopeCapture`](#scopecapture); `LinkError` if not done |
| `capture(**arm_args)` | `arm` + `wait` + `read` | `ScopeCapture`; `LinkError` on timeout/abort |
| `abort()` | stop an armed/running capture | — |

| `arm()` argument | Meaning |
|---|---|
| `n` | samples per channel; `n × channels` ≤ 16384 |
| `channels` | 1 = AIN0, 2 = AIN0 + AIN1 (alternating) |
| `rate_hz` | per channel; `None` = maximum. 48 MHz / integer ≥ 96, so it is rounded |
| `trigger` | `"none"`, `"rising"`, `"falling"`, on AIN0 |
| `level` | trigger level, volts |
| `pre` | samples kept before the trigger (≤ `n`) |
| `timeout_ms` | state becomes `"timeout"` if no trigger |

**Capture states** (scope and logic): `idle`, `armed`, `running`, `done`,
`timeout`, `aborted`.

## 13.7 t.awg

AOUT0, 0..3.3 V, 1 Hz – 100 kHz. Output = `offset + amp × shape(t)`,
shape in −1..1, clipped to 0..3.3 V.

| Method | What it does | Returns |
|---|---|---|
| `set(shape, freq_hz, amp=1.0, offset=1.65, param=0)` | `shape`: `"sine"`, `"tri"`, `"saw"`, `"square"`, `"stairs"`, `"arb"` (last loaded table). `param`: duty % for square (default 50), steps for stairs (2..256, default 8) | `{freq_hz (achieved), table, top}` |
| `sine(freq_hz, amp=1.0, offset=1.65)` | sine | same |
| `square(freq_hz, amp=1.0, offset=1.65, duty=50)` | square | same |
| `arbitrary(samples, freq_hz, amp=1.65, offset=1.65)` | load one period (≤ 4096 values in −1..1, stored as 8 bits) and play it | same |
| `dc(volts)` | constant level | — |
| `step(from_v, to_v, delay_ms)` | `from_v`, wait on the board, then `to_v`; returns after the step (keep it under the link timeout) | — |
| `off()` | stop | — |
| `status()` | `{running, shape, freq_hz, amp, offset}` | dict |

## 13.8 t.pwm

Hardware PWM on the DIOs. Pairs DIO7/6, DIO5/4, DIO3/2, DIO1/0 share one
slice: same frequency and phase, own duty and polarity. ~10 Hz – 75 MHz.

| Method | What it does | Returns |
|---|---|---|
| `set(dio, freq_hz, duty=50.0, invert=False)` | start/update one pin; duty in % (0.01 steps) | `{freq_hz (achieved), level, top, div}`; busy if the partner runs at another frequency |
| `phase(dio, degrees)` | phase delay of the pin's pair (0..359), applied by `sync()` | — |
| `sync(dio_mask=0)` | restart those pairs (0 = all live) on the same clock edge | — |
| `stop(dio_mask=0xFF)` | stop, pins back to plain GPIO inputs | — |
| `status()` | per DIO (DIO0 first): `{active, lost, invert, freq_hz, actual_hz, duty, phase}` | list of 8 dicts |
| `Pwm.slice_of(dio)`, `Pwm.partner(dio)`, `Pwm.PAIRS` | slice of a pin, its pair partner, the pairs per slice | |

See [GPIO and PWM](08-gpio-and-pwm.md#82-pwm-outputs).

## 13.9 t.logic

DIO0..DIO7, one byte per sample, 150 MS/s, 131072 samples.

| Method | What it does | Returns |
|---|---|---|
| `arm(n=4096, rate_hz=150e6, trigger="none", pin=0, mask=0, value=0, pre=0, timeout_ms=5000)` | start and return at once | actual rate |
| `status()` | `{state, n, rate_hz, trigger_index}` | dict |
| `wait(timeout=10.0)` | poll until done | final status; `LinkError` if still running |
| `read()` | download | [`LogicCapture`](#logiccapture) |
| `capture(**arm_args)` | `arm` + `wait` + `read` | `LogicCapture` |
| `abort()` | stop | — |

| `arm()` argument | Meaning |
|---|---|
| `n` | samples (rounded up to a multiple of 4); ≤ 131072, ≤ 122880 for a pattern |
| `rate_hz` | 150 MHz / integer, rounded |
| `trigger` | `"none"`, `"rising"`, `"falling"` (on `pin`), `"pattern"` |
| `pin` | DIO for the edge triggers |
| `mask`, `value` | pattern: fires when `(sample & mask)` becomes `== value` |
| `pre` | samples before the trigger: pattern trigger, or an edge at ≤ 15 MS/s |
| `timeout_ms` | state becomes `"timeout"` if no trigger |

The host raises `ValueError` for impossible pattern captures (rate above
15 MS/s, `mask` 0, more than 122880 samples). The analyzer and the two
sniffers share memory: one at a time.

## 13.10 t.uart

Control of the UART bridge (the data flows on the bridge port: use
pyserial, a terminal, or [`UartTap`](#1313-timeline-and-uarttap)).

| Method | What it does | Returns |
|---|---|---|
| `config(mode="auto", baud=0)` | `"auto"` (active while the port is open), `"on"`, `"off"`; `baud` ≠ 0 forces the rate on the target side | — |
| `ctrl(mode="esp", reset_dio=None, boot_dio=None)` | RTS/DTR drive two DIOs (open-drain): `"esp"`, `"direct"` or `"off"`; pins DIO2..DIO7 or `None` | — |
| `status()` | `{active, baud, rx_bytes, tx_bytes, mode, ctrl, reset_dio, boot_dio, dtr, rts, reset_low, boot_low}` | dict |

See the [UART bridge guide](09-uart-bridge.md).

## 13.11 t.emu

The board as an I2C or SPI device, from a 256-byte register file.

| Method | What it does | Returns |
|---|---|---|
| `i2c(address, scl=6, sda=7, pullups=False)` | answer at a 7-bit address on those DIOs | — |
| `spi(mode=0, cs=2, sck=5, mosi=4, miso=3, addr_mask=0x7F, read_mask=0x80, read_value=0x80, status=0x00)` | answer as a SPI device | — |
| `stop()` | stop and release the pins (registers kept) | — |
| `set_regs(offset, data)` | load 1..256 registers from `offset` (wraps at 256), also while running | — |
| `regs(offset=0, n=256)` | read registers back | bytes |
| `read_only(registers)` | registers whose writes are ignored; `[]` = all writable | — |
| `status()` | `{bus: "off"/"i2c"/"spi", transfers, bytes_read, bytes_written, naks, starts, stops, bytes}`; the last three are low-level counters (START/STOP seen, bytes shifted) for diagnosing a bus | dict |

Transfers arrive as [`EmuTransfer`](#1314-events) events. See the
[emulated devices guide](10-emulated-devices.md).

## 13.12 t.seq and Sequence

**`Sequence()`**: a builder; each method appends a step and returns the
sequence, so calls chain. `keep_going=True` records a failure and
continues.

| Method | Step |
|---|---|
| `delay_us(us)` / `delay_ms(ms)` | wait |
| `mark(mark_id)` | timestamp with an id |
| `loop(count)` / `end_loop()` | repeat the steps in between (one level) |
| `power(on)` / `pullups(on)` | as on the board |
| `dio_dir(mask, out_mask)` | GPIO directions |
| `dio_write(mask, value, keep_going=False)` | GPIO levels |
| `dio_read()` | read the levels |
| `wait_dio(mask, value, timeout_us=1_000_000, keep_going=False)` | wait until `dio & mask == value & mask` |
| `watch(mask)` | DIO watch on/off |
| `awg_dc(volts)` | AOUT0 level |
| `adc_read(channel=0)` | one ADC reading |
| `i2c_write(address, data, keep_going=False)` | I2C write |
| `i2c_read(address, n, keep_going=False)` | I2C read |
| `i2c_write_read(address, write, n, keep_going=False)` | register read |
| `spi_txn(write, read_n=0, keep_going=False)` | SPI transaction |
| `uart_send(data, keep_going=False)` | bytes to the target's RX (bridge active) |
| `logic_arm(n=4096, rate_hz=10e6, trigger="none", pin=0, mask=0, value=0, pre=0, timeout_ms=5000)` | arm the logic analyzer |
| `scope_arm(n=1024, channels=1, rate_hz=None, trigger="none", level=1.65, pre=0, timeout_ms=5000)` | arm the scope |

Attributes: `code` (the program bytes, ≤ 2048), `labels` (`{pc:
description}`).

**`t.seq`** (`Sequencer`):

| Method | What it does | Returns |
|---|---|---|
| `run(seq, timeout_s=10.0)` | upload, run, wait, download | `SeqResult`; `LinkError` if it doesn't finish |
| `start(seq, timeout_s=10.0)` | upload and start without waiting | — |
| `status()` | `{state, error, pc, result_len}` | dict |
| `result(seq=None)` | download the records of the last run (labels from `seq`) | `SeqResult` |
| `abort()` | stop the running program | — |
| `load(seq)` | upload only | — |

**`SeqResult`**: `state` (`done`, `failed`, `aborted`, `timeout`,
`invalid`), `error` (status name or `None`), `pc` (where it stopped),
`steps` (list of `SeqStep`), `ok` (property: `state == "done"`).

**`SeqStep`**: `pc`, `op` (step name), `ok`, `status` (`"ok"` or the error
name), `t_us` (board time when it finished), `dt_us` (µs since the first
step), `data` (raw bytes), `label`, and the property `value`: levels for
`dio_read`, µs for `wait_dio`, volts for `adc_read`, the rate for
`*_arm`, ACK count for `i2c_write`, bytes for reads. `str(step)` prints a
line.

See the [sequencer guide](11-sequencer.md).

## 13.13 Timeline and UartTap

**`Timeline(t, keep=20000, dio_names=None)`**

| Method / attribute | What it does |
|---|---|
| `start(watch=0, clear=True)` | start recording; DIO mask to watch; turns on event stamps |
| `watch(mask)` | change the watched DIOs |
| `open_uart(baud=115200, **kwargs)` | open the bridge port with a `UartTap` feeding the timeline; returns it |
| `feed_uart(data, at=None)` | record UART bytes read some other way |
| `note(text, source="host")` | add an entry |
| `mark(label)` | add a marker |
| `select(since=None, until=None, sources=None, contains=None, last=None)` | list of `Entry` |
| `format(**same)` | text, one entry per line |
| `stop()` | stop recording (entries kept), DIO watch and stamps off |
| `entries`, `running`, `t0`, `dio_names` | the deque of entries, state, start time (`time.monotonic()`), names |

**`Entry`**: `t` (seconds since `start()`), `source`, `text`, `board_us`
(the board's timestamp, or `None`), `obj` (the decoded event).

**`UartTap(t, baud=115200, parity="N", stop_bits=1, dtr=None, rts=None, timeline=None, port=None)`**:
opens the bridge port (found from `t`, or given by `port`) and reads it in
the background.

| Method / attribute | What it does |
|---|---|
| `read(timeout=1.0, until=None, max_bytes=65536)` | buffered bytes; returns early once `until` arrives (up to and including it) or `max_bytes` are there |
| `write(data)` | send to the target (noted on the timeline as `uart_tx`) |
| `set_lines(dtr=None, rts=None)` | control lines (see `t.uart.ctrl`) |
| `pending()` | bytes buffered |
| `close()` | stop and close |
| `timeline` | the `Timeline` it feeds (settable), `rx_total`, `ser` (the pyserial port), `port` |

See the [timeline guide](12-timeline.md).

## 13.14 Events

The board sends events on its own. The reader thread queues the last 1000;
`t.events(timeout)` yields them decoded, waiting up to `timeout` seconds for
each next one (`0` only drains the queue).

| Event | Yielded as |
|---|---|
| SPI message (CS low..high) | `SpiMessage` |
| I2C message (START..STOP) | `I2cMessage` |
| edge on a watched DIO | `DioEdge(t_us, levels, changed, lost)` |
| emulator transfer | `EmuTransfer(t_us, bus, read, address, reg, data, truncated, nak, no_start)`; `nak` = an I2C address byte not answered (`reg` holds the raw byte) |
| DTR/RTS change on the bridge port | `LineChange(t_us, dtr, rts)` |
| sequencer run finished | `SeqDone(t_us, state, error, pc, result_len)` |
| capture finished | `{"event": "capture_done", "instrument": "scope"/"logic", "state": ...}` |
| log text | `{"event": "log", "text": ...}` |
| anything else | `{"event": cmd, "payload": bytes}` |

```python
t.i2c.sniff(True)
for ev in t.events(timeout=5):
    print(ev)
```

Other ways to receive them:

- `t.on_event(fn)`: one raw callback `fn(cmd, payload)` in the reader
  thread (keep it short; no requests from inside it).
- `t.link.add_listener(fn)` / `remove_listener(fn)`: any number of raw
  callbacks (a `Timeline` uses one), alongside the queue.
- `decode_event(cmd, payload)` decodes a raw event;
  `decode_spi_event(payload)` and `decode_i2c_event(payload)` the sniffer
  ones.

All `t_us` values are the board clock (`time_us()`).

## 13.15 Result types

### ScopeCapture

| Member | Meaning |
|---|---|
| `channels` | 1 or 2 |
| `rate_hz` | samples/s per channel |
| `trigger_index` | index of the trigger sample in each channel (`pre`; 0 without a trigger) |
| `raw[ch][i]` | 12-bit codes, 0..4095 = 0..3.3 V |
| `n` | samples per channel |
| `volts(ch=0)` | list of volts |
| `times()` | seconds relative to the trigger |
| `stats(ch=0)` | `{min, max, vpp, mean, rms}` in volts, `freq` in Hz (mean-crossing estimate, `None` if unknown) |
| `to_csv(path)` | columns `t`, `ch0`[, `ch1`] |

### LogicCapture

| Member | Meaning |
|---|---|
| `rate_hz` | samples/s |
| `trigger_index` | trigger sample (`pre` for a pattern trigger, else 0) |
| `samples` | `bytes`, one per sample, bit i = DIOi |
| `n` | number of samples |
| `channel(dio)` | list of 0/1 levels |
| `edges(dio)` | `[(sample_index, new_level), ...]` |
| `to_vcd(path, names=None)` | VCD (1 ps timescale); `names` = 8 signal names, DIO0 first |
| `to_csv(path)` | columns `t`, `DIO0`..`DIO7` |

### SpiMessage

`total` (bytes in the transaction), `mosi`, `miso` (the first ≤ 256 bytes
of each direction), `overrun`, `t_us` (board time of CS falling when stamps
are on). `str()`: `SPI 4B  MOSI 9f 00 00 00  MISO 00 ef 40 18`.

### I2cMessage and I2cFrame

`I2cMessage`: `frames` (one per START), `overrun`, `truncated`, `t_us`.
`str()`: `I2C S 68W+ 75+  S 68R+ 71- P`.
`I2cFrame`: `address` (7-bit), `read`, `ack` (`None` if the bus stopped
first), `data` (list of `(byte, acked)`).

## 13.16 tramuntana.decode

Host-side analysis of captures; see the
[logic analyzer guide](07-logic-analyzer.md#73-decoding-uart-i2c-and-spi) and
[scope measurements](06-scope-and-awg.md#measurements).

| Function | Returns |
|---|---|
| `uart(cap, pin, baud, bits=8, parity="none", stop_bits=1, invert=False)` | list of `UartFrame(t_s, value, parity_error, framing_error)` |
| `guess_baud(cap, pin)` | nearest standard rate within 6 %, else the raw estimate; `None` with too few edges |
| `uart_text(frames)` | text, non-printables as `\xNN` |
| `i2c(cap, scl=6, sda=7)` | list of `I2cTransfer(t_s, message, stopped, partial_bits, scl_hz)` |
| `spi(cap, cs=2, sck=5, mosi=4, miso=3, mode=0, lsb_first=False, word_bits=8)` | list of `SpiTransfer(t_s, mosi, miso, partial_bits, sck_hz, complete)`; `cs=None` = no CS line |
| `measure(cap, ch=0, min_amplitude=0.05)` | dict: `low`, `high`, `amplitude`, `overshoot_pct`, `undershoot_pct`, `rise_time_s`, `fall_time_s`, `freq_hz`, `period_s`, `duty_pct`, `first_rise_s`, `first_fall_s`, `rising_edges`, `falling_edges`, `sample_period_s`; `dc`/`mean` for a flat signal; `edge_at_resolution_limit` |

`STANDARD_BAUDS` lists the rates `guess_baud` snaps to.

## 13.17 Errors

| Exception | When |
|---|---|
| `ProtoError` | the board answered with an error; `e.status` (code below), `e.cmd` (command id) |
| `LinkError` | no board, wrong port, no reply in time, a capture that ended in timeout/abort, a sequence that didn't finish |
| `ValueError` | arguments the host rejects itself (logic pattern limits, empty sequence, program too long…) |
| `struct.error` | a value that doesn't fit its field (negative voltage, too many bytes) |

| Status | Name | Typical cause |
|---|---|---|
| 1 | invalid argument | out-of-range value, data too long |
| 2 | busy | pins held exclusively, capture or sniffer memory in use, ADC in use, sequence running |
| 3 | timeout | I2C SCL held low > 10 ms, `wait_dio` timeout |
| 4 | bus error / NAK | I2C device didn't ACK |
| 5 | not supported | — |
| 6 | not permitted | pin not in the right mode, I2C bus not open, unfinished capture, bridge not active |
| 7 | no memory | PIO program space or state machines exhausted |
| 8 | unknown command | firmware older than the library |
| 9 | wrong state | — |

`tramuntana.proto.STATUS_NAMES` maps codes to these names.

## 13.18 tramuntana.proto

For low-level work, or commands added to the firmware before the library
has a method for them. The
`CMD_*` and `EVT_*` constants mirror `fw/apps/tramuntana_fw/src/proto.h`.

```python
from tramuntana import proto as P
r = t.link.request(P.CMD_ADC_READ, bytes([1]))      # payload in, reply data out
```

| Name | What |
|---|---|
| `Link(ser, timeout=2.0)` | framing over a pyserial port, with a reader thread |
| `Link.request(cmd, payload=b"", timeout=None)` | send a command, return the reply data (after the status byte); `ProtoError` / `LinkError` |
| `Link.events` | the event queue (deque of `(cmd, payload)`, last 1000) |
| `Link.on_event`, `add_listener(fn)`, `remove_listener(fn)` | event callbacks |
| `encode(cmd, seq, payload)` | build a frame |
| `crc16(data)` | CRC-16/CCITT-FALSE |
| `STATUS_NAMES`, `CAP_NAMES` | code → name |
| `PROTO_VERSION`, `MAGIC`, `MAX_PAYLOAD`, `SOF`, `REPLY_FLAG` | protocol constants |

## 13.19 tramuntana.pinout

| Name | What |
|---|---|
| `PINS` | 16 × `Pin(number, name, kind, function)`; `kind`: `power`, `gnd`, `ref`, `ain`, `aout`, `uart`, `spi`, `i2c` |
| `NOTES` | wiring notes |
| `render(colour=True, width=0)` | the drawing as a string (compact on narrow terminals) |
| `use_colour(stream)` | whether ANSI colours make sense on `stream` |

[← Timeline](12-timeline.md) · [Contents](README.md) · [Next: CLI →](14-cli.md)
