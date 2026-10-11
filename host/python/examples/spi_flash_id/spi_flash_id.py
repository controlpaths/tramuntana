#!/usr/bin/env python3
"""Read the JEDEC id and the first 256 bytes of a SPI NOR flash on DIO2..5."""
from tramuntana import Tramuntana

with Tramuntana() as t:
    t.power(True)
    t.spi.config(8_000_000, mode=0)
    jedec = t.spi.txn(b"\x9f", read_n=3)
    print("JEDEC id:", jedec.hex(" "))
    page = t.spi.txn(b"\x03\x00\x00\x00", read_n=256)   # READ from address 0
    for i in range(0, 256, 16):
        print(f"{i:04x}: {page[i:i + 16].hex(' ')}")
