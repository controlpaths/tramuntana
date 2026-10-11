# 18. Web application

[← Firmware internals](17-firmware.md) · [Contents](README.md) · [Next: AI tools →](19-ai-tools.md)

`host/webapp` is a browser front end for the board: pure HTML, CSS and
JavaScript, with no build step and no dependency beyond the browser. It
talks to the board through the [Web Serial API](https://developer.chrome.com/docs/capabilities/serial),
using the same binary protocol as the Python library. It needs no internet
connection (no external fonts or scripts), which makes it handy at a
workshop bench.

## 18.1 Running it

Web Serial exists in Chromium-based desktop browsers (Chrome, Edge,
Opera). Either open `host/webapp/index.html` directly (`file://` counts as
a secure context) or serve the folder:

```bash
cd host/webapp
python3 -m http.server 8099      # then open http://localhost:8099
```

Press **CONNECT** and pick the board's port. The browser lists the board's
three serial interfaces as separate ports **with identical names**. If you
pick the wrong one, the app says so and you try the next: the API port is
the one that answers a PING with `"TRMT"`.

Close any other program that has the board's API port open (the CLI, a
Python script, the AI tools): a port can only be open in one place.

## 18.2 Tabs

| Tab | What you can do |
|---|---|
| **SYSTEM** | board info, target power, I2C pull-ups, RGB LED, DIO as plain GPIO, one-shot ADC, a note on the board's screen, a live dashboard of pin ownership and instrument states (polled every second), reboot to the UF2 bootloader |
| **SPI** | configure clock and mode, `sel/unsel/write/read/xfer/txn`, and the sniffer (MOSI + MISO per message) |
| **I2C** | configure, scan (click an address to use it below), register read/write, the low-level `start / raw write / raw read / stop` primitives, bus recovery, and the sniffer |
| **SCOPE** | 1–2 channel captures with pre-trigger, a plot with a hover cursor, per-channel min/max/mean/RMS/frequency, CSV export |
| **AWG** | sine / triangle / sawtooth / square / stairs, DC, step, and arbitrary waveforms (paste values or generate a quick sine) |
| **PWM** | one card per pin pair (frequency and phase per pair, duty and polarity per pin), **SYNC ALL** |
| **LOGIC** | captures up to 150 MS/s (edge triggers) or 15 MS/s with pre-trigger (pattern trigger), a plot with zoom (wheel at the pointer, drag to pan, double-click to fit) and a cursor showing the 8-bit pattern, VCD and CSV export |
| **UART** | configure the bridge (auto/on/off, baud override); the **control lines** (RTS/DTR → reset/boot DIOs, `esp` or `direct` mode) with their live state; and the board's third serial port as a text terminal in the same page, with DTR / RTS switches and a **RESET PULSE** button |
| **EMU** | start an emulated I2C device (address, SCL/SDA pins, pull-ups) or SPI device (mode, four pins, command format); load registers, mark read-only ones; a 16×16 register grid (changes since the last read in green, read-only in amber, click a cell to edit it, optional refresh every second); counters; a live log of the target's transfers |
| **SEQ** | write a sequencer program as text (one step per line, see below), run it on the board, and see each step's time, status and value; the program is kept in the browser between visits |
| **TIMELINE** | record everything on one clock: DIO edges (pick the pins and name them), sniffed messages (start the sniffers on their tabs), emulator transfers, control-line changes, sequencer runs, the UART terminal's lines and what you typed; filter by source and text, add markers, save as a text file |

The UART terminal is a second, independent serial connection: closing the
API connection doesn't affect it, and vice versa. Its lines go to the
timeline while one is recording.

## 18.3 The SEQ tab's step syntax

One step per line; `#` starts a comment; a trailing `?` makes a failing
step record its error and carry on instead of stopping the run. Addresses
and bytes are hex; pins, counts and times are decimal.

```
delay_us 100            delay_ms 20
power on|off            pullups on|off
dio_dir 3 out|in        dio_write 3 0|1
dio_read                wait_dio 4 0 [timeout_us]
watch 18                (hex DIO mask; 0 = off)
i2c_write 76 f4 27      i2c_read 76 2
i2c_write_read 76 d0 read 1
spi_txn 9f read 3       uart_send help\r\n
awg_dc 1.2              adc_read 0
mark 1
loop 10
  ...
end_loop
```

It produces exactly the same program as the Python `Sequence` builder (see
[Sequencer](11-sequencer.md)).

## 18.4 Code layout

```
host/webapp/
├── index.html      one <section> per tab
├── style.css       theme matching the board's display; system fonts only
├── img/logo.png
└── js/
    ├── proto.js    framing over Web Serial; mirrors fw/apps/tramuntana_fw/src/proto.h
    ├── fmt.js      Hz / time / volts formatting, file downloads
    ├── device.js   Tramuntana + one class per instrument, Sequence / Sequencer (mirrors host/python)
    ├── timeline.js the timeline (mirrors host/python/tramuntana/timeline.py)
    ├── scope.js    oscilloscope canvas
    ├── logic.js    logic analyzer canvas
    └── app.js      UI wiring: tabs, forms, polling, the two serial links
```

The `Link` class in `proto.js` serialises requests (one in flight),
matches replies by sequence number, and routes events to `device.js`,
which passes them to whichever tab is listening. Bulk transfers are chunked
like in the Python library. When you add a command to the firmware, mirror
it in `PROTO.CMD` in `proto.js` and in `device.js`.

[← Firmware internals](17-firmware.md) · [Contents](README.md) · [Next: AI tools →](19-ai-tools.md)
