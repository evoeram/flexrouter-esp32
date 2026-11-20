// app.js - общие хелперы веб-морды (fetch, WebSocket с реконнектом, лог)
'use strict';

async function api(path, opts) {
  const r = await fetch(path, opts);
  const text = await r.text();
  try { return { ok: r.ok, status: r.status, data: JSON.parse(text) }; }
  catch { return { ok: r.ok, status: r.status, data: text }; }
}

function postJSON(path, obj) {
  return api(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(obj) });
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

function toast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg; t.style.display = 'block';
  clearTimeout(t._h); t._h = setTimeout(() => t.style.display = 'none', 2500);
}

function fmtUptime(sec) {
  const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60), s = sec % 60;
  return (d ? d + 'д ' : '') + h + ':' + String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
}

// WebSocket live-канал: onmsg(msg) получает разобранные JSON-события
function connectWS(onmsg, onstate) {
  let ws, closed = false;
  function open() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(proto + '://' + location.host + '/ws');
    ws.onopen = () => onstate && onstate(true);
    ws.onclose = () => { onstate && onstate(false); if (!closed) setTimeout(open, 2000); };
    ws.onmessage = e => {
      try { onmsg(JSON.parse(e.data)); } catch {}
    };
  }
  open();
  return () => { closed = true; ws && ws.close(); };
}

// Общая шапка статуса (index/config/debug/admin)
function bindStatus(id) {
  const el = document.getElementById(id);
  return function render(st) {
    const modeBadge = st.mode === 'sta' ? '<span class="badge ok">STA</span>'
      : st.mode === 'ap' ? '<span class="badge warn">AP (настройка Wi-Fi)</span>'
      : '<span class="badge">' + esc(st.mode) + '</span>';
    el.innerHTML =
      '<b>' + esc(st.name) + '</b> <span class="mono">v' + esc(st.fw) + '</span> ' + modeBadge +
      ' &nbsp;|&nbsp; SSID: ' + esc(st.ssid || '-') +
      ' &nbsp;|&nbsp; IP: <a href="http://' + esc(st.ip) + '" class="mono">' + esc(st.ip || '-') + '</a>' +
      ' &nbsp;|&nbsp; RSSI: ' + esc(st.rssi) + ' dBm' +
      ' &nbsp;|&nbsp; heap: ' + Math.round(st.heap / 1024) + 'K' +
      ' &nbsp;|&nbsp; uptime: ' + fmtUptime(st.uptime) +
      ' &nbsp;|&nbsp; ARCC ' + esc(st.transport) + (st.host ? ' <span class="badge ok">host</span>' : ' <span class="badge warn">no host</span>');
  };
}
