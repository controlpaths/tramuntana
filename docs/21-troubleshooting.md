# 21. Troubleshooting

[← Examples](20-examples.md) · [Contents](README.md)

## Connection

| Symptom | Cause and fix |
|---|---|
| `no Tramuntana board found` | the board isn't plugged in, runs no firmware or an old one (flash it, see [Getting started](02-getting-started.md#21-flash-the-firmware)), or your user can't open `/dev/ttyACM*` (`dialout` group or a udev rule) |
| `Device or resource busy` / the port won't open | another program has the port open: the webapp, a terminal, another script, the AI tools' server. Close it |
| `is not a Tramuntana API port` | `--port` points to the shell or bridge port; use the API port (`tramuntana list`) |
| `no reply to command 0x..` | the board stopped answering (crashed, or rebooted into the bootloader). Unplug and replug it; if it persists, reflash |
| `unknown command` (status 8) | the firmware is older than the host software: flash the current one |
| webapp: "not the API port" | Chrome lists the three ports with the same name; pick another one |
| several boards | pick one with `--serial ID` / `Tramuntana(serial_number=...)`; `tramuntana list` shows the IDs |

## Flashing

| Symptom | Fix |
|---|---|
| `build.sh flash` times out waiting for the drive | run `tramuntana bootloader`, wait for the `RP2350` drive, copy `zephyr.uf2` by hand |
| the board doesn't answer at all | hold BOOTSEL while plugging it in, copy the UF2 |
| after flashing, the ports don't come back | wait a few seconds; replug; check `dmesg` for USB errors |

## "busy" (status 2)

| Situation | Why |
|---|---|
| `dio dir` on DIO0/DIO1 | the UART bridge is active: close the bridge port or `uart off` |
| any claim on a pin marked `!` in `status` | held exclusively by the bridge, its control lines or the emulator |
| starting a sniffer / logic capture | the capture memory is in use by another one: stop the other sniffer or let the capture finish |
| `adc` | a scope capture is using the ADC |
| `uart ctrl` on a pin | the pin is held by the emulator (or vice versa) |
| sequencer start | a program is already running (`t.seq.status()`, `t.seq.abort()`) |
| PWM on a pin | its pair partner is a live PWM at another frequency |

## I2C

| Symptom | Likely cause |
|---|---|
| scan finds nothing | no pull-ups (`pullups on`), target unpowered, SDA/SCL swapped, wrong voltage level |
| scan finds every address | SDA held low: a stuck slave (`i2c reset`), a short, or another pin driving it (e.g. a jumper to an SPI pin) |
| `timeout` (status 3) | SCL held low: no pull-ups, or an unpowered target clamping the line |
| NAK on the address | wrong address (7-bit, not 8-bit!), device not ready, EEPROM busy writing |
| works at 100 kHz, fails at 400 kHz | slow rise times: stronger pull-ups, shorter wires |

More in [I2C](05-i2c.md#53-diagnosing-a-bus).

## SPI

| Symptom | Likely cause |
|---|---|
| reads `ff ff …` | MISO not driven: wiring, power, CS not reaching the device, flash in deep power-down (`ab`) |
| reads `00 00 …` | MISO shorted low, device in reset, MOSI/MISO swapped |
| right bytes shifted by one bit | wrong mode: decode a logic capture in all four modes |
| sniffer shows garbage | wrong mode given to the sniffer, or SCK above ~15 MHz |

## Scope and logic captures

| Symptom | Fix |
|---|---|
| state `timeout` | the trigger never came: check the level / pin / pattern, or capture with `trigger="none"` to see what is there |
| a pattern trigger never fires although the pattern is true | it fires when the pins *enter* the pattern; a pattern already true when armed waits for the next time |
| `ValueError` from the logic analyzer | pattern trigger limits: rate ≤ 15 MS/s, mask ≠ 0, n ≤ 122880 |
| a finished logic capture "disappeared" | a sniffer took the shared memory; read captures before starting a sniffer |
| decoded UART is garbage at the start | the capture began on the start bit with no idle before it: add `pre` |

## UART bridge

| Symptom | Fix |
|---|---|
| nothing received | TX/RX swapped (DIO0 is the board's RX, from the target's TX); wrong baud rate (capture DIO0 and use `decode.guess_baud`); no common ground |
| garbled characters | wrong baud rate or framing; a target clock problem gives a "non-standard" baud rate |
| the target is held in reset when a terminal opens the port | control lines in `direct` mode; use `esp` mode or `uart ctrl off` |
| esptool can't connect | check the EN/GPIO0 wiring, `uart ctrl esp --reset <EN> --boot <GPIO0>`, and that nothing else has `/dev/ttyACM2` open |

## Emulated devices

| Symptom | Fix |
|---|---|
| the target's master gets NAKs / timeouts | the master doesn't support clock stretching; or the address differs (7-bit) |
| two devices answer | the real device is still on the bus at the same address |
| SPI: first response byte wrong | SCK too fast for the first bit (modes 1/3 above ~2 MHz) |
| SPI: transactions merge | CS high time between transactions under 10 µs |
| writes not stored | the register is marked read-only (`read_only([])` clears) |

## Timeline and DIO watch

| Symptom | Fix |
|---|---|
| no `dio` entries for a pin | the pin wasn't in `watch`, or it was muted (more than 50 edges/ms: a bus, not a control line) |
| a `dio` entry reads `glitch` | the pin toggled back before its level was read |
| `uart` lines appear a few ms late | expected: they are stamped on arrival; see [precision](12-timeline.md#129-precision) |
| no sniffer entries | the sniffer has to be started separately (`t.i2c.sniff(True)`) |

## Getting more information

- `tramuntana status` / `pins` in the shell: who owns each pin.
- The shell's `kernel thread list` and `kernel thread stacks` show the
  firmware's threads.
- The firmware logs warnings (protocol errors, overruns) on the shell port.

[← Examples](20-examples.md) · [Contents](README.md)
