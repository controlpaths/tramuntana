# 2. Getting started

[← Overview](01-overview.md) · [Contents](README.md) · [Next: Hardware →](03-hardware.md)

This chapter takes you from a bare board to your first measurements:
flashing the firmware, installing the host software, and trying each way
of using the board.

## 2.1 Flash the firmware

A prebuilt image, `zephyr.uf2`, is attached to every GitHub release. To
build it yourself see [Firmware internals](17-firmware.md#176-building-and-flashing).

**First time (or a board that doesn't answer):**

1. Hold the Pico 2's **BOOTSEL** button and plug the USB cable in.
2. A drive called `RP2350` appears.
3. Copy `zephyr.uf2` onto it. The board reboots into the new firmware.

**Later updates** don't need the button. The running firmware can reboot
itself into the bootloader:

```bash
tramuntana bootloader          # or the shell command `boot`
# the RP2350 drive appears; copy the .uf2 onto it
```

`fw/build.sh flash` does both steps after building (see
[Firmware internals](17-firmware.md#176-building-and-flashing)). If it times out
waiting for the drive, use `tramuntana bootloader` and copy the file by
hand.

After flashing, three serial ports appear (on Linux `/dev/ttyACM0`, `1`,
`2`), the screen shows the dashboard and the LED flashes white once.

## 2.2 Install the host software

The Python package gives you both the `tramuntana` command and the
library. It needs Python 3.8+ and `pyserial`.

```bash
cd host/python
python3 -m venv .venv
. .venv/bin/activate
pip install -e .              # library + `tramuntana` command
pip install -e .[plot]        # + matplotlib/numpy, for --plot and images
pip install -e .[mcp]         # + the MCP server for AI agents (includes [plot])
```

Or skip the activation: `source host/python/env.sh` defines a `tramuntana`
command in the current shell, creating the virtual environment the first
time.

```bash
source host/python/env.sh
tramuntana info
```

**Linux permissions:** your user must be able to open `/dev/ttyACM*`. Add
yourself to the `dialout` group (`sudo usermod -aG dialout $USER`, then log
in again) or install a udev rule.

Check that everything works:

```bash
$ tramuntana list
FA6418C89B553238: shell=/dev/ttyACM0 api=/dev/ttyACM1 uart=/dev/ttyACM2
$ tramuntana info
fw_version   1.1.0
board        tramuntana_lite
...
```

## 2.3 Know the header before wiring

```
 1 3V3 (switched)   2 GND       3 AIN0     4 GND      5 AIN1    6 GND
 7 AOUT0            8 VREF      9 DIO0    10 DIO1    11 DIO2   12 DIO3
13 DIO4            14 DIO5     15 DIO6    16 DIO7

DIO0/1 = UART RX/TX    DIO2..5 = SPI CS / MISO / MOSI / SCK    DIO6/7 = I2C SCL/SDA
```

`tramuntana pinout` draws it in colour in the terminal and on the board's
own screen at the same time, which helps while you place wires.

Before connecting a target:

- **Share ground.** Connect at least one GND pin to the target's ground.
- **3.3 V logic only.** The DIOs are RP2350 pins: no 5 V signals. The
  analog pins accept 0..3.3 V.
- **Don't power the target twice.** If the target has its own supply,
  leave J1.1 (3V3) unconnected, or keep the rail off.
- **Never load VREF** (J1.8). It is the ADC's reference.

See [Hardware](03-hardware.md) for the details.

## 2.4 First session: the shell

The shell needs nothing but a terminal program:

```bash
picocom /dev/ttyACM0          # any baud rate works
```

```
tramuntana:~$ info
tramuntana:~$ power on
tramuntana:~$ i2c scan
tramuntana:~$ adc 0
tramuntana:~$ scope 1024
tramuntana:~$ help
```

Every command is listed in the [shell reference](15-shell.md). Exit picocom
with `Ctrl-A Ctrl-X`.

## 2.5 First session: the CLI

```bash
tramuntana status                     # who owns each pin, what is running
tramuntana power on                   # 3.3 V to the target
tramuntana i2c conf 400k              # 400 kHz, board pull-ups on
tramuntana i2c scan                   # address map
tramuntana i2c read 68 75             # register 0x75 of device 0x68
tramuntana spi txn 9f --read 3        # JEDEC id of a SPI flash
tramuntana scope -n 2048 --trigger rising --level 1.65 --plot
tramuntana logic -n 65536 --rate 25M --vcd capture.vcd
tramuntana i2c sniff                  # live bus decode, Ctrl-C to stop
```

The full list is in the [CLI reference](14-cli.md).

### A one-wire self test

Connect **AOUT0 (J1.7) to AIN0 (J1.3)**, then:

```bash
tramuntana awg sine 1k --amp 1 --offset 1.65
tramuntana scope -n 2048 --rate 100k --plot
tramuntana awg off
```

You should see a 1 kHz sine of about 2 V peak-to-peak around 1.65 V.

## 2.6 First session: Python

```python
from tramuntana import Tramuntana

with Tramuntana() as t:                     # finds the board by USB id
    print(t.info())
    t.power(True)
    t.i2c.config(400_000, pullups=True)
    print([hex(a) for a in t.i2c.scan()])

    t.awg.sine(1000, amp=1.0, offset=1.65)
    cap = t.scope.capture(n=2048, rate_hz=100_000, trigger="rising", level=1.65, pre=256)
    print(cap.stats())                      # min, max, vpp, mean, rms, freq
    t.awg.off()
```

Everything the library offers is in the
[Python library reference](13-python-library.md).

## 2.7 First session: the browser

Open `host/webapp/index.html` in Chrome, Edge or another Chromium browser
and press **CONNECT**. The browser lists the board's three ports with the
same name. Pick one; if it isn't the API port, the app says so and you try
the next one. See [Web application](18-webapp.md).

## 2.8 Talking to the target's console

Wire DIO1 to the target's RX, DIO0 to its TX, and GND. Then open the third
port at the target's baud rate:

```bash
picocom -b 115200 /dev/ttyACM2
```

The baud rate, parity and stop bits you open the port with are applied to
the target side. See [UART bridge](09-uart-bridge.md).

## 2.9 Only one program per port

A serial port can be open in one program at a time. If the webapp is
connected, the CLI can't reach the API port, and vice versa. Close one
before using the other. The three ports are independent: you can keep a
terminal on the bridge port while scripts use the API port.

## Where to go next

- Each instrument has a usage guide: start with [SPI](04-spi.md) or
  [I2C](05-i2c.md) if you have a device to talk to.
- To debug a firmware running on the target, read the
  [timeline](12-timeline.md), [sequencer](11-sequencer.md) and
  [emulated device](10-emulated-devices.md) chapters.
- To let an AI agent use the board, see [AI tools](19-ai-tools.md).

[← Overview](01-overview.md) · [Contents](README.md) · [Next: Hardware →](03-hardware.md)
