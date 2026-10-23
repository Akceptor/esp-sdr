/* RX primitives copied from main/targets/esp32/receiver.c at commit e342444: prepare_rx, apply_gain,
 * tune_rx, filter_apply/filter_restore, and acquire_iq split into arm/trigger/finish. Register
 * constants and the reserved SRAM region are unchanged. Receive only; nothing here transmits. */
#include "radio.h"
#include <stdio.h>
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "rx_lo.h"
#include "rx_recalibration.h"
#include "soc/dport_access.h"
#include "soc/dport_reg.h"
#include "soc/rtc.h"
#include "soc/soc.h"

/* Reserve both SRAM banks, including their instruction aliases. Mode 2 only
 * exposes 32 KiB to MAC; mode 3 exposes the entire 64 KiB capture aperture. */
SOC_RESERVE_MEMORY_REGION(0x3ffe8000, 0x3fff8000, esp32_rf_dump);
#define DUMP_CTRL   0x60033d90u
#define DUMP_STATUS 0x60033d94u
#define DUMP_BYTES  0x60033dc4u
#define RX_GAIN     0x3ff5c02cu
#define CAPACITY    16384u
#define MAX_SAMPLES (CAPACITY - 4)
#define SENTINEL    0xa55aa55au
#define TRIGGER_LEAD_US 1200

static uint32_t *const samples = (void *)0x3ffe8000;
static unsigned gain_max, gain_code = 40;
static bool hardware_agc = true;
static unsigned frequency_mhz = 2438;
static bool rx_ready;

/* Original ESP32 BBTOP RX filter: registers 1/2, seven-bit capacitor DAC.
 * Preserve bit 7 and restore PHY calibration before any retune or transfer. */
static int rx_filter = -1;
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);
static unsigned filter_saved[2];
static void filter_apply(void) {
    for (unsigned j = 0; j < 2; j++) {
        filter_saved[j] = rom_chip_i2c_readReg(0x67, 1, 1 + j);
        if (rx_filter >= 0)
            rom_chip_i2c_writeReg(0x67, 1, 1 + j, (filter_saved[j] & ~127u) | (unsigned)rx_filter);
    }
    esp_rom_delay_us(10);
}
static void filter_restore(void) {
    if (rx_filter >= 0)
        for (unsigned j = 0; j < 2; j++) rom_chip_i2c_writeReg(0x67, 1, 1 + j, filter_saved[j]);
}
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(int);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(int);
extern void set_chanfreq(unsigned mhz, unsigned mode);
extern void rom_set_rf_freq_offset(unsigned crystal, unsigned mhz, int offset);

static void apply_gain(void) {
    uint32_t gain = REG_READ(RX_GAIN);
    if (hardware_agc)
        REG_CLR_BIT(RX_GAIN, BIT(23));
    else
        REG_WRITE(RX_GAIN, (gain & 0x7fffff) | (gain_code << 24) | BIT(23));
}

static void tune_rx(unsigned mhz) {
    static unsigned calibrated_mhz;
    if (calibrated_mhz != mhz || rx_recalibration_stale()) {
        rx_recalibrate(mhz);
        calibrated_mhz = mhz;
    }
    rx_lo_plan_t plan = rx_lo_plan(mhz);
    rx_lo_select(false);
    bool channel = (mhz >= 2412 && mhz <= 2472 && (mhz - 2412) % 5 == 0) || mhz == 2484;
    /* Calibrate on a real channel before bypassing the channel-number mapping.
     * Use the PHY entry point even for repeated requests: the Wi-Fi API can
     * skip a channel it thinks is already selected after a direct PLL retune. */
    set_chanfreq(channel ? mhz : 2412, 0);
    if (!channel) {
        unsigned xtal = rtc_clk_xtal_freq_get();
        /* Channel setup pins its capacitor code. Release it before the ROM
         * retune, otherwise the SDM changes but the PLL can remain unlocked. */
        unsigned cap = ram_chip_i2c_readReg(0x62, 1, 0);
        ram_chip_i2c_writeReg(0x62, 1, 0, cap & ~0x80u);
        /* This ROM uses 1/1024 MHz offsets, unlike the newer kHz API. */
        int offset = (plan.offset_khz * 1024 + 500) / 1000;
        rom_set_rf_freq_offset(xtal == 26 ? 1 : xtal == 24 ? 2 : 0, plan.mhz, offset);
    }
}

static void prepare_rx(void) {
    if (!rx_ready || rx_recalibration_stale()) {
        tune_rx(frequency_mhz);
        rx_ready = true;
    }
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    /* PBUS work-mode setup releases a forced RX gain; apply it afterwards. */
    apply_gain();
    if (rx_lo_select(rx_lo_plan(frequency_mhz).alternate)) esp_rom_delay_us(3000);
}

void radio_init(void) {
    prepare_rx();
    gain_max = (REG_READ(RX_GAIN) >> 8) & 127;
    REG_CLR_BIT(RX_GAIN, BIT(23)); /* Hardware AGC is the default. */
}

void radio_park(unsigned mhz) {
    frequency_mhz = mhz;
    tune_rx(mhz);
    rx_ready = true;
    prepare_rx();
}

unsigned radio_park_mhz(void) {
    return frequency_mhz;
}

void radio_set_gain(bool hardware, unsigned code) {
    hardware_agc = hardware;
    if (!hardware) gain_code = code;
    apply_gain();
}

void radio_gain_reply(char *buf, size_t cap) {
    snprintf(buf, cap, "GAIN %s %d 0 %u %u\n", hardware_agc ? "HARDWARE" : "MANUAL",
             hardware_agc ? -1 : (int)gain_code, gain_max, (unsigned)((REG_READ(RX_GAIN) >> 23) & 1));
}

unsigned radio_gain_max(void) {
    return gain_max;
}

static unsigned capture_n;
static uint32_t saved_owner, saved_bytes;
static bool stale_seen;
static int64_t trig_us;
static unsigned arm_us;

static unsigned cycles_to_us(uint32_t cycles) {
    return cycles / (unsigned)esp_rom_get_cpu_ticks_per_us();
}

bool radio_arm(unsigned n) {
    if (n < 256 || n > MAX_SAMPLES) return false;
    uint32_t t0 = esp_cpu_get_cycle_count();
    stale_seen = rx_recalibration_stale();
    if (stale_seen) prepare_rx();
    REG_WRITE(DUMP_CTRL, 0);
    for (unsigned j = 0; j < CAPACITY; j++) samples[j] = SENTINEL;
    filter_apply();
    saved_owner = DPORT_REG_READ(DPORT_IRAM_DRAM_AHB_SEL_REG);
    saved_bytes = REG_READ(DUMP_BYTES);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG,
                    (saved_owner & ~DPORT_MAC_DUMP_MODE_M) | (3 << DPORT_MAC_DUMP_MODE_S));
    REG_WRITE(DUMP_BYTES, 0x03020100); /* Four distinct bytes, as in mac_init. */
    /* Source 0, clock selector 0 = 16 MS/s. */
    REG_WRITE(DUMP_CTRL, n | BIT(16) | BIT(31));
    capture_n = n;
    arm_us = cycles_to_us(esp_cpu_get_cycle_count() - t0);
    return true;
}

static void disarm(void) {
    REG_WRITE(DUMP_CTRL, 0);
    esp_rom_delay_us(10);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, saved_owner);
    REG_WRITE(DUMP_BYTES, saved_bytes);
    filter_restore();
}

bool radio_trigger(int64_t at_us) {
    if (at_us > 0) {
        int64_t wait = at_us - TRIGGER_LEAD_US - esp_timer_get_time();
        if (wait > 2000) vTaskDelay(pdMS_TO_TICKS((wait - 1000) / 1000));
        int64_t now = esp_timer_get_time();
        if (now > at_us) {
            disarm();
            return false;
        }
        uint32_t target = esp_cpu_get_cycle_count() + (uint32_t)(at_us - now) * (uint32_t)esp_rom_get_cpu_ticks_per_us();
        while ((int32_t)(esp_cpu_get_cycle_count() - target) < 0) {}
    }
    trig_us = esp_timer_get_time();
    REG_SET_BIT(DUMP_CTRL, BIT(19));
    REG_CLR_BIT(DUMP_CTRL, BIT(19));
    return true;
}

static bool finish(radio_capture_t *cap, bool full) {
    int64_t start = trig_us;
    while (!(REG_READ(DUMP_CTRL) & BIT(18)) && esp_timer_get_time() - start < 20000) {}
    uint32_t result = REG_READ(DUMP_CTRL), status = REG_READ(DUMP_STATUS);
    uint32_t t0 = esp_cpu_get_cycle_count();
    cap->t_trig_us = start;
    cap->arm_us = arm_us;
    cap->capture_us = (unsigned)(esp_timer_get_time() - start);
    disarm();
    unsigned n = capture_n;
    cap->words = samples;
    cap->n = n;
    cap->err = stale_seen ? "stale" : NULL;
    bool ok = (result & BIT(18)) && (status & 0x7fff) == n;
    if (!ok)
        cap->err = "capture_timeout";
    else if (full) {
        for (unsigned j = 0; j < CAPACITY; j++)
            if ((j < n && samples[j] == SENTINEL) || (j >= n && samples[j] != SENTINEL)) {
                cap->err = "capture_memory";
                ok = false;
                break;
            }
    } else if (samples[n] != SENTINEL) {
        cap->err = "capture_memory";
        ok = false;
    }
    cap->finish_us = cycles_to_us(esp_cpu_get_cycle_count() - t0);
    return ok;
}

bool radio_finish(radio_capture_t *cap) {
    return finish(cap, false);
}

bool radio_finish_full_check(radio_capture_t *cap) {
    return finish(cap, true);
}
