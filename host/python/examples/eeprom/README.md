# eeprom

Reads a 24xx I2C EEPROM into a `.bin` file, or writes a `.bin` file into
it and verifies it.

## Wiring

| EEPROM | Tramuntana |
|---|---|
| VCC | J1.1 (3V3) |
| GND | J1.2 |
| SCL | J1.15 (DIO6) |
| SDA | J1.16 (DIO7) |
| WP | GND |
| A0..A2 | GND or 3V3, sets the address |

The I2C address is `0x50 + A2..A0` (A1 high -> `0x52`). `tramuntana i2c
scan` shows it.

## Usage

```bash
python3 eeprom.py read  dump.bin  --addr 52 --size 256     # 24x02 -> file
python3 eeprom.py write image.bin --addr 52 --size 256     # file -> 24x02
python3 eeprom.py read  dump.bin  --addr 50 --size 16384   # 24LC128
```

| Option | Default | |
|---|---|---|
| `--addr` | `50` | 7-bit I2C address, hex |
| `--size` | 256 | capacity in bytes |
| `--page` | from `--size` | page size in bytes |
| `--freq` | 400000 | SCL frequency in Hz |
| `--no-pullups` | | leave the board's pull-ups off (modules with their own) |

The image to write may be shorter than the EEPROM: only its bytes are
written, from address 0.

## Supported chips

`--size` selects the addressing and the page size. It must be the real
capacity: the wrong family means the wrong addressing.

| `--size` | Chips | Memory address | Page |
|---|---|---|---|
| 128, 256 | 24x01, 24x02 | 1 byte | 8 |
| 512, 1024, 2048 | 24x04, 24x08, 24x16 | 1 byte + block in the I2C address | 16 |
| 4096, 8192 | 24x32, 24x64 | 2 bytes | 32 |
| 16384, 32768 | 24x128, 24x256 | 2 bytes | 64 |
| 65536 | 24x512 | 2 bytes | 128 |

Page sizes vary between vendors (an ST M24C02 has 16-byte pages). A page
smaller than the real one is safe, just slower; a bigger one corrupts data,
so the defaults are the conservative ones. Parts above 64 KiB (24x1025,
24xM01, 24xM02) are not supported.

## How it works

**Addressing.** Up to 2 KiB the memory address is one byte; the 24x04..16
select their 256-byte block with the low bits of the I2C address
(`0x50 | (mem >> 8)`), so a 24x16 answers on 0x50..0x57. From 4 KiB up the
address is two bytes, MSB first.

**Read.** One write of the memory address, a repeated START and a
sequential read (`t.i2c.write_read()`), in chunks of 256 bytes (one block)
on the small parts and 4096 bytes on the big ones.

**Write.** A write is START, address, up to one page of data, STOP. Bytes
past the end of a page wrap to its start, so the image is split at page
boundaries. After each page the chip spends up to 5 ms programming and
NAKs its address meanwhile; the script polls with address-only writes
until it ACKs again (ACK polling). Finally the whole EEPROM is read back
and compared with the image.

## Troubleshooting

- *verify FAILED*: WP is probably tied high. Writes are ACKed but ignored.
- Data repeats every 256 bytes in a dump: the chip is smaller than
  `--size` says.
