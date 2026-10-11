/*
 * Tramuntana webapp - UI wiring. Every instrument tab is a thin shell
 * around one T.Tramuntana instance (device.js): forms read their fields,
 * call an instrument method, and show whatever comes back. The only
 * state kept here is the connection itself, the last scope/logic
 * capture (for export) and the independent UART terminal port.
 */
'use strict';

const $ = (id) => document.getElementById(id);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const S = {
  t: null,
  statusTimer: null,
  spiSniffCount: 0,
  i2cSniffCount: 0,
  scopeCapture: null,
  logicCapture: null,
  scopePlot: null,
  logicPlot: null,
  uartTerm: { port: null, reader: null, writer: null, reading: false, dtr: true, rts: true },
  timeline: null,
  emuRegs: null,          /* last register read, to highlight changes */
  emuRo: new Set(),
  emuLogCount: 0,
  lastSeq: null,
};

/* --- small parsing helpers ------------------------------------------------ */

function parseFreq(str) {
  let s = String(str).trim().toLowerCase().replace(/hz$/, '');
  let mult = 1;
  if (s.endsWith('k')) { mult = 1e3; s = s.slice(0, -1); }
  else if (s.endsWith('m')) { mult = 1e6; s = s.slice(0, -1); }
  const v = parseFloat(s);
  if (!isFinite(v) || v <= 0) throw new Error(`bad frequency: "${str}"`);
  return Math.round(v * mult);
}

function parseHexBytes(str) {
  const tokens = String(str).trim().split(/[\s,:]+/).filter(Boolean);
  const out = [];
  for (const tokRaw of tokens) {
    const tok = tokRaw.replace(/^0x/i, '');
    if (!/^[0-9a-fA-F]+$/.test(tok)) throw new Error(`"${tokRaw}" is not hex`);
    if (tok.length > 2 && tok.length % 2 === 0) {
      for (let i = 0; i < tok.length; i += 2) out.push(parseInt(tok.slice(i, i + 2), 16));
    } else {
      out.push(parseInt(tok, 16));
    }
  }
  return Uint8Array.from(out);
}

function parseHexByte(str, def = 0) {
  const s = String(str).trim().replace(/^0x/i, '');
  if (s === '') return def;
  const v = parseInt(s, 16);
  if (!isFinite(v) || v < 0 || v > 0xFF) throw new Error(`"${str}" is not a hex byte`);
  return v;
}

/* --- global error / hint banners ------------------------------------------ */

function showError(msg) {
  const el = $('conn-error');
  el.textContent = String(msg && msg.message || msg);
  el.hidden = false;
  clearTimeout(showError._t);
  showError._t = setTimeout(() => { el.hidden = true; }, 6000);
}

function withBusy(btn, fn) {
  return async (...args) => {
    if (btn) btn.disabled = true;
    try {
      await fn(...args);
    } catch (e) {
      console.error(e);
      showError(e);
    } finally {
      if (btn) btn.disabled = false;
    }
  };
}

/* A checkbox/switch already flipped by the time 'change' fires; if the
 * command that should make it real fails (e.g. EBUSY: another sniffer or
 * a capture holds the shared ring), flip it back rather than leave the
 * switch showing a state the board never reached. */
function wireToggle(checkbox, apply) {
  checkbox.addEventListener('change', async () => {
    const wanted = checkbox.checked;
    checkbox.disabled = true;
    try {
      await apply(wanted);
    } catch (e) {
      checkbox.checked = !wanted;
      console.error(e);
      showError(e);
    } finally {
      checkbox.disabled = false;
    }
  });
}

/* --- connection ------------------------------------------------------------ */

function setConnectedUi(connected) {
  $('main').classList.toggle('disconnected', !connected);
  $('btn-connect').hidden = connected;
  $('btn-disconnect').hidden = !connected;
  const status = $('conn-status');
  status.textContent = connected ? 'CONNECTED' : 'DISCONNECTED';
  status.className = `status ${connected ? 'status-on' : 'status-off'}`;
}

async function connect() {
  $('conn-error').hidden = true;
  $('conn-hint').hidden = true;
  let t;
  try {
    t = await T.Tramuntana.connect();
  } catch (e) {
    if (/did not answer|not a Tramuntana/i.test(e.message)) $('conn-hint').hidden = false;
    if (!/cancelled|No port selected/i.test(e.message)) showError(e);
    return;
  }
  S.t = t;
  S.t.onEvent(handleDeviceEvent);
  try {
    const info = await t.info();
    $('sys-fw').textContent = info.fwVersion;
    $('sys-board').textContent = info.board;
    $('sys-sysclk').textContent = T.fmtHz(info.sysclkHz);
    $('sys-chip').textContent = info.chipId;
    $('sys-proto').textContent = info.protoVersion;
    $('sys-port').textContent = t.link.port.getInfo ? describePort(t.link.port) : '-';
  } catch (e) { showError(e); }
  setConnectedUi(true);
  pollStatus();
  S.statusTimer = setInterval(pollStatus, 1000);
}

function describePort(port) {
  try {
    const info = port.getInfo();
    return info.usbVendorId ? `VID:PID ${info.usbVendorId.toString(16)}:${info.usbProductId.toString(16)}` : 'serial port';
  } catch (e) { return 'serial port'; }
}

async function disconnect() {
  clearInterval(S.statusTimer);
  S.statusTimer = null;
  if (S.timeline && S.timeline.running) { try { await stopTimeline(); } catch (e) { /* ignore */ } }
  if (S.t) { try { await S.t.close(); } catch (e) { /* ignore */ } }
  S.t = null;
  setConnectedUi(false);
}

async function pollStatus() {
  if (!S.t) return;
  try {
    const st = await S.t.status();
    $('sys-power').checked = st.power;
    $('sys-pullups').checked = st.pullups;
    renderPinsGrid(st.pins);
    $('st-scope').textContent = st.scope;
    $('st-logic').textContent = st.logic;
    $('st-awg').textContent = st.awg ? 'running' : 'off';
    $('st-spi-sniff').textContent = st.spiSniff ? 'listening' : 'off';
    $('st-i2c-sniff').textContent = st.i2cSniff ? 'listening' : 'off';
    /* Keep the sniffer switches on the SPI/I2C tabs honest too - e.g. if
     * this toggle failed and got reverted, or another client started it. */
    if (document.activeElement !== $('spi-sniff-toggle')) $('spi-sniff-toggle').checked = st.spiSniff;
    if (document.activeElement !== $('i2c-sniff-toggle')) $('i2c-sniff-toggle').checked = st.i2cSniff;
    $('st-uart-bridge').textContent = st.uartBridge ? 'active' : 'idle';
    $('st-rx-dropped').textContent = st.rxDropped;
    const u = await S.t.uart.status();
    $('uart-st-active').textContent = u.active ? 'yes' : 'no';
    $('uart-st-baud').textContent = u.baud || '-';
    $('uart-st-bytes').textContent = `${u.rxBytes} / ${u.txBytes}`;
    if (u.ctrl !== undefined) {
      $('uart-ctrl-st-mode').textContent = u.ctrl;
      $('uart-ctrl-st-lines').textContent = `${u.dtr ? 1 : 0} / ${u.rts ? 1 : 0}`;
      $('uart-ctrl-st-reset').textContent = u.resetDio === null ? '-' : `DIO${u.resetDio} ${u.resetLow ? 'LOW' : 'released'}`;
      $('uart-ctrl-st-boot').textContent = u.bootDio === null ? '-' : `DIO${u.bootDio} ${u.bootLow ? 'LOW' : 'released'}`;
    }
    if (isTabActive('emu')) {
      await refreshEmuStatus();
      if ($('emu-auto').checked) await readEmuRegs();
    }
  } catch (e) {
    /* a poll failing (port gone) means the board disappeared */
    if (/no reply|link closed|network error/i.test(e.message || '')) await disconnect();
  }
}

const PIN_OWNER_CLASS = { gpio: 'owner-gpio', spi: 'owner-spi', i2c: 'owner-i2c', uart: 'owner-uart', pio: 'owner-pio', pwm: 'owner-pwm', emu: 'owner-emu' };

function isTabActive(name) { return $$('.tab.active').some((b) => b.dataset.tab === name); }

function renderPinsGrid(pins) {
  const grid = $('pins-grid');
  if (grid.children.length !== 8) {
    grid.innerHTML = '';
    for (let i = 0; i < 8; i++) {
      const cell = document.createElement('div');
      cell.className = 'pin-cell';
      cell.innerHTML = `<span class="pin-name">DIO${i}</span><b class="pin-owner"></b>`;
      grid.appendChild(cell);
    }
  }
  pins.forEach((owner, i) => {
    const cell = grid.children[i];
    const b = cell.querySelector('.pin-owner');
    const excl = owner.endsWith('!');
    const name = excl ? owner.slice(0, -1) : owner;
    b.textContent = name + (excl ? ' (excl)' : '');
    b.className = `pin-owner ${PIN_OWNER_CLASS[name] || ''}`;
  });
}

function handleDeviceEvent(ev) {
  if (ev.type === 'spi') appendSpiLog(ev);
  else if (ev.type === 'i2c') appendI2cLog(ev);
  else if (ev.type === 'emu') appendEmuLog(ev);
}

/* --- tabs -------------------------------------------------------------- */

function initTabs() {
  $$('.tab').forEach((tab) => {
    tab.addEventListener('click', () => {
      $$('.tab').forEach((b) => b.classList.toggle('active', b === tab));
      $$('.panel').forEach((p) => p.classList.toggle('active', p.dataset.panel === tab.dataset.tab));
      if (tab.dataset.tab === 'pwm' && S.t) refreshPwm(true).catch(showError);
      if (tab.dataset.tab === 'emu' && S.t) Promise.all([refreshEmuStatus(), readEmuRegs()]).catch(showError);
    });
  });
}

/* --- SYSTEM tab ---------------------------------------------------------- */

function initSystem() {
  wireToggle($('sys-power'), (on) => S.t.power(on));
  wireToggle($('sys-pullups'), (on) => S.t.pullups(on));

  $('btn-adc0').addEventListener('click', withBusy($('btn-adc0'), async () => {
    $('sys-adc0').textContent = T.fmtVolts(await S.t.adc(0));
  }));
  $('btn-adc1').addEventListener('click', withBusy($('btn-adc1'), async () => {
    $('sys-adc1').textContent = T.fmtVolts(await S.t.adc(1));
  }));
  $('btn-display-send').addEventListener('click', withBusy($('btn-display-send'), async () => {
    await S.t.display($('sys-display-text').value);
  }));

  $$('.led-btn').forEach((btn) => {
    btn.addEventListener('click', withBusy(btn, async () => { await S.t.led(btn.dataset.colour); }));
  });

  $('btn-bootloader').addEventListener('click', async () => {
    if (!confirm('Reboot the board into the UF2 bootloader? The API port will disappear.')) return;
    await S.t.bootloader();
    await disconnect();
  });

  const dioOut = $('dio-out-cells'), dioVal = $('dio-val-cells'), dioLvl = $('dio-lvl-cells');
  const wrapBit = (el) => {
    const wrap = document.createElement('span');
    wrap.className = 'bit-cell';
    wrap.appendChild(el);
    return wrap;
  };
  const bitCheckbox = () => { const el = document.createElement('input'); el.type = 'checkbox'; return el; };
  for (let i = 0; i < 8; i++) {
    dioOut.appendChild(wrapBit(bitCheckbox()));
    dioVal.appendChild(wrapBit(bitCheckbox()));
    const dot = document.createElement('span');
    dot.className = 'bit-lvl'; dot.id = `dio-lvl-${i}`;
    dioLvl.appendChild(wrapBit(dot));
  }

  $('btn-dio-dir').addEventListener('click', withBusy($('btn-dio-dir'), async () => {
    let outMask = 0;
    dioOut.querySelectorAll('input').forEach((cb, i) => { if (cb.checked) outMask |= (1 << i); });
    await S.t.dioDir(0xFF, outMask);
  }));
  $('btn-dio-write').addEventListener('click', withBusy($('btn-dio-write'), async () => {
    let value = 0;
    dioVal.querySelectorAll('input').forEach((cb, i) => { if (cb.checked) value |= (1 << i); });
    await S.t.dioWrite(0xFF, value);
  }));
  $('btn-dio-read').addEventListener('click', withBusy($('btn-dio-read'), async () => {
    const v = await S.t.dioRead();
    for (let i = 0; i < 8; i++) {
      const dot = $(`dio-lvl-${i}`);
      dot.classList.toggle('bit-on', !!((v >> i) & 1));
      dot.textContent = (v >> i) & 1;
    }
  }));
}

/* --- SPI tab --------------------------------------------------------------- */

function initSpi() {
  $('btn-spi-conf').addEventListener('click', withBusy($('btn-spi-conf'), async () => {
    const hz = parseFreq($('spi-freq').value);
    const mode = parseInt($('spi-mode').value, 10);
    const lsb = $('spi-lsb').checked;
    const actual = await S.t.spi.config(hz, mode, lsb);
    $('spi-actual-freq').textContent = T.fmtHz(actual);
  }));

  $('btn-spi-sel').addEventListener('click', withBusy($('btn-spi-sel'), async () => { await S.t.spi.sel(); }));
  $('btn-spi-unsel').addEventListener('click', withBusy($('btn-spi-unsel'), async () => { await S.t.spi.unsel(); }));

  $('btn-spi-write').addEventListener('click', withBusy($('btn-spi-write'), async () => {
    const tx = parseHexBytes($('spi-tx').value);
    await S.t.spi.write(tx);
    $('spi-tx-out').textContent = T.hex(tx);
    $('spi-rx-out').textContent = '-';
  }));

  $('btn-spi-read').addEventListener('click', withBusy($('btn-spi-read'), async () => {
    const n = parseInt($('spi-read-n').value, 10) || 0;
    const fill = parseHexByte($('spi-fill').value, 0);
    const rx = await S.t.spi.read(n, fill);
    $('spi-tx-out').textContent = `(fill 0x${fill.toString(16).padStart(2, '0')})`;
    $('spi-rx-out').textContent = T.hex(rx);
  }));

  $('btn-spi-xfer').addEventListener('click', withBusy($('btn-spi-xfer'), async () => {
    const tx = parseHexBytes($('spi-tx').value);
    const rx = await S.t.spi.xfer(tx);
    $('spi-tx-out').textContent = T.hex(tx);
    $('spi-rx-out').textContent = T.hex(rx);
  }));

  $('btn-spi-txn').addEventListener('click', withBusy($('btn-spi-txn'), async () => {
    const tx = parseHexBytes($('spi-tx').value);
    const n = parseInt($('spi-read-n').value, 10) || 0;
    const rx = await S.t.spi.txn(tx, n);
    $('spi-tx-out').textContent = T.hex(tx);
    $('spi-rx-out').textContent = n > 0 ? T.hex(rx) : '(sel + write + unsel, no read requested)';
  }));

  wireToggle($('spi-sniff-toggle'), (on) => {
    const modeSel = $('spi-sniff-mode').value;
    const mode = modeSel === '' ? null : parseInt(modeSel, 10);
    return S.t.spi.sniff(on, mode);
  });
  $('btn-spi-sniff-clear').addEventListener('click', () => { $('spi-sniff-log').innerHTML = ''; S.spiSniffCount = 0; });
}

function appendSpiLog(ev) {
  S.spiSniffCount++;
  const log = $('spi-sniff-log');
  const shown = ev.mosi.length;
  const more = ev.total > shown ? ' ..' : '';
  const row = document.createElement('div');
  row.className = 'sniff-msg' + (ev.overrun ? ' overrun' : '');
  row.innerHTML = `<span class="no">#${S.spiSniffCount}</span>` +
    `<code><span class="tag tag-tx" style="margin-right:6px">MOSI</span>${T.hex(ev.mosi)}${more}<br>` +
    `<span class="tag tag-rx" style="margin-right:6px">MISO</span>${T.hex(ev.miso)}${more}</code>` +
    `<span class="meta">${ev.total}B${ev.overrun ? ' OVERRUN' : ''}</span>`;
  log.appendChild(row);
  log.scrollTop = log.scrollHeight;
  while (log.children.length > 300) log.removeChild(log.firstChild);
}

/* --- I2C tab --------------------------------------------------------------- */

function buildI2cMap() {
  const map = $('i2c-map');
  map.innerHTML = '';
  for (let a = 0; a < 128; a++) {
    const cell = document.createElement('div');
    cell.className = 'i2c-cell' + (a < 0x08 || a > 0x77 ? ' reserved' : '');
    cell.id = `i2c-cell-${a}`;
    cell.textContent = a.toString(16).padStart(2, '0');
    cell.addEventListener('click', () => { $('i2c-addr').value = a.toString(16).padStart(2, '0'); });
    map.appendChild(cell);
  }
}

function initI2c() {
  buildI2cMap();

  $('btn-i2c-conf').addEventListener('click', withBusy($('btn-i2c-conf'), async () => {
    const hz = parseFreq($('i2c-freq').value);
    const actual = await S.t.i2c.config(hz, $('i2c-pullups').checked);
    $('i2c-actual-freq').textContent = T.fmtHz(actual);
  }));

  $('btn-i2c-scan').addEventListener('click', withBusy($('btn-i2c-scan'), async () => {
    $$('.i2c-cell').forEach((c) => c.classList.remove('found'));
    const found = await S.t.i2c.scan();
    found.forEach((a) => { const c = $(`i2c-cell-${a}`); if (c) c.classList.add('found'); });
    $('i2c-scan-count').textContent = `${found.length} device(s)`;
  }));

  $('btn-i2c-read').addEventListener('click', withBusy($('btn-i2c-read'), async () => {
    const addr = parseHexByte($('i2c-addr').value);
    const n = parseInt($('i2c-n').value, 10) || 1;
    const regStr = $('i2c-reg').value.trim();
    const data = regStr === '' ? await S.t.i2c.read(addr, n) : await S.t.i2c.regRead(addr, parseHexByte(regStr), n);
    $('i2c-out').textContent = T.hex(data);
  }));

  $('btn-i2c-write').addEventListener('click', withBusy($('btn-i2c-write'), async () => {
    const addr = parseHexByte($('i2c-addr').value);
    const regStr = $('i2c-reg').value.trim();
    const wbytes = parseHexBytes($('i2c-wbytes').value);
    const payload = regStr === '' ? wbytes : Uint8Array.from([parseHexByte(regStr), ...wbytes]);
    const acked = await S.t.i2c.write(addr, payload);
    $('i2c-out').textContent = `${acked} / ${payload.length} byte(s) acked`;
  }));

  $('btn-i2c-start').addEventListener('click', withBusy($('btn-i2c-start'), async () => {
    const b = parseHexByte($('i2c-start-byte').value);
    const ack = await S.t.i2c.start(b >> 1, !!(b & 1));
    $('i2c-lowlevel-out').textContent = `start 0x${b.toString(16)}: ${ack ? 'ACK' : 'NAK'}`;
  }));
  $('btn-i2c-stop').addEventListener('click', withBusy($('btn-i2c-stop'), async () => {
    await S.t.i2c.stop();
    $('i2c-lowlevel-out').textContent = 'stop';
  }));
  $('btn-i2c-raw-write').addEventListener('click', withBusy($('btn-i2c-raw-write'), async () => {
    const data = parseHexBytes($('i2c-raw-wbytes').value);
    const acked = await S.t.i2c.rawWrite(data);
    $('i2c-lowlevel-out').textContent = `raw write: ${acked} / ${data.length} acked`;
  }));
  $('btn-i2c-raw-read').addEventListener('click', withBusy($('btn-i2c-raw-read'), async () => {
    const n = parseInt($('i2c-raw-n').value, 10) || 1;
    const data = await S.t.i2c.rawRead(n, $('i2c-raw-nack').checked);
    $('i2c-lowlevel-out').textContent = `raw read: ${T.hex(data)}`;
  }));
  $('btn-i2c-reset').addEventListener('click', withBusy($('btn-i2c-reset'), async () => {
    await S.t.i2c.resetBus();
    $('i2c-lowlevel-out').textContent = 'bus reset';
  }));

  wireToggle($('i2c-sniff-toggle'), (on) => S.t.i2c.sniff(on, $('i2c-sniff-pullups').checked));
  $('btn-i2c-sniff-clear').addEventListener('click', () => { $('i2c-sniff-log').innerHTML = ''; S.i2cSniffCount = 0; });
}

function appendI2cLog(ev) {
  S.i2cSniffCount++;
  const log = $('i2c-sniff-log');
  const row = document.createElement('div');
  row.className = 'sniff-msg' + (ev.overrun ? ' overrun' : '');
  const text = ev.frames.map((f) => {
    const addr = f.address.toString(16).padStart(2, '0').toUpperCase();
    let s = `S ${addr}${f.read ? 'R' : 'W'}${f.ack ? '+' : '-'}`;
    s += f.data.map((d) => ` ${d.byte.toString(16).padStart(2, '0').toUpperCase()}${d.ack ? '+' : '-'}`).join('');
    return s;
  }).join('  ');
  row.innerHTML = `<span class="no">#${S.i2cSniffCount}</span><code>${text} P</code>` +
    `<span class="meta">${ev.overrun ? 'OVERRUN' : ev.truncated ? 'truncated' : ''}</span>`;
  log.appendChild(row);
  log.scrollTop = log.scrollHeight;
  while (log.children.length > 300) log.removeChild(log.firstChild);
}

/* --- SCOPE tab --------------------------------------------------------------- */

function initScope() {
  S.scopePlot = new T.ScopePlot($('scope-canvas'), $('scope-cursor'));

  const captureBtn = $('btn-scope-capture');
  captureBtn.addEventListener('click', withBusy(captureBtn, async () => {
    const opts = {
      n: parseInt($('scope-n').value, 10),
      channels: parseInt($('scope-ch').value, 10),
      rateHz: parseFreq($('scope-rate').value),
      trigger: $('scope-trigger').value,
      level: parseFloat($('scope-level').value),
      pre: parseInt($('scope-pre').value, 10) || 0,
      timeoutMs: parseInt($('scope-timeout').value, 10) || 5000,
    };
    $('scope-state').textContent = 'arming...';
    try {
      const cap = await S.t.scope.capture(opts, (st) => { $('scope-state').textContent = st.state; });
      S.scopeCapture = cap;
      S.scopePlot.setCapture(cap);
      $('scope-empty').hidden = true;
      updateScopeStats(cap);
      $('scope-state').textContent = 'done';
    } catch (e) {
      $('scope-state').textContent = /timeout/i.test(e.message) ? 'no trigger (timeout)' : 'aborted';
      if (!/timeout|aborted/i.test(e.message)) throw e;
    }
  }));

  $('btn-scope-abort').addEventListener('click', withBusy($('btn-scope-abort'), async () => { await S.t.scope.abort(); }));
  $('btn-scope-csv').addEventListener('click', () => {
    if (!S.scopeCapture) return showError('no capture to export');
    T.downloadBlob(S.scopeCapture.toCsvBlob(), 'tramuntana_scope.csv');
  });
}

function updateScopeStats(cap) {
  for (let c = 0; c < 2; c++) {
    const on = c < cap.channels.length;
    document.querySelectorAll(`.stat-ch${c}`).forEach((el) => { el.style.display = on ? '' : 'none'; });
    if (!on) continue;
    const s = cap.stats(c);
    $(`st-scope-mm${c}`).textContent = `${T.fmtVolts(s.min)} / ${T.fmtVolts(s.max)}`;
    $(`st-scope-mr${c}`).textContent = `${T.fmtVolts(s.mean)} / ${T.fmtVolts(s.rms)}`;
    $(`st-scope-f${c}`).textContent = s.freq ? T.fmtHz(s.freq) : '-';
  }
}

/* --- AWG tab ------------------------------------------------------------------ */

function initAwg() {
  const paramLabels = { square: 'Duty %', stairs: 'Steps' };
  function syncParamField() {
    const shape = $('awg-shape').value;
    const label = paramLabels[shape];
    $('awg-param-field').hidden = !label;
    if (label) $('awg-param-label').textContent = label;
  }
  $('awg-shape').addEventListener('change', syncParamField);
  syncParamField();

  $('btn-awg-set').addEventListener('click', withBusy($('btn-awg-set'), async () => {
    const shape = $('awg-shape').value;
    if (shape === 'arb') { showError('use LOAD & PLAY for the arbitrary table'); return; }
    const freq = parseFreq($('awg-freq').value);
    const amp = parseFloat($('awg-amp').value);
    const offset = parseFloat($('awg-offset').value);
    const param = paramLabels[shape] ? (parseInt($('awg-param').value, 10) || 0) : 0;
    const r = await S.t.awg.set(shape, freq, amp, offset, param);
    $('awg-status').textContent = `${shape} ${T.fmtHz(r.freqHz)} (table ${r.table}, top ${r.top})`;
  }));

  $('btn-awg-off').addEventListener('click', withBusy($('btn-awg-off'), async () => {
    await S.t.awg.off();
    $('awg-status').textContent = 'stopped';
  }));

  $('btn-awg-dc').addEventListener('click', withBusy($('btn-awg-dc'), async () => {
    await S.t.awg.dc(parseFloat($('awg-dc-volts').value));
    $('awg-status').textContent = `DC ${$('awg-dc-volts').value} V`;
  }));

  $('btn-awg-step').addEventListener('click', withBusy($('btn-awg-step'), async () => {
    await S.t.awg.step(parseFloat($('awg-step-from').value), parseFloat($('awg-step-to').value),
      parseInt($('awg-step-ms').value, 10) || 0);
    $('awg-status').textContent = 'step applied';
  }));

  $('btn-awg-arb-sine').addEventListener('click', () => {
    const n = 64;
    const vals = Array.from({ length: n }, (_, i) => Math.sin((2 * Math.PI * i) / n).toFixed(4));
    $('awg-arb-samples').value = vals.join(', ');
  });

  $('btn-awg-arb-load').addEventListener('click', withBusy($('btn-awg-arb-load'), async () => {
    const text = $('awg-arb-samples').value.trim();
    if (!text) { showError('paste or generate some samples first'); return; }
    const samples = text.split(/[,\s]+/).filter(Boolean).map(Number);
    if (samples.some((v) => !isFinite(v))) throw new Error('samples must be numbers between -1 and 1');
    if (samples.length > 4096) throw new Error('4096 samples max');
    const freq = parseFreq($('awg-freq').value);
    const amp = parseFloat($('awg-amp').value);
    const offset = parseFloat($('awg-offset').value);
    const r = await S.t.awg.arbitrary(samples.map((v) => Math.max(-1, Math.min(1, v))), freq, amp, offset);
    $('awg-shape').value = 'arb'; syncParamField();
    $('awg-status').textContent = `arb (${samples.length} pts) ${T.fmtHz(r.freqHz)} (table ${r.table})`;
  }));
}

/* --- PWM tab ------------------------------------------------------------------- */

const PWM_PAIRS = [[7, 6], [5, 4], [3, 2], [1, 0]];   /* slice 0..3, channel A first */

function buildPwmPairs() {
  const root = $('pwm-pairs');
  root.innerHTML = '';
  PWM_PAIRS.forEach(([a, b], slice) => {
    const card = document.createElement('div');
    card.className = 'card';
    const pin = (d) => `
      <div class="field-row pwm-pin">
        <label class="switch sw-green"><input type="checkbox" id="pwm-en-${d}"><span>DIO${d}</span></label>
        <label class="field"><span>Duty (%)</span><input type="number" id="pwm-duty-${d}" value="50" step="0.01" min="0" max="100"></label>
        <label class="switch"><input type="checkbox" id="pwm-inv-${d}"><span>invert</span></label>
        <b class="pwm-state" id="pwm-state-${d}">off</b>
      </div>`;
    card.innerHTML = `
      <h2>SLICE ${slice} <span class="hint">DIO${a} / DIO${b}</span></h2>
      <div class="form">
        <div class="field-row">
          <label class="field"><span>Frequency (Hz, k/M ok)</span><input type="text" id="pwm-freq-${slice}" value="1k"></label>
          <label class="field"><span>Phase (deg, on sync)</span><input type="number" id="pwm-phase-${slice}" value="0" min="0" max="359"></label>
        </div>
        ${pin(a)}${pin(b)}
        <div class="btn-row">
          <button id="btn-pwm-apply-${slice}" class="btn btn-primary">APPLY</button>
          <button id="btn-pwm-stop-${slice}" class="btn btn-danger">STOP</button>
        </div>
        <div class="hint" id="pwm-actual-${slice}">-</div>
      </div>`;
    root.appendChild(card);
    $(`btn-pwm-apply-${slice}`).addEventListener('click', withBusy($(`btn-pwm-apply-${slice}`), () => applyPwmPair(slice)));
    $(`btn-pwm-stop-${slice}`).addEventListener('click', withBusy($(`btn-pwm-stop-${slice}`), async () => {
      await S.t.pwm.stop((1 << a) | (1 << b));
      await refreshPwm(false);
    }));
  });
}

async function applyPwmPair(slice) {
  const pins = PWM_PAIRS[slice];
  const mask = (1 << pins[0]) | (1 << pins[1]);
  const freq = parseFreq($(`pwm-freq-${slice}`).value);
  const phase = parseInt($(`pwm-phase-${slice}`).value, 10) || 0;
  if (phase < 0 || phase > 359) throw new Error('phase: 0..359 degrees');
  const wanted = pins.filter((d) => $(`pwm-en-${d}`).checked);
  const duty = {};
  for (const d of wanted) {
    duty[d] = parseFloat($(`pwm-duty-${d}`).value);
    if (!isFinite(duty[d]) || duty[d] < 0 || duty[d] > 100) throw new Error(`DIO${d}: duty 0..100 %`);
  }

  const st = await S.t.pwm.status();
  const unwanted = pins.filter((d) => !wanted.includes(d) && st[d].active);
  if (unwanted.length) await S.t.pwm.stop(unwanted.reduce((m, d) => m | (1 << d), 0));
  /* Both pins share the slice's frequency: retuning a live pair means
   * stopping it first, or the second pin's set would be refused. */
  const live = pins.filter((d) => st[d].active && !st[d].lost);
  if (wanted.length === 2 && live.some((d) => st[d].freqHz !== freq)) await S.t.pwm.stop(mask);
  await S.t.pwm.phase(pins[0], phase);
  for (const d of wanted) await S.t.pwm.set(d, freq, duty[d], $(`pwm-inv-${d}`).checked);
  await refreshPwm(false);
}

/* loadForm: also copy the board's settings into the inputs (on tab open). */
async function refreshPwm(loadForm) {
  if (!S.t) return;
  const st = await S.t.pwm.status();
  PWM_PAIRS.forEach(([a, b], slice) => {
    const live = [a, b].filter((d) => st[d].active);
    for (const d of [a, b]) {
      const s = st[d];
      const el = $(`pwm-state-${d}`);
      el.textContent = !s.active ? 'off' : s.lost ? 'pin taken' : `${s.duty.toFixed(2)} %`;
      el.className = `pwm-state ${s.active && !s.lost ? 'pwm-on' : s.lost ? 'pwm-lost' : ''}`;
      if (loadForm) {
        $(`pwm-en-${d}`).checked = s.active && !s.lost;
        if (s.active) {
          $(`pwm-duty-${d}`).value = s.duty;
          $(`pwm-inv-${d}`).checked = s.invert;
        }
      }
    }
    const ref = live.length ? st[live[0]] : null;
    $(`pwm-actual-${slice}`).textContent = ref
      ? `running at ${T.fmtHz(ref.actualHz)} (requested ${T.fmtHz(ref.freqHz)}), phase ${ref.phase}°`
      : 'stopped';
    if (loadForm && ref) {
      $(`pwm-freq-${slice}`).value = ref.freqHz;
      $(`pwm-phase-${slice}`).value = ref.phase;
    }
  });
}

function initPwm() {
  buildPwmPairs();
  $('btn-pwm-sync').addEventListener('click', withBusy($('btn-pwm-sync'), async () => {
    /* Push the phase fields first, so SYNC uses what's on screen. */
    for (let slice = 0; slice < 4; slice++) {
      const phase = parseInt($(`pwm-phase-${slice}`).value, 10) || 0;
      await S.t.pwm.phase(PWM_PAIRS[slice][0], Math.max(0, Math.min(359, phase)));
    }
    await S.t.pwm.sync(0);
    await refreshPwm(false);
  }));
  $('btn-pwm-stop-all').addEventListener('click', withBusy($('btn-pwm-stop-all'), async () => {
    await S.t.pwm.stop(0xFF);
    await refreshPwm(true);
  }));
  $('btn-pwm-refresh').addEventListener('click', withBusy($('btn-pwm-refresh'), () => refreshPwm(true)));
}

/* --- LOGIC tab ------------------------------------------------------------------ */

function initLogic() {
  S.logicPlot = new T.LogicPlot($('logic-canvas'), $('logic-cursor'), $('logic-view'));
  $('btn-logic-zoom-in').addEventListener('click', () => S.logicPlot.zoomIn());
  $('btn-logic-zoom-out').addEventListener('click', () => S.logicPlot.zoomOut());
  $('btn-logic-fit').addEventListener('click', () => S.logicPlot.fit());

  function syncTriggerFields() {
    const mode = $('logic-trigger').value;
    $('logic-pin-field').hidden = mode !== 'rising' && mode !== 'falling';
    $('logic-pattern-fields').hidden = mode !== 'pattern';
    $('logic-pre-field').hidden = mode === 'none';
  }
  $('logic-trigger').addEventListener('change', syncTriggerFields);
  syncTriggerFields();

  const captureBtn = $('btn-logic-capture');
  captureBtn.addEventListener('click', withBusy(captureBtn, async () => {
    const trigger = $('logic-trigger').value;
    const opts = {
      n: parseInt($('logic-n').value, 10),
      rateHz: parseFreq($('logic-rate').value),
      trigger,
      pin: parseInt($('logic-pin').value, 10) || 0,
      mask: trigger === 'pattern' ? parseHexByte($('logic-mask').value) : 0,
      value: trigger === 'pattern' ? parseHexByte($('logic-value').value) : 0,
      pre: trigger !== 'none' ? (parseInt($('logic-pre').value, 10) || 0) : 0,
      timeoutMs: parseInt($('logic-timeout').value, 10) || 5000,
    };
    $('logic-state').textContent = 'arming...';
    try {
      const cap = await S.t.logic.capture(opts, (st) => { $('logic-state').textContent = st.state; });
      S.logicCapture = cap;
      S.logicPlot.setCapture(cap);
      $('logic-empty').hidden = true;
      updateLogicTransitions(cap);
      $('logic-state').textContent = 'done';
    } catch (e) {
      $('logic-state').textContent = /timeout/i.test(e.message) ? 'no trigger (timeout)' : 'aborted';
      if (!/timeout|aborted/i.test(e.message)) throw e;
    }
  }));

  $('btn-logic-abort').addEventListener('click', withBusy($('btn-logic-abort'), async () => { await S.t.logic.abort(); }));
  $('btn-logic-vcd').addEventListener('click', () => {
    if (!S.logicCapture) return showError('no capture to export');
    T.downloadBlob(S.logicCapture.toVcdBlob(), 'tramuntana_logic.vcd');
  });
  $('btn-logic-csv').addEventListener('click', () => {
    if (!S.logicCapture) return showError('no capture to export');
    T.downloadBlob(S.logicCapture.toCsvBlob(), 'tramuntana_logic.csv');
  });
}

function updateLogicTransitions(cap) {
  const grid = $('logic-transitions');
  grid.innerHTML = '';
  for (let d = 0; d < 8; d++) {
    const cell = document.createElement('div');
    cell.className = 'stat';
    cell.innerHTML = `<span style="color:${T.LOGIC_COLORS[d]}">DIO${d}</span><b>${cap.edges(d).length}</b>`;
    grid.appendChild(cell);
  }
}

/* --- UART tab ------------------------------------------------------------------- */

function initUart() {
  $('btn-uart-apply').addEventListener('click', withBusy($('btn-uart-apply'), async () => {
    const mode = $('uart-mode').value;
    const baud = parseInt($('uart-baud-override').value, 10) || 0;
    await S.t.uart.config(mode, baud);
  }));

  $('btn-uart-term-connect').addEventListener('click', withBusy($('btn-uart-term-connect'), uartTermConnect));
  $('btn-uart-term-disconnect').addEventListener('click', uartTermDisconnect);

  $('btn-uart-ctrl-apply').addEventListener('click', withBusy($('btn-uart-ctrl-apply'), async () => {
    const pin = (v) => (v === '' ? null : parseInt(v, 10));
    const reset = pin($('uart-ctrl-reset').value), boot = pin($('uart-ctrl-boot').value);
    if (reset !== null && reset === boot) throw new Error('reset and boot must be different pins');
    await S.t.uart.ctrl($('uart-ctrl-mode').value, reset, boot);
    await pollStatus();
  }));
  wireToggle($('uart-term-dtr'), (on) => uartTermSignals({ dtr: on }));
  wireToggle($('uart-term-rts'), (on) => uartTermSignals({ rts: on }));
  $('btn-uart-term-reset').addEventListener('click', withBusy($('btn-uart-term-reset'), async () => {
    const before = { dtr: S.uartTerm.dtr, rts: S.uartTerm.rts };
    await uartTermSignals({ dtr: false, rts: true });
    await new Promise((r) => setTimeout(r, 100));
    await uartTermSignals(before);
  }));

  const input = $('uart-term-input');
  input.addEventListener('keydown', async (e) => {
    if (e.key !== 'Enter' || !S.uartTerm.writer) return;
    const endings = { none: '', lf: '\n', crlf: '\r\n', cr: '\r' };
    const text = input.value + (endings[$('uart-term-lineend').value] || '');
    input.value = '';
    try {
      await S.uartTerm.writer.write(new TextEncoder().encode(text));
      termAppend(text, 'tx');
      if (S.timeline) S.timeline.note(JSON.stringify(text).slice(1, -1), 'uart_tx');
    } catch (e2) { showError(e2); }
  });
}

async function uartTermConnect() {
  const port = await navigator.serial.requestPort({
    filters: [{ usbVendorId: PROTO.USB_VID, usbProductId: PROTO.USB_PID }],
  });
  const baud = parseInt($('uart-term-baud').value, 10);
  await port.open({ baudRate: baud });
  S.uartTerm.port = port;
  S.uartTerm.writer = port.writable.getWriter();
  S.uartTerm.reading = true;
  termAppend(`[connected @ ${baud}]\n`, 'sys');
  $('btn-uart-term-connect').hidden = true;
  $('btn-uart-term-disconnect').hidden = false;
  $('uart-term-input').disabled = false;
  /* Opening a port asserts DTR and RTS: show it. */
  S.uartTerm.dtr = true; S.uartTerm.rts = true;
  for (const id of ['uart-term-dtr', 'uart-term-rts', 'btn-uart-term-reset']) $(id).disabled = false;
  $('uart-term-dtr').checked = true; $('uart-term-rts').checked = true;
  $('uart-term-input').focus();
  uartTermReadLoop();
}

async function uartTermReadLoop() {
  const port = S.uartTerm.port;
  const reader = port.readable.getReader();
  S.uartTerm.reader = reader;
  const decoder = new TextDecoder();
  try {
    while (S.uartTerm.reading) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value && value.length) {
        const text = decoder.decode(value, { stream: true });
        termAppend(text);
        if (S.timeline) S.timeline.feedUart(text);
      }
    }
  } catch (e) {
    termAppend(`\n[link error: ${e.message}]\n`, 'err');
  } finally {
    try { reader.releaseLock(); } catch (e) { /* ignore */ }
  }
  if (S.uartTerm.reading) await uartTermDisconnect();
}

async function uartTermDisconnect() {
  S.uartTerm.reading = false;
  try { await S.uartTerm.reader?.cancel(); } catch (e) { /* ignore */ }
  try { S.uartTerm.writer?.releaseLock(); } catch (e) { /* ignore */ }
  try { await S.uartTerm.port?.close(); } catch (e) { /* ignore */ }
  S.uartTerm = { port: null, reader: null, writer: null, reading: false, dtr: true, rts: true };
  termAppend('\n[disconnected]\n', 'sys');
  for (const id of ['uart-term-dtr', 'uart-term-rts', 'btn-uart-term-reset']) $(id).disabled = true;
  $('btn-uart-term-connect').hidden = false;
  $('btn-uart-term-disconnect').hidden = true;
  $('uart-term-input').disabled = true;
}

/* DTR / RTS of the terminal's port: with control lines on (UART tab), they
 * drive the target's reset / boot pins. */
async function uartTermSignals({ dtr, rts }) {
  if (!S.uartTerm.port) throw new Error('connect the terminal port first');
  const sig = {};
  if (dtr !== undefined) { sig.dataTerminalReady = dtr; S.uartTerm.dtr = dtr; $('uart-term-dtr').checked = dtr; }
  if (rts !== undefined) { sig.requestToSend = rts; S.uartTerm.rts = rts; $('uart-term-rts').checked = rts; }
  await S.uartTerm.port.setSignals(sig);
}

/* --- EMU tab ---------------------------------------------------------------------- */

function initEmu() {
  $('btn-emu-i2c').addEventListener('click', withBusy($('btn-emu-i2c'), async () => {
    const scl = parseInt($('emu-i2c-scl').value, 10), sda = parseInt($('emu-i2c-sda').value, 10);
    if (scl === sda) throw new Error('SCL and SDA must be different pins');
    const addr = parseHexByte($('emu-i2c-addr').value);
    if (addr > 0x7F) throw new Error('address: 7-bit (00..7f)');
    await S.t.emu.i2c(addr, scl, sda, $('emu-i2c-pu').checked);
    await refreshEmuStatus();
  }));
  $('btn-emu-spi').addEventListener('click', withBusy($('btn-emu-spi'), async () => {
    const pins = ['cs', 'sck', 'mosi', 'miso'].map((k) => parseInt($(`emu-spi-${k}`).value, 10));
    if (new Set(pins).size !== 4) throw new Error('CS, SCK, MOSI and MISO must be four different pins');
    await S.t.emu.spi({
      mode: parseInt($('emu-spi-mode').value, 10), cs: pins[0], sck: pins[1], mosi: pins[2], miso: pins[3],
      addrMask: parseHexByte($('emu-spi-amask').value), readMask: parseHexByte($('emu-spi-rmask').value),
      readValue: parseHexByte($('emu-spi-rval').value), status: parseHexByte($('emu-spi-status').value),
    });
    await refreshEmuStatus();
  }));
  $('btn-emu-stop').addEventListener('click', withBusy($('btn-emu-stop'), async () => {
    await S.t.emu.stop();
    await refreshEmuStatus();
  }));
  $('btn-emu-set').addEventListener('click', withBusy($('btn-emu-set'), async () => {
    const data = parseHexBytes($('emu-set-data').value);
    if (!data.length) throw new Error('no bytes to set');
    await S.t.emu.setRegs(parseHexByte($('emu-set-reg').value), data);
    await readEmuRegs();
  }));
  $('btn-emu-ro').addEventListener('click', withBusy($('btn-emu-ro'), async () => {
    const regs = Array.from(parseHexBytes($('emu-ro').value));
    await S.t.emu.readOnly(regs);
    S.emuRo = new Set(regs);
    await readEmuRegs();
  }));
  $('btn-emu-read').addEventListener('click', withBusy($('btn-emu-read'), readEmuRegs));
  $('btn-emu-log-clear').addEventListener('click', () => { $('emu-log').innerHTML = ''; S.emuLogCount = 0; });
  buildEmuGrid();
}

function buildEmuGrid() {
  const g = $('emu-regs');
  g.innerHTML = '<span class="reg-h"></span>' +
    Array.from({ length: 16 }, (_, i) => `<span class="reg-h">${i.toString(16).toUpperCase()}</span>`).join('');
  for (let row = 0; row < 16; row++) {
    g.insertAdjacentHTML('beforeend', `<span class="reg-h">${(row * 16).toString(16).padStart(2, '0').toUpperCase()}</span>`);
    for (let col = 0; col < 16; col++) {
      const r = row * 16 + col;
      const cell = document.createElement('span');
      cell.className = 'reg';
      cell.id = `emu-reg-${r}`;
      cell.textContent = '--';
      cell.title = `register 0x${r.toString(16).padStart(2, '0')}`;
      cell.addEventListener('click', () => {
        $('emu-set-reg').value = r.toString(16).padStart(2, '0');
        $('emu-set-data').value = cell.textContent === '--' ? '' : cell.textContent;
      });
      g.appendChild(cell);
    }
  }
}

async function readEmuRegs() {
  const regs = await S.t.emu.regs(0, 256);
  for (let r = 0; r < 256; r++) {
    const cell = $(`emu-reg-${r}`);
    cell.textContent = regs[r].toString(16).padStart(2, '0');
    cell.classList.toggle('changed', !!S.emuRegs && S.emuRegs[r] !== regs[r]);
    cell.classList.toggle('ro', S.emuRo.has(r));
  }
  S.emuRegs = regs;
}

async function refreshEmuStatus() {
  const st = await S.t.emu.status();
  $('emu-st-bus').textContent = st.bus;
  $('emu-st-txns').textContent = st.transfers;
  $('emu-st-rw').textContent = `${st.bytesRead} / ${st.bytesWritten}`;
  $('emu-st-naks').textContent = st.naks;
}

function appendEmuLog(ev) {
  const log = $('emu-log');
  const row = document.createElement('div');
  row.className = 'sniff-msg' + (ev.nak ? ' overrun' : '');
  row.innerHTML = `<span class="no">#${++S.emuLogCount}</span>` +
    `<code>${escapeHtml(T.formatEmu(ev))}</code>` +
    `<span class="meta">${(ev.tUs / 1e6).toFixed(6)} s</span>`;
  log.prepend(row);
  while (log.childNodes.length > 500) log.removeChild(log.lastChild);
}

/* --- SEQ tab ---------------------------------------------------------------------- */

const SEQ_EXAMPLE = `# reset pulse on DIO3, then wait for DIO4 and read a sensor
dio_dir 3 out
dio_write 3 0
delay_us 100
mark 1
dio_write 3 1
wait_dio 4 0 500000 ?
i2c_write_read 76 d0 read 1 ?
loop 3
  adc_read 0
  delay_ms 1
end_loop
dio_dir 3 in
`;

function initSeq() {
  let saved = null;
  try { saved = localStorage.getItem('tramuntana.seq'); } catch (e) { /* no storage */ }
  $('seq-text').value = saved || SEQ_EXAMPLE;
  $('seq-text').addEventListener('input', () => {
    try { localStorage.setItem('tramuntana.seq', $('seq-text').value); } catch (e) { /* ignore */ }
  });
  $('btn-seq-run').addEventListener('click', withBusy($('btn-seq-run'), async () => {
    const seq = T.Sequence.parse($('seq-text').value);
    const timeoutMs = Math.max(1, parseFloat($('seq-timeout').value) || 10) * 1000;
    $('seq-state').textContent = 'running...';
    $('seq-result').innerHTML = '';
    if (S.timeline) S.timeline.note(`sequence run (${seq.code.length} bytes)`);
    const r = await S.t.seq.run(seq, timeoutMs);
    S.lastSeq = r;
    $('seq-state').textContent = r.state + (r.error ? `: ${r.error} at "${seq.labels.get(r.pc) || r.pc}"` : '');
    const rows = r.steps.map((st) => `<tr class="${st.ok ? '' : 'fail'}"><td>${st.dtUs}</td><td>${escapeHtml(st.label)}</td>` +
      `<td>${st.ok ? 'ok' : escapeHtml(st.status)}</td><td>${st.value === null ? '' : escapeHtml(String(st.value))}</td></tr>`);
    $('seq-result').innerHTML = rows.join('');
  }));
  $('btn-seq-abort').addEventListener('click', withBusy($('btn-seq-abort'), async () => { await S.t.seq.abort(); }));
}

function escapeHtml(s) {
  return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

/* --- TIMELINE tab ----------------------------------------------------------------- */

const TL_SOURCES = ['uart', 'uart_tx', 'i2c', 'spi', 'dio', 'emu', 'line', 'seq', 'capture', 'host', 'mark'];

function initTimeline() {
  const watch = $('tl-watch');
  for (let d = 0; d < 8; d++) {
    watch.insertAdjacentHTML('beforeend', `
      <div class="field" style="min-width:90px;max-width:120px">
        <label class="switch sw-green"><input type="checkbox" id="tl-w-${d}"><span>DIO${d}</span></label>
        <input type="text" id="tl-n-${d}" placeholder="name">
      </div>`);
  }
  const srcs = $('tl-sources');
  srcs.insertAdjacentHTML('beforeend', '<span class="hint">show:</span>');
  for (const s of TL_SOURCES) {
    srcs.insertAdjacentHTML('beforeend', `<label class="switch"><input type="checkbox" id="tl-s-${s}" checked><span>${s}</span></label>`);
    $(`tl-s-${s}`).addEventListener('change', renderTimeline);
  }
  $('tl-filter').addEventListener('input', renderTimeline);

  $('btn-tl-start').addEventListener('click', withBusy($('btn-tl-start'), async () => {
    const tl = new T.Timeline(S.t);
    let mask = 0;
    for (let d = 0; d < 8; d++) {
      if ($(`tl-w-${d}`).checked) mask |= 1 << d;
      const name = $(`tl-n-${d}`).value.trim();
      if (name) tl.dioNames[d] = name;
    }
    tl.onChange = scheduleTimelineRender;
    await tl.start(mask);
    S.timeline = tl;
    $('btn-tl-start').disabled = true;
    $('btn-tl-stop').disabled = false;
    $('tl-state').textContent = `recording${mask ? `, watching 0x${mask.toString(16).padStart(2, '0')}` : ''}`;
  }));
  $('btn-tl-stop').addEventListener('click', withBusy($('btn-tl-stop'), stopTimeline));
  $('btn-tl-clear').addEventListener('click', () => { if (S.timeline) { S.timeline.entries = []; renderTimeline(); } });
  $('btn-tl-mark').addEventListener('click', () => {
    if (!S.timeline || !S.timeline.running) { showError('start the timeline first'); return; }
    S.timeline.mark($('tl-mark').value || 'mark');
    $('tl-mark').value = '';
  });
  $('btn-tl-save').addEventListener('click', () => {
    if (!S.timeline) return;
    const text = S.timeline.entries.map(T.Timeline.format).join('\n') + '\n';
    T.downloadBlob(new Blob([text], { type: 'text/plain' }), 'tramuntana-timeline.txt');
  });
}

async function stopTimeline() {
  if (S.timeline) await S.timeline.stop();
  $('btn-tl-start').disabled = false;
  $('btn-tl-stop').disabled = true;
  $('tl-state').textContent = S.timeline ? `stopped, ${S.timeline.entries.length} entries` : 'stopped';
}

function scheduleTimelineRender() {
  if (scheduleTimelineRender._p) return;
  scheduleTimelineRender._p = requestAnimationFrame(() => { scheduleTimelineRender._p = null; renderTimeline(); });
}

function renderTimeline() {
  if (!S.timeline) return;
  const sources = new Set(TL_SOURCES.filter((s) => $(`tl-s-${s}`).checked));
  const entries = S.timeline.select({ sources, contains: $('tl-filter').value.trim() }).slice(-2000);
  const out = $('tl-out');
  const atBottom = out.scrollTop + out.clientHeight >= out.scrollHeight - 20;
  out.innerHTML = entries.map((e) => `<span class="src-${e.source}">${escapeHtml(T.Timeline.format(e))}</span>`).join('\n');
  if (atBottom) out.scrollTop = out.scrollHeight;
}

function termAppend(text, cls) {
  const out = $('uart-term-out');
  const node = cls ? Object.assign(document.createElement('span'), { className: cls, textContent: text })
                   : document.createTextNode(text);
  out.appendChild(node);
  out.scrollTop = out.scrollHeight;
  while (out.childNodes.length > 4000) out.removeChild(out.firstChild);
}

/* --- boot ------------------------------------------------------------------------ */

function init() {
  if (!('serial' in navigator)) $('unsupported').hidden = false;
  $('btn-connect').addEventListener('click', withBusy($('btn-connect'), connect));
  $('btn-disconnect').addEventListener('click', withBusy($('btn-disconnect'), disconnect));
  initTabs();
  initSystem();
  initSpi();
  initI2c();
  initScope();
  initAwg();
  initPwm();
  initLogic();
  initUart();
  initEmu();
  initSeq();
  initTimeline();

  if ('serial' in navigator) {
    navigator.serial.addEventListener('disconnect', (e) => {
      if (S.t && S.t.link && S.t.link.port === e.target) disconnect();
      if (S.uartTerm.port === e.target) uartTermDisconnect();
    });
  }
}

document.addEventListener('DOMContentLoaded', init);
