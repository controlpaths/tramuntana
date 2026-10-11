# I2C debugging

Bus on J1: DIO6 = SCL, DIO7 = SDA. The board has switchable pull-ups
(`pullups`, or `i2c_config(pullups=...)`).

## Checklist

1. **Idle levels.** With the bus idle, `dio_read`: DIO6 and DIO7 must both be
   1.
   - Both 0 with pull-ups on: target unpowered (clamped through its ESD
     diodes), SCL/SDA shorted to GND, or a device holding the bus.
   - SDA 0, SCL 1: a slave stuck mid-byte (master reset during a read).
     `i2c_bus_reset` clocks it out. If it comes back, find who resets mid-transfer.
   - Both 0 with pull-ups off and nothing else on the bus: no pull-ups
     anywhere. Turn on the board's, or fit them on the target.
2. **Scan.** `i2c_scan` at 100 kHz.
   - Nothing found: check power, the pull-ups, SDA/SCL swapped (swap the
     wires or the definition), the address pins on the device, and whether
     the device needs an enable/reset line released first.
   - An address shows up but not the expected one: the datasheet address may
     be 8-bit (shifted). 0xD0 in a datasheet is 0x68 in 7-bit; A0..A2 pins
     change the low bits.
   - Every address answers: SDA stuck low (see 1).
3. **Identify.** Read the ID register (`i2c_write_read(addr, "<reg>", 1)`).
   Common ones: MPU-6050/9250 0x75 → 0x68/0x71; BME280 0xD0 → 0x60 (BMP280
   0x58); LIS3DH 0x0F → 0x33; VL53L0X 0xC0 → 0xEE; DS3231 has no ID (read
   0x00..0x06 time registers); 24Cxx EEPROM at 0x50..0x57, no ID.
4. **Look at the wire.** If a transfer NAKs or returns nonsense:
   `logic_capture(wait=false, rate_hz=2e6, trigger="pattern", mask=0x80,
   value=0x00, pre=1000, decode="i2c")` (triggers on SDA low = START),
   then the transfer, then `logic_read(decode="i2c")`.
   - NAK on the address → wrong address, device not ready/in reset, or the
     device is busy (EEPROM write cycle: ~5 ms of NAKs after a write is
     normal, so poll).
   - NAK on a data byte → register doesn't exist, device locked, or the write
     isn't allowed in its current mode.
   - SCL low stretched long after a byte → clock stretching; the device is
     slow (some sensors stretch during conversions). Usually fine. A problem
     only if a master doesn't support it.
   - `partial_bits` or missing STOP → the master aborted or the bus was reset
     mid-transfer.
5. **Signal quality.** If decode looks random or flaky at 400 kHz but works at
   100 kHz: slow rise times from weak pull-ups or bus capacitance. Wire SDA
   or SCL to AIN0 and `scope_capture` with a rising trigger. Rise time
   (`rise_time_s`) must stay under 1 us at 100 kHz and 300 ns at 400 kHz.
   The scope samples at 2 us, so a measurable rise time already means the
   edges are far too slow.

## Sniffing a target's own bus

The target is the master (an MCU talking to its sensors): `sniff_start("i2c")`
then `sniff_read` with the board's pull-ups off unless the bus has none.
For timing or long bursts use the logic analyzer with `decode="i2c"`. It
also catches what the firmware sniffer may drop at high rates.

## Root causes seen most

- Missing or doubled pull-ups (too strong, below ~1 kΩ total, can stop
  devices pulling low enough).
- 7-bit vs 8-bit address confusion.
- Device held in reset/power-down by an enable pin.
- Repeated START needed but firmware sends STOP + START (some devices lose the
  register pointer).
- Bus locked by a slave after a master reset; fix with 9 SCL clocks at init.
