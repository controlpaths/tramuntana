# ds3231m_rtc

Reads and sets a DS3231M real-time clock on Maxim's DS3231MPMB1 Pmod
module, shows the time live on the board's screen, and checks its 1 Hz
square-wave output with the logic analyzer.

## Module pinout

The DS3231MPMB1 has two connectors ([datasheet][ds]):

**J1, 6-pin Pmod (I2C)**, the one used here:

| Pin | Signal | |
|---|---|---|
| 1 | RST | active-low reset, open drain (pulled low when VCC drops) |
| 2 | INT/SQW | active-low alarm interrupt or 1 Hz square wave, open drain |
| 3 | SCL | I2C clock |
| 4 | SDA | I2C data |
| 5 | GND | |
| 6 | VCC | supply |

**J2, 2x4 (I2C expansion)**, to daisy-chain other I2C modules: pins 1-4
and 5-8 are each SCL, SDA, GND, VCC.

The module carries 4.7 kΩ pull-ups to VCC on its lines and 150 Ω series
resistors, plus a coin cell holder. **The datasheet says the battery must
be fitted for the module to work.**

[ds]: https://download.kamami.pl/p198546-ds3231mpmb1_maxim.pdf

## Wiring

The Tramuntana's J1 is not a Pmod connector, so use jumper wires:

| Module J1 | Tramuntana J1 |
|---|---|
| 6 VCC | 1 (3V3) |
| 5 GND | 2 (GND) |
| 3 SCL | 15 (DIO6) |
| 4 SDA | 16 (DIO7) |
| 2 INT/SQW | 9 (DIO0), only for `sqw` |
| 1 RST | not connected |

The board's I2C pull-ups stay off: the module has its own, and with both
on and the 3V3 rail off the module would be powered through SCL/SDA.

## Usage

```bash
python3 ds3231m_rtc.py read                         # time, temperature, flags
python3 ds3231m_rtc.py set                          # from this computer's clock
python3 ds3231m_rtc.py set 2026-10-03T18:30:00      # a given time
python3 ds3231m_rtc.py watch                        # live, until Ctrl-C
python3 ds3231m_rtc.py sqw                          # measure the 1 Hz output
```

| Option | Default | |
|---|---|---|
| `--sqw-dio` | 0 | DIO wired to INT/SQW, for `sqw` |
| `--pullups` | off | enable the board's pull-ups (a module without its own) |

`read` prints the RTC time next to the computer's and the difference in
seconds, the die temperature, and a warning if the oscillator-stop flag
says the time is not valid.

## How it works

The DS3231M answers at the fixed address 0x68 and keeps everything in
8-bit registers:

| Register | Contents |
|---|---|
| 0x00-0x06 | seconds, minutes, hours, weekday, date, month (bit 7 = century), year, all BCD |
| 0x0E | control: CONV starts a temperature conversion, INTCN picks alarm interrupt (1) or 1 Hz square wave (0) on INT/SQW |
| 0x0F | status: OSF (oscillator stopped), BSY (conversion running) |
| 0x11-0x12 | temperature: signed degrees + 0.25 C steps in bits 7:6 of 0x12 |

- **Read**: the seven time registers in one burst read. The chip latches
  them at the START of the transfer, so seconds can't roll over halfway
  through. Hours are decoded in both 12- and 24-hour mode.
- **Set**: the seven registers in one write, in 24-hour mode, and then OSF
  is cleared. Without a time on the command line, the script waits for
  the computer's clock to start a new second and writes it then, so the
  RTC ends up in step with the computer to well under a second.
- **Temperature**: the chip measures every 64 s on its own; `read` sets
  CONV for a fresh reading and polls until it clears. The sensor is there
  to compensate the MEMS resonator, it reads the die, not the room.
- **sqw**: clears INTCN so INT/SQW outputs 1 Hz, captures 3.5 s of DIO0
  at 10 kS/s with the logic analyzer triggered on a falling edge, and
  takes the period from the next falling edge (0.1 ms resolution). INTCN
  is set back afterwards.
- **watch**: polls the time every 100 ms and prints it, with the last
  temperature, whenever the second changes; it also goes to the board's
  screen.

## Troubleshooting

- *no DS3231M at 0x68*: check the wiring and that the coin cell is in.
  A timeout on the scan means nothing pulls the bus up: the module is
  not powered or SCL/SDA are not connected.
- *oscillator-stop flag set*: the clock stopped at some point (first
  power-up, battery removed or flat). Run `set`.
