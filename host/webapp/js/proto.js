/*
 * Tramuntana binary API over Web Serial - mirrors fw/apps/tramuntana_fw/
 * src/proto.h and host/python/tramuntana/proto.py.
 *
 *   0xAA 0x55 | cmd u8 | seq u8 | len u16 LE | payload | crc16 LE
 *
 * Replies: cmd | 0x80, same seq, payload[0] = status. Events 0xF0+.
 */
'use strict';

const PROTO = {
  VERSION: 1, MAGIC: 'TRMT', MAX_PAYLOAD: 4104,
  CMD: {
    PING: 0x01, INFO: 0x02, BOOTLOADER: 0x03, LED: 0x04, POWER: 0x05, PULLUPS: 0x06,
    DIO_DIR: 0x07, DIO_WRITE: 0x08, DIO_READ: 0x09, STATUS: 0x0A, DISPLAY_TEXT: 0x0B, ADC_READ: 0x0C, DISPLAY_PINOUT: 0x0D,
    TIME: 0x0E, DIO_WATCH: 0x0F,
    SPI_CONF: 0x10, SPI_SEL: 0x11, SPI_UNSEL: 0x12, SPI_WRITE: 0x13, SPI_READ: 0x14, SPI_XFER: 0x15,
    SPI_TXN: 0x16, SPI_SNIFF: 0x17,
    I2C_CONF: 0x20, I2C_SCAN: 0x21, I2C_WRITE: 0x22, I2C_READ: 0x23, I2C_WRITE_READ: 0x24,
    I2C_START: 0x25, I2C_STOP: 0x26, I2C_RAW_WRITE: 0x27, I2C_RAW_READ: 0x28, I2C_SNIFF: 0x29, I2C_RESET: 0x2A,
    SCOPE_ARM: 0x30, SCOPE_STATUS: 0x31, SCOPE_READ: 0x32, SCOPE_ABORT: 0x33,
    AWG_SET: 0x40, AWG_OFF: 0x41, AWG_STATUS: 0x42, AWG_ARB_LOAD: 0x43, AWG_DC: 0x44, AWG_STEP: 0x45,
    PWM_SET: 0x48, PWM_PHASE: 0x49, PWM_SYNC: 0x4A, PWM_STOP: 0x4B, PWM_STATUS: 0x4C,
    LOGIC_ARM: 0x50, LOGIC_STATUS: 0x51, LOGIC_READ: 0x52, LOGIC_ABORT: 0x53,
    UART_CONF: 0x60, UART_STATUS: 0x61, UART_CTRL: 0x62,
    EMU_I2C: 0x64, EMU_SPI: 0x65, EMU_REGS: 0x66, EMU_STATUS: 0x67,
    SEQ_LOAD: 0x68, SEQ_RUN: 0x69, SEQ_STATUS: 0x6A, SEQ_RESULT: 0x6B, SEQ_ABORT: 0x6C,
  },
  EVT: {
    LOG: 0xF0, SPI_MSG: 0xF1, I2C_MSG: 0xF2, CAPTURE_DONE: 0xF3,
    DIO: 0xF4, EMU: 0xF5, LINE: 0xF6, SEQ_DONE: 0xF7,
  },
  EVT_FLAG_STAMPED: 0x80,   /* sniffer events: a board timestamp follows the flags byte */
  SEQ_STATES: ['idle', 'running', 'done', 'failed', 'aborted', 'timeout', 'invalid'],
  STATUS: ['OK', 'invalid argument', 'busy', 'timeout', 'bus error / NAK', 'not supported',
           'not permitted (pins held / wrong state)', 'out of memory', 'unknown command', 'wrong state'],
  CAP: ['idle', 'armed', 'running', 'done', 'timeout', 'aborted'],
  USB_VID: 0x2fe3, USB_PID: 0x0100,
};

function crc16(bytes, start, end) {
  let crc = 0xFFFF;
  for (let i = start; i < end; i++) {
    crc ^= bytes[i] << 8;
    for (let b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
      crc &= 0xFFFF;
    }
  }
  return crc;
}

function encodeFrame(cmd, seq, payload) {
  const n = payload.length;
  const f = new Uint8Array(8 + n);
  f[0] = 0xAA; f[1] = 0x55; f[2] = cmd; f[3] = seq; f[4] = n & 0xFF; f[5] = n >> 8;
  f.set(payload, 6);
  const c = crc16(f, 2, 6 + n);
  f[6 + n] = c & 0xFF; f[7 + n] = c >> 8;
  return f;
}

/* Little helpers for building payloads. */
const P = {
  u8: (v) => [v & 0xFF],
  u16: (v) => [v & 0xFF, (v >> 8) & 0xFF],
  u32: (v) => [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >>> 24) & 0xFF],
  bytes: (...parts) => Uint8Array.from(parts.flat()),
  rd16: (b, o) => b[o] | (b[o + 1] << 8),
  rd32: (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0,
};

class ProtoError extends Error {
  constructor(cmd, status) {
    super(`command 0x${cmd.toString(16)}: ${PROTO.STATUS[status] || status}`);
    this.cmd = cmd; this.status = status;
  }
}

/* Web Serial link: opens the API port, frames requests, dispatches events. */
class Link {
  constructor() {
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.seq = 0;
    this.pending = new Map();      // seq -> {resolve, reject, cmd, timer}
    this.buf = new Uint8Array(0);
    this.onEvent = null;           // (evt, payload) => void
    this.onClose = null;
    this.queue = Promise.resolve(); // requests are serialised
    this.timeout = 3000;
  }

  get connected() { return this.port !== null; }

  async open(port) {
    await port.open({ baudRate: 115200, bufferSize: 65536 });
    this.port = port;
    this.writer = port.writable.getWriter();
    this._readLoop();
    /* Is this the API port? The shell port stays silent to a frame. */
    try {
      const r = await this.request(PROTO.CMD.PING, new Uint8Array(0), 800);
      const magic = String.fromCharCode(...r.slice(0, 4));
      if (magic !== PROTO.MAGIC) throw new Error('not a Tramuntana API port');
      this.protoVersion = r[4];
    } catch (e) {
      await this.close();
      throw new Error('That port did not answer as the API port. Pick the second Tramuntana port (the "API" interface).');
    }
  }

  async close() {
    const port = this.port;
    this.port = null;
    try { if (this.reader) await this.reader.cancel(); } catch (e) { /* ignore */ }
    try { if (this.writer) { this.writer.releaseLock(); } } catch (e) { /* ignore */ }
    this.writer = null;
    try { if (port) await port.close(); } catch (e) { /* ignore */ }
    for (const [, p] of this.pending) { clearTimeout(p.timer); p.reject(new Error('link closed')); }
    this.pending.clear();
    if (this.onClose) this.onClose();
  }

  async _readLoop() {
    while (this.port && this.port.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          this._feed(value);
        }
      } catch (e) {
        /* device unplugged or cancelled */
      } finally {
        try { this.reader.releaseLock(); } catch (e) { /* ignore */ }
        this.reader = null;
      }
      if (!this.port) break;
      if (this.port && !this.port.readable) break;
    }
    if (this.port) { await this.close(); }
  }

  _feed(chunk) {
    const b = new Uint8Array(this.buf.length + chunk.length);
    b.set(this.buf); b.set(chunk, this.buf.length);
    let buf = b;
    for (;;) {
      let i = 0;
      while (i + 1 < buf.length && !(buf[i] === 0xAA && buf[i + 1] === 0x55)) i++;
      if (i > 0) buf = buf.subarray(i);
      if (buf.length < 6) break;
      const len = buf[4] | (buf[5] << 8);
      if (len > PROTO.MAX_PAYLOAD) { buf = buf.subarray(2); continue; }
      const total = 8 + len;
      if (buf.length < total) break;
      const crc = buf[6 + len] | (buf[7 + len] << 8);
      if (crc !== crc16(buf, 2, 6 + len)) { buf = buf.subarray(2); continue; }
      const cmd = buf[2], seq = buf[3];
      const payload = buf.slice(6, 6 + len);
      buf = buf.subarray(total);
      if (cmd >= 0xF0) {
        if (this.onEvent) { try { this.onEvent(cmd, payload); } catch (e) { console.error(e); } }
      } else if (cmd & 0x80) {
        const p = this.pending.get(seq);
        if (p) {
          this.pending.delete(seq);
          clearTimeout(p.timer);
          if (payload.length === 0) p.reject(new Error('empty reply'));
          else if (payload[0] !== 0) p.reject(new ProtoError(cmd & 0x7F, payload[0]));
          else p.resolve(payload.slice(1));
        }
      }
    }
    this.buf = buf.slice();
  }

  /* Sends a command; resolves with the reply data (after the status byte). */
  request(cmd, payload = new Uint8Array(0), timeout = this.timeout) {
    const run = () => new Promise((resolve, reject) => {
      if (!this.writer) { reject(new Error('not connected')); return; }
      this.seq = (this.seq % 255) + 1;
      const seq = this.seq;
      const timer = setTimeout(() => {
        this.pending.delete(seq);
        reject(new Error(`no reply to command 0x${cmd.toString(16)}`));
      }, timeout);
      this.pending.set(seq, { resolve, reject, cmd, timer });
      this.writer.write(encodeFrame(cmd, seq, payload)).catch((e) => {
        this.pending.delete(seq); clearTimeout(timer); reject(e);
      });
    });
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => {});
    return p;
  }
}

/* --- event decoders --- */

/* Sniffer events carry the board time of their START / CS edge right after
 * the flags byte when stamps are on (CMD_TIME 1): returns [tUs, payload
 * without it]. */
function unstamp(p) {
  if (!(p[0] & PROTO.EVT_FLAG_STAMPED)) return [null, p];
  const rest = new Uint8Array(p.length - 4);
  rest[0] = p[0]; rest.set(p.subarray(5), 1);
  return [P.rd32(p, 1), rest];
}

function decodeSpiEvent(raw) {
  const [tUs, p] = unstamp(raw);
  const total = P.rd16(p, 1);
  const k = (p.length - 3) >> 1;
  return { type: 'spi', tUs, total, mosi: p.slice(3, 3 + k), miso: p.slice(3 + k, 3 + 2 * k), overrun: !!(p[0] & 1) };
}

function decodeDioEvent(p) {
  return { type: 'dio', tUs: P.rd32(p, 0), levels: p[4], changed: p[5], lost: p[6] };
}

function decodeEmuEvent(p) {
  return {
    type: 'emu', tUs: P.rd32(p, 0), bus: p[4] === 1 ? 'i2c' : 'spi', read: !!(p[5] & 1),
    nak: !!(p[5] & 2), truncated: !!(p[5] & 4), noStart: !!(p[5] & 8),
    address: p[6], reg: p[7], data: p.slice(8),
  };
}

function decodeLineEvent(p) {
  return { type: 'line', tUs: P.rd32(p, 0), dtr: !!p[4], rts: !!p[5] };
}

function decodeSeqDoneEvent(p) {
  return {
    type: 'seq_done', tUs: P.rd32(p, 0), state: PROTO.SEQ_STATES[p[4]] || String(p[4]), error: p[5],
    pc: P.rd16(p, 6), resultLen: P.rd16(p, 8),
  };
}

function decodeI2cEvent(raw) {
  const [tUs, p] = unstamp(raw);
  const msg = { type: 'i2c', tUs, frames: [], overrun: !!(p[0] & 1), truncated: !!(p[0] & 2) };
  let cur = null, pendingAck = null;
  for (let i = 1; i + 1 < p.length; i += 2) {
    const t = p[i], v = p[i + 1];
    if (t === 1) { cur = { address: v >> 1, read: !!(v & 1), ack: null, data: [] }; msg.frames.push(cur); pendingAck = 'addr'; }
    else if (t === 3 && cur) { cur.data.push({ byte: v, ack: false }); pendingAck = 'data'; }
    else if (t === 2 && cur) {
      if (pendingAck === 'addr') cur.ack = !!v;
      else if (pendingAck === 'data' && cur.data.length) cur.data[cur.data.length - 1].ack = !!v;
      pendingAck = null;
    }
  }
  return msg;
}

function hex(bytes, sep = ' ') {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join(sep);
}

function formatI2c(m) {
  return m.frames.map((f) => `S ${f.address.toString(16).padStart(2, '0').toUpperCase()}${f.read ? 'R' : 'W'}${f.ack ? '+' : '-'}` +
    f.data.map((d) => ` ${d.byte.toString(16).padStart(2, '0').toUpperCase()}${d.ack ? '+' : '-'}`).join('')).join('  ') +
    ' P' + (m.overrun ? ' OVERRUN' : '') + (m.truncated ? ' (truncated)' : '');
}
