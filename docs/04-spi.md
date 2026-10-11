# 4. SPI master and sniffer

[← Hardware](03-hardware.md) · [Contents](README.md) · [Next: I2C →](05-i2c.md)

## 4.1 Pins and wiring

| Board | J1 | Target |
|---|---|---|
| DIO2 | 11 | CS (chip select, active low) |
| DIO3 | 12 | MISO / SDO / DO |
| DIO4 | 13 | MOSI / SDI / DI |
| DIO5 | 14 | SCK / CLK |
| GND | 2 | GND |
| 3V3 | 1 | VCC (if the board powers the target) |

## 4.2 The master

The master is the RP2350's SPI0 peripheral, with chip select driven by hand
as a GPIO. It works like the SPIDriver:

- **CS is under your control.** `sel` pulls it low, `unsel` releases it,
  and between the two you can call `write`, `read` and `xfer` as many times
  as you like. Multi-part commands (opcode, then address, then data) are
  easy to build, and CS stays wherever you left it between calls.
- **`txn` does a whole transaction in one request**: CS low, write some
  bytes, read some bytes, CS high. Use it whenever you can: it is one USB
  round trip instead of four.

| Setting | Range |
|---|---|
| clock | 1 kHz – 75 MHz (the closest the peripheral can do; the actual value is returned) |
| mode | 0–3 (CPOL = bit 1, CPHA = bit 0) |
| bit order | MSB first (default) or LSB first |
| word size | 8 bits |
| bytes per request | up to 4096 (the library splits longer transfers) |

Bulk throughput is about **150 kB/s** whatever the clock, because the
firmware's SPI driver leaves ~1.5 µs between bytes. The clock rate still
matters for the target's timing.

### From the CLI

```bash
tramuntana spi conf 8M 0                      # 8 MHz, mode 0
tramuntana spi txn 9f --read 3                # JEDEC id: ef 40 18 = Winbond W25Q128
tramuntana spi txn 03 00 00 00 --read 256     # read 256 bytes from address 0
tramuntana spi sel                            # step by step:
tramuntana spi write 06                       #   write enable
tramuntana spi unsel
tramuntana spi xfer 9f 00 00 00               # full duplex, prints what came back
```

### From Python

```python
t.spi.config(8_000_000, mode=0)               # returns the actual clock
jedec = t.spi.txn(b"\x9f", read_n=3)          # b'\xef\x40\x18'

# a read built step by step
t.spi.sel()
t.spi.write(b"\x03\x00\x10\x00")              # READ from 0x001000
data = t.spi.read(4096)                       # MOSI sends 0x00 (fill=)
t.spi.unsel()

# full duplex: what came back on MISO while sending
rx = t.spi.xfer(b"\x9f\x00\x00\x00")          # CS untouched: use sel()/unsel()
```

### From the shell

```
spi conf 8000000 0
spi txn 3 9f
spi sel
spi write 03 00 00 00
spi read 16
spi unsel
```

### Recipes

**Identify a SPI NOR flash:** `txn 9f, read 3` returns manufacturer, type,
capacity (`ef` Winbond, `c2` Macronix, `20` Micron, `c8` GigaDevice). If it
returns `ff ff ff`, nothing drives MISO (wiring, power, CS, or the chip is
in deep power-down: send `ab` first). `00 00 00` usually means MISO is
shorted low or the device is held in reset.

**Register-based sensors** (most accelerometers, pressure sensors): the
first byte is the register address with a read bit (often bit 7).
`t.spi.txn(bytes([0x80 | reg]), read_n=n)` reads `n` registers.

**Writes to a flash** need a write-enable first (`06`), and erase/program
take time. Poll the status register (`05`) until bit 0 (busy) clears.
Erasing and programming are destructive, so read the content back
before and after.

**Not sure about the mode?** Capture one transaction with the logic
analyzer and decode it in all four modes. See
[Logic analyzer](07-logic-analyzer.md#73-decoding-uart-i2c-and-spi).

## 4.3 The sniffer

The sniffer listens to DIO2..DIO5 without driving anything. It captures
**both directions**, MOSI and MISO, and reports one message per CS-low
window: the total length, the first 256 bytes of each direction, and an
overrun flag if it couldn't keep up. It works up to about 15 MHz SCK.

You tell it the bus mode (which clock edge to sample on). If you don't, it
uses the master's mode.

```bash
tramuntana spi sniff --mode 0         # live, one line per message, Ctrl-C to stop
```
```
SPI 4B  MOSI 9f 00 00 00  MISO 00 ef 40 18
```

```python
t.spi.sniff(True, mode=0)
for msg in t.events(timeout=5):       # stops after 5 s without events
    print(msg)                        # SpiMessage: total, mosi, miso, overrun, t_us
t.spi.sniff(False)
```

From the shell, `spi sniff on 0` echoes every message as a line until
`spi sniff off`.

Uses:

- **Reverse engineering:** watch what a target's MCU sends to its flash or
  sensor at boot.
- **Checking your own traffic:** the sniffer also sees this board's
  master, so you can confirm what really went on the wire.
- **On the timeline:** with a [timeline](12-timeline.md) running, every
  message gets the board time of its CS falling edge, exact to the
  microsecond.

The sniffer, the I2C sniffer and the logic analyzer share the capture
memory: only one of them runs at a time (the others get **busy**).

## 4.4 Emulating a SPI device

The board can also be the *slave* on the target's SPI bus: see
[Emulated devices](10-emulated-devices.md#104-spi).

## 4.5 Limits and gotchas

- CS is a GPIO, so there is a few-µs gap between CS falling and the first
  clock. Every SPI device accepts that.
- After you change the mode, the next `sel` clocks one dummy byte with CS
  still high to settle SCK at its new idle level. Targets ignore it (CS is
  high), but you'll see 8 clock pulses on a logic capture.
- The SPI pins are shared with plain GPIO, the emulator and the reset/boot
  control lines (see [the pin arbiter](03-hardware.md#35-how-pins-are-shared-the-pin-arbiter)).
  The master takes its pins back on the next operation unless an exclusive
  owner holds them.

[← Hardware](03-hardware.md) · [Contents](README.md) · [Next: I2C →](05-i2c.md)
