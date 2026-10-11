/*
 * Logic analyzer canvas: 8 lanes (DIO0..DIO7), drawn min/max-per-column
 * like the firmware's own dashboard (T.LogicCapture can hold far more
 * samples than there are pixels), trigger marker, hover cursor with the
 * bit pattern at that sample.
 *
 * Horizontal zoom: the view is a window [v0, v0 + span) of sample indices.
 * Mouse wheel zooms around the pointer, dragging pans, double-click fits
 * the whole capture; zoomIn()/zoomOut()/fit() back the toolbar buttons.
 */
'use strict';

window.T = window.T || {};

T.LOGIC_COLORS = ['#40f0e0', '#c86eff', '#ffb547', '#4dff9a', '#4da3ff', '#ff4d6d', '#ffe066', '#9d8bff'];

T.LogicPlot = class {
  constructor(canvas, cursorEl, viewEl) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.cursorEl = cursorEl;
    this.viewEl = viewEl || null;
    this.capture = null;
    this.margin = { l: 54, r: 16, t: 14, b: 26 };
    this.v0 = 0;
    this.span = 1;
    this.minSpan = 8;
    this.drag = null;
    new ResizeObserver(() => this.draw()).observe(canvas);
    canvas.addEventListener('mousemove', (e) => this._move(e));
    canvas.addEventListener('mouseleave', () => { this.cursorEl.hidden = true; this._endDrag(); });
    canvas.addEventListener('mousedown', (e) => this._startDrag(e));
    window.addEventListener('mouseup', () => this._endDrag());
    canvas.addEventListener('wheel', (e) => this._wheel(e), { passive: false });
    canvas.addEventListener('dblclick', () => this.fit());
  }

  setCapture(capture) {
    this.capture = capture;
    this.v0 = 0;
    this.span = Math.max(1, capture ? capture.n : 1);
    this.draw();
  }

  /* --- view window ----------------------------------------------------------- */

  _clampView() {
    const n = this.capture ? this.capture.n : 1;
    this.span = Math.min(n, Math.max(Math.min(this.minSpan, n), this.span));
    this.v0 = Math.min(n - this.span, Math.max(0, this.v0));
  }

  /* Zoom by `factor` (> 1 = in) keeping sample `at` under the same pixel. */
  zoom(factor, at) {
    if (!this.capture) return;
    if (at === undefined) at = this.v0 + this.span / 2;
    const frac = (at - this.v0) / this.span;
    this.span /= factor;
    this._clampView();
    this.v0 = at - frac * this.span;
    this._clampView();
    this.draw();
  }

  zoomIn() { this.zoom(2); }
  zoomOut() { this.zoom(0.5); }

  fit() {
    if (!this.capture) return;
    this.v0 = 0;
    this.span = this.capture.n;
    this.draw();
  }

  _sampleAt(px) {
    const a = this._area();
    return this.v0 + ((px - a.x) / a.w) * this.span;
  }

  _wheel(e) {
    if (!this.capture) return;
    e.preventDefault();
    const rect = this.canvas.getBoundingClientRect();
    const a = this._area();
    if (Math.abs(e.deltaX) > Math.abs(e.deltaY) || e.shiftKey) {
      /* horizontal wheel / shift+wheel pans */
      const d = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.deltaY;
      this.v0 += (d / a.w) * this.span;
      this._clampView();
      this.draw();
      return;
    }
    const px = Math.max(a.x, Math.min(a.x + a.w, e.clientX - rect.left));
    this.zoom(Math.pow(1.0015, -e.deltaY), this._sampleAt(px));
  }

  _startDrag(e) {
    if (!this.capture || e.button !== 0) return;
    this.drag = { x: e.clientX, v0: this.v0 };
    this.canvas.style.cursor = 'grabbing';
    e.preventDefault();
  }

  _endDrag() {
    this.drag = null;
    this.canvas.style.cursor = '';
  }

  _move(e) {
    if (this.drag) {
      const a = this._area();
      this.v0 = this.drag.v0 - ((e.clientX - this.drag.x) / a.w) * this.span;
      this._clampView();
      this.draw();
    }
    this._hover(e);
  }

  /* --- drawing ------------------------------------------------------------------ */

  _area() {
    const w = this.canvas.clientWidth, h = this.canvas.clientHeight;
    const m = this.margin;
    return { x: m.l, y: m.t, w: w - m.l - m.r, h: h - m.t - m.b };
  }

  draw() {
    const dpr = window.devicePixelRatio || 1;
    const cw = this.canvas.clientWidth, ch = this.canvas.clientHeight;
    if (cw === 0 || ch === 0) return;
    if (this.canvas.width !== cw * dpr || this.canvas.height !== ch * dpr) {
      this.canvas.width = cw * dpr; this.canvas.height = ch * dpr;
    }
    const ctx = this.ctx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cw, ch);
    const a = this._area();
    ctx.fillStyle = 'rgba(0, 0, 0, 0.35)';
    ctx.fillRect(a.x, a.y, a.w, a.h);
    ctx.strokeStyle = '#40f0e0'; ctx.globalAlpha = 0.5;
    ctx.strokeRect(a.x + 0.5, a.y + 0.5, a.w - 1, a.h - 1);
    ctx.globalAlpha = 1;

    const laneH = a.h / 8;
    ctx.font = '10px ui-monospace, Menlo, Consolas, monospace';
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (let d = 0; d < 8; d++) {
      const y = a.y + d * laneH + laneH / 2;
      ctx.strokeStyle = '#1f2740'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(a.x, a.y + d * laneH); ctx.lineTo(a.x + a.w, a.y + d * laneH); ctx.stroke();
      ctx.fillStyle = T.LOGIC_COLORS[d];
      ctx.fillText(`DIO${d}`, a.x - 6, y);
    }

    if (!this.capture || this.capture.n === 0) { this._updateView(); return; }
    const cap = this.capture;
    const n = cap.n;
    const dt = 1 / cap.rateHz;
    const v0 = this.v0, span = this.span;
    /* Sample i covers [i, i + 1) in sample space, so x(i) is its left edge. */
    const xOf = (s) => a.x + ((s - v0) / span) * a.w;

    ctx.fillStyle = '#7d88ad'; ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    for (let i = 0; i <= 10; i++) {
      const s = v0 + (span * i) / 10;
      ctx.fillText(T.fmtTime((s - cap.triggerIndex) * dt), a.x + (a.w * i) / 10, a.y + a.h + 4);
    }

    const tx = xOf(cap.triggerIndex);
    if (tx >= a.x && tx <= a.x + a.w) {
      ctx.strokeStyle = 'rgba(255, 181, 71, 0.7)';
      ctx.setLineDash([4, 4]); ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(tx, a.y); ctx.lineTo(tx, a.y + a.h); ctx.stroke();
      ctx.setLineDash([]);
    }

    ctx.save();
    ctx.beginPath(); ctx.rect(a.x, a.y, a.w, a.h); ctx.clip();

    /* One column per pixel: for each, whether every lane was ever high
     * and ever low within it - same trick the firmware's dashboard uses,
     * so a capture far denser than the canvas still looks right. Each lane
     * is then one continuous trace: flat where the level holds, a vertical
     * stroke where it changes inside the column. */
    const cols = Math.max(1, Math.round(a.w));
    const hiM = new Uint8Array(cols), loM = new Uint8Array(cols);
    const first = new Uint8Array(cols), last = new Uint8Array(cols);
    for (let x = 0; x < cols; x++) {
      const lo = v0 + (x * span) / cols, hi = v0 + ((x + 1) * span) / cols;
      const start = Math.min(n - 1, Math.floor(lo));
      const end = Math.min(n, Math.max(start + 1, Math.ceil(hi)));
      let hiMask = 0, loMask = 0;
      for (let i = start; i < end; i++) {
        hiMask |= cap.samples[i];
        loMask |= (~cap.samples[i]) & 0xFF;
      }
      hiM[x] = hiMask; loM[x] = loMask;
      first[x] = cap.samples[start]; last[x] = cap.samples[end - 1];
    }

    ctx.lineWidth = 1.5;
    ctx.lineJoin = 'miter';
    for (let d = 0; d < 8; d++) {
      const yHi = a.y + d * laneH + laneH * 0.22;
      const yLo = a.y + d * laneH + laneH * 0.78;
      const yOf = (v) => (((v >> d) & 1) ? yHi : yLo);
      ctx.strokeStyle = T.LOGIC_COLORS[d];
      ctx.beginPath();
      for (let x = 0; x < cols; x++) {
        const px = a.x + x;
        const yFirst = yOf(first[x]), yLast = yOf(last[x]);
        if (x === 0) ctx.moveTo(px, yFirst);
        else ctx.lineTo(px, yFirst);
        if (((hiM[x] & loM[x]) >> d) & 1) {
          ctx.lineTo(px + 0.5, yFirst);
          ctx.lineTo(px + 0.5, yFirst === yHi ? yLo : yHi);
          ctx.lineTo(px + 0.5, yLast);
        }
        ctx.lineTo(px + 1, yLast);
      }
      ctx.stroke();
    }
    ctx.restore();
    this._updateView();
  }

  _updateView() {
    if (!this.viewEl) return;
    const cap = this.capture;
    if (!cap || cap.n === 0) { this.viewEl.textContent = '-'; return; }
    const dt = 1 / cap.rateHz;
    const zoom = cap.n / this.span;
    this.viewEl.textContent = `${T.fmtTime(this.span * dt)} span · x${zoom < 10 ? zoom.toFixed(1) : Math.round(zoom)}`;
  }

  _hover(e) {
    if (!this.capture || this.capture.n === 0) { this.cursorEl.hidden = true; return; }
    const rect = this.canvas.getBoundingClientRect();
    const px = e.clientX - rect.left, py = e.clientY - rect.top;
    const a = this._area();
    if (px < a.x || px > a.x + a.w || py < a.y || py > a.y + a.h) { this.cursorEl.hidden = true; return; }
    const cap = this.capture;
    const i = Math.max(0, Math.min(cap.n - 1, Math.floor(this._sampleAt(px))));
    const dt = 1 / cap.rateHz;
    const bits = cap.samples[i].toString(2).padStart(8, '0'); /* MSB first = DIO7..DIO0 */
    this.cursorEl.textContent = `t = ${T.fmtTime((i - cap.triggerIndex) * dt)}  [${i}]\nDIO7..0 = ${bits}`;
    this.cursorEl.style.left = `${px}px`;
    this.cursorEl.style.top = `${py}px`;
    this.cursorEl.hidden = false;
  }
};
