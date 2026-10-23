#ifndef ELRS_OTA3_H
#define ELRS_OTA3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ELRS_PT_RC = 0, ELRS_PT_MSP = 1, ELRS_PT_SYNC = 2, ELRS_PT_TLM = 3 };
#define ELRS_AIR_BYTES 14

/* 9-bit LFSR x^9+x^5+1, seed 0x100, bit-reversed keystream; involution. */
void elrs_whiten(uint8_t *buf, size_t n);
/* Hamming(7,4) over 16 nibbles, bit-interleaved into 14 bytes. */
void elrs_fec_encode(const uint8_t pkt[8], uint8_t air[14]);
/* Returns corrected-bit count; the last missing_tail_bits air bits (MSB-first) are ignored. */
unsigned elrs_fec_decode(const uint8_t air[14], unsigned missing_tail_bits, uint8_t pkt[8]);
/* FEC then whiten. */
void elrs_air_encode(const uint8_t pkt[8], uint8_t air[14]);
/* Whiten then FEC. */
unsigned elrs_air_decode(const uint8_t air[14], unsigned missing_tail_bits, uint8_t pkt[8]);

uint16_t elrs_crc14(const uint8_t *data, size_t n, uint16_t init);
uint16_t elrs_crc_init(const uint8_t uid[6]);
/* 0: valid with crcHigh field 0 (Hybrid RC or any non-RC type); 1 or 2: RC valid only with that
 * Wide slot; -1: invalid. */
int elrs_crc_slot(const uint8_t pkt[8], uint16_t init);
/* Fills crcHigh/crcLow over bytes 0..6 with byte0 = type | slot << 2. */
void elrs_finish_packet(uint8_t pkt[8], uint16_t init, unsigned slot);

typedef struct {
    uint8_t fhss_index, nonce, sw_mode, tlm_ratio, rate_index, uid3, uid4, uid5x;
} elrs_sync_t;
void elrs_parse_sync(const uint8_t pkt[8], elrs_sync_t *s);
bool elrs_sync_uid_ok(const elrs_sync_t *s, const uint8_t uid[6], uint8_t *model_id);

void elrs_unpack_4x10(const uint8_t raw[5], uint16_t out[4]);
uint16_t elrs_uint10_to_crsf(uint16_t v);
/* aux 2..8; Hybrid8 switch word to CRSF value. */
bool elrs_hybrid8(uint8_t switches7, uint8_t *aux, uint16_t *crsf);

#endif
