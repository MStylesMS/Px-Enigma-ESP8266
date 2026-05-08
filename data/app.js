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
    if (el.type === 'checkbox') { el.checked = !!v; }
    else if (el.tagName === 'SELECT') { el.value = String(v); }
    else { el.value = v; }
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
    const r = await fetch('/api/config');
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
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) { status.textContent = 'error: ' + (j.error || r.status); return; }
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
// Bit layout (matches firmware hardware_io / config.h):
//   bit = col_index * rows + row_index   (0-based)
//   switch number = bit + 1
// The grid uses grid-auto-flow:column so iterating bits 0..N in order
// places SW1-SWrows in column 1, SW(rows+1)..SW(2*rows) in column 2, etc.

let g_sw_layout = { cols: 4, rows: 5, switch_count: 20,
                    rotation: 0, mirror_x: false, mirror_y: false };

async function loadSwitchLayout() {
  try {
    const r = await fetch('/switch_layout.json');
    if (r.ok) Object.assign(g_sw_layout, await r.json());
  } catch (e) { /* use defaults */ }
  initSwitchGrid();
}

function initSwitchGrid() {
  const { cols, rows, switch_count, rotation, mirror_x, mirror_y } = g_sw_layout;
  const cellPx = 36, gapPx = 6;
  const gridW = cols * cellPx + (cols - 1) * gapPx;
  const gridH = rows * cellPx + (rows - 1) * gapPx;

  const grid = $('#switch-grid');
  grid.style.gridTemplateColumns = `repeat(${cols}, ${cellPx}px)`;
  grid.style.gridTemplateRows    = `repeat(${rows}, ${cellPx}px)`;
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
  const total = cols * rows;
  for (let bit = 0; bit < total; bit++) {
    const swNum = bit + 1;
    const cell = document.createElement('div');
    const inactive = swNum > switch_count;
    cell.className = 'sw-cell' + (inactive ? ' sw-inactive' : '');
    cell.dataset.bit = bit;
    if (!inactive) cell.textContent = swNum;
    grid.appendChild(cell);
  }
}

function updateSwitchGrid(code_bits) {
  if (code_bits == null) return;
  document.querySelectorAll('#switch-grid .sw-cell:not(.sw-inactive)').forEach(cell => {
    const on = (code_bits >>> parseInt(cell.dataset.bit)) & 1;
    cell.classList.toggle('sw-on', !!on);
  });
}

// ---- state ----
async function loadState() {
  try {
    const r = await fetch('/api/state');
    if (!r.ok) { $('#state-raw').textContent = 'state ' + r.status; return; }
    const s = await r.json();
    $('#state-raw').textContent = JSON.stringify(s, null, 2);

    // Summary row updates
    const set = (id, val) => { const el = $(id); if (el) el.textContent = val; };
    set('#s-status',  s.status || '—');
    set('#s-code',    (s.code && s.code.code) || '—');
    set('#s-solved',  (s.code && s.code.solved != null) ? String(s.code.solved) : '—');
    if (s.code) updateSwitchGrid(s.code.code_bits);
    if (s.wifi) {
      const sta = s.wifi.sta || {};
      set('#s-sta',  sta.connected ? (sta.ssid + ' ' + sta.ip) : 'disconnected');
      set('#s-rssi', sta.rssi != null ? sta.rssi + ' dBm' : '—');
      const ap = s.wifi.ap || {};
      set('#s-ap',   (ap.ssid || '—') + ' — ' + (ap.clients || 0) + ' client(s)');
      if (ap.ssid) $('#ap-ssid-ro').value = ap.ssid;
    }
    if (s.mqtt) {
      set('#s-mqtt', s.mqtt.connected ? ('connected to ' + s.mqtt.broker) : 'disconnected');
    }
    if (s.battery) {
      const pct = s.battery.percent != null ? s.battery.percent + '%' : '';
      const v   = s.battery.voltage_v != null ? ' ' + s.battery.voltage_v + 'V' : '';
      set('#s-batt', (s.battery.profile || '—') + (pct ? ' ' + pct : '') + v);
    }
    if (s.health) {
      const free = s.health.free_heap_bytes;
      set('#s-heap', free != null ? Math.round(free / 1024) + ' kB' : '—');
    }
    const uptime = s.uptime_s;
    if (uptime != null) {
      const h = Math.floor(uptime / 3600), m = Math.floor((uptime % 3600) / 60), sec = uptime % 60;
      set('#s-uptime', h + 'h ' + m + 'm ' + sec + 's');
    }
  } catch (e) {
    $('#state-raw').textContent = 'error: ' + e.message;
  }
}

// ---- log ----
async function loadLog() {
  try {
    const r = await fetch('/api/log');
    if (!r.ok) { $('#log-out').textContent = 'log ' + r.status; return; }
    const lines = await r.json();
    $('#log-out').textContent = Array.isArray(lines) ? lines.join('\n') : JSON.stringify(lines);
  } catch (e) {
    $('#log-out').textContent = 'error: ' + e.message;
  }
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

  $('#cfg-form').addEventListener('submit', saveConfig);
  $('#refresh').addEventListener('click', () => { loadState(); loadLog(); });
  $('#refresh-log').addEventListener('click', loadLog);

  $('#identify').addEventListener('click', async () => {
    const j = await post('/api/identify');
    showBanner(j.ok ? 'Identify triggered' : ('identify: ' + (j.error || 'error')));
  });

  $('#restart').addEventListener('click', async () => {
    if (!confirm('Restart the device?')) return;
    await post('/api/restart');
    showBanner('Restarting…');
    setTimeout(() => window.location.reload(), 6000);
  });

  $('#reset-puzzle').addEventListener('click', async () => {
    const j = await post('/api/reset');
    showBanner(j.ok ? 'Puzzle reset' : ('reset: ' + (j.error || 'error')));
  });

  $('#factory-reset').addEventListener('click', async () => {
    if (!confirm('Factory reset will wipe config and reboot. Continue?')) return;
    await post('/api/config/reset');
    showBanner('Factory reset — rebooting…');
    setTimeout(() => window.location.reload(), 7000);
  });

  // Auto-refresh state every 10 s
  setInterval(loadState, 10000);
});
