#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "elrs_fixture.h"
#include "fhss.h"
#include "ota3.h"

#define N ELRS_MAX_SAMPLES
#define LO_MHZ 2436
#define MAX_GOLD 128
#define MAX_SNAP 400

static const uint8_t UID[6] = {0, 0, 0, 0, 0x13, 0x3e};
static uint32_t words[N];
static elrs_iq16_t iq[N];
static elrs_dsp_work_t *work;

typedef struct {
    unsigned snap;
    double mhz;
    uint8_t pkt[8];
} gold_t;

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    assert(b && fread(b, 1, (size_t)n, f) == (size_t)n);
    b[n] = 0;
    fclose(f);
    return b;
}

static unsigned parse_list(const char *json, const char *key, unsigned *out)
{
    const char *p = strstr(json, key);
    assert(p);
    p = strchr(p, '[') + 1;
    unsigned n = 0;
    while (*p != ']') {
        char *end;
        out[n++] = (unsigned)strtoul(p, &end, 10);
        p = end;
        while (*p == ',' || *p == ' ') {
            p++;
        }
    }
    return n;
}

static unsigned parse_gold(const char *json, gold_t *g)
{
    unsigned n = 0;
    for (const char *p = strstr(json, "\"packets\""); p && (p = strstr(p, "{\"snap\":")); n++) {
        assert(n < MAX_GOLD);
        char *end;
        g[n].snap = (unsigned)strtoul(p + 8, &end, 10);
        p = strstr(end, "\"channelMhz\":");
        g[n].mhz = strtod(p + 13, &end);
        p = strstr(end, "\"hex\":\"") + 7;
        for (int i = 0; i < 8; i++) {
            char h[3] = {p[2 * i], p[2 * i + 1], 0};
            g[n].pkt[i] = (uint8_t)strtoul(h, NULL, 16);
        }
    }
    return n;
}

static size_t decode(const char *bin, unsigned snap, elrs_packet_t *out)
{
    elrs_decode_cfg_t cfg = {.lo_mhz = LO_MHZ, .polarity = ELRS_POL_INV};
    memcpy(cfg.uid, UID, 6);
    assert(fixture_load(bin, N, snap, words) == N);
    elrs_convert(words, iq, N, NULL);
    return elrs_decode_snapshot(iq, N, &cfg, work, out, ELRS_MAX_BURSTS, NULL);
}

static void test_rc(const char *bin, const char *json_path)
{
    char *json = slurp(json_path);
    static gold_t gold[MAX_GOLD];
    unsigned zero[MAX_SNAP], nzero = parse_list(json, "\"zeroBursts\"", zero);
    unsigned nsnap = (unsigned)strtoul(strstr(json, "\"snapshots\":") + 12, NULL, 10);
    unsigned ngold = parse_gold(json, gold);
    unsigned found = 0, c_total = 0;
    for (unsigned s = 0; s < nsnap; s++) {
        elrs_packet_t p[ELRS_MAX_BURSTS];
        size_t n = decode(bin, s, p);
        c_total += (unsigned)n;
        for (size_t k = 0; k < n; k++) {
            assert((p[k].pkt[0] & 3) != ELRS_PT_SYNC);
            uint16_t st[4];
            elrs_unpack_4x10(p[k].pkt + 1, st);
            assert(elrs_uint10_to_crsf(st[0]) == 992);
            assert(elrs_uint10_to_crsf(st[1]) == 996 || elrs_uint10_to_crsf(st[1]) == 997);
            assert(elrs_uint10_to_crsf(st[2]) == 174);
            assert(elrs_uint10_to_crsf(st[3]) == 992);
        }
        for (unsigned z = 0; z < nzero; z++) {
            assert(zero[z] != s || n == 0);
        }
        for (unsigned g = 0; g < ngold; g++) {
            if (gold[g].snap != s) {
                continue;
            }
            for (size_t k = 0; k < n; k++) {
                if (memcmp(p[k].pkt, gold[g].pkt, 8) == 0 &&
                    fabs(elrs_channel_khz(p[k].channel) / 1000.0 - gold[g].mhz) < 0.05) {
                    found++;
                    break;
                }
            }
        }
    }
    printf("caps: C %u packets, JS %u, matched %u (%.1f%%)\n", c_total, ngold, found, 100.0 * found / ngold);
    assert(found * 10 >= ngold * 9);
    free(json);
}

static void test_local(const char *bin)
{
    unsigned syncs = 0, rc = 0;
    for (unsigned s = 0;; s++) {
        elrs_packet_t p[ELRS_MAX_BURSTS];
        if (fixture_load(bin, N, s, words) != N) {
            break;
        }
        size_t n = decode(bin, s, p);
        for (size_t k = 0; k < n; k++) {
            if ((p[k].pkt[0] & 3) == ELRS_PT_SYNC) {
                elrs_sync_t sy;
                elrs_parse_sync(p[k].pkt, &sy);
                assert(sy.rate_index == 14 && sy.sw_mode == 1);
                syncs++;
            } else {
                rc++;
            }
        }
    }
    printf("caps_local: %u RC, %u SYNC\n", rc, syncs);
}

int main(int argc, char **argv)
{
    work = malloc(elrs_dsp_work_size());
    assert(work);
    if (argc > 2 && strcmp(argv[1], "--local") == 0) {
        test_local(argv[2]);
    } else {
        assert(argc > 2);
        test_rc(argv[1], argv[2]);
    }
    free(work);
    return 0;
}
