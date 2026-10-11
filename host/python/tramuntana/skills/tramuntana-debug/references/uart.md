# UART debugging

On J1: DIO0 = RX (from the target's TX), DIO1 = TX (to the target's RX).
Logic levels only (3.3 V TTL). An RS-232 port (±12 V) will damage the
pins. Use a transceiver.

## Checklist

1. **Idle level.** With the bridge closed, `dio_read`: DIO0 must idle at 1
   (UART idles high). 0 means target TX not connected, target not powered, or
   TX/RX swapped onto a target input with a pull-down. A line that idles low
   and has traffic is inverted (RS-232 after an inverter, or a misconfigured
   UART).
2. **Is there traffic at all?** With the bridge closed:
   `logic_capture(n=131072, rate_hz=2e6, trigger="falling", pin=0,
   timeout_ms=10000, decode="uart")` and reset or power-cycle the target to
   make it print. No trigger = the target sends nothing on DIO0. Try the
   other wire (swapped TX/RX is the most common fault) with `pin=1`.
3. **Baud rate.** The decode reports `baud` and `baud_guessed`. The guess
   comes from the shortest pulse, so it needs a few characters with
   single-bit pulses (text works well). Non-standard values (74880 on
   ESP8266 boot, a clock misconfigured by 2x or 8/9) show up as a raw
   estimate. That is a finding: the target's clock tree or divisor is wrong.
4. **Framing.** `framing_errors` > 0 with the right baud means wrong data bits,
   parity or stop bits. Retry with `logic_decode("uart", parity="even")`.
   A run of 0x00 frames with framing errors is a break (or the line going
   low at reset).
5. **Talk to it.** `uart_open(baud)`, `uart_read(until="<prompt>")`,
   `uart_send("help\r\n")`. No echo/answer while the target prints fine →
   our TX (DIO1) isn't reaching the target's RX, or the target's RX pin
   isn't configured (logic-capture DIO1 to confirm we send). Wrong line ending:
   try `\r`, `\n` and `\r\n`.

## Capturing a boot log

Record it on the timeline, so the log lines come with times and with
whatever else happened: `timeline_start(watch_dios=[<reset pin>])` →
`uart_open(115200)` → `power(false)` → `power(true)` → wait →
`timeline_read(sources=["uart", "dio", "host", "i2c"])`. The gap between
power-on (or the reset edge) and the first line is the boot time. A log that
starts over means the target is resetting (see power-and-signals.md).
Without the bridge (unknown baud) use the logic capture of step 2, which also
timestamps each character.

## Flashing through the bridge

With the target's reset and boot-mode pins on two DIOs, `uart_ctrl` lets the
vendor's serial flashing tool drive them through the bridge port (the `uart`
port in `list_boards`, e.g. /dev/ttyACM2). Close it here first (`uart_close`)
so the tool can open it, and confirm with the user before flashing.

- **ESP32 / ESP8266** (EN and GPIO0): `uart_ctrl("esp", reset_dio=<EN>,
  boot_dio=<GPIO0>)`, then
  `esptool --port /dev/ttyACM2 --baud 460800 write_flash 0x0 firmware.bin`
  (or `idf.py -p /dev/ttyACM2 flash`). esptool's default reset sequence works
  as with a USB-serial chip.
- **STM32 system bootloader** (NRST and BOOT0): BOOT0 has to be *high* to
  enter the bootloader, and `uart_ctrl` can only pull pins low. In `direct`
  mode any program that opens the port asserts RTS and holds NRST low for as
  long as it has it open, so don't use `uart_ctrl` here. Raise BOOT0 (the
  board's BOOT0 jumper/button, or a DIO driven high as a plain GPIO output,
  if the user confirms nothing else drives it), then pulse NRST with a
  `sequence_run` (`dio_dir` output, `dio_write` low, `delay_ms` 10,
  `dio_dir` input to release it), then
  `stm32flash -w firmware.bin -v -g 0x0 /dev/ttyACM2`. Release BOOT0 and
  pulse NRST again to run the new firmware.
- **Anything with a serial bootloader and no auto-reset**: use
  `sequence_run` to hold the boot pin, pulse reset and release, then run the
  tool.

Then `uart_ctrl("off")` (or keep it, if you'll reflash), `uart_open` again
and read the boot log of the new firmware on the timeline. In `esp` mode,
opening the port with both lines asserted (the default) doesn't reset the
target. `uart_set_lines(rts=true, dtr=false, pulse_ms=100)` resets it on
demand.

## Root causes seen most

- TX/RX swapped.
- Wrong baud from a wrong clock source (HSI vs HSE, PLL not locked, wrong
  crystal value in the firmware) → the measured baud is off by a ratio.
- Target's UART pins muxed to another function, or the console routed to USB
  or another UART.
- Missing common ground.
