# 16. Binary protocol reference

[← Shell](15-shell.md) · [Contents](README.md) · [Next: Firmware internals →](17-firmware.md)

The binary API runs on the board's second serial port (`/dev/ttyACM1`).
The Python library, the CLI, the webapp and the AI tools all use it. This
chapter is enough to write a client in any language. The authoritative
definition is `fw/apps/tramuntana_fw/src/proto.h`; `host/python/tramuntana/proto.py`
and `host/webapp/js/proto.js` mirror it.

## 16.1 Framing

All multi-byte fields are **little-endian**.

```
0xAA 0x55 | cmd u8 | seq u8 | len u16 | payload[len] | crc u16
```

| Field | Meaning |
|---|---|
| `0xAA 0x55` | start of frame |
| `cmd` | command id (requests < 0x70), reply id (`cmd \| 0x80`), or event id (0xF0..0xFF) |
| `seq` | chosen by the host (1..255), echoed in the reply; 0 in events |
| `len` | payload length, ≤ 4104 |
| `crc` | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR) over `cmd`, `seq`, `len` and the payload |

- **Request:** `cmd < 0x80`, the command's arguments as payload.
- **Reply:** `cmd | 0x80`, the same `seq`. The payload starts with a
  **status byte** (0 = OK), followed by the command's data.
- **Event:** `cmd` 0xF0..0xFF, `seq` 0, sent by the board on its own. An
  event can arrive **between a request and its reply**: match replies by
  `seq`, and route everything with `cmd >= 0xF0` to an event handler.

Command ids stay below 0x70 so a reply id (`cmd | 0x80`) can never fall in
the event range.

The board resynchronises on `AA 55`, drops frames with a bad CRC, and
abandons a frame that stops arriving halfway for 500 ms. The serial port's
baud rate setting doesn't matter (it is USB).

**Example:** PING with seq 1, and its reply:

```
→ aa 55 01 01 00 00 44 c5
← aa 55 81 01 06 00 00 54 52 4d 54 01 61 88      status 0, "TRMT", version 1
```

I2C_WRITE_READ (0x24), seq 2: address 0x76, write 1 byte `d0`, read 1:

```
→ aa 55 24 02 05 00 76 01 d0 01 00 72 25
```

## 16.2 Status codes

| Code | Name | Typical cause |
|---|---|---|
| 0 | OK | |
| 1 | invalid argument | bad payload length, value out of range |
| 2 | busy | pins held exclusively, instrument or memory in use, ADC in use, sequence running |
| 3 | timeout | I2C SCL held low > 10 ms, a wait that expired |
| 4 | bus error / NAK | I2C device didn't ACK |
| 5 | not supported | |
| 6 | not permitted | pin not in the right mode, I2C bus not open, capture not finished, bridge not active |
| 7 | no memory | PIO program space or state machines exhausted |
| 8 | unknown command | the firmware doesn't have that command |
| 9 | wrong state | |

## 16.3 Commands

`→` = reply data after the status byte. "—" = empty.

### System (0x01..0x0F)

| Id | Name | Request | Reply |
|---|---|---|---|
| 01 | PING | — | `"TRMT"`, proto_version u8 (1) |
| 02 | INFO | — | fw_version[16], board[16] (NUL-padded), sysclk u32, chip_id[8], features u32 |
| 03 | BOOTLOADER | — | — ; the board then reboots into the UF2 bootloader |
| 04 | LED | r u8, g u8, b u8 (0/1); `FF x x` = automatic | — |
| 05 | POWER | [on u8] (no payload = query) | on u8 |
| 06 | PULLUPS | [on u8] | on u8 |
| 07 | DIO_DIR | mask u8, out_mask u8 | — |
| 08 | DIO_WRITE | mask u8, value u8 | — |
| 09 | DIO_READ | — | levels u8 |
| 0A | STATUS | — | pins u32, power u8, pullups u8, scope u8, logic u8, awg u8, spi_sniff u8, i2c_sniff u8, bridge u8, rx_dropped u32 |
| 0B | DISPLAY_TEXT | text (≤ 24 ASCII) | — |
| 0C | ADC_READ | channel u8 | mV u16 |
| 0D | DISPLAY_PINOUT | seconds u16 (0 = dashboard, 0xFFFF = hold) | — |
| 0E | TIME | [stamp u8] | t_us u32 |
| 0F | DIO_WATCH | mask u8 (0 = off) | — |

- `features` bits: 0 SPI, 1 I2C, 2 scope, 3 AWG, 4 logic, 5 UART, 6 PWM,
  7 DIO watch/time, 8 UART control lines, 9 emulated targets,
  10 sequencer.
- STATUS `pins`: 4 bits per DIO, DIO0 in the low nibble: bits 2..0 =
  owner (0 gpio, 1 spi, 2 i2c, 3 uart, 4 pio, 5 pwm, 6 emu), bit 3 =
  exclusive. `scope`/`logic` are capture states (below).
- TIME with `stamp = 1` makes the sniffer events carry timestamps
  (EVT_SPI_MSG / EVT_I2C_MSG flag bit 7); `stamp = 0` turns it off.

### SPI (0x10..0x17)

| Id | Name | Request | Reply |
|---|---|---|---|
| 10 | SPI_CONF | freq_hz u32, mode u8, lsb_first u8 | actual freq_hz u32 |
| 11 | SPI_SEL | — | — (CS low) |
| 12 | SPI_UNSEL | — | — (CS high) |
| 13 | SPI_WRITE | data[1..4096] | — |
| 14 | SPI_READ | n u16, fill u8 | data[n] |
| 15 | SPI_XFER | data[1..4096] | rx[same length] |
| 16 | SPI_TXN | read_n u16, data[0..4096] | rx[read_n] (CS low, write, read, CS high) |
| 17 | SPI_SNIFF | on u8, [mode u8] (default: the master's) | — ; EVT_SPI_MSG events while on |

### I2C (0x20..0x2A)

| Id | Name | Request | Reply |
|---|---|---|---|
| 20 | I2C_CONF | hz u32, pullups u8 | actual hz u32 |
| 21 | I2C_SCAN | — | bitmap[16]: bit (a % 8) of byte (a / 8) set = address a ACKed |
| 22 | I2C_WRITE | addr u8, data[0..4096] | acked u16 (status 4 on NAK, still with `acked`) |
| 23 | I2C_READ | addr u8, n u16 | data[n] |
| 24 | I2C_WRITE_READ | addr u8, wn u8, w[wn], rn u16 | data[rn] |
| 25 | I2C_START | addr_rw u8 (address << 1 \| R/W) | ack u8 (repeated START if the bus is open) |
| 26 | I2C_STOP | — | — |
| 27 | I2C_RAW_WRITE | data[] | acked u16 (bus must be open) |
| 28 | I2C_RAW_READ | n u16, nack_last u8 | data[n] |
| 29 | I2C_SNIFF | on u8, pullups u8 | — ; EVT_I2C_MSG events while on |
| 2A | I2C_RESET | — | — (up to 9 SCL pulses + STOP) |

### Scope (0x30..0x33)

| Id | Name | Request | Reply |
|---|---|---|---|
| 30 | SCOPE_ARM | channels u8, n u16 (per channel), clkdiv u16 (≥ 96; rate = 48 MHz / clkdiv, in total), trig_mode u8 (0 none, 1 rising, 2 falling), trig_level u16 (0..4095), pre u16, timeout_ms u16 | rate u32 (total) |
| 31 | SCOPE_STATUS | — | state u8, channels u8, n u16, rate u32, trig_index u16 |
| 32 | SCOPE_READ | offset u16, count u16 (≤ 2048) | samples u16[count] (12-bit; interleaved AIN0, AIN1 with two channels) |
| 33 | SCOPE_ABORT | — | — |

**Capture states** (scope and logic): 0 idle, 1 armed, 2 running, 3 done,
4 timeout, 5 aborted. Usage: ARM, poll STATUS until it's no longer
armed/running (or wait for EVT_CAPTURE_DONE), then READ in pages.

### Waveform generator (0x40..0x45)

| Id | Name | Request | Reply |
|---|---|---|---|
| 40 | AWG_SET | shape u8 (0 sine, 1 tri, 2 saw, 3 square, 4 stairs, 5 arb), freq_hz u32 (1..100000), amp_mv u16, off_mv u16, param u16 (duty % / steps) | achieved freq u32 (**millihertz**), table length u16, PWM top u16 |
| 41 | AWG_OFF | — | — |
| 42 | AWG_STATUS | — | running u8, shape u8 (6 dc, 7 step, 8 off too), freq u32 (mHz), amp_mv u16, off_mv u16 |
| 43 | AWG_ARB_LOAD | offset u16, samples u8[] (0..255 = 0..3.3 V) | — |
| 44 | AWG_DC | mV u16 | — |
| 45 | AWG_STEP | from_mv u16, to_mv u16, delay_ms u16 | — (replies after the step) |

### PWM (0x48..0x4C)

| Id | Name | Request | Reply |
|---|---|---|---|
| 48 | PWM_SET | dio u8, freq_hz u32, duty u16 (0.01 %), invert u8 | achieved freq u32 (mHz), level u16, top u16, div u8 |
| 49 | PWM_PHASE | dio u8, phase_deg u16 (the pin's pair; applied on sync) | — |
| 4A | PWM_SYNC | dio_mask u8 (0 = all live pairs) | — |
| 4B | PWM_STOP | dio_mask u8 | — |
| 4C | PWM_STATUS | — | 8 × (flags u8 [bit0 active, bit1 lost, bit2 invert], freq_hz u32, freq u32 (mHz), duty u16, phase_deg u16), DIO0 first |

PWM_SET fails with **busy** if the pair partner is a live PWM at another
frequency.

### Logic analyzer (0x50..0x53)

| Id | Name | Request | Reply |
|---|---|---|---|
| 50 | LOGIC_ARM | n u32, clkdiv u16 (rate = 150 MHz / clkdiv), trig_mode u8 (0 none, 1 rising, 2 falling, 3 pattern), trig_pin u8, trig_mask u8, trig_value u8, pre u32, timeout_ms u16 | rate u32 |
| 51 | LOGIC_STATUS | — | state u8, n u32, rate u32, trig_index u32 |
| 52 | LOGIC_READ | offset u32, count u16 (≤ 4096) | samples u8[count] (bit i = DIOi) |
| 53 | LOGIC_ABORT | — | — |

Pattern trigger: clkdiv ≥ 10, n ≤ 122880, mask ≠ 0. An edge trigger with
pre > 0 becomes a one-pin pattern.

### UART bridge (0x60..0x62)

| Id | Name | Request | Reply |
|---|---|---|---|
| 60 | UART_CONF | mode u8 (0 auto, 1 on, 2 off), baud u32 (0 = follow the host) | — |
| 61 | UART_STATUS | — | active u8, baud u32, rx u32, tx u32, mode u8, ctrl u8, reset_dio u8, boot_dio u8, lines u8 (bit0 DTR, bit1 RTS, bit2 reset low, bit3 boot low) |
| 62 | UART_CTRL | mode u8 (0 off, 1 direct, 2 esp), reset_dio u8, boot_dio u8 (0xFF = none; DIO2..DIO7) | — |

The data itself flows on the third serial port. (Firmware before 1.1
replies to UART_STATUS with the first four fields only.)

### Emulated targets (0x64..0x67)

| Id | Name | Request | Reply |
|---|---|---|---|
| 64 | EMU_I2C | on u8; if on: addr u8, scl_dio u8, sda_dio u8, pullups u8 | — (on = 0 stops any emulator) |
| 65 | EMU_SPI | on u8; if on: mode u8, cs u8, sck u8, mosi u8, miso u8, addr_mask u8, read_mask u8, read_value u8, status u8 | — |
| 66 | EMU_REGS | op u8 = 0: offset u8, data[1..256] (set) · op 1: offset u8, n u16 (get) · op 2: mask[32] (read-only bitmap, bit r = register r) | op 1: data[n]; else — |
| 67 | EMU_STATUS | — | kind u8 (0 off, 1 i2c, 2 spi), transfers u32, bytes_read u32, bytes_written u32, naks u32, starts u32, stops u32, bytes u32 |

### Sequencer (0x68..0x6C)

| Id | Name | Request | Reply |
|---|---|---|---|
| 68 | SEQ_LOAD | offset u16, code[] (program ≤ 2048 bytes in total) | — |
| 69 | SEQ_RUN | len u16, timeout_ms u32 (0 = 10 s) | — ; runs in the background, EVT_SEQ_DONE at the end |
| 6A | SEQ_STATUS | — | state u8, error u8, pc u16, result_len u16 |
| 6B | SEQ_RESULT | offset u16, n u16 | records (up to n bytes) |
| 6C | SEQ_ABORT | — | — |

**States:** 0 idle, 1 running, 2 done, 3 failed (stopped at `pc` with
`error`), 4 aborted, 5 timeout, 6 invalid (malformed program).

**Program:** a sequence of instructions, opcode first, arguments after
(little-endian). Opcode | 0x80 = continue on failure.

| Op | Name | Arguments | Record data |
|---|---|---|---|
| 01 | DELAY_US | us u32 | — |
| 02 | POWER | on u8 | — |
| 03 | PULLUPS | on u8 | — |
| 04 | DIO_DIR | mask u8, out_mask u8 | — |
| 05 | DIO_WRITE | mask u8, value u8 | — |
| 06 | DIO_READ | — | levels u8 |
| 07 | WAIT_DIO | mask u8, value u8, timeout_us u32 | waited_us u32 |
| 08 | I2C_WRITE | addr u8, n u8, data[n] | acked u8 |
| 09 | I2C_READ | addr u8, n u8 | data[n] |
| 0A | I2C_WRITE_READ | addr u8, wn u8, w[wn], rn u8 | data[rn] |
| 0B | SPI_TXN | wn u8, w[wn], rn u8 | data[rn] |
| 0C | UART_SEND | n u8, data[n] | — |
| 0D | AWG_DC | mV u16 | — |
| 0E | ADC_READ | channel u8 | mV u16 |
| 0F | MARK | id u8 | id u8 |
| 10 | LOOP | count u16 | (no record) |
| 11 | ENDLOOP | — | (no record) |
| 12 | LOGIC_ARM | the 16 bytes of LOGIC_ARM | rate u32 |
| 13 | SCOPE_ARM | the 12 bytes of SCOPE_ARM | rate u32 |
| 14 | WATCH | mask u8 | — |
| 15 | DELAY_MS | ms u32 | — |

**Records** (SEQ_RESULT), one per executed step:

```
pc u16 | op u8 | status u8 | t_us u32 | len u8 | data[len]
```

`t_us` is the board time when the step finished.

## 16.4 Events

| Id | Name | Payload |
|---|---|---|
| F0 | EVT_LOG | text |
| F1 | EVT_SPI_MSG | flags u8 (bit0 overrun, bit7 stamped), [t_us u32 if stamped], total u16, mosi[k], miso[k] — k = (remaining length) / 2, ≤ 256 |
| F2 | EVT_I2C_MSG | flags u8 (bit0 overrun, bit1 truncated, bit7 stamped), [t_us u32 if stamped], records (type u8, value u8)*: 1 START (value = address byte), 2 ACK (1) / NAK (0), 3 DATA, 4 STOP |
| F3 | EVT_CAPTURE_DONE | instrument u8 (0 scope, 1 logic), state u8 |
| F4 | EVT_DIO | t_us u32, levels u8, changed u8, lost u8 |
| F5 | EVT_EMU | t_us u32, kind u8 (1 i2c, 2 spi), flags u8 (bit0 read, bit1 NAK, bit2 truncated, bit3 no START), addr u8, reg u8, data[≤ 32]. With NAK: an I2C address byte not answered, addr = byte >> 1, reg = the raw byte |
| F6 | EVT_LINE | t_us u32, dtr u8, rts u8 |
| F7 | EVT_SEQ_DONE | t_us u32, state u8, error u8, pc u16, result_len u16 |

`t_us` is the board clock (TIME): microseconds since boot, 32 bits.
Events are best effort: if the host doesn't read the port, the board
drops them rather than block.

## 16.5 Writing a client

1. Open the API port (any baud rate) and start a reader that parses frames
   continuously.
2. Send PING, check `"TRMT"` and the version.
3. For each request, pick a `seq`, send, and wait for the reply with that
   `seq`. Keep one request in flight at a time (the board processes them in
   order anyway).
4. Hand frames with `cmd >= 0xF0` to the event handler whenever they
   arrive.
5. Keep payloads ≤ 4104 bytes: split bulk transfers (the Python library
   uses 4096-byte data chunks, 2048 scope samples, 4096 logic samples).

A minimal Python client without the library:

```python
import serial, struct

def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def request(ser, cmd, seq, payload=b""):
    body = bytes([cmd, seq]) + struct.pack("<H", len(payload)) + payload
    ser.write(b"\xaa\x55" + body + struct.pack("<H", crc16(body)))
    while True:                                   # skip events, find our reply
        assert ser.read(2) == b"\xaa\x55"
        rcmd, rseq, n = struct.unpack("<BBH", ser.read(4))
        data = ser.read(n); ser.read(2)           # (check the CRC in real code)
        if rcmd == cmd | 0x80 and rseq == seq:
            return data[0], data[1:]              # status, reply data

ser = serial.Serial("/dev/ttyACM1", timeout=2)
print(request(ser, 0x01, 1))                      # (0, b'TRMT\x01')
print(request(ser, 0x0C, 2, bytes([0])))          # ADC_READ AIN0 -> (0, mV u16)
```

[← Shell](15-shell.md) · [Contents](README.md) · [Next: Firmware internals →](17-firmware.md)
