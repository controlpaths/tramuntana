/*
 * Timeline - the browser twin of host/python/tramuntana/timeline.py: every
 * event the board sends (DIO edges, emulator transfers, control lines,
 * sequencer runs, sniffed messages, captures) plus the target's UART
 * lines from the terminal, on one clock, in seconds since start().
 *
 * Board-stamped events keep the board's microsecond clock, mapped onto
 * performance.now() with a CMD_TIME sync every 5 s (the fastest of three
 * round trips). UART lines and host entries carry the time they arrived
 * or happened.
 */
'use strict';

window.T = window.T || {};

class Timeline {
  constructor(t) {
    this.t = t;
    this.entries = [];
    this.running = false;
    this.t0 = 0;
    this.dioNames = {};
    this.keep = 20000;
    this.onChange = null;       /* () => void, after entries change */
    this._ref = null;           /* { host (ms), board (us) } */
    this._unsub = null;
    this._timers = [];
    this._uartLine = '';
    this._uartLineT = null;
    this._uartLast = 0;
  }

  now() { return (performance.now() - this.t0) / 1000; }

  async start(watchMask = 0) {
    await this.t.eventStamps(true);
    await this._sync();
    this.entries = [];
    this.t0 = performance.now();
    this.running = true;
    await this.t.dioWatch(watchMask);
    this._unsub = this.t.onEvent((ev) => this._onEvent(ev));
    this._timers.push(setInterval(() => this._sync().catch(() => {}), 5000));
    this._timers.push(setInterval(() => {
      if (this._uartLine && this.now() - this._uartLast > 0.05) this._emitUartLine();
    }, 50));
    this._changed();
  }

  async watch(mask) { await this.t.dioWatch(mask); }

  async stop() {
    if (!this.running) return;
    this.running = false;
    this._timers.forEach(clearInterval);
    this._timers = [];
    if (this._unsub) this._unsub();
    this._unsub = null;
    if (this._uartLine) this._emitUartLine();
    try { await this.t.dioWatch(0); await this.t.eventStamps(false); } catch (e) { /* board gone */ }
    this._changed();
  }

  async _sync() {
    let best = null;
    for (let i = 0; i < 3; i++) {
      const a = performance.now();
      const b = await this.t.timeUs();
      const c = performance.now();
      if (!best || c - a < best.rtt) best = { rtt: c - a, host: (a + c) / 2, board: b };
    }
    this._ref = best;
  }

  /* Board microseconds -> seconds since start(). */
  boardT(us) {
    if (!this._ref) return this.now();
    let d = (us - this._ref.board) >>> 0;
    if (d >= 0x80000000) d -= 0x100000000;
    return (this._ref.host + d / 1000 - this.t0) / 1000;
  }

  _name(dio) { return this.dioNames[dio] || `DIO${dio}`; }

  _onEvent(ev) {
    if (!this.running) return;
    const stamped = (tUs) => (tUs === null || tUs === undefined ? this.now() : this.boardT(tUs));
    switch (ev.type) {
      case 'dio': {
        const parts = [];
        for (let i = 0; i < 8; i++) {
          if ((ev.changed >> i) & 1) parts.push(`${this._name(i)} ${(ev.levels >> i) & 1 ? '^' : 'v'}`);
        }
        const text = `${parts.join(', ') || 'glitch'}  (levels ${ev.levels.toString(2).padStart(8, '0')})` +
          (ev.lost ? ` ${ev.lost} lost` : '');
        this._add(this.boardT(ev.tUs), 'dio', text);
        break;
      }
      case 'emu':
        this._add(this.boardT(ev.tUs), 'emu', T.formatEmu(ev));
        break;
      case 'line':
        this._add(this.boardT(ev.tUs), 'line', `DTR=${ev.dtr ? 1 : 0} RTS=${ev.rts ? 1 : 0}`);
        break;
      case 'seq_done':
        this._add(this.boardT(ev.tUs), 'seq', `SEQ ${ev.state}` + (ev.state !== 'done' ? ` at pc ${ev.pc}` : ''));
        break;
      case 'i2c':
        this._add(stamped(ev.tUs), 'i2c', T.formatI2c(ev));
        break;
      case 'spi': {
        const more = ev.total > ev.mosi.length ? ' ..' : '';
        this._add(stamped(ev.tUs), 'spi', `${ev.total}B  MOSI ${T.hex(ev.mosi)}${more}  MISO ${T.hex(ev.miso)}${more}`);
        break;
      }
      case 'capture_done':
        this._add(this.now(), 'capture', `${ev.instrument} ${ev.state}`);
        break;
      default:
        break;
    }
  }

  /* Text received from the target's UART (the UART tab's terminal). */
  feedUart(text) {
    if (!this.running) return;
    const now = this.now();
    for (const ch of text) {
      if (this._uartLineT === null) this._uartLineT = now;
      if (ch === '\n') this._emitUartLine();
      else if (ch !== '\r') this._uartLine += ch;
    }
    this._uartLast = now;
  }

  _emitUartLine() {
    this._add(this._uartLineT === null ? this.now() : this._uartLineT, 'uart', this._uartLine);
    this._uartLine = '';
    this._uartLineT = null;
  }

  note(text, source = 'host') { if (this.running) this._add(this.now(), source, text); }
  mark(label) { this.note(label, 'mark'); }

  _add(t, source, text) {
    const e = { t, source, text };
    let i = this.entries.length;
    while (i > 0 && this.entries[i - 1].t > t) i--;   /* board stamps can be a bit older */
    this.entries.splice(i, 0, e);
    if (this.entries.length > this.keep) this.entries.splice(0, this.entries.length - this.keep);
    this._changed();
  }

  _changed() { if (this.onChange) { try { this.onChange(); } catch (e) { console.error(e); } } }

  select({ sources = null, contains = '' } = {}) {
    const needle = contains.toLowerCase();
    return this.entries.filter((e) => (!sources || sources.has(e.source)) &&
      (!needle || e.text.toLowerCase().includes(needle)));
  }

  static format(e) { return `${e.t.toFixed(6).padStart(11)}  ${e.source.padEnd(7)} ${e.text}`; }
}

T.Timeline = Timeline;
