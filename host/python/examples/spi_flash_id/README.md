# spi_flash_id

Reads the JEDEC ID and the first 256 bytes of a SPI NOR flash (W25Qxx,
MX25Lxx, GD25Qxx...).

## Wiring

| Flash | Tramuntana |
|---|---|
| VCC | J1.1 (3V3) |
| GND | J1.2 |
| /CS | J1.11 (DIO2) |
| DO (MISO) | J1.12 (DIO3) |
| DI (MOSI) | J1.13 (DIO4) |
| CLK | J1.14 (DIO5) |
| /WP, /HOLD | 3V3 |

## Usage

```bash
python3 spi_flash_id.py
```

## How it works

The 3V3 rail is switched on and the SPI master configured at 8 MHz, mode 0.
Each access is one `t.spi.txn()`: CS low, write the command, read the
answer, CS high.

- `9F` (READ JEDEC ID) returns 3 bytes: manufacturer, memory type and
  capacity. `ef 40 18` is a Winbond W25Q128, for instance.
- `03 00 00 00` (READ DATA from address 0) returns the first 256 bytes,
  printed 16 per line. An erased flash reads all `ff`.
