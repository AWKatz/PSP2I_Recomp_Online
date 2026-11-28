// PSP2i server admin panel. Plain JS; all dynamic text goes in through
// textContent (player names and audit details are untrusted).
'use strict';

const $ = (id) => document.getElementById(id);
const RATES = [
  ['exp_rate', 'Experience'], ['drop_rate', 'Drop rate'], ['rare_drop_rate', 'Rare drop rate'],
  ['meseta_rate', 'Meseta'], ['type_points_rate', 'Type points'],
];
let token = sessionStorage.getItem('psp2i_token') || '';
let user = sessionStorage.getItem('psp2i_user') || '';
let stream = null;
let last = null;
let dropTables = '';

// ---- helpers --------------------------------------------------------------
function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v);
  }
  for (const k of kids) e.append(k instanceof Node ? k : document.createTextNode(k == null ? '' : String(k)));
  return e;
}
function toast(text, bad = false) {
  const t = $('toast');
  t.textContent = text;
  t.className = 'toast show' + (bad ? ' bad' : '');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.className = 'toast' + (bad ? ' bad' : ''); }, 3500);
}
function dur(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : m ? `${m}m ${s % 60}s` : `${s}s`;
}
function when(t) { return t ? new Date(t * 1000).toLocaleString() : 'never'; }

async function api(method, path, body, raw = false) {
  const opt = { method, headers: { Authorization: 'Bearer ' + token } };
  if (body !== undefined) {
    opt.body = raw ? body : JSON.stringify(body);
    opt.headers['Content-Type'] = raw ? 'application/json' : 'application/json';
  }
  const r = await fetch('/api/v1/' + path, opt);
  if (r.status === 401 && path !== 'login') { signOut(); throw new Error('session expired: sign in again'); }
  const ct = r.headers.get('Content-Type') || '';
  const data = ct.includes('json') ? await r.json() : await r.text();
  if (!r.ok) throw new Error((data && data.message) || r.statusText);
  return data;
}
async function act(promise, quiet = false) {
  try {
    const r = await promise;
    if (!quiet && r && r.message) toast(r.message);
    return r;
  } catch (e) { toast(e.message, true); return null; }
}

// dialog: fields = [{id, label, type, value, placeholder}]
function ask(title, text, fields, okLabel = 'OK', danger = true) {
  return new Promise((resolve) => {
    const d = $('dlg');
    $('dlg-title').textContent = title;
    $('dlg-text').textContent = text || '';
    const box = $('dlg-fields');
    box.replaceChildren();
    for (const f of fields) {
      box.append(el('label', {}, f.label, el('input', { id: 'f-' + f.id, type: f.type || 'text', value: f.value ?? '', placeholder: f.placeholder || '' })));
    }
    const ok = $('dlg-ok');
    ok.textContent = okLabel;
    ok.className = danger ? 'danger' : 'primary';
    d.onclose = () => {
      if (d.returnValue !== 'ok') return resolve(null);
      const out = {};
      for (const f of fields) out[f.id] = $('f-' + f.id).value;
      resolve(out);
    };
    d.returnValue = '';
    d.showModal();
  });
}

// ---- sign-in ----------------------------------------------------------------
function signOut() {
  if (stream) { stream.close(); stream = null; }
  token = ''; user = '';
  sessionStorage.removeItem('psp2i_token'); sessionStorage.removeItem('psp2i_user');
  $('app').hidden = true; $('login').hidden = false;
}
$('login-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('login-error').textContent = '';
  try {
    const r = await api('POST', 'login', { username: $('login-user').value, password: $('login-pass').value });
    token = r.token; user = r.user;
    sessionStorage.setItem('psp2i_token', token); sessionStorage.setItem('psp2i_user', user);
    $('login-pass').value = '';
    start();
  } catch (err) { $('login-error').textContent = err.message; }
});
$('logout').addEventListener('click', async () => { await act(api('POST', 'logout'), true); signOut(); });

// ---- tabs -------------------------------------------------------------------
const loaders = { mod: loadModeration, rates: () => last && renderRates(last), drops: loadDrops, audit: loadAudit };
$('tabs').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-tab]');
  if (!b) return;
  for (const x of document.querySelectorAll('#tabs button')) x.classList.toggle('active', x === b);
  for (const s of document.querySelectorAll('.tab')) s.hidden = s.id !== 'tab-' + b.dataset.tab;
  (loaders[b.dataset.tab] || (() => {}))();
});

// ---- live telemetry -----------------------------------------------------------
function start() {
  $('login').hidden = true; $('app').hidden = false;
  $('who').textContent = user;
  if (stream) stream.close();
  stream = new EventSource('/api/v1/stream?token=' + encodeURIComponent(token));
  stream.onmessage = (m) => { last = JSON.parse(m.data); render(last); $('live-dot').classList.add('live'); };
  stream.onerror = () => { $('live-dot').classList.remove('live'); };
  api('GET', 'status').then(render).catch(() => {});
}

function render(s) {
  last = s;
  $('srv-name').textContent = s.name;
  $('srv-mode').textContent = s.mode === 'online' ? 'Online' : 'LAN';
  $('s-players').textContent = `${s.players.length} / ${s.max_players}`;
  $('s-hz').textContent = s.tick_hz.toFixed(1) + ' Hz';
  $('s-tick').textContent = `tick ${s.tick_avg_ms.toFixed(3)} ms avg, ${s.tick_max_ms.toFixed(2)} max · ${s.late_ticks} late`;
  $('s-cpu').textContent = s.cpu_pct.toFixed(2) + ' %';
  $('s-rss').textContent = s.rss_mb.toFixed(1) + ' MB';
  $('s-up').textContent = dur(s.uptime_s);
  $('s-drops').textContent = s.drops;
  $('s-rare').textContent = `${s.rare_drops} rare · ${s.kills} kills`;
  const body = $('players');
  body.replaceChildren(...s.players.map((p) => el('tr', {},
    el('td', { class: 'mono' }, p.session),
    el('td', {}, p.name),
    el('td', {}, p.account_id ?? '—'),
    el('td', { class: 'mono' }, p.ip),
    el('td', { class: 'mono' }, p.lan_ip || '—'),
    el('td', {}, p.ping_ms >= 0 ? p.ping_ms + ' ms' : '—'),
    el('td', {}, dur(p.online_s)),
    el('td', {}, p.location),
    el('td', { class: 'actions' },
      el('button', { class: 'sm ghost', onclick: () => kick(p) }, 'Kick'), ' ',
      el('button', { class: 'sm danger', onclick: () => ban({ target: 'session:' + p.session, label: `${p.name} (${p.ip})` }) }, 'Ban')))));
  $('players-empty').hidden = s.players.length > 0;
  if (!$('tab-rates').hidden && !document.activeElement.closest('#tab-rates')) renderRates(s);
}

async function kick(p) {
  const r = await ask(`Kick ${p.name}?`, 'They can reconnect straight away.', [{ id: 'reason', label: 'Reason (optional)' }], 'Kick');
  if (r) await act(api('POST', 'kick', { session: p.session, reason: r.reason }));
}
async function ban({ target, label }) {
  const r = await ask(`Ban ${label}?`, 'Banned players are disconnected now and refused when they come back.',
    [{ id: 'minutes', label: 'Minutes (0 = permanent)', type: 'number', value: '0' }, { id: 'reason', label: 'Reason' }], 'Ban');
  if (!r) return;
  await act(api('POST', 'ban', { target, minutes: Number(r.minutes) || 0, reason: r.reason }));
  loadModeration();
}

// ---- moderation ---------------------------------------------------------------
async function loadModeration() {
  const q = encodeURIComponent($('acc-q').value);
  const [accounts, bans] = await Promise.all([act(api('GET', 'accounts?q=' + q), true), act(api('GET', 'bans'), true)]);
  if (accounts) {
    $('accounts').replaceChildren(...accounts.map((a) => el('tr', {},
      el('td', { class: 'mono' }, a.account_id), el('td', {}, a.username),
      el('td', {}, a.is_admin ? el('span', { class: 'badge good' }, 'admin') : ''),
      el('td', {}, a.is_banned ? el('span', { class: 'badge bad', title: a.ban_reason || '' }, 'banned until ' + (a.ban_expires_at ? when(a.ban_expires_at) : 'forever')) : 'ok'),
      el('td', { class: 'mono' }, a.last_ip || '—'),
      el('td', { class: 'actions' },
        el('button', { class: 'sm ghost', onclick: async () => { await act(api('POST', 'admin', { account_id: a.account_id, admin: !a.is_admin })); loadModeration(); } }, a.is_admin ? 'Demote' : 'Promote'), ' ',
        a.is_banned
          ? el('button', { class: 'sm ghost', onclick: async () => { await act(api('POST', 'unban', { target: 'account:' + a.account_id })); loadModeration(); } }, 'Unban')
          : el('button', { class: 'sm danger', onclick: () => ban({ target: 'account:' + a.account_id, label: a.username }) }, 'Ban')))));
  }
  if (bans) {
    $('ipbans').replaceChildren(...bans.map((b) => el('tr', {},
      el('td', { class: 'mono' }, b.ip), el('td', {}, b.reason || ''), el('td', {}, b.expires_at ? when(b.expires_at) : 'forever'),
      el('td', {}, b.admin || ''),
      el('td', { class: 'actions' }, el('button', { class: 'sm ghost', onclick: async () => { await act(api('POST', 'unban', { target: 'ip:' + b.ip })); loadModeration(); } }, 'Unban')))));
    $('ipbans-empty').hidden = bans.length > 0;
  }
}
$('acc-q').addEventListener('input', () => { clearTimeout(loadModeration.t); loadModeration.t = setTimeout(loadModeration, 250); });
$('ipban-form').addEventListener('submit', (e) => { e.preventDefault(); const ip = $('ipban-ip').value.trim(); if (ip) ban({ target: 'ip:' + ip, label: ip }); });

// ---- rates and events -------------------------------------------------------------
function renderRates(s) {
  const rows = $('rate-rows');
  if (!rows.children.length) {
    for (const [k, label] of RATES) {
      const range = el('input', { type: 'range', min: '0', max: '10', step: '0.1', id: 'r-' + k });
      const num = el('input', { type: 'number', min: '0', max: '100', step: '0.1', id: 'n-' + k });
      range.addEventListener('input', () => { num.value = range.value; });
      num.addEventListener('input', () => { range.value = num.value; });
      rows.append(el('div', { class: 'rate' }, el('label', { for: 'n-' + k }, label), range, num));
    }
  }
  for (const [k] of RATES) { $('r-' + k).value = s.rates[k]; $('n-' + k).value = s.rates[k]; }
  const ev = $('event-rows');
  ev.replaceChildren(...Object.entries(s.events).map(([name, on]) => el('div', { class: 'toggle' },
    el('span', {}, name.replace('_', ' ').replace(/^./, (c) => c.toUpperCase())),
    el('button', { class: 'switch' + (on ? ' on' : ''), 'aria-pressed': String(on), 'aria-label': name,
      onclick: async () => { await act(api('POST', 'events', { name, on: !on })); } }))));
}
$('rates-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = {};
  for (const [k] of RATES) {
    const v = Number($('n-' + k).value);
    if (last && v !== last.rates[k]) body[k] = v;
  }
  if (!Object.keys(body).length) { toast('No changes'); return; }
  await act(api('POST', 'rates', body));
});

// ---- drop tables -------------------------------------------------------------------
async function loadDrops() {
  const r = await act(api('GET', 'drop_tables'), true);
  if (r == null) return;
  dropTables = typeof r === 'string' ? r : JSON.stringify(r, null, 2);
  $('drop-json').value = dropTables;
  $('drop-msg').textContent = ''; $('drop-msg').className = 'note';
  renderDropTable();
}
function renderDropTable() {
  let t;
  try { t = JSON.parse($('drop-json').value); } catch { return; }
  const q = $('drop-q').value.toLowerCase();
  const rows = (t.enemies || []).filter((e) => !q || JSON.stringify(e).toLowerCase().includes(q));
  $('drop-table').replaceChildren(...rows.map((e) => el('tr', {},
    el('td', { class: 'mono' }, e.id), el('td', {}, e.name || ''), el('td', {}, e.exp ?? 0),
    el('td', {}, ((e.drop_chance || 0) * 100).toFixed(1) + ' %'),
    el('td', {}, (e.items || []).map((i) => i.meseta ? `Meseta ×${i.weight}` : `${i.name || i.id} ×${i.weight}`).join(', ')),
    el('td', {}, (e.rares || []).map((r) => `${r.name || r.id} ${(r.chance * 100).toFixed(2)}%`).join(', ')))));
}
$('drop-q').addEventListener('input', renderDropTable);
$('drop-json').addEventListener('input', () => { clearTimeout(renderDropTable.t); renderDropTable.t = setTimeout(renderDropTable, 300); });
$('drop-reload').addEventListener('click', loadDrops);
$('drop-apply').addEventListener('click', async () => {
  const m = $('drop-msg');
  try {
    const r = await api('POST', 'drop_tables', $('drop-json').value, true);
    m.textContent = r.message; m.className = 'note ok';
    dropTables = $('drop-json').value;
    renderDropTable();
  } catch (e) { m.textContent = e.message; m.className = 'note bad'; }
});

// ---- broadcast -----------------------------------------------------------------------
function preview() { const p = $('ann-preview'); p.textContent = $('ann-text').value || 'Your announcement'; p.style.color = $('ann-color').value; }
$('ann-text').addEventListener('input', preview);
$('ann-color').addEventListener('input', preview);
$('announce-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const r = await act(api('POST', 'announce', { text: $('ann-text').value, color: $('ann-color').value }));
  if (r) $('ann-text').value = '';
  preview();
});

// ---- audit -------------------------------------------------------------------------------
async function loadAudit() {
  const rows = await act(api('GET', 'audit?limit=200&q=' + encodeURIComponent($('audit-q').value)), true);
  if (!rows) return;
  $('audit').replaceChildren(...rows.map((a) => el('tr', {},
    el('td', {}, when(a.time)), el('td', {}, a.admin), el('td', {}, el('span', { class: 'badge' }, a.action)),
    el('td', { class: 'mono' }, a.target || ''), el('td', { class: 'details' }, a.details ? JSON.stringify(a.details) : ''))));
}
$('audit-q').addEventListener('input', () => { clearTimeout(loadAudit.t); loadAudit.t = setTimeout(loadAudit, 250); });

// ---- console ---------------------------------------------------------------------------------
$('console-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const cmd = $('console-in').value.trim();
  if (!cmd) return;
  const out = $('console-out');
  out.append(el('span', { class: 'muted' }, '> ' + cmd + '\n'));
  $('console-in').value = '';
  try { const r = await api('POST', 'rcon', { command: cmd }); out.append((r.message || '') + '\n'); }
  catch (err) { out.append(el('span', { class: 'error' }, err.message + '\n')); }
  out.scrollTop = out.scrollHeight;
});

// ---- boot ---------------------------------------------------------------------------------------
preview();
if (token) start(); else { $('login').hidden = false; }
