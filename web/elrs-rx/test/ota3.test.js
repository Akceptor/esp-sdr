import test from 'node:test';
import assert from 'node:assert/strict';
import { parseLine, checkRc, parseSyncData, Hybrid8State } from '../ota3.js';

const uid45 = Uint8Array.of(0, 0, 0, 0, 0x13, 0x3e);
const S_LINE = 'S up=1 mode=SCAN state=ACQ snaps=3 bursts=1 ok=1 rc=1 sync=0 crcfail=0 mirror=0 dec_us=2000 cap_us=1400 ret_us=0 lock_err_us=0 cfo_khz=240 gain=40 drop=0';

test('P line parses', () => {
  assert.deepEqual(parseLine('P 123456 36 R - 240 18.3 1 f801020304050695'),
    { kind: 'P', tUs: 123456, ch: 36, type: 'R', nonce: null, cfoKhz: 240, snrDb: 18.3, fec: 1, data: 'f801020304050695' });
  assert.equal(parseLine('P 1 2 M 77 -5 3.0 0 00').nonce, 77);
});

test('corrupted hex is rejected', () => {
  assert.ok(checkRc('f801020304050695', uid45));
  assert.equal(checkRc('f801020304050694', uid45), null);
  assert.equal(checkRc('f80102030405069', uid45), null);
  assert.equal(checkRc('zz01020304050695', uid45), null);
});

test('checkRc returns sticks, arm and switches', () => {
  const rc = checkRc('f801020304050695', uid45);
  assert.equal(rc.sticksCrsf.length, 4);
  assert.equal(rc.armed, false);
  assert.equal(rc.switches7, 0x06);
});

test('SYNC data uses the 3.x layout', () => {
  assert.deepEqual(parseSyncData('160,77,14,1,1,5'), { fhss: 160, nonce: 77, rate: 14, tlm: 1, swMode: 1, mm: 5 });
  assert.deepEqual(parseSyncData('160,77,14,1,1,-'), { fhss: 160, nonce: 77, rate: 14, tlm: 1, swMode: 1, mm: null });
  assert.equal(parseSyncData('1,2,3'), null);
  assert.equal(parseSyncData('a,b,c,d,e,f'), null);
});

test('Hybrid8 covers AUX2..AUX8 including 16-position AUX8', () => {
  const h = new Hybrid8State();
  h.update({ armed: false, switches7: 0x37 });
  assert.equal(h.aux[8], 938);
  h.update({ armed: false, switches7: 0x38 });
  assert.equal(h.aux[8], 1044);
  h.update({ armed: false, switches7: (2 << 3) | 5 });
  assert.equal(h.aux[4], 1792);
  assert.equal(h.aux[1], 191);
  h.update({ armed: true, switches7: 0 });
  assert.equal(h.aux[1], 1792);
  assert.equal(typeof h.lastUpdate(4), 'number');
  assert.equal(new Hybrid8State().lastUpdate(3), null);
});

test('reply lines are not stream lines', () => {
  assert.equal(parseLine('SYNC 42').kind, 'reply');
  assert.equal(parseLine('OK').text, 'OK');
  assert.equal(parseLine(S_LINE).kind, 'S');
  assert.equal(parseLine(S_LINE).stats.cfo_khz, 240);
  assert.equal(parseLine(S_LINE).stats.mode, 'SCAN');
});

test('L, W and garbage lines', () => {
  assert.deepEqual(parseLine('L LOCKED sync'), { kind: 'L', state: 'LOCKED', reason: 'sync' });
  assert.deepEqual(parseLine('W rate 14'), { kind: 'W', text: 'rate 14' });
  assert.equal(parseLine(''), null);
  assert.equal(parseLine('\x00\x7fgarb'), null);
  assert.equal(parseLine('P 1 2'), null);
});
