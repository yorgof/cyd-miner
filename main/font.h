#pragma once
#include <stdint.h>

/* Fixed-width bitmap font covering ASCII 32..126, rows padded to whole bytes. */
typedef struct {
    uint8_t width, height;
    const uint8_t *data;
} font_t;

extern const font_t font_small; /* 8x16 */
extern const font_t font_big;   /* 16x32 */
