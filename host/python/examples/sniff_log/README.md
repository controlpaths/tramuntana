# sniff_log

Passive logger: prints every I2C and SPI message that goes through the
header, with a timestamp, until Ctrl-C. The Tramuntana only listens; it
never drives the bus.

## Wiring

Connect the bus to watch to the header, with GND shared:

| Bus | Tramuntana |
|---|---|
| I2C SCL | J1.15 (DIO6) |
| I2C SDA | J1.16 (DIO7) |
| SPI CS | J1.11 (DIO2) |
| SPI MISO | J1.12 (DIO3) |
| SPI MOSI | J1.13 (DIO4) |
| SPI SCK | J1.14 (DIO5) |

## Usage

```bash
python3 sniff_log.py
```

Example output:

```
12:04:31 I2C S 68W+ 75+  S 68R+ 71- P
```

(`+` = ACK, `-` = NAK, `S` = START, `P` = STOP.)

## How it works

The script starts both sniffers: `t.i2c.sniff(True, pullups=False)` (the
bus under test already has its own pull-ups) and `t.spi.sniff(True)`. Each
complete message arrives from the firmware as an event; `t.events()` returns
them already decoded as `I2cMessage` / `SpiMessage` objects, whose `str()`
is the compact line above. Both sniffers are stopped on exit, Ctrl-C
included.
