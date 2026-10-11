# Tramuntana — hardware hacking tool for embedded systems

An open bench tool for analysing and debugging embedded systems, built on
the RP2350 (Raspberry Pi Pico 2) and Zephyr OS. One USB cable, three serial
ports, one 16-pin header:

- **SPI master and sniffer** — SPIDriver-style `sel/write/read/unsel`
  transactions up to 75 MHz; passive full-duplex (MOSI + MISO) bus tap.
- **I2C master and sniffer** — I2CDriver-style `start/write/read/stop`
  primitives, scan, register access, bus recovery; passive decoder.
- **Oscilloscope** — 2 × 12-bit channels, 500 kS/s, pre-trigger.
- **Waveform generator** — sine/tri/saw/square/stairs/DC/arbitrary, 1 Hz–100 kHz.
- **Logic analyzer** — 8 channels, 150 MS/s, edge triggers in hardware,
  pattern trigger with pre-trigger, VCD export for PulseView.
- **USB-UART bridge** — the target's console as `/dev/ttyACM2`, baud rate
  follows whatever you open it with; RTS/DTR can drive the target's reset
  and boot pins, so esptool and friends flash through it.
- **Emulated I2C/SPI device** — the board answers on the target's bus from
  a register file the host loads and reads back.
- **Sequencer** — host-built step lists (GPIO, waits, bus transfers,
  captures) run on the board with microsecond timing.
- **Timeline** — the target's UART, sniffed buses, DIO edges and emulator
  transfers timestamped on one clock (host library, CLI, AI tools).
- Switched, protected 3.3 V rail for the target, switchable I2C pull-ups,
  RGB status LED and a TFT dashboard.

The passive instruments (sniffers, logic analyzer) only listen, so they can
watch a bus while the board's own master, the UART bridge or a target
drives it.

Two boards share the design: `tramuntana_r1a` (RP2350 on the PCB) and
`tramuntana_lite_r1a` (a Pico 2 soldered onto a two-layer carrier — the one
handed out at workshops). Firmware and host software are common.

## Documentation

The full documentation is in [`docs/`](docs/README.md): getting started,
a usage guide for every instrument, and complete references for the
Python library, the CLI, the shell, the binary protocol, the firmware
internals, the webapp and the AI tools.

## Repository

```
fw/apps/tramuntana_fw/   Zephyr firmware                                -> its README
fw/boards/               devicetree for the board
fw/imgs/                 display images + font, with their generators
fw/build.sh              build / flash wrapper (local paths in fw/.env)
host/python/             Python package: library, `tramuntana` CLI, MCP server
                         and skill for AI agents                         -> its README
host/webapp/             browser front-end (Web Serial, no install)     -> its README
hw/tramuntana_lite_r1a/  KiCad project, hardware notes, 3D-printed case
hw/tramuntana_r1a/       KiCad project of the full board
docs/                    documentation                           -> its README
```

## Quick start

1. Flash `zephyr.uf2` from the latest GitHub release (hold BOOTSEL while
   plugging the Pico 2 in, copy the file to the `RP2350` drive), or build it
   yourself — see below. Three ports appear: shell, API, UART bridge.
2. Talk to it:

```bash
picocom /dev/ttyACM0           # the shell: try 'help', 'i2c scan', 'scope 1024'
source host/python/env.sh      # `tramuntana` command for this shell (creates the venv once)
tramuntana info
tramuntana i2c scan
tramuntana scope -n 2048 --trigger rising --level 1.65 --plot
tramuntana logic -n 65536 --rate 25M --vcd capture.vcd
picocom -b 115200 /dev/ttyACM2 # the target's UART, through the bridge
```

or open `host/webapp/index.html` in Chrome/Edge and press **Connect**.

3. Or hand it to an AI agent. The package carries an MCP server and the
   `tramuntana-debug` skill; one command registers both:

```bash
python3 -m venv ~/.venvs/tramuntana
~/.venvs/tramuntana/bin/pip install "./host/python[mcp]"
~/.venvs/tramuntana/bin/tramuntana ai install claude     # or opencode, copilot, all
# --project PATH registers it for that project only; 'ai uninstall' undoes it
```

Then ask: *"Power the target, scan the I2C bus and tell me what the devices
are."* See [AI tools](docs/19-ai-tools.md).

## `tramuntana` command reference

`source host/python/env.sh` defines the command in the current shell (it
runs `host/python/.venv/bin/tramuntana`, creating the venv the first time).
It talks to the board's API port, found automatically by USB id.

Conventions: bytes and I2C addresses are **hex** (`9f 00 ff`, `9f00ff`,
`68`); frequencies accept `k`/`M` suffixes (`400k`, `8M`, `1e6`); volts are
decimal (`1.65`). Exit code 0 on success, 1 if no board is found, 2 if the
board rejected the command (NAK, busy, bad argument…), 3 on a link error.

### Global options

| Option | Meaning |
|---|---|
| `--port /dev/ttyACMx` | use this API port instead of autodetecting |
| `--serial <id>` | pick the board with this USB serial number (several boards plugged in) |

### Board

| Command | What it does |
|---|---|
| `tramuntana list` | list connected boards and their shell / API / UART ports |
| `tramuntana info` | firmware version, board, system clock, chip id, protocol version, port |
| `tramuntana status` | who owns each DIO pin, power, pull-ups, state of every instrument, API bytes dropped |
| `tramuntana power [on\|off]` | switch the target's 3.3 V rail (J1.1); no argument prints its state |
| `tramuntana pullups [on\|off]` | I2C pull-up bias on DIO6/DIO7; no argument prints its state |
| `tramuntana led <colour>` | `off red green blue yellow cyan magenta white`, or `auto` to give it back to the firmware |
| `tramuntana adc [0\|1]` | one reading of AIN0/AIN1 in volts (default AIN0) |
| `tramuntana display <text…>` | show up to 24 characters on the board's screen |
| `tramuntana pinout [off] [--seconds N\|--hold]` | draw J1 (every pin's name and function) in the terminal and on the board's screen, 60 s by default; works without a board (terminal only) |
| `tramuntana dio read` | levels of DIO7..DIO0 |
| `tramuntana dio dir <mask> <out_mask>` | set direction of the pins in `mask` (bit i = DIOi, 1 = output) |
| `tramuntana dio write <mask> <value>` | drive the output pins in `mask` (set them as outputs first) |
| `tramuntana bootloader` | reboot into the UF2 bootloader (the `RP2350` drive appears) |

`dio` masks accept `0x..`, `0b..` or decimal (`0x0f`, `0b1010`, `15`).

### SPI — DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK

| Command | What it does |
|---|---|
| `tramuntana spi conf <freq> [mode] [--lsb]` | clock (up to 75 MHz), mode 0–3 (default 0), LSB first; prints the actual clock |
| `tramuntana spi sel` | CS low |
| `tramuntana spi unsel` | CS high |
| `tramuntana spi write <bytes…>` | send bytes, CS untouched |
| `tramuntana spi read <n>` | read n bytes (sends 0x00), CS untouched |
| `tramuntana spi xfer <bytes…>` | full duplex: send bytes, print what came back, CS untouched |
| `tramuntana spi txn [bytes…] [--read N]` | whole transaction: CS low, write bytes, read N, CS high |
| `tramuntana spi sniff [--mode M]` | passive tap of MOSI and MISO, one line per CS-low message, until Ctrl-C (mode defaults to the one set with `conf`) |

```bash
tramuntana spi conf 8M 0
tramuntana spi txn 9f --read 3                  # JEDEC id of a SPI flash
tramuntana spi txn 03 00 00 00 --read 256       # first page of the flash
tramuntana spi sel && tramuntana spi write 06 && tramuntana spi unsel   # write enable, step by step
```

### I2C — DIO6 SCL, DIO7 SDA

| Command | What it does |
|---|---|
| `tramuntana i2c conf <freq> [--no-pullups]` | bus clock (1 kHz–1 MHz); turns the board's pull-ups on unless `--no-pullups` |
| `tramuntana i2c scan` | probe 0x08–0x77 and print the address map |
| `tramuntana i2c read <addr>` | read 1 byte from a device |
| `tramuntana i2c read <addr> <reg> [n]` | register read: write `reg`, repeated START, read n bytes (default 1). `reg` takes 2 hex digits per byte, so `0010` is a 16-bit address (24LC32 and up, VL53L1X…) |
| `tramuntana i2c write <addr> <bytes…>` | START, address, bytes, STOP; prints how many bytes were ACKed |
| `tramuntana i2c regs <addr>` | dump registers 0x00–0xff (16 at a time; blocks that NAK show as `ff`) |
| `tramuntana i2c sniff [--pullups]` | passive decoder, one line per START…STOP (`S 68W+ 75+  S 68R+ 71- P`), until Ctrl-C |
| `tramuntana i2c reset` | bus recovery: up to 9 SCL pulses and a STOP to free a stuck slave |

```bash
tramuntana i2c conf 400k
tramuntana i2c scan
tramuntana i2c read 68 75              # WHO_AM_I of an MPU-6050
tramuntana i2c write 68 6b 00          # wake it up
tramuntana i2c regs 50                 # dump an EEPROM's first 256 bytes
```

The low-level primitives (`start`, raw write/read, `stop`) are in the shell,
the Python library and the webapp; the CLI covers complete transactions.

### Oscilloscope — AIN0 (and AIN1)

`tramuntana scope [options]` captures, prints per-channel min / max / Vpp /
mean / RMS / frequency, and optionally saves or plots.

| Option | Default | Meaning |
|---|---|---|
| `-n N` | 2048 | samples per channel (16384 in total max) |
| `--ch 1\|2` | 1 | AIN0 only, or AIN0 + AIN1 |
| `--rate R` | 500k | sample rate per channel (500 kS/s total max, shared between channels) |
| `--trigger none\|rising\|falling` | none | edge on AIN0 |
| `--level V` | 1.65 | trigger level in volts |
| `--pre N` | 0 | samples kept before the trigger |
| `--csv file` | | save `t,ch0[,ch1]` in volts, t = 0 at the trigger |
| `--plot` | | show it with matplotlib (`host/python/.venv/bin/pip install matplotlib`) |

```bash
tramuntana scope -n 4096 --trigger rising --level 1.65 --pre 1024 --plot
tramuntana scope --ch 2 --rate 100k --csv two_channels.csv
```

### Waveform generator — AOUT0

| Command | What it does |
|---|---|
| `tramuntana awg sine <freq> [--amp V] [--offset V]` | sine; amplitude is the peak around the offset (defaults 1.0 V around 1.65 V) |
| `tramuntana awg tri <freq> [--amp V] [--offset V]` | triangle |
| `tramuntana awg saw <freq> [--amp V] [--offset V]` | sawtooth (rising ramp) |
| `tramuntana awg square <freq> [--amp V] [--offset V] [--param duty%]` | square, duty cycle in % (default 50) |
| `tramuntana awg stairs <freq> [--amp V] [--offset V] [--param steps]` | staircase with that many steps (default 8) |
| `tramuntana awg dc <volts>` | constant level |
| `tramuntana awg step <from_v> <to_v> <ms>` | hold `from_v`, then jump to `to_v` after `ms` |
| `tramuntana awg off` | stop, output low |
| `tramuntana awg status` | what is running |

Frequencies go from 1 Hz to 100 kHz; the command prints the frequency
actually achieved. Arbitrary waveforms are loaded from the Python library
(`t.awg.arbitrary(...)`) or the webapp.

### PWM outputs — any DIO

| Command | What it does |
|---|---|
| `tramuntana pwm set <dio> <freq> <duty%> [--invert]` | PWM on a DIO; pairs 7/6, 5/4, 3/2, 1/0 share a frequency |
| `tramuntana pwm phase <dio> <deg>` / `pwm sync [mask]` | phase per pair, restart the pairs together |
| `tramuntana pwm stop <dio\|all>` / `pwm status` | stop / show every output |

### Logic analyzer — DIO0..DIO7

`tramuntana logic [options]` captures, prints the number of transitions per
channel, and optionally saves or plots.

| Option | Default | Meaning |
|---|---|---|
| `-n N` | 16384 | samples (131072 max, 122880 with a pattern trigger) |
| `--rate R` | 150M | sample rate (150 MS/s max; 15 MS/s max with a pattern trigger) |
| `--trigger none\|rising\|falling\|pattern` | none | edge on one pin, or a pattern on several |
| `--pin P` | 0 | DIO used by `rising`/`falling` |
| `--mask M` / `--value V` | 0 / 0 | pattern trigger: fires when `(sample & mask) == value` (bit i = DIOi) |
| `--pre N` | 0 | samples kept before the trigger (pattern trigger only) |
| `--timeout ms` | 5000 | give up if the trigger doesn't come |
| `--vcd file` | | save for PulseView / GTKWave |
| `--csv file` | | save `t,DIO0..DIO7`, t = 0 at the trigger |
| `--plot` | | show the 8 lanes with matplotlib |

```bash
tramuntana logic -n 131072 --rate 150M --trigger rising --pin 5 --vcd spi.vcd
tramuntana logic -n 100000 --rate 10M --trigger pattern --mask 0x80 --value 0 --pre 2000 --vcd i2c.vcd   # I2C START (SDA low)
```

### UART bridge — DIO1 TX, DIO0 RX

| Command | What it does |
|---|---|
| `tramuntana uart` / `tramuntana uart status` | bridge active or not, baud rate, bytes received / sent, and which port to open |
| `tramuntana uart auto` | (default) bridge on while a program has the bridge port open |
| `tramuntana uart on [--baud B]` | force it on; `--baud` overrides the rate the host sets on the port |
| `tramuntana uart off` | force it off, DIO0/DIO1 free for plain GPIO |

The data itself goes through the third serial port, e.g.
`picocom -b 115200 /dev/ttyACM2` (the baud rate you open it with is the one
used on the target, unless overridden with `--baud`).

| Command | What it does |
|---|---|
| `tramuntana uart ctrl esp --reset R --boot B` | RTS/DTR of the bridge port drive DIO R / DIO B (open-drain) with the ESP32/ESP8266 auto-reset logic: `esptool --port /dev/ttyACM2 ...` resets the target into its ROM loader by itself |
| `tramuntana uart ctrl direct --reset R --boot B` | reset follows RTS, boot follows DTR (an adapter wired straight to the pins) |
| `tramuntana uart ctrl off` | release them |

### Emulated device, timeline, DIO watch

| Command | What it does |
|---|---|
| `tramuntana emu i2c ADDR [--scl D --sda D] [--pullups] [--set REG=HEX..]` | answer as an I2C device at ADDR (default on DIO6/DIO7), registers loaded first |
| `tramuntana emu spi [--mode M] [--cs --sck --mosi --miso D] [--set REG=HEX..]` | answer as a SPI device (bit 7 of the command byte = read) |
| `tramuntana emu set REG HEX..` / `emu regs [REG] [N]` | load / dump registers |
| `tramuntana emu status` / `emu log` / `emu off` | counters / live transfers / stop |
| `tramuntana watch MASK` | timestamped edges on the DIOs in MASK, until Ctrl-C |
| `tramuntana timeline [--watch MASK] [--uart BAUD] [--i2c] [--spi MODE]` | live timeline: the target's UART lines, sniffed messages, DIO edges and emulator transfers on one clock |

The sequencer has no CLI front: build a `Sequence` in Python
([API](host/python/API.md#tseq--sequencer)) or use the AI tools'
`sequence_run`.

## Building the firmware

A Zephyr workspace (per the
[getting started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html))
and the Zephyr SDK, plus `device-tree-compiler` on the host. `fw/` is not
its own west workspace; `build.sh` builds an app against that install.
Tell it where they are in `fw/.env` (defaults: `~/zephyrproject` and
`~/zephyr-sdk-1.0.1`):

```bash
cd fw
cp .env.example .env             # then edit ZEPHYRPROJECT / ZEPHYR_SDK
./build.sh                       # builds tramuntana_fw
./build.sh flash                 # build + flash (reboots the board into the
                                 # UF2 bootloader over USB, no BOOTSEL needed)
```

Only the first flash — or recovering from a hung firmware — needs BOOTSEL
held while plugging the board in.

## Header

```
 1 3V3 (switched)   2 GND       3 AIN0     4 GND      5 AIN1    6 GND
 7 AOUT0            8 VREF      9 DIO0    10 DIO1    11 DIO2   12 DIO3
13 DIO4            14 DIO5     15 DIO6    16 DIO7

DIO0/1 = UART RX/TX    DIO2..5 = SPI CS / MISO / MOSI / SCK    DIO6/7 = I2C SCL/SDA
DIO0..7 = GPIO and logic analyzer channels 0..7   (`tramuntana pinout` draws it)
```

## License

| Part | License |
|---|---|
| Firmware (`fw/`), host software (`host/`: library, CLI, MCP server, webapp) | [MIT](LICENSES/MIT.txt) |
| Hardware (`hw/`: schematics, PCB, fabrication files, case) | [CERN-OHL-P-2.0](LICENSES/CERN-OHL-P-2.0.txt) |
| Documentation (`docs/`, READMEs, images) | [CC-BY-4.0](LICENSES/CC-BY-4.0.txt) |

See [LICENSE](LICENSE).
