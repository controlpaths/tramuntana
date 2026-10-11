# Emulated devices, sequences and the timeline

Three tools for questions the other instruments can't answer on their own:
"what does the firmware do when the sensor says X?", "what happens in the
first 5 ms after reset?", "does it work if the steps are 200 µs apart
instead of 2 s?".

## Timeline: always on while debugging

Start it before the experiment, not after the symptom:

```
timeline_start(watch_dios=[3, 4], dio_names={"3": "RESET", "4": "IRQ"})
uart_open(115200)
sniff_start("i2c")
timeline_mark("power-up test 1")
power(on=true)
... wait / other tools ...
timeline_read(since_s=<the mark's time>)
```

Read it like a scope screen with words. Look for the order of events (did
the firmware talk to the sensor before or after it was out of reset?), the
gaps between them (how long the sensor took to answer, how long boot takes),
and repeats (a boot banner twice = reset loop, the same NAK every 100 ms =
retry loop). Quote the relevant lines when you report.

`watch_dios` is for control lines only (reset, enable, IRQ, chip select of
a slow device, LEDs). A clock or data line gets muted. Use the logic
analyzer for those.

## Sequences: when timing matters

A tool call takes milliseconds to seconds, so "pulse reset for 100 µs" or
"read the sensor 1 ms after enabling it" can't be done with `dio_write`
followed by `i2c_write_read`. Put the whole experiment in one `sequence_run`:

```
sequence_run(steps=[
  {"op": "dio_dir", "pin": 3, "output": true},
  {"op": "dio_write", "pin": 3, "level": 0},          # RESET low
  {"op": "delay_us", "us": 100},
  {"op": "mark", "id": 1},
  {"op": "dio_write", "pin": 3, "level": 1},          # release
  {"op": "wait_dio", "pin": 4, "level": 0, "timeout_us": 500000},   # IRQ asserted?
  {"op": "i2c_write_read", "address": 118, "write": "d0", "n": 1, "keep_going": true},
  {"op": "dio_dir", "pin": 3, "output": false}])      # release the line afterwards
```

The result gives each step's time from the first one, in µs. `wait_dio`
returns how long it waited (e.g. boot time to IRQ). A failing step stops
the run unless it has `keep_going`. Use that to probe: "try the sensor
every 500 µs after reset until it ACKs" is a `loop` of `i2c_read` with
`keep_going` and a `delay_us`, and the first `ok` step tells you when it
became ready.

Arm a capture inside the sequence (`logic_arm` / `scope_arm`) right before
the stimulus, then read it with `logic_read` / `scope_read`. The trigger
then lines up with the step that caused it.

Driving a pin the target also drives is a short. A sequence does exactly
what it says, so check the wiring first (rule 3 of the skill).

## Emulated device: test the firmware, not the sensor

`emu_i2c` / `emu_spi` make the board a device on the target's bus with a
256-byte register file. Wire it where the real device would be (or remove
the real one: two devices at one address fight). The target's firmware
can't tell the difference, so you control what it reads.

Setting up a device: put the ID register and the data registers the
firmware reads in `registers`, and mark the ID read-only. Datasheet register
maps go straight in:

```
emu_i2c(address=0x76, read_only=[0xD0], registers={
  "0xD0": "60",                       # BME280 chip id
  "0x88": "70 6b 43 67 18 fc ...",    # calibration (from a real part, or the datasheet example)
  "0xF7": "65 5a c0 7e ed 00 6b 6d"}) # raw pressure / temperature / humidity
```

Then:

- **Does it initialise correctly?** Let it boot, then `emu_get_registers`
  on the configuration registers (e.g. 0xF2/0xF4/0xF5 for a BME280) and
  compare with what the datasheet says the firmware should write. The
  timeline's `emu` entries show every access in order.
- **Edge cases:** `emu_set_registers` while it runs, with values at the
  limits (max/min, negative, the "invalid" code of the part, a stuck value),
  and read what the firmware prints or does.
- **Missing or broken device:** `emu_stop` (NAKs everything, like a missing
  part), or the wrong ID in the ID register, and check that the firmware
  reports it rather than hanging or using garbage.
- **Slow device:** the emulator can't delay answers. Use a real part for
  timing problems.

SPI: the default command format (bit 7 = read, 7-bit register, auto-
increment) fits most sensors. For others set `read_mask` / `read_value` /
`addr_mask` (e.g. read when bit 7 is 0: `read_mask=0x80, read_value=0x00`).
`status` is what MISO returns during the command byte (the nRF24 STATUS
register, for instance). The first response bit has to be ready half a
clock after the command, so if the target's SCK is above about 2 MHz in
modes 1/3, slow it down in the firmware under test or expect the first bit
to be wrong.

## Flashing through the bridge

See [uart.md](uart.md#flashing-through-the-bridge). After flashing, keep the
timeline running across the reset to catch the new firmware's boot.
