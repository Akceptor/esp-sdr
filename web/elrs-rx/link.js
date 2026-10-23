import { parseLine } from './ota3.js';

export function firmwareKind(infoReply) {
  const first = String(infoReply).trim().split(/\s+/)[0];
  return first === 'ESP32ELRS' ? 'elrs' : first === 'ESP32SDR' ? 'stock' : 'unknown';
}

const isStream = (p) => p.kind !== 'reply';

export class ElrsLink {
  constructor(sdr) {
    this.sdr = sdr;
    this.handlers = [];
    this.running = false;
    this.pending = null;
    this.tail = null;
  }

  onStream(fn) { this.handlers.push(fn); }

  dispatch(p) {
    for (const h of this.handlers) h(p);
  }

  command(text, timeoutMs = 3000) {
    const prev = this.tail;
    const run = prev ? prev.then(() => this.exec(text, timeoutMs)) : this.exec(text, timeoutMs);
    const mine = run.catch(() => {}).then(() => { if (this.tail === mine) this.tail = null; });
    this.tail = mine;
    return run;
  }

  async exec(text, timeoutMs) {
    if (this.running) {
      let timer;
      const reply = new Promise((resolve, reject) => {
        const me = {
          resolve: (t) => { clearTimeout(timer); this.pending = null; resolve(t); },
          reject: (e) => { clearTimeout(timer); this.pending = null; reject(e); },
        };
        timer = setTimeout(() => { if (this.pending === me) me.reject(new Error('timeout')); }, timeoutMs);
        this.pending = me;
      });
      try {
        await this.sdr.write(text + '\n');
      } catch (e) {
        clearTimeout(timer);
        this.pending = null;
        throw e;
      }
      return reply;
    }
    await this.sdr.write(text + '\n');
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      const p = parseLine(await this.sdr.readLine(Math.max(1, deadline - performance.now())));
      if (!p) continue;
      if (isStream(p)) this.dispatch(p);
      else return p.text;
    }
  }

  async pump() {
    this.running = true;
    try {
      while (this.running) {
        let line;
        try {
          line = await this.sdr.readLine(1000);
        } catch (e) {
          if (e && e.message === 'timeout') continue;
          break;
        }
        const p = parseLine(line);
        if (!p) continue;
        if (isStream(p)) this.dispatch(p);
        else if (this.pending) this.pending.resolve(p.text);
      }
    } finally {
      this.running = false;
      if (this.pending) this.pending.reject(new Error('serial port closed'));
    }
  }

  async stop() {
    try {
      await this.command('RX OFF');
    } finally {
      await this.sdr.write('RELEASE\n');
      this.running = false;
    }
  }
}
