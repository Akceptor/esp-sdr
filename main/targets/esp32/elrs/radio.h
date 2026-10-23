#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t *words;
    unsigned n;
    int64_t t_trig_us;
    unsigned arm_us, capture_us, finish_us;
    const char *err;
} radio_capture_t;

void radio_init(void);
void radio_park(unsigned mhz);
unsigned radio_park_mhz(void);
void radio_set_gain(bool hardware, unsigned code);
void radio_gain_reply(char *buf, size_t cap);
unsigned radio_gain_max(void);
bool radio_arm(unsigned n);
/* False means at_us had already passed; the hardware is restored as before radio_arm and radio_finish must not be called. */
bool radio_trigger(int64_t at_us);
bool radio_finish(radio_capture_t *cap);
bool radio_finish_full_check(radio_capture_t *cap);
