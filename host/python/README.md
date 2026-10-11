# tramuntana — Python library, CLI and MCP server

Drive a Tramuntana Lite board from scripts: SPI/I2C master and sniffers,
oscilloscope, waveform generator, logic analyzer and the UART bridge, over
the board's binary API port. Pure Python 3.8+, only depends on `pyserial`.
The same package carries an MCP server and an agent skill, so an AI agent
can use the board too (see [AI agents](#ai-agents)).

```bash
cd host/python
python3 -m venv .venv
. .venv/bin/activate
pip install -e .              # library + `tramuntana` command
pip install -e .[plot]        # + matplotlib/numpy for --plot
pip install -e .[mcp]         # + the MCP server for AI agents (includes [plot])
```

Or, without activating anything, source `env.sh` from any directory: it
creates `.venv` the first time if it's missing and defines a `tramuntana`
shell function pointing at `.venv/bin/tramuntana`, for that shell only:

```bash
source host/python/env.sh
tramuntana i2c scan
```

On Linux make sure you are in the `dialout` group (or add a udev rule) so
`/dev/ttyACM*` is writable.

## Library

```python
from tramuntana import Tramuntana

with Tramuntana() as t:                 # finds the board by USB VID/PID
    print(t.info())
    t.power(True)                       # 3.3 V to the target

    # I2C, I2CDriver style
    t.i2c.config(400_000, pullups=True)
    print(t.i2c.scan())                 # [0x68, ...]
    who = t.i2c.reg_read(0x68, 0x75)    # write reg, repeated START, read 1
    t.i2c.start(0x68); t.i2c.raw_write(b"\x6b\x00"); t.i2c.stop()

    # SPI, SPIDriver style
    t.spi.config(8_000_000, mode=0)
    jedec = t.spi.txn(b"\x9f", read_n=3)        # sel, write, read, unsel
    t.spi.sel(); t.spi.write(b"\x03\x00\x00\x00"); page = t.spi.read(256); t.spi.unsel()

    # Scope with pre-trigger
    cap = t.scope.capture(n=4096, channels=1, rate_hz=500_000,
                          trigger="rising", level=1.65, pre=1024)
    print(cap.stats())                  # min/max/vpp/mean/rms/freq
    cap.to_csv("trace.csv")

    # Waveform generator
    t.awg.sine(1000, amp=1.0, offset=1.65)
    t.awg.arbitrary([__import__("math").sin(i / 10) for i in range(63)], 200)
    t.awg.off()

    # Logic analyzer -> PulseView / GTKWave
    lc = t.logic.capture(n=100_000, rate_hz=10_000_000, trigger="pattern",
                         mask=0x80, value=0x00, pre=1000)   # SDA (DIO7) falls
    lc.to_vcd("i2c.vcd")
    print(lc.edges(6)[:5])              # (sample, level) transitions on SCL

    # Sniffers deliver events
    t.i2c.sniff(True)
    for msg in t.events(timeout=5):
        print(msg)                      # I2C S 68W+ 75+  S 68R+ 71- P
    t.i2c.sniff(False)
```

Every method maps 1:1 onto a command in `proto.py` (see the firmware
README for the wire format). Errors come back as `ProtoError` (the board
said no: NAK, busy, bad argument…) or `LinkError` (no reply, wrong port).

### Objects

The full reference - every method, argument, unit, limit, return value
and error - is in [API.md](API.md) (and in the docstrings: `help(t.spi)`).

- `Tramuntana(port=None, serial_number=None)` — `info() status() power()
  pullups() led() dio_dir() dio_write() dio_read() adc() display()
  display_pinout() bootloader() events() on_event()`
- `t.spi` — `config sel unsel write read xfer txn sniff`
- `t.i2c` — `config scan write read write_read reg_read reg_write start stop
  raw_write raw_read reset_bus sniff`
- `t.scope` — `arm status wait read capture abort` → `ScopeCapture`
  (`raw[ch]`, `volts()`, `times()`, `stats()`, `to_csv()`)
- `t.awg` — `set sine square dc step arbitrary off status`
- `t.pwm` — `set phase sync stop status`: hardware PWM on the DIOs
- `t.logic` — `arm status wait read capture abort` → `LogicCapture`
  (`samples`, `channel()`, `edges()`, `to_vcd()`, `to_csv()`)
- `t.uart` — `config status` (the data itself flows on the bridge port:
  open `find_ports()[0]["uart"]` with pyserial at whatever baud you need)
- `Tramuntana` also has `dio_watch() time_us() event_stamps()`, and
  `t.uart.ctrl()` maps the bridge port's RTS/DTR to reset/boot pins
- `t.emu` — `i2c spi stop set_regs regs read_only status`: the board as
  an I2C/SPI device on the target's bus, from a 256-byte register file
- `t.seq` — `run start status result abort` with a `Sequence` of steps
  (GPIO, delays, waits, bus transfers, captures) run on the board with
  microsecond timing
- `Timeline` / `UartTap` — the target's UART, sniffers, DIO edges,
  emulator transfers and host notes on one clock
- `tramuntana.decode` — `uart guess_baud uart_text i2c spi measure`:
  UART/I2C/SPI decoders over a `LogicCapture` and oscilloscope measurements
  (levels, rise/fall time, frequency, duty, overshoot) over a `ScopeCapture`

## CLI

```bash
tramuntana list                                  # boards and their three ports
tramuntana info | status | power on | pullups on | adc 0 | led blue
tramuntana pinout                    # J1 drawing here + on the board's screen (--hold, off)
tramuntana i2c scan
tramuntana i2c read 68 75            # register 0x75 of 0x68
tramuntana i2c read 50 0010 2       # 2 bytes at 16-bit address 0x0010 (24LC32 and up)
tramuntana i2c regs 68               # dump 0x00..0xff
tramuntana i2c write 68 6b 00
tramuntana i2c sniff                 # live, Ctrl-C to stop
tramuntana spi conf 8M 0
tramuntana spi txn 9f --read 3       # JEDEC id of a flash
tramuntana spi sniff
tramuntana scope -n 4096 --trigger rising --level 1.65 --pre 1024 --csv t.csv --plot
tramuntana awg sine 1k --amp 1 --offset 1.65
tramuntana logic -n 100000 --rate 10M --trigger pattern --mask 0x80 --value 0 --pre 1000 --vcd i2c.vcd
tramuntana logic -n 131072 --rate 150M --trigger rising --pin 5 --plot
tramuntana uart on --baud 9600 && picocom -b 9600 /dev/ttyACM2
tramuntana uart ctrl esp --reset 3 --boot 2   # RTS/DTR -> EN/GPIO0: esptool works on /dev/ttyACM2
tramuntana watch 0x09                # timestamped edges on DIO0 and DIO3
tramuntana emu i2c 76 --set d0=60    # answer as a device at 0x76 (on DIO6/DIO7)
tramuntana emu regs f4 2             # what the target wrote
tramuntana timeline --watch 0x08 --uart 115200 --i2c   # UART + I2C + DIO3 edges, live
tramuntana bootloader
tramuntana ai install claude         # MCP server + skill for an AI agent (see below)
```

Bytes and addresses are hex; frequencies accept `k`/`M` suffixes. The full
reference of every command and option is in the
[repository README](../../readme.md#tramuntana-command-reference).

## AI agents

`tramuntana.mcp_server` exposes the board as MCP tools, and the
`tramuntana-debug` skill (shipped in `tramuntana/skills/`) teaches an agent
how to debug with them. Install the `[mcp]` extra, then register both with
your agent:

```bash
tramuntana ai install claude                     # Claude Code, every project
tramuntana ai install opencode                   # opencode
tramuntana ai install copilot                    # GitHub Copilot in VS Code
tramuntana ai install all                        # every agent whose CLI is installed
tramuntana ai install claude --project ~/fw/foo  # only for that project
tramuntana ai uninstall all                      # undo (same --user/--project)
```

The server is registered as `<this venv's python> -m tramuntana.mcp_server`,
and the skill is linked into `~/.claude/skills/` (or `PATH/.claude/skills/`
with `--project`), where Claude Code, opencode and Copilot all find it. Run
`tramuntana ai uninstall` before removing the package. Any other MCP client:
run `tramuntana-mcp` (stdio). Tools, design and the skill are described in
[AI tools](../../docs/19-ai-tools.md).

## Examples

[`examples/`](examples/) has one folder per example, each with its script
and a README (wiring, usage, how it works):

| Example | |
|---|---|
| [`board_test`](examples/board_test/) | functional test of a board: flash, six jumpers, every instrument |
| [`ds3231m_rtc`](examples/ds3231m_rtc/) | DS3231M RTC Pmod: read, set from the PC clock, live view, 1 Hz output check |
| [`eeprom`](examples/eeprom/) | read / write a 24xx I2C EEPROM from / to a `.bin` file |
| [`hcsr04_range`](examples/hcsr04_range/) | live distance from an HC-SR04 ultrasonic sensor, timed with the logic analyzer |
| [`i2c_regs`](examples/i2c_regs/) | dump the first 32 registers of every I2C device |
| [`logic_to_vcd`](examples/logic_to_vcd/) | logic capture triggered on an I2C START, saved as VCD |
| [`nrf24_ble_scan`](examples/nrf24_ble_scan/) | passive BLE scanner with an nRF24L01 on SPI |
| [`scope_plot`](examples/scope_plot/) | AWG sine captured by the scope and plotted with matplotlib |
| [`sniff_log`](examples/sniff_log/) | log every I2C / SPI message on the header |
| [`spi_flash_id`](examples/spi_flash_id/) | JEDEC ID and first page of a SPI NOR flash |
| [`ssd1306_oled`](examples/ssd1306_oled/) | 128x32 SSD1306 OLED over I2C with a live AIN0 readout |
| [`vl53l0x_range`](examples/vl53l0x_range/) | VL53L0X time-of-flight distance, with ST's init sequence |
