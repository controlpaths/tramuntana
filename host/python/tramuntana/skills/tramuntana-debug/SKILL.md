---
name: tramuntana-debug
description: Debug an embedded target (microcontroller board, sensor, flash, display, bus) with a Tramuntana board as the bench instrument - power it, probe I2C/SPI/UART, capture and decode signals, and find the root cause of a hardware/firmware fault. Use when the user asks to bring up, test, diagnose or reverse-engineer hardware wired to a Tramuntana, or when the tramuntana MCP tools are available and the problem is on real hardware ("the sensor doesn't answer", "nothing on the UART", "the flash reads 0xFF", "the board doesn't boot").
---

# Debugging embedded hardware with Tramuntana

The `tramuntana` MCP tools give you a bench: a switched 3.3 V rail, I2C/SPI
masters and sniffers, a UART bridge, 8 GPIOs, a 2-channel 500 kS/s scope, a
waveform generator and an 8-channel logic analyzer (up to 150 MS/s) that can
decode UART/I2C/SPI. On top of those:

- **Timeline** (`timeline_start` / `timeline_read`): the target's UART
  output, sniffed buses, edges on watched pins, emulator transfers and your
  own tool calls, recorded continuously on one clock. Your main window on
  what the system does over time.
- **Sequencer** (`sequence_run`): steps executed on the board with
  microsecond timing. Separate tool calls are milliseconds to seconds
  apart, so anything where timing matters goes here.
- **Emulated device** (`emu_i2c` / `emu_spi`): the board answers on the
  target's bus as a sensor/memory with registers you control.
- **Reset/boot control** (`uart_ctrl`): the bridge port's RTS/DTR drive the
  target's reset and boot pins, so the usual flashing tools work through it.

The J1 pinout comes with the MCP server instructions. This skill is about
*how* to use them to find a fault.

## Rules

1. **Know the wiring before acting.** Before powering anything or driving a
   pin, know what each J1 pin connects to on the target. Look for a target
   description in the project first (`tramuntana.md`, `tramuntana.toml`,
   `HARDWARE.md`, a README section, a schematic). If there is none, ask the
   user, and offer `display_pinout` so they can check the wires on the board.
   Write down what you learn in a sentence back to the user.
2. **Measure before you drive.** Start passive: `board_status`, `dio_read`,
   `adc_read`, `logic_capture`, sniffers. Only then switch power, drive DIOs
   or send bus traffic.
3. **Ask before risky actions.** Confirm with the user before:
   - `power(on)` when the target might also be powered from elsewhere
     (USB, battery, bench supply): both supplies would fight.
   - driving a DIO that the target may also drive (`dio_set_direction`
     output, `dio_write`, `awg_*` on a node with an output on it).
   - writing to non-volatile memory (SPI flash erase/program, EEPROM writes,
     OTP or fuse registers), and flashing the target's firmware.
   - `emu_i2c` / `emu_spi` / `uart_ctrl`: they drive pins. The emulator
     must not share a bus with the real device at the same address.
   - anything on a target that is not 3.3 V logic: the DIOs are 3.3 V, so a
     5 V target needs level shifting, and AIN is limited to 0..3.3 V.
4. **One hypothesis, one discriminating measurement.** State the hypothesis,
   pick the measurement whose result would confirm or kill it, run it,
   conclude. Change one thing at a time.
5. **Evidence, not guesses.** Quote the numbers you measured (levels, times,
   bytes) when you conclude. If a measurement can't resolve something (the
   scope's 2 us sample period vs. a 50 ns edge), say so instead of reading
   noise.
6. **Leave the bench safe.** When done: `awg_off`, stop sniffers, release
   GPIOs as inputs, `uart_close`, and switch the rail off unless the user
   wants the target left running. `display_text` can show what you're doing.

## Workflow

1. **Inventory**: `board_info`, `board_status` (who owns each pin, power,
   pull-ups). Read the target description or ask for the wiring.
2. **Start recording** before touching anything: `timeline_start` with the
   control lines you know of in `watch_dios` (reset, IRQ, enable, status
   LED) and their names, `uart_open` at the console's baud rate, and
   `sniff_start` on the bus you suspect. From now on nothing is missed
   between your calls.
3. **Power**: with the user's go-ahead, `power(on)`. Confirm the rail and the
   target's supply on AIN if wired (`adc_read`, or `scope_capture` with a
   rising trigger right before power-on to see the ramp and any brown-out).
4. **Is it alive?** `timeline_read`: boot log, bus traffic and pin activity
   since power-on, in order. Then `i2c_scan`, a SPI ID command (`9f` for
   JEDEC flash), `uart_send` to its console.
5. **Narrow down** with the playbook that matches the symptom (below).
   When a check needs precise timing (reset pulse width, "talk to it 2 ms
   after reset", time from reset release to the first UART byte), build it
   as one `sequence_run` - its step times are exact.
6. **Look at the wire** when the protocol layer gives no answer:
   `logic_capture(decode=...)` while generating the traffic, then
   `logic_decode` with other settings, then `logic_image` / `scope_image`
   to see the waveform.
7. **Report**: root cause, the evidence (quote the timeline lines), the fix (hardware or firmware), and
   if useful a short Python script with the `tramuntana` library that
   reproduces the check, so it can become a regression test.

## Watching this board's own traffic

The analyzer and the sniffers only listen, so they see what Tramuntana
itself sends. Pattern: `logic_capture(wait=false, trigger=..., decode=...)`,
then the `i2c_*` / `spi_*` / `uart_send` call, then `logic_read(decode=...)`.
Pick the sample rate at 10x the bus clock or more (e.g. 2 MS/s for 100 kHz
I2C, 10 MS/s for 1 MHz SPI, 2 MS/s for 115200 baud).

## Symptom playbooks

Read the reference file for the bus involved; each has the full checklist,
the expected waveforms and the usual root causes.

| Symptom | First moves | Reference |
|---|---|---|
| I2C device doesn't ACK / scan finds nothing / bus stuck low | `dio_read` idle levels, pull-ups, `i2c_scan`, `i2c_bus_reset`, capture + `decode="i2c"` | [references/i2c.md](references/i2c.md) |
| SPI reads 0xFF or 0x00, wrong ID, garbage | check CS and mode, capture + `decode="spi"`, try the 4 modes with `logic_decode` | [references/spi.md](references/spi.md) |
| No UART output, garbage, wrong characters | idle level on DIO0, `logic_capture(decode="uart")` to guess the baud, swap TX/RX | [references/uart.md](references/uart.md) |
| Doesn't boot, resets, brown-out, works sometimes | rail with the scope at power-on, reset/boot pin levels, UART boot log | [references/power-and-signals.md](references/power-and-signals.md) |
| Signal looks wrong (slow edges, ringing, glitches, wrong frequency/duty) | `scope_capture` measurements + `scope_image`, logic for timing | [references/power-and-signals.md](references/power-and-signals.md) |
| Unknown device / reverse engineering | sniff the target's own bus traffic (`sniff_start`, or logic + decode), identify by ID registers | [references/i2c.md](references/i2c.md), [references/spi.md](references/spi.md) |
| Firmware mishandles a device (bad values, missing device, slow device) or no device to test with | emulate it with chosen register values, then watch what the firmware does | [references/emulate-and-sequence.md](references/emulate-and-sequence.md) |
| Timing: reset/boot sequence, start-up order, "works with a delay" | timeline with the control lines watched, `sequence_run` to reproduce exactly | [references/emulate-and-sequence.md](references/emulate-and-sequence.md), [references/power-and-signals.md](references/power-and-signals.md) |
| Flash or re-flash the target | `uart_ctrl` + the vendor's serial tool, or the user's own flow | [references/uart.md](references/uart.md) |

## Reading the results

- `timeline_read` lines are `seconds  source  text`. Narrow them with
  `since_s`/`until_s` around the moment that matters (a `timeline_mark`
  before each experiment makes it easy to find), or with `sources` and
  `contains`. Order is reliable: board-stamped sources (i2c, spi, dio, emu,
  seq) to the microsecond, uart and host to about a millisecond.

- `scope_capture` returns `measure` per channel: `low`/`high` levels from the
  histogram, `rise_time_s`/`fall_time_s` (10-90 %), `freq_hz`, `duty_pct`,
  `overshoot_pct`. `edge_at_resolution_limit` means the edge is faster than one
  sample: the scope can't measure it, so use the logic analyzer for timing.
  Levels, overshoot and rise/fall time only mean something on pulse-like
  signals (digital lines, PWM, steps). For a sine or other analog waveform
  use `stats` (vpp, rms, mean) and `freq_hz`.
- `logic_capture` gives per-channel edges, duty and frequency, and `decoded`
  when asked. `count: 0` decoded with edges present means wrong pins, wrong
  baud/mode, or too low a sample rate, not "no traffic".
- I2C text: `S 68W+ 75+  S 68R+ 71- P` is START, address 0x68 write ACKed,
  byte 0x75 ACKed, repeated START, read ACKed, byte 0x71 NAKed by the master
  (normal on the last read byte), STOP. `with_nak` counts transfers with an
  unexpected NAK.
- Images cost context: zoom into a window (`t_from_us`/`t_to_us`) around the
  event you care about instead of plotting a whole long capture.
