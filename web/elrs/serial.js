const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export const supported = () => typeof navigator !== 'undefined' && 'serial' in navigator;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class SdrLink {
  constructor() {
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.chunks = [];
    this.buffered = 0;
    this.waiter = null;
    this.closed = false;
    this.pump = null;
  }

  async open(baudRate = 2000000) {
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate, bufferSize: 1 << 20 });
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this.closed = false;
    this.pump = this.pumpLoop();
  }

  async pumpLoop() {
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (value && value.length) {
          this.chunks.push(value);
          this.buffered += value.length;
          this.wake();
        }
      }
    } catch (e) {
      this.error = e;
    }
    this.closed = true;
    this.wake();
  }

  wake() {
    if (this.waiter) {
      const w = this.waiter;
      this.waiter = null;
      w();
    }
  }

  async waitData(deadline) {
    if (this.closed) throw new Error('serial port closed');
    const left = deadline - performance.now();
    if (left <= 0) throw new Error('timeout');
    await new Promise((resolve) => {
      const timer = setTimeout(resolve, left);
      this.waiter = () => { clearTimeout(timer); resolve(); };
    });
  }

  take(n) {
    const out = new Uint8Array(n);
    let got = 0;
    while (got < n) {
      const c = this.chunks[0];
      const k = Math.min(c.length, n - got);
      out.set(c.subarray(0, k), got);
      got += k;
      if (k === c.length) this.chunks.shift();
      else this.chunks[0] = c.subarray(k);
    }
    this.buffered -= n;
    return out;
  }

  async readExact(n, timeoutMs = 5000) {
    const deadline = performance.now() + timeoutMs;
    while (this.buffered < n) await this.waitData(deadline);
    return this.take(n);
  }

  async readLine(timeoutMs = 5000) {
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      for (let ci = 0, off = 0; ci < this.chunks.length; ci++) {
        const c = this.chunks[ci];
        const i = c.indexOf(10);
        if (i >= 0) {
          const bytes = this.take(off + i + 1);
          return new TextDecoder().decode(bytes.subarray(0, bytes.length - 1)).trim();
        }
        off += c.length;
      }
      await this.waitData(deadline);
    }
  }

  flush() {
    this.chunks = [];
    this.buffered = 0;
  }

  async write(text) {
    await this.writer.write(new TextEncoder().encode(text));
  }

  async command(text, timeoutMs = 5000) {
    await this.write(text + '\n');
    return this.readLine(timeoutMs);
  }

  // Opening the port may reset the board; discard everything up to the echoed nonce.
  async synchronize(timeoutMs = 10000) {
    const deadline = performance.now() + timeoutMs;
    while (performance.now() < deadline) {
      const marker = 'SYNC ' + Math.floor(Math.random() * 1e9);
      await this.write('\nRELEASE\n' + marker + '\n');
      const end = performance.now() + 1000;
      while (performance.now() < end) {
        try {
          if ((await this.readLine(Math.max(1, end - performance.now()))) === marker) return;
        } catch (e) {
          if (this.closed) throw e;
        }
      }
    }
    throw new Error('Could not synchronize with the firmware');
  }

  async cap16(samples, rateCode) {
    const header = (await this.command(`CAP16 ${samples} ${rateCode}`)).split(/\s+/);
    if (header.length !== 4 || header[0] !== 'DATA') throw new Error('CAP16 failed: ' + header.join(' '));
    const n = parseInt(header[1], 10);
    const crc = parseInt(header[2], 16);
    const raw = await this.readExact(n * 2);
    if (crc32(raw) !== crc) throw new Error('CRC mismatch');
    return raw;
  }

  async close() {
    if (!this.port) return;
    try {
      if (!this.closed) {
        await Promise.race([this.write('RELEASE\n'), sleep(500)]);
      }
    } catch (e) { /* port already gone */ }
    try { await this.reader.cancel(); } catch (e) { /* ignore */ }
    try { await this.pump; } catch (e) { /* ignore */ }
    try { this.reader.releaseLock(); } catch (e) { /* ignore */ }
    try { this.writer.releaseLock(); } catch (e) { /* ignore */ }
    try { await this.port.close(); } catch (e) { /* ignore */ }
    this.port = null;
  }
}
