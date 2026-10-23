import test from 'node:test';
import assert from 'node:assert/strict';
import { RxState } from '../state.js';
import { parseLine } from '../ota3.js';

const uid45 = Uint8Array.of(0, 0, 0, 0, 0x13, 0x3e);
const RC = 'f801020304050695';
const p = (t, hex = RC, type = 'R') => parseLine(`P ${t} 36 ${type} - 240 18.3 1 ${hex}`);
const s = (o = {}) => {
  const v = { mode: 'SCAN', snaps: 100, bursts: 10, ok: 0, ...o };
  return parseLine(`S up=1 mode=${v.mode} state=ACQ snaps=${v.snaps} bursts=${v.bursts} ok=${v.ok} rc=0 sync=0 crcfail=0 mirror=0 dec_us=2000 cap_us=1400 ret_us=0 lock_err_us=0 cfo_khz=240 gain=40 drop=0`);
};

function make() {
  let t = 0;
  const st = new RxState(() => t);
  st.uid = uid45;
  return { st, at: (v) => { t = v; }, adv: (d) => { t += d; } };
}

test('updates per second over a 10 s window', () => {
  const { st, at } = make();
  for (let i = 0; i < 25; i++) { at(100 + i * 380); st.onLine(p(i)); }
  at(10000);
  assert.equal(st.updatesPerSec, 2.5);
  at(15000);
  assert.ok(st.updatesPerSec < 2.5);
});

test('RC lines failing the CRC for the UID are ignored', () => {
  const { st } = make();
  st.onLine(p(1, 'f801020304050694'));
  assert.equal(st.updatesPerSec, 0);
  assert.equal(st.sticks, null);
  assert.equal(st.lastPacketAgeMs, null);
});

test('bursts without decoded packets give the wrong_phrase hint', () => {
  const { st } = make();
  for (let i = 1; i <= 4; i++) st.onLine(s({ bursts: 10 * i }));
  assert.equal(st.hint, null);
  st.onLine(s({ bursts: 50 }));
  assert.equal(st.hint, 'wrong_phrase');
  st.onLine(s({ bursts: 60, ok: 3 }));
  assert.equal(st.hint, null);
});

test('no_signal after 5 S lines without bursts', () => {
  const { st } = make();
  for (let i = 0; i < 5; i++) st.onLine(s({ bursts: 0 }));
  assert.equal(st.hint, 'no_signal');
});

test('wide_mode is sticky, clip lasts 5 s', () => {
  const { st, adv } = make();
  st.onLine(parseLine('W clip'));
  assert.equal(st.hint, 'clip');
  adv(5001);
  assert.equal(st.hint, null);
  st.onLine(parseLine('W wide_mode'));
  adv(60000);
  assert.equal(st.hint, 'wide_mode');
});

test('stale after 2001 ms without an RC packet', () => {
  const { st, adv } = make();
  assert.equal(st.stale, true);
  assert.equal(st.lastPacketAgeMs, null);
  st.onLine(p(1));
  assert.equal(st.stale, false);
  adv(2000);
  assert.equal(st.stale, false);
  adv(1);
  assert.equal(st.stale, true);
  assert.equal(st.lastPacketAgeMs, 2001);
});

test('badge follows L lines and the S mode', () => {
  const { st } = make();
  assert.equal(st.badge, 'SCAN');
  st.onLine(s({ mode: 'TIMED' }));
  st.onLine(parseLine('L LOCK sync'));
  assert.equal(st.badge, 'LOCKED·TIMED');
  st.onLine(s({ mode: 'HOP' }));
  assert.equal(st.badge, 'LOCKED·HOP');
  st.onLine(parseLine('L COAST miss'));
  assert.equal(st.badge, 'COAST');
  st.onLine(parseLine('L LOST timeout'));
  assert.equal(st.badge, 'SCAN');
});

test('hit rate is ok over snaps of the last S line', () => {
  const { st } = make();
  assert.equal(st.hitRate, null);
  st.onLine(s({ snaps: 0 }));
  assert.equal(st.hitRate, null);
  st.onLine(s({ snaps: 200, ok: 50 }));
  assert.equal(st.hitRate, 0.25);
});

test('Mode 2 mapping: OTA 0 roll, 1 pitch, 2 throttle, 3 yaw', () => {
  const { st } = make();
  assert.equal(st.sticks, null);
  st.onLine(p(1));
  const rc = st.sticks;
  assert.deepEqual(Object.keys(rc), ['roll', 'pitch', 'throttle', 'yaw']);
  assert.equal(rc.roll, st.lastRc.sticksCrsf[0]);
  assert.equal(rc.pitch, st.lastRc.sticksCrsf[1]);
  assert.equal(rc.throttle, st.lastRc.sticksCrsf[2]);
  assert.equal(rc.yaw, st.lastRc.sticksCrsf[3]);
  assert.equal(st.aux[1], 191);
});

test('t_us wrap does not produce a negative age', () => {
  const { st, adv } = make();
  st.onLine(p(4294967000));
  adv(10);
  st.onLine(p(200));
  adv(5);
  assert.equal(st.lastPacketAgeMs, 5);
  assert.ok(st.lastPacketAgeMs >= 0);
});
