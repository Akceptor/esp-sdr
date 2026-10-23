#include "dsp.h"

#include <math.h>
#include <string.h>

#include "fhss.h"

#define MAX_FRAMES ((ELRS_MAX_SAMPLES - ELRS_FFT_N) / ELRS_FRAME_STEP + 1)
#define MAX_CH 16
#define CH_BINS 4
#define CENTROID_MARGIN 1
#define MIN_FRAMES 20
#define THRESH_X100 631u /* 8 dB over median */
#define WINDOW_SHIFT 11  /* Q15 window >> 11 leaves 4 bits of headroom over 10-bit input */
#define OVERLAP_DROP_DB 10.0f

static int16_t g_window[ELRS_FFT_N];
static int16_t g_cos[ELRS_FFT_N / 2], g_sin[ELRS_FFT_N / 2];
static volatile int g_tables_ready;

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
        elrs_iq16_t s = {(int16_t)sext10(w), (int16_t)sext10(w >> 10)};
        if (s.i >= 511 || s.i <= -511 || s.q >= 511 || s.q <= -511) {
            local.clipped++;
        }
        sum_i += s.i;
        sum_q += s.q;
        memcpy(&out[k], &s, sizeof s);
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

void elrs_fft64(int16_t data[2 * ELRS_FFT_N])
{
    tables_init();
    for (unsigned i = 0; i < ELRS_FFT_N; i++) {
        unsigned j = bitrev6(i);
        if (j > i) {
            int16_t tr = data[2 * i], ti = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = tr;
            data[2 * j + 1] = ti;
        }
    }
    for (unsigned half = 1; half < ELRS_FFT_N; half <<= 1) {
        unsigned tw_step = ELRS_FFT_N / (2 * half);
        for (unsigned base = 0; base < ELRS_FFT_N; base += 2 * half) {
            for (unsigned k = 0; k < half; k++) {
                int wr = g_cos[k * tw_step], wi = -g_sin[k * tw_step];
                unsigned a = base + k, b = a + half;
                int br = data[2 * b], bi = data[2 * b + 1];
                int tr = (br * wr - bi * wi + 16384) >> 15;
                int ti = (br * wi + bi * wr + 16384) >> 15;
                int ar = data[2 * a], ai = data[2 * a + 1];
                data[2 * a] = (int16_t)((ar + tr) >> 1);
                data[2 * a + 1] = (int16_t)((ai + ti) >> 1);
                data[2 * b] = (int16_t)((ar - tr) >> 1);
                data[2 * b + 1] = (int16_t)((ai - ti) >> 1);
            }
        }
    }
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
            buf[2 * i] = (int16_t)((p[i].i * (int)g_window[i]) >> WINDOW_SHIFT);
            buf[2 * i + 1] = (int16_t)((p[i].q * (int)g_window[i]) >> WINDOW_SHIFT);
        }
        elrs_fft64(buf);
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
