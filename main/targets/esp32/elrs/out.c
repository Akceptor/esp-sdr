#include "out.h"

#include <stdatomic.h>

#include "burst_serial.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RING_BYTES 8192
#define TX_PRIORITY 3

static RingbufHandle_t ring;
static SemaphoreHandle_t tx_lock;
static atomic_uint queued;
static atomic_uint drops;

static void tx_task(void *arg) {
    (void)arg;
    for (;;) {
        size_t n;
        void *item = xRingbufferReceive(ring, &n, portMAX_DELAY);
        if (!item) continue;
        xSemaphoreTake(tx_lock, portMAX_DELAY);
        burst_serial_send(item, n);
        xSemaphoreGive(tx_lock);
        vRingbufferReturnItem(ring, item);
        atomic_fetch_sub(&queued, 1);
    }
}

void out_init(void) {
    ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    tx_lock = xSemaphoreCreateMutex();
    configASSERT(ring && tx_lock);
    BaseType_t ok = xTaskCreate(tx_task, "out_tx", 3072, NULL, TX_PRIORITY, NULL);
    configASSERT(ok == pdPASS);
}

static bool enqueue(const char *line, size_t n, TickType_t wait) {
    atomic_fetch_add(&queued, 1);
    if (xRingbufferSend(ring, line, n, wait) == pdTRUE) return true;
    atomic_fetch_sub(&queued, 1);
    return false;
}

bool out_stream(const char *line, size_t n) {
    if (enqueue(line, n, 0)) return true;
    atomic_fetch_add(&drops, 1);
    return false;
}

void out_reply(const char *line, size_t n) {
    enqueue(line, n, pdMS_TO_TICKS(5000));
}

void out_direct_begin(void) {
    while (atomic_load(&queued)) vTaskDelay(1);
    xSemaphoreTake(tx_lock, portMAX_DELAY);
}

void out_direct_end(void) {
    xSemaphoreGive(tx_lock);
}

unsigned out_drops(void) {
    return atomic_load(&drops);
}
