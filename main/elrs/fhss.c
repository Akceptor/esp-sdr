#include "fhss.h"

/* ELRS 3.6.4 LCG: 31-bit state, kept in uint32 so the multiply wraps without UB. */
static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state = (214013u * rng_state + 2531011u) & 0x7fffffffu;
    return rng_state >> 16;
}

uint32_t elrs_uid_seed(const uint8_t uid[6])
{
    return ((uint32_t)uid[2] << 24) | ((uint32_t)uid[3] << 16) | ((uint32_t)uid[4] << 8) | (uint32_t)(uid[5] ^ 3u);
}

void elrs_fhss_build(uint32_t seed, uint8_t seq[ELRS_SEQ_LEN])
{
    rng_state = seed;
    for (unsigned i = 0; i < ELRS_SEQ_LEN; i++) {
        unsigned m = i % ELRS_NCH;
        seq[i] = m == 0 ? ELRS_SYNC_CH : m == ELRS_SYNC_CH ? 0 : (uint8_t)m;
    }
    for (unsigned i = 0; i < ELRS_SEQ_LEN; i++) {
        if (i % ELRS_NCH == 0) continue;
        unsigned offset = (i / ELRS_NCH) * ELRS_NCH;
        unsigned r = rng() % (ELRS_NCH - 1) + 1;
        uint8_t t = seq[i];
        seq[i] = seq[offset + r];
        seq[offset + r] = t;
    }
}

unsigned elrs_fhss_ptr(unsigned ptr0, int64_t n0, int64_t n)
{
    int64_t d = (n >> 1) - (n0 >> 1);
    int64_t p = ((int64_t)ptr0 + d) % ELRS_SEQ_LEN;
    return (unsigned)(p < 0 ? p + ELRS_SEQ_LEN : p);
}
