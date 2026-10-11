#!/usr/bin/env python3
"""Passive Bluetooth LE scanner with an nRF24L01(+) on the SPI master.

The nRF24L01 is no Bluetooth chip, but it shares BLE's 1 Mbit/s GFSK, so it
can listen on the three BLE advertising channels (37/38/39 = 2402/2426/2480
MHz) if it uses the BLE advertising access address (0x8E89BED6) as its own
address and its CRC is turned off. Everything else is done here: bit order,
BLE de-whitening, the BLE CRC-24, and decoding the advertising PDU (device
address, name, manufacturer). It only listens - it never transmits.

Limits: the nRF24 payload is 32 bytes, so the longest advertisements (up to
39 bytes + CRC) arrive truncated; those are shown once the same device has
been heard three times, since their CRC can't be checked (and their fields
are the most frequent decode seen). Some nRF24L01 modules
(clone chips) also return the first bit of every 32-bit payload word stuck
at 1 - one bit of every fourth byte is lost. Packets that fit get those bits
back by trying both values against the CRC; in truncated ones the address
bit is fixed where the address type allows it, and shown as '?' otherwise. There is no RSSI, only
the chip's "received power > -64 dBm" flag (shown as "near"). Only legacy
advertising on the 1M PHY is visible (no BLE 5 extended/coded advertising).

Wiring (nRF24L01 module -> J1):

    VCC  -> J1.1  3V3 (switched, turned on here)    GND -> J1.2 GND
    CSN  -> J1.11 DIO2        SCK  -> J1.14 DIO5
    MOSI -> J1.13 DIO4        MISO -> J1.12 DIO3
    CE   -> J1.9  DIO0        IRQ  -> J1.10 DIO1

    python3 nrf24_ble_scan.py                 # 30 s, live table
    python3 nrf24_ble_scan.py -t 0 --raw      # until Ctrl-C, print every packet
"""
import argparse
import collections
import sys
import time

from tramuntana import Tramuntana

# nRF24L01 registers and commands.
CONFIG, EN_AA, EN_RXADDR, SETUP_AW, SETUP_RETR, RF_CH, RF_SETUP, STATUS = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
RPD, RX_ADDR_P0, RX_PW_P0, FIFO_STATUS, DYNPD, FEATURE = 0x09, 0x0A, 0x11, 0x17, 0x1C, 0x1D
W_REGISTER, R_RX_PAYLOAD, FLUSH_RX = 0x20, 0x61, 0xE2
RX_DR = 0x40

# BLE advertising channel index -> nRF24 RF_CH (MHz above 2400).
ADV_CHANNELS = {37: 2, 38: 26, 39: 80}
# Access address 0x8E89BED6: BLE sends it LSB first, the nRF24 MSB first, so
# each byte is bit-reversed; the register takes the LSByte first.
BLE_ADDRESS = bytes([0x71, 0x91, 0x7D, 0x6B])

PDU_TYPES = {0: "ADV_IND", 1: "ADV_DIRECT_IND", 2: "ADV_NONCONN_IND", 3: "SCAN_REQ",
             4: "SCAN_RSP", 5: "CONNECT_IND", 6: "ADV_SCAN_IND"}
COMPANIES = {0x004C: "Apple", 0x0006: "Microsoft", 0x0075: "Samsung", 0x00E0: "Google",
             0x0059: "Nordic", 0x0157: "Huami", 0x038F: "Xiaomi", 0x0087: "Garmin",
             0x0499: "Ruuvi", 0x02E5: "Espressif", 0x0131: "Cypress", 0x000F: "Broadcom"}
APPLE_TYPES = {0x02: "iBeacon", 0x05: "AirDrop", 0x07: "AirPods", 0x09: "AirPlay", 0x0C: "Handoff",
               0x10: "Nearby", 0x12: "FindMy"}

REVERSE = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))


# --- BLE link layer -----------------------------------------------------------------

def dewhiten(data, channel):
    """BLE data whitening (x^7 + x^4 + 1, seeded with the channel index);
    the same operation whitens and de-whitens. Bytes in BLE bit order."""
    pos = [1] + [(channel >> (5 - i)) & 1 for i in range(6)]   # position 0..6
    out = bytearray()
    for byte in data:
        res = 0
        for bit in range(8):
            w = pos[6]
            res |= (((byte >> bit) & 1) ^ w) << bit
            pos = [w, pos[0], pos[1], pos[2], pos[3] ^ w, pos[4], pos[5]]
        out.append(res)
    return bytes(out)


def crc24(data, init=0x555555):
    """BLE CRC-24 (x^24 + x^10 + x^9 + x^6 + x^4 + x^3 + x + 1) over data,
    bits taken LSB first as on air."""
    crc = init
    for byte in data:
        for bit in range(8):
            fb = ((byte >> bit) & 1) ^ (crc >> 23)
            crc = (crc << 1) & 0xFFFFFF
            if fb:
                crc ^= 0x00065B
    return crc


def crc_on_air(b):
    """The 3 CRC bytes as received (BLE bit order) -> CRC value; the CRC is
    sent most significant bit first."""
    v = 0
    for k in range(24):
        v |= ((b[k // 8] >> (k % 8)) & 1) << (23 - k)
    return v


# Payload bits the module may return stuck at 1: MSB of raw bytes 3, 7, 11..,
# which is bit 0 of the same bytes once bit-reversed into BLE order.
STUCK = [i for i in range(3, 32, 4)]


def fix_with_crc(pkt, end):
    """Tries every value of the stuck bits inside pkt[:end + 3] until the
    CRC matches. Returns the fixed packet or None."""
    pos = [i for i in STUCK if i < end + 3]
    base = bytearray(pkt)
    for i in pos:
        base[i] &= 0xFE
    for combo in range(1 << len(pos)):
        p = bytearray(base)
        for k, i in enumerate(pos):
            p[i] |= (combo >> k) & 1
        if crc24(p[:end]) == crc_on_air(p[end:end + 3]):
            return bytes(p)
    return None


def fmt_mac(addr, unsure):
    """6 address bytes (little-endian) -> "AA:BB:..", '?' for the low hex
    digit of a byte whose bit 0 is unknown."""
    out = []
    for i in reversed(range(6)):
        h = f"{addr[i]:02X}"
        out.append(h[0] + "?" if i in unsure else h)
    return ":".join(out)


def decode(raw, channel):
    """32 bytes from the nRF24 -> dict, or None if it isn't a sane PDU.
    'crc' is True when checked (and the stuck bits recovered), None when
    the packet didn't fit in 32 bytes."""
    pkt = dewhiten(bytes(REVERSE[b] for b in raw), channel)
    ptype, length = pkt[0] & 0x0F, pkt[1] & 0x3F
    if ptype not in PDU_TYPES or not 6 <= length <= 37:
        return None
    end = 2 + length
    unsure = set()
    if end + 3 <= len(pkt):
        pkt = fix_with_crc(pkt, end)
        if pkt is None:
            return None
        crc = True
    else:
        crc = None
        random_addr = bool(pkt[0] & 0x40)
        addr = bytearray(pkt[2:8])
        if not random_addr:
            addr[5] &= 0xFE                  # public: the multicast bit is always 0
        else:
            unsure.add(5)
        unsure.add(1)                        # pkt[3]
        pkt = pkt[:2] + bytes(addr) + pkt[8:]
    adv = {"type": PDU_TYPES[ptype], "random": bool(pkt[0] & 0x40), "crc": crc,
           "mac": fmt_mac(pkt[2:8], unsure), "key": bytes(pkt[2:8][i] & (0xFE if i in (1, 5) else 0xFF)
                                                          for i in range(6)),
           "name": None, "maker": None, "info": [], "truncated": crc is None}
    if ptype in (0, 2, 4, 6):
        parse_ad(pkt[8:min(end, len(pkt))], adv)
    return adv


def parse_ad(data, adv):
    """Advertising data: [len][type][value...] structures."""
    i = 0
    while i + 1 < len(data):
        n = data[i]
        if n == 0:
            break
        t, v = data[i + 1], data[i + 2:i + 1 + n]
        if t in (0x08, 0x09):
            name = v.decode("ascii", "replace")
            if name.isprintable() and "\ufffd" not in name:
                adv["name"] = name + ("" if len(v) == n - 1 else "...")
        elif t == 0xFF and len(v) >= 2:
            cid = v[0] | v[1] << 8
            adv["maker"] = COMPANIES.get(cid, f"0x{cid:04X}")
            if cid == 0x004C and len(v) >= 3:
                adv["info"].append(APPLE_TYPES.get(v[2], f"type 0x{v[2]:02X}"))
        elif t in (0x02, 0x03) and len(v) >= 2:
            adv["info"].append("uuid16 " + ",".join(f"{v[k] | v[k + 1] << 8:04X}" for k in range(0, len(v) - 1, 2)))
        elif t == 0x16 and len(v) >= 2:
            uuid = v[0] | v[1] << 8
            adv["info"].append({0xFE9F: "Google", 0xFD6F: "ExposureNotif", 0xFEAA: "Eddystone"}
                               .get(uuid, f"svc {uuid:04X}"))
        elif t == 0x0A and len(v) == 1:
            adv["info"].append(f"tx {int.from_bytes(v, 'little', signed=True)} dBm")
        i += 1 + n


# --- nRF24L01 ------------------------------------------------------------------------

class Nrf24:
    def __init__(self, t, ce, irq):
        self.t, self.ce, self.irq = t, ce, irq
        t.spi.config(8_000_000, mode=0)
        mask = (1 << ce) | ((1 << irq) if irq is not None else 0)
        t.dio_dir(mask, 1 << ce)
        self.ce_set(False)

    def ce_set(self, on):
        self.t.dio_write(1 << self.ce, (1 << self.ce) if on else 0)

    def w(self, reg, *data):
        self.t.spi.txn(bytes([W_REGISTER | reg, *data]))

    def r(self, reg, n=1):
        return self.t.spi.txn(bytes([reg]), read_n=n)

    def setup_ble_rx(self):
        self.w(CONFIG, 0x00)                 # power down while configuring
        self.w(EN_AA, 0x00)                  # no auto-ack
        self.w(EN_RXADDR, 0x01)              # pipe 0 only
        self.w(SETUP_AW, 0x02)               # 4-byte address
        self.w(SETUP_RETR, 0x00)
        self.w(RF_SETUP, 0x06)               # 1 Mbit/s, max power (RX doesn't care)
        self.w(DYNPD, 0x00)
        self.w(FEATURE, 0x00)
        self.w(RX_ADDR_P0, *BLE_ADDRESS)
        self.w(RX_PW_P0, 32)                 # fixed 32-byte payload
        self.w(STATUS, 0x70)
        self.w(CONFIG, 0x03)                 # PWR_UP, PRIM_RX, CRC off, IRQs on
        time.sleep(0.005)
        if self.r(SETUP_AW)[0] != 0x02 or self.r(RX_PW_P0)[0] != 32:
            raise RuntimeError("nRF24L01 not answering on SPI (check CSN/SCK/MOSI/MISO and power)")

    def tune(self, rf_ch):
        self.ce_set(False)
        self.w(RF_CH, rf_ch)
        self.t.spi.txn(bytes([FLUSH_RX]))
        self.w(STATUS, 0x70)
        self.ce_set(True)

    def pending(self):
        if self.irq is not None:
            return not (self.t.dio_read() >> self.irq) & 1
        return bool(self.r(STATUS)[0] & RX_DR)

    def read_all(self):
        """Drains the RX FIFO: list of (payload, near)."""
        out = []
        for _ in range(3):
            if self.r(FIFO_STATUS)[0] & 0x01:            # RX_EMPTY
                break
            near = bool(self.r(RPD)[0] & 1)
            out.append((self.t.spi.txn(bytes([R_RX_PAYLOAD]), read_n=32), near))
        self.w(STATUS, RX_DR)
        return out

    def power_down(self):
        self.ce_set(False)
        self.w(CONFIG, 0x00)


# --- main ------------------------------------------------------------------------------

def show_table(devices, elapsed, stats, clear):
    rows = sorted((d for d in devices.values() if d["ok"]), key=lambda d: -d["count"])
    lines = [f"BLE scan  {elapsed:5.1f} s   packets {stats['pkts']} (crc ok {stats['crc']}, "
             f"truncated {stats['trunc']}, rejected {stats['bad']})   devices {len(rows)}",
             f"{'MAC':<17} {'addr':<6} {'pkts':>4} {'ch':<8} {'near':<4}  {'name':<20} {'maker':<10} info"]
    for d in rows:
        chans = ",".join(str(c) for c in sorted(d["channels"]))
        name, maker, info = (c.most_common(1)[0][0] if c else None for c in d["fields"])
        lines.append(f"{d['mac']:<17} {'rand' if d['random'] else 'public':<6} {d['count']:>4} {chans:<8} "
                     f"{'yes' if d['near'] else '':<4}  {(name or '')[:20]:<20} {(maker or '')[:10]:<10} "
                     f"{' '.join(info or ())}")
    if clear:
        sys.stdout.write("\033[H\033[J")
    print("\n".join(lines), flush=True)


def main():
    ap = argparse.ArgumentParser(description="Passive BLE advertising scanner with an nRF24L01")
    ap.add_argument("-t", "--time", type=float, default=30, help="seconds to scan, 0 = until Ctrl-C (default 30)")
    ap.add_argument("--dwell", type=float, default=0.6, help="seconds on each advertising channel")
    ap.add_argument("--ce", type=int, default=0, help="DIO wired to CE (default 0)")
    ap.add_argument("--irq", type=int, default=1, help="DIO wired to IRQ, -1 = poll STATUS over SPI (default 1)")
    ap.add_argument("--raw", action="store_true", help="print every packet instead of the table")
    args = ap.parse_args()

    devices, stats = {}, {"pkts": 0, "crc": 0, "trunc": 0, "bad": 0}
    clear = sys.stdout.isatty() and not args.raw
    with Tramuntana() as t:
        t.power(True)
        time.sleep(0.1)
        radio = Nrf24(t, args.ce, None if args.irq < 0 else args.irq)
        radio.setup_ble_rx()
        chans = list(ADV_CHANNELS)
        ci = 0
        radio.tune(ADV_CHANNELS[chans[ci]])
        t0 = t_hop = t_show = time.time()
        try:
            while args.time <= 0 or time.time() - t0 < args.time:
                now = time.time()
                if now - t_hop >= args.dwell:
                    ci = (ci + 1) % len(chans)
                    radio.tune(ADV_CHANNELS[chans[ci]])
                    t_hop = now
                if not args.raw and now - t_show >= 1.0:
                    show_table(devices, now - t0, stats, clear)
                    t_show = now
                if not radio.pending():
                    continue
                for raw, near in radio.read_all():
                    stats["pkts"] += 1
                    adv = decode(raw, chans[ci])
                    if adv is None:
                        stats["bad"] += 1
                        continue
                    stats["crc" if adv["crc"] else "trunc"] += 1
                    key = (adv["key"], adv["random"])
                    d = devices.setdefault(key, {"mac": adv["mac"], "random": adv["random"], "count": 0,
                                                 "crc_ok": False, "channels": set(), "near": False,
                                                 "fields": [collections.Counter() for _ in range(3)], "ok": False})
                    d["count"] += 1
                    d["channels"].add(chans[ci])
                    d["near"] |= near
                    fields = (adv["name"], adv["maker"], tuple(sorted(adv["info"])))
                    if adv["crc"] and not d["crc_ok"]:
                        # first checked packet: its address and fields are exact
                        d.update(crc_ok=True, mac=adv["mac"], fields=[collections.Counter() for _ in range(3)])
                    if adv["crc"] or not d["crc_ok"]:
                        for counter, value in zip(d["fields"], fields):
                            if value:
                                counter[value] += 1
                    d["ok"] = d["crc_ok"] or d["count"] >= 3
                    if args.raw:
                        print(f"ch{chans[ci]} {adv['type']:<15} {adv['mac']} {'crc ok' if adv['crc'] else 'trunc ':<6} "
                              f"{'near ' if near else '':<5}{adv['name'] or ''} {adv['maker'] or ''} "
                              f"{' '.join(adv['info'])}", flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            radio.power_down()
        show_table(devices, time.time() - t0, stats, False)


if __name__ == "__main__":
    main()
