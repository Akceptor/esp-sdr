#pragma once

#include <stdint.h>

#define ELRS_NCH 80
#define ELRS_SEQ_LEN 240
#define ELRS_SYNC_CH 41

uint32_t elrs_uid_seed(const uint8_t uid[6]);
void elrs_fhss_build(uint32_t seed, uint8_t seq[ELRS_SEQ_LEN]);

static inline uint32_t elrs_channel_khz(unsigned ch)
{
    return 2400400u + 1000u * ch;
}

/* (ptr0 + floor(n/2) - floor(n0/2)) mod ELRS_SEQ_LEN; nonces are unwrapped counters. */
unsigned elrs_fhss_ptr(unsigned ptr0, int64_t n0, int64_t n);
