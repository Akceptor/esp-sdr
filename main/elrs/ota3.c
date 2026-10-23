#include "ota3.h"

#include <string.h>

#define CRC14_POLY 0x2e57u

static const uint8_t HAMMING_CODES[16] = {
    0x00, 0x71, 0x62, 0x13, 0x54, 0x25, 0x36, 0x47,
    0x38, 0x49, 0x5a, 0x2b, 0x6c, 0x1d, 0x0e, 0x7f};

static unsigned popcount8(unsigned v)
{
    unsigned c = 0;
    for (; v; v &= v - 1) c++;
    return c;
}

void elrs_whiten(uint8_t *buf, size_t n)
{
    unsigned lfsr = 0x100;
    for (size_t k = 0; k < n; k++) {
        uint8_t b = (uint8_t)lfsr, r = 0;
        for (int i = 0; i < 8; i++) r |= (uint8_t)(((b >> i) & 1) << (7 - i));
        buf[k] ^= r;
        for (int s = 0; s < 8; s++) {
            unsigned fb = (lfsr ^ (lfsr >> 5)) & 1;
            lfsr = (lfsr >> 1) | (fb << 8);
        }
    }
}

void elrs_fec_encode(const uint8_t pkt[8], uint8_t air[14])
{
    uint8_t enc[16];
    for (int i = 0; i < 8; i++) {
        enc[i * 2] = HAMMING_CODES[pkt[i] & 0x0f];
        enc[i * 2 + 1] = HAMMING_CODES[pkt[i] >> 4];
    }
    memset(air, 0, ELRS_AIR_BYTES);
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 8; j++) {
            air[i * 2] |= (uint8_t)(((enc[j] >> i) & 1) << j);
            air[i * 2 + 1] |= (uint8_t)(((enc[j + 8] >> i) & 1) << j);
        }
    }
}

unsigned elrs_fec_decode(const uint8_t air[14], unsigned missing_tail_bits, uint8_t pkt[8])
{
    uint8_t enc[16] = {0}, erased[16] = {0};
    if (missing_tail_bits > 112) missing_tail_bits = 112;
    for (unsigned p = 112 - missing_tail_bits; p < 112; p++) {
        unsigned byte = p >> 3, i = 7 - (p & 7);
        erased[i + 8 * (byte & 1)] |= (uint8_t)(1u << (byte >> 1));
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 7; j++) {
            enc[i] |= (uint8_t)(((air[j * 2] >> i) & 1) << j);
            enc[i + 8] |= (uint8_t)(((air[j * 2 + 1] >> i) & 1) << j);
        }
    }
    unsigned mism = 0;
    uint8_t nib[16];
    for (int c = 0; c < 16; c++) {
        unsigned keep = 0x7fu & ~(unsigned)erased[c], best = 0, best_d = 99;
        for (unsigned v = 0; v < 16; v++) {
            unsigned d = popcount8((HAMMING_CODES[v] ^ enc[c]) & keep);
            if (d < best_d) { best_d = d; best = v; }
        }
        mism += best_d;
        nib[c] = (uint8_t)best;
    }
    for (int i = 0; i < 8; i++) pkt[i] = (uint8_t)(nib[i * 2] | (nib[i * 2 + 1] << 4));
    return mism;
}

void elrs_air_encode(const uint8_t pkt[8], uint8_t air[14])
{
    elrs_fec_encode(pkt, air);
    elrs_whiten(air, ELRS_AIR_BYTES);
}

unsigned elrs_air_decode(const uint8_t air[14], unsigned missing_tail_bits, uint8_t pkt[8])
{
    uint8_t tmp[ELRS_AIR_BYTES];
    memcpy(tmp, air, sizeof tmp);
    elrs_whiten(tmp, sizeof tmp);
    return elrs_fec_decode(tmp, missing_tail_bits, pkt);
}

uint16_t elrs_crc14(const uint8_t *data, size_t n, uint16_t init)
{
    unsigned crc = init;
    for (size_t k = 0; k < n; k++) {
        unsigned t = ((crc >> 6) ^ data[k]) & 0xff;
        unsigned e = t << 6;
        for (int b = 0; b < 8; b++) e = ((e << 1) ^ ((e & 0x2000) ? CRC14_POLY : 0)) & 0xffff;
        crc = ((crc << 8) & 0xffff) ^ e;
    }
    return (uint16_t)(crc & 0x3fff);
}

uint16_t elrs_crc_init(const uint8_t uid[6])
{
    return (uint16_t)((((unsigned)uid[4] << 8) | uid[5]) ^ 3);
}

static uint16_t slot_crc(const uint8_t pkt[8], uint16_t init, unsigned slot)
{
    uint8_t head[7];
    memcpy(head, pkt, 7);
    head[0] = (uint8_t)((pkt[0] & 3) | (slot << 2));
    return elrs_crc14(head, 7, init);
}

int elrs_crc_slot(const uint8_t pkt[8], uint16_t init)
{
    unsigned in = ((unsigned)(pkt[0] >> 2) << 8) | pkt[7];
    unsigned max_slot = (pkt[0] & 3) == ELRS_PT_RC ? 2 : 0;
    for (unsigned s = 0; s <= max_slot; s++)
        if (in == slot_crc(pkt, init, s)) return (int)s;
    return -1;
}

void elrs_finish_packet(uint8_t pkt[8], uint16_t init, unsigned slot)
{
    uint16_t crc = slot_crc(pkt, init, slot);
    pkt[0] = (uint8_t)((pkt[0] & 3) | ((crc >> 8) << 2));
    pkt[7] = (uint8_t)(crc & 0xff);
}

void elrs_parse_sync(const uint8_t pkt[8], elrs_sync_t *s)
{
    s->fhss_index = pkt[1];
    s->nonce = pkt[2];
    s->sw_mode = pkt[3] & 1;
    s->tlm_ratio = (pkt[3] >> 1) & 7;
    s->rate_index = pkt[3] >> 4;
    s->uid3 = pkt[4];
    s->uid4 = pkt[5];
    s->uid5x = pkt[6];
}

bool elrs_sync_uid_ok(const elrs_sync_t *s, const uint8_t uid[6], uint8_t *model_id)
{
    uint8_t x = (uint8_t)(s->uid5x ^ uid[5]);
    if (s->uid3 != uid[3] || s->uid4 != uid[4] || (x & 0xc0)) return false;
    if (model_id) *model_id = (uint8_t)(~x & 0x3f);
    return true;
}

void elrs_unpack_4x10(const uint8_t raw[5], uint16_t out[4])
{
    unsigned merged = 0, value = 0, ri = 0;
    for (int n = 0; n < 4; n++) {
        while (merged < 10) {
            value |= (unsigned)raw[ri++] << merged;
            merged += 8;
        }
        out[n] = (uint16_t)(value & 0x3ff);
        value >>= 10;
        merged -= 10;
    }
}

uint16_t elrs_uint10_to_crsf(uint16_t v)
{
    /* fmap(v,0,1023,172,1811): floor(2*v*1639/1023 + 345) >> 1, in exact integers */
    unsigned n = 2u * v * 1639u + 345u * 1023u;
    unsigned r = (n / 1023u) >> 1;
    return (uint16_t)(r > 65535 ? 65535 : r);
}

bool elrs_hybrid8(uint8_t switches7, uint8_t *aux, uint16_t *crsf)
{
    unsigned sw = switches7 & 0x7f, idx = (sw >> 3) & 7;
    if (idx >= 6) {
        *aux = 8;
        *crsf = (uint16_t)((sw & 15) * 1601 / 15 + 191);
        return true;
    }
    unsigned v = sw & 7;
    *aux = (uint8_t)(2 + idx);
    *crsf = v == 0 ? 191 : v == 5 ? 1792 : v >= 6 ? 992 : (uint16_t)(v * 240 + 391);
    return true;
}
