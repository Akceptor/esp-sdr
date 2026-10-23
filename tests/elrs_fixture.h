#pragma once

#include <stdint.h>
#include <stdio.h>

/* One int8 interleaved I/Q snapshot, scaled by 4 to the 10-bit bank range. Returns samples read. */
static inline size_t fixture_load(const char *bin, unsigned samples, unsigned index, uint32_t *words)
{
    FILE *f = fopen(bin, "rb");
    if (!f) {
        return 0;
    }
    size_t got = 0;
    if (fseek(f, (long)index * (long)samples * 2, SEEK_SET) == 0) {
        for (; got < samples; got++) {
            int8_t iq[2];
            if (fread(iq, 1, 2, f) != 2) {
                break;
            }
            words[got] = ((uint32_t)(iq[0] * 4) & 0x3ffu) | (((uint32_t)(iq[1] * 4) & 0x3ffu) << 10);
        }
    }
    fclose(f);
    return got;
}
