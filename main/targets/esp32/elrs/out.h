#pragma once
#include <stdbool.h>
#include <stddef.h>

void out_init(void);
/* Streamed P/S/L/W line: never blocks; false and drop count incremented when the ring is full. */
bool out_stream(const char *line, size_t n);
/* Command reply: blocks until queued, so it follows every streamed line queued before it. */
void out_reply(const char *line, size_t n);
/* Waits until the ring is drained and takes the TX lock; send with burst_serial_send until out_direct_end. */
void out_direct_begin(void);
void out_direct_end(void);
unsigned out_drops(void);
