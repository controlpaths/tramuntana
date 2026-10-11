/*
 * Oscilloscope canvas: draws a ScopeCapture (device.js) - volts vs time
 * relative to the trigger, 0..3.3 V full scale, up to 2 channels, grid,
 * trigger marker and a hover cursor readout.
 */
'use strict';

window.T = window.T || {};

T.ScopePlot = class {
  constructor(canvas, cursorEl) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.cursorEl = cursorEl;
    this.capture = null;
    this.colors = ['#40f0e0', '#c86eff'];
    this.margin = { l: 54, r: 16, t: 14, b: 30 };
    new ResizeObserver(() => this.draw()).observe(canvas);
    canvas.addEventListener('mousemove', (e) => this._hover(e));
    canvas.addEventListener('mouseleave', () => { this.cursorEl.hidden = true; });
  }

  setCapture(capture) { this.capture = capture; this.draw(); }

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
    this._grid(ctx, a);
    if (!this.capture || this.capture.n === 0) return;

    const cap = this.capture;
    const n = cap.n;
    const t0 = cap.timeAt(0), t1 = cap.timeAt(n - 1);
    const xOf = (i) => a.x + (a.w * i) / Math.max(n - 1, 1);
    const yOf = (raw) => a.y + a.h - (a.h * raw) / 4095;

    ctx.fillStyle = '#7d88ad';
    ctx.font = '10px ui-monospace, Menlo, Consolas, monospace';
    ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    for (let i = 0; i <= 10; i++) {
      const t = t0 + ((t1 - t0) * i) / 10;
      ctx.fillText(T.fmtTime(t), a.x + (a.w * i) / 10, a.y + a.h + 6);
    }
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (let i = 0; i <= 8; i++) {
      ctx.fillText(((3.3 * i) / 8).toFixed(2), a.x - 6, a.y + a.h - (a.h * i) / 8);
    }

    /* Trigger marker: always the capture's trigger index (0 if untriggered). */
    const tx = xOf(cap.triggerIndex);
    ctx.strokeStyle = 'rgba(255, 181, 71, 0.7)';
    ctx.setLineDash([4, 4]); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(tx, a.y); ctx.lineTo(tx, a.y + a.h); ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = '#ffb547'; ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
    ctx.fillText('T', tx + 4, a.y + 10);

    cap.channels.forEach((data, c) => {
      ctx.strokeStyle = this.colors[c % this.colors.length];
      ctx.shadowColor = ctx.strokeStyle; ctx.shadowBlur = 6;
      ctx.lineWidth = 1.5; ctx.lineJoin = 'round';
      ctx.beginPath();
      for (let i = 0; i < n; i++) {
        const x = xOf(i), y = yOf(data[i]);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
      ctx.shadowBlur = 0;
    });
  }

  _grid(ctx, a) {
    ctx.fillStyle = 'rgba(0, 0, 0, 0.35)';
    ctx.fillRect(a.x, a.y, a.w, a.h);
    ctx.lineWidth = 1;
    for (let i = 0; i <= 10; i++) {
      const x = Math.round(a.x + (a.w * i) / 10) + 0.5;
      ctx.strokeStyle = i === 5 ? '#2c375a' : '#1f2740';
      ctx.beginPath(); ctx.moveTo(x, a.y); ctx.lineTo(x, a.y + a.h); ctx.stroke();
    }
    for (let i = 0; i <= 8; i++) {
      const y = Math.round(a.y + a.h - (a.h * i) / 8) + 0.5;
      ctx.strokeStyle = i === 4 ? '#2c375a' : '#1f2740';
      ctx.beginPath(); ctx.moveTo(a.x, y); ctx.lineTo(a.x + a.w, y); ctx.stroke();
    }
    ctx.strokeStyle = '#40f0e0'; ctx.globalAlpha = 0.5;
    ctx.strokeRect(a.x + 0.5, a.y + 0.5, a.w - 1, a.h - 1);
    ctx.globalAlpha = 1;
  }

  _hover(e) {
    if (!this.capture || this.capture.n === 0) { this.cursorEl.hidden = true; return; }
    const rect = this.canvas.getBoundingClientRect();
    const px = e.clientX - rect.left, py = e.clientY - rect.top;
    const a = this._area();
    if (px < a.x || px > a.x + a.w || py < a.y || py > a.y + a.h) { this.cursorEl.hidden = true; return; }
    const cap = this.capture;
    const i = Math.round(((px - a.x) / a.w) * (cap.n - 1));
    const lines = [`t = ${T.fmtTime(cap.timeAt(i))}  [${i}]`];
    cap.channels.forEach((data, c) => {
      lines.push(`ch${c} = ${T.fmtVolts((data[i] * T.VREF_MV) / 4095 / 1000)}  (${data[i]})`);
    });
    this.cursorEl.textContent = lines.join('\n');
    this.cursorEl.style.left = `${px}px`;
    this.cursorEl.style.top = `${py}px`;
    this.cursorEl.hidden = false;
  }
};
