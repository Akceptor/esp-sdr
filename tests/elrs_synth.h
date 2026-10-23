#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fhss.h"
#include "ota3.h"

typedef struct {
    unsigned lo_mhz;
    uint8_t channel;
    double cfo_hz, dev_hz, snr_db /* in 1 MHz */, mirror_dbc, amplitude;
    long start_sample;
    unsigned truncate_tail_bits;
    bool second_packet;
    uint32_t seed;
} synth_cfg_t;

#define SYNTH_FS 16e6
#define SYNTH_BAUD 300e3
#define SYNTH_BITS (16 + 16 + 112)

typedef struct {
    uint32_t s;
} synth_rng_t;

static inline double synth_uniform(synth_rng_t *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return (r->s + 0.5) / 4294967296.0;
}

static inline double synth_gauss(synth_rng_t *r)
{
    return sqrt(-2.0 * log(synth_uniform(r))) * cos(6.283185307179586 * synth_uniform(r));
}

static inline int synth_q(double v)
{
    long r = lround(v);
    return r > 511 ? 511 : r < -511 ? -511 : (int)r;
}

static inline uint32_t synth_word(double i, double q)
{
    return ((uint32_t)synth_q(i) & 0x3ffu) | (((uint32_t)synth_q(q) & 0x3ffu) << 10);
}

static inline void synth_noise(uint32_t seed, double sigma_lsb, uint32_t *words, size_t n)
{
    synth_rng_t r = {seed ? seed : 1u};
    for (size_t k = 0; k < n; k++) {
        double i = sigma_lsb * synth_gauss(&r);
        words[k] = synth_word(i, sigma_lsb * synth_gauss(&r));
    }
}

static inline void synth_frame_bits(const uint8_t uid[6], const uint8_t pkt[8], uint8_t bits[SYNTH_BITS])
{
    uint8_t air[ELRS_AIR_BYTES], bytes[18] = {0x55, 0x55, uid[5], uid[4]};
    elrs_air_encode(pkt, air);
    for (int i = 0; i < ELRS_AIR_BYTES; i++) {
        bytes[4 + i] = air[i];
    }
    for (int i = 0; i < SYNTH_BITS; i++) {
        bits[i] = (bytes[i / 8] >> (7 - i % 8)) & 1u;
    }
}

/* mirror_dbc >= 0 means no mirror image. Samples are 10-bit and saturate at +-511. Polarity is inverted (bit 1 = low frequency). The
 * second packet is on the same channel. */
static inline void synth_snapshot(const synth_cfg_t *c, const uint8_t uid[6], const uint8_t pkt[8], uint32_t *words,
                                  size_t n)
{
    uint8_t bits[SYNTH_BITS];
    synth_frame_bits(uid, pkt, bits);
    double off = ((double)elrs_channel_khz(c->channel) - c->lo_mhz * 1000.0) * 1e3 + c->cfo_hz;
    double sigma = c->amplitude * sqrt(8.0 / pow(10.0, c->snr_db / 10.0));
    double mirror = c->mirror_dbc < 0 ? pow(10.0, c->mirror_dbc / 20.0) : 0.0;
    unsigned nbits = SYNTH_BITS - c->truncate_tail_bits;
    synth_rng_t r = {c->seed ? c->seed : 1u};
    double spb = SYNTH_FS / SYNTH_BAUD;
    int packets = c->second_packet ? 2 : 1;
    for (size_t k = 0; k < n; k++) {
        double re = sigma * synth_gauss(&r), im = sigma * synth_gauss(&r);
        for (int p = 0; p < packets; p++) {
            double t = (double)((long)k - c->start_sample - p * 16000L);
            if (t < 0 || t >= nbits * spb) {
                continue;
            }
            /* phase = integral of the instantaneous frequency, so bit edges stay continuous */
            double phase = 0.0;
            unsigned cur = (unsigned)(t / spb);
            for (unsigned b = 0; b < cur; b++) {
                phase += (bits[b] ? -1.0 : 1.0) * c->dev_hz * spb / SYNTH_FS;
            }
            phase += (bits[cur] ? -1.0 : 1.0) * c->dev_hz * (t - cur * spb) / SYNTH_FS;
            double a = 2.0 * M_PI * (off * t / SYNTH_FS + phase);
            re += c->amplitude * cos(a) + mirror * c->amplitude * cos(a);
            im += c->amplitude * sin(a) - mirror * c->amplitude * sin(a);
        }
        words[k] = synth_word(re, im);
    }
}
