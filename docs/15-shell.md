# 15. Shell reference

[← CLI](14-cli.md) · [Contents](README.md) · [Next: Binary protocol →](16-binary-protocol.md)

The firmware has its own human console on the first serial port. It needs
no host software, just a terminal:

```bash
picocom /dev/ttyACM0          # any baud rate; exit with Ctrl-A Ctrl-X
screen /dev/ttyACM0
```

The prompt is `tramuntana:~$`. `help` lists the commands, `<command> -h`
shows the arguments, and Tab completes. The shell calls the same firmware
functions as the binary API, so both behave the same.

**Conventions:** bytes and I2C addresses are hex without `0x` (`9f 00`,
`68`); other numbers are decimal or `0x..`; volts are decimal (`1.65`).
While a sniffer is on, every message is echoed as one line.

## System

| Command | What it does |
|---|---|
| `info` | firmware version, board, clock, chip id |
| `pins` | owner of each DIO (and whether it is exclusive), and the levels |
| `boot` | reboot into the UF2 bootloader |
| `note <text>` | show a note on the screen |
| `pinout [seconds\|hold\|off]` | J1 drawing on the screen (60 s by default) |
| `led <off\|red\|green\|blue\|yellow\|cyan\|magenta\|white\|auto>` | the RGB LED |
| `power [on\|off]` | target 3.3 V rail |
| `pullups [on\|off]` | I2C pull-ups on DIO6/DIO7 |
| `adc <0\|1>` | one reading of AIN0/AIN1 |
| `time` | the board clock in µs (every event timestamp uses it) |
| `watch <dio_mask\|off>` | report edges on these DIOs as events on the API port |

## GPIO

| Command | What it does |
|---|---|
| `dio dir <mask> <out_mask>` | directions (bit i = DIOi, 1 = output) |
| `dio write <mask> <value>` | drive outputs |
| `dio read` | all levels |

## SPI (DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK)

| Command | What it does |
|---|---|
| `spi conf <hz> [mode 0-3] [lsb]` | clock, mode, bit order |
| `spi sel` / `spi unsel` | CS low / high |
| `spi write <hex..>` | send, CS untouched |
| `spi read <n>` | read n bytes, CS untouched |
| `spi xfer <hex..>` | full duplex, CS untouched |
| `spi txn <read_n> <hex..>` | CS low, write, read `read_n`, CS high |
| `spi sniff <on\|off> [mode]` | passive tap, MOSI + MISO, one line per message |

## I2C (DIO6 SCL, DIO7 SDA)

| Command | What it does |
|---|---|
| `i2c conf <hz> [pu on\|off]` | clock and pull-ups |
| `i2c scan` | probe 08..77 |
| `i2c write <addr> <hex..>` | START, addr+W, bytes, STOP |
| `i2c read <addr> <n>` | START, addr+R, n bytes, STOP |
| `i2c wrrd <addr> <n> <hex..>` | write, repeated START, read n |
| `i2c start <addr_rw>` | START + raw address byte (address << 1 \| R/W), I2CDriver style |
| `i2c raw write <hex..>` | bytes on the open bus |
| `i2c raw read <n>` | bytes from the open bus (NAK on the last) |
| `i2c stop` | STOP |
| `i2c sniff <on\|off> [pu]` | passive decoder |
| `i2c reset` | bus recovery (9 clocks + STOP) |

## Oscilloscope

| Command | What it does |
|---|---|
| `scope <n> [ch 1\|2] [clkdiv>=96] [trig 0\|1\|2] [level 0-4095] [pre]` | capture and print statistics; the rate is 48 MHz / clkdiv (96 = 500 kS/s), the trigger 0 none / 1 rising / 2 falling, the level in raw ADC codes (2048 ≈ 1.65 V) |
| `scope dump [offset] [count]` | raw samples of the last capture |

## Waveform generator

| Command | What it does |
|---|---|
| `awg sine\|tri\|saw <hz> [amp_v] [off_v]` | periodic shapes |
| `awg square <hz> [amp_v] [off_v] [duty%]` | square |
| `awg stairs <hz> [amp_v] [off_v] [steps]` | staircase |
| `awg arb <hz> [amp_v] [off_v]` | play the table uploaded through the API |
| `awg dc <v>` | constant level |
| `awg step <from_v> <to_v> <delay_ms>` | step |
| `awg off` / `awg status` | stop / show |

## PWM

| Command | What it does |
|---|---|
| `pwm set <dio> <hz> <duty%> [inv]` | PWM on a DIO; pairs 7/6, 5/4, 3/2, 1/0 share a frequency |
| `pwm phase <dio> <deg>` | phase delay of the pin's pair, applied on `sync` |
| `pwm sync [dio_mask]` | restart the pairs together (default: all) |
| `pwm stop <dio\|all>` | stop |
| `pwm status` | every output |

## Logic analyzer

| Command | What it does |
|---|---|
| `logic <n> [clkdiv>=1] [trig] [pin] [mask] [value] [pre]` | capture; rate = 150 MHz / clkdiv; trig 0 none, 1 rising, 2 falling, 3 pattern |
| `logic dump [offset] [count]` | samples of the last capture |

## UART bridge

| Command | What it does |
|---|---|
| `uart <auto\|on\|off> [baud]` | bridge mode, optional forced baud rate |
| `uart status` | state, baud rate, bytes |
| `uart ctrl <off\|direct\|esp> [reset_dio] [boot_dio]` | RTS/DTR drive reset/boot pins |

## Emulated device

| Command | What it does |
|---|---|
| `emu i2c <addr> [scl] [sda] [pu]` | answer as an I2C device (default DIO6/DIO7) |
| `emu spi <mode> [cs sck mosi miso] [amask rmask rval status]` | answer as a SPI device (masks in hex) |
| `emu set <reg> <hex..>` | load registers |
| `emu regs [reg] [n]` | dump registers |
| `emu status` | counters |
| `emu off` | stop |

## Sequencer

Programs come from the API (Python, AI tools). The shell can only look:

| Command | What it does |
|---|---|
| `seq status` | state, position, error, result size |
| `seq abort` | stop the running program |

## Zephyr built-ins

The standard Zephyr shell commands are also enabled: `kernel uptime`,
`kernel thread list` / `kernel thread stacks`, `device list`, `date`,
`devmem`, `history`, `clear`, `resize`. They are useful when working on the
firmware itself.

[← CLI](14-cli.md) · [Contents](README.md) · [Next: Binary protocol →](16-binary-protocol.md)
