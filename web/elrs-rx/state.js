import { checkRc, Hybrid8State } from './ota3.js';

const WINDOW_MS = 10000;
const STALE_MS = 2000;
const CLIP_MS = 5000;
const S_HISTORY = 5;

export class RxState {
  constructor(now = () => performance.now()) {
    this.now = now;
    this.uid = null;
    this.hybrid = new Hybrid8State();
    this.reset();
  }

  reset() {
    this.rcTimes = [];
    this.sHistory = [];
    this.lastS = null;
    this.lastL = null;
    this.lastPacketAt = null;
    this.lastRcAt = null;
    this.lastRc = null;
    this.lastChannel = null;
    this.clipUntil = 0;
    this.wide = false;
    this.hybrid = new Hybrid8State();
  }

  // Device counters are cumulative and restart when the UID changes.
  resetCounters() {
    this.sHistory = [];
    this.lastS = null;
  }

  onLine(line) {
    const now = this.now();
    if (line.kind === 'P') {
      this.#onP(line, now);
    } else if (line.kind === 'S') {
      this.lastS = line.stats;
      const prev = this.sHistory[this.sHistory.length - 1];
      if (prev && (line.stats.bursts < prev.bursts || line.stats.ok < prev.ok)) this.sHistory = [];
      this.sHistory.push(line.stats);
      if (this.sHistory.length > S_HISTORY) this.sHistory.shift();
    } else if (line.kind === 'L') {
      this.lastL = line.state;
    } else if (line.kind === 'W') {
      if (line.text === 'wide_mode') this.wide = true;
      else if (line.text === 'clip') this.clipUntil = now + CLIP_MS;
    }
  }

  #onP(line, now) {
    if (line.type === 'S') {
      this.lastPacketAt = now;
      this.lastChannel = line.ch;
      return;
    }
    if (line.type !== 'R' || !this.uid) return;
    const rc = checkRc(line.data, this.uid);
    if (!rc) return;
    this.lastPacketAt = now;
    this.lastRcAt = now;
    this.lastChannel = line.ch;
    this.lastRc = rc;
    this.rcTimes.push(now);
    this.hybrid.update(rc, now);
  }

  get updatesPerSec() {
    const now = this.now();
    while (this.rcTimes.length && now - this.rcTimes[0] > WINDOW_MS) this.rcTimes.shift();
    return this.rcTimes.length / (WINDOW_MS / 1000);
  }

  get hitRate() {
    const s = this.lastS;
    return s && s.snaps > 0 ? s.ok / s.snaps : null;
  }

  get channel() { return this.lastChannel; }

  get badge() {
    if (this.lastL === 'COAST') return 'COAST';
    if (this.lastL === 'LOCK') {
      if (this.lastS?.mode === 'TIMED') return 'LOCKED·TIMED';
      if (this.lastS?.mode === 'HOP') return 'LOCKED·HOP';
    }
    return 'SCAN';
  }

  get lastPacketAgeMs() {
    return this.lastPacketAt === null ? null : this.now() - this.lastPacketAt;
  }

  get stale() {
    return this.lastRcAt === null || this.now() - this.lastRcAt > STALE_MS;
  }

  get hint() {
    if (this.wide) return 'wide_mode';
    if (this.sHistory.length >= S_HISTORY) {
      const first = this.sHistory[0], last = this.sHistory[S_HISTORY - 1];
      const bursts = last.bursts - first.bursts;
      const ok = last.ok - first.ok;
      if (bursts >= 20 && ok === 0) return 'wrong_phrase';
      if (bursts === 0) return 'no_signal';
    }
    if (this.now() < this.clipUntil) return 'clip';
    return null;
  }

  // OTA order is roll, pitch, throttle, yaw; this is the Mode 2 gimbal layout.
  get sticks() {
    if (!this.lastRc) return null;
    const [roll, pitch, throttle, yaw] = this.lastRc.sticksCrsf;
    return { roll, pitch, throttle, yaw };
  }

  get aux() { return this.hybrid.aux; }
}
