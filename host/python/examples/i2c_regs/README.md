# i2c_regs

Scans the I2C bus and prints the first 32 registers (0x00..0x1F) of every
device that answers. A quick way to see what is on the bus and whether it
looks alive.

## Wiring

| Device | Tramuntana |
|---|---|
| VCC | J1.1 (3V3) |
| GND | J1.2 |
| SCL | J1.15 (DIO6) |
| SDA | J1.16 (DIO7) |

## Usage

```bash
python3 i2c_regs.py
```

## How it works

The script switches the 3V3 rail on, configures the I2C master at 400 kHz
with the board's pull-ups, runs `t.i2c.scan()` and, for each address found,
does `t.i2c.reg_read(addr, 0x00, 32)`: one write of the register byte, a
repeated START and a 32-byte read. A device that NAKs the register access
prints the error instead of its registers.

It assumes 8-bit register addresses. Devices with 16-bit addresses (24LC32
and larger EEPROMs, VL53L1X...) take the first byte as a register and the
dump is meaningless for them.
