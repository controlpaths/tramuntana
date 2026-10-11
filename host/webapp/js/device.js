/*
 * Tramuntana device object - the browser twin of host/python/tramuntana/
 * device.py. Wraps a connected Link (proto.js) with one class per
 * instrument; every method packs a request and unpacks the reply exactly
 * like the Python library, so app.js never touches raw bytes.
 */
'use strict';

window.T = window.T || {};

const VREF_MV = 3300;
const ADC_CLOCK_HZ = 48_000_000;
const SYSCLK_HZ = 150_000_000;

/* --- capture result helpers ------------------------------------------- */

/* channels: array of Uint16Array (one per analog channel), rateHz: per
 * channel sample rate, triggerIndex: sample index of the trigger. */
class ScopeCapture {
  constructor(channels, rateHz, triggerIndex) {
    this.channels = channels;
    this.rateHz = rateHz;
    this.triggerIndex = triggerIndex;
  }

  get n() { return this.channels[0] ? this.channels[0].length : 0; }

  volts(ch = 0) {
    const raw = this.channels[ch];
    const out = new Float32Array(raw.length);
    for (let i = 0; i < raw.length; i++) out[i] = (raw[i] * VREF_MV) / 4095 / 1000;
    return out;
  }

  timeAt(i) { return (i - this.triggerIndex) / this.rateHz; }

  stats(ch = 0) {
    const v = this.volts(ch);
    const n = v.length;
    if (n === 0) return null;
    let sum = 0, sumSq = 0, min = Infinity, max = -Infinity;
    for (let i = 0; i < n; i++) {
      sum += v[i]; sumSq += v[i] * v[i];
      if (v[i] < min) min = v[i];
      if (v[i] > max) max = v[i];
    }
    const mean = sum / n;
    const rms = Math.sqrt(sumSq / n);
    const vpp = max - min;
    let freq = null;
    if (vpp > 0.05) {
      const hyst = vpp * 0.05;
      let armed = v[0] < mean - hyst;
      let first = -1, last = -1, count = 0;
      for (let i = 1; i < n; i++) {
        if (armed && v[i] > mean + hyst) { armed = false; if (first < 0) first = i; last = i; count++; }
        else if (!armed && v[i] < mean - hyst) { armed = true; }
      }
      if (count >= 2) freq = ((count - 1) * this.rateHz) / (last - first);
    }
    return { min, max, vpp, mean, rms, freq };
  }

  toCsvBlob() {
    const ch = this.channels.length;
    const lines = ['t,' + Array.from({ length: ch }, (_, c) => `ch${c}`).join(',')];
    for (let i = 0; i < this.n; i++) {
      const t = this.timeAt(i);
      const row = [t.toExponential(6)];
      for (let c = 0; c < ch; c++) row.push(((this.channels[c][i] * VREF_MV) / 4095 / 1000).toFixed(4));
      lines.push(row.join(','));
    }
    return new Blob([lines.join('\n')], { type: 'text/csv' });
  }
}

/* samples: Uint8Array, bit i = DIOi. */
class LogicCapture {
  constructor(samples, rateHz, triggerIndex) {
    this.samples = samples;
    this.rateHz = rateHz;
    this.triggerIndex = triggerIndex;
  }

  get n() { return this.samples.length; }

  channel(dio) {
    const out = new Uint8Array(this.n);
    for (let i = 0; i < this.n; i++) out[i] = (this.samples[i] >> dio) & 1;
    return out;
  }

  edges(dio) {
    const out = [];
    let prev = null;
    for (let i = 0; i < this.n; i++) {
      const b = (this.samples[i] >> dio) & 1;
      if (prev !== null && b !== prev) out.push([i, b]);
      prev = b;
    }
    return out;
  }

  toVcdBlob(names) {
    names = names || Array.from({ length: 8 }, (_, i) => `DIO${i}`);
    const psPerSample = Math.round(1e12 / this.rateHz);
    const lines = ['$timescale 1ps $end', '$scope module tramuntana $end'];
    for (let i = 0; i < 8; i++) lines.push(`$var wire 1 ${String.fromCharCode(33 + i)} ${names[i]} $end`);
    lines.push('$upscope $end', '$enddefinitions $end');
    let prev = null;
    for (let i = 0; i < this.n; i++) {
      const s = this.samples[i];
      if (prev === null || s !== prev) {
        lines.push(`#${i * psPerSample}`);
        for (let b = 0; b < 8; b++) {
          if (prev === null || (((s ^ prev) >> b) & 1)) lines.push(`${(s >> b) & 1}${String.fromCharCode(33 + b)}`);
        }
        prev = s;
      }
    }
    lines.push(`#${this.n * psPerSample}`);
    return new Blob([lines.join('\n')], { type: 'text/plain' });
  }

  toCsvBlob() {
    const lines = ['t,' + Array.from({ length: 8 }, (_, i) => `DIO${i}`).join(',')];
    for (let i = 0; i < this.n; i++) {
      const t = (i - this.triggerIndex) / this.rateHz;
      const bits = [];
      for (let b = 0; b < 8; b++) bits.push((this.samples[i] >> b) & 1);
      lines.push(t.toExponential(6) + ',' + bits.join(','));
    }
    return new Blob([lines.join('\n')], { type: 'text/csv' });
  }
}

/* --- instruments -------------------------------------------------------- */

class Spi {
  constructor(link) { this.link = link; }

  async config(freqHz, mode = 0, lsbFirst = false) {
    const r = await this.link.request(PROTO.CMD.SPI_CONF, P.bytes(P.u32(freqHz), P.u8(mode), P.u8(lsbFirst ? 1 : 0)));
    return P.rd32(r, 0);
  }

  sel() { return this.link.request(PROTO.CMD.SPI_SEL); }
  unsel() { return this.link.request(PROTO.CMD.SPI_UNSEL); }

  async write(bytes) {
    for (let i = 0; i < bytes.length; i += 4096) await this.link.request(PROTO.CMD.SPI_WRITE, bytes.slice(i, i + 4096));
  }

  async read(n, fill = 0x00) {
    const out = new Uint8Array(n);
    let got = 0;
    while (got < n) {
      const k = Math.min(4096, n - got);
      const r = await this.link.request(PROTO.CMD.SPI_READ, P.bytes(P.u16(k), P.u8(fill)));
      out.set(r, got); got += r.length;
    }
    return out;
  }

  async xfer(bytes) {
    const out = new Uint8Array(bytes.length);
    for (let i = 0; i < bytes.length; i += 4096) {
      const r = await this.link.request(PROTO.CMD.SPI_XFER, bytes.slice(i, i + 4096));
      out.set(r, i);
    }
    return out;
  }

  txn(bytes, readN = 0) {
    return this.link.request(PROTO.CMD.SPI_TXN, P.bytes(P.u16(readN), Array.from(bytes)));
  }

  sniff(on, mode = null) {
    const p = mode === null ? P.bytes(P.u8(on ? 1 : 0)) : P.bytes(P.u8(on ? 1 : 0), P.u8(mode));
    return this.link.request(PROTO.CMD.SPI_SNIFF, p);
  }
}

class I2c {
  constructor(link) { this.link = link; }

  async config(hz, pullups = true) {
    const r = await this.link.request(PROTO.CMD.I2C_CONF, P.bytes(P.u32(hz), P.u8(pullups ? 1 : 0)));
    return P.rd32(r, 0);
  }

  async scan() {
    const bm = await this.link.request(PROTO.CMD.I2C_SCAN);
    const out = [];
    for (let a = 0; a < 128; a++) if ((bm[a >> 3] >> (a & 7)) & 1) out.push(a);
    return out;
  }

  async write(addr, data) {
    const r = await this.link.request(PROTO.CMD.I2C_WRITE, P.bytes(P.u8(addr), Array.from(data)));
    return P.rd16(r, 0);
  }

  read(addr, n) { return this.link.request(PROTO.CMD.I2C_READ, P.bytes(P.u8(addr), P.u16(n))); }

  writeRead(addr, data, n) {
    return this.link.request(PROTO.CMD.I2C_WRITE_READ,
      P.bytes(P.u8(addr), P.u8(data.length), Array.from(data), P.u16(n)));
  }

  regRead(addr, reg, n = 1) { return this.writeRead(addr, [reg], n); }
  regWrite(addr, reg, data) { return this.write(addr, [reg, ...data]); }

  async start(addr, read = false) {
    const r = await this.link.request(PROTO.CMD.I2C_START, P.bytes(P.u8((addr << 1) | (read ? 1 : 0))));
    return !!r[0];
  }

  stop() { return this.link.request(PROTO.CMD.I2C_STOP); }

  async rawWrite(data) {
    const r = await this.link.request(PROTO.CMD.I2C_RAW_WRITE, Uint8Array.from(data));
    return P.rd16(r, 0);
  }

  rawRead(n, nackLast = true) {
    return this.link.request(PROTO.CMD.I2C_RAW_READ, P.bytes(P.u16(n), P.u8(nackLast ? 1 : 0)));
  }

  resetBus() { return this.link.request(PROTO.CMD.I2C_RESET); }
  sniff(on, pullups = false) { return this.link.request(PROTO.CMD.I2C_SNIFF, P.bytes(P.u8(on ? 1 : 0), P.u8(pullups ? 1 : 0))); }
}

class Scope {
  constructor(link) { this.link = link; }

  async arm({ n = 1024, channels = 1, rateHz = null, trigger = 'none', level = 1.65, pre = 0, timeoutMs = 5000 } = {}) {
    const clkdiv = rateHz === null ? 96 : Math.max(96, Math.min(65535, Math.round(ADC_CLOCK_HZ / (rateHz * channels))));
    const lvl = Math.max(0, Math.min(4095, Math.round((level * 4095) / (VREF_MV / 1000))));
    const trig = { none: 0, rising: 1, falling: 2 }[trigger];
    const r = await this.link.request(PROTO.CMD.SCOPE_ARM,
      P.bytes(P.u8(channels), P.u16(n), P.u16(clkdiv), P.u8(trig), P.u16(lvl), P.u16(pre), P.u16(timeoutMs)));
    return Math.floor(P.rd32(r, 0) / channels);
  }

  async status() {
    const r = await this.link.request(PROTO.CMD.SCOPE_STATUS);
    return {
      state: PROTO.CAP[r[0]], channels: r[1], n: P.rd16(r, 2), rateHz: P.rd32(r, 4), triggerIndex: P.rd16(r, 8),
    };
  }

  abort() { return this.link.request(PROTO.CMD.SCOPE_ABORT); }

  async wait(timeoutMs = 10000, onTick = null) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const st = await this.status();
      if (onTick) onTick(st);
      if (st.state !== 'armed' && st.state !== 'running') return st;
      if (Date.now() > deadline) throw new Error('scope: capture did not finish');
      await new Promise((r) => setTimeout(r, 20));
    }
  }

  async read() {
    const st = await this.status();
    if (st.state !== 'done') throw new Error(`scope: no capture (${st.state})`);
    const total = st.n * st.channels;
    const raw = new Uint16Array(total);
    let off = 0;
    while (off < total) {
      const k = Math.min(2048, total - off);
      const r = await this.link.request(PROTO.CMD.SCOPE_READ, P.bytes(P.u16(off), P.u16(k)));
      for (let i = 0; i < r.length / 2; i++) raw[off + i] = P.rd16(r, i * 2);
      off += r.length / 2;
    }
    const ch = st.channels;
    const perChannel = Array.from({ length: ch }, (_, c) => {
      const out = new Uint16Array(st.n);
      for (let i = 0; i < st.n; i++) out[i] = raw[i * ch + c];
      return out;
    });
    return new ScopeCapture(perChannel, Math.floor(st.rateHz / ch), st.triggerIndex);
  }

  async capture(opts, onTick) {
    await this.arm(opts);
    const st = await this.wait((opts && opts.timeoutMs || 5000) + 5000, onTick);
    if (st.state !== 'done') throw new Error(`scope: ${st.state}`);
    return this.read();
  }
}

class Awg {
  constructor(link) { this.link = link; }

  async set(shape, freqHz, ampV = 1.0, offsetV = 1.65, param = 0) {
    const shapeId = { sine: 0, tri: 1, saw: 2, square: 3, stairs: 4, arb: 5 }[shape];
    const r = await this.link.request(PROTO.CMD.AWG_SET,
      P.bytes(P.u8(shapeId), P.u32(Math.round(freqHz)), P.u16(Math.round(ampV * 1000)),
             P.u16(Math.round(offsetV * 1000)), P.u16(param)));
    return { freqHz: P.rd32(r, 0) / 1000, table: P.rd16(r, 4), top: P.rd16(r, 6) };
  }

  sine(f, a = 1, o = 1.65) { return this.set('sine', f, a, o); }
  tri(f, a = 1, o = 1.65) { return this.set('tri', f, a, o); }
  saw(f, a = 1, o = 1.65) { return this.set('saw', f, a, o); }
  square(f, a = 1, o = 1.65, duty = 50) { return this.set('square', f, a, o, duty); }
  stairs(f, a = 1, o = 1.65, steps = 8) { return this.set('stairs', f, a, o, steps); }

  dc(volts) { return this.link.request(PROTO.CMD.AWG_DC, P.bytes(P.u16(Math.round(volts * 1000)))); }
  step(fromV, toV, ms) {
    return this.link.request(PROTO.CMD.AWG_STEP,
      P.bytes(P.u16(Math.round(fromV * 1000)), P.u16(Math.round(toV * 1000)), P.u16(ms)));
  }

  /* samples: array of -1..1, up to 4096; played freqHz periods/s. */
  async arbitrary(samples, freqHz, ampV = 1.65, offsetV = 1.65) {
    const data = Uint8Array.from(samples, (s) => Math.max(0, Math.min(255, Math.round((s + 1) * 127.5))));
    for (let off = 0; off < data.length; off += 4096) {
      await this.link.request(PROTO.CMD.AWG_ARB_LOAD, P.bytes(P.u16(off), Array.from(data.slice(off, off + 4096))));
    }
    return this.set('arb', freqHz, ampV, offsetV);
  }

  off() { return this.link.request(PROTO.CMD.AWG_OFF); }

  async status() {
    const r = await this.link.request(PROTO.CMD.AWG_STATUS);
    const names = ['sine', 'tri', 'saw', 'square', 'stairs', 'arb', 'dc', 'step', 'off'];
    return {
      running: !!r[0], shape: names[r[1]] || r[1], freqHz: P.rd32(r, 2) / 1000, ampV: P.rd16(r, 6) / 1000,
      offsetV: P.rd16(r, 8) / 1000,
    };
  }
}

/* PWM outputs on the DIO header. Pairs 7/6, 5/4, 3/2, 1/0 share one
 * hardware slice, so the two pins of a pair always run at the same
 * frequency (and phase); each has its own duty and polarity. */
class Pwm {
  constructor(link) { this.link = link; }

  static slice(dio) { return (7 - dio) >> 1; }
  static partner(dio) { return dio ^ 1; }

  /* duty in %, 0..100 with 0.01 resolution. */
  async set(dio, freqHz, duty, invert = false) {
    const r = await this.link.request(PROTO.CMD.PWM_SET,
      P.bytes(P.u8(dio), P.u32(Math.round(freqHz)), P.u16(Math.round(duty * 100)), P.u8(invert ? 1 : 0)));
    return { freqHz: P.rd32(r, 0) / 1000, level: P.rd16(r, 4), top: P.rd16(r, 6), div: r[8] };
  }

  /* Phase delay of the pin's pair, 0..359 degrees; applied by sync(). */
  phase(dio, degrees) { return this.link.request(PROTO.CMD.PWM_PHASE, P.bytes(P.u8(dio), P.u16(degrees))); }

  /* Restarts the pairs behind dioMask (0 = all live) on the same clock edge. */
  sync(dioMask = 0) { return this.link.request(PROTO.CMD.PWM_SYNC, P.bytes(P.u8(dioMask))); }

  stop(dioMask = 0xFF) { return this.link.request(PROTO.CMD.PWM_STOP, P.bytes(P.u8(dioMask))); }

  /* -> [DIO0..DIO7] of { active, lost, invert, freqHz (requested), actualHz, duty (%), phase } */
  async status() {
    const r = await this.link.request(PROTO.CMD.PWM_STATUS);
    const out = [];
    for (let i = 0; i < 8; i++) {
      const o = i * 13;
      out.push({
        active: !!(r[o] & 1), lost: !!(r[o] & 2), invert: !!(r[o] & 4), freqHz: P.rd32(r, o + 1),
        actualHz: P.rd32(r, o + 5) / 1000, duty: P.rd16(r, o + 9) / 100, phase: P.rd16(r, o + 11),
      });
    }
    return out;
  }
}

class Logic {
  constructor(link) { this.link = link; }

  async arm({ n = 16384, rateHz = SYSCLK_HZ, trigger = 'none', pin = 0, mask = 0, value = 0, pre = 0,
             timeoutMs = 5000 } = {}) {
    const clkdiv = Math.max(1, Math.min(65535, Math.round(SYSCLK_HZ / rateHz)));
    const trig = { none: 0, rising: 1, falling: 2, pattern: 3 }[trigger];
    if (trigger === 'pattern' && clkdiv < 10) throw new Error('pattern trigger needs rate <= 15 MS/s');
    if ((trigger === 'rising' || trigger === 'falling') && pre > 0 && clkdiv < 10) {
      throw new Error('pre-trigger on an edge needs rate <= 15 MS/s (or pre-trigger 0)');
    }
    const r = await this.link.request(PROTO.CMD.LOGIC_ARM,
      P.bytes(P.u32(n), P.u16(clkdiv), P.u8(trig), P.u8(pin), P.u8(mask), P.u8(value), P.u32(pre), P.u16(timeoutMs)));
    return P.rd32(r, 0);
  }

  async status() {
    const r = await this.link.request(PROTO.CMD.LOGIC_STATUS);
    return { state: PROTO.CAP[r[0]], n: P.rd32(r, 1), rateHz: P.rd32(r, 5), triggerIndex: P.rd32(r, 9) };
  }

  abort() { return this.link.request(PROTO.CMD.LOGIC_ABORT); }

  async wait(timeoutMs = 10000, onTick = null) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const st = await this.status();
      if (onTick) onTick(st);
      if (st.state !== 'armed' && st.state !== 'running') return st;
      if (Date.now() > deadline) throw new Error('logic: capture did not finish');
      await new Promise((r) => setTimeout(r, 20));
    }
  }

  async read() {
    const st = await this.status();
    if (st.state !== 'done') throw new Error(`logic: no capture (${st.state})`);
    const out = new Uint8Array(st.n);
    let got = 0;
    while (got < st.n) {
      const k = Math.min(4096, st.n - got);
      const r = await this.link.request(PROTO.CMD.LOGIC_READ, P.bytes(P.u32(got), P.u16(k)));
      out.set(r, got); got += r.length;
    }
    return new LogicCapture(out, st.rateHz, st.triggerIndex);
  }

  async capture(opts, onTick) {
    await this.arm(opts);
    const st = await this.wait((opts && opts.timeoutMs || 5000) + 5000, onTick);
    if (st.state !== 'done') throw new Error(`logic: ${st.state}`);
    return this.read();
  }
}

class Uart {
  constructor(link) { this.link = link; }

  config(mode, baud = 0) {
    const m = { auto: 0, on: 1, off: 2 }[mode];
    return this.link.request(PROTO.CMD.UART_CONF, P.bytes(P.u8(m), P.u32(baud)));
  }

  async status() {
    const r = await this.link.request(PROTO.CMD.UART_STATUS);
    const st = { active: !!r[0], baud: P.rd32(r, 1), rxBytes: P.rd32(r, 5), txBytes: P.rd32(r, 9) };
    if (r.length >= 18) {   /* firmware 1.1: control lines */
      const pin = (d) => (d === 0xFF ? null : d);
      Object.assign(st, {
        mode: ['auto', 'on', 'off'][r[13]], ctrl: ['off', 'direct', 'esp'][r[14]],
        resetDio: pin(r[15]), bootDio: pin(r[16]), dtr: !!(r[17] & 1), rts: !!(r[17] & 2),
        resetLow: !!(r[17] & 4), bootLow: !!(r[17] & 8),
      });
    }
    return st;
  }

  /* RTS/DTR of the bridge port drive two DIOs (open-drain, asserted = low).
   * mode: 'esp' (ESP32 auto-reset logic), 'direct' or 'off'; pins DIO2..7 or null. */
  ctrl(mode, resetDio = null, bootDio = null) {
    const m = { off: 0, direct: 1, esp: 2 }[mode];
    const pin = (d) => (d === null || d === undefined ? 0xFF : d);
    return this.link.request(PROTO.CMD.UART_CTRL, P.bytes(P.u8(m), P.u8(pin(resetDio)), P.u8(pin(bootDio))));
  }
}

/* The board as an I2C or SPI device on the target's bus, from a 256-byte
 * register file (auto-incrementing pointer). Mirrors t.emu in Python. */
class Emu {
  constructor(link) { this.link = link; }

  i2c(address, scl = 6, sda = 7, pullups = false) {
    return this.link.request(PROTO.CMD.EMU_I2C, P.bytes(P.u8(1), P.u8(address), P.u8(scl), P.u8(sda), P.u8(pullups ? 1 : 0)));
  }

  spi({ mode = 0, cs = 2, sck = 5, mosi = 4, miso = 3, addrMask = 0x7F, readMask = 0x80, readValue = 0x80, status = 0 } = {}) {
    return this.link.request(PROTO.CMD.EMU_SPI, P.bytes(P.u8(1), P.u8(mode), P.u8(cs), P.u8(sck), P.u8(mosi), P.u8(miso),
      P.u8(addrMask), P.u8(readMask), P.u8(readValue), P.u8(status)));
  }

  stop() { return this.link.request(PROTO.CMD.EMU_I2C, P.bytes(P.u8(0))); }

  setRegs(offset, bytes) {
    return this.link.request(PROTO.CMD.EMU_REGS, P.bytes(P.u8(0), P.u8(offset), Array.from(bytes)));
  }

  regs(offset = 0, n = 256) {
    return this.link.request(PROTO.CMD.EMU_REGS, P.bytes(P.u8(1), P.u8(offset), P.u16(n)));
  }

  readOnly(registers) {
    const mask = new Array(32).fill(0);
    for (const r of registers) mask[r >> 3] |= 1 << (r & 7);
    return this.link.request(PROTO.CMD.EMU_REGS, P.bytes(P.u8(2), mask));
  }

  async status() {
    const r = await this.link.request(PROTO.CMD.EMU_STATUS);
    return { bus: ['off', 'i2c', 'spi'][r[0]], transfers: P.rd32(r, 1), bytesRead: P.rd32(r, 5),
             bytesWritten: P.rd32(r, 9), naks: P.rd32(r, 13) };
  }
}

/* --- sequencer -------------------------------------------------------------------- */

const SEQ_OPS = {
  delay_us: 0x01, power: 0x02, pullups: 0x03, dio_dir: 0x04, dio_write: 0x05, dio_read: 0x06,
  wait_dio: 0x07, i2c_write: 0x08, i2c_read: 0x09, i2c_write_read: 0x0A, spi_txn: 0x0B,
  uart_send: 0x0C, awg_dc: 0x0D, adc_read: 0x0E, mark: 0x0F, loop: 0x10, end_loop: 0x11,
  logic_arm: 0x12, scope_arm: 0x13, watch: 0x14,
};
const SEQ_OP_NAMES = Object.fromEntries(Object.entries(SEQ_OPS).map(([k, v]) => [v, k]));
const SEQ_CONTINUE = 0x80;

/* A program for the board's sequencer (fw src/seq.h), built step by step
 * like host/python/tramuntana/seq.py, or parsed from text (Sequence.parse). */
class Sequence {
  constructor() { this.code = []; this.labels = new Map(); }

  add(op, args, label, keepGoing = false) {
    this.labels.set(this.code.length, label);
    this.code.push(SEQ_OPS[op] | (keepGoing ? SEQ_CONTINUE : 0), ...args);
    if (this.code.length > 2048) throw new Error('program longer than 2048 bytes');
    return this;
  }

  delayUs(us) { return this.add('delay_us', P.u32(Math.round(us)), `delay ${us} us`); }
  delayMs(ms) { return this.add('delay_us', P.u32(Math.round(ms * 1000)), `delay ${ms} ms`); }
  mark(id) { return this.add('mark', P.u8(id), `mark ${id}`); }
  loop(count) { return this.add('loop', P.u16(Math.max(1, count)), `loop x${count}`); }
  endLoop() { return this.add('end_loop', [], 'end loop'); }
  power(on) { return this.add('power', P.u8(on ? 1 : 0), `power ${on ? 'on' : 'off'}`); }
  pullups(on) { return this.add('pullups', P.u8(on ? 1 : 0), `pullups ${on ? 'on' : 'off'}`); }
  dioDir(mask, outMask) { return this.add('dio_dir', [mask, outMask], `dio_dir ${hex8(mask)} out ${hex8(outMask)}`); }
  dioWrite(mask, value, kg = false) { return this.add('dio_write', [mask, value], `dio_write ${hex8(mask)} = ${hex8(value)}`, kg); }
  dioRead() { return this.add('dio_read', [], 'dio_read'); }
  waitDio(mask, value, timeoutUs = 1000000, kg = false) {
    return this.add('wait_dio', [mask, value, ...P.u32(timeoutUs)], `wait_dio ${hex8(mask)} == ${hex8(value & mask)}`, kg);
  }
  watch(mask) { return this.add('watch', P.u8(mask), `watch ${hex8(mask)}`); }
  awgDc(volts) { return this.add('awg_dc', P.u16(Math.round(volts * 1000)), `awg_dc ${volts} V`); }
  adcRead(ch = 0) { return this.add('adc_read', P.u8(ch), `adc_read AIN${ch}`); }
  i2cWrite(addr, data, kg = false) {
    return this.add('i2c_write', [addr, data.length, ...data], `i2c_write ${hex8(addr)} [${hex(data)}]`, kg);
  }
  i2cRead(addr, n, kg = false) { return this.add('i2c_read', [addr, n], `i2c_read ${hex8(addr)} x${n}`, kg); }
  i2cWriteRead(addr, w, n, kg = false) {
    return this.add('i2c_write_read', [addr, w.length, ...w, n], `i2c_write_read ${hex8(addr)} [${hex(w)}] x${n}`, kg);
  }
  spiTxn(w, readN = 0, kg = false) { return this.add('spi_txn', [w.length, ...w, readN], `spi_txn [${hex(w)}] x${readN}`, kg); }
  uartSend(bytes, kg = false) { return this.add('uart_send', [bytes.length, ...bytes], `uart_send ${bytes.length} B`, kg); }

  /* One step per line, '#' comments, a trailing '?' = keep going on failure:
   *   delay_us 100 | delay_ms 20 | power on|off | pullups on|off | mark 1
   *   dio_dir <pin> out|in        dio_write <pin> 0|1      dio_read
   *   wait_dio <pin> 0|1 [timeout_us]                     watch <mask>
   *   i2c_write <addr> <hex..>    i2c_read <addr> <n>      i2c_write_read <addr> <hex..> read <n>
   *   spi_txn <hex..> [read <n>]  uart_send <text>         awg_dc <volts>     adc_read <ch>
   *   loop <count> ... end_loop
   * Addresses and bytes are hex; pins, counts and times decimal. */
  static parse(text) {
    const s = new Sequence();
    const lines = text.split('\n');
    lines.forEach((raw, i) => {
      let line = raw.replace(/#.*$/, '').trim();
      if (!line) return;
      let kg = false;
      if (line.endsWith('?')) { kg = true; line = line.slice(0, -1).trim(); }
      const [op, ...a] = line.split(/\s+/);
      const where = `line ${i + 1}: `;
      const int = (v) => { const n = parseInt(v, 10); if (!isFinite(n)) throw new Error(`${where}"${v}" is not a number`); return n; };
      const hx = (v) => { const n = parseInt(v, 16); if (!isFinite(n)) throw new Error(`${where}"${v}" is not hex`); return n; };
      const bytes = (arr) => arr.map(hx);
      const pin = (v) => { const p = int(v); if (p < 0 || p > 7) throw new Error(`${where}pin 0..7`); return 1 << p; };
      const onOff = (v) => /^(on|1|true)$/i.test(v);
      switch (op) {
        case 'delay_us': s.delayUs(int(a[0])); break;
        case 'delay_ms': s.delayMs(parseFloat(a[0])); break;
        case 'power': s.power(onOff(a[0])); break;
        case 'pullups': s.pullups(onOff(a[0])); break;
        case 'mark': s.mark(int(a[0])); break;
        case 'dio_dir': s.dioDir(pin(a[0]), /^out/i.test(a[1]) ? pin(a[0]) : 0); break;
        case 'dio_write': s.dioWrite(pin(a[0]), int(a[1]) ? pin(a[0]) : 0, kg); break;
        case 'dio_read': s.dioRead(); break;
        case 'wait_dio': s.waitDio(pin(a[0]), int(a[1]) ? pin(a[0]) : 0, a[2] ? int(a[2]) : 1000000, kg); break;
        case 'watch': s.watch(hx(a[0])); break;
        case 'i2c_write': s.i2cWrite(hx(a[0]), bytes(a.slice(1)), kg); break;
        case 'i2c_read': s.i2cRead(hx(a[0]), int(a[1]), kg); break;
        case 'i2c_write_read': {
          const r = a.indexOf('read');
          if (r < 0) throw new Error(`${where}i2c_write_read <addr> <hex..> read <n>`);
          s.i2cWriteRead(hx(a[0]), bytes(a.slice(1, r)), int(a[r + 1]), kg);
          break;
        }
        case 'spi_txn': {
          const r = a.indexOf('read');
          s.spiTxn(bytes(r < 0 ? a : a.slice(0, r)), r < 0 ? 0 : int(a[r + 1]), kg);
          break;
        }
        case 'uart_send': {
          const textArg = raw.replace(/#.*$/, '').trim().replace(/\?$/, '').replace(/^uart_send\s+/, '')
            .replace(/\\r/g, '\r').replace(/\\n/g, '\n');
          s.uartSend(Array.from(new TextEncoder().encode(textArg)), kg);
          break;
        }
        case 'awg_dc': s.awgDc(parseFloat(a[0])); break;
        case 'adc_read': s.adcRead(a[0] ? int(a[0]) : 0); break;
        case 'loop': s.loop(int(a[0])); break;
        case 'end_loop': s.endLoop(); break;
        default: throw new Error(`${where}unknown step "${op}"`);
      }
    });
    if (!s.code.length) throw new Error('empty program');
    return s;
  }
}

class Sequencer {
  constructor(link) { this.link = link; }

  async start(seq, timeoutMs = 10000) {
    const code = Uint8Array.from(seq.code);
    for (let off = 0; off < code.length; off += 1024) {
      await this.link.request(PROTO.CMD.SEQ_LOAD, P.bytes(P.u16(off), Array.from(code.subarray(off, off + 1024))));
    }
    await this.link.request(PROTO.CMD.SEQ_RUN, P.bytes(P.u16(code.length), P.u32(timeoutMs)));
  }

  async status() {
    const r = await this.link.request(PROTO.CMD.SEQ_STATUS);
    return { state: PROTO.SEQ_STATES[r[0]] || String(r[0]), error: r[1] ? PROTO.STATUS[r[1]] : null,
             pc: P.rd16(r, 2), resultLen: P.rd16(r, 4) };
  }

  abort() { return this.link.request(PROTO.CMD.SEQ_ABORT); }

  /* -> { state, error, pc, steps: [{ pc, op, label, ok, status, tUs, dtUs, data, value }] } */
  async result(seq = null) {
    const st = await this.status();
    const raw = [];
    while (raw.length < st.resultLen) {
      const chunk = await this.link.request(PROTO.CMD.SEQ_RESULT, P.bytes(P.u16(raw.length), P.u16(2048)));
      if (!chunk.length) break;
      raw.push(...chunk);
    }
    const b = Uint8Array.from(raw);
    const steps = [];
    let t0 = null;
    for (let i = 0; i + 9 <= b.length;) {
      const pc = P.rd16(b, i), op = b[i + 2], status = b[i + 3], tUs = P.rd32(b, i + 4), n = b[i + 8];
      const data = b.slice(i + 9, i + 9 + n);
      i += 9 + n;
      if (t0 === null) t0 = tUs;
      const name = SEQ_OP_NAMES[op & ~SEQ_CONTINUE] || `op${op}`;
      steps.push({ pc, op: name, label: (seq && seq.labels.get(pc)) || name, ok: status === 0,
                   status: status === 0 ? 'ok' : (PROTO.STATUS[status] || String(status)),
                   tUs, dtUs: (tUs - t0) >>> 0, data, value: seqValue(name, data) });
    }
    return { ...st, steps };
  }

  async run(seq, timeoutMs = 10000) {
    await this.start(seq, timeoutMs);
    const deadline = Date.now() + timeoutMs + 2000;
    for (;;) {
      const st = await this.status();
      if (st.state !== 'running') break;
      if (Date.now() > deadline) { await this.abort(); throw new Error('sequence did not finish'); }
      await new Promise((r) => setTimeout(r, 10));
    }
    return this.result(seq);
  }
}

function seqValue(op, d) {
  if (!d.length) return null;
  if (op === 'dio_read') return `0b${d[0].toString(2).padStart(8, '0')}`;
  if (op === 'mark') return d[0];
  if (op === 'wait_dio') return `${P.rd32(d, 0)} us`;
  if (op === 'adc_read') return `${(P.rd16(d, 0) / 1000).toFixed(3)} V`;
  if (op === 'i2c_write') return `${d[0]} ACKed`;
  if (op === 'logic_arm' || op === 'scope_arm') return `${P.rd32(d, 0)} S/s`;
  return hex(d);
}

function hex8(v) { return `0x${v.toString(16).padStart(2, '0')}`; }

/* One line for an emulator transfer event (NAKed address bytes too). */
function formatEmu(ev) {
  const h = (v) => v.toString(16).padStart(2, '0').toUpperCase();
  if (ev.nak) return `I2C NAK address ${h(ev.address)} (byte ${h(ev.reg)}${ev.noStart ? ', no START before it' : ''})`;
  const who = ev.bus === 'i2c' ? `I2C ${h(ev.address)}` : 'SPI';
  return `${who} ${ev.read ? 'R' : 'W'} reg ${h(ev.reg)}: ${hex(ev.data)}${ev.truncated ? ' ..' : ''}`;
}

/* --- the device ----------------------------------------------------------- */

const LED_MAP = {
  off: [0, 0, 0], red: [1, 0, 0], green: [0, 1, 0], blue: [0, 0, 1], yellow: [1, 1, 0],
  cyan: [0, 1, 1], magenta: [1, 0, 1], white: [1, 1, 1],
};
const PIN_OWNERS = ['gpio', 'spi', 'i2c', 'uart', 'pio', 'pwm', 'emu', '?'];

class Tramuntana {
  constructor(link) {
    this.link = link;
    this.spi = new Spi(link);
    this.i2c = new I2c(link);
    this.scope = new Scope(link);
    this.awg = new Awg(link);
    this.pwm = new Pwm(link);
    this.logic = new Logic(link);
    this.uart = new Uart(link);
    this.emu = new Emu(link);
    this.seq = new Sequencer(link);
    this._listeners = new Set();
    link.onEvent = (evt, payload) => this._dispatch(evt, payload);
  }

  static async connect() {
    if (!('serial' in navigator)) throw new Error('This browser has no Web Serial API. Use Chrome, Edge or Opera on desktop.');
    const port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: PROTO.USB_VID, usbProductId: PROTO.USB_PID }],
    });
    const link = new Link();
    await link.open(port);
    return new Tramuntana(link);
  }

  async close() { await this.link.close(); }
  get connected() { return this.link.connected; }

  onEvent(cb) { this._listeners.add(cb); return () => this._listeners.delete(cb); }

  _dispatch(evt, payload) {
    let decoded = null;
    if (evt === PROTO.EVT.SPI_MSG) decoded = decodeSpiEvent(payload);
    else if (evt === PROTO.EVT.I2C_MSG) decoded = decodeI2cEvent(payload);
    else if (evt === PROTO.EVT.DIO) decoded = decodeDioEvent(payload);
    else if (evt === PROTO.EVT.EMU) decoded = decodeEmuEvent(payload);
    else if (evt === PROTO.EVT.LINE) decoded = decodeLineEvent(payload);
    else if (evt === PROTO.EVT.SEQ_DONE) decoded = decodeSeqDoneEvent(payload);
    else if (evt === PROTO.EVT.CAPTURE_DONE) {
      decoded = { type: 'capture_done', instrument: ['scope', 'logic'][payload[0]], state: PROTO.CAP[payload[1]] };
    } else if (evt === PROTO.EVT.LOG) {
      decoded = { type: 'log', text: new TextDecoder().decode(payload) };
    } else {
      decoded = { type: 'unknown', evt, payload };
    }
    for (const cb of this._listeners) { try { cb(decoded); } catch (e) { console.error(e); } }
  }

  async info() {
    const r = await this.link.request(PROTO.CMD.INFO);
    const dec = new TextDecoder();
    const cstr = (b) => dec.decode(b).replace(/\0.*$/, '');
    return {
      fwVersion: cstr(r.slice(0, 16)), board: cstr(r.slice(16, 32)), sysclkHz: P.rd32(r, 32),
      chipId: hex(r.slice(36, 44), ''), features: P.rd32(r, 44), protoVersion: this.link.protoVersion,
    };
  }

  async bootloader() { try { await this.link.request(PROTO.CMD.BOOTLOADER, new Uint8Array(0), 1000); } catch (e) { /* port vanishes */ } }

  led(colour) {
    if (colour === null || colour === 'auto') return this.link.request(PROTO.CMD.LED, P.bytes(P.u8(0xFF), P.u8(0), P.u8(0)));
    const [r, g, b] = LED_MAP[colour] || [0, 0, 0];
    return this.link.request(PROTO.CMD.LED, P.bytes(P.u8(r), P.u8(g), P.u8(b)));
  }

  async power(on = null) {
    const r = await this.link.request(PROTO.CMD.POWER, on === null ? new Uint8Array(0) : P.bytes(P.u8(on ? 1 : 0)));
    return !!r[0];
  }

  async pullups(on = null) {
    const r = await this.link.request(PROTO.CMD.PULLUPS, on === null ? new Uint8Array(0) : P.bytes(P.u8(on ? 1 : 0)));
    return !!r[0];
  }

  dioDir(mask, outMask) { return this.link.request(PROTO.CMD.DIO_DIR, P.bytes(P.u8(mask), P.u8(outMask))); }
  dioWrite(mask, value) { return this.link.request(PROTO.CMD.DIO_WRITE, P.bytes(P.u8(mask), P.u8(value))); }
  async dioRead() { return (await this.link.request(PROTO.CMD.DIO_READ))[0]; }

  /* Report every edge on these DIOs as 'dio' events (board timestamps); 0 stops. */
  dioWatch(mask) { return this.link.request(PROTO.CMD.DIO_WATCH, P.bytes(P.u8(mask))); }

  /* The board clock, microseconds since boot (32 bits). */
  async timeUs() { return P.rd32(await this.link.request(PROTO.CMD.TIME), 0); }

  /* Sniffer events carry the board time of their START / CS edge too. */
  eventStamps(on) { return this.link.request(PROTO.CMD.TIME, P.bytes(P.u8(on ? 1 : 0))); }

  async adc(channel = 0) {
    const r = await this.link.request(PROTO.CMD.ADC_READ, P.bytes(P.u8(channel)));
    return P.rd16(r, 0) / 1000;
  }

  display(text) { return this.link.request(PROTO.CMD.DISPLAY_TEXT, new TextEncoder().encode(text.slice(0, 24))); }

  /* J1 pinout on the board's screen: seconds, null = hold, 0 = back to the dashboard. */
  displayPinout(seconds = 60) {
    const s = seconds === null ? 0xFFFF : Math.max(0, Math.min(0xFFFE, seconds | 0));
    return this.link.request(PROTO.CMD.DISPLAY_PINOUT, P.bytes(P.u16(s)));
  }

  async status() {
    const r = await this.link.request(PROTO.CMD.STATUS);
    const pins32 = P.rd32(r, 0);
    const pins = [];
    for (let i = 0; i < 8; i++) {
      const nib = (pins32 >> (4 * i)) & 0xF;
      pins.push(PIN_OWNERS[nib & 7] + (nib & 8 ? '!' : ''));
    }
    return {
      pins, power: !!r[4], pullups: !!r[5], scope: PROTO.CAP[r[6]], logic: PROTO.CAP[r[7]], awg: !!r[8],
      spiSniff: !!r[9], i2cSniff: !!r[10], uartBridge: !!r[11], rxDropped: r.length >= 16 ? P.rd32(r, 12) : 0,
    };
  }
}

Object.assign(T, {
  Tramuntana, ScopeCapture, LogicCapture, PROTO, decodeSpiEvent, decodeI2cEvent, formatI2c, hex, VREF_MV, SYSCLK_HZ,
  Sequence, formatEmu,
});
