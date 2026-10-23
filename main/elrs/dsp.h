#ifndef ELRS_DSP_H
#define ELRS_DSP_H

#include <stddef.h>
#include <stdint.h>

#define ELRS_FS_HZ 16000000
#define ELRS_MAX_SAMPLES 16380
#define ELRS_FFT_N 64
#define ELRS_FRAME_STEP 256
/* Wider than the 6 MHz in the design: the real fixture has a packet at +7.4 MHz from the LO. */
#define ELRS_INBAND_KHZ 7500
#define ELRS_MAX_BURSTS 8

typedef struct {
    int16_t i, q;
} elrs_iq16_t;

typedef struct {
    unsigned clipped;
    uint8_t gain;
    int16_t mean_i, mean_q;
} elrs_convert_info_t;

/* words: bank format (I = bits 0-9, Q = bits 10-19, two's complement; gain = bits 20-27, taken from
 * the middle word). Removes the mean. out may alias words (in place on the device); each element
 * is read before it is written. */
void elrs_convert(const uint32_t *words, elrs_iq16_t *out, size_t n, elrs_convert_info_t *info);

/* In-place radix-2 sc16 on interleaved re/im, scaled 1/2 per stage like dsps_fft2r_sc16. */
void elrs_fft64(int16_t data[2 * ELRS_FFT_N]);

typedef struct {
    uint8_t channel;
    uint16_t frame_start, frame_end; /* end exclusive; frame f covers samples [256 f, 256 f + 64) */
    float cfo_hz;
    float power_db, snr_db; /* arbitrary reference; snr is against the channel's median frame power */
} elrs_burst_t;

/* bursts = detected before overlap drop, so returned count = bursts - overlap_dropped unless max truncates. */
typedef struct {
    unsigned bursts, overlap_dropped;
} elrs_detect_stats_t;

/* Returns the number of bursts written to out, ordered by channel. st may be NULL. */
size_t elrs_detect(const elrs_iq16_t *x, size_t n, unsigned lo_mhz, elrs_burst_t *out, size_t max,
                   elrs_detect_stats_t *st);

#endif
