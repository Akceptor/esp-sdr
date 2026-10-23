import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { decodeSnapshot, PolarityFilter } from '../demod.js';
import { uidFromPhrase } from '../dk500.js';

const dir = new URL('.', import.meta.url);
const meta = JSON.parse(fs.readFileSync(new URL('real_dk500.json', dir)));
const raw = fs.readFileSync(new URL('real_dk500.bin', dir));
const uid45 = Uint8Array.of(0, 0, 0, 0, 0x13, 0x3e);

function snapshot(i) {
  const n = meta.samples * 2;
  return new Int8Array(raw.buffer, raw.byteOffset + i * n, n);
}

test('real DK500 capture decodes to the known sticks without mirror ghosts', () => {
  const filter = new PolarityFilter();
  const all = [];
  const times = [];
  for (let i = 0; i < meta.snapshots; i++) {
    const t0 = performance.now();
    const r = decodeSnapshot(snapshot(i), meta.fs, meta.freqMhz, uid45);
    times.push(performance.now() - t0);
    all.push(...r.packets);
  }
  console.log('decode ms per snapshot:', times.map((t) => t.toFixed(1)).join(' '));
  assert.ok(Math.max(...times) < 150);

  const kept = filter.accept(all);
  const rc = kept.filter((p) => p.type === 'RCDATA');
  assert.ok(rc.length >= 5, `valid RC packets: ${rc.length}`);
  assert.ok(kept.every((p) => p.polarity === 'inv'), 'mirror-image ghosts must be dropped');
  assert.equal(all.length - kept.length, 2, 'two ghost packets in the capture');
  for (const p of rc) {
    assert.equal(p.sticksCrsf[0], 992);
    assert.ok(p.sticksCrsf[1] === 996 || p.sticksCrsf[1] === 997);
    assert.equal(p.sticksCrsf[2], 174);
    assert.equal(p.sticksCrsf[3], 992);
    assert.equal(p.ota, 3);
    assert.ok(Math.abs(p.cfoHz - 240e3) < 20e3);
  }
});

test('wrong binding phrase decodes nothing', () => {
  for (let i = 0; i < meta.snapshots; i++) {
    const r = decodeSnapshot(snapshot(i), meta.fs, meta.freqMhz, uidFromPhrase('test'));
    assert.equal(r.packets.length, 0);
  }
});

test('without a phrase only bursts are counted', () => {
  const r = decodeSnapshot(snapshot(1), meta.fs, meta.freqMhz, null);
  assert.ok(r.bursts >= 1);
  assert.deepEqual(r.packets, []);
});
