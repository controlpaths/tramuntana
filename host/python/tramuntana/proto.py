"""
Wire protocol of the Tramuntana binary API - mirrors fw/apps/tramuntana_fw/src/proto.h.

Frame (little-endian):

    0xAA 0x55 | cmd u8 | seq u8 | len u16 | payload[len] | crc u16

crc: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over cmd..payload.
Replies set cmd | 0x80, echo seq and start with a status byte. Events
(0xF0..0xFF) arrive unsolicited with seq 0.
"""
from __future__ import annotations

import struct
import threading
import time
from collections import deque
from typing import Callable, Deque, Optional, Tuple

PROTO_VERSION = 1
SOF = b"\xaa\x55"
MAGIC = b"TRMT"
MAX_PAYLOAD = 4104
REPLY_FLAG = 0x80

# --- status codes ------------------------------------------------------------
STATUS_NAMES = {
    0: "OK",
    1: "invalid argument",
    2: "busy",
    3: "timeout",
    4: "bus error / NAK",
    5: "not supported",
    6: "not permitted (pins held / wrong state)",
    7: "out of memory",
    8: "unknown command",
    9: "wrong state",
}

# --- commands ----------------------------------------------------------------
CMD_PING = 0x01
CMD_INFO = 0x02
CMD_BOOTLOADER = 0x03
CMD_LED = 0x04
CMD_POWER = 0x05
CMD_PULLUPS = 0x06
CMD_DIO_DIR = 0x07
CMD_DIO_WRITE = 0x08
CMD_DIO_READ = 0x09
CMD_STATUS = 0x0A
CMD_DISPLAY_TEXT = 0x0B
CMD_ADC_READ = 0x0C
CMD_DISPLAY_PINOUT = 0x0D
CMD_TIME = 0x0E
CMD_DIO_WATCH = 0x0F

CMD_SPI_CONF = 0x10
CMD_SPI_SEL = 0x11
CMD_SPI_UNSEL = 0x12
CMD_SPI_WRITE = 0x13
CMD_SPI_READ = 0x14
CMD_SPI_XFER = 0x15
CMD_SPI_TXN = 0x16
CMD_SPI_SNIFF = 0x17

CMD_I2C_CONF = 0x20
CMD_I2C_SCAN = 0x21
CMD_I2C_WRITE = 0x22
CMD_I2C_READ = 0x23
CMD_I2C_WRITE_READ = 0x24
CMD_I2C_START = 0x25
CMD_I2C_STOP = 0x26
CMD_I2C_RAW_WRITE = 0x27
CMD_I2C_RAW_READ = 0x28
CMD_I2C_SNIFF = 0x29
CMD_I2C_RESET = 0x2A

CMD_SCOPE_ARM = 0x30
CMD_SCOPE_STATUS = 0x31
CMD_SCOPE_READ = 0x32
CMD_SCOPE_ABORT = 0x33

CMD_AWG_SET = 0x40
CMD_AWG_OFF = 0x41
CMD_AWG_STATUS = 0x42
CMD_AWG_ARB_LOAD = 0x43
CMD_AWG_DC = 0x44
CMD_AWG_STEP = 0x45

CMD_PWM_SET = 0x48
CMD_PWM_PHASE = 0x49
CMD_PWM_SYNC = 0x4A
CMD_PWM_STOP = 0x4B
CMD_PWM_STATUS = 0x4C

CMD_LOGIC_ARM = 0x50
CMD_LOGIC_STATUS = 0x51
CMD_LOGIC_READ = 0x52
CMD_LOGIC_ABORT = 0x53

CMD_UART_CONF = 0x60
CMD_UART_STATUS = 0x61
CMD_UART_CTRL = 0x62

CMD_EMU_I2C = 0x64
CMD_EMU_SPI = 0x65
CMD_EMU_REGS = 0x66
CMD_EMU_STATUS = 0x67

CMD_SEQ_LOAD = 0x68
CMD_SEQ_RUN = 0x69
CMD_SEQ_STATUS = 0x6A
CMD_SEQ_RESULT = 0x6B
CMD_SEQ_ABORT = 0x6C

EVT_LOG = 0xF0
EVT_SPI_MSG = 0xF1
EVT_I2C_MSG = 0xF2
EVT_CAPTURE_DONE = 0xF3
EVT_DIO = 0xF4
EVT_EMU = 0xF5
EVT_LINE = 0xF6
EVT_SEQ_DONE = 0xF7

# capture states
CAP_IDLE, CAP_ARMED, CAP_RUNNING, CAP_DONE, CAP_TIMEOUT, CAP_ABORTED = range(6)
CAP_NAMES = ["idle", "armed", "running", "done", "timeout", "aborted"]


class ProtoError(Exception):
    """The board answered with an error status.

    Attributes:
        cmd: the command id.
        status: 1 invalid argument, 2 busy, 3 timeout, 4 bus error / NAK,
            5 not supported, 6 not permitted, 7 no memory, 8 unknown
            command, 9 wrong state (``STATUS_NAMES`` has the texts).
    """

    def __init__(self, cmd: int, status: int):
        self.cmd = cmd
        self.status = status
        super().__init__(f"command 0x{cmd:02x}: {STATUS_NAMES.get(status, status)}")


class LinkError(Exception):
    """No reply, a malformed reply, or no board/wrong port."""


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), the frame checksum."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def encode(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    """Builds a frame: ``AA 55 | cmd | seq | len u16 | payload | crc16`` (little-endian)."""
    body = bytes([cmd, seq]) + struct.pack("<H", len(payload)) + payload
    return SOF + body + struct.pack("<H", crc16(body))


class Link:
    """Frames requests/replies over a pyserial port and queues events.

    A background reader thread parses everything the board sends; replies
    are matched by sequence number, events go to ``on_event`` (if set) and
    to the ``events`` deque.
    """

    def __init__(self, ser, timeout: float = 2.0):
        self.ser = ser
        self.timeout = timeout
        self.on_event: Optional[Callable[[int, bytes], None]] = None
        self.events: Deque[Tuple[int, bytes]] = deque(maxlen=1000)
        self._listeners: list = []
        self._seq = 0
        self._lock = threading.Lock()
        self._reply_cv = threading.Condition()
        self._replies = {}
        self._buf = bytearray()
        self._stop = False
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def close(self):
        """Stops the reader thread (the serial port is closed by its owner)."""
        self._stop = True
        try:
            self._thread.join(0.5)
        except Exception:
            pass

    # --- reader -------------------------------------------------------------
    def _reader(self):
        # read(1) blocks until the first byte (or the port timeout), then
        # whatever else is already waiting comes in one go - the reply to
        # a request is delivered the moment it lands, not after a timeout.
        while not self._stop:
            try:
                chunk = self.ser.read(1)
                if chunk:
                    waiting = self.ser.in_waiting
                    if waiting:
                        chunk += self.ser.read(waiting)
            except Exception:
                break
            if not chunk:
                continue
            self._buf += chunk
            self._parse()

    def _parse(self):
        buf = self._buf
        while True:
            i = buf.find(SOF)
            if i < 0:
                del buf[:-1]
                return
            if i > 0:
                del buf[:i]
            if len(buf) < 6:
                return
            ln = struct.unpack_from("<H", buf, 4)[0]
            if ln > MAX_PAYLOAD:
                del buf[:2]
                continue
            total = 6 + ln + 2
            if len(buf) < total:
                return
            cmd, seq = buf[2], buf[3]
            payload = bytes(buf[6:6 + ln])
            crc = struct.unpack_from("<H", buf, 6 + ln)[0]
            if crc != crc16(bytes(buf[2:6 + ln])):
                del buf[:2]
                continue
            del buf[:total]
            if cmd >= 0xF0:
                self.events.append((cmd, payload))
                for fn in [self.on_event] + self._listeners:
                    if fn:
                        try:
                            fn(cmd, payload)
                        except Exception:
                            pass
            elif cmd & REPLY_FLAG:
                with self._reply_cv:
                    self._replies[seq] = (cmd & 0x7F, payload)
                    self._reply_cv.notify_all()

    def add_listener(self, fn: Callable[[int, bytes], None]):
        """Calls ``fn(cmd, payload)`` for every event, in the reader thread,
        besides ``on_event`` and the ``events`` deque (so several consumers,
        e.g. a ``Timeline`` and a script, can watch the same events)."""
        self._listeners.append(fn)

    def remove_listener(self, fn: Callable[[int, bytes], None]):
        """Undoes ``add_listener``."""
        if fn in self._listeners:
            self._listeners.remove(fn)

    # --- requests -----------------------------------------------------------
    def request(self, cmd: int, payload: bytes = b"", timeout: Optional[float] = None) -> bytes:
        """Sends one command and waits for its reply.

        Args:
            cmd: command id (``CMD_*``).
            payload: request bytes (at most ``MAX_PAYLOAD``).
            timeout: seconds; None uses the link's default.

        Returns:
            The reply data after the status byte.

        Raises:
            ProtoError: non-OK status.
            LinkError: no reply in time, or a mismatched reply.
        """
        if len(payload) > MAX_PAYLOAD:
            raise ValueError("payload too long")
        with self._lock:
            self._seq = (self._seq % 255) + 1
            seq = self._seq
            with self._reply_cv:
                self._replies.pop(seq, None)
            self.ser.write(encode(cmd, seq, payload))
            deadline = time.monotonic() + (timeout or self.timeout)
            with self._reply_cv:
                while seq not in self._replies:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise LinkError(f"no reply to command 0x{cmd:02x}")
                    self._reply_cv.wait(remaining)
                rcmd, data = self._replies.pop(seq)
        if rcmd != cmd:
            raise LinkError(f"reply mismatch: sent 0x{cmd:02x}, got 0x{rcmd:02x}")
        if not data:
            raise LinkError("empty reply")
        if data[0] != 0:
            raise ProtoError(cmd, data[0])
        return data[1:]
