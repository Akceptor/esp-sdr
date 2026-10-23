#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "frames.h"

static const uint8_t uid45[6] = {0, 0, 0, 0xAA, 0x13, 0x3e};

static void rc_line(void)
{
    elrs_packet_t p = { .pkt = {0xf8,1,2,3,4,5,6,0x95}, .channel = 36, .cfo_hz = 240400, .snr_db = 18.26f, .fec = 1 };
    char b[128];
    int n = elrs_fmt_packet(b, sizeof b, 123456, &p, -1, uid45);
    assert(!strcmp(b, "P 123456 36 R - 240 18.3 1 f801020304050695\n"));
    assert(n == (int)strlen(b));
    p.cfo_hz = -240400;
    elrs_fmt_packet(b, sizeof b, 1, &p, 9, uid45);
    assert(!strcmp(b, "P 1 36 R 9 -240 18.3 1 f801020304050695\n"));
    p.pkt[0] = 0x01;
    elrs_fmt_packet(b, sizeof b, 1, &p, 9, uid45);
    assert(!strncmp(b, "P 1 36 M 9 ", 11));
    p.pkt[0] = 0x03;
    elrs_fmt_packet(b, sizeof b, 1, &p, 9, uid45);
    assert(!strncmp(b, "P 1 36 M 9 ", 11));
}

static void sync_line(void)
{
    uint8_t pkt[8] = {ELRS_PT_SYNC, 160, 77, 0xE3, 0xAA, 0x13, (uint8_t)(0x3e ^ (~5 & 0x3f)), 0};
    elrs_finish_packet(pkt, elrs_crc_init(uid45), 0);
    elrs_packet_t p = { .channel = 41, .cfo_hz = 240000, .snr_db = 18.26f, .fec = 0 };
    memcpy(p.pkt, pkt, 8);
    char b[128];
    elrs_fmt_packet(b, sizeof b, 5000, &p, 77, uid45);
    assert(!strcmp(b, "P 5000 41 S 77 240 18.3 0 160,77,14,1,1,5\n"));
    assert(!strstr(b, "aa") && !strstr(b, "13") && !strstr(b, "3e") && !strstr(b, "170") && !strstr(b, "62"));

    uint8_t bad[6] = {0, 0, 0, 0x11, 0x22, 0x33};
    elrs_fmt_packet(b, sizeof b, 5000, &p, 77, bad);
    assert(!strcmp(b, "P 5000 41 S 77 240 18.3 0 160,77,14,1,1,-\n"));
}

static void stats_line(void)
{
    elrs_stats_t s = { .up_s = 12, .mode = ELRS_MODE_HOP, .state = "lock", .snaps = 1, .bursts = 2, .ok = 3, .rc = 4,
                       .sync = 5, .crcfail = 6, .mirror = 7, .dec_us = 8, .cap_us = 9, .ret_us = 10,
                       .lock_err_us = -11, .cfo_khz = -12, .gain = 13, .drop = 14 };
    char b[256];
    elrs_fmt_stats(b, sizeof b, &s);
    char want[256];
    snprintf(want, sizeof want, "S up=12 mode=%s state=lock snaps=1 bursts=2 ok=3 rc=4 sync=5 crcfail=6 mirror=7 "
             "dec_us=8 cap_us=9 ret_us=10 lock_err_us=-11 cfo_khz=-12 gain=13 drop=14\n", elrs_mode_name(ELRS_MODE_HOP));
    assert(!strcmp(b, want));
}

static void misc_lines(void)
{
    char b[64];
    assert(elrs_fmt_lock(b, sizeof b, "lock", "ok") == 10 && !strcmp(b, "L lock ok\n"));
    assert(elrs_fmt_warn(b, sizeof b, "hello") == 8 && !strcmp(b, "W hello\n"));
}

static void small_cap(void)
{
    elrs_packet_t p = { .pkt = {0xf8,1,2,3,4,5,6,0x95}, .channel = 36 };
    char b[16];
    memset(b, 'x', sizeof b);
    assert(elrs_fmt_packet(b, sizeof b, 123456, &p, -1, uid45) == -1);
    assert(memchr(b, 0, sizeof b));
    memset(b, 'x', sizeof b);
    assert(elrs_fmt_warn(b, 4, "hello") == -1 && b[3] == 0);
    assert(elrs_fmt_lock(b, 0, "a", "b") == -1);
}

static void spectrum_line(void)
{
    uint8_t bins[64];
    for (int j = 0; j < 64; j++) bins[j] = (uint8_t)(j * 4);
    char f[160];
    assert(elrs_fmt_spectrum(f, sizeof f, 5000, 2438, bins) == 141);
    assert(!strcmp(f, "F 5000 2438 "
        "0004080c1014181c2024282c3034383c4044484c5054585c6064686c7074787c"
        "8084888c9094989ca0a4a8acb0b4b8bcc0c4c8ccd0d4d8dce0e4e8ecf0f4f8fc\n"));
    assert(elrs_fmt_spectrum(f, 141, 5000, 2438, bins) == -1 && memchr(f, 0, 141));
    assert(elrs_fmt_spectrum(f, 142, 5000, 2438, bins) == 141);
}

int main(void)
{
    spectrum_line();
    rc_line();
    sync_line();
    stats_line();
    misc_lines();
    small_cap();
    puts("frames ok");
    return 0;
}
