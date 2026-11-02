# 10. Emulated I2C/SPI devices

[← UART bridge](09-uart-bridge.md) · [Contents](README.md) · [Next: Sequencer →](11-sequencer.md)

## 10.1 What it is for

The board can be a **device on the target's bus**: it answers the target's
I2C or SPI master from a 256-byte register file that you load and read
back. The target's firmware can't tell it from a real sensor, so you
control exactly what it reads:

- run a firmware without the real part on the bench;
- feed it values that are hard to produce for real: limits, invalid codes,
  a stuck value, a sensor reporting 85 °C;
- check what it writes during initialisation (configuration registers,
  calibration reads);
- simulate a missing or broken device: stop the emulator, or give it the
  wrong ID.

Only one emulator runs at a time. Its pins are held exclusively (owner
`emu`) until you stop it. If the real device is also on the bus at the
same address, remove it: two devices at one address fight.

## 10.2 The register model

Both buses use the same model:

- a **register pointer** that auto-increments after every byte read or
  written (wrapping at 256);
- **reads** return `regs[ptr++]`;
- **writes** store into `regs[ptr++]`, unless the register is marked
  **read-only**: the write is then ACKed and ignored, as real devices do
  with ID registers;
- the register file survives `stop()` and switching between I2C and SPI.
  Load it before or while the emulator runs.

Writes are applied **byte by byte**, as they arrive. A host or a test that
reads a block of registers while the target is rewriting it can get it
half-updated (the first bytes new, the rest old): read twice and compare,
or have the target write a flag register last.

Every transfer is reported as an `EmuTransfer` event: whether the target
read or wrote, the starting register, and the bytes (up to 32 per event).
For a read, the bytes are what went out on the bus: on I2C, each byte read
back from SDA as it was sent; on SPI, the response row the board fed to
MISO (firmware 1.3; before, the register contents at the time the event
was logged, which could differ from what the target got).
An I2C address byte the emulator doesn't answer is reported too (`nak`):
the address it carried, and whether a START came before it. That tells a
master talking to the wrong address from a bus that is out of step.
The [timeline](12-timeline.md) shows them, e.g.
`emu  I2C 76 W reg F4: 27` / `emu  I2C 76 R reg F7: 51 2a c0`.

Events are **best effort**. The board queues them (64 deep) and sends them
over USB; a target making thousands of transfers a second outruns that,
and the extra events are dropped. Since firmware 1.3 nothing is lost
silently: `status()["dropped"]` counts them, and the next event that gets
through has `gap=True` (the timeline prints *events dropped before this
one*). The `transfers`, `bytes_read` and `bytes_written` counters always
include every transfer, so compare them with the events before concluding
that a write was lost.

## 10.3 I2C

| | |
|---|---|
| Address | any 7-bit address |
| Pins | any two DIOs; default DIO6 SCL / DIO7 SDA (the J1 I2C pins) |
| Bus speed | tested at 100 kHz, 400 kHz and 1 MHz |
| Protocol | the first byte the master writes sets the register pointer; further bytes are data. A read starts at the current pointer, so *write register, repeated START, read* works as with a real sensor |
| Other addresses | not answered: after the address byte (and a brief stretch) the board releases the bus until the next START, so other devices work normally |

**Clock stretching:** after every START and every byte, the board holds SCL
low for a moment while its CPU decides whether to ACK and which byte comes
next. This keeps it correct at any bus speed, but the target's I2C master
must support clock stretching. Practically all hardware I2C peripherals
do; some bit-banged masters don't. It also lowers the effective rate: a
1 MHz master gets about 550 kHz, a 400 kHz one about 300 kHz.

**SDA hold:** when it transmits (ACK bits, read data), the board changes SDA
about 330 ns after SCL falls, the internal hold time UM10204 asks of a
device. Firmware before 1.3 changed it 30–140 ns after the edge, which
made some samplers (this board's own I2C sniffer, logic analyzers) see a
data bit as a START or a STOP.

Pull-ups: the bus still needs them. The board's own (`pullups=True`) only
exist on DIO6/DIO7.

```python
# a BME280 at 0x76, on the default pins
t.emu.set_regs(0xD0, b"\x60")                                  # chip ID
t.emu.set_regs(0x88, bytes.fromhex("70 6b 43 67 18 fc 7d 8e 43 d6 d0 0b 27 0b 8c 00 f9 ff 8c 3c f8 c6 70 17"))
t.emu.set_regs(0xF7, bytes.fromhex("65 5a c0 7e ed 00 6b 6d"))  # raw pressure/temperature/humidity
t.emu.read_only([0xD0])
t.emu.i2c(0x76)                                                 # scl=6, sda=7, pullups=False

# ... let the target run ...

print(t.emu.status())             # {'bus': 'i2c', 'transfers', 'bytes_read', 'bytes_written', 'naks', ..., 'dropped', 'late'}
print(t.emu.regs(0xF2, 4).hex())  # the configuration the firmware wrote
t.emu.set_regs(0xFA, bytes.fromhex("90 00 00"))                # change the temperature while it runs
t.emu.stop()
```

In the webapp, the **EMU** tab does all of this: start/stop, a register
grid that highlights what the target changed, and a live transfer log.

CLI and shell:

```bash
tramuntana emu i2c 76 --set d0=60 --set f7=655ac07eed006b6d
tramuntana emu log                 # live transfers, Ctrl-C to stop
tramuntana emu regs f2 4
tramuntana emu off
```
```
emu set d0 60
emu i2c 76
emu status
emu off
```

## 10.4 SPI

| | |
|---|---|
| Pins | any four DIOs; default DIO2 CS, DIO5 SCK, DIO4 MOSI, DIO3 MISO (wired like the real device) |
| Modes | 0–3 |
| Clock | up to ~7 MHz in modes 0/2, about 2 MHz in modes 1/3 (see below) |
| Between transactions | CS high for a few µs (see below) |

Each CS-low window starts with a **command byte**. The command format is
configurable:

| Parameter | Default | Meaning |
|---|---|---|
| `read_mask`, `read_value` | 0x80, 0x80 | the command is a read when `cmd & read_mask == read_value` |
| `addr_mask` | 0x7F | the register is `cmd & addr_mask` |
| `status` | 0x00 | what MISO returns while the command byte comes in |

- **Read command:** MISO returns `regs[reg], regs[reg+1]…`, up to 32 bytes.
  Further clocks keep the line at the last bit.
- **Write command:** the MOSI bytes after the command are written from
  `reg` on, each one as it arrives.

The defaults fit most sensors (bit 7 set = read, 7-bit register). Other
formats:

| Device style | Settings |
|---|---|
| read when bit 7 is **clear** | `read_mask=0x80, read_value=0x00` |
| bit 7 = read, bit 6 = multi-byte (ADXL345) | `addr_mask=0x3F` |
| a status byte returned during the command (nRF24L01's STATUS) | `status=0x0E` |
| a fixed response to any command (e.g. a JEDEC ID) | `read_mask=0, read_value=0, addr_mask=0`: every command reads from register 0 |
| a different response per command byte | `read_mask=0, read_value=0, addr_mask=0xFF`: command c reads from register c (put the JEDEC ID at 0x9F, the status at 0x05…) |

**Speed:** the first response bit has to be on MISO half a clock after the
command byte ends. The board doesn't use the CPU for this: the PIO turns
the command into the address of a 32-byte row in a precomputed response
table, and DMA loads the row into the output FIFO. Measured against an FPGA
master (modes 0/2, 250 µs between transactions):

| SCK | Read-back errors |
|---|---|
| ≤ 7.14 MHz | none in 25 900 transactions on one run, 1 on another |
| 8.33 MHz | 50 % |
| 10 MHz | 94 % |
| 12.5 MHz | 100 % |

So stay **below about 7 MHz** for zero errors in modes 0/2. When it fails,
the first data byte is right and the **most significant bit of the bytes
after it** is wrong (e.g. `1f e0 45 56` arrives as `1f 60 c5 d6`); at
10 MHz and above more bits go. In modes 1/3 the first bit is needed sooner;
they passed at 2, 2.94 and 3.85 MHz with a master that leaves about 1.5 bit
times between bytes, but count on about 2 MHz with one that sends bytes
back to back.

**Time between transactions:** when CS rises, the board's CPU rearms the
shifters for the next command. In 14 400 measured transactions that took
under 2 µs in 99.4 %, under 5 µs in all but two, and about 50 µs at worst
(when another interrupt was served first). CS high for 60 µs is a safe
margin; a few µs usually works. Firmware before 1.3 also rebuilt the response table
there, which kept the emulator deaf for 100–160 µs after a write: a
transaction that came sooner was lost or answered with garbage. If a CS
window still starts before the rearm, the board can't tell where its bytes
begin: it counts it in `status()["late"]` and flags its event `suspect`.

```python
t.emu.set_regs(0x00, b"\xe5")                       # ADXL345 DEVID
t.emu.spi(mode=3, addr_mask=0x3F)                   # CS DIO2, SCK DIO5, MOSI DIO4, MISO DIO3
...
t.emu.stop()
```

```bash
tramuntana emu spi --mode 3 --set 00=e5
```

## 10.5 A test workflow

1. Wire the emulator where the real device goes (or remove the real one).
2. Load the registers the firmware reads at boot (ID, calibration, data),
   and mark the ID read-only.
3. Start a [timeline](12-timeline.md), open the UART, start the emulator,
   reset or power the target.
4. Read the timeline: the order of register accesses, what the firmware
   printed.
5. Check the configuration it wrote: `emu.regs(...)` against the
   datasheet.
6. Change data registers while it runs and watch its output, or stop the
   emulator to see how it handles a missing device.

The AI tools expose the same functions (`emu_i2c`, `emu_spi`,
`emu_set_registers`, `emu_get_registers`, `emu_status`, `emu_stop`). The
`tramuntana-debug` skill describes this workflow for agents in
[`references/emulate-and-sequence.md`](../host/python/tramuntana/skills/tramuntana-debug/references/emulate-and-sequence.md).

## 10.6 Limits

- One emulator at a time, one I2C address, 256 registers, 8-bit register
  addresses.
- No delayed answers: the emulator can't simulate a slow device or an
  EEPROM's write cycle (NAK while busy).
- I2C needs a master that supports clock stretching.
- SPI: up to 32 bytes per read burst, a few µs of CS high between
  transactions, and the speed limits above.
- Events are best effort (see [10.2](#102-the-register-model)): at high
  transfer rates count on `status()`, not on the event log.

[← UART bridge](09-uart-bridge.md) · [Contents](README.md) · [Next: Sequencer →](11-sequencer.md)
