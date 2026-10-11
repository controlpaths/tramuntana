#!/usr/bin/env python3
"""Drive a 128x32 SSD1306 OLED over I2C (DIO6 SCL, DIO7 SDA).

Wiring: VCC -> J1.1 (3V3), GND -> J1.2, SCL -> J1.15 (DIO6), SDA -> J1.16
(DIO7). Most modules answer at 0x3C (0x3D with the address jumper moved)
and many have no pull-ups of their own, so the board's are switched on.

The script draws a splash screen, then a live readout of AIN0 (voltage,
bar and a scrolling trace) until Ctrl-C or --seconds run out.

    python3 ssd1306_oled.py                  # 0x3C, 400 kHz, 30 s of AIN0
    python3 ssd1306_oled.py --text "hello"   # just show some text
"""
import argparse
import time

from tramuntana import Tramuntana

WIDTH, HEIGHT = 128, 32
PAGES = HEIGHT // 8

# I2C control byte: next bytes are commands / display RAM data.
CTRL_CMD = 0x00
CTRL_DATA = 0x40

INIT = [
    0xAE,              # display off
    0xD5, 0x80,        # clock divide / oscillator
    0xA8, HEIGHT - 1,  # multiplex ratio: 32 rows
    0xD3, 0x00,        # display offset
    0x40,              # start line 0
    0x8D, 0x14,        # charge pump on (internal VCC)
    0x20, 0x00,        # horizontal addressing mode
    0xA1,              # segment remap: column 127 = SEG0
    0xC8,              # COM scan from COM[N-1] to COM0
    0xDA, 0x02,        # COM pins: sequential, no remap (128x32 panels)
    0x81, 0x8F,        # contrast
    0xD9, 0xF1,        # pre-charge period
    0xDB, 0x40,        # VCOMH deselect level
    0xA4,              # display follows RAM
    0xA6,              # normal (not inverted)
    0xAF,              # display on
]

# Classic 5x7 font, ASCII 0x20..0x7E, 5 columns per glyph, bit 0 = top row.
FONT = bytes.fromhex(
    "0000000000" "00005f0000" "0007000700" "147f147f14" "242a7f2a12"   # space ! " # $
    "2313086462" "3649552250" "0005030000" "001c224100" "0041221c00"   # % & ' ( )
    "082a1c2a08" "08083e0808" "0050300000" "0808080808" "0060600000"   # * + , - .
    "2010080402" "3e5149453e" "00427f4000" "4261514946" "2141454b31"   # / 0 1 2 3
    "1814127f10" "2745454539" "3c4a494930" "0171090503" "3649494936"   # 4 5 6 7 8
    "064949291e" "0036360000" "0056360000" "0814224100" "1414141414"   # 9 : ; < =
    "0041221408" "0201510906" "324979413e" "7e1111117e" "7f49494936"   # > ? @ A B
    "3e41414122" "7f4141221c" "7f49494941" "7f09090101" "3e41415132"   # C D E F G
    "7f0808087f" "00417f4100" "2040413f01" "7f08142241" "7f40404040"   # H I J K L
    "7f0204027f" "7f0408107f" "3e4141413e" "7f09090906" "3e4151215e"   # M N O P Q
    "7f09192946" "4649494931" "01017f0101" "3f4040403f" "1f2040201f"   # R S T U V
    "7f2018207f" "6314081463" "0304780403" "6151494543" "007f414100"   # W X Y Z [
    "0204081020" "0041417f00" "0402010204" "4040404040" "0001020400"   # \ ] ^ _ `
    "2054545478" "7f48444438" "3844444420" "384444487f" "3854545418"   # a b c d e
    "087e090102" "081454543c" "7f08040478" "00447d4000" "2040443d00"   # f g h i j
    "007f102844" "00417f4000" "7c04180478" "7c08040478" "3844444438"   # k l m n o
    "7c14141408" "081414187c" "7c08040408" "4854545420" "043f444020"   # p q r s t
    "3c4040207c" "1c2040201c" "3c4030403c" "4428102844" "0c5050503c"   # u v w x y
    "4464544c44" "0008364100" "00007f0000" "0041360800" "0201020402"   # z { | } ~
)


class Ssd1306:
    """Frame buffer in the controller's own layout: one byte per column per
    8-row page, bit 0 = top row of the page."""

    def __init__(self, t: Tramuntana, addr: int = 0x3C):
        self.i2c = t.i2c
        self.addr = addr
        self.buf = bytearray(WIDTH * PAGES)

    def command(self, *cmds: int):
        self.i2c.write(self.addr, bytes([CTRL_CMD, *cmds]))

    def init(self):
        self.command(*INIT)

    def clear(self):
        self.buf[:] = bytes(len(self.buf))

    def pixel(self, x: int, y: int, on: bool = True):
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            i = (y // 8) * WIDTH + x
            if on:
                self.buf[i] |= 1 << (y % 8)
            else:
                self.buf[i] &= ~(1 << (y % 8)) & 0xFF

    def hline(self, x0: int, x1: int, y: int):
        for x in range(x0, x1 + 1):
            self.pixel(x, y)

    def rect(self, x: int, y: int, w: int, h: int, fill: bool = False):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if fill or yy in (y, y + h - 1) or xx in (x, x + w - 1):
                    self.pixel(xx, yy)

    def text(self, x: int, y: int, s: str, scale: int = 1):
        """5x7 glyphs on a 6-pixel pitch (times 'scale')."""
        for ch in s:
            c = ord(ch) if 0x20 <= ord(ch) <= 0x7E else ord("?")
            glyph = FONT[(c - 0x20) * 5:(c - 0x20) * 5 + 5]
            for col, bits in enumerate(glyph):
                for row in range(7):
                    if bits >> row & 1:
                        for dx in range(scale):
                            for dy in range(scale):
                                self.pixel(x + col * scale + dx, y + row * scale + dy)
            x += 6 * scale

    def show(self):
        """Sends the whole frame: 512 bytes in one I2C transaction."""
        self.command(0x21, 0, WIDTH - 1,   # column range
                     0x22, 0, PAGES - 1)   # page range
        self.i2c.write(self.addr, bytes([CTRL_DATA]) + bytes(self.buf))


def splash(oled: Ssd1306):
    oled.clear()
    oled.text(4, 2, "Tramuntana", scale=2)
    oled.text(4, 22, "SSD1306 128x32 I2C")
    oled.show()


def ain0_monitor(t: Tramuntana, oled: Ssd1306, seconds: float):
    """AIN0 voltage as text, a bar and a trace that scrolls right to left."""
    trace = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        v = t.adc(0)
        trace = (trace + [v])[-60:]
        oled.clear()
        oled.text(0, 0, f"AIN0 {v:.2f}V")
        oled.rect(0, 12, 64, 8)
        oled.rect(0, 12, max(1, round(64 * min(v, 3.3) / 3.3)), 8, fill=True)
        oled.text(0, 24, "0      3.3V")
        oled.rect(66, 0, 62, 32)
        for i, s in enumerate(trace):
            oled.pixel(67 + i, 30 - round(min(s, 3.3) / 3.3 * 29))
        oled.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", type=lambda s: int(s, 16), default=0x3C, help="7-bit address, hex (default 3c)")
    ap.add_argument("--freq", type=int, default=400_000, help="SCL frequency in Hz (default 400000)")
    ap.add_argument("--text", help="show this text and exit")
    ap.add_argument("--seconds", type=float, default=30, help="how long the AIN0 monitor runs")
    a = ap.parse_args()

    with Tramuntana() as t:
        t.power(True)                               # the OLED runs from J1.1
        time.sleep(0.1)                             # let its supply settle
        t.i2c.config(a.freq, pullups=True)
        if a.addr not in t.i2c.scan():
            raise SystemExit(f"no device at 0x{a.addr:02x} (check wiring and power)")
        t.display("SSD1306 demo")                   # label on the board's screen

        oled = Ssd1306(t, a.addr)
        oled.init()
        if a.text:
            oled.clear()
            for i, line in enumerate(a.text.split("\\n")[:4]):
                oled.text(0, i * 8, line)
            oled.show()
            return
        splash(oled)
        time.sleep(2)
        try:
            ain0_monitor(t, oled, a.seconds)
        except KeyboardInterrupt:
            pass
        splash(oled)


if __name__ == "__main__":
    main()
