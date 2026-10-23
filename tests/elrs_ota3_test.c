#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota3.h"

static const uint8_t uid45[6] = {0, 0, 0, 0, 0x13, 0x3e};

static void golden(void)
{
    uint8_t ks[14] = {0};
    elrs_whiten(ks, 14);
    assert(!memcmp(ks, (uint8_t[]){0x00,0x88,0xc8,0xea,0xd8,0xe2,0x54,0x6c,0xf9,0xe2,0xdc,0xa4,0x13,0x3a}, 14));
    assert(elrs_crc_init(uid45) == 0x133d);
    assert(elrs_crc14((uint8_t[]){0,1,2,3,4,5,6}, 7, 0x133d) == 0x3e95);
    uint8_t pkt[8] = {0,1,2,3,4,5,6,0};
    elrs_finish_packet(pkt, 0x133d, 0);
    assert(!memcmp(pkt, (uint8_t[]){0xf8,0x01,0x02,0x03,0x04,0x05,0x06,0x95}, 8));
    uint8_t air[14];
    elrs_air_encode(pkt, air);
    assert(!memcmp(air, (uint8_t[]){0x46,0x4c,0x9a,0xfa,0xda,0xb7,0x57,0xec,0xbe,0xf3,0xcb,0xf0,0x05,0xbb}, 14));
}

static void round_trip(void)
{
    srand(1);
    for (int n = 0; n < 1000; n++) {
        uint8_t pkt[8], air[14], out[8];
        for (int i = 0; i < 7; i++) pkt[i] = (uint8_t)rand();
        pkt[7] = 0;
        elrs_finish_packet(pkt, 0x133d, (unsigned)(n % 3));
        elrs_air_encode(pkt, air);
        unsigned flips = (unsigned)rand() % 6, used = 0;
        unsigned done = 0;
        while (done < flips) {
            unsigned c = (unsigned)rand() % 16;
            if (used & (1u << c)) continue;
            used |= 1u << c;
            unsigned j = (unsigned)rand() % 7;
            air[2 * j + (c >> 3)] ^= (uint8_t)(1u << (c & 7));
            done++;
        }
        assert(elrs_air_decode(air, 0, out) == flips);
        assert(!memcmp(out, pkt, 8));
    }
}

static void erasures(void)
{
    srand(2);
    for (int n = 0; n < 200; n++) {
        uint8_t pkt[8], air[14], out[8];
        for (int i = 0; i < 8; i++) pkt[i] = (uint8_t)rand();
        elrs_air_encode(pkt, air);
        for (unsigned missing = 0; missing <= 31; missing++) {
            uint8_t rx[14];
            memcpy(rx, air, 14);
            for (unsigned p = 112 - missing; p < 112; p++) rx[p >> 3] &= (uint8_t)~(0x80 >> (p & 7));
            unsigned c = elrs_air_decode(rx, missing, out);
            if (missing <= 30) {
                assert(!memcmp(out, pkt, 8));
                assert(c == 0);
            }
        }
    }
    uint8_t z[14] = {0}, out[8];
    (void)elrs_air_decode(z, 200, out);
}

static void crc_slots(void)
{
    uint8_t pkt[8] = {0, 1, 2, 3, 4, 5, 6, 0};
    elrs_finish_packet(pkt, 0x133d, 0);
    assert(elrs_crc_slot(pkt, 0x133d) == 0);
    for (unsigned s = 1; s <= 2; s++) {
        uint8_t p[8] = {0, 1, 2, 3, 4, 5, 6, 0};
        elrs_finish_packet(p, 0x133d, s);
        assert(elrs_crc_slot(p, 0x133d) == (int)s);
    }
    uint8_t sync[8] = {ELRS_PT_SYNC, 160, 77, 0xe3, 0xaa, 0x13, 0x3b, 0};
    elrs_finish_packet(sync, 0x133d, 0);
    assert(elrs_crc_slot(sync, 0x133d) == 0);
    sync[3] ^= 0x10;
    assert(elrs_crc_slot(sync, 0x133d) == -1);
    pkt[2] ^= 1;
    assert(elrs_crc_slot(pkt, 0x133d) == -1);
}

static void sync_layout(void)
{
    uint8_t pkt[8] = {ELRS_PT_SYNC, 160, 77, 0xe3, 0xaa, 0x13, 0x3e ^ (~5 & 0x3f), 0};
    elrs_finish_packet(pkt, 0x133d, 0);
    elrs_sync_t s;
    elrs_parse_sync(pkt, &s);
    assert(s.fhss_index == 160 && s.nonce == 77 && s.sw_mode == 1 && s.tlm_ratio == 1 && s.rate_index == 14);
    uint8_t uid[6] = {0, 0, 0, 0xaa, 0x13, 0x3e}, model = 0xff;
    assert(elrs_sync_uid_ok(&s, uid, &model) && model == 5);
    uid[3] = 0xab;
    assert(!elrs_sync_uid_ok(&s, uid, &model));
}

static void hybrid8(void)
{
    static const uint16_t sw3[8] = {191, 631, 871, 1111, 1351, 1792, 992, 992};
    for (unsigned idx = 0; idx < 6; idx++)
        for (unsigned v = 0; v < 8; v++) {
            uint8_t aux = 0;
            uint16_t c = 0;
            assert(elrs_hybrid8((uint8_t)(idx << 3 | v), &aux, &c));
            assert(aux == 2 + idx && c == sw3[v]);
        }
    uint8_t aux;
    uint16_t c;
    assert(elrs_hybrid8(0x37, &aux, &c) && aux == 8 && c == 938);
    assert(elrs_hybrid8(0x38, &aux, &c) && aux == 8 && c == 1044);
    assert(elrs_hybrid8(0x30, &aux, &c) && c == 191);
    assert(elrs_hybrid8(0x3f, &aux, &c) && c == 1792);
}

static void sticks(void)
{
    static const uint16_t want[4] = {992, 996, 174, 992}, v10[4] = {512, 514, 1, 512};
    uint8_t raw[5] = {0};
    unsigned shift = 0, idx = 0;
    for (int i = 0; i < 4; i++) {
        raw[idx] |= (uint8_t)((v10[i] << shift) & 0xff);
        unsigned left = 2 + shift;
        raw[idx + 1] = (uint8_t)(v10[i] >> (10 - left));
        idx++;
        shift = left;
    }
    assert(!memcmp(raw, (uint8_t[]){0x00,0x0a,0x18,0x00,0x80}, 5));
    uint16_t out[4];
    elrs_unpack_4x10(raw, out);
    for (int i = 0; i < 4; i++) {
        assert(out[i] == v10[i]);
        assert(elrs_uint10_to_crsf(out[i]) == want[i]);
    }
    assert(elrs_uint10_to_crsf(0) == 172 && elrs_uint10_to_crsf(1023) == 1811);
}

int main(void)
{
    golden();
    round_trip();
    erasures();
    crc_slots();
    sync_layout();
    hybrid8();
    sticks();
    puts("ok");
    return 0;
}
