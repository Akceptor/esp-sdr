import test from 'node:test';
import assert from 'node:assert/strict';
import { ElrsLink, firmwareKind } from '../link.js';

function fake(lines) {
  const written = [];
  return {
    written,
    readLine: async () => {
      if (!lines.length) throw new Error('serial port closed');
      return lines.shift();
    },
    write: async (t) => { written.push(t); },
    synchronize: async () => {},
    close: async () => {},
  };
}

test('firmwareKind', () => {
  assert.equal(firmwareKind('ESP32ELRS 1 elrs-rx 0'), 'elrs');
  assert.equal(firmwareKind('ESP32SDR 6 burst 16380'), 'stock');
  assert.equal(firmwareKind('ERR command'), 'unknown');
});

test('command reply is found among streamed lines', async () => {
  const sdr = fake(['P 1 2 R - 3 4.0 0 f801020304050695', 'S up=1 mode=SCAN state=ACQ snaps=3 bursts=1 ok=1 rc=1 sync=0 crcfail=0 mirror=0 dec_us=2000 cap_us=1400 ret_us=0 lock_err_us=0 cfo_khz=240 gain=40 drop=0', 'OK']);
  const link = new ElrsLink(sdr);
  const seen = [];
  link.onStream((l) => seen.push(l.kind));
  assert.equal(await link.command('UID 000000001 33e'), 'OK');
  assert.deepEqual(seen, ['P', 'S']);
  assert.equal(sdr.written[0], 'UID 000000001 33e\n');
});

test('malformed lines are ignored', async () => {
  const sdr = fake(['\x00\x7fgarb', 'P 1 2', 'P x y z w v u t s']);
  const link = new ElrsLink(sdr);
  const seen = [];
  link.onStream((l) => seen.push(l));
  await link.pump();
  assert.deepEqual(seen, []);
});

test('stop writes RX OFF then RELEASE', async () => {
  const sdr = fake(['OK']);
  const link = new ElrsLink(sdr);
  await link.stop();
  assert.deepEqual(sdr.written, ['RX OFF\n', 'RELEASE\n']);
});

test('pump routes replies to a pending command', async () => {
  const lines = [];
  const sdr = fake(lines);
  const link = new ElrsLink(sdr);
  const seen = [];
  link.onStream((l) => seen.push(l.kind));
  sdr.readLine = (() => {
    const q = ['W stale', 'MODE a b'];
    return async () => { if (!q.length) throw new Error('serial port closed'); return q.shift(); };
  })();
  const p = link.pump();
  assert.equal(await link.command('MODE?'), 'MODE a b');
  await p;
  assert.deepEqual(seen, ['W']);
});

function scripted(q, written = []) {
  return {
    written,
    readLine: async () => { if (!q.length) throw new Error('serial port closed'); return q.shift(); },
    write: async (t) => { written.push(t); },
  };
}

test('concurrent commands are serialized in order', async () => {
  const written = [];
  const replies = { 'X\n': 'A', 'Y\n': 'B' };
  let served = 0;
  const sdr = {
    write: async (t) => { written.push(t); },
    readLine: async () => {
      while (served >= written.length) {
        if (written.length >= 2 && served >= 2) throw new Error('serial port closed');
        await new Promise((r) => setTimeout(r, 1));
      }
      return replies[written[served++]];
    },
  };
  const link = new ElrsLink(sdr);
  const p = link.pump();
  const [a, b] = await Promise.all([link.command('X'), link.command('Y')]);
  assert.deepEqual([a, b], ['A', 'B']);
  assert.deepEqual(written, ['X\n', 'Y\n']);
  await p;
});

test('throwing write rejects without leaving a timer or pending waiter', async () => {
  const sdr = scripted(['W x']);
  sdr.write = async () => { throw new Error('boom'); };
  const link = new ElrsLink(sdr);
  const p = link.pump();
  await assert.rejects(link.command('X', 50), /boom/);
  assert.equal(link.pending, null);
  await p;
  await new Promise((r) => setTimeout(r, 80));
});

test('pump exit rejects a pending command immediately', async () => {
  const link = new ElrsLink(scripted([]));
  link.running = true;
  const c = link.command('X', 5000);
  const p = link.pump();
  await assert.rejects(c, /closed/);
  await p;
});
