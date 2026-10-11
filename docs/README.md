# Tramuntana documentation

![Tramuntana](img/logo.jpeg)

Tramuntana is an open bench tool for analysing, testing and debugging
embedded systems. It is built on the RP2350 (a Raspberry Pi Pico 2) and runs
a Zephyr firmware. One USB cable and one 16-pin header give you:

| Instrument | In one line |
|---|---|
| SPI master + sniffer | SPIDriver-style transactions up to 75 MHz, passive full-duplex tap |
| I2C master + sniffer | I2CDriver-style primitives, scan, register access, bus recovery, passive decoder |
| Oscilloscope | 2 channels, 12 bits, 500 kS/s, pre-trigger |
| Waveform generator | sine / triangle / saw / square / stairs / DC / step / arbitrary, 1 Hz – 100 kHz |
| Logic analyzer | 8 channels, 150 MS/s, edge and pattern triggers, VCD export, UART/I2C/SPI decoders |
| PWM outputs | hardware PWM on any DIO, phase-synchronised pairs |
| UART bridge | the target's console as a serial port; RTS/DTR can drive its reset/boot pins |
| Emulated device | the board answers on the target's I2C or SPI bus from a register file |
| Sequencer | step lists run on the board with microsecond timing |
| Timeline | UART output, bus traffic, pin edges and more, timestamped on one clock |
| Target power | switched, protected 3.3 V rail; switchable I2C pull-ups |

You can drive it from a terminal (the shell), from a command-line tool,
from Python, from a browser, or from an AI agent. All of them reach the
same firmware.

## Contents

### Start here

1. [Overview](01-overview.md): what the board is, how it is organised, and
   which interface to use for what.
2. [Getting started](02-getting-started.md): flash the firmware, install
   the host software, take your first measurements.
3. [Hardware](03-hardware.md): the J1 header, electrical limits, power,
   pull-ups, how pins are shared between instruments, the LED and the
   screen.

### Using the instruments (guides)

4. [SPI master and sniffer](04-spi.md)
5. [I2C master and sniffer](05-i2c.md)
6. [Oscilloscope and waveform generator](06-scope-and-awg.md)
7. [Logic analyzer and protocol decoding](07-logic-analyzer.md)
8. [GPIO and PWM outputs](08-gpio-and-pwm.md)
9. [UART bridge, reset/boot control and flashing a target](09-uart-bridge.md)
10. [Emulated I2C/SPI devices](10-emulated-devices.md)
11. [Sequencer: microsecond-timed step lists](11-sequencer.md)
12. [Timeline, DIO watch and timestamps](12-timeline.md)

### Reference

13. [Python library reference](13-python-library.md): every class,
    method, argument and return value of the `tramuntana` package.
14. [Command-line reference](14-cli.md): the `tramuntana` command.
15. [Shell reference](15-shell.md): the firmware's own console.
16. [Binary protocol reference](16-binary-protocol.md): framing, every
    command and event, payload layouts.
17. [Firmware internals](17-firmware.md): source layout, threads, PIO/DMA
    use, memory map, building, adding a command.

### Other front ends and material

18. [Web application](18-webapp.md): the browser front end (Web Serial).
19. [AI tools](19-ai-tools.md): the MCP server and the `tramuntana-debug`
    skill that let an AI agent use the board.
20. [Examples](20-examples.md): the ready-made scripts in
    `host/python/examples`.
21. [Troubleshooting](21-troubleshooting.md): common problems and their
    fixes.

## Conventions used in these documents

- **DIOn** is header pin J1.(9+n): DIO0 is J1.9 and DIO7 is J1.16.
  A **DIO mask** has one bit per pin, bit i = DIOi (`0x80` = DIO7).
- Bytes and I2C addresses are written in **hex**. I2C addresses are always
  **7-bit** (`0x68`, never `0xD0`).
- Voltages are in volts and every analog pin works over 0..3.3 V.
- `t` in Python snippets is a connected board:
  `from tramuntana import Tramuntana; t = Tramuntana()`.
- `/dev/ttyACM0..2` are the board's three ports on Linux. On another
  machine they have other names (`tramuntana list` shows them).

## Where things live in the repository

```
fw/apps/tramuntana_fw/   firmware (Zephyr)
fw/boards/               devicetree of the board
fw/build.sh              build / flash helper
host/python/             Python package: library, `tramuntana` command, MCP server
                         and AI agent skill; examples
host/webapp/             browser front end
hw/                      KiCad projects of both boards
docs/                    this documentation
```
