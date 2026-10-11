# 19. AI tools

[← Web application](18-webapp.md) · [Contents](README.md) · [Next: Examples →](20-examples.md)

Tramuntana can be the hands and eyes of an AI agent (Claude Code,
opencode, GitHub Copilot or any MCP client). The agent powers the target,
talks to it, listens to its buses and console, measures its signals and
draws conclusions, with you approving the risky steps. Two pieces make
that work, both shipped in the Python package
([`host/python/tramuntana`](../host/python/tramuntana)):

| Piece | What it is |
|---|---|
| **MCP server** (`tramuntana/mcp_server.py`) | exposes the board as [Model Context Protocol](https://modelcontextprotocol.io) tools: one tool per operation, with typed, documented arguments. It is a thin layer over the Python library |
| **Skill** (`tramuntana/skills/tramuntana-debug/`) | teaches the agent *how* to debug with those tools: safety rules, a workflow, and playbooks per symptom (I2C, SPI, UART, power and signals, emulation and sequencing) |

## 19.1 Setup

Install the package with the `[mcp]` extra (the MCP SDK plus matplotlib
for the images), in a virtual environment of its own:

```bash
python3 -m venv ~/.venvs/tramuntana
~/.venvs/tramuntana/bin/pip install "./host/python[mcp]"
```

Then register the server and the skill with your agent (`tramuntana` is
`~/.venvs/tramuntana/bin/tramuntana` unless that environment is active):

```bash
tramuntana ai install claude                       # Claude Code
tramuntana ai install opencode                     # opencode
tramuntana ai install copilot                      # GitHub Copilot in VS Code
tramuntana ai install all                          # every agent whose CLI is installed
tramuntana ai install claude --project ~/fw/foo    # only for that project
```

`--user` (the default) makes it available from every project; `--project
PATH` writes the configuration into that directory only. `tramuntana ai
uninstall` takes the same arguments and undoes it.

| Agent | `--user` | `--project PATH` |
|---|---|---|
| `claude` | `claude mcp add --scope user` → `~/.claude.json` | `PATH/.mcp.json` |
| `opencode` | `~/.config/opencode/opencode.json[c]` | `PATH/opencode.json[c]` |
| `copilot` | the VS Code profile's `mcp.json` (`~/.config/Code/User/`) | `PATH/.vscode/mcp.json` |
| skill | `~/.claude/skills/tramuntana-debug` | `PATH/.claude/skills/tramuntana-debug` |

Some details:

- The server is registered as `<python> -m tramuntana.mcp_server`, with the
  interpreter of the environment the package is installed in. Install it
  in a new environment and run `tramuntana ai install` again: the entry is
  replaced.
- The skill is a symlink to the copy inside the package, so it follows
  upgrades. Run `tramuntana ai uninstall` before removing the package.
- `uninstall` keeps the skill while another agent still uses the server in
  the same place.
- opencode and VS Code have no command to remove a server, so their JSON
  files are edited directly. A file with comments (`opencode.jsonc`) keeps
  them: the entry is cut out as text.
- With `all`, an agent whose CLI isn't installed is skipped with a note
  (`--project` installs for opencode and Copilot only write a file and need
  no CLI).

Check the result with `claude mcp list`, `opencode mcp list`, or in VS
Code with **MCP: List Servers** (Copilot Chat must be in Agent mode).

For any other MCP client, run it as a stdio server. The package installs a
`tramuntana-mcp` command for that:

```json
{
  "command": "/home/you/.venvs/tramuntana/bin/tramuntana-mcp",
  "args": []
}
```

On Linux, your user must be able to open `/dev/ttyACM*`.

The server sends the J1 pinout and the conventions to the agent as MCP
*instructions*, so the agent knows which pin is which without being told.
The connection opens on the first tool call and reopens by itself after a
link error (a replugged board, new firmware).

## 19.2 Tools

| Group | Tools |
|---|---|
| Board | `list_boards`, `connect(port?, serial_number?)`, `disconnect`, `board_info`, `board_status`, `power(on?)`, `pullups(on?)`, `led(colour)`, `display_text(text)`, `display_pinout(seconds?)`, `adc_read(channel?)`, `bootloader` |
| GPIO | `dio_read`, `dio_set_direction(mask, out_mask)`, `dio_write(mask, value)` |
| SPI master | `spi_config(freq_hz?, mode?, lsb_first?)`, `spi_transaction(write, read_n?)`, `spi_xfer(data, manage_cs?)`, `spi_select(active)`, `spi_write(data)`, `spi_read(n, fill?)` |
| I2C master | `i2c_config(freq_hz?, pullups?)`, `i2c_scan`, `i2c_write(address, data)`, `i2c_read(address, n)`, `i2c_write_read(address, write, n)`, `i2c_sequence(steps)` (low-level start/write/read/stop list), `i2c_bus_reset` |
| Sniffers | `sniff_start(bus, spi_mode?, i2c_pullups?)`, `sniff_read(duration_s?, max_messages?)`, `sniff_stop(bus?)` |
| Scope | `scope_capture(n?, channels?, rate_hz?, trigger?, level_v?, pre?, timeout_ms?, wait?, max_points?, csv_path?)`, `scope_read(...)`, `scope_image(t_from_us?, t_to_us?)` |
| Waveform generator | `awg_set(shape, freq_hz, amp_v?, offset_v?, param?)`, `awg_arbitrary(samples, freq_hz, ...)`, `awg_dc(volts)`, `awg_step(from_v, to_v, delay_ms)`, `awg_off`, `awg_status` |
| Logic analyzer | `logic_capture(n?, rate_hz?, trigger?, pin?, mask?, value?, pre?, timeout_ms?, wait?, channels?, max_edges?, vcd_path?, csv_path?, names?, decode?, max_items?)`, `logic_read(...)`, `logic_decode(protocol, ...)`, `logic_image(t_from_us?, t_to_us?, channels?, names?)` |
| UART bridge | `uart_open(baud?, parity?, stop_bits?, dtr?, rts?)`, `uart_send(data, as_hex?, newline?, wait_s?, until?)`, `uart_read(timeout_s?, until?, max_bytes?)`, `uart_close`, `uart_status`, `uart_ctrl(mode?, reset_dio?, boot_dio?)`, `uart_set_lines(dtr?, rts?, pulse_ms?)` |
| Timeline | `timeline_start(watch_dios?, dio_names?, clear?)`, `timeline_read(since_s?, until_s?, sources?, contains?, last?)`, `timeline_mark(label)`, `timeline_stop` |
| Emulated device | `emu_i2c(address, registers?, read_only?, scl_dio?, sda_dio?, pullups?)`, `emu_spi(registers?, read_only?, mode?, cs_dio?, sck_dio?, mosi_dio?, miso_dio?, addr_mask?, read_mask?, read_value?, status?)`, `emu_set_registers(start, data)`, `emu_get_registers(start?, n?)`, `emu_status(last?)`, `emu_stop` |
| Sequencer | `sequence_run(steps, timeout_s?)` |

(`?` = optional.) Each tool's description, visible to the agent, explains
its arguments and when to use it.

### Design choices that matter to an agent

- **Summaries, not dumps.** A 131072-sample capture would flood an agent's
  context. Captures return statistics, measurements (levels, rise/fall
  time, frequency, duty), the first edges per channel and, on request,
  **decoded frames** (`decode="uart"|"i2c"|"spi"`). The full data can be
  saved to CSV/VCD for a human.
- **Images when shape matters.** `scope_image` and `logic_image` return a
  PNG of the last capture (zoomable to a time window) for multimodal
  models.
- **Re-decode without re-capturing.** `logic_decode` tries other pins,
  baud rates or SPI modes on the last capture.
- **Arm, act, read.** Captures accept `wait=false`: arm, use other tools to
  stimulate, then read. That is how the agent captures its own traffic.
- **Nothing lost between calls.** `uart_open` buffers the target's output
  in the background, and the timeline records everything with
  timestamps. **Every acting tool call is noted on the timeline** (source
  `host`), so the agent sees its own actions next to their effects.
- **Exact timing on demand.** `sequence_run` takes a list of steps
  (`{"op": "dio_write", "pin": 3, "level": 0}`, `{"op": "delay_us",
  "us": 100}`…) and runs them on the board with µs timing. See
  [Sequencer](11-sequencer.md#115-from-the-ai-tools).
- **Hex everywhere.** Byte strings are hex (`"9f 00 ff"`, `"9f00ff"`,
  `"0x9f,0x00"`); I2C addresses are 7-bit integers.

## 19.3 The skill

`tramuntana/skills/tramuntana-debug/SKILL.md` is loaded by the agent when a
task involves hardware attached to a Tramuntana. It contains:

- **Rules:** know the wiring before acting (look for a target description
  such as `tramuntana.md` in the project, or ask); measure before driving;
  ask before powering a target that may have its own supply, before
  driving pins the target may drive, before writing non-volatile memory or
  flashing, and before starting an emulator; one hypothesis per
  measurement; quote evidence; leave the bench safe.
- **Workflow:** inventory → start the timeline (with the control lines
  watched, the console open and a sniffer on) → power → is it alive → narrow
  down with a playbook → look at the wire → report with evidence.
- **Playbooks** (`references/`):

| File | Covers |
|---|---|
| `i2c.md` | idle levels, scan results, ID registers of common parts, NAK patterns, clock stretching, slow edges |
| `spi.md` | MISO patterns (`ff`, `00`, shifted bits = wrong mode), flash status/protection, sniffing |
| `uart.md` | idle level, swapped TX/RX, baud from a capture, framing, boot logs, flashing through the bridge (ESP32, STM32) |
| `power-and-signals.md` | rail ramp, brown-outs, reset/boot pins, clocks, signal integrity, sequenced resets |
| `emulate-and-sequence.md` | the timeline in practice, sequences for timing, emulated devices to test firmware |

A short **target description** in the project being debugged helps the
agent a lot. For example:

```markdown
# Tramuntana wiring
- J1.1 3V3 -> board VCC (the board has no other supply)
- DIO0 <- MCU TX (console, 115200 8N1), DIO1 -> MCU RX
- DIO3 -> NRST (open-drain, 10k pull-up on the board)
- DIO6/DIO7 -> I2C bus: BME280 at 0x76 (4.7k pull-ups on the board)
- DIO4 <- BME280 interrupt (unused by the firmware so far)
```

## 19.4 Example requests

- "Power the target, scan the I2C bus and tell me what the devices are."
- "The target prints nothing on its console. Find out why."
- "The BME280 doesn't answer on I2C. Debug it."
- "Reset the target and tell me how long it takes from reset release to
  the first byte on its UART, and what it does on the I2C bus meanwhile."
- "Pretend to be the BME280 at 0x76 and check how the firmware handles a
  temperature of 85 °C, and a sensor that doesn't answer."
- "Flash build/firmware.bin onto the ESP32 through the bridge: EN is on
  DIO3, GPIO0 on DIO2."
- "Capture the SPI traffic at boot and tell me which flash commands the
  bootloader uses."

[← Web application](18-webapp.md) · [Contents](README.md) · [Next: Examples →](20-examples.md)
