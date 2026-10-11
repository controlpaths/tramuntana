# board_test

Functional test of a Tramuntana Lite board. Flashes the firmware, asks for
six jumper wires on J1 and tests every instrument against them. Exit code
0 when everything passes.

## Wiring

Six wires on J1, nothing else connected:

| From | To | Tests |
|---|---|---|
| J1.7 AOUT0 | J1.3 AIN0 | waveform generator -> ADC / scope ch0 |
| J1.1 3V3 | J1.5 AIN1 | target power rail -> ADC / scope ch1 |
| J1.9 DIO0 | J1.10 DIO1 | GPIO, UART bridge loopback, PWM |
| J1.12 DIO3 | J1.13 DIO4 | GPIO, SPI MISO <- MOSI loopback |
| J1.11 DIO2 | J1.15 DIO6 | GPIO, SPI CS / I2C SCL |
| J1.14 DIO5 | J1.16 DIO7 | GPIO, SPI SCK / I2C SDA |

The script prints this list and draws the pinout on the board's screen
while it waits for Enter.

## Usage

```bash
python3 board_test.py                  # flash fw/build + test
python3 board_test.py --no-flash       # test the firmware already on it
python3 board_test.py --uf2 my.uf2     # flash another file
```

| Option | |
|---|---|
| `--uf2` | firmware to flash first (default `fw/build/tramuntana_fw/zephyr/zephyr.uf2`) |
| `--no-flash` | test the firmware already on the board |
| `--serial` | USB serial number of the board, when several are plugged in |
| `--yes` | don't wait for Enter after the wiring step |
| `--no-visual` | skip the LED / screen questions |

## How it works

**Flash.** Unless `--no-flash`, the board is sent to its UF2 bootloader,
the UF2 is copied to the drive that mounts, and the script waits for the
API port to come back. Without a UF2 it tests what is on the board.

**Automatic tests**, in order (a failing step is reported and the board
restored before the next one):

| Test | What it checks |
|---|---|
| info | firmware version, board, chip ID |
| gpio | each DIO pair, both directions, both levels |
| uart bridge | 256 bytes at 115200 echoed DIO1 -> DIO0 through `/dev/ttyACM2` |
| spi | 64-byte full duplex loopback MOSI -> MISO at 1 MHz, plus a logic analyzer capture of it |
| i2c | SCL/SDA idle high with the pull-ups, a clean scan, and a logic capture showing SCL edges and a START |
| pwm | DIO0 -> DIO1, frequency and duty measured with the logic analyzer |
| awg / scope | DC levels 0.30 / 1.65 / 3.00 V read on AIN0 within 100 mV, then a sine captured with the scope |
| power rail | AIN1 reads the 3V3 rail on and off, scope ch1 sees it |

**Visual checks** (skipped with `--no-visual`): the power LED with the rail
on and off, each RGB LED colour, and a random code on the screen; for each
one the script asks what you see.

A summary line closes the run, with the failing tests if any.
