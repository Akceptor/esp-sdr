#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "burst_serial.h"
#include "burst_version.h"
#include "cmd.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static void reply(const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (length > 0) burst_serial_send(text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
}

static void command(char *line) {
    if (burst_version_command(line)) return;
    elrs_cmd_t c;
    elrs_cmd_parse(line, &c);
    switch (c.kind) {
    case ELRS_CMD_INFO: reply("ESP32ELRS 1 elrs-rx 0\n"); break;
    case ELRS_CMD_CAPS: reply("CAPS VERSION ELRSRX DK500 OTA3\n"); break;
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
    fflush(stdout);
    burst_serial_init();
    burst_serial_serve(command);
}
