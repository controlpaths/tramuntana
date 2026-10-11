#!/usr/bin/env python3
"""Tramuntana Lite r1a - sticker for the top of the lid.

1:1 SVG in mm, same frame as case.py (PCB bottom left corner at (0, 0),
x to the right, y up, looking down on the lid). The sticker is the PCB
outline (80 x 60 mm, r = 3 mm), which sits on the flat top of the lid
(81 x 61 mm, r 3.5 mm inside the r 2 edge rounding) with 0.5 mm to spare.
Holes for the display window and both LEDs (light pipes on the opaque
lids, plain plastic on the translucent ones), J1 pinout along the right
edge, next to the opening.

All text is converted to outlines (fontTools), so the print file needs no
fonts. Outputs:

    tramuntana_lite_sticker.svg          print file: artwork + 1.5 mm bleed,
                                         cut lines in the "CutContour" layer
    tramuntana_lite_sticker_preview.png  how it looks on a black lid (needs cairosvg)

    .venv/bin/pip install fonttools pillow cairosvg
    .venv/bin/python sticker.py
"""
import math
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from PIL import Image

import case

HERE = Path(__file__).resolve().parent
LOGO = HERE / "../../../host/webapp/img/logo.png"
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"

# ---------------------------------------------------------------- geometry
W, H, R = case.PCB_W, case.PCB_H, case.PCB_R   # sticker outline = PCB outline
BLEED = 1.5
CUT_CLEAR = 0.5                                 # sticker hole edge to the plastic hole edge

ww, wh, wr = case.WINDOW
WIN_CX = (case.DISPLAY[0] + case.DISPLAY[2]) / 2 - case.WINDOW_EXTRA_LEFT / 2
ww += case.WINDOW_EXTRA_LEFT
WIN = (WIN_CX - ww / 2 - CUT_CLEAR, case.WINDOW_Y0 - CUT_CLEAR,
       WIN_CX + ww / 2 + CUT_CLEAR, case.WINDOW_Y0 + wh + CUT_CLEAR)   # x0, y0, x1, y1
WIN_R = wr + CUT_CLEAR
LED_RGB = case.LEDS["led_rgb"]
LED_OUT = case.LEDS["led_out"]
J1_PITCH = 2.54
J1_PIN1_Y = case.kicad(0, 81.11)[1]             # pin 1/2 row; pins 15/16 are 7 pitches below

# J1, (odd pin, even pin) per row, from pin 1 down. Odd pins are the row away
# from the board edge, which bends over the other one: the upper row.
# (prefix, name, colour) - prefix in colour, name in the foreground colour
J1_ROWS = [
    (("", "3V3T", "red"), ("", "GND", "dim")),
    (("", "AIN0", "cyan"), ("", "GND", "dim")),
    (("", "AIN1", "cyan"), ("", "GND", "dim")),
    (("", "AOUT", "magenta"), ("", "3V3A", "amber")),
    (("D0", "RX", "green"), ("D1", "TX", "green")),
    (("D2", "CS", "green"), ("D3", "MISO", "green")),
    (("D4", "MOSI", "green"), ("D5", "SCK", "green")),
    (("D6", "SCL", "green"), ("D7", "SDA", "green")),
]

# palette of the webapp (host/webapp/style.css)
C = {"bg": "#07080d", "line": "#1f2740", "fg": "#d8e1ff", "dim": "#7d88ad", "cyan": "#40f0e0",
     "magenta": "#c86eff", "amber": "#ffb547", "red": "#ff4d6d", "green": "#4dff9a",
     "blue": "#4da3ff", "yellow": "#ffe066"}
CUT_COLOR = "#ec008c"                           # CutContour spot colour


def Y(y):
    """Case frame (y up) to SVG (y down)."""
    return H - y


def rrect_path(x0, y0, x1, y1, r):
    """Rounded rectangle in the case frame, as SVG path data."""
    t, b = Y(y1), Y(y0)
    return (f"M{x0 + r:.3f},{t:.3f} H{x1 - r:.3f} A{r},{r} 0 0 1 {x1:.3f},{t + r:.3f} "
            f"V{b - r:.3f} A{r},{r} 0 0 1 {x1 - r:.3f},{b:.3f} H{x0 + r:.3f} "
            f"A{r},{r} 0 0 1 {x0:.3f},{b - r:.3f} V{t + r:.3f} A{r},{r} 0 0 1 {x0 + r:.3f},{t:.3f} Z")


def circle_path(x, y, r):
    return (f"M{x - r:.3f},{Y(y):.3f} A{r},{r} 0 1 0 {x + r:.3f},{Y(y):.3f} "
            f"A{r},{r} 0 1 0 {x - r:.3f},{Y(y):.3f} Z")


# ---------------------------------------------------------------- text to outlines
_fonts = {}


def text_width(s, size, font=FONT, spacing=0.0):
    f = _fonts.setdefault(font, TTFont(font))
    cmap, hmtx, upm = f.getBestCmap(), f["hmtx"], f["head"].unitsPerEm
    return sum(hmtx[cmap[ord(ch)]][0] for ch in s) * size / upm + spacing * (len(s) - 1)


def text(s, x, y, size, fill, font=FONT, anchor="start", spacing=0.0):
    """Text as a filled path; (x, y) is the baseline in the case frame."""
    f = _fonts.setdefault(font, TTFont(font))
    cmap, hmtx, gs, upm = f.getBestCmap(), f["hmtx"], f.getGlyphSet(), f["head"].unitsPerEm
    k = size / upm
    w = text_width(s, size, font, spacing)
    x -= {"start": 0, "middle": w / 2, "end": w}[anchor]
    pen = SVGPathPen(gs, ntos=lambda v: f"{v:.3f}")
    for ch in s:
        name = cmap[ord(ch)]
        gs[name].draw(TransformPen(pen, (k, 0, 0, -k, x, Y(y))))
        x += hmtx[name][0] * k + spacing
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


# ---------------------------------------------------------------- logo
def logo(x0, y_top, width, fill):
    """The webapp's pixel-art logo, traced as merged pixel runs."""
    im = Image.open(LOGO).convert("RGBA")
    im = im.crop(im.getbbox())
    w, h = im.size
    alpha = im.getchannel("A").tobytes()
    k = width / w
    rows = []
    for y in range(h):
        runs, x = [], 0
        while x < w:
            if alpha[y * w + x] > 127:
                x1 = x
                while x1 < w and alpha[y * w + x1] > 127:
                    x1 += 1
                runs.append((x, x1))
                x = x1
            else:
                x += 1
        rows.append(runs)
    # merge identical runs in consecutive rows into rectangles
    open_, rects = {}, []
    for y, runs in enumerate(rows + [[]]):
        for run in list(open_):
            if run not in runs:
                rects.append((*run, open_.pop(run), y))
        for run in runs:
            open_.setdefault(run, y)
    top = Y(y_top)
    e = 0.01                                    # overlap, so no hairline seams between rectangles
    d = " ".join(f"M{x0 + a * k - e:.3f},{top + y0 * k - e:.3f}h{(b - a) * k + 2 * e:.3f}"
                 f"v{(y1 - y0) * k + 2 * e:.3f}h{-(b - a) * k - 2 * e:.3f}z" for a, b, y0, y1 in rects)
    return f'<path fill="{fill}" d="{d}"/>', h * k


# ---------------------------------------------------------------- artwork
def artwork():
    out = []
    add = out.append
    # background with bleed (no border: the die cut drifts ~0.5 mm)
    add(f'<rect x="{-BLEED}" y="{-BLEED}" width="{W + 2 * BLEED}" height="{H + 2 * BLEED}" fill="{C["bg"]}"/>')

    # -- left panel: logo, wordmark
    lx0, lx1 = 2.5, WIN[0] - 2.0
    lcx = (lx0 + lx1) / 2
    lw = 22.0
    path, lh = logo(lcx - lw / 2, 56.0, lw, C["fg"])
    add(path)
    y = 56.0 - lh - 5.2
    name = "TRAMUNTANA"
    size = 4.2
    sp = (lx1 - lx0 - text_width(name, size, FONT_BOLD)) / (len(name) - 1)
    add(text(name, lx0, y, size, C["fg"], FONT_BOLD, spacing=sp))
    y -= 4.6
    add(text("LITE", lx0, y, 3.0, C["cyan"], FONT_BOLD, spacing=0.9))
    add(text("r1a", lx1, y, 2.0, C["dim"], anchor="end"))
    y -= 4.0
    add(text("hardware hacking", lx0, y, 1.8, C["dim"]))
    add(text("console", lx0, y - 2.4, 1.8, C["dim"]))

    # tramuntana: the north wind - three gusts with a curl
    for i, (yy, x_end, col) in enumerate(((15.0, 24.0, C["cyan"]), (11.5, 19.0, C["dim"]), (8.0, 26.0, C["line"]))):
        r = 1.6 - 0.25 * i
        d = (f"M{lx0:.2f},{Y(yy):.2f} H{x_end - r:.2f} "
             f"A{r},{r} 0 1 0 {x_end - r:.2f},{Y(yy) - 2 * r:.2f} "
             f"A{r * 0.55:.2f},{r * 0.55:.2f} 0 0 0 {x_end - r:.2f},{Y(yy) - 2 * r + 1.1 * r:.2f}")
        add(f'<path d="{d}" fill="none" stroke="{col}" stroke-width="0.45" stroke-linecap="round"/>')

    # micro-USB of the Pico 2, bottom edge
    ux = case.USB_X
    add(f'<path d="M{ux - 1.1:.2f},{Y(3.2):.2f} L{ux + 1.1:.2f},{Y(3.2):.2f} L{ux:.2f},{Y(2.0):.2f} Z" fill="{C["dim"]}"/>')
    add(text("USB", ux, 4.0, 1.5, C["dim"], anchor="middle"))

    # -- display window: viewfinder corners
    x0, y0, x1, y1 = WIN
    o, a = 0.9, 3.0
    for cx, cy, sx, sy in ((x0, y0, 1, 1), (x1, y0, -1, 1), (x0, y1, 1, -1), (x1, y1, -1, -1)):
        px, py = cx - sx * o, cy - sy * o
        add(f'<path d="M{px + sx * a:.2f},{Y(py):.2f} H{px:.2f} V{Y(py + sy * a):.2f}" fill="none" '
            f'stroke="{C["cyan"]}" stroke-width="0.3" stroke-linecap="round"/>')

    # under the window: what it does
    wcx = (x0 + x1) / 2
    add(text("SCOPE · AWG · LOGIC", wcx, 8.7, 1.9, C["fg"], anchor="middle"))
    add(text("SPI · I2C · UART", wcx, 5.8, 1.9, C["fg"], anchor="middle"))
    add(text("controlpaths.com", wcx, 2.2, 1.4, C["dim"], anchor="middle"))

    # -- right strip: LEDs and J1
    sx0, sx1 = x1 + 0.9, W - 1.0
    (gx, gy), _, _, bore = LED_RGB
    rr = bore / 2 + CUT_CLEAR + 0.8
    for i, col in enumerate(("red", "green", "blue")):         # RGB ring around the status LED
        a0, a1 = math.radians(90 + 120 * i + 8), math.radians(90 + 120 * (i + 1) - 8)
        p0 = (gx + rr * math.cos(a0), Y(gy + rr * math.sin(a0)))
        p1 = (gx + rr * math.cos(a1), Y(gy + rr * math.sin(a1)))
        add(f'<path d="M{p0[0]:.3f},{p0[1]:.3f} A{rr},{rr} 0 0 0 {p1[0]:.3f},{p1[1]:.3f}" fill="none" '
            f'stroke="{C[col]}" stroke-width="0.45" stroke-linecap="round"/>')
    add(text("STATUS", gx, gy - rr - 2.3, 1.5, C["dim"], anchor="middle"))

    (ox, oy), _, _, bore = LED_OUT
    ro = bore / 2 + CUT_CLEAR + 0.7
    add(f'<path d="{circle_path(ox, oy, ro)}" fill="none" stroke="{C["red"]}" stroke-width="0.35"/>')
    add(text("TARGET", ox, oy + ro + 4.3, 1.5, C["red"], anchor="middle"))
    add(text("POWER", ox, oy + ro + 2.3, 1.5, C["dim"], anchor="middle"))

    size = 1.5
    add(text("upper", sx0, J1_PIN1_Y + 1.7, 1.1, C["dim"]))
    add(text("lower", sx1, J1_PIN1_Y + 1.7, 1.1, C["dim"], anchor="end"))
    for i, (odd, even) in enumerate(J1_ROWS):
        yb = J1_PIN1_Y - i * J1_PITCH - 0.55
        for (pre, nm, col), x, anchor in ((odd, sx0, "start"), (even, sx1, "end")):
            if pre:
                if anchor == "end":
                    x -= text_width(nm, size) + 0.3 + text_width(pre, size)
                add(text(pre, x, yb, size, C[col], FONT_BOLD))
                add(text(nm, x + text_width(pre, size) + 0.3, yb, size, C["fg"]))
            else:
                add(text(nm, x, yb, size, C[col], FONT_BOLD, anchor=anchor))
    jb = J1_PIN1_Y - 7 * J1_PITCH - 1.0
    add(text("Dn = logic ch n", (sx0 + sx1) / 2, jb - 1.6, 1.1, C["dim"], anchor="middle"))
    return out


def cut_paths():
    (gx, gy), _, _, gb = LED_RGB
    (ox, oy), _, _, ob = LED_OUT
    return [rrect_path(0, 0, W, H, R), rrect_path(*WIN, WIN_R),
            circle_path(gx, gy, gb / 2 + CUT_CLEAR), circle_path(ox, oy, ob / 2 + CUT_CLEAR)]


def svg(body, x0, y0, w, h):
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            f'width="{w}mm" height="{h}mm" viewBox="{x0} {y0} {w} {h}">\n' + "\n".join(body) + "\n</svg>\n")


def main():
    art = artwork()
    cuts = cut_paths()
    body = ['<g id="artwork" inkscape:groupmode="layer" inkscape:label="Artwork">', *art, "</g>",
            '<g id="CutContour" inkscape:groupmode="layer" inkscape:label="CutContour">',
            *(f'<path d="{d}" fill="none" stroke="{CUT_COLOR}" stroke-width="0.1"/>' for d in cuts), "</g>"]
    (HERE / "tramuntana_lite_sticker.svg").write_text(svg(body, -BLEED, -BLEED, W + 2 * BLEED, H + 2 * BLEED))

    # preview: the sticker, cut, on a black lid
    m = case.CLEAR + case.WALL
    lid = rrect_path(-m, -m, W + m, H + m, case.OUT_R)
    pr = ['<rect x="-6" y="-6" width="92" height="72" fill="#c9ccd3"/>',
          f'<path d="{lid}" fill="#16171a"/>',
          f'<path d="{rrect_path(-m + case.EDGE_R, -m + case.EDGE_R, W + m - case.EDGE_R, H + m - case.EDGE_R, case.OUT_R - case.EDGE_R)}" fill="#1e1f23"/>',
          f'<clipPath id="cut"><path clip-rule="evenodd" d="{" ".join(cuts)}"/></clipPath>',
          '<g clip-path="url(#cut)">', *art, "</g>"]
    pr.append(f'<path d="{rrect_path(*WIN, WIN_R)}" fill="#0b1020"/>')     # display glass under the window
    for (x, y), _, _, b in (LED_RGB, LED_OUT):
        pr.append(f'<path d="{circle_path(x, y, b / 2)}" fill="#3a3c42"/>')
    preview = svg(pr, -6, -6, 92, 72)
    try:
        import cairosvg
        cairosvg.svg2png(bytestring=preview.encode(), write_to=str(HERE / "tramuntana_lite_sticker_preview.png"),
                         output_width=1840)
    except ImportError:
        print("cairosvg not installed: no preview")
    print(f"sticker {W:.0f} x {H:.0f} mm r {R}, bleed {BLEED} mm")
    print(f"window cut x {WIN[0]:.2f}..{WIN[2]:.2f}, y {WIN[1]:.2f}..{WIN[3]:.2f}, r {WIN_R}")
    for name, ((x, y), _, _, b) in case.LEDS.items():
        print(f"{name} cut at ({x:.2f}, {y:.2f}), d {b + 2 * CUT_CLEAR} mm")


if __name__ == "__main__":
    main()
