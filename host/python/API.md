# tramuntana — Python API reference

Everything the `tramuntana` package exposes, for scripts that drive a
Tramuntana board. The [README](README.md) has installation and a
walk-through, and [`examples/`](examples/) has runnable scripts. The same
text is in the docstrings, so `help(t.spi.txn)` or the editor's hover
shows it too.

- [Conventions](#conventions)
- [Connecting](#connecting)
- [Tramuntana — the board](#tramuntana--the-board)
- [t.spi — SPI master and sniffer](#tspi--spi-master-and-sniffer)
- [t.i2c — I2C master and sniffer](#ti2c--i2c-master-and-sniffer)
- [t.scope — oscilloscope](#tscope--oscilloscope)
- [t.awg — waveform generator](#tawg--waveform-generator)
- [t.pwm — PWM outputs](#tpwm--pwm-outputs)
- [t.logic — logic analyzer](#tlogic--logic-analyzer)
- [t.uart — UART bridge](#tuart--uart-bridge)
- [t.emu — emulated I2C/SPI device](#temu--emulated-i2cspi-device)
- [t.seq — sequencer](#tseq--sequencer)
- [Timeline — everything on one clock](#timeline--everything-on-one-clock)
- [Events and sniffers](#events-and-sniffers)
- [Result types](#result-types)
- [Errors](#errors)
- [tramuntana.pinout](#tramuntanapinout)
- [tramuntana.proto — the wire protocol](#tramuntanaproto--the-wire-protocol)

## Conventions

| What | How |
|---|---|
| Bus data | `bytes` (or anything `bytes()` accepts) in, `bytes` out |
| I2C addresses | 7-bit integers (`0x68`), never shifted |
| DIO masks | one bit per pin, bit i = DIOi (`0x80` = DIO7) |
| Voltages | volts as floats (`1.65`), 0..3.3 V on every analog pin |
| Frequencies and rates | Hz / samples per second |
| Capture sizes | samples (per channel for the scope) |

Every method is one or a few commands of the binary protocol
([`fw/apps/tramuntana_fw/README.md`](../../fw/apps/tramuntana_fw/README.md#binary-api)).
Methods return once the board has answered. The exceptions are the capture
`arm()` calls and the sniffers: they return at once and the board keeps
working in the background.

**Header pins** (`tramuntana pinout` draws them):

| DIO | J1 | Bus function | | DIO | J1 | Bus function |
|---|---|---|---|---|---|---|
| DIO0 | 9 | UART RX | | DIO4 | 13 | SPI MOSI |
| DIO1 | 10 | UART TX | | DIO5 | 14 | SPI SCK |
| DIO2 | 11 | SPI CS | | DIO6 | 15 | I2C SCL |
| DIO3 | 12 | SPI MISO | | DIO7 | 16 | I2C SDA |

Every DIO is also a plain GPIO and a logic analyzer channel. The analog
pins are J1.3 AIN0, J1.5 AIN1 and J1.7 AOUT0, and J1.1 is the switched
3.3 V rail for the target.

## Connecting

```python
from tramuntana import Tramuntana, find_ports

print(find_ports())
# [{'serial': 'FA6418C89B869936', 'shell': '/dev/ttyACM0',
#   'api': '/dev/ttyACM1', 'uart': '/dev/ttyACM2'}]

with Tramuntana() as t:                        # first board found
    ...
with Tramuntana(serial_number="FA6418C89B869936") as t:   # a specific one
    ...
```

| Function | Returns |
|---|---|
| `find_ports()` | one dict per board: `serial` (USB serial number = chip id) and the `shell`, `api` and `uart` device paths |
| `find_api_port(serial_number=None)` | the API port path; `LinkError` if no board matches |

## Tramuntana — the board

```python
Tramuntana(port=None, serial_number=None, timeout=2.0)
```

| Argument | Meaning |
|---|---|
| `port` | API serial port; `None` finds it by USB id |
| `serial_number` | pick this board when several are plugged in |
| `timeout` | seconds to wait for each reply |

Opening checks the port with a PING. `LinkError` means no board was found
or the port is not a Tramuntana API port. Use the object as a context
manager, or call `close()`.

A background thread reads the port and every request goes through one
lock, so a `Tramuntana` can be shared between threads.

Attributes: `spi`, `i2c`, `scope`, `awg`, `pwm`, `logic`, `uart`, `emu`, `seq` (below),
`port` (the device path), `proto_version`, `link` (the
[`Link`](#tramuntanaproto--the-wire-protocol)).

| Method | What it does | Returns |
|---|---|---|
| `info()` | firmware and board identity | `{fw_version, board, sysclk_hz, chip_id, features, proto_version}` |
| `status()` | snapshot of the whole board, see below | dict |
| `power(on=None)` | switch the target's 3.3 V rail on J1.1; `None` only reads | state after the call |
| `pullups(on=None)` | switch the board's I2C pull-ups on DIO6/DIO7; `None` only reads | state after the call |
| `led(r=None, g=False, b=False)` | RGB LED: `led(True, False, False)` = red; `led()` = back to automatic | — |
| `adc(channel=0)` | one reading of AIN0 (0) or AIN1 (1) | volts; `ProtoError` busy while a scope capture uses the ADC |
| `dio_dir(mask, out_mask)` | make the pins in `mask` plain GPIO; 1 in `out_mask` = output | — ; busy if a pin is held exclusively (UART bridge) |
| `dio_write(mask, value)` | drive the output pins in `mask` | — ; not permitted if a pin isn't a plain GPIO |
| `dio_read()` | levels of DIO7..DIO0, whoever drives them | int, bit i = DIOi |
| `dio_watch(mask)` | report every edge on these DIOs as a [`DioEdge`](#events-and-sniffers) event, with the board's microsecond time; `0` stops. For control lines (reset, IRQ, LEDs): a pin toggling faster than ~50 edges/ms is muted for 100 ms | — |
| `time_us()` | the board clock every event timestamp uses, in µs (32 bits, wraps after ~71 min) | int |
| `event_stamps(on)` | make sniffer events carry the board time of their START / CS edge too (`t_us` field); a `Timeline` turns it on | — |
| `display(text)` | up to 24 ASCII characters on the dashboard's note row | — |
| `display_pinout(seconds=60)` | draw J1 on the board's screen; `None` = until the next call, `0` = back to the dashboard | — |
| `events(timeout=0.0)` | decoded events, see [Events](#events-and-sniffers) | generator |
| `on_event(callback)` | raw event callback `fn(cmd, payload)`, or `None` | — |
| `bootloader()` | reboot into the UF2 bootloader (the port goes away) | — |
| `close()` | stop the reader thread, close the port | — |

`status()` returns:

| Key | Value |
|---|---|
| `pins` | owner of DIO0..DIO7: `"gpio"`, `"spi"`, `"i2c"`, `"uart"`, `"pio"`, `"pwm"` or `"emu"`, with `!` appended when held exclusively |
| `power`, `pullups` | bools |
| `scope`, `logic` | capture state (see [captures](#tscope--oscilloscope)) |
| `awg`, `spi_sniff`, `i2c_sniff`, `uart_bridge` | bools: running / active |
| `rx_dropped` | API bytes the board had to drop (0 unless the host floods it) |

**Pin ownership.** Instruments that *drive* pins take them over as needed.
The SPI and I2C masters and plain GPIO hold theirs "softly": anyone else can
take them. The UART bridge holds DIO0/DIO1 exclusively while it is active.
The sniffers and the logic analyzer never take pins, so they can watch
anything, this board's own traffic included.

## t.spi — SPI master and sniffer

DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK. It works like SPIDriver: CS is
driven by hand, so a transaction can be built from several calls. `txn()`
does a whole transaction in one request. Words are 8 bits.

| Method | What it does | Returns |
|---|---|---|
| `config(freq_hz=1_000_000, mode=0, lsb_first=False)` | clock (≥ 1 kHz, clamped to 75 MHz), mode 0..3, bit order | frequency in use |
| `sel()` / `unsel()` | CS low / high | — |
| `write(data)` | send, CS untouched, MISO ignored; any length | — |
| `read(n, fill=0x00)` | read `n` bytes sending `fill`, CS untouched; any length | bytes |
| `xfer(data)` | full duplex, CS untouched; any length | bytes clocked in |
| `txn(data, read_n=0)` | CS low, write `data` (≤ 4096), read `read_n` (≤ 4096), CS high | the `read_n` bytes |
| `sniff(on=True, mode=None)` | passive sniffer on the same pins; `mode=None` = the master's mode | — |

`write`, `read` and `xfer` split long data into 4096-byte requests. With
the firmware's polled SPI driver, bulk transfers reach about 150 kB/s
whatever the clock is.

```python
t.spi.config(8_000_000, mode=0)
jedec = t.spi.txn(b"\x9f", read_n=3)                 # flash JEDEC id
t.spi.sel(); t.spi.write(b"\x03\x00\x00\x00"); page = t.spi.read(256); t.spi.unsel()
```

## t.i2c — I2C master and sniffer

DIO6 SCL, DIO7 SDA. It works like I2CDriver: the bus is bit-banged and
open-drain, 1 kHz..1 MHz, and it honours clock stretching up to 10 ms.
The bus needs pull-ups: the target's own, or the board's (`config(...,
pullups=True)` or `t.pullups(True)`).

| Method | What it does | Returns |
|---|---|---|
| `config(hz=100_000, pullups=True)` | bus speed and the board's pull-ups | frequency |
| `scan()` | probe 0x08..0x77 | list of addresses that ACKed |
| `write(addr, data)` | START, addr+W, data (≤ 4096), STOP | bytes ACKed; `ProtoError` on NAK |
| `read(addr, n)` | START, addr+R, `n` bytes (≤ 4096, NAK on the last), STOP | bytes |
| `write_read(addr, data, n)` | write (≤ 255 bytes), repeated START, read `n` | bytes |
| `reg_read(addr, reg, n=1)` | `write_read(addr, [reg], n)` | bytes |
| `reg_write(addr, reg, data)` | `write(addr, [reg] + data)` | bytes ACKed |
| `start(addr, read=False)` | START (repeated if open) + address byte; leaves the bus open | `True` if ACKed; a NAK does not raise |
| `raw_write(data)` | bytes on the open bus | bytes ACKed; NAK raises |
| `raw_read(n, nack_last=True)` | bytes from the open bus; `nack_last=False` to keep reading | bytes |
| `stop()` | STOP | — |
| `reset_bus()` | 9 SCL pulses + STOP to free a slave holding SDA | — |
| `sniff(on=True, pullups=False)` | passive sniffer on DIO6/DIO7 | — |

Failures come as `ProtoError`: status 4 for a NAK, 3 (timeout) when SCL
stays low (no pull-ups, or a target without power pulling the lines down),
and 6 for `raw_write`/`raw_read` without an open bus.

```python
t.i2c.config(400_000, pullups=True)
print([hex(a) for a in t.i2c.scan()])
who = t.i2c.reg_read(0x68, 0x75)                     # WHO_AM_I
t.i2c.start(0x50); t.i2c.raw_write(b"\x00\x10"); t.i2c.start(0x50, read=True)
data = t.i2c.raw_read(4); t.i2c.stop()               # EEPROM random read
```

## t.scope — oscilloscope

The scope samples AIN0 (J1.3) and AIN1 (J1.5) at 12 bits over 0..3.3 V.
It reaches 500 kS/s in total, so two channels get 250 kS/s each, and holds
up to 16384 samples in total. The trigger is found in a ring buffer, so a
capture can keep pre-trigger samples.

| Method | What it does | Returns |
|---|---|---|
| `capture(**arm_args)` | `arm` + `wait` + `read` | [`ScopeCapture`](#scopecapture); `LinkError` on timeout/abort |
| `arm(n=1024, channels=1, rate_hz=None, trigger="none", level=1.65, pre=0, timeout_ms=5000)` | start and return at once | actual rate per channel |
| `status()` | `{state, channels, n, rate_hz, trigger_index}` (`rate_hz` = total) | dict |
| `wait(timeout=10.0)` | poll until the capture finishes | final status; `LinkError` if still running |
| `read()` | download a finished capture | `ScopeCapture`; `LinkError` if not done |
| `abort()` | stop an armed/running capture | — |

`arm()` arguments:

| Argument | Meaning |
|---|---|
| `n` | samples per channel; `n * channels` ≤ 16384 |
| `channels` | 1 = AIN0, 2 = AIN0 + AIN1 (sampled alternately) |
| `rate_hz` | per channel; `None` or above the maximum = maximum. The 48 MHz ADC clock is divided by an integer ≥ 96, so the rate gets rounded |
| `trigger` | `"none"`, `"rising"` or `"falling"`, always on AIN0 |
| `level` | trigger level, volts |
| `pre` | samples kept before the trigger (≤ `n`) |
| `timeout_ms` | the state becomes `"timeout"` if the trigger never comes |

**Capture states** (scope and logic): `"idle"`, `"armed"` (waiting for the
trigger), `"running"`, `"done"`, `"timeout"`, `"aborted"`.

Use `arm()` / `wait()` / `read()` instead of `capture()` when something
else has to happen while the capture is armed:

```python
t.scope.arm(n=2000, rate_hz=100_000, trigger="rising", level=1.0, pre=200)
t.awg.step(0.5, 2.5, delay_ms=5)                     # the edge to catch (AOUT0 wired to AIN0)
t.scope.wait()
cap = t.scope.read()
print(cap.stats())
```

## t.awg — waveform generator

The generator drives AOUT0 (J1.7) through a PWM and an RC filter, over
0..3.3 V and 1 Hz..100 kHz. The output is `offset + amp × shape(t)`, with
the shape in −1..1, clipped to 0..3.3 V. A DMA loop keeps it running while
the rest of the board is in use, until `off()` or the next shape.

| Method | What it does | Returns |
|---|---|---|
| `set(shape, freq_hz, amp=1.0, offset=1.65, param=0)` | periodic waveform, see below | `{freq_hz (achieved), table, top}` |
| `sine(freq_hz, amp=1.0, offset=1.65)` | `set("sine", ...)` | same |
| `square(freq_hz, amp=1.0, offset=1.65, duty=50)` | `set("square", ..., duty)` | same |
| `arbitrary(samples, freq_hz, amp=1.65, offset=1.65)` | load one period (≤ 4096 values in −1..1, 8-bit) and play it | same |
| `dc(volts)` | constant level | — |
| `step(from_v, to_v, delay_ms)` | `from_v`, wait, then `to_v` | — |
| `off()` | stop | — |
| `status()` | `{running, shape, freq_hz, amp, offset}` | dict |

`set()`: `shape` is `"sine"`, `"tri"`, `"saw"`, `"square"`, `"stairs"` or
`"arb"` (the last table loaded). `freq_hz` is rounded to an integer. `amp`
is the peak amplitude (half of peak-to-peak), and voltages have 1 mV
resolution. `param` is the duty cycle in % for `square` (0 or > 100 means
50), and the number of steps (2..256, default 8) for `stairs`.

`step()` waits on the board, so the call returns after the step. The delay
has to fit in the link timeout (2 s by default, `Tramuntana(timeout=...)`).

## t.pwm — PWM outputs

Hardware PWM on any DIO. The pairs DIO7/6, DIO5/4, DIO3/2 and DIO1/0 each
share one PWM slice, so the two pins of a pair have the same frequency and
phase, each with its own duty and polarity: up to four independent
frequencies, ~10 Hz – 75 MHz.

| Method | What it does | Returns |
|---|---|---|
| `set(dio, freq_hz, duty=50.0, invert=False)` | start or update one pin (duty in %, 0.01 steps) | `{freq_hz (achieved), level, top, div}`; busy if the partner runs at another frequency |
| `phase(dio, degrees)` | phase delay of the pin's pair, applied by `sync()` | — |
| `sync(dio_mask=0)` | restart those pairs (0 = all live) on the same clock edge | — |
| `stop(dio_mask=0xFF)` | stop, pins back to inputs | — |
| `status()` | 8 dicts (DIO0 first): `active`, `lost`, `invert`, `freq_hz`, `actual_hz`, `duty`, `phase` | list |

```python
t.pwm.set(4, 10_000, 25)
t.pwm.set(3, 10_000, 50); t.pwm.phase(3, 90)
t.pwm.sync()
t.pwm.stop()
```

## t.logic — logic analyzer

The analyzer captures 8 channels, DIO0..DIO7, into one byte per sample
(bit i = DIOi). It samples at up to 150 MS/s and holds up to 131072
samples. It only listens.

| Trigger | Max rate | Max samples | Pre-trigger |
|---|---|---|---|
| `"none"` | 150 MS/s | 131072 | — |
| `"rising"` / `"falling"` on `pin` | 150 MS/s | 131072 | — |
| `"pattern"`: `(sample & mask)` becomes `== value` | 15 MS/s | 122880 | yes |

| Method | What it does | Returns |
|---|---|---|
| `capture(**arm_args)` | `arm` + `wait` + `read` | [`LogicCapture`](#logiccapture); `LinkError` on timeout/abort |
| `arm(n=4096, rate_hz=150e6, trigger="none", pin=0, mask=0, value=0, pre=0, timeout_ms=5000)` | start and return at once | actual rate |
| `status()` | `{state, n, rate_hz, trigger_index}` | dict |
| `wait(timeout=10.0)` | poll until the capture finishes | final status; `LinkError` if still running |
| `read()` | download a finished capture | `LogicCapture`; `LinkError` if not done |
| `abort()` | stop an armed/running capture | — |

The rate is 150 MHz divided by an integer (`rate_hz` gets rounded). `n` is
rounded up to a multiple of 4. `pre` applies to the pattern trigger only.
The pattern trigger fires when the pins *enter* the pattern: a sample that
does not match has to come first, so a pattern already true when the
capture is armed waits for the next time it starts.
The host rejects impossible pattern captures before sending them
(`ValueError`: rate above 15 MS/s, `mask` 0, more than 122880 samples).

The analyzer, the SPI sniffer and the I2C sniffer share the capture
memory, so only one of them runs at a time; the others get busy.

To see this board's own traffic, arm the analyzer, generate the traffic,
then read:

```python
t.logic.arm(n=20000, rate_hz=10e6, trigger="pattern", mask=0x80, value=0x00, pre=200)
t.i2c.scan()                                         # SDA (DIO7) falls at the START
t.logic.wait()
cap = t.logic.read()
cap.to_vcd("scan.vcd")                               # open in PulseView
```

## t.uart — UART bridge

The bridge connects the board's third serial port to the target's UART:
DIO1 is TX (to the target's RX) and DIO0 is RX (from the target's TX). The
data itself does not go through this object. Open the bridge port with
pyserial or a terminal; its baud rate, parity and stop bits are applied to
the target side.

| Method | What it does | Returns |
|---|---|---|
| `config(mode="auto", baud=0)` | `"auto"`: active while a program holds the port open; `"on"` / `"off"` force it. `baud` ≠ 0 overrides the port's rate | — |
| `status()` | `{active, baud, rx_bytes, tx_bytes}` (counters since boot), plus `mode`, `ctrl`, `reset_dio`, `boot_dio`, `dtr`, `rts`, `reset_low`, `boot_low` | dict |
| `ctrl(mode="esp", reset_dio=None, boot_dio=None)` | drive two DIOs from the bridge port's RTS/DTR, see below | — |

```python
import serial
port = find_ports()[0]["uart"]
with serial.Serial(port, 115200, timeout=1) as u:    # opening activates the bridge
    u.write(b"help\r\n")
    print(u.read(256))
```

While the bridge is active it holds DIO0/DIO1 exclusively, and `dio_dir()`
on them gets busy. SPI and I2C keep working alongside.

**Control lines.** `ctrl()` makes the bridge port's RTS and DTR drive two
DIOs as open-drain outputs (asserted = pulled low, deasserted = released
to the target's pull-up), like the RTS#/DTR# pins of a USB-serial adapter.
Then tools that reset a target and select its bootloader through those
lines work through the bridge unchanged:

| `mode` | reset (RTS) | boot (DTR) | For |
|---|---|---|---|
| `"esp"` | low only when RTS and not DTR | low only when DTR and not RTS | the two-transistor auto-reset circuit of ESP32/ESP8266 boards; esptool's default reset. Both asserted (what opening a port does) changes nothing |
| `"direct"` | low while RTS | low while DTR | an adapter wired straight to active-low RESET/BOOT pins, driven by your own program. Opening the port asserts both, so the target stays in reset until the program drops RTS |
| `"off"` | released | released | — |

```python
t.uart.ctrl("esp", reset_dio=3, boot_dio=2)        # DIO3 -> EN, DIO2 -> GPIO0
# esptool --port /dev/ttyACM2 flash_id             # resets into the ROM loader by itself
```

While control lines are on, the bridge stays active whether or not the
port is open (DTR is a signal now, not "port open"). Every change of the
host's DTR/RTS is reported as a `LineChange` event.

`UartTap(t, baud, ...)` in `tramuntana.timeline` opens the bridge port
and reads it continuously in the background: `read(timeout, until,
max_bytes)`, `write(data)`, `set_lines(dtr, rts)`, `close()`. With a
`Timeline` it records the target's output on it.

## t.emu — emulated I2C/SPI device

The board answers on the target's bus as a device backed by a 256-byte
register file, so a firmware can be tested against a sensor that returns
whatever the test needs - or that isn't there. One emulator at a time; it
holds its pins exclusively (owner `"emu"`).

Register model: a pointer that auto-increments on every byte. Reads return
`regs[ptr++]`, writes store into `regs[ptr++]` unless the register is
read-only (the write is still ACKed). The register file survives `stop()`.

| Method | What it does |
|---|---|
| `i2c(address, scl=6, sda=7, pullups=False)` | answer at a 7-bit address. The first written byte sets the pointer; a read starts at the pointer. The board stretches SCL after each byte while it decides, so the target's master must allow clock stretching. Tested at 100 kHz, 400 kHz and 1 MHz |
| `spi(mode=0, cs=2, sck=5, mosi=4, miso=3, addr_mask=0x7F, read_mask=0x80, read_value=0x80, status=0)` | answer as a SPI device. The first byte of each CS-low window is the command: a read of register `cmd & addr_mask` when `cmd & read_mask == read_value` (MISO then returns up to 32 registers from there), otherwise the following bytes are written. MISO carries `status` during the command byte. Pins are named from the target's side, so the wiring is the same as for the sniffer |
| `stop()` | stop and release the pins |
| `set_regs(offset, data)` / `regs(offset=0, n=256)` | load / read back registers (also while running) |
| `read_only(registers)` | registers whose writes are ignored; `[]` = all writable |
| `status()` | `{bus, transfers, bytes_read, bytes_written, naks}` |

Every transfer is reported as an [`EmuTransfer`](#events-and-sniffers)
event (what the target read or wrote, from which register).

```python
t.emu.set_regs(0xD0, b"\x60")                 # BME280 chip id
t.emu.set_regs(0xF7, bytes.fromhex("80 00 00 80 00 00"))
t.emu.read_only([0xD0])
t.emu.i2c(0x76)                                # on DIO6/DIO7
...                                            # let the target run
print(t.emu.regs(0xF4, 2).hex())               # the configuration it wrote
```

SPI limits: the first response bit has to be on MISO half a clock after
the command byte. A DMA lookup does it without the CPU, which holds up to
8 MHz in modes 0/2 and about 2 MHz in modes 1/3. Keep CS high for at least
10 µs between transactions.

## t.seq — sequencer

A list of steps the board runs on its own, with microsecond timing between
them. A request from the host costs a USB round trip plus the host's
scheduling, so "release reset, then read the sensor 2 ms later" from a
script lands anywhere from 1 to 20 ms; from the sequencer it lands within
microseconds. Every executed step leaves a record with the board time at
which it finished.

```python
from tramuntana import Sequence

s = (Sequence()
     .dio_dir(1 << 3, 1 << 3)                       # DIO3 = target RESET
     .dio_write(1 << 3, 0).delay_us(100).dio_write(1 << 3, 1 << 3)
     .wait_dio(1 << 0, 0, timeout_us=500_000)       # until its TX (DIO0) moves
     .i2c_write_read(0x76, b"\xd0", 1))
r = t.seq.run(s)
print(r.state)            # "done", "failed", "timeout", "aborted" or "invalid"
for step in r.steps:
    print(step)           # +    362 us  wait_dio mask 0x01 == 0x00   ok  -> 214
```

`Sequence` methods (each returns the sequence, so they chain;
`keep_going=True` records a failure and carries on instead of stopping):

| Step | Record value |
|---|---|
| `delay_us(us)`, `delay_ms(ms)` | — (busy wait below 2 ms; each delay step adds ~14 µs below 1 ms, ~20 µs up to 2 ms, ~37 µs above — see [docs/11](../../docs/11-sequencer.md#116-timing-and-side-effects)) |
| `power(on)`, `pullups(on)` | — |
| `dio_dir(mask, out_mask)`, `dio_write(mask, value)` | — |
| `dio_read()` | levels |
| `wait_dio(mask, value, timeout_us)` | µs waited (timeout = failure) |
| `i2c_write(addr, data)`, `i2c_read(addr, n)`, `i2c_write_read(addr, write, n)` | bytes ACKed / bytes read |
| `spi_txn(write, read_n)` | bytes read |
| `uart_send(data)` | — (the bridge must be active) |
| `awg_dc(volts)`, `adc_read(channel)` | — / volts |
| `mark(id)` | the id (a timestamp to measure against) |
| `watch(mask)` | — (as `dio_watch`) |
| `loop(count)` ... `end_loop()` | — (one level) |
| `logic_arm(...)`, `scope_arm(...)` | sample rate; read the capture afterwards |

`t.seq.run(seq, timeout_s=10)` uploads, runs and waits, and returns a
`SeqResult` (`state`, `error`, `pc`, `steps`, `ok`). Each `SeqStep` has
`op`, `label`, `ok`, `status`, `t_us` (board time), `dt_us` (since the
first step), `data` and `value` (decoded). `start()`, `status()`,
`result()` and `abort()` run it asynchronously. A program is up to 2 KB
and a run records up to 4 KB of results.

## Timeline — everything on one clock

`Timeline` collects what the board sees into one time-ordered list: the
target's UART output, sniffed I2C/SPI messages, edges on watched DIOs,
emulator transfers, control-line changes, sequencer runs, plus host-side
notes and marks. Cause and effect line up:

```python
from tramuntana import Timeline

tl = Timeline(t, dio_names={3: "RESET"})
tl.start(watch=1 << 3)          # record DIO3 edges
tap = tl.open_uart(115200)      # the target's console, read in the background
t.i2c.sniff(True)
tl.note("power on"); t.power(True)
time.sleep(2)
print(tl.format())
#   0.000102  host    power on
#   0.012311  dio     RESET ^  (levels 00001001)
#   0.150204  uart    Booting v1.2...
#   0.153190  i2c     S 76W- P
#   0.154087  uart    sensor not found
```

| Method | What it does |
|---|---|
| `start(watch=0, clear=True)` | start recording; `watch` = DIO mask for edges; turns on `event_stamps` |
| `watch(mask)` | change the watched DIOs |
| `open_uart(baud, ...)` | open the bridge port with a `UartTap` that feeds the timeline |
| `feed_uart(data)` | bytes from the target's UART, if you read the port yourself |
| `note(text)`, `mark(label)` | host-side entries |
| `select(since, until, sources, contains, last)` | entries as `Entry(t, source, text, board_us, obj)` |
| `format(**same)` | the same as text |
| `stop()` | stop recording (entries stay) |

Times are seconds from `start()`. Board-stamped entries (DIO edges,
emulator transfers, control lines, sequencer runs, sniffer messages) keep
the board's microsecond clock, mapped onto the host's with a periodic
`time_us()` sync. UART lines and host entries carry the host time at which
they arrived or happened: about a millisecond late, plus the UART receive
FIFO's grouping (up to ~1.5 ms at 115200 baud, more at lower rates). Sources: `uart`,
`uart_tx`, `i2c`, `spi`, `dio`, `emu`, `line`, `seq`, `capture`, `host`,
`mark`.

## Events and sniffers

The board sends events on its own: sniffed messages and finished
captures. The reader thread queues the last 1000.

```python
t.i2c.sniff(True)
for msg in t.events(timeout=5):          # stops after 5 s with no events
    print(msg)                           # I2C S 68W+ 75+  S 68R+ 71- P
t.i2c.sniff(False)
```

`events(timeout)` yields decoded events and waits up to `timeout` seconds
for each next one. With `timeout=0` it only drains the queue. What it
yields:

| Event | Yielded as |
|---|---|
| SPI transaction (CS low..high) | [`SpiMessage`](#spimessage) |
| I2C message (START..STOP) | [`I2cMessage`](#i2cmessage) |
| edge on a watched DIO | `DioEdge(t_us, levels, changed, lost)` |
| transfer with the emulated device | `EmuTransfer(t_us, bus, read, address, reg, data, truncated)` |
| host changed DTR/RTS on the bridge port | `LineChange(t_us, dtr, rts)` |
| sequencer run finished | `SeqDone(t_us, state, error, pc, result_len)` |
| capture finished | `{"event": "capture_done", "instrument": "scope" \| "logic", "state": ...}` |
| anything else | `{"event": cmd, "payload": bytes}` |

`on_event(fn)` calls `fn(cmd, payload)` with every raw event as it
arrives, from the reader thread. Keep it short and don't send requests
from inside it. `t.link.add_listener(fn)` adds more such callbacks (a
`Timeline` uses one), so several consumers see every event.
`decode_event(cmd, payload)` turns any raw event into the types above;
`decode_spi_event(payload)` and `decode_i2c_event(payload)` the sniffer
messages. `t_us` on events is the board clock (`time_us()`).

The SPI sniffer works up to about 15 MHz SCK. Both sniffers can watch a
target's master or this board's own.

## Result types

### ScopeCapture

| Member | Meaning |
|---|---|
| `channels` | 1 or 2 |
| `rate_hz` | samples/s per channel |
| `trigger_index` | index of the trigger sample in each channel (= `pre`; 0 without a trigger) |
| `raw[ch][i]` | 12-bit codes, 0..4095 = 0..3.3 V |
| `n` | samples per channel |
| `volts(ch=0)` | list of volts |
| `times()` | seconds relative to the trigger, one per sample |
| `stats(ch=0)` | `{min, max, vpp, mean, rms}` in volts and `freq` in Hz (mean-crossing estimate, `None` if it can't tell) |
| `to_csv(path)` | columns `t`, `ch0`[, `ch1`] in seconds and volts |

### LogicCapture

| Member | Meaning |
|---|---|
| `rate_hz` | samples/s |
| `trigger_index` | trigger sample (= `pre` for a pattern trigger, else 0) |
| `samples` | `bytes`, one per sample, bit i = DIOi |
| `n` | number of samples |
| `channel(dio)` | list of 0/1 levels of one pin |
| `edges(dio)` | `[(sample_index, new_level), ...]` |
| `to_vcd(path, names=None)` | VCD with a 1 ps timescale; `names` = 8 signal names, DIO0 first |
| `to_csv(path)` | columns `t`, `DIO0`..`DIO7` |

### SpiMessage

`total` (bytes in the transaction), `mosi` and `miso` (the first ≤ 256 bytes
of each direction), `overrun` (data was lost), `t_us` (board time of CS
falling, with `event_stamps(True)`; else `None`). `str()` gives
`SPI 4B  MOSI 9f 00 00 00  MISO 00 ef 40 18`.

### I2cMessage

`frames` (one [`I2cFrame`](#i2cframe) per START / repeated START),
`overrun`, `truncated`, `t_us` (board time of the START, with
`event_stamps(True)`; else `None`). `str()` gives
`I2C S 68W+ 75+  S 68R+ 71- P` (`+` ACK, `-` NAK).

### I2cFrame

`address` (7-bit), `read` (R/W bit), `ack` (address ACKed; `None` if the
bus stopped first), `data` (`[(byte, acked), ...]`).

## tramuntana.decode — protocol decoders and measurements

Host-side analysis of captures already downloaded, so it can be re-run with
other settings without capturing again. Unlike the firmware sniffers it works
on any DIO. Sample at 4x the bit/clock rate or more.

```python
from tramuntana import Tramuntana, decode

with Tramuntana() as t:
    lc = t.logic.capture(n=65536, rate_hz=2e6, trigger="falling", pin=0)
    baud = decode.guess_baud(lc, pin=0)
    print(decode.uart_text(decode.uart(lc, pin=0, baud=baud)))
    for x in decode.i2c(lc):              # DIO6 SCL, DIO7 SDA
        print(x)                          # 12.5us S 68W+ 75+  S 68R+ 71- P
    sc = t.scope.capture(n=4096, trigger="rising", level=1.65, pre=512)
    print(decode.measure(sc, 0)["rise_time_s"])
```

| Function | Returns |
|---|---|
| `uart(cap, pin, baud, bits=8, parity="none", stop_bits=1, invert=False)` | `[UartFrame]`: `t_s`, `value`, `parity_error`, `framing_error` |
| `uart_text(frames)` | the characters as text, non-printables as `\xNN` |
| `guess_baud(cap, pin)` | nearest standard rate (within 6 %), else the raw estimate; `None` with too few edges |
| `i2c(cap, scl=6, sda=7)` | `[I2cTransfer]`: `t_s`, `message` (an [`I2cMessage`](#i2cmessage)), `stopped`, `partial_bits`, `scl_hz` |
| `spi(cap, cs=2, sck=5, mosi=4, miso=3, mode=0, lsb_first=False, word_bits=8)` | `[SpiTransfer]`: `t_s`, `mosi`, `miso` (lists of words), `partial_bits`, `sck_hz`, `complete`. `cs=None` decodes the whole capture as one transfer |
| `measure(cap, ch=0, min_amplitude=0.05)` | dict: `low`, `high`, `amplitude`, `overshoot_pct`, `undershoot_pct`, `rise_time_s`, `fall_time_s` (10-90 %, median), `freq_hz`, `period_s`, `duty_pct`, `first_rise_s`, `first_fall_s`, `rising_edges`, `falling_edges`, `sample_period_s`; `dc: True` and `mean` for a flat signal; `edge_at_resolution_limit` when an edge takes one sample |

Times (`t_s`, `first_*_s`) are seconds from the trigger.

## Errors

| Exception | When |
|---|---|
| `ProtoError` | the board answered with an error. `e.status` is the code below, `e.cmd` the command id |
| `LinkError` | no board, wrong port, no reply in time, or a capture that ended in timeout/abort (`capture()`, `read()`, `wait()`) |
| `ValueError` | arguments the host can reject by itself (logic pattern trigger limits) |
| `struct.error` | a value that doesn't fit its field (e.g. a negative voltage, more than 255 bytes in `write_read`) |

| `ProtoError.status` | Name | Typical cause |
|---|---|---|
| 1 | invalid argument | out-of-range value, data too long |
| 2 | busy | pins held exclusively, capture already running, capture memory in use, ADC in use |
| 3 | timeout | I2C SCL held low for over 10 ms |
| 4 | bus error / NAK | I2C device didn't ACK |
| 5 | not supported | — |
| 6 | not permitted | pin not in the right mode, I2C bus not open, reading an unfinished capture |
| 7 | no memory | — |
| 8 | unknown command | firmware older than the library |
| 9 | wrong state | — |

`tramuntana.proto.STATUS_NAMES` maps codes to the names above.

## tramuntana.pinout

The J1 header as data, used by `tramuntana pinout`.

| Name | What |
|---|---|
| `PINS` | list of 16 `Pin(number, name, kind, function)`; `kind` is `power`, `gnd`, `ref`, `ain`, `aout`, `uart`, `spi` or `i2c` |
| `NOTES` | wiring notes printed under the drawing |
| `render(colour=True, width=0)` | the drawing as a string; `width=0` uses the terminal width, and narrow terminals get a compact version |
| `use_colour(stream)` | whether ANSI colours make sense on `stream` |

## tramuntana.ai — AI agent setup

What `tramuntana ai install|uninstall` runs. Both work without a board.

| Function | Does |
|---|---|
| `install(target, project=None)` | register the MCP server with `target` (`"claude"`, `"opencode"`, `"copilot"` or `"all"`) and link the `tramuntana-debug` skill; `project=None` = for the user, else only in that directory. Returns the exit code |
| `uninstall(target, project=None)` | undo it; the skill stays while another agent still uses the server there |
| `server_command()` | how agents launch the server: `[sys.executable, "-m", "tramuntana.mcp_server"]` |
| `skill_source()` | the skill's folder inside the package |

The server itself is `tramuntana.mcp_server` (`tramuntana-mcp` script, `[mcp]`
extra); its tools are listed in [AI tools](../../docs/19-ai-tools.md#192-tools).

## tramuntana.proto — the wire protocol

For commands that have no method yet. `CMD_*` constants mirror
`fw/apps/tramuntana_fw/src/proto.h`, which is the reference for every
payload.

```python
from tramuntana import proto as P
r = t.link.request(P.CMD_ADC_READ, bytes([1]))       # payload in, reply data out
```

| Name | What |
|---|---|
| `Link.request(cmd, payload=b"", timeout=None)` | send one command and return the reply data (after the status byte); raises `ProtoError` / `LinkError` |
| `Link.events` | the event queue (`deque` of `(cmd, payload)`) |
| `Link.add_listener(fn)` / `remove_listener(fn)` | extra `fn(cmd, payload)` callbacks for every event |
| `encode(cmd, seq, payload)` | build a frame: `AA 55 \| cmd \| seq \| len u16 \| payload \| crc16` |
| `crc16(data)` | CRC-16/CCITT-FALSE, the frame checksum |
| `CAP_NAMES` | capture state names, index = state code |
| `PROTO_VERSION`, `MAGIC`, `MAX_PAYLOAD` | protocol constants |
