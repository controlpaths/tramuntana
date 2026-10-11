/* Engineering-notation helpers shared by the scope, logic and status views. */
'use strict';

window.T = window.T || {};

T.fmtHz = (hz) => {
  if (!isFinite(hz)) return '-';
  if (hz >= 1e6) return `${(hz / 1e6).toFixed(hz >= 1e7 ? 1 : 3)} MHz`;
  if (hz >= 1e3) return `${(hz / 1e3).toFixed(hz >= 1e4 ? 1 : 2)} kHz`;
  return `${hz.toFixed(1)} Hz`;
};

T.fmtTime = (s) => {
  const sign = s < 0 ? '-' : '';
  const a = Math.abs(s);
  if (a >= 1) return `${sign}${a.toFixed(3)} s`;
  if (a >= 1e-3) return `${sign}${(a * 1e3).toFixed(2)} ms`;
  if (a >= 1e-6) return `${sign}${(a * 1e6).toFixed(1)} us`;
  return `${sign}${(a * 1e9).toFixed(0)} ns`;
};

T.fmtVolts = (v) => (Math.abs(v) < 1 ? `${(v * 1e3).toFixed(0)} mV` : `${v.toFixed(3)} V`);

T.downloadBlob = (blob, filename) => {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = filename;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 4000);
};
