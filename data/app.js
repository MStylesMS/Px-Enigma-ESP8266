// app.js — vanilla JS for the px-enigma Web UI.
'use strict';

const $ = (sel) => document.querySelector(sel);

// ---- nested object helpers ----
function setNested(obj, path, val) {
  const parts = path.split('.');
  let o = obj;
  for (let i = 0; i < parts.length - 1; i++) {
    o[parts[i]] = o[parts[i]] || {};
    o = o[parts[i]];
  }
  o[parts[parts.length - 1]] = val;
}
function getNested(obj, path) {
  const parts = path.split('.');
  let o = obj;
  for (const p of parts) { if (o == null) return undefined; o = o[p]; }
  return o;
}

// ---- banner ----
function showBanner(msg, type) {
  const b = $('#banner');
  b.textContent = msg;
  b.className = type || 'warn';
  setTimeout(() => { b.className = 'hidden'; }, 5000);
}

// ---- config form ----
function fillForm(cfg) {
  document.querySelectorAll('#cfg-form [name]').forEach(el => {
    const v = getNested(cfg, el.name);
    if (v === undefined) return;
    if (el.type === 'checkbox') {
      el.checked = !!v;
    } else if (el.type === 'password') {
      // Server returns masked dots for configured secrets.
      el.value = '';
      el.placeholder = String(v || '');
    } else if (el.tagName === 'SELECT') {
      el.value = String(v);
    } else {
      el.value = v;
    }
    if (el.type === 'range') {
      const sp = el.parentElement.querySelector('span');
      if (sp) sp.textContent = el.value;
    }
  });
  const name = cfg.device && cfg.device.prop_name;
  document.title = (name ? name + ' — px-enigma' : 'px-enigma');
  $('#title').textContent = name || 'px-enigma';
}

function readForm() {
  const out = {};
  document.querySelectorAll('#cfg-form [name]').forEach(el => {
    if (el.disabled) return;

    if (el.type === 'password') {
      // Blank or whitespace-only input means "keep existing secret".
      if (typeof el.value === 'string' && el.value.trim() === '') return;
    }

    let v;
    if (el.type === 'checkbox') v = el.checked;
    else if (el.type === 'number' || el.type === 'range') v = Number(el.value);
    else v = el.value;
    // puzzle.target: blank string → send null so the firmware stores null
    if (el.name === 'puzzle.target' && v === '') {
      setNested(out, el.name, null);
      return;
    }
    setNested(out, el.name, v);
  });
  return out;
}

async function loadConfig() {
  try {
    const r = await fetch('api/config');
    if (!r.ok) throw new Error('config ' + r.status);
    const cfg = await r.json();
    fillForm(cfg);
    // Populate read-only AP SSID from current wifi object
    if (cfg.wifi && cfg.wifi.ap) {
      // The AP SSID is derived server-side from device.prop_name; show it from state
    }
  } catch (e) {
    showBanner('Failed to load config: ' + e.message);
  }
}

async function saveConfig(ev) {
  ev.preventDefault();
  const status = $('#save-status');
  status.textContent = 'saving…';
  try {
    const body = JSON.stringify(readForm());
    const r = await fetch('api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) { status.textContent = 'error: ' + (j.error || r.status); return; }
    await loadState();
    status.textContent = j.reboot_required ? 'saved — rebooting…' : 'saved ✓';
    if (j.reboot_required) {
      setTimeout(() => { status.textContent = ''; loadConfig(); }, 6000);
    } else {
      setTimeout(() => { status.textContent = ''; }, 3000);
    }
  } catch (e) {
    status.textContent = 'error: ' + e.message;
  }
}

// ---- switch grid ----
let g_sw_layout = {
  cols: 4,
  rows: 5,
  ui_cols: 5,
  ui_rows: 4,
  switch_count: 20,
  rotation: 0,
  mirror_x: false,
  mirror_y: false,
  prop_row_to_scan_col: [1, 0, 2, 3],
  prop_col_to_scan_row: [3, 2, 0, 4, 1],
  row_gpios: [12, 3, 14, 4, 13],
  col_gpios: [15, 1, 5, 16],
  row_gpio_to_a: { '12': 3, '3': 2, '14': 0, '4': 4, '13': 1 },
  col_gpio_to_b: { '15': 3, '1': 2, '5': 0, '16': 1 },
  digit_order: [4, 2, 6, 1, 5, 3]
};

// Invert digit_order to find the raw code_bits value that would produce tgtInt
// after the firmware's ordered_code_int() reordering.
// digit_order semantics: display[i] = raw[order[i]-1]
// inverse:              raw[order[i]-1] = display[i]
function rawBitsForTarget(tgtInt) {
  const ord = Array.isArray(g_sw_layout.digit_order) ? g_sw_layout.digit_order : [1, 2, 3, 4, 5, 6];
  if (ord.length !== 6) return tgtInt;
  const tgtStr = String(Math.round(tgtInt)).padStart(6, '0');
  const raw = new Array(6);
  for (let i = 0; i < 6; i++) raw[ord[i] - 1] = tgtStr[i];
  return parseInt(raw.join(''), 10);
}

function parseCodeInt(codeStr) {
  if (typeof codeStr !== 'string') return null;
  const digits = codeStr.replace(/[^0-9]/g, '');
  if (digits.length === 0 || digits.length > 6) return null;
  const n = Number(digits);
  if (!Number.isInteger(n) || n < 0 || n > 999999) return null;
  return n;
}

function puzzleMask() {
  const bits = g_sw_layout.rows * g_sw_layout.cols;
  if (bits <= 0 || bits > 20) return (1 << 20) - 1;
  return (1 << bits) - 1;
}

function setModeBadge(mode) {
  const el = $('#switch-mode');
  if (!el) return;
  const m = String(mode || '').toLowerCase();
  if (m === 'live') el.textContent = 'Mode: Live';
  else if (m === 'latching') el.textContent = 'Mode: Latching';
  else el.textContent = 'Mode: —';
}

function formatCodeForUi(codeStr) {
  if (typeof codeStr !== 'string') return codeStr;
  const digits = codeStr.replace(/[^0-9]/g, '');
  if (digits.length !== 6) return codeStr;

  const ord = Array.isArray(g_sw_layout.digit_order) ? g_sw_layout.digit_order : [1, 2, 3, 4, 5, 6];
  if (ord.length !== 6) return codeStr;

  const seen = new Set();
  for (const x of ord) {
    if (!Number.isInteger(x) || x < 1 || x > 6 || seen.has(x)) return codeStr;
    seen.add(x);
  }

  const remap = ord.map((p) => digits[p - 1]).join('');
  return remap.slice(0, 2) + '-' + remap.slice(2, 4) + '-' + remap.slice(4, 6);
}

async function loadSwitchLayout() {
  try {
    const r = await fetch('switch_layout.json');
    if (r.ok) Object.assign(g_sw_layout, await r.json());
  } catch (e) { /* use defaults */ }
  initSwitchGrid();
}

function initSwitchGrid() {
  const { cols, rows, switch_count, rotation, mirror_x, mirror_y } = g_sw_layout;
  const uiCols = Number.isInteger(g_sw_layout.ui_cols) ? g_sw_layout.ui_cols : cols;
  const uiRows = Number.isInteger(g_sw_layout.ui_rows) ? g_sw_layout.ui_rows : rows;
  const cellPx = 36, gapPx = 6;
  const gridW = uiCols * cellPx + (uiCols - 1) * gapPx;
  const gridH = uiRows * cellPx + (uiRows - 1) * gapPx;

  const grid = $('#switch-grid');
  grid.style.gridTemplateColumns = `repeat(${uiCols}, ${cellPx}px)`;
  grid.style.gridTemplateRows    = `repeat(${uiRows}, ${cellPx}px)`;
  grid.style.width  = gridW + 'px';
  grid.style.height = gridH + 'px';

  const xforms = [];
  if (rotation)  xforms.push(`rotate(${rotation}deg)`);
  if (mirror_x)  xforms.push('scaleX(-1)');
  if (mirror_y)  xforms.push('scaleY(-1)');
  grid.style.transform = xforms.join(' ');

  // Expand the wrapper so rotation never clips the grid.
  const diag = Math.ceil(Math.hypot(gridW, gridH)) + 16;
  $('#switch-grid-wrap').style.minHeight = diag + 'px';

  grid.innerHTML = '';
  const rowGpios = Array.isArray(g_sw_layout.row_gpios) ? g_sw_layout.row_gpios : [12, 3, 14, 4, 13];
  const colGpios = Array.isArray(g_sw_layout.col_gpios) ? g_sw_layout.col_gpios : [15, 1, 5, 16];
  const propRowToScanCol = Array.isArray(g_sw_layout.prop_row_to_scan_col)
    ? g_sw_layout.prop_row_to_scan_col : [0, 1, 2, 3];
  const propColToScanRow = Array.isArray(g_sw_layout.prop_col_to_scan_row)
    ? g_sw_layout.prop_col_to_scan_row : [0, 1, 2, 3, 4];
  const rowMap = g_sw_layout.row_gpio_to_a || {};
  const colMap = g_sw_layout.col_gpio_to_b || {};

  for (let r = 0; r < uiRows; r++) {
    for (let c = 0; c < uiCols; c++) {
      // UI uses prop orientation. Map prop row/col to scan col/row indexes.
      const scanCol = Number.isInteger(propRowToScanCol[r]) ? propRowToScanCol[r] : r;
      const scanRow = Number.isInteger(propColToScanRow[c]) ? propColToScanRow[c] : c;

      const rowGpio = String(rowGpios[scanRow]);
      const colGpio = String(colGpios[scanCol]);
      const a = Number.isInteger(rowMap[rowGpio]) ? rowMap[rowGpio] : scanRow;
      const b = Number.isInteger(colMap[colGpio]) ? colMap[colGpio] : scanCol;
      const bit = a + rows * b;

      const swNum = r * uiCols + c + 1;
      const inactive = swNum > switch_count;

      const cell = document.createElement('div');
      cell.className = 'sw-cell' + (inactive ? ' sw-inactive' : '');
      cell.dataset.bit = bit;
      if (!inactive) cell.textContent = swNum;
      grid.appendChild(cell);
    }
  }
}

function updateSwitchGrid(codeGrid, targetGrid) {
  const rows = Array.isArray(codeGrid) ? codeGrid : [];
  const tgtRows = Array.isArray(targetGrid) ? targetGrid : null;
  const uiCols = Number.isInteger(g_sw_layout.ui_cols) ? g_sw_layout.ui_cols : g_sw_layout.cols;
  const cells = document.querySelectorAll('#switch-grid .sw-cell');
  cells.forEach((cell, idx) => {
    if (cell.classList.contains('sw-inactive')) {
      cell.classList.remove('sw-on', 'sw-target');
      return;
    }
    const r = Math.floor(idx / uiCols);
    const c = idx % uiCols;
    const row = rows[r] || '';
    const tgtRow = tgtRows ? (tgtRows[r] || '') : '';
    const ch = row[c];
    const tgtCh = tgtRow[c];
    cell.classList.toggle('sw-on', ch === '1');
    cell.classList.toggle('sw-target', tgtCh === '1');
  });
}

function applyState(s) {
  $('#state-raw').textContent = JSON.stringify(s, null, 2);

  const set = (id, val) => {
    const el = $(id);
    if (el) el.textContent = val;
  };

  set('#s-status', s.status || '—');
  set('#s-solved', (s.code && s.code.solved != null) ? String(s.code.solved) : '—');

  // firmware already applies digit_order to code_str; target_str is plain (raw).
  // Both are displayed as-is: target shows the code the display will show when solved.
  const currentCode = (s.code && s.code.code) ? s.code.code : '—';
  const targetCode = (s.code && s.code.target) ? s.code.target : '—';
  set('#switch-target-code', 'Target: ' + targetCode);
  set('#switch-current-code', 'Current: ' + currentCode);

  updateSwitchGrid(
    s.code ? s.code.grid : null,
    s.code ? s.code.target_grid : null
  );

  setModeBadge(s.puzzle && s.puzzle.mode);

  if (s.wifi) {
    const sta = s.wifi.sta || {};
    set('#s-sta', sta.connected ? (sta.ssid + ' ' + sta.ip) : 'disconnected');
    set('#s-rssi', sta.rssi != null ? sta.rssi + ' dBm' : '—');
    const ap = s.wifi.ap || {};
    set('#s-ap', (ap.ssid || '—') + ' — ' + (ap.clients || 0) + ' client(s)');
    if (ap.ssid) $('#ap-ssid-ro').value = ap.ssid;
  }

  if (s.mqtt) {
    set('#s-mqtt', s.mqtt.connected ? ('connected to ' + s.mqtt.broker) : 'disconnected');
  }

  if (s.battery) {
    const pct = s.battery.percent != null ? s.battery.percent + '%' : '';
    const v = s.battery.voltage_v != null ? ' ' + s.battery.voltage_v + 'V' : '';
    set('#s-batt', (s.battery.profile || '—') + (pct ? ' ' + pct : '') + v);
  }

  if (s.health) {
    const free = s.health.free_heap_bytes;
    set('#s-heap', free != null ? Math.round(free / 1024) + ' kB' : '—');
  }

  const uptime = s.uptime_s;
  if (uptime != null) {
    const h = Math.floor(uptime / 3600);
    const m = Math.floor((uptime % 3600) / 60);
    const sec = uptime % 60;
    set('#s-uptime', h + 'h ' + m + 'm ' + sec + 's');
  }
}

// ---- state ----
async function loadState() {
  try {
    const r = await fetch('api/state');
    if (!r.ok) {
      $('#state-raw').textContent = 'state ' + r.status;
      return;
    }
    applyState(await r.json());
  } catch (e) {
    $('#state-raw').textContent = 'error: ' + e.message;
  }
}

// ---- log ----
async function loadLog() {
  try {
    const r = await fetch('api/log');
    if (!r.ok) { $('#log-out').textContent = 'log ' + r.status; return; }
    const lines = await r.json();
    $('#log-out').textContent = Array.isArray(lines) ? lines.join('\n') : JSON.stringify(lines);
  } catch (e) {
    $('#log-out').textContent = 'error: ' + e.message;
  }
}

let g_evt = null;

function connectEvents() {
  if (!window.EventSource) return;
  if (g_evt) g_evt.close();

  const es = new EventSource('api/events');
  g_evt = es;
  es.addEventListener('state', (ev) => {
    try { applyState(JSON.parse(ev.data)); } catch (_) {}
  });
  es.addEventListener('code_changed', (ev) => {
    try { applyState(JSON.parse(ev.data)); } catch (_) {}
  });
  es.onerror = () => {
    // Polling fallback remains active.
  };
}

// ---- button actions ----
async function post(path) {
  const r = await fetch(path, { method: 'POST' });
  return r.json().catch(() => ({}));
}

// ---- wire everything up ----
document.addEventListener('DOMContentLoaded', () => {
  loadConfig();
  loadState();
  loadLog();
  loadSwitchLayout();
  connectEvents();

  $('#cfg-form').addEventListener('submit', saveConfig);
  $('#refresh').addEventListener('click', () => { loadState(); loadLog(); });
  $('#refresh-log').addEventListener('click', loadLog);

  $('#identify').addEventListener('click', async () => {
    const j = await post('api/identify');
    showBanner(j.ok ? 'Identify triggered' : ('identify: ' + (j.error || 'error')));
  });

  $('#restart').addEventListener('click', async () => {
    if (!confirm('Restart the device?')) return;
    await post('api/restart');
    showBanner('Restarting…');
    setTimeout(() => window.location.reload(), 6000);
  });

  $('#reset-puzzle').addEventListener('click', async () => {
    const j = await post('api/reset');
    showBanner(j.ok ? 'Puzzle reset' : ('reset: ' + (j.error || 'error')));
  });

  $('#factory-reset').addEventListener('click', async () => {
    if (!confirm('Factory reset will wipe config and reboot. Continue?')) return;
    await post('api/config/reset');
    showBanner('Factory reset — rebooting…');
    setTimeout(() => window.location.reload(), 7000);
  });

  // Auto-refresh state every 10 s
  setInterval(loadState, 10000);
});
