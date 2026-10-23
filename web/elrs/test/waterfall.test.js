import test from 'node:test';
import assert from 'node:assert/strict';
import {
  WF_BINS, specQ, spectrumFromIq8, binColumn, axisTicks, WF_PALETTE, WF_MARK, LevelScale, WaterfallModel,
} from '../waterfall.js';

test('specQ', () => {
  const cases = [[0, 0], [1, 0], [3, 10], [10, 20], [100, 40], [1000, 60], [2 ** 27, 163], [2 ** 32 - 1, 193]];
  for (const [p, q] of cases) assert.equal(specQ(p), q, String(p));
});

function tone(mhz, n = 16380, amp = 100) {
  const iq = new Int8Array(2 * n);
  const cyc = mhz * 4;
  for (let k = 0; k < n; k++) {
    iq[2 * k] = Math.round(amp * Math.cos(2 * Math.PI * cyc * k / 64));
    iq[2 * k + 1] = Math.round(amp * Math.sin(2 * Math.PI * cyc * k / 64));
  }
  return iq;
}
const argmax = (a) => a.indexOf(Math.max(...a));

test('spectrumFromIq8 tone at +2 MHz', () => {
  const out = spectrumFromIq8(tone(2));
  assert.equal(out.length, WF_BINS);
  assert.equal(argmax(out), 40);
  for (let j = 0; j < 64; j++) if (Math.abs(j - 40) >= 3) assert.ok(out[40] >= out[j] + 40, `bin ${j}`);
});

test('spectrumFromIq8 tone at -3 MHz', () => {
  assert.equal(argmax(spectrumFromIq8(tone(-3))), 20);
});

test('spectrumFromIq8 DC lands in the centre column', () => {
  const iq = new Int8Array(2 * 16380);
  for (let k = 0; k < 16380; k++) iq[2 * k] = 50;
  assert.equal(argmax(spectrumFromIq8(iq)), 32);
});

test('spectrumFromIq8 silence and short input', () => {
  assert.ok(spectrumFromIq8(new Int8Array(2 * 16380)).every((v) => v === 0));
  assert.ok(spectrumFromIq8(tone(2, 32)).every((v) => v === 0));
});

test('binColumn', () => {
  assert.equal(binColumn(2436400, 2438), 26);
  assert.equal(binColumn(2438000, 2438), 32);
  assert.equal(binColumn(2430000, 2438), 0);
  assert.equal(binColumn(2445750, 2438), 63);
  assert.equal(binColumn(2446000, 2438), null);
  assert.equal(binColumn(2429800, 2438), null);
});

test('axisTicks', () => {
  const t = axisTicks(2438);
  assert.deepEqual(t.map((x) => x.text), ['2430', '2434', '2438', '2442']);
  assert.deepEqual(t.map((x) => x.frac), [0.5 / 64, 16.5 / 64, 32.5 / 64, 48.5 / 64]);
});

test('WF_PALETTE', () => {
  assert.equal(WF_PALETTE.length, 768);
  assert.deepEqual([...WF_PALETTE.slice(0, 3)], [0, 0, 0]);
  assert.deepEqual([...WF_PALETTE.slice(765)], [255, 255, 255]);
  let prev = -1;
  for (let i = 0; i < 256; i++) {
    const y = 0.299 * WF_PALETTE[3 * i] + 0.587 * WF_PALETTE[3 * i + 1] + 0.114 * WF_PALETTE[3 * i + 2];
    assert.ok(y >= prev, `entry ${i}`);
    prev = y;
  }
});

test('LevelScale', () => {
  const s = new LevelScale();
  s.update(new Uint8Array(64).fill(40));
  assert.equal(s.floor, 40);
  assert.equal(s.index(40), 0);
  assert.equal(s.index(70), 128);
  assert.equal(s.index(100), 255);
  assert.equal(s.index(10), 0);
  s.update(new Uint8Array(64).fill(50));
  assert.ok(Math.abs(s.floor - 41) < 1e-9);
});

const px = (pix, col, row) => [...pix.slice((row * WF_BINS + col) * 4, (row * WF_BINS + col) * 4 + 4)];
const pal = (i) => [WF_PALETTE[3 * i], WF_PALETTE[3 * i + 1], WF_PALETTE[3 * i + 2], 255];

test('WaterfallModel rows, marks and history', () => {
  const m = new WaterfallModel(3);
  assert.equal(m.loMhz, null);
  assert.doesNotThrow(() => m.mark(2436400));
  m.push(new Uint8Array(64).fill(40), 2438);
  m.push(new Uint8Array(64).fill(100), 2438);
  let p = m.pixels();
  assert.equal(p.length, 64 * 3 * 4);
  assert.deepEqual(px(p, 5, 0), pal(255));
  assert.deepEqual(px(p, 5, 1), pal(0));
  assert.deepEqual(px(p, 5, 2), [0, 0, 0, 255]);
  m.mark(2436400);
  assert.equal(m.dirty, true);
  p = m.pixels();
  assert.equal(m.dirty, false);
  assert.deepEqual(px(p, 26, 0), [...WF_MARK, 255]);
  assert.deepEqual(px(p, 27, 0), pal(255));
  m.push(new Uint8Array(64).fill(100), 2438);
  m.push(new Uint8Array(64).fill(100), 2438);
  p = m.pixels();
  assert.deepEqual(px(p, 26, 2), [...WF_MARK, 255]);
  assert.notDeepEqual(px(p, 26, 0), [...WF_MARK, 255]);
  m.push(new Uint8Array(64).fill(100), 2450);
  p = m.pixels();
  assert.equal(m.loMhz, 2450);
  assert.deepEqual(px(p, 0, 1), [0, 0, 0, 255]);
  assert.notDeepEqual(px(p, 0, 0), [0, 0, 0, 255]);
});

test('a new WaterfallModel is dirty so the first draw clears a stale canvas', () => {
  const m = new WaterfallModel(2);
  assert.equal(m.dirty, true);
  const p = m.pixels();
  assert.deepEqual(px(p, 0, 0), [0, 0, 0, 255]);
  assert.equal(m.dirty, false);
});
