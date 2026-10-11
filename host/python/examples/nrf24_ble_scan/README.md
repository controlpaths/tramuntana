# nrf24_ble_scan

Passive Bluetooth LE scanner built from an nRF24L01(+) on the SPI master.
Lists the devices advertising nearby: address, name, manufacturer and how
often each one was heard. It only listens, it never transmits.

## Wiring

| nRF24L01 | Tramuntana |
|---|---|
| VCC | J1.1 (3V3) |
| GND | J1.2 |
| CSN | J1.11 (DIO2) |
| MISO | J1.12 (DIO3) |
| MOSI | J1.13 (DIO4) |
| SCK | J1.14 (DIO5) |
| CE | J1.9 (DIO0) |
| IRQ | J1.10 (DIO1) |

## Usage

```bash
python3 nrf24_ble_scan.py                 # 30 s, live table
python3 nrf24_ble_scan.py -t 0 --raw      # until Ctrl-C, print every packet
```

| Option | Default | |
|---|---|---|
| `-t`, `--time` | 30 | seconds to scan, 0 = until Ctrl-C |
| `--dwell` | 0.6 | seconds on each advertising channel |
| `--ce` | 0 | DIO wired to CE |
| `--irq` | 1 | DIO wired to IRQ, -1 = poll STATUS over SPI |
| `--raw` | | print every packet instead of the table |

## How it works

The nRF24L01 is no Bluetooth chip, but it shares BLE's 1 Mbit/s GFSK
modulation, so it can receive BLE advertising packets if it is told what
to expect:

- It hops between the three advertising channels 37/38/39 (2402, 2426 and
  2480 MHz), `--dwell` seconds on each.
- Its RX address is the BLE advertising access address `0x8E89BED6`, bit
  reversed byte by byte (BLE sends LSB first, the nRF24 MSB first).
- Its own CRC is off and the payload fixed at 32 bytes.

Everything else is done in Python for each payload: bit reversal, BLE
de-whitening (seeded with the channel index), the BLE CRC-24, and decoding
the advertising PDU (type, device address, AD structures: name,
manufacturer, Apple sub-types...).

## Limits

- The 32-byte payload truncates the longest advertisements; their CRC
  cannot be checked, so they are only shown once the same device has been
  heard three times.
- Some clone nRF24L01 chips return the first bit of every 32-bit word
  stuck at 1. Packets that fit get those bits back by trying both values
  against the CRC; in truncated ones the bit shows as `?` where it cannot
  be inferred.
- No RSSI, only the chip's "received power > -64 dBm" flag (`near`).
- Only legacy advertising on the 1M PHY (no BLE 5 extended or coded).
