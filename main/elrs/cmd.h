#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { ELRS_MODE_AUTO, ELRS_MODE_SCAN, ELRS_MODE_TIMED, ELRS_MODE_HOP } elrs_mode_t;
typedef enum { ELRS_POL_INV, ELRS_POL_AUTO } elrs_polarity_t;
typedef enum {
    ELRS_CMD_INVALID, ELRS_CMD_INFO, ELRS_CMD_CAPS, ELRS_CMD_SYNC, ELRS_CMD_TRANSPORT,
    ELRS_CMD_UID_SET, ELRS_CMD_UID_QUERY, ELRS_CMD_UID_SAVE, ELRS_CMD_UID_CLEAR,
    ELRS_CMD_MODE, ELRS_CMD_MODE_QUERY, ELRS_CMD_PARK, ELRS_CMD_PARK_QUERY,
    ELRS_CMD_GAIN_HARDWARE, ELRS_CMD_GAIN_MANUAL, ELRS_CMD_GAIN_QUERY,
    ELRS_CMD_RX_ON, ELRS_CMD_RX_OFF, ELRS_CMD_STATS, ELRS_CMD_BENCH, ELRS_CMD_RETUNE,
    ELRS_CMD_LEAD, ELRS_CMD_SNAP16, ELRS_CMD_POLARITY, ELRS_CMD_SPECTRUM
} elrs_cmd_kind_t;

typedef struct {
    elrs_cmd_kind_t kind;
    const char *err; /* INVALID only: "uid", "args" or "command" */
    union {
        uint8_t uid[6];
        elrs_mode_t mode;
        elrs_polarity_t polarity;
        bool on;
        uint64_t nonce;
        unsigned park_mhz, gain, bench_n, lead_us, snap_samples;
        struct { unsigned mhz_a, mhz_b, n; bool fast; } retune;
    } u;
} elrs_cmd_t;

void elrs_cmd_parse(const char *line, elrs_cmd_t *out);
const char *elrs_mode_name(elrs_mode_t m);
