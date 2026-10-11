# Tramuntana Lite r1a

RP2350-based board. The KiCad project lives in [kicad/](kicad/). Firmware
for this board lives in the workspace-wide [../../fw/](../../fw/) tree (see
its README for building/flashing) — bring-up is happening on a
**Raspberry Pi Pico 2** (RP2350) ahead of the final board, since the MCU
(A1 in the schematic) is a Pico module soldered straight onto this board.

---

## Board devicetree

There's no dedicated Zephyr board port for tramuntana_lite_r1a (that's more
machinery than a bring-up needs). Instead, the hardware is described in
[../../fw/boards/tramuntana_lite_r1a-common.dtsi](../../fw/boards/tramuntana_lite_r1a-common.dtsi) —
an app-level devicetree overlay applied on top of the stock
`rpi_pico2/rp2350a/m33` board, shared by every app under `fw/apps/`.

### What's on the board

Derived from the schematic netlist ([kicad/tramuntana_lite_r1a.kicad_sch](kicad/tramuntana_lite_r1a.kicad_sch),
cross-checked against `kicad-cli sch export netlist`):

| Header pin | Net | Pico GPIO | Devicetree |
|---|---|---|---|
| J1.1 | `/3V3_TARGET` (switched, via Q1) | GPIO11 (enable, active-low) | `regulator-fixed`, alias `targetpwr` |
| J1.3 | `/AIN0` | GPIO26 / ADC0 | `&adc`, channel 0 |
| J1.5 | `/AIN1` | GPIO27 / ADC1 | `&adc`, channel 1 |
| J1.7 | `/AOUT0` | GPIO8 | `zephyr,user` / `aout0-gpios` |
| J1.9-16 | `DIO0`..`DIO7` | GPIO7..GPIO0 | `zephyr,user` / `dio-gpios` |
| — | `/DIO_PU_ENABLE` (shared pull-up bias for DIO6/DIO7) | GPIO10 | `zephyr,user` / `dio-pu-enable-gpios` |
| J1.11-14 (DIO2-5) | SPI0 CSN/RX/TX/SCK | GPIO5/4/3/2 | `&spi0` (hardware CS on DIO2) |
| J1.15-16 (DIO6/7) | I2C0 SCL/SDA | GPIO1/0 | `&i2c0` (100kHz by default) |
| J2.3/4 | SPI1 SCK/MOSI | GPIO14/15 | `&spi1` (hardware CS on GPIO13) |
| J2.5/6/7 | TFT reset/DC/CS | GPIO16/17/13 | the `zephyr,mipi-dbi-spi` + `sitronix,st7735r` nodes |
| D3 (RGB LED) | `/LEDR`, `/LEDG`, `/LEDB` | GPIO20/21/22 | `gpio-leds` (active-low), aliases `led-r`/`led-g`/`led-b` |

Q1 is a P-FET high-side switch (R17 pulls its gate to +3V3 at reset, so the
rail is off by default); D3 is a common-anode RGB LED, so all three legs
sink current through their GPIO and are active-low. D7 (power-good LED) and
the protection parts (F1 fuse, D1/D2 clamps, the series resistors) aren't
modeled — they're not GPIO-controlled, just electrical protection and
indication.

Confirmed on real hardware: D3's red and green legs are swapped relative to
the schematic's own net names (`LEDR`/`LEDG`) — GPIO20 is actually green and
GPIO21 is actually red, the opposite of what the net names say. The
devicetree's `led-r`/`led-g` aliases are wired to match reality (GPIO21/
GPIO20), not the schematic labels, so software colors are correct even
though the underlying net names are misleading.

One thing worth knowing: UART0 (GPIO0/GPIO1) is this board target's default
Zephyr console, and that's DIO7/DIO6 on the target header here — the same
two pins `&i2c0` uses. The firmware (`tramuntana_fw`) overrides the
console to run over the Pico's own USB port (CDC-ACM) instead, precisely to
keep those two header pins free for I2C. Any other app built for this board
that doesn't override it still uses UART0 for its boot log — keep J1's DIO6/DIO7
unplugged while flashing/testing those, and don't expect I2C0 to work in
them at the same time as the console.

`&spi0` and `&i2c0` are enabled alongside `dio-gpios`'s plain-GPIO view of
the same pins (DIO2-7): nothing stops an app from referencing both, but the
RP2350 only drives one function per pin at a time, so pick whichever one an
app actually needs for DIO2-7 rather than mixing them on the same pins.

The display node assumes a generic 1.8" 128x160 ST7735R module (GND, 3V3,
SCK, MOSI, RST, DC, CS, BL on J2, in that order; BL is tied straight to
+3V3, no GPIO control). If the picture comes out shifted or mirrored, that's
the well-known ST7735 clone quirk where `x-offset`/`y-offset`/`madctl` differ
by tab color — adjust them in `tramuntana_lite_r1a-common.dtsi`.
