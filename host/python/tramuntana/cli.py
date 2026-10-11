"""
tramuntana - command line front-end for the Tramuntana Lite board.

    tramuntana info
    tramuntana pinout                          # what each J1 pin is, here and on the board
    tramuntana power on
    tramuntana i2c scan
    tramuntana i2c read 68 75 1                # register 0x75 of 0x68
    tramuntana i2c read 50 0010 2              # 16-bit register (24LC32 and up)
    tramuntana spi txn 9f --read 3             # JEDEC id of a SPI flash
    tramuntana scope -n 2048 --trigger rising --level 1.65 --csv trace.csv --plot
    tramuntana awg sine 1000 --amp 1 --offset 1.65
    tramuntana pwm set 4 10k 25                # DIO4: 10 kHz, 25 % duty
    tramuntana logic -n 100000 --rate 25e6 --trigger pattern --mask 0x80 --value 0 --vcd i2c.vcd
    tramuntana spi sniff                       # live, Ctrl-C to stop
    tramuntana uart status
    tramuntana uart ctrl esp --reset 2 --boot 3   # RTS/DTR -> EN/GPIO0, then esptool on the bridge port
    tramuntana emu i2c 76 --set d0=60          # answer as a BME280 (ID 0x60) on DIO6/DIO7
    tramuntana timeline --watch 0x08 --uart 115200 --i2c   # live, Ctrl-C to stop
    tramuntana ai install claude               # MCP server + skill for an AI agent

Bytes and addresses are hex; volts and frequencies are plain numbers
(1.65, 1e6, 400k).
"""
from __future__ import annotations

import argparse
import sys
import time

from . import pinout
from . import proto as P
from .device import Tramuntana, find_ports, SpiMessage, I2cMessage, DioEdge, EmuTransfer
from .proto import ProtoError, LinkError


def num(s: str) -> float:
    s = s.strip().lower().replace("hz", "")
    mult = 1
    if s.endswith("k"):
        mult, s = 1e3, s[:-1]
    elif s.endswith("m"):
        mult, s = 1e6, s[:-1]
    return float(s) * mult


def hexbytes(items) -> bytes:
    out = bytearray()
    for it in items:
        it = it.replace(":", "").replace(",", "")
        if len(it) > 2 and len(it) % 2 == 0:
            out += bytes.fromhex(it)
        else:
            out.append(int(it, 16))
    return bytes(out)


def hexdump(data: bytes, width: int = 16):
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        print(f"{i:08x}: {chunk.hex(' '):<{width * 3}} |{''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)}|")


def sniff_loop(t: Tramuntana, what: str):
    print(f"sniffing {what} - Ctrl-C to stop", file=sys.stderr)
    try:
        while True:
            for ev in t.events(timeout=0.2):
                if isinstance(ev, (SpiMessage, I2cMessage)):
                    print(f"{time.strftime('%H:%M:%S')} {ev}")
    except KeyboardInterrupt:
        pass


def plot_scope(cap):
    import matplotlib.pyplot as plt
    ts = [x * 1e3 for x in cap.times()]
    for ch in range(cap.channels):
        plt.plot(ts, cap.volts(ch), label=f"AIN{ch}")
    plt.axvline(0, color="grey", ls="--", lw=0.8)
    plt.xlabel("ms from trigger")
    plt.ylabel("V")
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.title(f"Tramuntana scope - {cap.rate_hz / 1e3:.1f} kS/s")
    plt.show()


def plot_logic(cap):
    import matplotlib.pyplot as plt
    ts = [(i - cap.trigger_index) * 1e6 / cap.rate_hz for i in range(cap.n)]
    fig, axes = plt.subplots(8, 1, sharex=True, figsize=(10, 6))
    for d in range(8):
        axes[d].step(ts, cap.channel(d), where="post", lw=0.8)
        axes[d].set_ylim(-0.2, 1.2)
        axes[d].set_yticks([])
        axes[d].set_ylabel(f"DIO{d}", rotation=0, ha="right", va="center")
    axes[-1].set_xlabel("us from trigger")
    fig.suptitle(f"Tramuntana logic - {cap.rate_hz / 1e6:g} MS/s")
    plt.show()


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tramuntana", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="API serial port (default: autodetect)")
    ap.add_argument("--serial", help="pick the board with this serial number")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="list connected boards")
    sub.add_parser("info")
    sub.add_parser("status")
    sub.add_parser("bootloader", help="reboot into the UF2 bootloader")
    p = sub.add_parser("power"); p.add_argument("state", choices=["on", "off"], nargs="?")
    p = sub.add_parser("pullups", aliases=["pullup"]); p.add_argument("state", choices=["on", "off"], nargs="?")
    p = sub.add_parser("led"); p.add_argument("colour", help="off red green blue yellow cyan magenta white auto")
    p = sub.add_parser("adc"); p.add_argument("channel", type=int, nargs="?", default=0)
    p = sub.add_parser("display"); p.add_argument("text", nargs="+")
    p = sub.add_parser("pinout", help="draw the J1 header (what each pin is and does) here and on the board's screen")
    p.add_argument("state", nargs="?", choices=["off"], help="'off' puts the dashboard back on the board's screen")
    p.add_argument("--seconds", type=int, default=60, help="how long the board shows it (default 60)")
    p.add_argument("--hold", action="store_true", help="keep it on the board until 'tramuntana pinout off'")
    p.add_argument("--no-board", action="store_true", help="only draw it in the terminal")
    p.add_argument("--no-color", action="store_true")
    p = sub.add_parser("dio")
    dsub = p.add_subparsers(dest="dcmd", required=True)
    dsub.add_parser("read")
    d = dsub.add_parser("dir"); d.add_argument("mask"); d.add_argument("out_mask")
    d = dsub.add_parser("write"); d.add_argument("mask"); d.add_argument("value")

    p = sub.add_parser("spi")
    ssub = p.add_subparsers(dest="scmd", required=True)
    s = ssub.add_parser("conf"); s.add_argument("freq"); s.add_argument("mode", type=int, nargs="?", default=0); s.add_argument("--lsb", action="store_true")
    ssub.add_parser("sel"); ssub.add_parser("unsel")
    s = ssub.add_parser("write"); s.add_argument("bytes", nargs="+")
    s = ssub.add_parser("read"); s.add_argument("n", type=int)
    s = ssub.add_parser("xfer"); s.add_argument("bytes", nargs="+")
    s = ssub.add_parser("txn"); s.add_argument("bytes", nargs="*"); s.add_argument("--read", type=int, default=0)
    s = ssub.add_parser("sniff"); s.add_argument("--mode", type=int)

    p = sub.add_parser("i2c")
    isub = p.add_subparsers(dest="icmd", required=True)
    s = isub.add_parser("conf"); s.add_argument("freq"); s.add_argument("--no-pullups", action="store_true")
    isub.add_parser("scan")
    s = isub.add_parser("read"); s.add_argument("addr"); s.add_argument("reg", nargs="?"); s.add_argument("n", type=int, nargs="?", default=1)
    s = isub.add_parser("write"); s.add_argument("addr"); s.add_argument("bytes", nargs="+")
    s = isub.add_parser("regs", help="dump registers 0x00..0xff"); s.add_argument("addr")
    s = isub.add_parser("sniff"); s.add_argument("--pullups", action="store_true")
    isub.add_parser("reset")

    p = sub.add_parser("scope")
    p.add_argument("-n", type=int, default=2048)
    p.add_argument("--ch", type=int, default=1, choices=[1, 2])
    p.add_argument("--rate", default="500k")
    p.add_argument("--trigger", default="none", choices=["none", "rising", "falling"])
    p.add_argument("--level", type=float, default=1.65)
    p.add_argument("--pre", type=int, default=0)
    p.add_argument("--csv"); p.add_argument("--plot", action="store_true")

    p = sub.add_parser("awg")
    asub = p.add_subparsers(dest="acmd", required=True)
    for shape in ("sine", "tri", "saw", "square", "stairs"):
        s = asub.add_parser(shape); s.add_argument("freq"); s.add_argument("--amp", type=float, default=1.0)
        s.add_argument("--offset", type=float, default=1.65); s.add_argument("--param", type=int, default=0)
    s = asub.add_parser("dc"); s.add_argument("volts", type=float)
    s = asub.add_parser("step"); s.add_argument("from_v", type=float); s.add_argument("to_v", type=float); s.add_argument("ms", type=int)
    asub.add_parser("off"); asub.add_parser("status")

    p = sub.add_parser("pwm", help="hardware PWM on the DIOs (pairs 7/6, 5/4, 3/2, 1/0 share a frequency)")
    psub = p.add_subparsers(dest="pcmd", required=True)
    s = psub.add_parser("set"); s.add_argument("dio", type=int); s.add_argument("freq")
    s.add_argument("duty", type=float, help="percent"); s.add_argument("--invert", action="store_true")
    s = psub.add_parser("phase"); s.add_argument("dio", type=int); s.add_argument("degrees", type=int)
    s = psub.add_parser("sync"); s.add_argument("mask", nargs="?", default="0", help="DIO mask, 0 = every live pair")
    s = psub.add_parser("stop"); s.add_argument("dio", help="a DIO number or 'all'")
    psub.add_parser("status")

    p = sub.add_parser("logic")
    p.add_argument("-n", type=int, default=16384)
    p.add_argument("--rate", default="150M")
    p.add_argument("--trigger", default="none", choices=["none", "rising", "falling", "pattern"])
    p.add_argument("--pin", type=int, default=0)
    p.add_argument("--mask", default="0"); p.add_argument("--value", default="0")
    p.add_argument("--pre", type=int, default=0)
    p.add_argument("--timeout", type=int, default=5000)
    p.add_argument("--vcd"); p.add_argument("--csv"); p.add_argument("--plot", action="store_true")

    p = sub.add_parser("uart")
    p.add_argument("mode", nargs="?", choices=["status", "auto", "on", "off", "ctrl"], default="status")
    p.add_argument("ctrl_mode", nargs="?", choices=["off", "direct", "esp"],
                   help="with 'ctrl': how RTS/DTR drive --reset/--boot")
    p.add_argument("--baud", type=int, default=0)
    p.add_argument("--reset", type=int, help="with 'ctrl': DIO wired to the target's reset/EN")
    p.add_argument("--boot", type=int, help="with 'ctrl': DIO wired to its boot pin (GPIO0, BOOT0...)")

    p = sub.add_parser("watch", help="print timestamped edges on DIOs until Ctrl-C")
    p.add_argument("mask", help="DIOs to watch (bit i = DIOi), e.g. 0x09")

    p = sub.add_parser("emu", help="answer as an I2C/SPI device from a 256-byte register file")
    esub = p.add_subparsers(dest="ecmd", required=True)
    s = esub.add_parser("i2c"); s.add_argument("addr"); s.add_argument("--scl", type=int, default=6)
    s.add_argument("--sda", type=int, default=7); s.add_argument("--pullups", action="store_true")
    s.add_argument("--set", action="append", default=[], metavar="REG=HEX..", help="load registers first")
    s = esub.add_parser("spi"); s.add_argument("--mode", type=int, default=0)
    for pin, dflt in (("cs", 2), ("sck", 5), ("mosi", 4), ("miso", 3)):
        s.add_argument(f"--{pin}", type=int, default=dflt)
    s.add_argument("--set", action="append", default=[], metavar="REG=HEX..", help="load registers first")
    esub.add_parser("off"); esub.add_parser("status")
    s = esub.add_parser("set"); s.add_argument("reg"); s.add_argument("bytes", nargs="+")
    s = esub.add_parser("regs"); s.add_argument("reg", nargs="?", default="0"); s.add_argument("n", type=int, nargs="?", default=16)
    esub.add_parser("log", help="print the target's transfers until Ctrl-C")

    p = sub.add_parser("timeline", help="live timeline of UART, sniffers, DIO edges, emulator... until Ctrl-C")
    p.add_argument("--watch", default="0", help="DIO mask to record edges of")
    p.add_argument("--uart", type=int, metavar="BAUD", help="also open the target's UART at this baud")
    p.add_argument("--i2c", action="store_true", help="also sniff I2C")
    p.add_argument("--spi", type=int, metavar="MODE", help="also sniff SPI in this mode")

    p = sub.add_parser("ai", help="register the MCP server and the tramuntana-debug skill with an AI agent")
    asub = p.add_subparsers(dest="aicmd", required=True)
    for action in ("install", "uninstall"):
        s = asub.add_parser(action)
        s.add_argument("agent", choices=["claude", "opencode", "copilot", "all"])
        where = s.add_mutually_exclusive_group()
        where.add_argument("--user", action="store_true",
                           help="for every project of this user (the default)")
        where.add_argument("--project", metavar="PATH",
                           help="only for the project in PATH (config files and skill go there)")

    a = ap.parse_args(argv)
    if a.cmd == "pullup":
        a.cmd = "pullups"

    if a.cmd == "list":
        for b in find_ports():
            print(f"{b['serial']}: shell={b['shell']} api={b['api']} uart={b['uart']}")
        return 0
    if a.cmd == "pinout":
        return cmd_pinout(a)
    if a.cmd == "ai":
        from . import ai
        return (ai.install if a.aicmd == "install" else ai.uninstall)(a.agent, a.project)

    try:
        t = Tramuntana(a.port, a.serial)
    except LinkError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    try:
        with t:
            return run(t, a)
    except ProtoError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except LinkError as e:
        print(f"link error: {e}", file=sys.stderr)
        return 3
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


def cmd_pinout(a) -> int:
    """Works without a board: the terminal drawing is the main output, the
    board's screen is a bonus when one is plugged in."""
    if a.state != "off":
        print(pinout.render(colour=not a.no_color and pinout.use_colour(sys.stdout)))
    if a.no_board:
        return 0
    try:
        t = Tramuntana(a.port, a.serial)
    except LinkError as e:
        if a.state == "off":
            print(f"error: {e}", file=sys.stderr)
            return 1
        print("\n(no board connected: drawn here only)", file=sys.stderr)
        return 0
    try:
        with t:
            if a.state == "off":
                t.display_pinout(0)
                return 0
            t.display_pinout(None if a.hold else a.seconds)
    except ProtoError as e:
        if e.status == 8:
            print("\n(the board's firmware has no display pinout: flash the current one)", file=sys.stderr)
            return 0
        print(f"error: {e}", file=sys.stderr)
        return 2
    except LinkError as e:
        print(f"link error: {e}", file=sys.stderr)
        return 3
    how = "until 'tramuntana pinout off'" if a.hold else f"for {a.seconds} s"
    print(f"\nAlso on the board's screen {how}.")
    return 0


def run(t: Tramuntana, a) -> int:
    if a.cmd == "info":
        for k, v in t.info().items():
            print(f"{k:14} {v}")
        print(f"{'port':14} {t.port}")
    elif a.cmd == "status":
        for k, v in t.status().items():
            print(f"{k:12} {v}")
    elif a.cmd == "bootloader":
        t.bootloader(); print("rebooting into the bootloader")
    elif a.cmd == "power":
        print("power:", "on" if t.power(None if a.state is None else a.state == "on") else "off")
    elif a.cmd == "pullups":
        print("pull-ups:", "on" if t.pullups(None if a.state is None else a.state == "on") else "off")
    elif a.cmd == "led":
        m = {"off": (0, 0, 0), "red": (1, 0, 0), "green": (0, 1, 0), "blue": (0, 0, 1), "yellow": (1, 1, 0),
             "cyan": (0, 1, 1), "magenta": (1, 0, 1), "white": (1, 1, 1)}
        t.led(None) if a.colour == "auto" else t.led(*m[a.colour])
    elif a.cmd == "adc":
        print(f"AIN{a.channel} = {t.adc(a.channel):.3f} V")
    elif a.cmd == "display":
        t.display(" ".join(a.text))
    elif a.cmd == "dio":
        if a.dcmd == "read":
            v = t.dio_read(); print(f"DIO7..0 = {v:08b} (0x{v:02x})")
        elif a.dcmd == "dir":
            t.dio_dir(int(a.mask, 0), int(a.out_mask, 0))
        else:
            t.dio_write(int(a.mask, 0), int(a.value, 0))
    elif a.cmd == "spi":
        c = a.scmd
        if c == "conf":
            print("spi:", t.spi.config(int(num(a.freq)), a.mode, a.lsb), "Hz")
        elif c == "sel":
            t.spi.sel()
        elif c == "unsel":
            t.spi.unsel()
        elif c == "write":
            t.spi.write(hexbytes(a.bytes))
        elif c == "read":
            hexdump(t.spi.read(a.n))
        elif c == "xfer":
            hexdump(t.spi.xfer(hexbytes(a.bytes)))
        elif c == "txn":
            r = t.spi.txn(hexbytes(a.bytes), a.read)
            if r:
                hexdump(r)
        elif c == "sniff":
            t.spi.sniff(True, a.mode)
            try:
                sniff_loop(t, "SPI (DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK)")
            finally:
                t.spi.sniff(False)
    elif a.cmd == "i2c":
        c = a.icmd
        if c == "conf":
            print("i2c:", t.i2c.config(int(num(a.freq)), not a.no_pullups), "Hz")
        elif c == "scan":
            found = t.i2c.scan()
            print("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f")
            for row in range(8):
                print(f"{row * 16:02x}: " + " ".join(f"{x:02x}" if x in found else "--" for x in range(row * 16, row * 16 + 16)))
            print(f"{len(found)} device(s)")
        elif c == "read":
            addr = int(a.addr, 16)
            if a.reg is None:
                hexdump(t.i2c.read(addr, a.n))
            else:
                # 2 hex digits per register byte: "75" -> 0x75, "0010" -> 00 10.
                reg = a.reg if len(a.reg) % 2 == 0 else "0" + a.reg
                hexdump(t.i2c.write_read(addr, bytes.fromhex(reg), a.n))
        elif c == "write":
            print("acked:", t.i2c.write(int(a.addr, 16), hexbytes(a.bytes)))
        elif c == "regs":
            addr = int(a.addr, 16)
            data = bytearray()
            for reg in range(0, 256, 16):
                try:
                    data += t.i2c.reg_read(addr, reg, 16)
                except ProtoError:
                    data += b"\xff" * 16
            hexdump(bytes(data))
        elif c == "sniff":
            t.i2c.sniff(True, a.pullups)
            try:
                sniff_loop(t, "I2C (DIO6 SCL, DIO7 SDA)")
            finally:
                t.i2c.sniff(False)
        elif c == "reset":
            t.i2c.reset_bus()
    elif a.cmd == "scope":
        cap = t.scope.capture(n=a.n, channels=a.ch, rate_hz=int(num(a.rate)), trigger=a.trigger,
                              level=a.level, pre=a.pre)
        print(f"{cap.n} samples x {cap.channels} ch @ {cap.rate_hz} S/s, trigger at {cap.trigger_index}")
        for ch in range(cap.channels):
            st = cap.stats(ch)
            f = f"{st['freq']:.1f} Hz" if st["freq"] else "n/a"
            print(f"AIN{ch}: min {st['min']:.3f} max {st['max']:.3f} Vpp {st['vpp']:.3f} mean {st['mean']:.3f} rms {st['rms']:.3f} freq {f}")
        if a.csv:
            cap.to_csv(a.csv); print("saved", a.csv)
        if a.plot:
            plot_scope(cap)
    elif a.cmd == "awg":
        c = a.acmd
        if c in ("sine", "tri", "saw", "square", "stairs"):
            r = t.awg.set(c, num(a.freq), a.amp, a.offset, a.param)
            print(f"awg: {c} {r['freq_hz']:.3f} Hz (table {r['table']}, top {r['top']})")
        elif c == "dc":
            t.awg.dc(a.volts)
        elif c == "step":
            t.awg.step(a.from_v, a.to_v, a.ms)
        elif c == "off":
            t.awg.off()
        else:
            print(t.awg.status())
    elif a.cmd == "logic":
        cap = t.logic.capture(n=a.n, rate_hz=int(num(a.rate)), trigger=a.trigger, pin=a.pin,
                              mask=int(a.mask, 0), value=int(a.value, 0), pre=a.pre, timeout_ms=a.timeout)
        print(f"{cap.n} samples @ {cap.rate_hz} S/s ({cap.n / cap.rate_hz * 1e6:.1f} us), trigger at {cap.trigger_index}")
        for d in range(8):
            print(f"DIO{d}: {len(cap.edges(d))} transition(s)")
        if a.vcd:
            cap.to_vcd(a.vcd); print("saved", a.vcd)
        if a.csv:
            cap.to_csv(a.csv); print("saved", a.csv)
        if a.plot:
            plot_logic(cap)
    elif a.cmd == "uart":
        if a.mode == "ctrl":
            t.uart.ctrl(a.ctrl_mode or "esp", a.reset, a.boot)
        elif a.mode != "status":
            t.uart.config(a.mode, a.baud)
        st = t.uart.status()
        ports = [b for b in find_ports() if b["api"] == t.port]
        print(f"bridge: {'active' if st['active'] else 'idle'}, {st['baud']} baud, "
              f"{st['rx_bytes']} rx / {st['tx_bytes']} tx  (port: {ports[0]['uart'] if ports else '?'})")
        if st.get("ctrl", "off") != "off":
            print(f"control lines: {st['ctrl']}, reset DIO{st['reset_dio']} (RTS) {'LOW' if st['reset_low'] else 'released'}, "
                  f"boot DIO{st['boot_dio']} (DTR) {'LOW' if st['boot_low'] else 'released'}")
    elif a.cmd == "watch":
        t.dio_watch(int(a.mask, 0))
        print("watching - Ctrl-C to stop", file=sys.stderr)
        try:
            while True:
                for ev in t.events(timeout=0.2):
                    if isinstance(ev, DioEdge):
                        print(f"{ev.t_us / 1e6:12.6f}  {ev}  levels {ev.levels:08b}")
        except KeyboardInterrupt:
            pass
        t.dio_watch(0)
    elif a.cmd == "pwm":
        run_pwm(t, a)
    elif a.cmd == "emu":
        run_emu(t, a)
    elif a.cmd == "timeline":
        run_timeline(t, a)
    return 0


def run_pwm(t: Tramuntana, a):
    if a.pcmd == "set":
        r = t.pwm.set(a.dio, num(a.freq), a.duty, a.invert)
        print(f"DIO{a.dio}: {r['freq_hz']:g} Hz, {a.duty:g} %{' inverted' if a.invert else ''} "
              f"(top {r['top']}, div {r['div']})")
        return
    if a.pcmd == "phase":
        t.pwm.phase(a.dio, a.degrees)
        return
    if a.pcmd == "sync":
        t.pwm.sync(int(a.mask, 0))
        return
    if a.pcmd == "stop":
        t.pwm.stop(0xFF if a.dio == "all" else 1 << int(a.dio))
        return
    for dio, s in enumerate(t.pwm.status()):
        if not s["active"]:
            print(f"DIO{dio}  off")
            continue
        print(f"DIO{dio}  {s['actual_hz']:g} Hz  {s['duty']:.2f} %  phase {s['phase']}"
              + ("  inverted" if s["invert"] else "") + ("  (pin taken by another instrument)" if s["lost"] else ""))


def run_emu(t: Tramuntana, a):
    def load(sets):
        for item in sets:
            reg, _, data = item.partition("=")
            t.emu.set_regs(int(reg, 16), hexbytes(data.replace(",", " ").split()))
    if a.ecmd == "i2c":
        load(a.set)
        t.emu.i2c(int(a.addr, 16), a.scl, a.sda, a.pullups)
        print(f"answering at 0x{int(a.addr, 16):02x} on SCL DIO{a.scl} / SDA DIO{a.sda}")
    elif a.ecmd == "spi":
        load(a.set)
        t.emu.spi(a.mode, a.cs, a.sck, a.mosi, a.miso)
        print(f"answering as SPI mode {a.mode}: CS DIO{a.cs} SCK DIO{a.sck} MOSI DIO{a.mosi} MISO DIO{a.miso}")
    elif a.ecmd == "off":
        t.emu.stop()
    elif a.ecmd == "set":
        t.emu.set_regs(int(a.reg, 16), hexbytes(a.bytes))
    elif a.ecmd == "regs":
        hexdump(t.emu.regs(int(a.reg, 16), a.n))
    elif a.ecmd == "status":
        print(t.emu.status())
    elif a.ecmd == "log":
        print("logging - Ctrl-C to stop", file=sys.stderr)
        try:
            while True:
                for ev in t.events(timeout=0.2):
                    if isinstance(ev, EmuTransfer):
                        print(f"{ev.t_us / 1e6:12.6f}  {ev}")
        except KeyboardInterrupt:
            pass


def run_timeline(t: Tramuntana, a):
    from .timeline import Timeline
    tl = Timeline(t)
    tl.start(watch=int(a.watch, 0))
    tap = tl.open_uart(a.uart) if a.uart else None
    if a.i2c:
        t.i2c.sniff(True)
    if a.spi is not None:
        t.spi.sniff(True, a.spi)
    print("recording - Ctrl-C to stop", file=sys.stderr)
    shown = 0
    try:
        while True:
            time.sleep(0.1)
            entries = tl.select()
            for e in entries[shown:]:
                print(e)
            shown = len(entries)
    except KeyboardInterrupt:
        pass
    if a.i2c:
        t.i2c.sniff(False)
    if a.spi is not None:
        t.spi.sniff(False)
    if tap:
        tap.close()
    tl.stop()


if __name__ == "__main__":
    sys.exit(main())
