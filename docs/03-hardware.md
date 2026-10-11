# 3. Hardware

[← Getting started](02-getting-started.md) · [Contents](README.md) · [Next: SPI →](04-spi.md)

## 3.1 The J1 header

J1 is a 2×8 header. Pin 1 (square pad) is at the top; odd pins are in the
inner column, even pins in the outer one.

| Pin | Name | RP2350 | Function |
|---|---|---|---|
| 1 | 3V3 | GPIO11 (enable) | switched 3.3 V rail for the target |
| 2, 4, 6 | GND | — | ground |
| 3 | AIN0 | GPIO26 / ADC0 | scope channel 0, ADC 0 |
| 5 | AIN1 | GPIO27 / ADC1 | scope channel 1, ADC 1 |
| 7 | AOUT0 | GPIO8 | waveform generator output |
| 8 | VREF | — | ADC reference: **never load it** |
| 9 | DIO0 | GPIO7 | UART RX (from the target's TX) · logic 0 |
| 10 | DIO1 | GPIO6 | UART TX (to the target's RX) · logic 1 |
| 11 | DIO2 | GPIO5 | SPI CS · logic 2 |
| 12 | DIO3 | GPIO4 | SPI MISO · logic 3 |
| 13 | DIO4 | GPIO3 | SPI MOSI · logic 4 |
| 14 | DIO5 | GPIO2 | SPI SCK · logic 5 |
| 15 | DIO6 | GPIO1 | I2C SCL · logic 6 |
| 16 | DIO7 | GPIO0 | I2C SDA · logic 7 |

`DIOn = GPIO(7 − n)`. The software always uses DIO numbers; the GPIO
numbers only matter when reading the firmware.

`tramuntana pinout` (CLI), `pinout` (shell) or `display_pinout` (API) draw
the header on the board's screen, with each pin's name and function, for 60
seconds or until dismissed. Use it while wiring.

### Wiring names

The bus functions are named from the board's point of view when it is the
**master**:

| Bus | Board pin | Connect to the target's |
|---|---|---|
| UART | DIO1 (TX) | RX |
| UART | DIO0 (RX) | TX |
| SPI | DIO2 CS, DIO5 SCK, DIO4 MOSI, DIO3 MISO | CS, SCK, MOSI (SDI), MISO (SDO): same name to same name |
| I2C | DIO6 SCL, DIO7 SDA | SCL, SDA |

The same wiring works for the sniffers and the emulated devices: the board
then listens on, or answers on, the same pins.

## 3.2 Electrical limits

| What | Limit |
|---|---|
| DIO logic level | 3.3 V (RP2350 GPIO). **Not 5 V tolerant**: use a level shifter for 5 V targets |
| AIN0 / AIN1 | 0..3.3 V, 12-bit. Use a divider for higher voltages |
| AOUT0 | 0..3.3 V, through a 2.7 kΩ / 1 nF RC filter (≈59 kHz corner): a high-impedance output, not a supply |
| 3V3 rail (J1.1) | the Pico's 3.3 V through a P-FET switch, fused. Meant for small targets (sensors, MCU boards) |
| VREF (J1.8) | reference voltage, for measurement only |

The board has a fuse, clamp diodes and series resistors. They protect
against accidents, not against wiring a 5 V or 12 V signal to a pin.

## 3.3 Target power

`power on` / `t.power(True)` switches the 3.3 V rail on J1.1. It is **off at
power-up** and after a firmware reset. The power-good LED (D7) lights
while it is on.

- If the target has its own supply, don't connect J1.1, or keep the rail
  off: two supplies would fight.
- To watch the target's supply ramp at power-on, wire it to AIN0 and arm a
  scope capture with a rising trigger before switching the rail on. See
  [Oscilloscope](06-scope-and-awg.md#watching-a-power-up).
- A power cycle is the simplest reset: `power off`, wait, `power on`. With
  the [sequencer](11-sequencer.md) the off time is exact.

## 3.4 I2C pull-ups

I2C needs pull-up resistors on SCL and SDA. The board has a pair of 4.7 kΩ
pull-ups on DIO6/DIO7 that can be switched in and out:

- `pullups on` / `t.pullups(True)`, or `i2c conf` (which turns them on by
  default; `--no-pullups` keeps them off).
- When off, the pull-up network is disconnected, never driven low, so it
  doesn't load the target's bus.

The I2C sniffer and the emulated I2C device can turn them on for their
own use (their `pullups` option). When they stop they put the previous
state back, unless you changed the pull-ups yourself in the meantime:
then your setting stays.

Turn them on when the target has no pull-ups of its own (a bare sensor
breakout often has them; a bare chip doesn't). Leave them off when sniffing
a target's bus that already has pull-ups. Doubling the pull-ups lowers the
total resistance, which some weak devices can't pull down.

## 3.5 How pins are shared: the pin arbiter

Each DIO can only do one thing at a time. A small arbiter in the firmware
decides who drives each pin:

| Owner | Holds pins | Can be taken over |
|---|---|---|
| `gpio` | plain GPIO (`dio dir`, `dio write`) | yes |
| `spi` | the SPI master, DIO2..DIO5 | yes ("soft") |
| `i2c` | the I2C master, DIO6/DIO7 | yes ("soft") |
| `pwm` | PWM outputs | yes ("soft") |
| `uart` | the UART bridge (DIO0/DIO1) while active, and the reset/boot control lines | **no** (exclusive) |
| `emu` | an emulated device's pins | **no** (exclusive) |

A *soft* owner keeps its pins between operations, so the bus idles at the
right levels, but any other instrument that needs them takes them. The SPI
master then reclaims its pins on its next transaction. An *exclusive*
owner refuses: a claim on its pins fails with **busy** until it releases
them.

**Passive instruments don't take pins.** The logic analyzer, the sniffers
and the DIO watch read the pads whatever drives them: this board's master,
the bridge, an emulated device or the target. That is how you can capture
this board's own I2C transaction with the logic analyzer.

`tramuntana status` (or `pins` in the shell) shows the owner of each pin,
with `!` for exclusive ones:

```
pins         ['uart!', 'uart!', 'spi', 'spi', 'spi', 'spi', 'i2c', 'i2c']
```

## 3.6 The status LED

The RGB LED shows what the board is doing:

| Colour | Meaning |
|---|---|
| white flash | boot |
| blue | an SPI/I2C operation in progress |
| green flash | operation finished OK / capture done |
| red flash | operation failed (NAK, timeout) / capture timed out |
| red (steady) | a capture is armed, waiting for its trigger |
| green (steady) | a capture is running |
| blue background | a sniffer is listening |
| magenta background | the waveform generator is running |
| magenta | a sequencer program is running |

`led <colour>` / `t.led(...)` overrides it; `led auto` / `t.led()` gives it
back to the firmware.

## 3.7 The screen

The 128×160 TFT shows a dashboard rather than one screen per command:

- a title bar that briefly shows the last activity (`SPI 4B`,
  `I2C W 50 2B`, `UART on`, `EMU R D0 1B`, `SEQ done`…);
- one status line per instrument: system (power, pull-ups, host link),
  SPI, I2C, AWG, scope, logic, UART, and a free **note** line;
- a plot area with the last scope trace or logic capture.

The note line takes up to 24 characters from the host:
`tramuntana display "flashing v2"` or `t.display(...)`. Scripts and AI
agents use it to show what they are doing. The pinout screen
(`pinout`) temporarily replaces the dashboard.

## 3.8 USB ports and identification

| Interface | Linux | Purpose |
|---|---|---|
| 0 | `/dev/ttyACM0` | shell |
| 2 | `/dev/ttyACM1` | binary API |
| 4 | `/dev/ttyACM2` | UART bridge |

USB VID:PID is `2FE3:0100`. The serial number is the chip id (16 hex
digits), also returned by `info`. The host software finds the ports by
VID/PID and interface number; with several boards connected, pick one with
`--serial <id>` (CLI) or `Tramuntana(serial_number=...)` (Python).

## 3.9 Hardware sources

The KiCad projects are in [`hw/tramuntana_lite_r1a`](../hw/tramuntana_lite_r1a)
and [`hw/tramuntana_r1a`](../hw/tramuntana_r1a). The devicetree that
describes the board to the firmware is
[`fw/boards/tramuntana_lite_r1a-common.dtsi`](../fw/boards/tramuntana_lite_r1a-common.dtsi).

[← Getting started](02-getting-started.md) · [Contents](README.md) · [Next: SPI →](04-spi.md)
