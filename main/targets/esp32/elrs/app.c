#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "burst_serial.h"
#include "burst_version.h"
#include "cmd.h"
#include <math.h>
#include "dsp.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "frames.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "out.h"
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

#define RADIO_PRIORITY 4
#define COMMAND_PRIORITY 5

typedef enum { UID_NONE, UID_RAM, UID_NVS } uid_source_t;

static volatile bool rx_on, want_rx;
static volatile unsigned loop_gen;
static volatile bool stats_reset_req;
static volatile bool spectrum_on, spectrum_clear_req;
static volatile elrs_polarity_t polarity = ELRS_POL_INV;
static elrs_mode_t mode_requested = ELRS_MODE_AUTO;
static uint8_t uid[6];
static uid_source_t uid_source = UID_NONE;
static portMUX_TYPE uid_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t radio_mx;
static elrs_stats_t stats;
static float cfo_ema_khz;
static bool cfo_valid;
static elrs_dsp_work_t *dsp_work;
static uint8_t dsp_work_storage[20480] __attribute__((aligned(16)));
static uint16_t bench_us[BENCH_METRICS][BENCH_MAX];

static void reply(const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (length > 0) out_reply(text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
}

static bool uid_get(uint8_t out[6]) {
    portENTER_CRITICAL(&uid_mux);
    bool set = uid_source != UID_NONE;
    memcpy(out, uid, 6);
    portEXIT_CRITICAL(&uid_mux);
    return set;
}

static void uid_put(const uint8_t in[6], uid_source_t source) {
    portENTER_CRITICAL(&uid_mux);
    memcpy(uid, in, 6);
    uid_source = source;
    portEXIT_CRITICAL(&uid_mux);
}

static bool nvs_uid_load(void) {
    nvs_handle_t h;
    uint8_t v[6];
    size_t n = sizeof(v);
    bool ok = nvs_open("elrsrx", NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_blob(h, "uid", v, &n) == ESP_OK && n == sizeof(v);
        nvs_close(h);
    }
    if (ok) uid_put(v, UID_NVS);
    memset(v, 0, sizeof(v));
    return ok;
}

static bool nvs_uid_save(void) {
    nvs_handle_t h;
    uint8_t v[6];
    uid_get(v);
    if (nvs_open("elrsrx", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "uid", v, sizeof(v)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    memset(v, 0, sizeof(v));
    return ok;
}

static bool nvs_uid_clear(void) {
    nvs_handle_t h;
    if (nvs_open("elrsrx", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, "uid");
    bool ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static unsigned ticks_us(uint32_t cycles) {
    return cycles / (unsigned)esp_rom_get_cpu_ticks_per_us();
}

static int stats_line(char *buf, size_t cap) {
    elrs_stats_t s = stats;
    s.up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s.mode = ELRS_MODE_SCAN;
    s.state = "ACQ";
    s.cfo_khz = cfo_valid ? (int)lroundf(cfo_ema_khz) : 0;
    s.drop = out_drops();
    return elrs_fmt_stats(buf, cap, &s);
}

typedef struct {
    int64_t last_us;
} limiter_t;

static void warn(limiter_t *l, int64_t period_us, const char *text) {
    int64_t now = esp_timer_get_time();
    if (l->last_us && now - l->last_us < period_us) return;
    l->last_us = now;
    char buf[48];
    int n = elrs_fmt_warn(buf, sizeof(buf), text);
    if (n > 0) out_stream(buf, (size_t)n);
}

static void emit_packet(const radio_capture_t *cap, const elrs_packet_t *p, const uint8_t u[6]) {
    char buf[96];
    uint32_t t_us = (uint32_t)(cap->t_trig_us + p->sync_end_sample / 16);
    int n = elrs_fmt_packet(buf, sizeof(buf), t_us, p, -1, u);
    if (n > 0) out_stream(buf, (size_t)n);
}

static void accept_packet(const radio_capture_t *cap, const elrs_packet_t *p, const uint8_t u[6]) {
    static limiter_t wide_w, rate_w;
    unsigned type = p->pkt[0] & 3;
    if (p->crc_slot == 1 || p->crc_slot == 2) {
        warn(&wide_w, 5000000, "wide_mode");
        return;
    }
    if (type == ELRS_PT_SYNC) {
        elrs_sync_t sy;
        uint8_t model;
        elrs_parse_sync(p->pkt, &sy);
        if (!elrs_sync_uid_ok(&sy, u, &model)) {
            stats.crcfail++;
            return;
        }
        if (!sy.sw_mode) {
            warn(&wide_w, 5000000, "wide_mode");
            return;
        }
        if (sy.rate_index != 14) {
            char text[24];
            snprintf(text, sizeof(text), "rate %u", (unsigned)sy.rate_index);
            warn(&rate_w, 5000000, text);
        }
        stats.sync++;
    } else if (type == ELRS_PT_RC) {
        stats.rc++;
    }
    stats.ok++;
    float khz = p->cfo_hz / 1000.0f;
    if (!cfo_valid) {
        cfo_ema_khz = khz;
        cfo_valid = true;
    } else if (fabsf(khz - cfo_ema_khz) <= 50.0f) {
        cfo_ema_khz += 0.2f * (khz - cfo_ema_khz);
    }
    emit_packet(cap, p, u);
}

static void radio_task(void *arg) {
    (void)arg;
    static limiter_t clip_w, stale_w, cap_w;
    int64_t last_stats_us = esp_timer_get_time();
    uint8_t acc[ELRS_FFT_N];
    unsigned acc_lo = 0;
    uint32_t acc_t_us = 0;
    bool acc_has = false;
    for (;;) {
        if (spectrum_clear_req) {
            spectrum_clear_req = false;
            acc_has = false;
        }
        if (stats_reset_req) {
            stats_reset_req = false;
            memset(&stats, 0, sizeof(stats));
            cfo_valid = false;
        }
        if (!want_rx) {
            acc_has = false;
            loop_gen++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        elrs_decode_cfg_t cfg = {.polarity = polarity};
        uid_get(cfg.uid);
        radio_capture_t cap;
        xSemaphoreTake(radio_mx, portMAX_DELAY);
        cfg.lo_mhz = radio_park_mhz();
        bool armed = radio_arm(ELRS_MAX_SAMPLES) && radio_trigger(0);
        if (armed) radio_finish(&cap);
        xSemaphoreGive(radio_mx);
        if (!armed) {
            warn(&cap_w, 5000000, "capture_args");
        } else if (cap.err && strcmp(cap.err, "stale")) {
            char text[40];
            snprintf(text, sizeof(text), "%s", cap.err);
            warn(&cap_w, 5000000, text);
        } else {
            if (cap.err) warn(&stale_w, 5000000, "stale");
            elrs_convert_info_t info;
            elrs_iq16_t *x = (elrs_iq16_t *)cap.words;
            elrs_convert(cap.words, x, cap.n, &info);
            if (info.clipped > 50) warn(&clip_w, 1000000, "clip");
            elrs_packet_t pk[ELRS_MAX_BURSTS];
            elrs_decode_stats_t ds;
            uint32_t t0 = esp_cpu_get_cycle_count();
            size_t n = elrs_decode_snapshot(x, cap.n, &cfg, dsp_work, pk, ELRS_MAX_BURSTS, &ds);
            stats.dec_us = ticks_us(esp_cpu_get_cycle_count() - t0);
            stats.cap_us = cap.capture_us;
            stats.gain = info.gain;
            stats.snaps++;
            stats.bursts += ds.bursts;
            stats.crcfail += ds.crc_fail;
            for (size_t i = 0; i < n; i++) accept_packet(&cap, &pk[i], cfg.uid);
            if (spectrum_on) {
                uint8_t tmp[ELRS_FFT_N];
                if (elrs_spectrum(tmp)) {
                    if (!acc_has || cfg.lo_mhz != acc_lo) {
                        memcpy(acc, tmp, sizeof(acc));
                        acc_lo = cfg.lo_mhz;
                        acc_t_us = (uint32_t)cap.t_trig_us;
                        acc_has = true;
                    } else {
                        for (int j = 0; j < ELRS_FFT_N; j++)
                            if (tmp[j] > acc[j]) acc[j] = tmp[j];
                    }
                    if ((uint32_t)cap.t_trig_us - acc_t_us >= 100000) {
                        char fb[160];
                        int fn = elrs_fmt_spectrum(fb, sizeof(fb), acc_t_us, acc_lo, acc);
                        if (fn > 0) out_stream(fb, (size_t)fn);
                        acc_has = false;
                    }
                }
            }
        }
        memset(cfg.uid, 0, sizeof(cfg.uid));
        int64_t now = esp_timer_get_time();
        if (now - last_stats_us >= 1000000) {
            last_stats_us = now;
            char buf[320];
            int n = stats_line(buf, sizeof(buf));
            if (n > 0) out_stream(buf, (size_t)n);
        }
        loop_gen++;
        vTaskDelay(1);
    }
}

static void rx_stop(void) {
    want_rx = false;
    unsigned start = loop_gen;
    int64_t deadline = esp_timer_get_time() + 2000000;
    while ((int)(loop_gen - start) < 2 && esp_timer_get_time() < deadline) vTaskDelay(1);
    rx_on = false;
}

static bool send_failed(const radio_capture_t *cap) {
    if (cap->err && strcmp(cap->err, "stale")) reply("ERR %s\n", cap->err);
    return cap->err && strcmp(cap->err, "stale");
}

static void snap16(unsigned n) {
    radio_capture_t cap;
    xSemaphoreTake(radio_mx, portMAX_DELAY);
    bool armed = radio_arm(n) && radio_trigger(0);
    if (armed) radio_finish(&cap);
    xSemaphoreGive(radio_mx);
    if (!armed) {
        reply("ERR args\n");
        return;
    }
    if (send_failed(&cap)) return;
    uint8_t *p = (uint8_t *)cap.words;
    for (unsigned j = 0; j < n; j++) {
        uint32_t w = cap.words[j];
        p[2 * j] = (w >> 2) & 255;
        p[2 * j + 1] = (w >> 12) & 255;
    }
    uint32_t crc = esp_rom_crc32_le(0, p, 2 * n);
    char h[64];
    int len = snprintf(h, sizeof(h), "DATA %u %08x %u\n", n, (unsigned)crc, cap.capture_us);
    out_direct_begin();
    if (cap.err) burst_serial_send("W stale\n", 8);
    burst_serial_send(h, (size_t)len);
    burst_serial_send(p, 2 * n);
    out_direct_end();
}

static int cmp_u16(const void *a, const void *b) {
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static uint16_t sat16(uint32_t v) {
    return v > 65535 ? 65535 : (uint16_t)v;
}

static void bench(unsigned n) {
    static const char *const names[BENCH_METRICS] = {"cap_us", "arm_us", "read_us", "det_us", "demod_us", "total_us"};
    elrs_decode_cfg_t cfg = {.lo_mhz = radio_park_mhz(), .polarity = polarity};
    if (!uid_get(cfg.uid)) memcpy(cfg.uid, (const uint8_t[6]){0, 0, 0, 0, 0x13, 0x3e}, 6);
    unsigned hits = 0, done = 0;
    for (unsigned i = 0; i < n; i++) {
        radio_capture_t cap;
        uint32_t t0 = esp_cpu_get_cycle_count();
        xSemaphoreTake(radio_mx, portMAX_DELAY);
        bool armed = radio_arm(ELRS_MAX_SAMPLES) && radio_trigger(0);
        if (armed) radio_finish(&cap);
        xSemaphoreGive(radio_mx);
        if (!armed) {
            reply("ERR args\n");
            return;
        }
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
    memset(cfg.uid, 0, sizeof(cfg.uid));
    reply("%s hits=%u\n", line, hits);
}

static void command(char *line) {
    if (!strcmp(line, "VERSION?")) {
        out_direct_begin();
        burst_version_command(line);
        out_direct_end();
        return;
    }
    elrs_cmd_t c;
    elrs_cmd_parse(line, &c);
    switch (c.kind) {
    case ELRS_CMD_INFO: reply("ESP32ELRS 1 elrs-rx 0\n"); break;
    case ELRS_CMD_CAPS: reply("CAPS VERSION ELRSRX DK500 OTA3 UIDNVS BENCH SNAP16 SPECTRUM\n"); break;
    case ELRS_CMD_PARK:
        xSemaphoreTake(radio_mx, portMAX_DELAY);
        radio_park(c.u.park_mhz);
        xSemaphoreGive(radio_mx);
        reply("OK\n");
        break;
    case ELRS_CMD_PARK_QUERY: reply("PARK %u\n", radio_park_mhz()); break;
    case ELRS_CMD_GAIN_HARDWARE:
        xSemaphoreTake(radio_mx, portMAX_DELAY);
        radio_set_gain(true, 0);
        xSemaphoreGive(radio_mx);
        reply("OK\n");
        break;
    case ELRS_CMD_GAIN_MANUAL:
        if (c.u.gain > radio_gain_max()) reply("ERR args\n");
        else {
            xSemaphoreTake(radio_mx, portMAX_DELAY);
            radio_set_gain(false, c.u.gain);
            xSemaphoreGive(radio_mx);
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
    case ELRS_CMD_UID_SET:
        uid_put(c.u.uid, UID_RAM);
        memset(c.u.uid, 0, sizeof(c.u.uid));
        stats_reset_req = true;
        reply("OK\n");
        break;
    case ELRS_CMD_UID_QUERY:
        reply("UID %s\n", uid_source == UID_NONE ? "NONE" : uid_source == UID_RAM ? "RAM" : "NVS");
        break;
    case ELRS_CMD_UID_SAVE:
        if (uid_source == UID_NONE) reply("ERR no_uid\n");
        else if (!nvs_uid_save()) reply("ERR nvs\n");
        else {
            portENTER_CRITICAL(&uid_mux);
            uid_source = UID_NVS;
            portEXIT_CRITICAL(&uid_mux);
            reply("OK\n");
        }
        break;
    case ELRS_CMD_UID_CLEAR: {
        rx_stop();
        const uint8_t zero[6] = {0};
        uid_put(zero, UID_NONE);
        stats_reset_req = true;
        reply(nvs_uid_clear() ? "OK\n" : "ERR nvs\n");
        break;
    }
    case ELRS_CMD_MODE:
        if (c.u.mode == ELRS_MODE_TIMED) reply("ERR timed_unavailable\n");
        else if (c.u.mode == ELRS_MODE_HOP) reply("ERR hop_unqualified\n");
        else {
            mode_requested = c.u.mode;
            reply("OK\n");
        }
        break;
    case ELRS_CMD_MODE_QUERY: reply("MODE %s SCAN\n", elrs_mode_name(mode_requested)); break;
    case ELRS_CMD_POLARITY:
        polarity = c.u.polarity;
        reply("OK\n");
        break;
    case ELRS_CMD_SPECTRUM:
        spectrum_on = c.u.on;
        spectrum_clear_req = true;
        reply("OK\n");
        break;
    case ELRS_CMD_RX_ON:
        if (uid_source == UID_NONE) {
            reply("ERR no_uid\n");
            break;
        }
        spectrum_clear_req = true;
        rx_on = true;
        reply("OK\n");
        want_rx = true;
        break;
    case ELRS_CMD_RX_OFF:
        rx_stop();
        reply("OK\n");
        break;
    case ELRS_CMD_STATS: {
        char text[320];
        int n = stats_line(text, sizeof(text));
        if (n > 0) out_reply(text, (size_t)n);
        break;
    }
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
    radio_mx = xSemaphoreCreateMutex();
    assert(radio_mx);
    nvs_uid_load();
    dsp_work = (elrs_dsp_work_t *)dsp_work_storage;
    assert(elrs_dsp_work_size() <= sizeof(dsp_work_storage));
    fflush(stdout);
    burst_serial_init();
    out_init();
    BaseType_t created = xTaskCreate(radio_task, "radio", 6144, NULL, RADIO_PRIORITY, NULL);
    assert(created == pdPASS);
    vTaskPrioritySet(NULL, COMMAND_PRIORITY);
    burst_serial_serve(command);
}
