# Tramuntana webapp

A browser front-end for every instrument on a Tramuntana Lite board: SPI
and I2C master + sniffer, oscilloscope, waveform generator, PWM, 8-channel
logic analyzer, the USB-UART bridge terminal and its reset/boot control
lines, emulated I2C/SPI devices, the sequencer and the timeline. Pure HTML/CSS/JS, no
build step, no dependency beyond the browser — it talks to the board over
the [Web Serial API](https://developer.chrome.com/docs/capabilities/serial),
the same binary protocol as `host/python`.

## Running it

Web Serial only exists in Chromium-based desktop browsers (Chrome, Edge,
Opera). Either open `index.html` directly (`file://` counts as a secure
context) or serve the folder:

```bash
cd host/webapp
python3 -m http.server 8099
# then http://localhost:8099
```

Press **CONNECT** and pick the board's port. **Chrome lists all three of
the board's serial interfaces as separate ports** (shell, API, UART
bridge) with identical names — if you land on the wrong one the app says
so and tells you to try again; the API port is the one that answers a
PING with `"TRMT"`, found in one or two tries. Close any other program
(picocom, the `tramuntana` CLI) that has a port of this board open first —
Web Serial can't share.

## What each tab does

| Tab | Talks to | Covers |
|---|---|---|
| **SYSTEM** | `CMD_INFO/STATUS/POWER/PULLUPS/LED/DIO_*/ADC_READ/DISPLAY_TEXT/BOOTLOADER` | board info, target power, I2C pull-ups, RGB LED, DIO as plain GPIO, one-shot ADC, a note on the board's screen, live pin-ownership + instrument-state dashboard (polled every second), reboot to the UF2 bootloader |
| **SPI** | `CMD_SPI_*` | configure clock/mode, `sel/unsel/write/read/xfer/txn`, and the full-duplex sniffer (decoded MOSI+MISO per message) |
| **I2C** | `CMD_I2C_*` | configure, scan (click a hit to load its address below), register read/write, the low-level `start/raw write/raw read/stop` primitives, bus recovery, and the sniffer (decoded START/ACK/DATA/STOP) |
| **SCOPE** | `CMD_SCOPE_*` | arm/read a 1–2 channel capture with pre-trigger, canvas plot with a hover cursor, per-channel min/max/mean/rms/frequency, CSV export |
| **AWG** | `CMD_AWG_*` | sine/triangle/sawtooth/square/stairs, DC, step, and an arbitrary waveform (paste values or generate a quick sine) |
| **PWM** | `CMD_PWM_*` | hardware PWM on any DIO: one card per pair (7/6, 5/4, 3/2, 1/0 share a slice, so a frequency and a phase), per-pin duty and polarity, SYNC ALL to restart every pair on the same clock edge with its phase offset |
| **LOGIC** | `CMD_LOGIC_*` | 8-channel capture up to 150 MS/s (edge triggers) or 15 MS/s with pre-trigger (pattern trigger), canvas plot with horizontal zoom (wheel at the pointer, drag to pan, double-click to fit) and a hover cursor showing the 8-bit pattern, VCD export for PulseView/GTKWave, CSV export |
| **UART** | `CMD_UART_*` + a second port | configure the bridge (auto/on/off, baud override) and its control lines (RTS/DTR → reset/boot DIOs, `esp`/`direct`); independently, open the board's *third* serial port directly as a text terminal, with DTR/RTS switches and a reset pulse — no CLI needed to talk to the target |
| **EMU** | `CMD_EMU_*` + `EVT_EMU` | the board as an I2C or SPI device: start/stop, registers (16×16 grid, changes highlighted, read-only marks), counters, live transfer log |
| **SEQ** | `CMD_SEQ_*` | sequencer programs written as text (one step per line), run on the board; per-step time, status and value |
| **TIMELINE** | `CMD_TIME`, `CMD_DIO_WATCH`, every event | DIO edges, sniffers, emulator, control lines, sequences and the UART terminal on one clock; filters, markers, save as text |

Sniffer events and capture-done notifications arrive as unsolicited
frames on the same API port mid-request; `js/proto.js`'s `Link` class
queues requests and routes events to `js/device.js`'s `Tramuntana`
instance, which fans them out to whichever tab is listening.

## Layout

```
webapp/
├── index.html      one <section> per tab
├── style.css        cyberpunk theme matching the board's display artwork
│                    and the project logo - system fonts only, no CDN
├── img/logo.png     extracted from fw/imgs/logo.jpeg
└── js/
    ├── proto.js      binary API framing over Web Serial (mirrors fw's proto.h)
    ├── fmt.js         Hz/time/volts formatting, blob-download helper
    ├── device.js      Tramuntana + one class per instrument, Sequence/Sequencer (mirrors host/python)
    ├── timeline.js    the timeline (mirrors host/python/tramuntana/timeline.py)
    ├── scope.js       oscilloscope canvas
    ├── logic.js       logic analyzer canvas
    └── app.js         UI wiring: tabs, forms, polling, the two serial links
```

`js/proto.js` is the protocol reference on this side, kept in step by hand
with `fw/apps/tramuntana_fw/src/proto.h` and
`host/python/tramuntana/proto.py`; if you add a command there, mirror it
in `PROTO.CMD` here and in `device.js`.

## Notes

- The API `Link` serialises requests (one in flight at a time) and
  resolves them by sequence number, so a sniffer event landing between a
  request and its reply never gets mistaken for the reply.
- Big transfers (SPI read/write, scope/logic paging) are chunked at 4096 /
  2048 bytes to stay under `PROTO_MAX_PAYLOAD`, exactly like the Python
  library.
- The UART tab's terminal is a second, independent `SerialPort` — closing
  the API connection doesn't touch it, and vice versa.
- No external fonts or scripts: everything needed is in this folder, so
  it works with no internet at a workshop bench.
