# 17. Firmware internals

[← Binary protocol](16-binary-protocol.md) · [Contents](README.md) · [Next: Web application →](18-webapp.md)

This chapter is for people working on the firmware: how it is organised,
how each instrument uses the RP2350's hardware, and how to build, flash
and extend it. The firmware lives in
[`fw/apps/tramuntana_fw`](../fw/apps/tramuntana_fw); its own
[README](../fw/apps/tramuntana_fw/README.md) has more design notes and a list
of lessons learned.

## 17.1 Platform

| | |
|---|---|
| MCU | RP2350A (Raspberry Pi Pico 2), Cortex-M33 core 0 at 150 MHz |
| OS | Zephyr (board `rpi_pico2/rp2350a/m33`, plus an app-level devicetree overlay) |
| Hardware access | Zephyr drivers for USB, UART, SPI0 (master), ADC, display (SPI1 + ST7735R); the Pico SDK's `hardware/*.h` directly for PIO, DMA, PWM, GPIO and the ADC FIFO |
| Version | `FW_VERSION` in `src/version.h` (1.1.0); protocol version 1 |

## 17.2 Source layout

```
fw/
├── build.sh                       build / flash helper
├── boards/tramuntana_lite_r1a-common.dtsi   the board: header, power switch, LEDs, display, pull-ups
└── apps/tramuntana_fw/
    ├── CMakeLists.txt, prj.conf
    ├── boards/rpi_pico2_rp2350a_m33.overlay   3 CDC-ACM ports, capture arena, UART1, PIO1
    └── src/
        main.c        boot order
        usb.c/h       USB composite device: three CDC-ACM ports
        port.c/h      interrupt-driven, ring-buffered serial port (USB backpressure aware)
        proto.c/h     binary API: framing, dispatch table, events      ← protocol reference
        shell_cmds.c  shell commands
        board.c/h     LED, target power, pull-ups, DIO GPIO, ADC one-shot, cycle delays, pin arbiter, board clock
        display.c/h   ST7735R primitives, dashboard and pinout screens (own thread)
        arena.c/h     capture memory (scope ring + shared 128 KB ring)
        dmaring.c/h   endless circular DMA of any size (4-channel ping-pong)
        spi.c/h       SPI master (SPI0 + GPIO CS) and SPI sniffer (PIO0)
        i2c.c/h       bit-banged I2C master and I2C sniffer (PIO0)
        scope.c/h     ADC free-running into a DMA ring, software trigger
        awg.c/h       PWM + RC "DAC" fed by a self-restarting DMA loop
        pwm.c/h       PWM outputs on the DIOs
        logic.c/h     8-channel PIO logic analyzer
        bridge.c/h    USB <-> UART1 bridge, RTS/DTR control lines
        watch.c/h     timestamped DIO edges (GPIO interrupts)
        stamp.c/h     exact timestamps for sniffer messages (PIO0 IRQ)
        emu.c/h       emulated I2C/SPI target (PIO1 + DMA)
        seq.c/h       sequencer                                       ← program format reference
        version.h
```

## 17.3 Boot order and threads

`main()` brings up USB, the board (LEDs, power off, pull-ups off, all DIOs
as inputs, cycle counter), the display, the sniffer timestamp interrupt,
then each instrument, the bridge, the DIO watch, the emulator, the
sequencer, and finally the API thread. It shows the splash screen for
1.5 s and switches to the dashboard.

| Thread | Priority (lower = more urgent) | Job |
|---|---|---|
| `bridge` | 1 | move bytes between the USB port and UART1, follow line coding and DTR/RTS |
| `seq` | 2 | run sequencer programs (busy waits for µs timing) |
| `scope`, `logic` | 4 | watch the capture rings, find triggers, finish captures |
| `api` (proto) | 5 | read requests, dispatch them, send replies |
| sniffer decoders, `watch` | 6 | turn ring data / edge queues into events |
| `emu` | 7 | turn emulator transfer logs into events |
| display | 12 | render the dashboard from a snapshot |
| shell | Zephyr default | the console |

Interrupt handlers do the time-critical parts: the I2C/SPI emulator
(PIO1 IRQ 0), sniffer timestamps (PIO0 IRQ 0), DIO edges (GPIO IRQ), and
the serial ports' ring buffers.

The bridge sits above the sequencer on purpose: a long sequence of short
busy waits would otherwise hold up the target's bytes and DTR/RTS changes.
Event-producing threads below the sequencer just deliver late. Their
events carry board timestamps, so nothing is mis-timed.

## 17.4 Hardware resources

| Resource | Used by |
|---|---|
| SPI0 | SPI master (SCK/MOSI/MISO; CS is a GPIO) |
| SPI1 | display |
| UART1 (on GPIO6/7 via the UART_AUX function) | UART bridge |
| ADC + FIFO | scope (free-running) and one-shot readings (under a mutex) |
| PWM slice 4 (GPIO8) | waveform generator |
| PWM slices 0–3 | PWM outputs on the DIOs |
| PIO0 | logic analyzer, SPI sniffer, I2C sniffer (one at a time); IRQ flag 0 for timestamps |
| PIO1 | emulated I2C (2 state machines) or SPI (3 state machines) target |
| DMA | scope 1 channel, AWG 2, logic 1 (or 4 for the pattern trigger), sniffers 4 (ring), emulator 2 |
| GPIO interrupts | DIO watch |
| DWT cycle counter | precise delays (I2C bit-banging, sequencer) |

### Memory

The devicetree overlay shrinks Zephyr's SRAM to 320 KB and reserves
192 KB at `0x20050000` as the **capture arena**, aligned so the DMA can use
its address-wrapping mode:

| Address | Size | Use |
|---|---|---|
| `0x20050000` | 32 KB | scope ring (ADC samples) |
| `0x20060000` | 128 KB | shared ring: logic analyzer or one of the sniffers. A finished logic capture is "evictable": a sniffer that needs the ring takes it |

The SPI emulator's 8 KB response table is a normal, 8 KB-aligned array.

## 17.5 How the instruments work

**USB.** One composite device, one full-speed configuration, three CDC-ACM
instances (`usb.c`). Each port is wrapped by `port.c`: interrupt-driven
RX/TX rings with blocking read/write. On a full RX ring the CDC port stops
taking packets (the host waits) instead of dropping bytes.

**API.** One thread reads the API port, resynchronises on `AA 55`, checks
the CRC and calls the handler for the command id (`handlers[]` in
`proto.c`). Handlers write the reply straight into the transmit frame.
Replies and events share a mutex, so frames never interleave. Long jobs
(captures, sequences) are arm / status / read, so the port never blocks.

**Pin arbiter** (`board.c`). Every instrument that drives pins claims a DIO
mask with its owner id. Masters, GPIO and PWM are soft owners; the bridge
and the emulator are exclusive. The claim also sets each pad's function
(SIO, SPI, UART, PWM, PIO0, PIO1). SPI's CS is set as a high SIO output
before its function switch, so it never glitches.

**SPI master.** Zephyr's PL022 driver for shifting, CS as a GPIO (the PL022
pulses its own CS on every word). After a mode change, the next select
first clocks a dummy byte with CS high, so SCK sits at the new idle level
before CS falls.

**I2C master.** Bit-banged from RAM (`__ramfunc`), open-drain by switching
the pad direction, delays precomputed in cycles, clock stretching
honoured. The scheduler is locked during each operation so higher-priority
threads don't stretch individual clock phases.

**Sniffers.** A PIO0 state machine samples the bus (SPI: MOSI and MISO on
the sampling edge, a marker at CS rising; I2C: one word per bit, START or
STOP). DMA streams the words into the shared ring, and a thread decodes
them into events. At the message start, the PIO program raises IRQ flag 0;
`stamp.c` records the board time with the DMA write position, and the
decoder looks the stamp up by position (exact however late it runs).

**Scope.** The ADC free-runs (round-robin for two channels) into its FIFO,
and DMA streams it endlessly into the 32 KB ring. The scope thread scans
new samples for the trigger, waits for the post-trigger samples, stops, and
copies the window out. That gives pre-trigger, and no edge can fall
between arming and capturing.

**AWG.** A PWM at ~586 kHz behind the board's RC filter. One period of the
waveform is precomputed as compare values; a DMA channel feeds them paced
by the PWM wrap, and a second DMA channel restarts the first. No CPU
involvement while it runs.

**Logic analyzer.** A one-instruction PIO loop (`in pins, 8`) with
autopush: one sample per PIO clock, divider = rate. Edge triggers prefix
the loop with `wait` instructions (zero latency). The pattern trigger runs
the DMA ring endlessly while the CPU scans four samples per word, then
lets the post-trigger samples land.

**UART bridge.** A thread shuttles bytes between the CDC port and UART1,
applies the host's line coding, and follows DTR in `auto` mode. It reads
DTR/RTS every loop and drives the control-line pins (open-drain: output low
or input).

**DIO watch.** Zephyr GPIO callbacks on the watched pins. The ISR takes the
time and levels into a queue, and a thread sends `EVT_DIO`. More than 50
edges in a millisecond mutes the pins for 100 ms.

**Emulator.** I2C: a "bytes" state machine shifts 9-bit frames and holds
SCL low after each byte until the ISR answers (ACK/NAK + next byte). A
"conditions" state machine detects START/STOP and holds SCL after a START
until the ISR has restarted the byte machine. SPI: an rx machine captures
MOSI for the ISR, a "cs" machine flags CS rising, and a tx machine shifts
`status`, turns the command byte into a response-table row address and
pushes it. DMA channel A writes that address into channel B's
`READ_ADDR_TRIG`, and channel B streams the row into the tx FIFO. Writes
are applied, and the affected table rows rebuilt, at CS rising.

**Sequencer.** A thread interprets the bytecode (`seq.c`). Delays below
2 ms are busy waits on the cycle counter; longer ones sleep in slices.
The wait itself is cycle-accurate, but a delay step measures ~14 µs longer
than requested even with `delay_us(0)`, against ~4 µs for other steps, so
the overhead is in the step around the wait (see
[Sequencer timing](11-sequencer.md#116-timing-and-side-effects)).
`WAIT_DIO` busy-polls for 2 ms and then polls once per tick. Bus steps call
the same driver functions as the API.

**Display.** A low-priority thread redraws from a snapshot of the status
lines every 100 ms, so no instrument waits for the 30 ms SPI transfer of a
frame.

## 17.6 Building and flashing

Requirements: a Zephyr workspace (see Zephyr's
[getting started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)),
the Zephyr SDK, and `device-tree-compiler`. `fw/` is not its own west
workspace: `build.sh` builds an app against that installation. Their
locations go in `fw/.env`, which is not versioned:

| Variable | Default | Meaning |
|---|---|---|
| `ZEPHYRPROJECT` | `~/zephyrproject` | west workspace (contains `zephyr/`) |
| `ZEPHYR_SDK` | `~/zephyr-sdk-1.0.1` | SDK install (contains `cmake/`) |
| `WEST` | `$ZEPHYRPROJECT/.venv/bin/west` | west executable |
| `TRAMUNTANA_ACM_PORT` | `/dev/ttyACM0` | shell port used by `flash` |

Variables already set in the environment take precedence over the file.

```bash
cd fw
cp .env.example .env             # then edit the paths
./build.sh                       # builds apps/tramuntana_fw -> build/tramuntana_fw/zephyr/zephyr.uf2
./build.sh flash                 # build, reboot the board into its bootloader, copy the UF2
```

`build.sh flash` sends `boot` to the shell port and waits for the
`RP2350` drive. If that doesn't work on your machine, send the board to the
bootloader through the API (`tramuntana bootloader`) and copy
`zephyr.uf2` to the drive by hand. Close any program that has the board's
ports open first. A board whose firmware doesn't respond needs BOOTSEL
held while plugging it in.

The functional test in `host/python/examples/board_test` flashes the
firmware and checks every instrument with six jumper wires (see
[Examples](20-examples.md)).

## 17.7 Adding a command

1. **`proto.h`:** add `CMD_…` with an unused id below 0x70, and document
   its payload in the comment (that comment is the reference).
2. **The instrument:** implement the function in its module (or a new
   `.c/.h`, added to `CMakeLists.txt` and initialised in `main.c`).
3. **`proto.c`:** write a handler `h_…(p, len, r, rlen)`. Check `len`, call
   the function, map errors with `errno_to_status()`, write the reply data
   into `r` and its length into `*rlen`. Add it to `handlers[]`.
4. **`shell_cmds.c`:** a shell command calling the same function, if a
   human should reach it.
5. **Host side:** mirror the constant in `host/python/tramuntana/proto.py`
   (and `host/webapp/js/proto.js`), add a method in `device.py`, and if
   useful a CLI subcommand in `cli.py` and an MCP tool in
   `host/python/tramuntana/mcp_server.py`.
6. **Documentation:** the [protocol reference](16-binary-protocol.md), the
   [Python reference](13-python-library.md), and the relevant guide.

For a new event, use an id in 0xF0..0xFF and send it with
`proto_send_event()` from a thread (not an ISR: it takes the transmit
mutex). Decode it in `decode_event()` in `device.py`.

[← Binary protocol](16-binary-protocol.md) · [Contents](README.md) · [Next: Web application →](18-webapp.md)
