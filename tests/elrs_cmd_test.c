#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "cmd.h"

static elrs_cmd_t c;
#define P(s) elrs_cmd_parse(s, &c)
#define INVALID(s, e) do { P(s); assert(c.kind == ELRS_CMD_INVALID && !strcmp(c.err, e)); } while (0)

int main(void) {
    P("INFO");                 assert(c.kind == ELRS_CMD_INFO);
    P("CAPS");                 assert(c.kind == ELRS_CMD_CAPS);
    P("TRANSPORT?");           assert(c.kind == ELRS_CMD_TRANSPORT);
    P("UID 00000000133E");     assert(c.kind == ELRS_CMD_UID_SET && c.u.uid[4] == 0x13 && c.u.uid[5] == 0x3e);
    P("UID 00000000133e");     assert(c.kind == ELRS_CMD_UID_SET && c.u.uid[5] == 0x3e);
    P("UID ffFFffFFffFF");     assert(c.kind == ELRS_CMD_UID_SET && c.u.uid[0] == 0xff && c.u.uid[5] == 0xff);
    INVALID("UID 00000000133", "uid");
    INVALID("UID 00000000133e0", "uid");
    INVALID("UID 0000000013xe", "uid");
    INVALID("UID", "uid");
    INVALID("UID 00000000133e x", "args");
    P("UID?");                 assert(c.kind == ELRS_CMD_UID_QUERY);
    P("UID SAVE");             assert(c.kind == ELRS_CMD_UID_SAVE);
    P("UID CLEAR");            assert(c.kind == ELRS_CMD_UID_CLEAR);
    P("PARK 2402");            assert(c.kind == ELRS_CMD_PARK && c.u.park_mhz == 2402);
    P("PARK 2478");            assert(c.kind == ELRS_CMD_PARK && c.u.park_mhz == 2478);
    INVALID("PARK 2401", "args");
    INVALID("PARK 2479", "args");
    INVALID("PARK 2438 x", "args");
    INVALID("PARK 99999999999999999999999", "args");
    INVALID("RETUNE 2402 2478 10 FAST x y", "args");
    INVALID("FOO 1 2 3 4 5 6 7", "command");
    P("PARK?");                assert(c.kind == ELRS_CMD_PARK_QUERY);
    P("MODE HOP");             assert(c.kind == ELRS_CMD_MODE && c.u.mode == ELRS_MODE_HOP);
    P("MODE AUTO");            assert(c.u.mode == ELRS_MODE_AUTO);
    P("MODE SCAN");            assert(c.u.mode == ELRS_MODE_SCAN);
    P("MODE TIMED");           assert(c.u.mode == ELRS_MODE_TIMED);
    INVALID("MODE FOO", "args");
    INVALID("MODE HOP x", "args");
    P("MODE?");                assert(c.kind == ELRS_CMD_MODE_QUERY);
    P("GAIN HARDWARE");        assert(c.kind == ELRS_CMD_GAIN_HARDWARE);
    P("GAIN MANUAL 127");      assert(c.kind == ELRS_CMD_GAIN_MANUAL && c.u.gain == 127);
    P("GAIN MANUAL 0");        assert(c.kind == ELRS_CMD_GAIN_MANUAL && c.u.gain == 0);
    INVALID("GAIN MANUAL 128", "args");
    INVALID("GAIN MANUAL", "args");
    P("GAIN?");                assert(c.kind == ELRS_CMD_GAIN_QUERY);
    P("RX ON");                assert(c.kind == ELRS_CMD_RX_ON);
    P("RX OFF");               assert(c.kind == ELRS_CMD_RX_OFF);
    INVALID("RX ON x", "args");
    INVALID("RX", "args");
    P("STATS");                assert(c.kind == ELRS_CMD_STATS);
    P("STATS?");               assert(c.kind == ELRS_CMD_STATS);
    P("BENCH 1");              assert(c.kind == ELRS_CMD_BENCH && c.u.bench_n == 1);
    P("BENCH 2000");           assert(c.kind == ELRS_CMD_BENCH && c.u.bench_n == 2000);
    INVALID("BENCH 0", "args");
    INVALID("BENCH 2001", "args");
    P("RETUNE 2402 2478 1000 FAST");
    assert(c.kind == ELRS_CMD_RETUNE && c.u.retune.fast && c.u.retune.n == 1000 &&
           c.u.retune.mhz_a == 2402 && c.u.retune.mhz_b == 2478);
    P("RETUNE 2410 2420 10000 FULL");
    assert(c.kind == ELRS_CMD_RETUNE && !c.u.retune.fast);
    INVALID("RETUNE 2401 2478 10 FAST", "args");
    INVALID("RETUNE 2402 2479 10 FAST", "args");
    INVALID("RETUNE 2402 2478 0 FAST", "args");
    INVALID("RETUNE 2402 2478 10001 FAST", "args");
    INVALID("RETUNE 2402 2478 10 SLOW", "args");
    P("LEAD 0");               assert(c.kind == ELRS_CMD_LEAD && c.u.lead_us == 0);
    P("LEAD 5000");            assert(c.kind == ELRS_CMD_LEAD && c.u.lead_us == 5000);
    INVALID("LEAD 5001", "args");
    P("SNAP16 256");           assert(c.kind == ELRS_CMD_SNAP16 && c.u.snap_samples == 256);
    P("SNAP16 16380");         assert(c.kind == ELRS_CMD_SNAP16 && c.u.snap_samples == 16380);
    INVALID("SNAP16 255", "args");
    INVALID("SNAP16 16381", "args");
    P("POLARITY INV");         assert(c.kind == ELRS_CMD_POLARITY && c.u.polarity == ELRS_POL_INV);
    P("POLARITY AUTO");        assert(c.kind == ELRS_CMD_POLARITY && c.u.polarity == ELRS_POL_AUTO);
    INVALID("POLARITY X", "args");
    P("SYNC 18446744073709551615"); assert(c.kind == ELRS_CMD_SYNC && c.u.nonce == UINT64_MAX);
    P("SYNC 7");               assert(c.kind == ELRS_CMD_SYNC && c.u.nonce == 7);
    INVALID("SYNC", "args");
    INVALID("SYNC 7 8", "args");
    INVALID("SYNC abc", "args");
    INVALID("CAP16 16380 6", "command");
    INVALID("", "command");
    INVALID("INFO x", "args");
    INVALID("FOO", "command");

    assert(!strcmp(elrs_mode_name(ELRS_MODE_AUTO), "AUTO"));
    assert(!strcmp(elrs_mode_name(ELRS_MODE_SCAN), "SCAN"));
    assert(!strcmp(elrs_mode_name(ELRS_MODE_TIMED), "TIMED"));
    assert(!strcmp(elrs_mode_name(ELRS_MODE_HOP), "HOP"));
    return 0;
}
