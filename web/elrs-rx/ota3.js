import { crc14, crcInit, parsePacket, PT_RCDATA } from '../elrs/dk500.js';

const STATS_KEYS = ['up', 'mode', 'state', 'snaps', 'bursts', 'ok', 'rc', 'sync', 'crcfail', 'mirror',
  'dec_us', 'cap_us', 'ret_us', 'lock_err_us', 'cfo_khz', 'gain', 'drop'];
const STRING_STATS = new Set(['mode', 'state']);
const INT = /^-?\d+$/;

function parseP(tok) {
  if (tok.length !== 9) return null;
  const [, t, ch, type, nonce, cfo, snr, fec, data] = tok;
  if (!INT.test(t) || !INT.test(ch) || !/^[RSM]$/.test(type) || !(nonce === '-' || INT.test(nonce))
      || !INT.test(cfo) || !/^-?\d+(\.\d+)?$/.test(snr) || !INT.test(fec)) return null;
  if (type === 'S' ? !parseSyncData(data) : !/^[0-9a-f]{16}$/i.test(data)) return null;
  return { kind: 'P', tUs: +t, ch: +ch, type, nonce: nonce === '-' ? null : +nonce, cfoKhz: +cfo,
    snrDb: +snr, fec: +fec, data };
}

function parseS(tok) {
  const stats = {};
  for (const t of tok.slice(1)) {
    const eq = t.indexOf('=');
    if (eq < 0) return null;
    const k = t.slice(0, eq), v = t.slice(eq + 1);
    if (!STATS_KEYS.includes(k)) continue;
    if (STRING_STATS.has(k)) stats[k] = v;
    else if (INT.test(v)) stats[k] = +v;
    else return null;
  }
  return STATS_KEYS.every((k) => k in stats) ? { kind: 'S', stats } : null;
}

function parseF(tok) {
  if (tok.length !== 4) return null;
  const [, t, lo, data] = tok;
  if (!INT.test(t) || !INT.test(lo) || +lo < 2402 || +lo > 2478 || !/^[0-9a-f]{128}$/i.test(data)) return null;
  return { kind: 'F', tUs: +t, loMhz: +lo, bins: Uint8Array.from(data.match(/../g), (h) => parseInt(h, 16)) };
}

export function parseLine(line) {
  const s = String(line).trim();
  if (!s) return null;
  if (!/^[\x20-\x7e]+$/.test(s)) return null;
  const tok = s.split(/\s+/);
  switch (tok[0]) {
    case 'P': return parseP(tok);
    case 'S': return parseS(tok);
    case 'F': return parseF(tok);
    case 'L': return tok.length >= 2 ? { kind: 'L', state: tok[1], reason: tok.slice(2).join(' ') } : null;
    case 'W': return tok.length >= 2 ? { kind: 'W', text: tok.slice(1).join(' ') } : null;
    default: return { kind: 'reply', text: s };
  }
}

export function checkRc(hex, uid) {
  if (typeof hex !== 'string' || !/^[0-9a-f]{16}$/i.test(hex)) return null;
  const pkt = Uint8Array.from(hex.match(/../g), (h) => parseInt(h, 16));
  if ((pkt[0] & 3) !== PT_RCDATA) return null;
  const head = pkt.slice(0, 7);
  head[0] = 0;
  if (crc14(head, crcInit(uid, 3)) !== (((pkt[0] >> 2) << 8) | pkt[7])) return null;
  const p = parsePacket(pkt);
  return { sticksCrsf: p.sticksCrsf, armed: p.armed, switches7: p.switchesRaw };
}

export function parseSyncData(data) {
  const parts = String(data).split(',');
  if (parts.length !== 6) return null;
  const [fhss, nonce, rate, tlm, swMode] = parts.slice(0, 5);
  if (![fhss, nonce, rate, tlm, swMode].every((v) => INT.test(v))) return null;
  const mm = parts[5];
  if (mm !== '-' && !INT.test(mm)) return null;
  return { fhss: +fhss, nonce: +nonce, rate: +rate, tlm: +tlm, swMode: +swMode, mm: mm === '-' ? null : +mm };
}

// Mapping is delegated to parsePacket so it stays identical to the stock page.
export class Hybrid8State {
  constructor() {
    this.values = new Array(9).fill(null);
    this.times = new Array(9).fill(null);
  }

  update(rc, now = Date.now()) {
    const pkt = new Uint8Array(8);
    pkt[6] = (rc.switches7 & 0x7f) | (rc.armed ? 0x80 : 0);
    const p = parsePacket(pkt);
    this.#set(1, rc.armed ? 1792 : 191, now);
    this.#set(p.hybrid8.aux, p.hybrid8.crsf, now);
  }

  #set(i, v, now) {
    this.values[i] = v;
    this.times[i] = now;
  }

  get aux() { return this.values; }

  lastUpdate(i) { return this.times[i] ?? null; }
}
