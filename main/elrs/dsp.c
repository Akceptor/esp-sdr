#include "dsp.h"

#include <math.h>
#include <string.h>

#include "fhss.h"
#include "ota3.h"

#define MAX_FRAMES ((ELRS_MAX_SAMPLES - ELRS_FFT_N) / ELRS_FRAME_STEP + 1)
#define MAX_CH 16
#define CH_BINS 4
#define CENTROID_MARGIN 1
#define MIN_FRAMES 20
#define SMOOTH_FRAMES 2 /* +-2 frames: a 6 dB packet is invisible frame by frame */
#define THRESH_X100 200u /* 3 dB over the 25th percentile of the smoothed power */
#define WINDOW_SHIFT 11  /* Q15 window >> 11 leaves 4 bits of headroom over 10-bit input */
#define OVERLAP_DROP_DB 10.0f

static int16_t g_window[ELRS_FFT_N];
static int16_t g_cos[ELRS_FFT_N / 2], g_sin[ELRS_FFT_N / 2];
static uint8_t g_rev[ELRS_FFT_N];
static volatile int g_tables_ready;

static unsigned bitrev6(unsigned v);

static void tables_init(void)
{
    if (g_tables_ready) {
        return;
    }
    const float pi = 3.14159265358979f;
    for (int i = 0; i < ELRS_FFT_N; i++) {
        g_window[i] = (int16_t)lrintf(32767.0f * (0.5f - 0.5f * cosf(2.0f * pi * (float)i / ELRS_FFT_N)));
    }
    for (int i = 0; i < ELRS_FFT_N / 2; i++) {
        g_cos[i] = (int16_t)lrintf(32767.0f * cosf(2.0f * pi * (float)i / ELRS_FFT_N));
        g_sin[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * pi * (float)i / ELRS_FFT_N));
    }
    for (unsigned i = 0; i < ELRS_FFT_N; i++) {
        g_rev[i] = (uint8_t)bitrev6(i);
    }
    g_tables_ready = 1;
}

static int sext10(uint32_t v)
{
    return (int)(v & 0x3ffu) - (int)((v & 0x200u) << 1);
}

void elrs_convert(const uint32_t *words, elrs_iq16_t *out, size_t n, elrs_convert_info_t *info)
{
    elrs_convert_info_t local = {0, 0, 0, 0};
    if (n == 0) {
        if (info) {
            *info = local;
        }
        return;
    }
    local.gain = (uint8_t)((words[n / 2] >> 20) & 0xffu);
    long sum_i = 0, sum_q = 0;
    for (size_t k = 0; k < n; k++) {
        uint32_t w = words[k];
        int vi = sext10(w), vq = sext10(w >> 10);
        local.clipped += ((unsigned)(vi + 510) > 1020u) | ((unsigned)(vq + 510) > 1020u);
        sum_i += vi;
        sum_q += vq;
        out[k].i = (int16_t)vi;
        out[k].q = (int16_t)vq;
    }
    long half = (long)(n / 2);
    long mi = (sum_i >= 0 ? sum_i + half : sum_i - half) / (long)n;
    long mq = (sum_q >= 0 ? sum_q + half : sum_q - half) / (long)n;
    for (size_t k = 0; k < n; k++) {
        out[k].i = (int16_t)(out[k].i - mi);
        out[k].q = (int16_t)(out[k].q - mq);
    }
    local.mean_i = (int16_t)mi;
    local.mean_q = (int16_t)mq;
    if (info) {
        *info = local;
    }
}

static unsigned bitrev6(unsigned v)
{
    unsigned r = 0;
    for (int b = 0; b < 6; b++) {
        r = (r << 1) | ((v >> b) & 1u);
    }
    return r;
}

/* Decimation-in-time butterflies on data already in bit-reversed order. */
static void fft64_butterflies(int16_t *data)
{
    for (unsigned half = 1; half < ELRS_FFT_N; half <<= 1) {
        unsigned tw_step = ELRS_FFT_N / (2 * half);
        for (unsigned base = 0; base < ELRS_FFT_N; base += 2 * half) {
            for (unsigned k = 0; k < half; k++) {
                int wr = g_cos[k * tw_step], wi = -g_sin[k * tw_step];
                int16_t *pa = data + 2 * (base + k), *pb = pa + 2 * half;
                int br = pb[0], bi = pb[1];
                int tr = (br * wr - bi * wi + 16384) >> 15;
                int ti = (br * wi + bi * wr + 16384) >> 15;
                int ar = pa[0], ai = pa[1];
                pa[0] = (int16_t)((ar + tr) >> 1);
                pa[1] = (int16_t)((ai + ti) >> 1);
                pb[0] = (int16_t)((ar - tr) >> 1);
                pb[1] = (int16_t)((ai - ti) >> 1);
            }
        }
    }
}

void elrs_fft64(int16_t data[2 * ELRS_FFT_N])
{
    tables_init();
    for (unsigned i = 0; i < ELRS_FFT_N; i++) {
        unsigned j = g_rev[i];
        if (j > i) {
            int16_t tr = data[2 * i], ti = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = tr;
            data[2 * j + 1] = ti;
        }
    }
    fft64_butterflies(data);
}

/* ---- Burst detector ---- */

typedef struct {
    int off_khz;
    int first_bin; /* first bin index (unwrapped) within +-500 kHz */
    int ch;
} chan_t;

/* 25th percentile, not the median: the fixture's bursts fill 54-60% of a snapshot, so a median sits
 * inside the burst. */
static unsigned floor_u32(const uint32_t *v, size_t n)
{
    uint32_t s[MAX_FRAMES];
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        while (j > 0 && s[j - 1] > v[i]) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = v[i];
    }
    return s[n / 4];
}

static float db10(float v)
{
    return 10.0f * log10f(v > 1.0f ? v : 1.0f);
}

static size_t spectrogram(const elrs_iq16_t *x, size_t n, const chan_t *ch, size_t nch,
                          uint32_t pw[MAX_CH][MAX_FRAMES], uint32_t bins[MAX_FRAMES][ELRS_FFT_N])
{
    size_t frames = n >= ELRS_FFT_N ? (n - ELRS_FFT_N) / ELRS_FRAME_STEP + 1 : 0;
    if (frames > MAX_FRAMES) {
        frames = MAX_FRAMES;
    }
    int16_t buf[2 * ELRS_FFT_N];
    for (size_t f = 0; f < frames; f++) {
        const elrs_iq16_t *p = x + f * ELRS_FRAME_STEP;
        for (int i = 0; i < ELRS_FFT_N; i++) {
            unsigned r = g_rev[i];
            buf[2 * r] = (int16_t)((p[i].i * (int)g_window[i]) >> WINDOW_SHIFT);
            buf[2 * r + 1] = (int16_t)((p[i].q * (int)g_window[i]) >> WINDOW_SHIFT);
        }
        fft64_butterflies(buf);
        for (int k = 0; k < ELRS_FFT_N; k++) {
            bins[f][k] = (uint32_t)((int)buf[2 * k] * buf[2 * k] + (int)buf[2 * k + 1] * buf[2 * k + 1]);
        }
        for (size_t c = 0; c < nch; c++) {
            uint32_t s = 0;
            for (int j = 0; j < CH_BINS; j++) {
                s += bins[f][(ch[c].first_bin + j) & (ELRS_FFT_N - 1)];
            }
            pw[c][f] = s;
        }
    }
    for (size_t c = 0; c < nch; c++) {
        uint32_t sm[MAX_FRAMES];
        for (size_t f = 0; f < frames; f++) {
            /* Bin power stays below ~2^27 (1/64 FFT scaling plus Parseval margin), so 4 bins x 5 frames fits 32 bits. */
            uint32_t acc = 0;
            unsigned cnt = 0;
            size_t lo = f > SMOOTH_FRAMES ? f - SMOOTH_FRAMES : 0;
            size_t hi = f + SMOOTH_FRAMES < frames ? f + SMOOTH_FRAMES : frames - 1;
            for (size_t g = lo; g <= hi; g++) {
                acc += pw[c][g];
                cnt++;
            }
            sm[f] = acc / cnt;
        }
        memcpy(pw[c], sm, frames * sizeof sm[0]);
    }
    return frames;
}

static float overlap_frac(const elrs_burst_t *a, const elrs_burst_t *b)
{
    int lo = a->frame_start > b->frame_start ? a->frame_start : b->frame_start;
    int hi = a->frame_end < b->frame_end ? a->frame_end : b->frame_end;
    int la = a->frame_end - a->frame_start, lb = b->frame_end - b->frame_start;
    int shorter = la < lb ? la : lb;
    return hi > lo ? (float)(hi - lo) / (float)shorter : 0.0f;
}

size_t elrs_detect(const elrs_iq16_t *x, size_t n, unsigned lo_mhz, elrs_burst_t *out, size_t max,
                   elrs_detect_stats_t *st)
{
    tables_init();
    /* Static: ~17 KB is too much for a task stack, so elrs_detect is not reentrant. */
    static uint32_t pw[MAX_CH][MAX_FRAMES];
    static uint32_t bins[MAX_FRAMES][ELRS_FFT_N];
    chan_t ch[MAX_CH];
    size_t nch = 0;
    for (unsigned c = 0; c < ELRS_NCH && nch < MAX_CH; c++) {
        int off = (int)elrs_channel_khz(c) - (int)(lo_mhz * 1000u);
        if (off < -ELRS_INBAND_KHZ || off > ELRS_INBAND_KHZ) {
            continue;
        }
        int lo_khz = off - 500;
        int first = lo_khz >= 0 ? (lo_khz + 249) / 250 : -((-lo_khz) / 250);
        ch[nch].off_khz = off;
        ch[nch].first_bin = first;
        ch[nch].ch = (int)c;
        nch++;
    }

    size_t frames = spectrogram(x, n, ch, nch, pw, bins);
    elrs_burst_t cand[2 * ELRS_MAX_BURSTS];
    size_t nc = 0;
    unsigned total = 0;
    if (frames >= MIN_FRAMES) {
        for (size_t c = 0; c < nch; c++) {
            uint64_t med = floor_u32(pw[c], frames);
            if (med < 1) {
                med = 1;
            }
            size_t run = 0;
            for (size_t f = 0; f <= frames; f++) {
                int hit = 0;
                if (f < frames) {
                    uint32_t p = pw[c][f];
                    hit = (uint64_t)p * 100u >= med * THRESH_X100;
                    if (hit && c > 0 && ch[c - 1].ch == ch[c].ch - 1 && pw[c - 1][f] > p) {
                        hit = 0;
                    }
                    if (hit && c + 1 < nch && ch[c + 1].ch == ch[c].ch + 1 && pw[c + 1][f] > p) {
                        hit = 0;
                    }
                }
                if (hit) {
                    run++;
                    continue;
                }
                if (run >= MIN_FRAMES) {
                    size_t s = f - run;
                    float pwr = 0.0f, wsum = 0.0f, wk = 0.0f;
                    for (size_t g = s; g < f; g++) {
                        pwr += (float)pw[c][g];
                        for (int j = -CENTROID_MARGIN; j < CH_BINS + CENTROID_MARGIN; j++) {
                            float b = (float)bins[g][(ch[c].first_bin + j) & (ELRS_FFT_N - 1)];
                            wsum += b;
                            wk += b * (float)(ch[c].first_bin + j);
                        }
                    }
                    pwr /= (float)run;
                    total++;
                    if (nc < 2 * ELRS_MAX_BURSTS) {
                        elrs_burst_t *b = &cand[nc++];
                        b->channel = (uint8_t)ch[c].ch;
                        b->frame_start = (uint16_t)s;
                        b->frame_end = (uint16_t)f;
                        b->cfo_hz = wsum > 0.0f ? (wk / wsum * 250000.0f - (float)ch[c].off_khz * 1000.0f) : 0.0f;
                        b->power_db = db10(pwr);
                        b->snr_db = b->power_db - db10((float)med);
                    }
                }
                run = 0;
            }
        }
    }

    unsigned dropped = 0;
    int dead[2 * ELRS_MAX_BURSTS] = {0};
    for (size_t a = 0; a < nc; a++) {
        for (size_t b = a + 1; b < nc; b++) {
            if (dead[a] || dead[b] || overlap_frac(&cand[a], &cand[b]) < 0.5f) {
                continue;
            }
            float d = cand[a].power_db - cand[b].power_db;
            if (d > OVERLAP_DROP_DB) {
                dead[b] = 1;
                dropped++;
            } else if (-d > OVERLAP_DROP_DB) {
                dead[a] = 1;
                dropped++;
            }
        }
    }
    size_t w = 0;
    for (size_t a = 0; a < nc && w < max; a++) {
        if (!dead[a]) {
            out[w++] = cand[a];
        }
    }
    if (st) {
        st->bursts = total;
        st->overlap_dropped = dropped;
    }
    return w;
}

/* ---- Per-burst demodulator ---- */

#define BOX 4
#define LP_TAPS 15
#define LP_CUTOFF_HZ 400000.0f
#define LP_FS_HZ 4000000.0f
#define DEC2 2
#define OUT_FS_HZ 2000000.0f
#define BURST_MARGIN 1040
#define OUT_MAX 2048
#define BIT_RATE_HZ 300000.0f
#define SYNC_BITS 16
#define PAYLOAD_BITS 112
#define MAX_BITS 336
#define MAX_CAND 32
#define MAX_FEC 5
#define MAX_MISSING 30
#define TIMING_PHASES 8
#define TIMING_SMOOTH 2
#define NCO_BITS 10
#define NCO_N (1 << NCO_BITS)
/* Input-sample position of output sample m is r0 + 8 m + LP_DELAY_IN: box-4 centre (1.5) plus the
 * 15-tap filter centre (7 boxes * 4). */
#define LP_DELAY_IN 29.5f

struct elrs_dsp_work {
    float cum[OUT_MAX + 1];
    float tmp[OUT_MAX];
    uint8_t bits[MAX_BITS];
};

static float g_nco[NCO_N];
static float g_lp[LP_TAPS];
static volatile int g_demod_ready;

static void demod_tables_init(void)
{
    if (g_demod_ready) {
        return;
    }
    const float pi = 3.14159265358979f;
    for (int i = 0; i < NCO_N; i++) {
        g_nco[i] = cosf(2.0f * pi * (float)i / NCO_N);
    }
    float sum = 0.0f;
    for (int i = 0; i < LP_TAPS; i++) {
        float k = (float)(i - LP_TAPS / 2);
        float x = 2.0f * LP_CUTOFF_HZ / LP_FS_HZ * k;
        float sinc = i == LP_TAPS / 2 ? 1.0f : sinf(pi * x) / (pi * x);
        g_lp[i] = sinc * (0.54f - 0.46f * cosf(2.0f * pi * (float)i / (LP_TAPS - 1)));
        sum += g_lp[i];
    }
    for (int i = 0; i < LP_TAPS; i++) {
        g_lp[i] /= sum;
    }
    g_demod_ready = 1;
}

size_t elrs_dsp_work_size(void)
{
    return sizeof(struct elrs_dsp_work);
}

/* Mixes [r0, r1) down by freq_hz, box-4 + 15-tap low-pass, decimates to 2 MS/s and writes the
 * discriminator output (I[n-1] Q[n] - Q[n-1] I[n]) / power into d. Returns the output count. */
static size_t mix_filter_discriminate(const elrs_iq16_t *x, size_t r0, size_t r1, float freq_hz, float *d)
{
    size_t nbox = (r1 - r0) / BOX;
    if (nbox < LP_TAPS) {
        return 0;
    }
    size_t nout = (nbox - LP_TAPS) / DEC2 + 1;
    if (nout > OUT_MAX) {
        nout = OUT_MAX;
    }
    uint32_t inc = (uint32_t)(int32_t)lrintf(freq_hz / (float)ELRS_FS_HZ * 4294967296.0f);
    uint32_t phase = 0;
    float br[16], bi[16];
    size_t produced = 0;
    float pr = 0.0f, pi = 0.0f;
    for (size_t m = 0; m < nout; m++) {
        while (produced < 2 * m + LP_TAPS) {
            float sr = 0.0f, si = 0.0f;
            for (int k = 0; k < BOX; k++) {
                const elrs_iq16_t *s = &x[r0 + produced * BOX + (size_t)k];
                unsigned idx = phase >> (32 - NCO_BITS);
                float c = g_nco[idx], sn = g_nco[(idx - NCO_N / 4) & (NCO_N - 1)];
                sr += (float)s->i * c + (float)s->q * sn;
                si += (float)s->q * c - (float)s->i * sn;
                phase += inc;
            }
            br[produced & 15] = sr;
            bi[produced & 15] = si;
            produced++;
        }
        float yr = 0.0f, yi = 0.0f;
        for (int k = 0; k < LP_TAPS; k++) {
            yr += g_lp[k] * br[(2 * m + (size_t)k) & 15];
            yi += g_lp[k] * bi[(2 * m + (size_t)k) & 15];
        }
        float pw = 0.5f * (yr * yr + yi * yi + pr * pr + pi * pi) + 1e-6f;
        d[m] = m ? (pr * yi - pi * yr) / pw : 0.0f;
        pr = yr;
        pi = yi;
    }
    return nout;
}

static float select_kth(float *a, size_t n, size_t k)
{
    size_t lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = a[(lo + hi) / 2];
        size_t i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) {
                i++;
            }
            while (a[j] > pivot) {
                j--;
            }
            if (i <= j) {
                float t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                if (j == 0) {
                    break;
                }
                j--;
            }
        }
        if (k <= j) {
            hi = j;
        } else if (k >= i) {
            lo = i;
        } else {
            break;
        }
    }
    return a[k];
}

static float quantile(float *a, size_t n, float q)
{
    float pos = (float)(n - 1) * q;
    size_t lo = (size_t)pos;
    float v = select_kth(a, n, lo);
    if (lo + 1 >= n) {
        return v;
    }
    float next = a[lo + 1];
    for (size_t i = lo + 2; i < n; i++) {
        if (a[i] < next) {
            next = a[i];
        }
    }
    return v + (next - v) * (pos - (float)lo);
}

static float two_means_center(const float *d, size_t n, float *scratch)
{
    memcpy(scratch, d, n * sizeof *d);
    float p10 = quantile(scratch, n, 0.1f), p90 = quantile(scratch, n, 0.9f);
    float c = 0.5f * (p10 + p90);
    for (int it = 0; it < 5; it++) {
        float ls = 0.0f, hs = 0.0f;
        size_t ln = 0, hn = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i] < c) {
                ls += d[i];
                ln++;
            } else {
                hs += d[i];
                hn++;
            }
        }
        if (!ln || !hn) {
            break;
        }
        c = 0.5f * (ls / (float)ln + hs / (float)hn);
    }
    return c;
}

static float interp_cum(const float *cum, size_t n, float t)
{
    if (t <= 0.0f) {
        return cum[0];
    }
    if (t >= (float)(n - 1)) {
        return cum[n - 1];
    }
    size_t i = (size_t)t;
    return cum[i] + (cum[i + 1] - cum[i]) * (t - (float)i);
}

static float soft_at(const float *cum, size_t n, float t, float half)
{
    return (interp_cum(cum, n, t + half) - interp_cum(cum, n, t - half)) / (2.0f * half);
}

static float recover_timing(const float *cum, size_t n, float s, float e, float sps)
{
    float score[TIMING_PHASES];
    for (int j = 0; j < TIMING_PHASES; j++) {
        float sum = 0.0f;
        int cnt = 0;
        for (float t = s + sps * (float)j / TIMING_PHASES; t < e; t += sps) {
            sum += fabsf(soft_at(cum, n, t, sps / 4.0f));
            cnt++;
        }
        score[j] = cnt ? sum / (float)cnt : 0.0f;
    }
    float best = -1.0f;
    int best_j = 0;
    for (int j = 0; j < TIMING_PHASES; j++) {
        float sm = 0.0f;
        for (int r = -TIMING_SMOOTH; r <= TIMING_SMOOTH; r++) {
            sm += score[(j - r + 2 * TIMING_PHASES) % TIMING_PHASES];
        }
        if (sm > best) {
            best = sm;
            best_j = j;
        }
    }
    return sps * (float)best_j / TIMING_PHASES;
}

typedef struct {
    uint8_t errors;
    uint8_t noninv;
    uint16_t pos;
} sync_cand_t;

static size_t find_sync(const uint8_t *bits, size_t nb, const uint8_t uid[6], bool both, sync_cand_t *cand)
{
    uint8_t pat[SYNC_BITS];
    for (int i = 0; i < 8; i++) {
        pat[i] = (uint8_t)((uid[5] >> (7 - i)) & 1u);
        pat[8 + i] = (uint8_t)((uid[4] >> (7 - i)) & 1u);
    }
    size_t nc = 0;
    if (nb < SYNC_BITS + PAYLOAD_BITS - MAX_MISSING) {
        return 0;
    }
    for (size_t i = 0; i + SYNC_BITS + PAYLOAD_BITS - MAX_MISSING <= nb; i++) {
        unsigned dist = 0;
        for (int j = 0; j < SYNC_BITS; j++) {
            dist += bits[i + (size_t)j] != pat[j];
        }
        for (unsigned pol = 0; pol < (both ? 2u : 1u); pol++) {
            unsigned e = pol ? SYNC_BITS - dist : dist;
            if (e > 1) {
                continue;
            }
            sync_cand_t c = {(uint8_t)e, (uint8_t)pol, (uint16_t)i};
            size_t w = nc < MAX_CAND ? nc++ : MAX_CAND - 1;
            while (w > 0 && (cand[w - 1].errors > c.errors ||
                             (cand[w - 1].errors == c.errors &&
                              (cand[w - 1].pos > c.pos || (cand[w - 1].pos == c.pos && cand[w - 1].noninv > c.noninv))))) {
                cand[w] = cand[w - 1];
                w--;
            }
            cand[w] = c;
        }
    }
    return nc;
}

typedef enum { BURST_NONE, BURST_NO_SYNC, BURST_FEC_REJECT, BURST_CRC_FAIL, BURST_OK } burst_result_t;

static burst_result_t demod_burst(const elrs_iq16_t *x, size_t n, const elrs_decode_cfg_t *cfg, const elrs_burst_t *b,
                                  uint16_t crc_init, elrs_dsp_work_t *w, elrs_packet_t *pk)
{
    size_t r0 = b->frame_start * (size_t)ELRS_FRAME_STEP;
    size_t r1 = b->frame_end * (size_t)ELRS_FRAME_STEP;
    r0 = r0 > BURST_MARGIN ? r0 - BURST_MARGIN : 0;
    r1 = r1 + BURST_MARGIN < n ? r1 + BURST_MARGIN : n;
    if (r1 > r0 + (size_t)OUT_MAX * 8 + 64) {
        r1 = r0 + (size_t)OUT_MAX * 8 + 64;
    }
    float f_hz = ((float)elrs_channel_khz(b->channel) - (float)(cfg->lo_mhz * 1000u)) * 1000.0f + b->cfo_hz;
    size_t nout = mix_filter_discriminate(x, r0, r1, f_hz, w->tmp);
    float so = ((float)(b->frame_start * (size_t)ELRS_FRAME_STEP) - (float)r0 - LP_DELAY_IN) / 8.0f;
    float eo = ((float)(b->frame_end * (size_t)ELRS_FRAME_STEP) - (float)r0 - LP_DELAY_IN) / 8.0f;
    so = so < 1.0f ? 1.0f : so;
    eo = eo > (float)nout ? (float)nout : eo;
    if (eo - so < 40.0f) {
        return BURST_NONE;
    }
    size_t si = (size_t)so, ei = (size_t)eo;
    /* tmp holds d; cum reuses w->cum. The scratch for the quantiles is cum, overwritten right after. */
    float centre = two_means_center(w->tmp + si, ei - si, w->cum);
    float acc = 0.0f;
    for (size_t m = 0; m < nout; m++) {
        acc += m ? w->tmp[m] - centre : 0.0f;
        w->cum[m] = acc;
    }
    float cfo = b->cfo_hz + centre * OUT_FS_HZ / (2.0f * 3.14159265358979f);

    const float sps = OUT_FS_HZ / BIT_RATE_HZ, half = sps / 4.0f;
    float tau = recover_timing(w->cum, nout, so, eo, sps);
    float t0 = so + tau;
    t0 -= sps * floorf((t0 - half) / sps);
    size_t nb = 0;
    for (float t = t0; t + half < (float)(nout - 1) && nb < MAX_BITS; t += sps) {
        w->bits[nb++] = soft_at(w->cum, nout, t, half) < 0.0f ? 1u : 0u;
    }

    sync_cand_t cand[MAX_CAND];
    size_t nc = find_sync(w->bits, nb, cfg->uid, cfg->polarity == ELRS_POL_AUTO, cand);
    if (!nc) {
        return BURST_NO_SYNC;
    }
    int saw_fec_ok = 0;
    for (size_t c = 0; c < nc; c++) {
        size_t p0 = cand[c].pos + SYNC_BITS;
        size_t avail = nb - p0 < PAYLOAD_BITS ? nb - p0 : PAYLOAD_BITS;
        uint8_t air[ELRS_AIR_BYTES] = {0};
        for (size_t k = 0; k < avail; k++) {
            unsigned bit = w->bits[p0 + k] ^ cand[c].noninv;
            air[k / 8] = (uint8_t)(air[k / 8] | (bit << (7 - k % 8)));
        }
        unsigned missing = (unsigned)(PAYLOAD_BITS - avail);
        uint8_t raw[8];
        unsigned corr = elrs_air_decode(air, missing, raw);
        if (corr > MAX_FEC) {
            continue;
        }
        saw_fec_ok = 1;
        int slot = elrs_crc_slot(raw, crc_init);
        if (slot < 0) {
            continue;
        }
        float t_end = t0 + sps * ((float)cand[c].pos + (float)SYNC_BITS - 0.5f);
        float in_pos = (float)r0 + 8.0f * t_end + LP_DELAY_IN;
        memcpy(pk->pkt, raw, 8);
        pk->channel = b->channel;
        pk->crc_slot = (int8_t)slot;
        pk->sync_end_sample = in_pos > 0.0f ? (uint32_t)(in_pos + 0.5f) : 0u;
        pk->cfo_hz = cfo;
        pk->snr_db = b->snr_db;
        pk->fec = (uint8_t)corr;
        pk->sync_errors = cand[c].errors;
        pk->missing_bits = (uint8_t)missing;
        pk->inverted = !cand[c].noninv;
        return BURST_OK;
    }
    return saw_fec_ok ? BURST_CRC_FAIL : BURST_FEC_REJECT;
}

size_t elrs_decode_snapshot(const elrs_iq16_t *x, size_t n, const elrs_decode_cfg_t *cfg, elrs_dsp_work_t *work,
                            elrs_packet_t *out, size_t max_out, elrs_decode_stats_t *st)
{
    demod_tables_init();
    elrs_decode_stats_t local = {0};
    elrs_burst_t bursts[ELRS_MAX_BURSTS];
    elrs_detect_stats_t ds;
    size_t nbur = elrs_detect(x, n, cfg->lo_mhz, bursts, ELRS_MAX_BURSTS, &ds);
    local.bursts = ds.bursts;
    local.overlap_dropped = ds.overlap_dropped;
    uint16_t init = elrs_crc_init(cfg->uid);
    size_t got = 0;
    for (size_t k = 0; k < nbur && got < max_out; k++) {
        switch (demod_burst(x, n, cfg, &bursts[k], init, work, &out[got])) {
        case BURST_NO_SYNC:
            break;
        case BURST_FEC_REJECT:
            local.sync_found++;
            local.fec_reject++;
            break;
        case BURST_CRC_FAIL:
            local.sync_found++;
            local.crc_fail++;
            break;
        case BURST_OK:
            local.sync_found++;
            local.wide += out[got].crc_slot > 0;
            got++;
            break;
        case BURST_NONE:
            break;
        }
    }
    if (st) {
        *st = local;
    }
    return got;
}
