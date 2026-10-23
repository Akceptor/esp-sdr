import { decodeSnapshot, PolarityFilter } from './demod.js';

let uid = null;
let filter = new PolarityFilter();
const FS = 16e6;

self.onmessage = (ev) => {
  const m = ev.data;
  if (m.type === 'uid') {
    uid = m.uid ? Uint8Array.from(m.uid) : null;
    filter = new PolarityFilter();
    return;
  }
  const t0 = performance.now();
  try {
    const r = decodeSnapshot(new Int8Array(m.iq), FS, m.freqMhz, uid);
    const packets = filter.accept(r.packets).map((p) => ({
      type: p.type, channelMhz: p.channelMhz, cfoHz: p.cfoHz, snrDb: p.snrDb,
      sticksCrsf: p.sticksCrsf, armed: p.armed, hybrid8: p.hybrid8,
    }));
    self.postMessage({ type: 'result', bursts: r.bursts, packets, ms: performance.now() - t0 });
  } catch (err) {
    self.postMessage({ type: 'error', message: String(err && err.message || err) });
  }
};
