import { SdrLink, supported } from '../elrs/serial.js';
import { uidFromPhrase, toHex } from '../elrs/dk500.js';
import { ElrsLink, firmwareKind } from './link.js';
import { RxState } from './state.js';

const STORE_KEY = 'elrs-rx-phrase';
const HINTS = {
  wrong_phrase: 'Bursts seen but nothing decodes — check the binding phrase',
  wide_mode: 'TX uses Wide switch mode; set Hybrid',
  clip: 'Signal clipping — move the board further away',
  no_signal: 'No ELRS bursts seen — is the TX on?',
};

const $ = (id) => document.getElementById(id);
const el = {
  phrase: $('phrase'), set: $('set'), connect: $('connect'), remember: $('remember'), store: $('store'),
  led: $('led'), stats: $('stats'), badge: $('badge'), uidset: $('uidset'), hint: $('hint'),
  unsupported: $('unsupported'), message: $('message'),
  padL: $('padL'), padR: $('padR'), valsL: $('valsL'), valsR: $('valsR'), auxrows: $('auxrows'),
};

const state = new RxState();
let sdr = null;
let link = null;
let connected = false;
let rxOn = false;
let uid = null;
let status = { text: 'Not connected', kind: '' };

const auxBars = [];
for (let i = 0; i < 8; i++) {
  const row = document.createElement('div');
  row.className = 'auxrow unseen';
  row.innerHTML = `<span>AUX${i + 1}</span><div class="track"><div class="fill"></div></div><span class="num">-</span>`;
  el.auxrows.appendChild(row);
  auxBars.push({ row, fill: row.querySelector('.fill'), num: row.querySelector('.num') });
}

function setStatus(text, kind = '') { status = { text, kind }; }

function showMessage(text) {
  el.message.hidden = !text;
  el.message.textContent = text || '';
}

function storageGet() {
  try { return localStorage.getItem(STORE_KEY); } catch (e) { return null; }
}
function storageSet(v) {
  try {
    if (v === null) localStorage.removeItem(STORE_KEY);
    else localStorage.setItem(STORE_KEY, v);
  } catch (e) { /* storage unavailable */ }
}

async function cmd(text) {
  const reply = await link.command(text);
  if (reply.startsWith('ERR')) throw new Error(text.split(' ')[0] + ': ' + reply);
  return reply;
}

function pushUid() {
  return cmd('UID ' + toHex(uid));
}

async function startRx() {
  await cmd('MODE AUTO');
  await cmd('RX ON');
  rxOn = true;
}

function applyPhrase() {
  const phrase = el.phrase.value;
  uid = phrase ? uidFromPhrase(phrase) : null;
  state.uid = uid;
  state.resetCounters();
  el.uidset.hidden = !uid;
  if (el.remember.checked) storageSet(phrase || null);
}

async function onSet() {
  applyPhrase();
  if (!connected || !uid) return;
  try {
    await pushUid();
    if (el.store.checked) await cmd('UID SAVE');
    if (!rxOn) await startRx();
  } catch (e) {
    setStatus('Setup failed: ' + e.message, 'err');
  }
}

async function onStoreChange() {
  if (!connected || el.store.checked) return;
  try {
    await cmd('UID CLEAR');
    rxOn = false;
    if (uid) {
      await pushUid();
      await startRx();
    }
  } catch (e) {
    setStatus('UID clear failed: ' + e.message, 'err');
  }
}

async function connect() {
  el.connect.disabled = true;
  showMessage('');
  sdr = new SdrLink();
  try {
    await sdr.open(2000000);
  } catch (e) {
    sdr = null;
    el.connect.disabled = false;
    setStatus(e.name === 'NotFoundError' ? 'No port selected' : 'Open failed: ' + e.message, e.name === 'NotFoundError' ? '' : 'err');
    return;
  }
  const mine = new ElrsLink(sdr);
  link = mine;
  try {
    setStatus('Synchronizing', 'warn');
    await sdr.synchronize();
    const kind = firmwareKind(await mine.command('INFO'));
    if (kind !== 'elrs') {
      showMessage(kind === 'stock'
        ? 'This board runs stock ESP-SDR. Use the raw-I/Q viewer (../elrs/) or flash the ELRS receiver firmware (../flash/)'
        : 'Unknown firmware');
      throw new Error(kind === 'stock' ? 'stock firmware' : 'unknown firmware');
    }
    state.reset();
    applyPhrase();
    rxOn = false;
    if (uid) {
      await pushUid();
      if (el.store.checked) await cmd('UID SAVE');
      await startRx();
    }
  } catch (e) {
    setStatus('Setup failed: ' + e.message, 'err');
    link = null;
    await sdr.close();
    sdr = null;
    el.connect.disabled = false;
    return;
  }
  mine.onStream((l) => state.onLine(l));
  connected = true;
  setStatus('Connected', 'on');
  el.connect.textContent = 'Disconnect';
  el.connect.disabled = false;
  mine.pump().then(() => {
    if (link === mine && connected) disconnect('Disconnected: serial port closed');
  });
}

async function disconnect(reason) {
  if (!connected) return;
  connected = false;
  rxOn = false;
  el.connect.disabled = true;
  const l = link, s = sdr;
  link = null;
  sdr = null;
  try { await l.stop(); } catch (e) { /* port may already be gone */ }
  await s.close();
  el.connect.textContent = 'Connect';
  el.connect.disabled = false;
  setStatus(reason || 'Not connected', reason ? 'err' : '');
}

function paint(pad, vals, s, xk, yk, xn, yn, stale) {
  pad.classList.toggle('stale', stale);
  const dot = pad.firstElementChild;
  if (!s) {
    dot.style.left = '50%';
    dot.style.top = '50%';
    vals.textContent = '-';
    return;
  }
  const clamp = (v) => Math.max(0, Math.min(1, (v - 172) / (1811 - 172)));
  dot.style.left = `${clamp(s[xk]) * 100}%`;
  dot.style.top = `${(1 - clamp(s[yk])) * 100}%`;
  vals.textContent = `${xn} ${s[xk]} · ${yn} ${s[yk]}`;
}

function render() {
  const age = state.lastPacketAgeMs;
  const stale = !connected || state.stale;
  if (connected) {
    const hit = state.hitRate;
    const parts = [
      `${state.updatesPerSec.toFixed(1)} updates/s`,
      hit === null ? 'hit rate -' : `${Math.round(hit * 100)}% hit rate`,
      state.channel === null ? 'ch -' : `ch ${state.channel}`,
      age === null ? 'no packet yet' : `last packet ${(age / 1000).toFixed(1)} s ago`,
    ];
    if (!uid) parts.push('enter a binding phrase');
    el.stats.textContent = parts.join(' · ');
    el.led.className = 'led ' + (stale ? 'warn' : 'on');
    el.badge.textContent = state.badge;
  } else {
    el.stats.textContent = status.text;
    el.led.className = 'led ' + (status.kind === 'err' ? 'err' : '');
    el.badge.textContent = 'SCAN';
  }
  const hint = connected ? state.hint : null;
  el.hint.hidden = !hint;
  if (hint) el.hint.textContent = HINTS[hint];
  const s = state.sticks;
  paint(el.padL, el.valsL, s, 'yaw', 'throttle', 'yaw', 'thr', stale);
  paint(el.padR, el.valsR, s, 'roll', 'pitch', 'roll', 'pitch', stale);
  const aux = state.aux;
  auxBars.forEach((b, i) => {
    const v = aux[i + 1];
    b.row.classList.toggle('unseen', v === null);
    b.fill.style.width = v === null ? '0' : `${Math.max(0, Math.min(100, ((v - 172) / (1811 - 172)) * 100))}%`;
    b.num.textContent = v === null ? '-' : String(v);
  });
  requestAnimationFrame(render);
}

el.set.addEventListener('click', onSet);
el.phrase.addEventListener('keydown', (e) => { if (e.key === 'Enter') onSet(); });
el.remember.addEventListener('change', () => {
  storageSet(el.remember.checked ? el.phrase.value || null : null);
});
el.store.addEventListener('change', onStoreChange);
el.connect.addEventListener('click', () => (connected ? disconnect() : connect()));
window.addEventListener('pagehide', () => { if (connected) disconnect(); });

if (!supported()) {
  el.unsupported.hidden = false;
  el.connect.disabled = true;
}
const saved = storageGet();
if (saved) {
  el.phrase.value = saved;
  el.remember.checked = true;
  applyPhrase();
}
render();
