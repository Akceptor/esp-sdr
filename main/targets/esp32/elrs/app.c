#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "burst_serial.h"
#include "burst_version.h"
#include "cmd.h"
#include "dsp.h"
#include "esp_cpu.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "radio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define BENCH_MAX 2000
#define BENCH_METRICS 6

static bool rx_on;
static elrs_dsp_work_t *dsp_work;
static uint8_t dsp_work_storage[20480] __attribute__((aligned(16)));
static uint16_t bench_us[BENCH_METRICS][BENCH_MAX];

static void reply(const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (length > 0) burst_serial_send(text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
}

static bool send_failed(const radio_capture_t *cap) {
    if (cap->err && strcmp(cap->err, "stale")) reply("ERR %s\n", cap->err);
    return cap->err && strcmp(cap->err, "stale");
}

static void snap16(unsigned n) {
    radio_capture_t cap;
    if (!radio_arm(n) || !radio_trigger(0)) {
        reply("ERR args\n");
        return;
    }
    radio_finish(&cap);
    if (send_failed(&cap)) return;
    uint8_t *p = (uint8_t *)cap.words;
    for (unsigned j = 0; j < n; j++) {
        uint32_t w = cap.words[j];
        p[2 * j] = (w >> 2) & 255;
        p[2 * j + 1] = (w >> 12) & 255;
    }
    uint32_t crc = esp_rom_crc32_le(0, p, 2 * n);
    if (cap.err) reply("W stale\n");
    char h[64];
    int len = snprintf(h, sizeof(h), "DATA %u %08x %u\n", n, (unsigned)crc, cap.capture_us);
    burst_serial_send(h, (size_t)len);
    burst_serial_send(p, 2 * n);
}

static int cmp_u16(const void *a, const void *b) {
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static uint16_t sat16(uint32_t v) {
    return v > 65535 ? 65535 : (uint16_t)v;
}

static unsigned ticks_us(uint32_t cycles) {
    return cycles / (unsigned)esp_rom_get_cpu_ticks_per_us();
}

static void bench(unsigned n) {
    static const uint8_t uid[6] = {0, 0, 0, 0, 0x13, 0x3e};
    static const char *const names[BENCH_METRICS] = {"cap_us", "arm_us", "read_us", "det_us", "demod_us", "total_us"};
    elrs_decode_cfg_t cfg = {.lo_mhz = radio_park_mhz(), .polarity = ELRS_POL_INV};
    memcpy(cfg.uid, uid, 6);
    unsigned hits = 0, done = 0;
    for (unsigned i = 0; i < n; i++) {
        radio_capture_t cap;
        uint32_t t0 = esp_cpu_get_cycle_count();
        if (!radio_arm(ELRS_MAX_SAMPLES) || !radio_trigger(0)) {
            reply("ERR args\n");
            return;
        }
        radio_finish(&cap);
        if (send_failed(&cap)) return;
        uint32_t t1 = esp_cpu_get_cycle_count();
        elrs_iq16_t *x = (elrs_iq16_t *)cap.words;
        elrs_convert(cap.words, x, cap.n, NULL);
        uint32_t t2 = esp_cpu_get_cycle_count();
        elrs_burst_t bursts[ELRS_MAX_BURSTS];
        elrs_detect(x, cap.n, cfg.lo_mhz, bursts, ELRS_MAX_BURSTS, NULL);
        uint32_t t3 = esp_cpu_get_cycle_count();
        elrs_packet_t pk[ELRS_MAX_BURSTS];
        hits += (unsigned)elrs_decode_snapshot(x, cap.n, &cfg, dsp_work, pk, ELRS_MAX_BURSTS, NULL);
        uint32_t t4 = esp_cpu_get_cycle_count();
        unsigned det = ticks_us(t3 - t2), decode = ticks_us(t4 - t3);
        bench_us[0][i] = sat16(ticks_us(t1 - t0));
        bench_us[1][i] = sat16(cap.arm_us);
        bench_us[2][i] = sat16(ticks_us(t2 - t1));
        bench_us[3][i] = sat16(det);
        bench_us[4][i] = sat16(decode > det ? decode - det : 0);
        bench_us[5][i] = sat16(ticks_us(t4 - t0) - det);
        done++;
    }
    char line[256];
    int len = snprintf(line, sizeof(line), "BENCH n=%u", done);
    for (unsigned m = 0; m < BENCH_METRICS; m++) {
        qsort(bench_us[m], done, sizeof(uint16_t), cmp_u16);
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %s=%u/%u", names[m], bench_us[m][done / 2],
                        bench_us[m][(done * 99 + 99) / 100 - 1]);
    }
    reply("%s hits=%u\n", line, hits);
}

static void command(char *line) {
    if (burst_version_command(line)) return;
    elrs_cmd_t c;
    elrs_cmd_parse(line, &c);
    switch (c.kind) {
    case ELRS_CMD_INFO: reply("ESP32ELRS 1 elrs-rx 0\n"); break;
    case ELRS_CMD_CAPS: reply("CAPS VERSION ELRSRX DK500 OTA3 SNAP16 BENCH\n"); break;
    case ELRS_CMD_PARK:
        radio_park(c.u.park_mhz);
        reply("OK\n");
        break;
    case ELRS_CMD_PARK_QUERY: reply("PARK %u\n", radio_park_mhz()); break;
    case ELRS_CMD_GAIN_HARDWARE:
        radio_set_gain(true, 0);
        reply("OK\n");
        break;
    case ELRS_CMD_GAIN_MANUAL:
        if (c.u.gain > radio_gain_max()) reply("ERR args\n");
        else {
            radio_set_gain(false, c.u.gain);
            reply("OK\n");
        }
        break;
    case ELRS_CMD_GAIN_QUERY: {
        char text[96];
        radio_gain_reply(text, sizeof(text));
        reply("%s", text);
        break;
    }
    case ELRS_CMD_SNAP16:
        if (rx_on) reply("ERR rx_on\n");
        else snap16(c.u.snap_samples);
        break;
    case ELRS_CMD_BENCH:
        if (rx_on) reply("ERR rx_on\n");
        else bench(c.u.bench_n);
        break;
    case ELRS_CMD_SYNC: reply("SYNC %llu\n", (unsigned long long)c.u.nonce); break;
    case ELRS_CMD_TRANSPORT:
        reply("TRANSPORT %s %u\n", burst_serial_port() == BURST_SERIAL_UART ? "UART" : "USB", burst_serial_baud());
        break;
    case ELRS_CMD_INVALID: reply("ERR %s\n", c.err); break;
    default: reply("ERR unsupported\n"); break;
    }
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    radio_init();
    dsp_work = (elrs_dsp_work_t *)dsp_work_storage;
    assert(elrs_dsp_work_size() <= sizeof(dsp_work_storage));
    fflush(stdout);
    burst_serial_init();
    burst_serial_serve(command);
}
