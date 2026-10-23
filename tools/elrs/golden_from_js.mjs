// Usage: node golden_from_js.mjs <bin> <samples> <freqMhz>
// Prints one JSON line per snapshot ({snap, bursts}) and per kept packet. Hex is emitted for RCDATA only.
import fs from 'node:fs';
import { decodeSnapshot, PolarityFilter } from '../../web/elrs/demod.js';

const [bin, samplesArg, freqArg] = process.argv.slice(2);
const samples = Number(samplesArg), freqMhz = Number(freqArg);
const raw = fs.readFileSync(bin);
const uid45 = Uint8Array.of(0, 0, 0, 0, 0x13, 0x3e);
const n = samples * 2;

const results = [];
for (let i = 0; i < raw.length / n; i++) {
  results.push(decodeSnapshot(new Int8Array(raw.buffer, raw.byteOffset + i * n, n), 16e6, freqMhz, uid45));
}
const filter = new PolarityFilter();
const kept = filter.accept(results.flatMap((r) => r.packets));
for (let i = 0; i < results.length; i++) {
  console.log(JSON.stringify({ snap: i, bursts: results[i].bursts }));
  for (const p of results[i].packets) {
    if (!kept.includes(p)) continue;
    const rec = { snap: i, channelMhz: p.channelMhz, type: p.type, polarity: p.polarity, ota: p.ota };
    if (p.type === 'RCDATA') rec.hex = Buffer.from(p.pkt).toString('hex');
    console.log(JSON.stringify(rec));
  }
}
