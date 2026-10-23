#include "frames.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static int finish(char *buf, size_t cap, int n)
{
    if (n < 0 || (size_t)n >= cap) {
        if (cap) buf[cap - 1] = 0;
        return -1;
    }
    return n;
}

static int append(char *buf, size_t cap, int pos, const char *fmt, ...)
{
    if (pos < 0) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, pos < (int)cap ? cap - (size_t)pos : 0, fmt, ap);
    va_end(ap);
    return n < 0 ? -1 : pos + n;
}

int elrs_fmt_packet(char *buf, size_t cap, uint32_t t_us, const elrs_packet_t *p, int nonce, const uint8_t uid[6])
{
    if (!cap) return -1;
    unsigned type = p->pkt[0] & 3;
    char tc = type == ELRS_PT_RC ? 'R' : type == ELRS_PT_SYNC ? 'S' : 'M';
    int n = snprintf(buf, cap, "P %lu %u %c ", (unsigned long)t_us, (unsigned)p->channel, tc);
    if (nonce < 0) n = append(buf, cap, n, "-");
    else n = append(buf, cap, n, "%d", nonce);
    n = append(buf, cap, n, " %ld %.1f %u ", lroundf(p->cfo_hz / 1000.0f), (double)p->snr_db, (unsigned)p->fec);
    if (type == ELRS_PT_SYNC) {
        elrs_sync_t s;
        elrs_parse_sync(p->pkt, &s);
        uint8_t mm;
        n = append(buf, cap, n, "%u,%u,%u,%u,%u,", (unsigned)s.fhss_index, (unsigned)s.nonce, (unsigned)s.rate_index,
                   (unsigned)s.tlm_ratio, (unsigned)s.sw_mode);
        if (elrs_sync_uid_ok(&s, uid, &mm)) n = append(buf, cap, n, "%u", (unsigned)mm);
        else n = append(buf, cap, n, "-");
    } else {
        for (int i = 0; i < 8; i++) n = append(buf, cap, n, "%02x", (unsigned)p->pkt[i]);
    }
    n = append(buf, cap, n, "\n");
    return finish(buf, cap, n);
}

int elrs_fmt_stats(char *buf, size_t cap, const elrs_stats_t *s)
{
    if (!cap) return -1;
    int n = snprintf(buf, cap,
                     "S up=%lu mode=%s state=%s snaps=%u bursts=%u ok=%u rc=%u sync=%u crcfail=%u mirror=%u "
                     "dec_us=%u cap_us=%u ret_us=%u lock_err_us=%d cfo_khz=%d gain=%u drop=%u\n",
                     (unsigned long)s->up_s, elrs_mode_name(s->mode), s->state, s->snaps, s->bursts, s->ok, s->rc,
                     s->sync, s->crcfail, s->mirror, s->dec_us, s->cap_us, s->ret_us, s->lock_err_us, s->cfo_khz,
                     s->gain, s->drop);
    return finish(buf, cap, n);
}

int elrs_fmt_lock(char *buf, size_t cap, const char *state, const char *reason)
{
    if (!cap) return -1;
    return finish(buf, cap, snprintf(buf, cap, "L %s %s\n", state, reason));
}

int elrs_fmt_warn(char *buf, size_t cap, const char *text)
{
    if (!cap) return -1;
    return finish(buf, cap, snprintf(buf, cap, "W %s\n", text));
}

int elrs_fmt_spectrum(char *buf, size_t cap, uint32_t t_us, unsigned lo_mhz, const uint8_t bins[ELRS_FFT_N])
{
    if (!cap) return -1;
    int n = snprintf(buf, cap, "F %lu %u ", (unsigned long)t_us, lo_mhz);
    for (int j = 0; j < ELRS_FFT_N; j++) n = append(buf, cap, n, "%02x", (unsigned)bins[j]);
    n = append(buf, cap, n, "\n");
    return finish(buf, cap, n);
}
