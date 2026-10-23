import { md5 } from './md5.js';

export const CRC14_POLY = 0x2e57;
export const PT_RCDATA = 0, PT_DATA = 1, PT_SYNC = 2;

const HAMMING_CODES = [0x00, 0x71, 0x62, 0x13, 0x54, 0x25, 0x36, 0x47,
  0x38, 0x49, 0x5a, 0x2b, 0x6c, 0x1d, 0x0e, 0x7f];
const HAMMING_PACKED_DECODE = [
  0x00, 0x03, 0x05, 0xe7, 0x09, 0xeb, 0xed, 0xee, 0x03, 0x33, 0x4d, 0x63, 0x8d, 0xa3, 0xdd, 0xed,
  0x05, 0x2b, 0x55, 0x65, 0x8b, 0xbb, 0xc5, 0xeb, 0x81, 0x63, 0x65, 0x66, 0x88, 0x8b, 0x8d, 0x6f,
  0x09, 0x27, 0x47, 0x77, 0x99, 0xa9, 0xc9, 0xe7, 0x41, 0xa3, 0x44, 0x47, 0xa9, 0xaa, 0x4d, 0xaf,
  0x21, 0x22, 0xc5, 0x27, 0xc9, 0x2b, 0xcc, 0xcf, 0x11, 0x21, 0x41, 0x6f, 0x81, 0xaf, 0xcf, 0xff];

const popcount = (v) => { let c = 0; for (; v; v &= v - 1) c++; return c; };

export function uidFromPhrase(phrase) {
  const bytes = new TextEncoder().encode('-DMY_BINDING_PHRASE="' + phrase + '"');
  return md5(bytes).slice(0, 6);
}

export const toHex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

export function hammingDecode(code) {
  const d = HAMMING_PACKED_DECODE[code >> 1];
  return code & 1 ? d & 0x0f : d >> 4;
}

export function fecEncode(data) {
  const enc = new Array(16);
  for (let i = 0; i < 8; i++) {
    enc[i * 2] = HAMMING_CODES[data[i] & 0x0f];
    enc[i * 2 + 1] = HAMMING_CODES[data[i] >> 4];
  }
  const out = new Uint8Array(14);
  for (let i = 0; i < 7; i++) {
    for (let j = 0; j < 8; j++) {
      out[i * 2] |= ((enc[j] >> i) & 1) << j;
      out[i * 2 + 1] |= ((enc[j + 8] >> i) & 1) << j;
    }
  }
  return out;
}

function deinterleave(buf) {
  const enc = new Array(16).fill(0);
  for (let i = 0; i < 8; i++) {
    for (let j = 0; j < 7; j++) {
      enc[i] |= ((buf[j * 2] >> i) & 1) << j;
      enc[i + 8] |= ((buf[j * 2 + 1] >> i) & 1) << j;
    }
  }
  return enc;
}

export function fecDecode(buf) {
  const enc = deinterleave(buf);
  const out = new Uint8Array(8);
  for (let i = 0; i < 8; i++) out[i] = hammingDecode(enc[i * 2]) | (hammingDecode(enc[i * 2 + 1]) << 4);
  return out;
}

export function fecCountErrors(buf) {
  let n = 0;
  for (const c of deinterleave(buf)) if (HAMMING_CODES[hammingDecode(c)] !== c) n++;
  return n;
}

// The last `missingTailBits` of the 14-byte buffer (MSB-first on air) were never received.
// Each Hamming codeword is matched ignoring erased bits.
export function fecDecodeErasures(buf, missingTailBits) {
  const erased = new Map();
  for (let p = 112 - missingTailBits; p < 112; p++) {
    const byte = p >> 3, i = 7 - (p & 7);
    const key = i + 8 * (byte & 1);
    erased.set(key, (erased.get(key) || 0) | (1 << (byte >> 1)));
  }
  const enc = deinterleave(buf);
  let mism = 0;
  const nib = [];
  for (let c = 0; c < 16; c++) {
    const keep = 0x7f & ~(erased.get(c) || 0);
    let best = 0, bestD = 99;
    for (let v = 0; v < 16; v++) {
      const d = popcount((HAMMING_CODES[v] ^ enc[c]) & keep);
      if (d < bestD) { bestD = d; best = v; }
    }
    mism += bestD;
    nib.push(best);
  }
  const out = new Uint8Array(8);
  for (let i = 0; i < 8; i++) out[i] = nib[i * 2] | (nib[i * 2 + 1] << 4);
  return { data: out, mismatches: mism };
}

// 9-bit LFSR x^9+x^5+1, one byte per 8 shifts.
export function whiteningKeystream(n, seed = 0x1ff, bitrev = false) {
  let lfsr = seed;
  const out = new Uint8Array(n);
  for (let k = 0; k < n; k++) {
    let b = lfsr & 0xff;
    if (bitrev) {
      let r = 0;
      for (let i = 0; i < 8; i++) r |= ((b >> i) & 1) << (7 - i);
      b = r;
    }
    out[k] = b;
    for (let s = 0; s < 8; s++) {
      const fb = (lfsr ^ (lfsr >> 5)) & 1;
      lfsr = (lfsr >> 1) | (fb << 8);
    }
  }
  return out;
}

export function whiten(data, seed = 0x1ff, bitrev = false) {
  const ks = whiteningKeystream(data.length, seed, bitrev);
  return Uint8Array.from(data, (v, i) => v ^ ks[i]);
}

const CRC_TAB = (() => {
  const t = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    let crc = i << 6;
    for (let b = 0; b < 8; b++) crc = ((crc << 1) ^ (crc & 0x2000 ? CRC14_POLY : 0)) & 0xffff;
    t[i] = crc;
  }
  return t;
})();

export function crc14(data, init) {
  let crc = init & 0xffff;
  for (let i = 0; i < data.length; i++) crc = ((crc << 8) & 0xffff) ^ CRC_TAB[((crc >> 6) ^ data[i]) & 0xff];
  return crc & 0x3fff;
}

// OTA 4 xors the version into the high byte and the nonce into the low byte per packet.
// OTA 3 (ELRS 3.x) xors the version into the low byte and does not mix in the nonce.
export function crcInit(uid, ota = 4) {
  const base = (uid[4] << 8) | uid[5];
  return ota === 4 ? base ^ (4 << 8) : base ^ ota;
}

export function packetCrcOk(pkt, init, nonce) {
  const inCrc = ((pkt[0] >> 2) << 8) | pkt[7];
  const head = new Uint8Array(7);
  head.set(pkt.subarray(1, 7), 1);
  head[0] = pkt[0] & 3;
  const v = (pkt[0] & 3) === PT_SYNC ? 0 : nonce;
  return inCrc === crc14(head, init ^ v);
}

export function finishPacket(head7, init, nonce) {
  const v = (head7[0] & 3) === PT_SYNC ? 0 : nonce;
  const crc = crc14(head7, init ^ v);
  const out = new Uint8Array(8);
  out.set(head7);
  out[0] = head7[0] | ((crc >> 8) << 2);
  out[7] = crc & 0xff;
  return out;
}

export function fmap(x, inMin, inMax, outMin, outMax) {
  const r = Math.floor(((x - inMin) * (outMax - outMin) * 2) / (inMax - inMin) + outMin * 2 + 1) >> 1;
  return Math.max(0, Math.min(65535, r));
}

export const uint10ToCrsf = (v) => fmap(v, 0, 1023, 172, 1811);
export const crsfToUint10 = (v) => fmap(Math.min(Math.max(v, 172), 1811), 172, 1811, 0, 1023);
export const crsfToUs = (v) => fmap(v, 172, 1811, 988, 2012);
const nToCrsf = (v, mx) => Math.floor((v * 1601) / mx) + 191;
const switch3bToCrsf = (v) => ({ 0: 191, 5: 1792, 6: 992, 7: 992 })[v] ?? v * 240 + 391;

export function pack4x10(vals10) {
  const out = new Uint8Array(5);
  let shift = 0, idx = 0;
  for (const v of vals10) {
    out[idx] |= (v << shift) & 0xff;
    const left = 10 - 8 + shift;
    out[idx + 1] = v >> (10 - left);
    idx++;
    shift = left;
  }
  return out;
}

export function unpack4x10(raw) {
  let merged = 0, value = 0, ri = 0;
  const out = [];
  for (let n = 0; n < 4; n++) {
    while (merged < 10) {
      value |= raw[ri++] << merged;
      merged += 8;
    }
    out.push(value & 0x3ff);
    value >>= 10;
    merged -= 10;
  }
  return out;
}

export function buildRcPacket(sticksCrsf, armed, switches7, init, nonce) {
  const head = new Uint8Array(7);
  head[0] = PT_RCDATA;
  head.set(pack4x10(sticksCrsf.map(crsfToUint10)), 1);
  head[6] = (switches7 & 0x7f) | (armed ? 0x80 : 0);
  return finishPacket(head, init, nonce);
}

// Sticks are CRSF values in OTA order: roll, pitch, throttle, yaw.
export function parsePacket(pkt) {
  const t = pkt[0] & 3;
  if (t === PT_RCDATA) {
    const raw = unpack4x10(pkt.subarray(1, 6));
    const crsf = raw.map(uint10ToCrsf);
    const sw = pkt[6] & 0x7f;
    const idx = (sw >> 3) & 7;
    const hybrid = idx >= 6
      ? { aux: 8, crsf: nToCrsf(sw & 15, 15) }
      : { aux: 2 + idx, crsf: switch3bToCrsf(sw & 7) };
    return { type: 'RCDATA', sticksRaw10: raw, sticksCrsf: crsf, sticksUs: crsf.map(crsfToUs),
      armed: !!(pkt[6] >> 7), switchesRaw: sw, hybrid8: hybrid };
  }
  if (t === PT_SYNC) {
    return { type: 'SYNC', fhssIndex: pkt[1], syncNonce: pkt[2], rfRateEnum: pkt[3], flags: pkt[4],
      uid4: pkt[5], uid5XorModel: pkt[6] };
  }
  return { type: t === PT_DATA ? 'DATA' : 'type3', raw: toHex(pkt) };
}
