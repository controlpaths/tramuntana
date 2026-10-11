# vl53l0x_range

Distance readings from a VL53L0X time-of-flight sensor over I2C.

## Wiring

| Module | Tramuntana |
|---|---|
| VIN | J1.1 (3V3) |
| GND | J1.2 |
| SCL | J1.15 (DIO6) |
| SDA | J1.16 (DIO7) |
| XSHUT, GPIO1 | not connected |

VIN must go to J1.1. Breakout boards carry their own SDA/SCL pull-ups;
with the board's pull-ups on and the rail off, current flows through SCL
into the module (its power LED lights up) and the chip never really powers
down. It then answers on I2C but cannot fire its laser. That is why the
script leaves the board's pull-ups off by default.

## Usage

```bash
python3 vl53l0x_range.py               # 10 readings
python3 vl53l0x_range.py -n 0          # until Ctrl-C
```

| Option | Default | |
|---|---|---|
| `--addr` | `29` | 7-bit I2C address, hex |
| `--freq` | 100000 | SCL frequency in Hz |
| `--pullups` | off | enable the board's I2C pull-ups (module without its own) |
| `-n` | 10 | number of readings, 0 = until Ctrl-C |
| `--period` | 0.2 | seconds between readings |

Each reading prints the distance in mm, plus the range status when it is
not valid (e.g. no target in range). The value is also shown on the
board's screen.

## How it works

The VL53L0X does not measure out of reset: writing `SYSRANGE_START` alone
never completes. It needs ST's init sequence, ported here from Pololu's
VL53L0X library:

1. Power cycle: pull-ups off, rail off for 0.5 s, rail on. Clean boot.
2. Read the model ID (`0xC0`, `0xEE` on genuine parts; a different value
   only prints a warning).
3. Data init: 2V8 I/O mode, read the *stop variable* from the hidden
   register page (`0xFF`/`0x80` dance), disable the MSRC and pre-range
   signal checks, final range signal limit 0.25 MCPS.
4. Reference SPADs: read the count and type from NVM and enable that many
   in the reference SPAD map.
5. Load ST's default tuning settings (about 80 register writes).
6. Interrupt on "new sample ready", then VHV and phase reference
   calibrations, each one a short ranging run.

A measurement then rewrites the stop variable, starts a single shot, polls
`RESULT_INTERRUPT_STATUS` (`0x13`) and reads 12 bytes from `0x14`: byte 0
holds the range status (11 = valid) and bytes 10..11 the distance in mm.

## Troubleshooting

- *timeout waiting for calibration*: the chip cannot fire its laser.
  Check VIN is on J1.1 and that the module's power LED goes off with
  `tramuntana pullups off` + `tramuntana power off`. If it still fails,
  the sensor is faulty (some clones report model ID `0x2e` and never
  complete a calibration).
- A "VL53L0/1X" breakout may carry a VL53L1X instead, which uses 16-bit
  register addresses and is not supported by this script.
