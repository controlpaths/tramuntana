# 9. UART bridge, reset/boot control and flashing a target

[← GPIO and PWM](08-gpio-and-pwm.md) · [Contents](README.md) · [Next: Emulated devices →](10-emulated-devices.md)

## 9.1 The bridge

The board's third serial port (`/dev/ttyACM2` on Linux) is connected to a
hardware UART on the header:

| Board | J1 | Target |
|---|---|---|
| DIO1 (TX) | 10 | RX |
| DIO0 (RX) | 9 | TX |
| GND | 2 | GND |

Open the port with any terminal or serial library. **The baud rate,
parity, data bits and stop bits you open it with are applied to the
target side**, so there is nothing else to configure:

```bash
picocom -b 115200 /dev/ttyACM2
picocom -b 9600 --parity e /dev/ttyACM2
python3 -m serial.tools.miniterm /dev/ttyACM2 115200
```

```python
import serial
from tramuntana import find_ports
port = find_ports()[0]["uart"]
with serial.Serial(port, 115200, timeout=1) as u:
    u.write(b"help\r\n")
    print(u.read(256))
```

### Bridge modes

| Mode | Behaviour |
|---|---|
| `auto` (default) | active while a program holds the bridge port open (it asserts DTR when opening) |
| `on` | always active |
| `off` | never active: DIO0/DIO1 free for plain GPIO |

A `baud` override forces a rate on the target side, whatever the host
program sets. Set them with `tramuntana uart on --baud 9600`,
`uart on 9600` in the shell, or `t.uart.config("on", 9600)`.

While the bridge is active it holds DIO0/DIO1 **exclusively**: `dio_dir`
on them fails with **busy**. SPI and I2C keep working alongside, because
they use other pins.

`tramuntana uart status` (or `t.uart.status()`) shows whether it's active,
the baud rate, the bytes received from and sent to the target since boot,
and the control-line state.

### Reading the console from scripts

`UartTap` opens the bridge port and reads it continuously in a background
thread. You don't lose output between reads, and with a
[timeline](12-timeline.md) every line is recorded with its time:

```python
from tramuntana import Tramuntana, UartTap

with Tramuntana() as t:
    tap = UartTap(t, 115200)
    t.power(True)
    print(tap.read(timeout=3, until=b"login:").decode())   # boot log up to the prompt
    tap.write(b"root\r\n")
    print(tap.read(timeout=1).decode())
    tap.close()
```

The AI tools work the same way: `uart_open` starts buffering, and
`uart_read` / `uart_send` collect what arrived.

## 9.2 Control lines: reset/boot

Many targets are flashed through their UART by a ROM bootloader. A
USB-serial adapter enters it by driving the target's reset and boot-mode
pins from its RTS and DTR outputs. Tramuntana can do the same: the bridge
port's **RTS and DTR drive two DIOs** of your choice (DIO2..DIO7).

The two pins are **open-drain**: asserted = pulled low, deasserted =
released (the target's own pull-up sets the high level). They can pull a
pin low but never drive it high.

| Mode | Reset pin (RTS) | Boot pin (DTR) | For |
|---|---|---|---|
| `esp` | low only when RTS is asserted and DTR isn't | low only when DTR is asserted and RTS isn't | the two-transistor auto-reset circuit of ESP32/ESP8266 boards, which is what esptool expects |
| `direct` | low while RTS is asserted | low while DTR is asserted | an adapter wired straight to active-low pins, driven by your own program |
| `off` | released | released | — |

In `esp` mode, opening the port (which asserts both lines on most systems)
does nothing, so a terminal doesn't reset the target. In `direct` mode it
holds the target in reset until the program releases RTS.

While control lines are on, the bridge stays active whether or not the
port is open, because DTR is now a signal rather than "port open". Every
DTR/RTS change is reported as a `LineChange` event and appears on the
timeline.

```bash
tramuntana uart ctrl esp --reset 3 --boot 2      # DIO3 -> EN, DIO2 -> GPIO0
tramuntana uart status                           # shows the pins and their state
tramuntana uart ctrl off
```

```python
t.uart.ctrl("esp", reset_dio=3, boot_dio=2)
```

Shell: `uart ctrl esp 3 2`, `uart ctrl off`. Webapp: the **Control lines**
card on the UART tab (and DTR / RTS / RESET PULSE next to its terminal).

Driving the lines yourself:

```python
tap = UartTap(t, 115200)
tap.set_lines(dtr=False, rts=True)    # esp mode: EN low (reset held)
tap.set_lines(dtr=False, rts=False)   # release: the target boots normally
```

The AI tools' `uart_set_lines(rts=True, dtr=False, pulse_ms=100)` does a
timed pulse.

## 9.3 Flashing a target through the bridge

### ESP32 / ESP8266

Wire EN to one DIO and GPIO0 to another, plus TX/RX and GND:

```bash
tramuntana uart ctrl esp --reset 3 --boot 2
esptool --port /dev/ttyACM2 --baud 460800 write_flash 0x0 firmware.bin
# or: idf.py -p /dev/ttyACM2 flash monitor
```

esptool's default reset sequence works as it does with a USB-serial chip:
it holds GPIO0 low, pulses EN, and finds the ROM loader.

### STM32 (system bootloader)

BOOT0 has to be **high** to enter the bootloader, and the control lines can
only pull low, so they can't drive BOOT0. Don't use `uart ctrl` here (in
`direct` mode, any program opening the port would hold NRST low):

1. Raise BOOT0: the board's BOOT0 jumper or button, or a DIO driven high as
   a plain GPIO output, if nothing else drives that pin.
2. Pulse NRST low with a [sequence](11-sequencer.md): `dio_dir` output,
   `dio_write` low, `delay_ms(10)`, `dio_dir` input.
3. `stm32flash -w firmware.bin -v -g 0x0 /dev/ttyACM2`
4. Release BOOT0 and pulse NRST again to run the new firmware.

### Anything else with a serial bootloader

Use a sequence to put the boot pin in the right state, pulse reset, then
run the vendor's tool on `/dev/ttyACM2`.

### After flashing

Keep a [timeline](12-timeline.md) running with the UART open across the
reset to capture the new firmware's boot log, with times.

## 9.4 Limits

- 3.3 V logic only. An RS-232 port (±12 V) would damage the pins: use a
  transceiver.
- The tools accept rates up to 4 Mbaud. 2 KB buffers on each side absorb
  bursts. Bytes that arrive while nobody has the port open are dropped.
- Received bytes reach the host in groups (the UART's receive FIFO). Host
  timestamps of UART lines are therefore up to ~1.5 ms late at 115200 baud
  (see [Timeline](12-timeline.md#129-precision)).

[← GPIO and PWM](08-gpio-and-pwm.md) · [Contents](README.md) · [Next: Emulated devices →](10-emulated-devices.md)
