import { SdrLink, supported } from './serial.js';
import { uidFromPhrase, toHex } from './dk500.js';

const FREQ_MHZ = 2436;
const RATE_CODE = 6;
const SAMPLES = 16380;
const STALE_MS = 2000;
const WINDOW_MS = 10000;
const STORE_KEY = 'elrs-viewer-phrase';

const $ = (id) => document.getElementById(id);
const el = {
  phrase: $('phrase'), set: $('set'), connect: $('connect'), remember: $('remember'),
  led: $('led'), stats: $('stats'), uid: $('uid'), unsupported: $('unsupported'),
  padL: $('padL'), padR: $('padR'), valsL: $('valsL'), valsR: $('valsR'), auxrows: $('auxrows'),
};

let link = null;
let worker = null;
let running = false;
let busy = false;
let uid = null;
let status = { text: 'Not connected', kind: '' };
const snaps = [];
const rcTimes = [];
let lastPacketAt = null;
let lastChannel = null;
let sticks = null;
const aux = new Array(8).fill(null);

const auxBars = [];
for (let i = 0; i < 8; i++) {
  const row = document.createElement('div');
  row.className = 'auxrow unseen';
  row.innerHTML = `<span>AUX${i + 1}</span><div class="track"><div class="fill"></div></div><span class="num">-</span>`;
  el.auxrows.appendChild(row);
  auxBars.push({ row, fill: row.querySelector('.fill'), num: row.querySelector('.num') });
}

function setStatus(text, kind = '') {
  status = { text, kind };
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

function applyPhrase() {
  const phrase = el.phrase.value;
  if (!phrase) {
    uid = null;
    el.uid.textContent = '';
  } else {
    uid = uidFromPhrase(phrase);
    el.uid.textContent = 'UID ' + Array.from(uid).join(',') + ' (' + toHex(uid) + ')';
  }
  if (worker) worker.postMessage({ type: 'uid', uid: uid ? Array.from(uid) : null });
  sticks = null;
  aux.fill(null);
  if (el.remember.checked) storageSet(phrase || null);
}

function startWorker() {
  worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
  worker.onmessage = (ev) => {
    const m = ev.data;
    busy = false;
    if (m.type === 'error') {
      setStatus('Decoder error: ' + m.message, 'err');
      return;
    }
    const now = performance.now();
    const rc = m.packets.filter((p) => p.type === 'RCDATA');
    snaps.push([now, m.packets.length > 0]);
    for (const p of m.packets) {
      lastPacketAt = now;
      lastChannel = p.channelMhz;
    }
    for (const p of rc) {
      rcTimes.push(now);
      sticks = p.sticksCrsf;
      aux[0] = p.armed ? 1811 : 172;
      aux[p.hybrid8.aux - 1] = p.hybrid8.crsf;
    }
  };
  worker.postMessage({ type: 'uid', uid: uid ? Array.from(uid) : null });
}

async function loop() {
  let failures = 0;
  while (running) {
    try {
      const raw = await link.cap16(SAMPLES, RATE_CODE);
      failures = 0;
      setStatus('Connected', 'on');
      if (!busy) {
        busy = true;
        worker.postMessage({ type: 'snapshot', iq: raw.buffer, freqMhz: FREQ_MHZ }, [raw.buffer]);
      }
    } catch (e) {
      if (!running) return;
      if (link.closed || ++failures > 20) {
        setStatus('Disconnected: ' + e.message, 'err');
        break;
      }
      setStatus('Serial error: ' + e.message + ', resyncing', 'warn');
      try { await link.synchronize(); } catch (e2) { setStatus('Resync failed: ' + e2.message, 'err'); break; }
    }
  }
  if (running) await disconnect();
}

async function connect() {
  el.connect.disabled = true;
  link = new SdrLink();
  try {
    await link.open(2000000);
  } catch (e) {
    link = null;
    el.connect.disabled = false;
    setStatus(e.name === 'NotFoundError' ? 'No port selected' : 'Open failed: ' + e.message, e.name === 'NotFoundError' ? '' : 'err');
    return;
  }
  try {
    setStatus('Synchronizing', 'warn');
    await link.synchronize();
    const bad = [];
    for (const cmd of [`FREQ ${FREQ_MHZ}`, 'GAIN HARDWARE']) {
      const reply = await link.command(cmd);
      if (reply.startsWith('ERR')) bad.push(cmd + ': ' + reply);
    }
    if (bad.length) throw new Error(bad.join('; '));
  } catch (e) {
    setStatus('Setup failed: ' + e.message, 'err');
    await link.close();
    link = null;
    el.connect.disabled = false;
    return;
  }
  snaps.length = 0;
  rcTimes.length = 0;
  lastPacketAt = null;
  startWorker();
  running = true;
  el.connect.textContent = 'Disconnect';
  el.connect.disabled = false;
  loop();
}

async function disconnect() {
  running = false;
  busy = false;
  el.connect.disabled = true;
  if (worker) { worker.terminate(); worker = null; }
  if (link) { await link.close(); link = null; }
  el.connect.textContent = 'Connect';
  el.connect.disabled = false;
  if (status.kind !== 'err') setStatus('Not connected');
}

function render() {
  const now = performance.now();
  while (snaps.length && now - snaps[0][0] > WINDOW_MS) snaps.shift();
  while (rcTimes.length && now - rcTimes[0] > WINDOW_MS) rcTimes.shift();
  const span = Math.min(WINDOW_MS, Math.max(now - (snaps.length ? snaps[0][0] : now), 1000)) / 1000;
  const age = lastPacketAt === null ? null : now - lastPacketAt;
  if (running) {
    const hits = snaps.filter((s) => s[1]).length;
    const rate = snaps.length ? Math.round((100 * hits) / snaps.length) : 0;
    const parts = [
      `${(rcTimes.length / span).toFixed(1)} updates/s`,
      `${rate}% hit rate`,
      lastChannel === null ? '- MHz' : `${lastChannel.toFixed(1)} MHz`,
      age === null ? 'no packet yet' : `last packet ${(age / 1000).toFixed(1)} s ago`,
    ];
    if (!uid) parts.push('enter a binding phrase');
    el.stats.textContent = parts.join(' · ');
    el.led.className = 'led ' + (age !== null && age < STALE_MS ? 'on' : 'warn');
  } else {
    el.stats.textContent = status.text;
    el.led.className = 'led ' + (status.kind === 'err' ? 'err' : '');
  }
  const stale = age === null || age > STALE_MS || !running;
  paint(el.padL, el.valsL, sticks, 3, 2, 'yaw', 'thr', stale);
  paint(el.padR, el.valsR, sticks, 0, 1, 'roll', 'pitch', stale);
  aux.forEach((v, i) => {
    const b = auxBars[i];
    b.row.classList.toggle('unseen', v === null);
    b.fill.style.width = v === null ? '0' : `${Math.max(0, Math.min(100, ((v - 172) / (1811 - 172)) * 100))}%`;
    b.num.textContent = v === null ? '-' : String(v);
  });
  requestAnimationFrame(render);
}

function paint(pad, vals, s, xi, yi, xn, yn, stale) {
  pad.classList.toggle('stale', stale);
  const dot = pad.firstElementChild;
  if (!s) {
    dot.style.left = '50%';
    dot.style.top = '50%';
    vals.textContent = '-';
    return;
  }
  const fx = (s[xi] - 172) / (1811 - 172), fy = (s[yi] - 172) / (1811 - 172);
  dot.style.left = `${Math.max(0, Math.min(1, fx)) * 100}%`;
  dot.style.top = `${(1 - Math.max(0, Math.min(1, fy))) * 100}%`;
  vals.textContent = `${xn} ${s[xi]} · ${yn} ${s[yi]}`;
}

el.set.addEventListener('click', applyPhrase);
el.phrase.addEventListener('keydown', (e) => { if (e.key === 'Enter') applyPhrase(); });
el.remember.addEventListener('change', () => {
  if (el.remember.checked) storageSet(el.phrase.value || null);
  else storageSet(null);
});
el.connect.addEventListener('click', () => (running ? disconnect() : connect()));

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
