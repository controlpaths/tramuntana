"""
The J1 target header: what every pin is and does, and a drawing of it for
the terminal (`tramuntana pinout`). The board draws the same header on its
own screen (CMD_DISPLAY_PINOUT, fw/apps/tramuntana_fw/src/display.c).

The drawing is the header as seen on the board: J1 sits on the right edge,
pin 1 (square pad) at the top, odd pins in the inner column and even pins
in the outer one - so the left labels are the odd pins and the right labels
the even ones, exactly where the wires go.
"""
from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from typing import List


@dataclass(frozen=True)
class Pin:
    """One J1 pin: number (1..16), name ("DIO4"), kind (colour group) and
    what it does ("SPI MOSI · logic 4")."""
    number: int
    name: str
    kind: str       # power, gnd, ref, ain, aout, uart, spi, i2c
    function: str


# Must match hw/tramuntana_lite_r1a (J1 nets) and pinout_pins[] in display.c.
PINS: List[Pin] = [
    Pin(1, "3V3", "power", "target power (power on|off)"),
    Pin(2, "GND", "gnd", "ground"),
    Pin(3, "AIN0", "ain", "scope ch0 · adc 0"),
    Pin(4, "GND", "gnd", "ground"),
    Pin(5, "AIN1", "ain", "scope ch1 · adc 1"),
    Pin(6, "GND", "gnd", "ground"),
    Pin(7, "AOUT0", "aout", "waveform generator"),
    Pin(8, "VREF", "ref", "ADC reference · do not load"),
    Pin(9, "DIO0", "uart", "UART RX · logic 0"),
    Pin(10, "DIO1", "uart", "UART TX · logic 1"),
    Pin(11, "DIO2", "spi", "SPI CS · logic 2"),
    Pin(12, "DIO3", "spi", "SPI MISO · logic 3"),
    Pin(13, "DIO4", "spi", "SPI MOSI · logic 4"),
    Pin(14, "DIO5", "spi", "SPI SCK · logic 5"),
    Pin(15, "DIO6", "i2c", "I2C SCL · logic 6"),
    Pin(16, "DIO7", "i2c", "I2C SDA · logic 7"),
]

NOTES = [
    "UART bridge: DIO1 is Tramuntana's TX -> wire it to the target's RX; DIO0 <- target TX.",
    "Every DIO is also a plain GPIO (tramuntana dio).",
    "I2C: `tramuntana pullups on` if the target has no pull-ups of its own.",
    "Every DIO also feeds the logic analyzer (bit n = DIOn) and the sniffers, which only listen.",
    "AIN0, AIN1 and AOUT0 are 0..3.3 V. VREF is the ADC reference: never load it.",
    "Always share GND with the target.",
]

# ANSI SGR per pin kind; same palette as the board's screen.
COLOURS = {
    "power": "1;31", "gnd": "90", "ref": "38;5;208", "ain": "1;36", "aout": "1;33",
    "uart": "38;5;141", "spi": "1;35", "i2c": "1;94",
}
DIM = "90"
PAD_PIN1 = "■"
PAD = "●"


def use_colour(stream) -> bool:
    """True if ANSI colours make sense on 'stream' (a terminal, NO_COLOR
    unset, TERM not dumb)."""
    return stream.isatty() and "NO_COLOR" not in os.environ and os.environ.get("TERM") != "dumb"


def render(colour: bool = True, width: int = 0) -> str:
    """The header drawing as a multi-line string. With a terminal narrower
    than the full drawing, the functions move to a list below it."""
    def paint(text: str, sgr: str) -> str:
        return f"\x1b[{sgr}m{text}\x1b[0m" if colour else text

    width = width or shutil.get_terminal_size((100, 24)).columns
    fw = max(len(p.function) for p in PINS)
    full = 2 * fw + 40 <= width

    lines = [paint("Tramuntana J1", "1") + paint(" - 2x8 header, 2.54 mm, right edge of the board, "
                                                  "display on its left", DIM), ""]
    lpad = (fw + 2 if full else 0) + 9
    lines.append(" " * lpad + "┌─────────┐")
    for row in range(8):
        odd, even = PINS[2 * row], PINS[2 * row + 1]
        pad_odd = PAD_PIN1 if odd.number == 1 else PAD
        left = f"{odd.name:>5} ─┤"
        right = f"├─ {even.name:<5}"
        left = (f"{odd.function:>{fw}}  " if full else "") + paint(left[:5], COLOURS[odd.kind]) + left[5:]
        right = right[:3] + paint(right[3:], COLOURS[even.kind]) + (f"  {even.function}" if full else "")
        body = f" {odd.number:>2}{paint(pad_odd, COLOURS[odd.kind])} {paint(PAD, COLOURS[even.kind])}{even.number:<2} "
        lines.append(f"  {left}{body}{right}")
    lines.append(" " * lpad + "└─────────┘")

    if not full:
        lines.append("")
        for p in PINS:
            lines.append(f"  {p.number:>2}  {paint(f'{p.name:<5}', COLOURS[p.kind])}  {p.function}")

    lines.append("")
    lines += [paint(f"  - {n}", DIM) for n in NOTES]
    return "\n".join(line.rstrip() for line in lines)
