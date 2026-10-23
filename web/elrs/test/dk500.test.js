import test from 'node:test';
import assert from 'node:assert/strict';
import * as dk from '../dk500.js';
import { md5 } from '../md5.js';

const enc = (s) => new TextEncoder().encode(s);

test('md5 known vectors', () => {
  assert.equal(dk.toHex(md5(new Uint8Array())), 'd41d8cd98f00b204e9800998ecf8427e');
  assert.equal(dk.toHex(md5(enc('abc'))), '900150983cd24fb0d6963f7d28e17f72');
  const long = enc('12345678901234567890123456789012345678901234567890123456789012345678901234567890');
  assert.equal(dk.toHex(md5(long)), '57edf4a22be3c955ac49da2e2107b67a');
});

test('UID from binding phrase', () => {
  assert.equal(dk.toHex(dk.uidFromPhrase('test')), '4f04fd822155');
  assert.equal(dk.toHex(dk.uidFromPhrase('correct horse battery')), 'd755d89d3f5b');
});

test('CRC14 matches Python', () => {
  assert.equal(dk.crc14(Uint8Array.of(1, 2, 3, 4, 5, 6, 7), 0x1234), 10355);
  assert.equal(dk.crc14(enc('hello world'), 0), 4555);
  assert.equal(dk.crcInit(Uint8Array.of(0, 0, 0, 0, 0x13, 0x3e), 3), 0x133d);
});

test('FEC encode matches Python and roundtrips with single bit errors', () => {
  const data = Uint8Array.of(0, 1, 2, 0x35, 0xff, 0x80, 0x7a, 0x11);
  const e = dk.fecEncode(data);
  assert.equal(dk.toHex(e), 'c4e390334023001b84db54cb14f3');
  assert.deepEqual(dk.fecDecode(e), data);
  assert.equal(dk.fecCountErrors(e), 0);
  for (let bit = 0; bit < 112; bit++) {
    const bad = Uint8Array.from(e);
    bad[bit >> 3] ^= 1 << (bit & 7);
    assert.deepEqual(dk.fecDecode(bad), data, `bit ${bit}`);
    assert.equal(dk.fecCountErrors(bad), 1);
  }
});

test('FEC erasure decode recovers a truncated tail', () => {
  const data = Uint8Array.of(0x8c, 0x00, 0x0a, 0x18, 0x00, 0x80, 0xaa, 0x7e);
  const e = dk.fecEncode(data);
  const cut = Uint8Array.from(e);
  cut.fill(0, 11);
  const r = dk.fecDecodeErasures(cut, 24);
  assert.deepEqual(r.data, data);
});

test('whitening keystream and packet CRC', () => {
  assert.equal(dk.toHex(dk.whiteningKeystream(8, 0x100, true)), '0088c8ead8e2546c');
  assert.equal(dk.toHex(dk.whiteningKeystream(8, 0x1ff, false)), 'ffe11d9aed853324');
  const init = dk.crcInit(Uint8Array.of(0, 0, 0, 0, 0x13, 0x3e), 3);
  const pkt = dk.buildRcPacket([992, 996, 174, 992], true, 0x2a, init, 0);
  assert.equal(dk.toHex(pkt), '8c000a180080aa7e');
  assert.ok(dk.packetCrcOk(pkt, init, 0));
  assert.ok(!dk.packetCrcOk(pkt, init ^ 1, 0));
  assert.deepEqual(dk.parsePacket(pkt).sticksCrsf, [992, 996, 174, 992]);
});

test('OTA4 10-bit pack/unpack', () => {
  assert.deepEqual(dk.unpack4x10(Uint8Array.of(1, 2, 3, 4, 5)), [513, 192, 64, 20]);
  assert.equal(dk.toHex(dk.pack4x10([1, 2, 3, 1023])), '010830c0ff');
});
