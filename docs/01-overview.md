# 1. Overview

[← Contents](README.md) · [Next: Getting started →](02-getting-started.md)

## What Tramuntana is

Tramuntana is the bench you plug between your computer and the embedded
system you are working on (the **target**). It powers the target, talks to
it over its buses, listens to what it says, measures its signals and
stimulates its inputs. All of this goes through one USB cable and a 16-pin
header (J1).

Two boards share the same design and software:

| Board | What it is |
|---|---|
| `tramuntana_lite_r1a` | a Raspberry Pi Pico 2 soldered onto a two-layer carrier. This is the workshop board, and the one the firmware targets today |
| `tramuntana_r1a` | the full board, with the RP2350 on the PCB |

## The instruments

Each instrument uses specific header pins. **DIO0..DIO7** are general
digital pins, each with one bus function. All eight are also GPIOs and
logic analyzer channels.

| Instrument | Pins | Highlights |
|---|---|---|
| SPI master | DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK | modes 0–3, up to 75 MHz, CS driven by hand so transactions can be built step by step |
| SPI sniffer | same pins, listening | captures MOSI *and* MISO for each CS-low window, up to ~15 MHz SCK |
| I2C master | DIO6 SCL, DIO7 SDA | 1 kHz – 1 MHz, START / bytes / repeated START / STOP as separate primitives, clock stretching, bus recovery |
| I2C sniffer | same pins, listening | decodes START, address, data, ACK/NAK, STOP |
| Oscilloscope | AIN0 (J1.3), AIN1 (J1.5) | 12-bit, 500 kS/s total, 16384 samples, rising/falling trigger with pre-trigger |
| Waveform generator | AOUT0 (J1.7) | 1 Hz – 100 kHz, 0..3.3 V, standard shapes, DC, step and 4096-point arbitrary waveforms |
| Logic analyzer | DIO0..DIO7 | 150 MS/s, 131072 samples, edge trigger in hardware, pattern trigger with pre-trigger |
| PWM | any DIO | 4 independent frequencies (pins in pairs), per-pin duty and polarity, synchronised phases |
| UART bridge | DIO1 TX, DIO0 RX | the target's console as the board's third serial port; baud rate and framing follow the host |
| Reset/boot control | any two of DIO2..DIO7 | the bridge port's RTS/DTR drive the target's reset and boot pins (esptool works through it) |
| Emulated device | I2C or SPI pins of your choice | the board answers as a sensor/memory with registers you load and read back |
| Sequencer | — | runs a list of steps (GPIO, waits, bus transfers, captures) on the board with µs timing |
| DIO watch | any DIO | timestamped events for every edge on control lines (reset, IRQ, LEDs) |
| Target power | J1.1 | switched, fused 3.3 V rail, off at power-up |
| I2C pull-ups | DIO6/DIO7 | switchable 4.7 kΩ pull-ups |

The **passive** instruments (sniffers, logic analyzer, DIO watch) only
listen. They need no pins of their own, so they can watch a bus while the
board's own master, the UART bridge or the target drives it. Arming the
logic analyzer and then running an I2C transaction captures this board's
own traffic.

## How the system is organised

```
                    ┌──────────────────────────── Tramuntana ─────────────────────────────┐
  your computer     │                                                                       │    target
 ┌──────────────┐   │  USB  ┌─────────────┐   ┌───────────────────────────────────────┐    │  ┌──────────┐
 │ shell        │◄──┼──────►│ ACM0 shell  │──►│                                       │    │  │          │
 │ (picocom)    │   │       ├─────────────┤   │   instruments: SPI, I2C, scope, AWG,  │ J1 │  │  MCU,    │
 │ CLI / Python │◄──┼──────►│ ACM1 API    │──►│   logic, PWM, emulator, sequencer,    │◄──►│  │  sensors,│
 │ webapp / AI  │   │       ├─────────────┤   │   DIO watch, power, pull-ups          │    │  │  memory  │
 │ terminal     │◄──┼──────►│ ACM2 bridge │──►│   UART1 ◄──► DIO0/DIO1                │    │  │          │
 └──────────────┘   │       └─────────────┘   └───────────────────────────────────────┘    │  └──────────┘
                    └───────────────────────────────────────────────────────────────────────┘
```

The board shows up as **one USB device with three serial ports**:

| Port (Linux name) | Interface | Purpose |
|---|---|---|
| `/dev/ttyACM0` | shell | a human console: type `help` |
| `/dev/ttyACM1` | binary API | framed request/reply protocol used by the CLI, the Python library, the webapp and the AI tools |
| `/dev/ttyACM2` | UART bridge | a plain serial port connected to the target's UART on DIO0/DIO1 |

The USB serial number is the RP2350's unique chip id, so
`/dev/serial/by-id/usb-controlpaths_Tramuntana_Lite_r1a_<id>-if00/02/04`
stays the same across reboots, and several boards can be told apart.

## Which interface should I use?

| You want to… | Use |
|---|---|
| poke at something interactively, no installation | the [shell](15-shell.md) (`picocom /dev/ttyACM0`) or the [webapp](18-webapp.md) |
| run a quick command from a terminal, save a capture to a file | the [`tramuntana` CLI](14-cli.md) |
| automate a test, process captures, decode buses | the [Python library](13-python-library.md) |
| see waveforms and logic traces with a mouse | the [webapp](18-webapp.md) |
| let an AI assistant debug a target for you | the [AI tools](19-ai-tools.md) |
| talk to the target's console | the bridge port (`picocom -b 115200 /dev/ttyACM2`) |

The CLI, the library, the webapp and the AI tools all speak the same
[binary protocol](16-binary-protocol.md), and the shell calls the same
firmware functions. Whatever one front end can do, the others can do too,
with one documented exception: the sequencer has no CLI command (use
Python, the webapp or the AI tools).

## What makes it useful for debugging

Beyond being a set of instruments in one box, a few features are meant for
working on a misbehaving system:

- **Everything on one clock.** DIO edges, sniffed messages, emulator
  transfers, control-line changes and sequencer steps carry the board's
  microsecond timestamp. The [timeline](12-timeline.md) merges them with
  the target's UART output, so you can see what the firmware printed next
  to what happened on the wires.
- **Exact timing when it matters.** The [sequencer](11-sequencer.md) runs
  "release reset, wait for the IRQ line, read the sensor" on the board
  itself. Between two steps there are microseconds, not the milliseconds of
  a USB round trip.
- **A device you control.** The [emulator](10-emulated-devices.md) lets you
  test how a firmware reacts to sensor values, missing devices or wrong
  IDs without touching real hardware.
- **Flash and reset through the bridge.** [Reset/boot
  control](09-uart-bridge.md#92-control-lines-resetboot) turns the bridge port
  into an auto-reset serial adapter, so the edit / build / flash / observe
  loop needs no other tool.
- **Machine-friendly results.** The Python [decoders](07-logic-analyzer.md#73-decoding-uart-i2c-and-spi)
  and the AI tools turn raw captures into frames and measurements, which is
  what an automated tester or an AI agent can reason about.

[← Contents](README.md) · [Next: Getting started →](02-getting-started.md)
