#ifndef ELRS_FRAMES_H
#define ELRS_FRAMES_H

#include <stddef.h>
#include <stdint.h>

#include "dsp.h"
#include "ota3.h"

typedef struct {
    uint32_t up_s; elrs_mode_t mode; const char *state; unsigned snaps, bursts, ok, rc, sync, crcfail, mirror;
    unsigned dec_us, cap_us, ret_us; int lock_err_us; int cfo_khz; unsigned gain, drop;
} elrs_stats_t;

/* nonce < 0 prints '-'. channel is the 0..79 index. Returns length, or -1 if cap is too small. */
int elrs_fmt_packet(char *buf, size_t cap, uint32_t t_us, const elrs_packet_t *p, int nonce, const uint8_t uid[6]);
int elrs_fmt_stats(char *buf, size_t cap, const elrs_stats_t *s);
int elrs_fmt_lock(char *buf, size_t cap, const char *state, const char *reason);
int elrs_fmt_warn(char *buf, size_t cap, const char *text);

#endif
