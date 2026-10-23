#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elrs_fhss_golden.h"
#include "fhss.h"
#include "ota3.h"
#include "elrs_txsim.h"

static void golden(void)
{
    uint8_t seq[ELRS_SEQ_LEN];
    assert(elrs_uid_seed(golden_uid_test) == golden_seed_test);
    assert(elrs_uid_seed(golden_uid_chb) == golden_seed_chb);
    elrs_fhss_build(golden_seed_test, seq);
    assert(!memcmp(seq, golden_seq_test, ELRS_SEQ_LEN));
    elrs_fhss_build(golden_seed_chb, seq);
    assert(!memcmp(seq, golden_seq_chb, ELRS_SEQ_LEN));
}

static void permutation(void)
{
    uint32_t s = 12345;
    for (int n = 0; n < 1000; n++) {
        s = s * 1664525u + 1013904223u;
        uint8_t seq[ELRS_SEQ_LEN];
        elrs_fhss_build(s, seq);
        for (int b = 0; b < 3; b++) {
            uint8_t seen[ELRS_NCH] = {0};
            for (int i = 0; i < ELRS_NCH; i++) {
                assert(seq[b * ELRS_NCH + i] < ELRS_NCH);
                seen[seq[b * ELRS_NCH + i]]++;
            }
            for (int i = 0; i < ELRS_NCH; i++) assert(seen[i] == 1);
            assert(seq[b * ELRS_NCH] == ELRS_SYNC_CH);
        }
    }
}

static void ptr_mapping(void)
{
    assert(elrs_fhss_ptr(10, 6, 7) == 10);
    assert(elrs_fhss_ptr(10, 6, 8) == 11);
    assert(elrs_fhss_ptr(10, 7, 8) == 11);
    assert(elrs_fhss_ptr(239, 0, 2) == 0);
    assert(elrs_fhss_ptr(5, 254, 258) == 7);
    assert(elrs_fhss_ptr(0, 10, 4) == 237);
    assert(elrs_channel_khz(0) == 2400400u && elrs_channel_khz(79) == 2479400u);
}

static void sync_cadence(void)
{
    static const uint8_t uid[6] = {0x4f, 0x04, 0xfd, 0x82, 0x21, 0x55};
    txsim_t tx;
    txsim_init(&tx, uid, 0, 0, 1);
    tx.tlm_denom = 1000003u; /* keeps TLM slots from perturbing the SYNC slot parity pattern */
    int visit = -1, in_visit = 0, syncs_in_visit = 0;
    int last_sync_tick = -1, parity[600], nsync = 0;
    for (int tick = 1; tick <= 80000; tick++) {
        txsim_pkt_t p;
        assert(txsim_next(&tx, &p));
        int on_sync = p.channel == ELRS_SYNC_CH;
        if (on_sync && !in_visit) {
            if (visit >= 1) assert(syncs_in_visit == 1);
            visit++;
            syncs_in_visit = 0;
        }
        in_visit = on_sync;
        if (p.type == ELRS_PT_SYNC) {
            assert(on_sync);
            syncs_in_visit++;
            if (visit >= 1) {
                if (last_sync_tick >= 0) assert(abs(tick - last_sync_tick - 160) <= 1);
                last_sync_tick = tick;
                assert(nsync < 600);
                parity[nsync++] = (int)(p.nonce & 1);
            }
        }
    }
    assert(nsync > 400);
    for (int i = 0; i < nsync; i++) assert(parity[i] == (i / 2) % 2);

    txsim_init(&tx, uid, 0, 0, 1);
    tx.connected = true;
    int last = -1, count = 0;
    for (int tick = 1; tick <= 80000; tick++) {
        txsim_pkt_t p;
        if (!txsim_next(&tx, &p)) continue;
        if (p.type == ELRS_PT_SYNC) {
            if (last >= 0) assert(tick - last >= 5000);
            last = tick;
            count++;
        }
    }
    assert(count >= 10);

    txsim_init(&tx, uid, 0, 0, 1);
    int gaps = 0, prev_gap = -1;
    for (int tick = 1; tick <= 12800; tick++) {
        txsim_pkt_t p;
        if (!txsim_next(&tx, &p)) {
            if (prev_gap >= 0) assert(tick - prev_gap == 128);
            prev_gap = tick;
            gaps++;
        }
    }
    assert(gaps == 100);
}

static void reboot_and_loss(void)
{
    static const uint8_t uid[6] = {0x4f, 0x04, 0xfd, 0x82, 0x21, 0x55};
    txsim_t tx;
    txsim_init(&tx, uid, 100.0, 0.5, 7);
    txsim_reboot(&tx, 1000, 3);
    int lost = 0, n = 0;
    double t0 = 0, t1 = 0;
    for (int i = 0; i < 4000; i++) {
        txsim_pkt_t p;
        if (!txsim_next(&tx, &p)) continue;
        if (!n) { t0 = p.t_us; assert(p.nonce > 1000 && p.ptr < ELRS_SEQ_LEN); }
        t1 = p.t_us;
        lost += p.lost;
        n++;
    }
    assert(lost > n / 3 && lost < 2 * n / 3);
    assert(t1 - t0 > 3900 * 1000.0 * 1.0001 * 0.99);
}

int main(void)
{
    golden();
    permutation();
    ptr_mapping();
    sync_cadence();
    reboot_and_loss();
    puts("ok");
    return 0;
}
