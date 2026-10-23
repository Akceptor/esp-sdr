export const WF_BINS = 64;
const FRAME = 64;
const HOP = 256;
const STEP_KHZ = 250;

export function specQ(p) {
  return Math.max(0, Math.min(255, Math.round(20 * Math.log10(Math.max(p, 1)))));
}

const HANN = Float64Array.from({ length: FRAME }, (_, i) => 0.5 - 0.5 * Math.cos(2 * Math.PI * i / FRAME));
const COS = Float64Array.from({ length: FRAME }, (_, i) => Math.cos(2 * Math.PI * i / FRAME));
const SIN = Float64Array.from({ length: FRAME }, (_, i) => Math.sin(2 * Math.PI * i / FRAME));

export function spectrumFromIq8(iq) {
  const peak = new Float64Array(FRAME);
  const wr = new Float64Array(FRAME);
  const wi = new Float64Array(FRAME);
  for (let s = 0; s + FRAME <= iq.length / 2; s += HOP) {
    for (let n = 0; n < FRAME; n++) {
      wr[n] = iq[2 * (s + n)] * HANN[n];
      wi[n] = iq[2 * (s + n) + 1] * HANN[n];
    }
    for (let k = 0; k < FRAME; k++) {
      let re = 0, im = 0;
      for (let n = 0; n < FRAME; n++) {
        const t = (k * n) % FRAME;
        re += wr[n] * COS[t] + wi[n] * SIN[t];
        im += wi[n] * COS[t] - wr[n] * SIN[t];
      }
      const p = re * re + im * im;
      if (p > peak[k]) peak[k] = p;
    }
  }
  const out = new Uint8Array(WF_BINS);
  for (let j = 0; j < WF_BINS; j++) out[j] = specQ(peak[(j + FRAME / 2) % FRAME]);
  return out;
}

export function binColumn(freqKhz, loMhz) {
  const c = Math.round((freqKhz - loMhz * 1000) / STEP_KHZ) + WF_BINS / 2;
  return c >= 0 && c < WF_BINS ? c : null;
}

export function axisTicks(loMhz) {
  return [-8, -4, 0, 4].map((d) => ({
    frac: (binColumn((loMhz + d) * 1000, loMhz) + 0.5) / WF_BINS,
    text: String(loMhz + d),
  }));
}

const STOPS = [[0, 0, 0, 0], [0.35, 0, 0, 170], [0.6, 0, 200, 200], [0.8, 255, 220, 0], [1, 255, 255, 255]];
export const WF_PALETTE = (() => {
  const pal = new Uint8ClampedArray(256 * 3);
  for (let i = 0; i < 256; i++) {
    const x = i / 255;
    let s = 1;
    while (s < STOPS.length - 1 && x > STOPS[s][0]) s++;
    const [x0, ...c0] = STOPS[s - 1];
    const [x1, ...c1] = STOPS[s];
    const f = (x - x0) / (x1 - x0);
    for (let c = 0; c < 3; c++) pal[3 * i + c] = c0[c] + f * (c1[c] - c0[c]);
  }
  return pal;
})();
export const WF_MARK = [255, 0, 255];

const SPAN_Q = 60;

export class LevelScale {
  #floor = null;

  update(bins) {
    const pct = Uint8Array.from(bins).sort()[16];
    this.#floor = this.#floor === null ? pct : this.#floor + 0.1 * (pct - this.#floor);
  }

  get floor() { return this.#floor; }

  index(q) {
    return Math.max(0, Math.min(255, Math.round((q - this.#floor) * 255 / SPAN_Q)));
  }
}

export class WaterfallModel {
  #rows = [];
  #lo = null;
  #dirty = true;
  #scale = new LevelScale();

  constructor(rows = 120) {
    this.maxRows = rows;
  }

  push(bins, loMhz) {
    if (loMhz !== this.#lo) this.#rows = [];
    this.#lo = loMhz;
    // The first row has no history to compare against, so it defines the floor itself;
    // later rows are scaled before they pull the floor towards their own level.
    if (this.#scale.floor === null) this.#scale.update(bins);
    const idx = Uint8Array.from(bins, (q) => this.#scale.index(q));
    this.#scale.update(bins);
    this.#rows.unshift({ idx, mark: -1 });
    if (this.#rows.length > this.maxRows) this.#rows.pop();
    this.#dirty = true;
  }

  mark(freqKhz) {
    const col = this.#lo === null ? null : binColumn(freqKhz, this.#lo);
    if (col === null || !this.#rows.length) return;
    this.#rows[0].mark = col;
    this.#dirty = true;
  }

  get loMhz() { return this.#lo; }

  get dirty() { return this.#dirty; }

  pixels() {
    const out = new Uint8ClampedArray(WF_BINS * this.maxRows * 4);
    for (let r = 0; r < this.maxRows; r++) {
      const row = this.#rows[r];
      for (let c = 0; c < WF_BINS; c++) {
        const o = (r * WF_BINS + c) * 4;
        out[o + 3] = 255;
        if (!row) continue;
        if (row.mark === c) {
          out[o] = WF_MARK[0];
          out[o + 1] = WF_MARK[1];
          out[o + 2] = WF_MARK[2];
        } else {
          const p = 3 * row.idx[c];
          out[o] = WF_PALETTE[p];
          out[o + 1] = WF_PALETTE[p + 1];
          out[o + 2] = WF_PALETTE[p + 2];
        }
      }
    }
    this.#dirty = false;
    return out;
  }
}

export function drawWaterfall(canvas, model, axisEl) {
  if (model.dirty) {
    canvas.getContext('2d').putImageData(new ImageData(model.pixels(), WF_BINS, model.maxRows), 0, 0);
  }
  const lo = model.loMhz === null ? '' : String(model.loMhz);
  if (axisEl.dataset.lo === lo) return;
  axisEl.dataset.lo = lo;
  axisEl.replaceChildren();
  if (model.loMhz === null) return;
  for (const t of axisTicks(model.loMhz)) {
    const s = document.createElement('span');
    s.textContent = t.text;
    s.style.left = `${t.frac * 100}%`;
    axisEl.appendChild(s);
  }
}
