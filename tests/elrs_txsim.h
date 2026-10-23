#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fhss.h"
#include "ota3.h"

typedef struct {
    uint8_t uid[6];
    double ppm;
    double loss;
    unsigned tlm_denom;
    bool connected;
    uint32_t rng;
    int64_t nonce;
    unsigned ptr;
    unsigned sync_slot;
    uint32_t last_sync_ms;
    double t_us;
    uint8_t seq[240];
} txsim_t;

typedef struct {
    double t_us;
    uint8_t channel;
    uint8_t type;
    int64_t nonce;
    unsigned ptr;
    bool lost;
} txsim_pkt_t;

static inline void txsim_init(txsim_t *tx, const uint8_t uid[6], double ppm, double loss, uint32_t rng_seed)
{
    memset(tx, 0, sizeof *tx);
    memcpy(tx->uid, uid, 6);
    tx->ppm = ppm;
    tx->loss = loss;
    tx->tlm_denom = 128;
    tx->rng = rng_seed ? rng_seed : 1u;
    elrs_fhss_build(elrs_uid_seed(uid), tx->seq);
}

static inline double txsim_rand(txsim_t *tx)
{
    tx->rng ^= tx->rng << 13;
    tx->rng ^= tx->rng >> 17;
    tx->rng ^= tx->rng << 5;
    return tx->rng / 4294967296.0;
}

static inline bool txsim_next(txsim_t *tx, txsim_pkt_t *out)
{
    tx->nonce++;
    tx->t_us += 1000.0 * (1.0 + tx->ppm * 1e-6);
    if (tx->nonce % (int64_t)tx->tlm_denom == 0) {
        return false;
    }
    uint32_t now_ms = (uint32_t)(tx->t_us / 1000.0);
    uint32_t interval = tx->connected ? 5000u : 3u;
    unsigned parity = (unsigned)(tx->nonce & 1);
    bool sync = (tx->sync_slot / 2) <= parity && now_ms - tx->last_sync_ms > interval &&
                tx->seq[tx->ptr] == ELRS_SYNC_CH;
    if (sync) {
        tx->last_sync_ms = now_ms;
        tx->sync_slot = (tx->sync_slot + 1) % 4;
    }
    out->t_us = tx->t_us;
    out->channel = tx->seq[tx->ptr];
    out->type = sync ? ELRS_PT_SYNC : ELRS_PT_RC;
    out->nonce = tx->nonce;
    out->ptr = tx->ptr;
    out->lost = txsim_rand(tx) < tx->loss;
    if ((tx->nonce + 1) % 2 == 0) {
        tx->ptr = (tx->ptr + 1) % ELRS_SEQ_LEN;
    }
    return true;
}

static inline void txsim_reboot(txsim_t *tx, int64_t new_nonce, unsigned new_ptr)
{
    tx->nonce = new_nonce;
    tx->ptr = new_ptr % ELRS_SEQ_LEN;
    tx->sync_slot = 0;
}
