#include "cmd.h"
#include <stddef.h>
#include <string.h>

#define MAX_TOKENS 6

typedef struct { const char *p; size_t n; } token_t;

static bool is(token_t t, const char *word) {
    return strlen(word) == t.n && !memcmp(t.p, word, t.n);
}

/* Strict decimal: no sign, no whitespace, no empty string. */
static bool number(token_t t, uint64_t max, uint64_t *out) {
    if (!t.n) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < t.n; i++) {
        unsigned d = (unsigned)(t.p[i] - '0');
        if (d > 9 || v > (max - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

static bool ranged(token_t t, unsigned lo, unsigned hi, unsigned *out) {
    uint64_t v;
    if (!number(t, hi, &v) || v < lo) return false;
    *out = (unsigned)v;
    return true;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool uid_token(token_t t, uint8_t uid[6]) {
    if (t.n != 12) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex_digit(t.p[2 * i]), lo = hex_digit(t.p[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        uid[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static void fail(elrs_cmd_t *out, const char *err) {
    out->kind = ELRS_CMD_INVALID;
    out->err = err;
}

static void done(elrs_cmd_t *out, elrs_cmd_kind_t kind) {
    out->kind = kind;
}

void elrs_cmd_parse(const char *line, elrs_cmd_t *out) {
    memset(out, 0, sizeof(*out));
    token_t t[MAX_TOKENS];
    size_t count = 0;
    bool overflow = false;
    for (const char *s = line; *s;) {
        if (*s == ' ') { s++; continue; }
        const char *start = s;
        while (*s && *s != ' ') s++;
        if (count == MAX_TOKENS) { overflow = true; break; }
        t[count++] = (token_t){start, (size_t)(s - start)};
    }
    if (!count) { fail(out, "command"); return; }

    token_t v = t[0];
    size_t argc = count - 1;
    unsigned n;
    uint64_t big;

#define NOARGS(kind) do { if (argc || overflow) fail(out, "args"); else done(out, kind); return; } while (0)
    if (is(v, "INFO")) NOARGS(ELRS_CMD_INFO);
    if (is(v, "CAPS")) NOARGS(ELRS_CMD_CAPS);
    if (is(v, "TRANSPORT?")) NOARGS(ELRS_CMD_TRANSPORT);
    if (is(v, "STATS") || is(v, "STATS?")) NOARGS(ELRS_CMD_STATS);
    if (is(v, "UID?")) NOARGS(ELRS_CMD_UID_QUERY);
    if (is(v, "MODE?")) NOARGS(ELRS_CMD_MODE_QUERY);
    if (is(v, "PARK?")) NOARGS(ELRS_CMD_PARK_QUERY);
    if (is(v, "GAIN?")) NOARGS(ELRS_CMD_GAIN_QUERY);
#undef NOARGS

    if (overflow) {
        fail(out, is(v, "UID") || is(v, "SYNC") || is(v, "PARK") || is(v, "MODE") ||
                          is(v, "GAIN") || is(v, "RX") || is(v, "BENCH") || is(v, "RETUNE") ||
                          is(v, "LEAD") || is(v, "SNAP16") || is(v, "POLARITY")
                      ? "args" : "command");
        return;
    }

    if (is(v, "SYNC")) {
        if (argc != 1 || !number(t[1], UINT64_MAX, &big)) { fail(out, "args"); return; }
        out->u.nonce = big;
        { done(out, ELRS_CMD_SYNC); return; }
    }
    if (is(v, "UID")) {
        if (!argc) { fail(out, "uid"); return; }
        if (is(t[1], "SAVE")) { if (argc == 1) done(out, ELRS_CMD_UID_SAVE); else fail(out, "args"); return; }
        if (is(t[1], "CLEAR")) { if (argc == 1) done(out, ELRS_CMD_UID_CLEAR); else fail(out, "args"); return; }
        if (!uid_token(t[1], out->u.uid)) {
            memset(out->u.uid, 0, sizeof(out->u.uid));
            { fail(out, "uid"); return; }
        }
        if (argc != 1) {
            memset(out->u.uid, 0, sizeof(out->u.uid));
            { fail(out, "args"); return; }
        }
        { done(out, ELRS_CMD_UID_SET); return; }
    }
    if (is(v, "PARK")) {
        if (argc != 1 || !ranged(t[1], 2402, 2478, &n)) { fail(out, "args"); return; }
        out->u.park_mhz = n;
        { done(out, ELRS_CMD_PARK); return; }
    }
    if (is(v, "MODE")) {
        if (argc != 1) { fail(out, "args"); return; }
        elrs_mode_t m;
        if (is(t[1], "AUTO")) m = ELRS_MODE_AUTO;
        else if (is(t[1], "SCAN")) m = ELRS_MODE_SCAN;
        else if (is(t[1], "TIMED")) m = ELRS_MODE_TIMED;
        else if (is(t[1], "HOP")) m = ELRS_MODE_HOP;
        else { fail(out, "args"); return; }
        out->u.mode = m;
        { done(out, ELRS_CMD_MODE); return; }
    }
    if (is(v, "GAIN")) {
        if (argc == 1 && is(t[1], "HARDWARE")) { done(out, ELRS_CMD_GAIN_HARDWARE); return; }
        if (argc == 2 && is(t[1], "MANUAL") && ranged(t[2], 0, 127, &n)) {
            out->u.gain = n;
            { done(out, ELRS_CMD_GAIN_MANUAL); return; }
        }
        { fail(out, "args"); return; }
    }
    if (is(v, "RX")) {
        if (argc == 1 && is(t[1], "ON")) { done(out, ELRS_CMD_RX_ON); return; }
        if (argc == 1 && is(t[1], "OFF")) { done(out, ELRS_CMD_RX_OFF); return; }
        { fail(out, "args"); return; }
    }
    if (is(v, "BENCH")) {
        if (argc != 1 || !ranged(t[1], 1, 2000, &n)) { fail(out, "args"); return; }
        out->u.bench_n = n;
        { done(out, ELRS_CMD_BENCH); return; }
    }
    if (is(v, "RETUNE")) {
        unsigned a, b, count_n;
        if (argc != 4 || !ranged(t[1], 2402, 2478, &a) || !ranged(t[2], 2402, 2478, &b) ||
            !ranged(t[3], 1, 10000, &count_n) || (!is(t[4], "FULL") && !is(t[4], "FAST")))
            { fail(out, "args"); return; }
        out->u.retune.mhz_a = a;
        out->u.retune.mhz_b = b;
        out->u.retune.n = count_n;
        out->u.retune.fast = is(t[4], "FAST");
        { done(out, ELRS_CMD_RETUNE); return; }
    }
    if (is(v, "LEAD")) {
        if (argc != 1 || !ranged(t[1], 0, 5000, &n)) { fail(out, "args"); return; }
        out->u.lead_us = n;
        { done(out, ELRS_CMD_LEAD); return; }
    }
    if (is(v, "SNAP16")) {
        if (argc != 1 || !ranged(t[1], 256, 16380, &n)) { fail(out, "args"); return; }
        out->u.snap_samples = n;
        { done(out, ELRS_CMD_SNAP16); return; }
    }
    if (is(v, "POLARITY")) {
        if (argc == 1 && is(t[1], "INV")) out->u.polarity = ELRS_POL_INV;
        else if (argc == 1 && is(t[1], "AUTO")) out->u.polarity = ELRS_POL_AUTO;
        else { fail(out, "args"); return; }
        { done(out, ELRS_CMD_POLARITY); return; }
    }
    fail(out, "command");
}

const char *elrs_mode_name(elrs_mode_t m) {
    switch (m) {
    case ELRS_MODE_AUTO: return "AUTO";
    case ELRS_MODE_SCAN: return "SCAN";
    case ELRS_MODE_TIMED: return "TIMED";
    case ELRS_MODE_HOP: return "HOP";
    }
    return "AUTO";
}
