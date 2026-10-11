#!/usr/bin/env python3
"""Read a 24xx I2C EEPROM into a .bin file, or write a .bin file into it.

Wiring: VCC -> J1.1 (3V3), GND -> J1.2, SCL -> J1.15 (DIO6), SDA -> J1.16
(DIO7), WP to GND (tied high, writes are ACKed but ignored). The device
answers at 0x50 + A2..A0. A bare chip has no pull-ups of its own, so the
board's are switched on (--no-pullups for modules that carry them).

--size picks the addressing: up to 2 KiB (24x01..24x16) one address byte,
with the 256-byte blocks of the bigger ones selected by the low bits of
the device address; above that (24x32..24x512) two address bytes. Writes
go page by page, polling for the ACK that ends each write cycle, and are
read back to verify.

    python3 eeprom.py read dump.bin --addr 52 --size 256     # 24x02
    python3 eeprom.py write image.bin --addr 50 --size 16384 # 24LC128
"""
import argparse
import time

from tramuntana import Tramuntana, ProtoError

CHUNK = 4096                                         # largest read per request
WRITE_TIMEOUT = 0.05                                 # tWC is 5 ms max

# Page size by capacity (Microchip 24LCxx / Atmel 24Cxx).
PAGE = {128: 8, 256: 8, 512: 16, 1024: 16, 2048: 16, 4096: 32, 8192: 32,
        16384: 64, 32768: 64, 65536: 128}


class Eeprom:
    def __init__(self, t: Tramuntana, addr: int, size: int, page: int):
        self.i2c = t.i2c
        self.addr = addr
        self.size = size
        self.page = page
        self.wide = size > 2048                      # two address bytes

    def target(self, mem: int):
        """Device address and address bytes for memory location ``mem``."""
        if self.wide:
            return self.addr, mem.to_bytes(2, "big")
        return self.addr | (mem >> 8), bytes([mem & 0xFF])

    def read(self, mem: int, n: int) -> bytes:
        dev, a = self.target(mem)
        return self.i2c.write_read(dev, a, n)

    def wait_ready(self, dev: int):
        """ACK polling: the chip NAKs its address until the write cycle ends."""
        end = time.monotonic() + WRITE_TIMEOUT
        while True:
            try:
                self.i2c.write(dev, bytes(2 if self.wide else 1))   # address only, no data
                return
            except ProtoError:
                if time.monotonic() > end:
                    raise TimeoutError(f"EEPROM 0x{dev:02x} still busy after {WRITE_TIMEOUT * 1000:.0f} ms")

    def write(self, mem: int, data: bytes):
        dev, a = self.target(mem)
        self.i2c.write(dev, a + data)
        self.wait_ready(dev)

    def dump(self, progress) -> bytes:
        # Reads stay inside one 256-byte block on the small parts.
        step = CHUNK if self.wide else 256
        data = bytearray()
        while len(data) < self.size:
            data += self.read(len(data), min(step, self.size - len(data)))
            progress(len(data))
        return bytes(data)

    def program(self, data: bytes, progress):
        mem = 0
        while mem < len(data):
            n = self.page - (mem % self.page)        # never cross a page boundary
            self.write(mem, data[mem:mem + n])
            mem += n
            progress(min(mem, len(data)))


def progress_bar(total: int, what: str):
    def show(done: int):
        print(f"\r{what} {done}/{total} bytes", end="", flush=True)
    return show


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("op", choices=["read", "write"], help="read: EEPROM -> file, write: file -> EEPROM")
    ap.add_argument("file", help=".bin file")
    ap.add_argument("--addr", type=lambda s: int(s, 16), default=0x50, help="7-bit address, hex (default 50)")
    ap.add_argument("--size", type=int, default=256, help="capacity in bytes (default 256, 24x02)")
    ap.add_argument("--page", type=int, help="page size in bytes (default from --size)")
    ap.add_argument("--freq", type=int, default=400_000, help="SCL frequency in Hz (default 400000)")
    ap.add_argument("--no-pullups", action="store_true", help="leave the board's I2C pull-ups off")
    a = ap.parse_args()

    if a.size not in PAGE and a.page is None:
        raise SystemExit(f"unknown size {a.size}: pass --page too")
    page = a.page or PAGE[a.size]

    if a.op == "write":
        with open(a.file, "rb") as f:
            image = f.read()
        if len(image) > a.size:
            raise SystemExit(f"{a.file} is {len(image)} bytes, the EEPROM only {a.size}")

    with Tramuntana() as t:
        t.power(True)
        time.sleep(0.1)
        t.i2c.config(a.freq, pullups=not a.no_pullups)
        if a.addr not in t.i2c.scan():
            raise SystemExit(f"no device at 0x{a.addr:02x} (check wiring and power)")

        ee = Eeprom(t, a.addr, a.size, page)
        start = time.monotonic()
        if a.op == "read":
            data = ee.dump(progress_bar(a.size, "read"))
            print(f"  ({time.monotonic() - start:.1f} s)")
            with open(a.file, "wb") as f:
                f.write(data)
            print(f"wrote {a.file}")
        else:
            ee.program(image, progress_bar(len(image), "write"))
            print(f"  ({time.monotonic() - start:.1f} s)")
            back = ee.dump(progress_bar(a.size, "verify"))[:len(image)]
            print()
            bad = [i for i in range(len(image)) if back[i] != image[i]]
            if bad:
                raise SystemExit(f"verify FAILED at {len(bad)} byte(s), first 0x{bad[0]:04x} "
                                 f"(wrote {image[bad[0]]:02x}, read {back[bad[0]]:02x}) - WP tied high?")
            print(f"verify ok, {len(image)} bytes")


if __name__ == "__main__":
    main()
