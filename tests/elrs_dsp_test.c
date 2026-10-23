#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "elrs_fixture.h"
#include "elrs_synth.h"

#define N ELRS_MAX_SAMPLES
static const uint8_t UID[6] = {0, 0, 0, 0, 0x13, 0x3e};
static uint32_t words[N];
static elrs_iq16_t iq[N];

static void test_convert(void)
{
    uint32_t w[8];
    for (int k = 0; k < 8; k++) {
        w[k] = 0x05500000u;
    }
    w[0] = (uint32_t)(-512 & 0x3ff) | ((uint32_t)(-1 & 0x3ff) << 10);
    w[1] = 511u;
    elrs_iq16_t o[8], inplace[8];
    elrs_convert_info_t info;
    elrs_convert(w, o, 8, &info);
    assert(info.clipped == 2 && info.gain == 0x55);
    assert(info.mean_i == (-512 + 511) / 8 || info.mean_i == 0 || info.mean_i == -1);
    long si = 0, sq = 0;
    for (int k = 0; k < 8; k++) {
        si += o[k].i;
        sq += o[k].q;
    }
    assert(labs(si) <= 8 && labs(sq) <= 8);
    assert(o[0].i - o[1].i == -512 - 511);
    memcpy(inplace, w, sizeof w);
    elrs_convert((const uint32_t *)(void *)inplace, inplace, 8, &info);
    assert(memcmp(o, inplace, sizeof o) == 0);

    uint32_t dc[16];
    for (int k = 0; k < 16; k++) {
        dc[k] = 100u | (((uint32_t)(-30 & 0x3ff)) << 10);
    }
    elrs_iq16_t od[16];
    elrs_convert(dc, od, 16, &info);
    assert(info.mean_i == 100 && info.mean_q == -30 && od[3].i == 0 && od[3].q == 0);
}

static void test_fft(void)
{
    int16_t d[2 * ELRS_FFT_N];
    for (int i = 0; i < ELRS_FFT_N; i++) {
        d[2 * i] = (int16_t)lrint(8000 * cos(2 * M_PI * 5 * i / 64.0));
        d[2 * i + 1] = (int16_t)lrint(8000 * sin(2 * M_PI * 5 * i / 64.0));
    }
    elrs_fft64(d);
    double tot = 0, bin5 = 0;
    for (int k = 0; k < ELRS_FFT_N; k++) {
        double p = (double)d[2 * k] * d[2 * k] + (double)d[2 * k + 1] * d[2 * k + 1];
        tot += p;
        if (k == 5) {
            bin5 = p;
        }
    }
    assert(bin5 >= 0.9 * tot);
    assert(fabs(sqrt(bin5) - 8000) < 40);
    for (int i = 0; i < ELRS_FFT_N; i++) {
        d[2 * i] = 4000;
        d[2 * i + 1] = 0;
    }
    elrs_fft64(d);
    assert(d[0] == 4000);
    for (int k = 1; k < ELRS_FFT_N; k++) {
        assert(abs(d[2 * k]) <= 2 && abs(d[2 * k + 1]) <= 2);
    }
}

static void make_packet(uint8_t pkt[8])
{
    memset(pkt, 0, 8);
    pkt[1] = 0x12;
    pkt[2] = 0x34;
    pkt[3] = 0x56;
    elrs_finish_packet(pkt, elrs_crc_init(UID), 0);
}

static synth_cfg_t base_cfg(unsigned ch, double cfo)
{
    synth_cfg_t c;
    memset(&c, 0, sizeof c);
    c.lo_mhz = 2438;
    c.channel = (uint8_t)ch;
    c.cfo_hz = cfo;
    c.dev_hz = 120e3;
    c.snr_db = 20;
    c.amplitude = 150;
    c.start_sample = 3000;
    c.seed = 7 + ch;
    return c;
}

static void test_detect_synth(void)
{
    uint8_t pkt[8];
    make_packet(pkt);
    const double cfos[] = {-50e3, 230e3, 300e3};
    unsigned tested = 0;
    double worst = 0;
    for (unsigned ch = 0; ch < ELRS_NCH; ch++) {
        int off = (int)elrs_channel_khz(ch) - 2438000;
        if (off < -ELRS_INBAND_KHZ || off > ELRS_INBAND_KHZ) {
            continue;
        }
        for (int ci = 0; ci < 3; ci++) {
            synth_cfg_t c = base_cfg(ch, cfos[ci]);
            synth_snapshot(&c, UID, pkt, words, N);
            elrs_convert(words, iq, N, NULL);
            elrs_burst_t b[ELRS_MAX_BURSTS];
            elrs_detect_stats_t st;
            size_t nb = elrs_detect(iq, N, 2438, b, ELRS_MAX_BURSTS, &st);
            if (nb != 1 || b[0].channel != ch) {
                printf("FAIL ch %u cfo %.0f: %zu bursts", ch, cfos[ci], nb);
                for (size_t k = 0; k < nb; k++) {
                    printf(" [ch %u %u-%u cfo %.0f]", b[k].channel, b[k].frame_start, b[k].frame_end, b[k].cfo_hz);
                }
                printf("\n");
            }
            assert(nb == 1 && b[0].channel == ch);
            double err = fabs(b[0].cfo_hz - cfos[ci]);
            if (err > worst) {
                worst = err;
            }
            assert(err < 60e3);
            assert(abs((int)b[0].frame_start - (int)(c.start_sample / ELRS_FRAME_STEP)) <= 2);
            tested++;
        }
    }
    assert(tested >= 3 * 15);
    printf("detect_synth: %u cases, worst CFO error %.1f kHz\n", tested, worst / 1e3);
}

static void test_detect_noise(void)
{
    unsigned total = 0;
    for (uint32_t s = 1; s <= 1000; s++) {
        synth_noise(s, 20.0, words, N);
        elrs_convert(words, iq, N, NULL);
        elrs_burst_t b[ELRS_MAX_BURSTS];
        total += (unsigned)elrs_detect(iq, N, 2438, b, ELRS_MAX_BURSTS, NULL);
    }
    printf("detect_noise: %u bursts / 1000\n", total);
    assert(total <= 5);
}

static void test_overlap_drop(void)
{
    uint8_t pkt[8];
    make_packet(pkt);
    synth_cfg_t c = base_cfg(42, 230e3);
    c.snr_db = 40;
    c.mirror_dbc = -25;
    synth_snapshot(&c, UID, pkt, words, N);
    elrs_convert(words, iq, N, NULL);
    elrs_burst_t b[ELRS_MAX_BURSTS];
    elrs_detect_stats_t st;
    size_t nb = elrs_detect(iq, N, 2438, b, ELRS_MAX_BURSTS, &st);
    printf("overlap_drop: %zu kept, bursts %u dropped %u\n", nb, st.bursts, st.overlap_dropped);
    assert(nb == 1 && b[0].channel == 42 && st.overlap_dropped == 1);
}

static void test_detect_fixture(const char *bin)
{
    const unsigned want[6] = {36, 38, 32, 41, 43, 31};
    for (unsigned i = 0; i < 6; i++) {
        assert(fixture_load(bin, N, i, words) == N);
        elrs_convert(words, iq, N, NULL);
        elrs_burst_t b[ELRS_MAX_BURSTS];
        elrs_detect_stats_t st;
        size_t nb = elrs_detect(iq, N, 2436, b, ELRS_MAX_BURSTS, &st);
        int found = 0;
        printf("fixture %u:", i);
        for (size_t k = 0; k < nb; k++) {
            printf(" [ch %u f%u-%u cfo %.0f snr %.1f]", b[k].channel, b[k].frame_start, b[k].frame_end,
                   b[k].cfo_hz, b[k].snr_db);
            if (b[k].channel == want[i]) {
                found = 1;
                assert(fabs(b[k].cfo_hz - 240e3) < 40e3);
            }
        }
        printf(" dropped %u\n", st.overlap_dropped);
        assert(found);
    }
}

int main(int argc, char **argv)
{
    test_convert();
    test_fft();
    test_detect_synth();
    test_detect_noise();
    test_overlap_drop();
    assert(argc > 1);
    test_detect_fixture(argv[1]);
    return 0;
}
