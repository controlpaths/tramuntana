#!/usr/bin/env python3
"""Tramuntana Lite r1a - two-piece 3D-printable case (base + lid).

The board is clamped between four standoffs in the base and four turrets
in the lid, through its own M3 holes. Self-tapping screws go in from the
bottom: counterbore and clearance hole in the base, through the PCB, and
into a 2.5 mm pilot hole in each lid turret (M3). Meant for translucent filament:
the LEDs show through, only the display gets a window. The *_opaque lids are
for black (opaque) filament: each LED gets a hole through the ceiling and a
tube that hangs down to just above it, to be filled with clear silicone as a
light pipe.

Board geometry comes from ../kicad/tramuntana_lite_r1a.kicad_pcb:
80 x 60 mm, r = 3 mm corners, M3 holes 3.5 mm in from every edge, display
module outline (F.SilkS) 34 x 56 mm, Pico 2 (A1) at (68.19, 93.51) rotated
180 degrees, J1 horizontal 2x8 header on the right edge.

Frame: x to the right, y up (KiCad y is flipped), z up; the PCB's bottom
left corner of the outline bounding box is (0, 0). Units: mm.

    .venv/bin/python case.py        # -> *.step, *.stl, check report
"""
from pathlib import Path

import cadquery as cq

HERE = Path(__file__).resolve().parent

# ---------------------------------------------------------------- board
PCB_W, PCB_H, PCB_R, PCB_T = 80.0, 60.0, 3.0, 1.6
HOLES = [(3.5, 3.5), (76.5, 3.5), (3.5, 56.5), (76.5, 56.5)]
KICAD_X0, KICAD_Y1 = 50.0, 120.0          # KiCad (x, y) -> (x - 50, 120 - y)

def kicad(x, y):
    return x - KICAD_X0, KICAD_Y1 - y

DISPLAY = (*kicad(81.06, 117.0), *kicad(115.06, 61.0))     # x0, y0, x1, y1
DISPLAY_H = 6.0                                           # above the PCB top
PICO_C = kicad(68.19, 93.51)                              # module centre
PICO_W, PICO_L, PICO_H = 21.0, 51.0, 4.5                  # 1 mm PCB + parts
USB_X = PICO_C[0]                                         # micro-USB on the y = 0 edge
USB_Z = 1.0 + 1.3                                         # receptacle centre above the PCB top
J1_Y = 30.0                                               # connector centred on the board
J1_BODY = (kicad(118.30 + 4.04, 0)[0], kicad(118.30 + 6.58, 0)[0])   # plastic body, x
J1_PINS_X = kicad(130.88, 0)[0]                           # pin tips, just past the edge
# LEDs under the opaque lid's light pipes: centre, body (w, l), height above the PCB, pipe bore
LEDS = {
    "led_rgb": (kicad(123.645, 108.67), (5.0, 5.0), 1.6, 4.0),   # D3, RGB 5050
    "led_out": (kicad(123.2, 75.6), (1.6, 0.8), 0.55, 3.0),      # D7, 0603 on /3V3_TARGET
}

# ---------------------------------------------------------------- case
CLEAR = 0.5            # PCB edge to inner wall: covers +/-0.2 mm board outline
                       # tolerance plus ~0.2 mm undersize of printed cavities
WALL = 2.0
FLOOR = 2.0            # base floor
CEIL = 2.0             # lid ceiling
UNDER = 3.0            # PCB bottom above the floor: room for through-hole pins
OVER = DISPLAY_H + 0.5 # PCB top to the ceiling
EDGE_R = 2.0           # rounding of the outer top and bottom edges

PILOT_D = 2.5          # turret hole: an M3 cuts its own thread in the plastic
TURRET_D = 6.0         # turrets and standoffs (fit inside the M3 hole keep-out)
SCREW_CLEAR_D = 3.4    # M3 clearance in the base (printed holes come out small)
HEAD_D, HEAD_DEPTH = 6.2, 2.4   # counterbore for a <= 5.5 mm head, 2 mm tall: sits 0.4 mm recessed
BOSS_D = 8.5           # standoff foot: wraps the counterbore so the standoff stays joined to the floor
BOSS_H = 0.8           # foot height above the head seat, then a 45 deg taper down to TURRET_D

USB_OPEN = (12.0, 8.5)          # plug overmould, width x height
J1_OPEN_Y = (18.5, 41.5)        # header body is 19.84..40.16
J1_OPEN_Z = (-1.2, OVER - 1.0)  # relative to the PCB top: 6.7 mm tall, 1 mm below the ceiling

DISPLAY_WINDOW = True           # set False for a closed lid (a closed *_lid_nodisplay is always built too)
WINDOW = (31.0, 38.5, 1.0)      # width, height, corner radius (glass area)
WINDOW_Y0 = 13.5                # bottom of the window (the flex side of the module)
WINDOW_EXTRA_LEFT = 0.0         # widens the window on the left only (<0 narrows it)

PIPE_WALL = 1.2                 # light pipe tube wall (opaque lids)
PIPE_GAP = 0.5                  # tube end to the top of its LED

Z_PCB_BOT = FLOOR + UNDER
Z_PCB_TOP = Z_PCB_BOT + PCB_T
Z_CEIL = Z_PCB_TOP + OVER
Z_TOP = Z_CEIL + CEIL
OUT_W, OUT_H = PCB_W + 2 * (CLEAR + WALL), PCB_H + 2 * (CLEAR + WALL)
OUT_R = PCB_R + CLEAR + WALL


def rounded_box(x0, y0, w, h, r, z0, z1):
    return (cq.Workplane("XY").workplane(offset=z0)
            .center(x0 + w / 2, y0 + h / 2).rect(w, h).extrude(z1 - z0)
            .edges("|Z").fillet(r))


def box_between(x0, y0, z0, x1, y1, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def shell(window=DISPLAY_WINDOW):
    """Closed box with the cavity, openings cut; split later into two pieces."""
    outer = rounded_box(-CLEAR - WALL, -CLEAR - WALL, OUT_W, OUT_H, OUT_R, 0, Z_TOP)
    outer = outer.faces(">Z or <Z").edges().fillet(EDGE_R)
    cavity = rounded_box(-CLEAR, -CLEAR, PCB_W + 2 * CLEAR, PCB_H + 2 * CLEAR, PCB_R + CLEAR,
                         FLOOR, Z_CEIL)
    body = outer.cut(cavity)

    # micro-USB of the Pico 2, bottom edge (y = 0)
    uw, uh = USB_OPEN
    uy0, uy1 = -(CLEAR + WALL + 1), 1.5
    usb = (box_between(USB_X - uw / 2, uy0, Z_PCB_TOP + USB_Z - uh / 2,
                       USB_X + uw / 2, uy1, Z_PCB_TOP + USB_Z + uh / 2)
           .edges("|Y").fillet(1.5))
    body = body.cut(usb)

    # J1, right edge (x = 80): the header, dupont housings and an addon socket
    y0, y1 = J1_OPEN_Y
    j1 = (box_between(PCB_W - 1, y0, Z_PCB_TOP + J1_OPEN_Z[0],
                      PCB_W + CLEAR + WALL + 1, y1, Z_PCB_TOP + J1_OPEN_Z[1])
          .edges("|X").fillet(1.0))
    body = body.cut(j1)

    if window:
        ww, wh, wr = WINDOW
        cx = (DISPLAY[0] + DISPLAY[2]) / 2 - WINDOW_EXTRA_LEFT / 2
        ww += WINDOW_EXTRA_LEFT
        win = (cq.Workplane("XY").workplane(offset=Z_CEIL - 1)
               .center(cx, WINDOW_Y0 + wh / 2).rect(ww, wh).extrude(CEIL + 2)
               .edges("|Z").fillet(wr))
        body = body.cut(win)
    return body


def split(body):
    big = OUT_W + OUT_H + 50
    below = cq.Workplane("XY").box(big, big, Z_PCB_TOP, centered=(True, True, False)).translate((PCB_W / 2, PCB_H / 2, 0))
    above = cq.Workplane("XY").box(big, big, Z_TOP, centered=(True, True, False)).translate((PCB_W / 2, PCB_H / 2, Z_PCB_TOP))
    return body.intersect(below), body.intersect(above)


def base_features(base):
    for x, y in HOLES:
        standoff = cq.Workplane("XY").workplane(offset=FLOOR - 0.01).center(x, y).circle(TURRET_D / 2).extrude(UNDER + 0.01)
        # the counterbore is wider than the standoff and deeper than the floor: without
        # this foot the standoff would float over it
        z_foot = HEAD_DEPTH + BOSS_H
        taper = (BOSS_D - TURRET_D) / 2
        foot = (cq.Workplane("XZ").polyline([(x, FLOOR - 0.01), (x + BOSS_D / 2, FLOOR - 0.01),
                                            (x + BOSS_D / 2, z_foot), (x + TURRET_D / 2, z_foot + taper),
                                            (x, z_foot + taper)]).close()
                .revolve(360, (x, 0, 0), (x, 1, 0)).translate((0, y, 0)))
        base = base.union(standoff).union(foot)
        through = cq.Workplane("XY").workplane(offset=-1).center(x, y).circle(SCREW_CLEAR_D / 2).extrude(Z_PCB_BOT + 2)
        head = cq.Workplane("XY").workplane(offset=-1).center(x, y).circle(HEAD_D / 2).extrude(HEAD_DEPTH + 1)
        base = base.cut(through).cut(head)
    return base


def lid_features(lid, pipes=False):
    if pipes:
        # tube from the ceiling down to PIPE_GAP over the LED, bore open right through
        for (x, y), _, led_h, bore in LEDS.values():
            z0 = Z_PCB_TOP + led_h + PIPE_GAP
            tube = (cq.Workplane("XY").workplane(offset=z0).center(x, y)
                    .circle(bore / 2 + PIPE_WALL).extrude(Z_CEIL - z0 + 0.01))
            hole = cq.Workplane("XY").workplane(offset=z0 - 1).center(x, y).circle(bore / 2).extrude(Z_TOP - z0 + 2)
            lid = lid.union(tube).cut(hole)
    for x, y in HOLES:
        turret = (cq.Workplane("XY").workplane(offset=Z_PCB_TOP).center(x, y)
                  .circle(TURRET_D / 2).extrude(OVER + 0.01))
        lid = lid.union(turret)
        pilot = cq.Workplane("XY").workplane(offset=Z_PCB_TOP - 0.5).center(x, y).circle(PILOT_D / 2).extrude(OVER + 0.5)
        lid = lid.cut(pilot)                      # blind: stops at the ceiling
    return lid


def board_model():
    """The board and its tall parts, for the interference check and the assembly."""
    pcb = rounded_box(0, 0, PCB_W, PCB_H, PCB_R, Z_PCB_BOT, Z_PCB_TOP)
    for x, y in HOLES:
        pcb = pcb.cut(cq.Workplane("XY").workplane(offset=Z_PCB_BOT - 1).center(x, y).circle(1.6).extrude(PCB_T + 2))
    x0, y0, x1, y1 = DISPLAY
    display = cq.Workplane("XY").workplane(offset=Z_PCB_TOP).center((x0 + x1) / 2, (y0 + y1) / 2).rect(x1 - x0, y1 - y0).extrude(DISPLAY_H)
    pico = cq.Workplane("XY").workplane(offset=Z_PCB_TOP).center(*PICO_C).rect(PICO_W, PICO_L).extrude(PICO_H)
    usb = (cq.Workplane("XY").workplane(offset=Z_PCB_TOP + 1.0)
           .center(USB_X, PICO_C[1] - PICO_L / 2 + 2.5 - 1.3).rect(8.0, 5.0).extrude(2.6))
    bx0, bx1 = J1_BODY
    header = cq.Workplane("XY").workplane(offset=Z_PCB_TOP).center((bx0 + bx1) / 2, J1_Y).rect(bx1 - bx0, 20.32).extrude(5.08)
    pins = cq.Workplane("XY").workplane(offset=Z_PCB_TOP + 0.9).center((bx1 + J1_PINS_X) / 2, J1_Y).rect(J1_PINS_X - bx1, 18.5).extrude(3.6)
    tht = cq.Workplane("XY").workplane(offset=Z_PCB_BOT - 2.5).center(69.6, J1_Y).rect(4.0, 20.5).extrude(2.5)
    parts = {"pcb": pcb, "display": display, "pico": pico, "usb": usb, "header": header, "j1_pins": pins, "tht_pins": tht}
    for name, ((x, y), (w, l), h, _) in LEDS.items():
        parts[name] = cq.Workplane("XY").workplane(offset=Z_PCB_TOP).center(x, y).rect(w, l).extrude(h)
    return parts


def main():
    base, lid = split(shell())
    base, lid = base_features(base), lid_features(lid)
    lid_closed = lid_features(split(shell(window=False))[1])    # boards without display
    lid_opaque = lid_features(split(shell())[1], pipes=True)     # black filament: LED light pipes
    lid_opaque_closed = lid_features(split(shell(window=False))[1], pipes=True)
    board = board_model()
    lids = (("lid", lid), ("lid_nodisplay", lid_closed), ("lid_opaque", lid_opaque),
            ("lid_opaque_nodisplay", lid_opaque_closed))

    # interference check: every board part against both pieces
    report = []
    for name, part in board.items():
        for piece_name, piece in (("base", base),) + lids:
            v = part.intersect(piece).val().Volume() if part.intersect(piece).vals() else 0.0
            report.append((name, piece_name, v))
    worst = max(v for *_, v in report)
    pieces = {n: len(p.solids().vals()) for n, p in (("base", base),) + lids}

    cq.exporters.export(base, str(HERE / "tramuntana_lite_case_base.step"))
    cq.exporters.export(lid, str(HERE / "tramuntana_lite_case_lid.step"))
    cq.exporters.export(base, str(HERE / "tramuntana_lite_case_base.stl"), tolerance=0.02, angularTolerance=0.1)
    # the lid prints upside down: ceiling on the bed
    lid_print = lid.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, PCB_H, Z_TOP))
    cq.exporters.export(lid_print, str(HERE / "tramuntana_lite_case_lid.stl"), tolerance=0.02, angularTolerance=0.1)
    cq.exporters.export(lid_closed, str(HERE / "tramuntana_lite_case_lid_nodisplay.step"))
    lid_closed_print = lid_closed.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, PCB_H, Z_TOP))
    cq.exporters.export(lid_closed_print, str(HERE / "tramuntana_lite_case_lid_nodisplay.stl"), tolerance=0.02, angularTolerance=0.1)
    for name, piece in (("lid_opaque", lid_opaque), ("lid_opaque_nodisplay", lid_opaque_closed)):
        cq.exporters.export(piece, str(HERE / f"tramuntana_lite_case_{name}.step"))
        piece_print = piece.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, PCB_H, Z_TOP))
        cq.exporters.export(piece_print, str(HERE / f"tramuntana_lite_case_{name}.stl"), tolerance=0.02, angularTolerance=0.1)

    assy = cq.Assembly(name="tramuntana_lite_case")
    assy.add(base, name="base", color=cq.Color(0.8, 0.9, 1.0, 0.6))
    assy.add(lid, name="lid", color=cq.Color(0.8, 0.9, 1.0, 0.4))
    assy.add(board["pcb"], name="pcb", color=cq.Color(0.1, 0.3, 0.7))
    for name in ("display", "pico", "header"):
        assy.add(board[name], name=name, color=cq.Color(0.2, 0.2, 0.2))
    assy.save(str(HERE / "tramuntana_lite_case_assembly.step"))

    print(f"outer: {OUT_W:.1f} x {OUT_H:.1f} x {Z_TOP:.1f} mm   split at z = {Z_PCB_TOP:.1f}")
    print(f"base height {Z_PCB_TOP:.1f} mm, lid height {Z_TOP - Z_PCB_TOP:.1f} mm")
    print(f"screw: counterbore {HEAD_D} x {HEAD_DEPTH}, engages the turret from z = {Z_PCB_TOP:.1f} to {Z_CEIL:.1f} "
          f"-> length {HEAD_DEPTH:.0f} mm head seat .. {Z_CEIL - HEAD_DEPTH:.1f} mm max")
    for name, piece_name, v in report:
        flag = "" if v < 1e-3 else "   <-- COLLISION"
        print(f"  {name:9} vs {piece_name:20}: {v:8.4f} mm3{flag}")
    print("OK: no interference" if worst < 1e-3 else "FAIL: interference")
    for n, k in pieces.items():
        print(f"{n}: {k} solid" + ("" if k == 1 else "s   <-- FAIL: floating parts"))
    print("volumes: " + ", ".join(f"{n} {p.val().Volume() / 1000:.1f} cm3" for n, p in (("base", base),) + lids))
    for name, ((x, y), _, led_h, bore) in LEDS.items():
        z0 = Z_PCB_TOP + led_h + PIPE_GAP
        print(f"light pipe {name}: ({x:.2f}, {y:.2f}), bore {bore} mm, tube {bore + 2 * PIPE_WALL:.1f} mm, "
              f"{Z_TOP - z0:.1f} mm long incl. ceiling, {PIPE_GAP} mm above the LED")


if __name__ == "__main__":
    main()
