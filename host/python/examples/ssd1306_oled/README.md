# ssd1306_oled

Drives a 128x32 SSD1306 OLED over I2C: init sequence, frame buffer, 5x7
font, and a live readout of AIN0 (voltage, bar and scrolling trace).

## Wiring

| OLED | Tramuntana |
|---|---|
| VCC | J1.1 (3V3) |
| GND | J1.2 |
| SCL | J1.15 (DIO6) |
| SDA | J1.16 (DIO7) |

Anything on J1.3 (AIN0) shows up on the readout; a wire from J1.7 (AOUT0)
plus `tramuntana awg sine 1 --amp 1` makes a nice moving trace.

## Usage

```bash
python3 ssd1306_oled.py                  # 0x3C, 400 kHz, 30 s of AIN0
python3 ssd1306_oled.py --text "hello"   # just show some text (\n for new line)
python3 ssd1306_oled.py --addr 3d        # address jumper moved
```

| Option | Default | |
|---|---|---|
| `--addr` | `3c` | 7-bit I2C address, hex |
| `--freq` | 400000 | SCL frequency in Hz |
| `--text` | | show this text and exit |
| `--seconds` | 30 | how long the AIN0 monitor runs |

## How it works

The SSD1306 takes a control byte after its address: `0x00` means the next
bytes are commands, `0x40` means they are display RAM. The script:

1. Switches the 3V3 rail on, configures I2C with the board's pull-ups (most
   modules have none) and checks the address answers.
2. Sends the init sequence: charge pump on, horizontal addressing, 32-row
   multiplex, segment/COM remap so (0,0) is the top left corner, contrast.
3. Draws into a 512-byte frame buffer in the host (128 columns x 4 pages
   of 8 rows; bit 0 of each byte is the top row of its page): pixels,
   rectangles and text with a 5x7 font, scalable.
4. `show()` sets the column and page range and sends the whole buffer in
   one I2C write of 513 bytes.

The monitor reads `t.adc(0)` in a loop and redraws value, bar and the last
60 samples until Ctrl-C or `--seconds` run out, then shows the splash
screen again.
