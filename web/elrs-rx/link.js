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
  }

  onStream(fn) { this.handlers.push(fn); }

  dispatch(p) {
    for (const h of this.handlers) h(p);
  }

  async command(text, timeoutMs = 3000) {
    if (this.running) {
      const reply = new Promise((resolve, reject) => {
        const timer = setTimeout(() => { this.pending = null; reject(new Error('timeout')); }, timeoutMs);
        this.pending = (t) => { clearTimeout(timer); this.pending = null; resolve(t); };
      });
      await this.sdr.write(text + '\n');
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
        else if (this.pending) this.pending(p.text);
      }
    } finally {
      this.running = false;
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
