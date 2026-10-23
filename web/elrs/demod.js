import * as dk from './dk500.js';

const BITRATE = 300e3;
const CH0_100K = 24004;
const NCH = 80;
const DEC = 4;
const SYNC_BITS = 16;
const PAYLOAD_BITS = 112;
const MAX_FEC_ERRORS = 5;
const MAX_MISSING_BITS = 30;
const MARGIN = 260;

export function lowpass(fs, cutoff, width) {
  const taps = Math.floor((3.3 * fs) / width) | 1;
  const h = new Float32Array(taps);
  let sum = 0;
  for (let i = 0; i < taps; i++) {
    const k = i - (taps >> 1);
    const x = (2 * cutoff / fs) * k;
    const sinc = k === 0 ? 1 : Math.sin(Math.PI * x) / (Math.PI * x);
    h[i] = sinc * (0.54 - 0.46 * Math.cos((2 * Math.PI * i) / (taps - 1)));
    sum += h[i];
  }
  for (let i = 0; i < taps; i++) h[i] /= sum;
  return h;
}

function percentiles(arr, qs) {
  const s = Float32Array.from(arr).sort();
  return qs.map((q) => {
    const pos = (s.length - 1) * q;
    const lo = Math.floor(pos);
    return s[lo] + (s[Math.min(lo + 1, s.length - 1)] - s[lo]) * (pos - lo);
  });
}

function movingAverage(p, n) {
  const len = p.length;
  const pre = new Float64Array(len + 1);
  for (let i = 0; i < len; i++) pre[i + 1] = pre[i] + p[i];
  const start = (n - 1) >> 1;
  const out = new Float32Array(len);
  for (let i = 0; i < len; i++) {
    const hi = Math.min(len, i + start + 1);
    const lo = Math.max(0, i + start - n + 1);
    out[i] = (pre[hi] - pre[lo]) / n;
  }
  return out;
}

function findBursts(yr, yi, fsd, minUs = 250, maxUs = 800) {
  const len = yr.length;
  const p = new Float32Array(len);
  for (let i = 0; i < len; i++) p[i] = yr[i] * yr[i] + yi[i] * yi[i];
  const env = movingAverage(p, Math.max(2, Math.floor(fsd / 1e6)));
  const [noise, peak] = percentiles(env, [0.1, 0.98]);
  if (noise <= 0 || peak < 8 * noise) return [];
  const thr = noise + 0.1 * (peak - noise);
  const gap = Math.floor(2e-6 * fsd);
  const runs = [];
  let i = 0;
  while (i < len) {
    if (env[i] <= thr) { i++; continue; }
    let j = i;
    while (j < len && env[j] > thr) j++;
    if (runs.length && i - runs[runs.length - 1][1] < gap) runs[runs.length - 1][1] = j;
    else runs.push([i, j]);
    i = j;
  }
  const out = [];
  for (const [s, e] of runs) {
    const us = ((e - s) / fsd) * 1e6;
    if (us < minUs || us > maxUs) continue;
    let sum = 0;
    for (let k = s; k < e; k++) sum += env[k];
    out.push({ s, e, noise, sigp: sum / (e - s), ratio: peak / noise });
  }
  return out;
}

function discriminate(yr, yi, lo, hi, out) {
  for (let n = Math.max(1, lo); n < hi; n++) {
    out[n] = Math.atan2(yi[n] * yr[n - 1] - yr[n] * yi[n - 1], yr[n] * yr[n - 1] + yi[n] * yi[n - 1]);
  }
}

function twoMeansCenter(d, s, e) {
  const v = Float32Array.from(d.subarray(s, e));
  const [p10, p90] = percentiles(v, [0.1, 0.9]);
  let c = (p10 + p90) / 2;
  for (let it = 0; it < 5; it++) {
    let ls = 0, ln = 0, hs = 0, hn = 0;
    for (const x of v) {
      if (x < c) { ls += x; ln++; } else { hs += x; hn++; }
    }
    if (!ln || !hn) break;
    c = (ls / ln + hs / hn) / 2;
  }
  return c;
}

function interp(cum, t) {
  if (t <= 0) return cum[0];
  const last = cum.length - 1;
  if (t >= last) return cum[last];
  const i = Math.floor(t);
  return cum[i] + (cum[i + 1] - cum[i]) * (t - i);
}

const softAt = (cum, t, half) => (interp(cum, t + half) - interp(cum, t - half)) / (2 * half);

function recoverTiming(cum, s, e, sps) {
  const nTau = Math.floor(sps * DEC) + 1;
  const score = new Float64Array(nTau);
  for (let j = 0; j < nTau; j++) {
    let sum = 0, cnt = 0;
    for (let t = s + j / DEC; t < e; t += sps) {
      sum += Math.abs(softAt(cum, t, sps / 4));
      cnt++;
    }
    score[j] = cnt ? sum / cnt : 0;
  }
  const k = Math.floor(sps * DEC / 4);
  let best = -1, bestJ = 0;
  for (let j = 0; j < nTau; j++) {
    let sm = 0;
    for (let r = -k; r <= k; r++) sm += score[(((j - r) % nTau) + nTau) % nTau];
    if (sm > best) { best = sm; bestJ = j; }
  }
  return bestJ / DEC;
}

function sampleBits(cum, t0, step, nBits) {
  const bits = [];
  for (let n = 0; n < nBits; n++) {
    const t = t0 + n * step;
    if (!(t + step / 4 < cum.length - 1)) break;
    bits.push(softAt(cum, t, step / 4) > 0 ? 1 : 0);
  }
  return bits;
}

function bitsToBytes(bits) {
  const out = new Uint8Array(bits.length >> 3);
  for (let i = 0; i < out.length; i++) {
    let v = 0;
    for (let b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
    out[i] = v;
  }
  return out;
}

function syncPatterns(uid) {
  const b = [];
  for (const v of [uid[5], uid[4]]) for (let i = 0; i < 8; i++) b.push((v >> (7 - i)) & 1);
  return [['pos', b], ['inv', b.map((x) => 1 - x)]];
}

class Channelizer {
  constructor(fs, f0Mhz) {
    this.fs = fs;
    this.fsd = fs / DEC;
    this.f0 = f0Mhz;
    this.hWide = lowpass(fs, 600e3, 400e3);
    this.hNarrow = lowpass(fs, 400e3, 300e3);
    this.tableN = Math.round(fs / 1e5);
    this.cos = new Float64Array(this.tableN);
    this.sin = new Float64Array(this.tableN);
    for (let i = 0; i < this.tableN; i++) {
      this.cos[i] = Math.cos((2 * Math.PI * i) / this.tableN);
      this.sin[i] = Math.sin((2 * Math.PI * i) / this.tableN);
    }
    this.zr = new Float32Array(0);
    this.zi = new Float32Array(0);
  }

  ensure(n) {
    if (this.zr.length < n) {
      this.zr = new Float32Array(n);
      this.zi = new Float32Array(n);
    }
  }

  fir(h, count) {
    const T = h.length, zr = this.zr, zi = this.zi;
    const outR = new Float32Array(count), outI = new Float32Array(count);
    for (let m = 0; m < count; m++) {
      const b = m * DEC;
      let ar = 0, ai = 0;
      for (let k = 0; k < T; k++) {
        const w = h[k];
        ar += w * zr[b + k];
        ai += w * zi[b + k];
      }
      outR[m] = ar;
      outI[m] = ai;
    }
    return [outR, outI];
  }

  gridChannel(xr, xi, d100) {
    const N = xr.length, half = this.hWide.length >> 1, tn = this.tableN;
    const count = Math.ceil(N / DEC);
    this.ensure(N + 2 * half);
    this.zr.fill(0, 0, half);
    this.zi.fill(0, 0, half);
    this.zr.fill(0, half + N, N + 2 * half);
    this.zi.fill(0, half + N, N + 2 * half);
    const zr = this.zr, zi = this.zi;
    const step = ((d100 % tn) + tn) % tn;
    let idx = 0;
    for (let n = 0; n < N; n++) {
      const c = this.cos[idx], s = this.sin[idx];
      zr[half + n] = xr[n] * c + xi[n] * s;
      zi[half + n] = xi[n] * c - xr[n] * s;
      idx += step;
      if (idx >= tn) idx -= tn;
    }
    return this.fir(this.hWide, count);
  }

  windowChannel(xr, xi, freqHz, w0, w1, totalCount) {
    const N = xr.length, T = this.hNarrow.length, half = T >> 1;
    const count = w1 - w0;
    const len = (count - 1) * DEC + T;
    const o0 = w0 * DEC - half;
    this.ensure(len);
    const zr = this.zr, zi = this.zi;
    const w = (-2 * Math.PI * freqHz) / this.fs;
    const rc = Math.cos(w), rs = Math.sin(w);
    let pr = Math.cos(w * o0), pi = Math.sin(w * o0);
    for (let j = 0; j < len; j++) {
      const n = o0 + j;
      if (n >= 0 && n < N) {
        zr[j] = xr[n] * pr - xi[n] * pi;
        zi[j] = xi[n] * pr + xr[n] * pi;
      } else {
        zr[j] = 0;
        zi[j] = 0;
      }
      const nr = pr * rc - pi * rs;
      pi = pr * rs + pi * rc;
      pr = nr;
    }
    const [r, i] = this.fir(this.hNarrow, count);
    const yr = new Float32Array(totalCount), yi = new Float32Array(totalCount);
    yr.set(r, w0);
    yi.set(i, w0);
    return [yr, yi];
  }
}

let cached = null;
function channelizer(fs, f0) {
  if (!cached || cached.fs !== fs || cached.f0 !== f0) cached = new Channelizer(fs, f0);
  return cached;
}

function detect(ch, xr, xi) {
  const f0x10 = Math.round(ch.f0 * 10);
  const half = ch.fs / 2e6 - 0.5;
  const cands = [];
  for (let k = 0; k < NCH; k++) {
    const d100 = CH0_100K + 10 * k - f0x10;
    if (Math.abs(d100 / 10) > half) continue;
    const [yr, yi] = ch.gridChannel(xr, xi, d100);
    for (const b of findBursts(yr, yi, ch.fsd)) cands.push({ k, channelMhz: (CH0_100K + 10 * k) / 10, yr, yi, ...b });
  }
  return cands.filter((c) => cands.every((o) => Math.abs(o.k - c.k) !== 1
    || !(o.s < c.e && c.s < o.e) || c.ratio >= o.ratio));
}

const otaInits = (uid) => [
  { ota: 3, init: dk.crcInit(uid, 3), nonces: 1 },
  { ota: 4, init: dk.crcInit(uid, 4), nonces: 256 },
];

function tryPayload(bits112, inits, missing) {
  const white = dk.whiten(bitsToBytes(bits112), 0x100, true);
  let pkt, fecErr;
  if (missing) {
    const r = dk.fecDecodeErasures(white, missing);
    pkt = r.data;
    fecErr = r.mismatches;
  } else {
    fecErr = dk.fecCountErrors(white);
    pkt = dk.fecDecode(white);
  }
  if (fecErr > MAX_FEC_ERRORS) return [];
  const found = [];
  for (const { ota, init, nonces } of inits) {
    const ok = [];
    for (let n = 0; n < nonces; n++) if (dk.packetCrcOk(pkt, init, n)) ok.push(n);
    if (ok.length) found.push({ ota, nonces: ok, pkt, fecErr });
  }
  return found;
}

/**
 * iq: Int8Array of interleaved I/Q at fs (Hz) centred on f0Mhz (integer MHz).
 * uid: 6 bytes or null (burst counting only). votes: optional Map of prior config votes.
 * Returns packets in both IQ polarities; use PolarityFilter to drop the mirror-image ghost.
 */
export function decodeSnapshot(iq, fs, f0Mhz, uid, votes = new Map()) {
  const N = iq.length >> 1;
  const xr = new Float32Array(N), xi = new Float32Array(N);
  let mr = 0, mi = 0;
  for (let n = 0; n < N; n++) { xr[n] = iq[2 * n]; xi[n] = iq[2 * n + 1]; mr += xr[n]; mi += xi[n]; }
  mr /= N; mi /= N;
  for (let n = 0; n < N; n++) { xr[n] -= mr; xi[n] -= mi; }

  const ch = channelizer(fs, f0Mhz);
  const fsd = ch.fsd, nd = Math.ceil(N / DEC);
  const cands = detect(ch, xr, xi);
  if (!uid) return { bursts: cands.length, packets: [] };

  const inits = otaInits(uid);
  const pats = syncPatterns(uid);
  const bursts = [];
  const d = new Float64Array(nd);
  for (const c of cands) {
    const { s, e } = c;
    d.fill(0);
    discriminate(c.yr, c.yi, 0, nd, d);
    let cfo = (twoMeansCenter(d, s, e) * fsd) / (2 * Math.PI);
    const w0 = Math.max(0, s - MARGIN), w1 = Math.min(nd, e + MARGIN);
    const f = ((CH0_100K + 10 * c.k - Math.round(ch.f0 * 10)) / 10) * 1e6 + cfo;
    const [yr, yi] = ch.windowChannel(xr, xi, f, w0, w1, nd);
    d.fill(0);
    discriminate(yr, yi, w0, w1, d);
    const center = twoMeansCenter(d, s, e);
    cfo += (center * fsd) / (2 * Math.PI);
    for (let n = w0; n < w1; n++) d[n] -= center;
    const cum = new Float64Array(nd);
    let acc = 0;
    for (let n = 0; n < nd; n++) { acc += d[n]; cum[n] = acc; }

    const sps = fsd / BITRATE;
    const tau = recoverTiming(cum, s, e, sps);
    const bits = sampleBits(cum, s + tau, sps, Math.floor((e - s - tau) / sps) + 16);
    const rec = { channelMhz: c.channelMhz, cfoHz: cfo, burstUs: ((e - s) / fsd) * 1e6,
      snrDb: 10 * Math.log10(Math.max(c.sigp - c.noise, 1e-12) / c.noise),
      startSample: s * DEC, matches: [] };
    if (bits.length >= SYNC_BITS + PAYLOAD_BITS - MAX_MISSING_BITS) {
      const cs = [];
      for (const [polarity, p] of pats) {
        for (let i = 0; i + SYNC_BITS <= bits.length; i++) {
          let dist = 0;
          for (let j = 0; j < SYNC_BITS && dist <= 1; j++) dist += bits[i + j] !== p[j];
          if (dist <= 1 && i + SYNC_BITS + PAYLOAD_BITS - MAX_MISSING_BITS <= bits.length) cs.push([dist, i, polarity]);
        }
      }
      cs.sort((a, b) => a[0] - b[0] || a[1] - b[1] || (a[2] < b[2] ? -1 : a[2] > b[2] ? 1 : 0));
      for (const [syncErrors, i, polarity] of cs) {
        let payload = bits.slice(i + SYNC_BITS, i + SYNC_BITS + PAYLOAD_BITS);
        const missing = PAYLOAD_BITS - payload.length;
        while (payload.length < PAYLOAD_BITS) payload.push(0);
        if (polarity === 'inv') payload = payload.map((x) => 1 - x);
        for (const m of tryPayload(payload, inits, missing)) rec.matches.push({ ...m, polarity, syncErrors });
      }
    }
    bursts.push(rec);
  }

  const local = new Map(votes);
  const cfgOf = (m) => m.polarity + '/' + m.ota;
  for (const b of bursts) for (const cfg of new Set(b.matches.map(cfgOf))) local.set(cfg, (local.get(cfg) || 0) + 1);
  const packets = [];
  for (const b of bursts) {
    if (!b.matches.length) continue;
    let best = null;
    for (const m of b.matches) {
      const key = [local.get(cfgOf(m)), -m.fecErr, -m.syncErrors];
      const better = !best || key[0] > best.key[0]
        || (key[0] === best.key[0] && (key[1] > best.key[1] || (key[1] === best.key[1] && key[2] > best.key[2])));
      if (better) best = { m, key };
    }
    const { m } = best;
    const { matches, ...info } = b;
    packets.push({ ...info, polarity: m.polarity, ota: m.ota, nonces: m.nonces, fecCorrections: m.fecErr,
      syncErrors: m.syncErrors, ...dk.parsePacket(m.pkt) });
  }
  return { bursts: bursts.length, packets };
}

/** Drops packets in the minority IQ polarity (the mirror-image ghost) once enough packets were seen. */
export class PolarityFilter {
  constructor(minSamples = 5) {
    this.min = minSamples;
    this.counts = { pos: 0, inv: 0 };
  }

  accept(packets) {
    for (const p of packets) this.counts[p.polarity]++;
    if (this.counts.pos + this.counts.inv < this.min) return packets;
    const major = this.counts.inv >= this.counts.pos ? 'inv' : 'pos';
    return packets.filter((p) => p.polarity === major);
  }
}
