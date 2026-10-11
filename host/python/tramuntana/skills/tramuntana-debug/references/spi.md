# SPI debugging

Bus on J1: DIO2 = CS (active low), DIO3 = MISO, DIO4 = MOSI, DIO5 = SCK.

## Checklist

1. **Start slow.** `spi_config(freq_hz=1_000_000, mode=0)`. Most devices
   accept mode 0 and mode 3. Check the datasheet's CPOL/CPHA.
2. **Read an ID.** JEDEC flash: `spi_transaction("9f", 3)` → manufacturer,
   type, capacity (EF = Winbond, C2 = Macronix, 20 = Micron/ST, 1F = Adesto,
   C8 = GigaDevice). Other common IDs: SD card (CMD0 then CMD8 sequence), ADXL345
   reg 0x00 → 0xE5 (read = address | 0x80), nRF24L01 reads registers with
   `0x00 | reg`.
3. **Interpret the MISO pattern.**
   - All `ff`: MISO floating high or pulled up. The device isn't driving it:
     not powered, CS not reaching it, MISO wire open, or the device is in
     deep power-down (flash: send `ab` to release it, then retry `9f`).
   - All `00`: MISO shorted low, the device held in reset, or MISO/MOSI swapped.
   - Right bytes shifted by one bit (e.g. `ef` reads as `de` or `f7`):
     wrong mode. Capture once and re-decode with `logic_decode("spi",
     spi_mode=0..3)` to find the mode where the bytes make sense.
   - Echo of MOSI on MISO: MOSI and MISO shorted or swapped with a loop.
4. **Look at the wire.** `logic_capture(wait=false, rate_hz=10e6,
   trigger="falling", pin=2, decode="spi")` triggers on CS. Then
   `spi_transaction`, then `logic_read(decode="spi")`. Check that SCK idles
   at the CPOL level, that CS stays low for the whole command, that MISO
   changes on the opposite edge to the one sampled, and `sck_hz`.
5. **Speed.** Works at 1 MHz but not at 20 MHz → wiring length/ringing or the
   device's max clock. Long jumper wires rarely work above a few MHz.

## Flash specifics

- Status register `05`: bit0 = WIP (busy), bit1 = WEL. Writes need `06`
  (write enable) first, and WEL clears after each program/erase.
- Protect bits (BP0..BP2 in SR1, and SR2/SR3 on some parts) block writes
  silently: program "succeeds" but data doesn't change.
- Erase and program are destructive. **Confirm with the user first** and
  read back before and after.
- `03 aa aa aa` + n bytes reads data. All `ff` after erase is correct.

## Sniffing a target's own bus

`sniff_start("spi", spi_mode=...)` then `sniff_read`, or the logic analyzer
with `decode="spi"` and the right `spi_mode` when the target uses other pins
(`logic_decode` takes any DIO for cs/sck/mosi/miso). A CS-less bus: pass
`cs=null` and every SCK edge goes into one transaction.

## Root causes seen most

- Wrong mode (CPOL/CPHA) or bit order.
- CS toggled between bytes by the firmware when the device wants it held low
  for the whole command.
- Flash in deep power-down or write-protected.
- MISO not tri-stated by another device sharing the bus (its CS left low).
