# 14. Command-line reference: `tramuntana`

[← Python library](13-python-library.md) · [Contents](README.md) · [Next: Shell →](15-shell.md)

The `tramuntana` command comes with the Python package. Install it with
`pip install -e host/python`, or `source host/python/env.sh` to get it in
the current shell without activating anything. It talks to the board's API
port, found automatically by USB id.

```
tramuntana [--port /dev/ttyACMx] [--serial ID] <command> [arguments]
```

| Global option | Meaning |
|---|---|
| `--port` | use this API port instead of autodetecting |
| `--serial` | pick the board with this USB serial number |

**Conventions:** bytes and I2C addresses are hex (`9f 00 ff`, `9f00ff`,
`68`). Frequencies accept `k`/`M` suffixes and exponents (`400k`, `8M`,
`1e6`). Volts are decimal (`1.65`). Masks accept `0x..`, `0b..` or decimal.

**Exit codes:** 0 success, 1 no board found, 2 the board rejected the
command (NAK, busy, bad argument) or a bad value, 3 link error.

## Board

| Command | What it does |
|---|---|
| `tramuntana list` | boards connected and their shell / API / UART ports |
| `tramuntana info` | firmware version, board, clock, chip id, features, protocol version, port |
| `tramuntana status` | owner of each DIO, power, pull-ups, state of every instrument, dropped bytes |
| `tramuntana power [on\|off]` | switch the 3.3 V rail; no argument prints it |
| `tramuntana pullups [on\|off]` | I2C pull-ups on DIO6/DIO7 (alias `pullup`) |
| `tramuntana led <colour>` | `off red green blue yellow cyan magenta white`, or `auto` |
| `tramuntana adc [0\|1]` | one reading of AIN0 (default) or AIN1, in volts |
| `tramuntana display <text…>` | up to 24 characters on the screen's note line |
| `tramuntana pinout [off] [--seconds N] [--hold] [--no-board] [--no-color]` | draw J1 in the terminal and on the board's screen (60 s by default); `off` restores the dashboard; works without a board |
| `tramuntana dio read` | levels of DIO7..DIO0 |
| `tramuntana dio dir <mask> <out_mask>` | directions of the pins in `mask` (1 = output) |
| `tramuntana dio write <mask> <value>` | drive output pins |
| `tramuntana bootloader` | reboot into the UF2 bootloader |

## SPI (DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK)

| Command | What it does |
|---|---|
| `tramuntana spi conf <freq> [mode] [--lsb]` | clock (≤ 75 MHz), mode 0–3, LSB first; prints the actual clock |
| `tramuntana spi sel` / `unsel` | CS low / high |
| `tramuntana spi write <bytes…>` | send, CS untouched |
| `tramuntana spi read <n>` | read n bytes (sends 0x00), CS untouched |
| `tramuntana spi xfer <bytes…>` | full duplex, CS untouched; prints what came back |
| `tramuntana spi txn [bytes…] [--read N]` | CS low, write, read N, CS high |
| `tramuntana spi sniff [--mode M]` | live sniffer, one line per message, Ctrl-C to stop |

## I2C (DIO6 SCL, DIO7 SDA)

| Command | What it does |
|---|---|
| `tramuntana i2c conf <freq> [--no-pullups]` | 1 kHz – 1 MHz; pull-ups on unless `--no-pullups` |
| `tramuntana i2c scan` | address map of 0x08–0x77 |
| `tramuntana i2c read <addr>` | read one byte |
| `tramuntana i2c read <addr> <reg> [n]` | write `reg`, repeated START, read n (default 1); `reg` with 4 hex digits = 16-bit register |
| `tramuntana i2c write <addr> <bytes…>` | write; prints the bytes ACKed |
| `tramuntana i2c regs <addr>` | dump registers 0x00–0xff |
| `tramuntana i2c sniff [--pullups]` | live sniffer, Ctrl-C to stop |
| `tramuntana i2c reset` | bus recovery |

## Oscilloscope (AIN0, AIN1)

`tramuntana scope [options]` captures and prints min / max / Vpp / mean /
RMS / frequency per channel.

| Option | Default | Meaning |
|---|---|---|
| `-n N` | 2048 | samples per channel (16384 in total max) |
| `--ch 1\|2` | 1 | AIN0, or AIN0 + AIN1 |
| `--rate R` | 500k | rate per channel (500 kS/s shared) |
| `--trigger none\|rising\|falling` | none | edge on AIN0 |
| `--level V` | 1.65 | trigger level |
| `--pre N` | 0 | samples before the trigger |
| `--csv file` | | save `t,ch0[,ch1]` |
| `--plot` | | plot with matplotlib |

## Waveform generator (AOUT0)

| Command | What it does |
|---|---|
| `tramuntana awg sine\|tri\|saw <freq> [--amp V] [--offset V]` | periodic shape (defaults 1.0 V peak around 1.65 V) |
| `tramuntana awg square <freq> [--amp V] [--offset V] [--param duty%]` | square, duty in % |
| `tramuntana awg stairs <freq> [--amp V] [--offset V] [--param steps]` | staircase |
| `tramuntana awg dc <volts>` | constant level |
| `tramuntana awg step <from_v> <to_v> <ms>` | `from_v`, then `to_v` after `ms` |
| `tramuntana awg off` / `status` | stop / show what runs |

## PWM outputs

Pairs DIO7/6, DIO5/4, DIO3/2, DIO1/0 share a frequency.

| Command | What it does |
|---|---|
| `tramuntana pwm set <dio> <freq> <duty%> [--invert]` | start or update one pin; prints the achieved frequency |
| `tramuntana pwm phase <dio> <degrees>` | phase of the pin's pair, applied on `sync` |
| `tramuntana pwm sync [mask]` | restart the pairs (default: all) on the same clock edge |
| `tramuntana pwm stop <dio\|all>` | stop |
| `tramuntana pwm status` | every DIO: frequency, duty, phase, polarity |

## Logic analyzer (DIO0..DIO7)

`tramuntana logic [options]` captures and prints the transitions per
channel.

| Option | Default | Meaning |
|---|---|---|
| `-n N` | 16384 | samples (≤ 131072; ≤ 122880 with a pattern) |
| `--rate R` | 150M | sample rate (≤ 15M with a pattern) |
| `--trigger none\|rising\|falling\|pattern` | none | |
| `--pin P` | 0 | DIO for the edge triggers |
| `--mask M`, `--value V` | 0, 0 | pattern: `(sample & mask) == value` |
| `--pre N` | 0 | samples before the trigger (pattern) |
| `--timeout ms` | 5000 | give up without a trigger |
| `--vcd file`, `--csv file` | | save |
| `--plot` | | plot the 8 lanes |

## UART bridge (DIO1 TX, DIO0 RX)

| Command | What it does |
|---|---|
| `tramuntana uart` / `uart status` | active or not, baud, bytes, which port to open, control lines |
| `tramuntana uart auto\|on\|off [--baud B]` | bridge mode; `--baud` forces the target-side rate |
| `tramuntana uart ctrl esp\|direct --reset D --boot D` | RTS/DTR drive the reset/boot DIOs (see [UART bridge](09-uart-bridge.md#92-control-lines-resetboot)) |
| `tramuntana uart ctrl off` | release them |

The data goes through the bridge port: `picocom -b 115200 /dev/ttyACM2`.

## Emulated device

| Command | What it does |
|---|---|
| `tramuntana emu i2c <addr> [--scl D] [--sda D] [--pullups] [--set REG=HEX…]` | answer as an I2C device (default DIO6/DIO7); `--set` (repeatable) loads registers first |
| `tramuntana emu spi [--mode M] [--cs D --sck D --mosi D --miso D] [--set REG=HEX…]` | answer as a SPI device (default command format: bit 7 = read) |
| `tramuntana emu set <reg> <bytes…>` | load registers |
| `tramuntana emu regs [reg] [n]` | dump registers (default 0, 16) |
| `tramuntana emu status` | counters |
| `tramuntana emu log` | the target's transfers, live, Ctrl-C to stop |
| `tramuntana emu off` | stop |

## Timeline and DIO watch

| Command | What it does |
|---|---|
| `tramuntana watch <mask>` | timestamped edges on those DIOs, until Ctrl-C |
| `tramuntana timeline [--watch MASK] [--uart BAUD] [--i2c] [--spi MODE]` | live timeline: UART lines, sniffed messages, DIO edges, emulator transfers on one clock, until Ctrl-C |

The sequencer has no CLI command: use Python or the AI tools.

## AI agents

These work without a board. They need the `[mcp]` extra installed (see
[AI tools](19-ai-tools.md#191-setup)).

| Command | What it does |
|---|---|
| `tramuntana ai install claude\|opencode\|copilot\|all [--user \| --project PATH]` | register the MCP server with the agent and link the `tramuntana-debug` skill; `--user` (default) for every project, `--project PATH` for that directory only |
| `tramuntana ai uninstall claude\|opencode\|copilot\|all [--user \| --project PATH]` | remove them again; the skill stays while another agent still uses the server |

## Examples

```bash
tramuntana power on && tramuntana i2c conf 400k && tramuntana i2c scan
tramuntana spi conf 8M 0 && tramuntana spi txn 9f --read 3
tramuntana scope -n 4096 --trigger rising --level 1.65 --pre 1024 --csv t.csv --plot
tramuntana logic -n 100000 --rate 10M --trigger pattern --mask 0x80 --value 0 --pre 1000 --vcd i2c.vcd
tramuntana uart ctrl esp --reset 3 --boot 2 && esptool --port /dev/ttyACM2 flash_id
tramuntana emu i2c 76 --set d0=60 && tramuntana emu log
tramuntana timeline --watch 0x08 --uart 115200 --i2c
```

[← Python library](13-python-library.md) · [Contents](README.md) · [Next: Shell →](15-shell.md)
