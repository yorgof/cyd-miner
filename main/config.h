#pragma once

/* WiFi, pool and payout address live in settings.h (not checked in). */
#include "settings.h"

/* Share difficulty to ask the pool for; 0.001 is a share every minute or so. */
#define SUGGEST_DIFFICULTY 0.001

/* Source of network stats, price and fees */
#define MEMPOOL_API_URL "https://mempool.space/api"
#define CURRENCY "CAD"
#define STATS_REFRESH_SECONDS 30

/* Board: Freenove FNK0103L, 3.2" 240x320 ST7789 + XPT2046 on one SPI bus */
#define PIN_SPI_MISO 12
#define PIN_SPI_MOSI 13
#define PIN_SPI_SCLK 14
#define PIN_LCD_CS 15
#define PIN_LCD_DC 2
#define PIN_LCD_BL 27
#define PIN_TOUCH_CS 33
#define LCD_SPI_HZ (40 * 1000 * 1000)
/* Set to 1 to rotate the picture 180 degrees. */
#define LCD_FLIP 0
/* Minimum XPT2046 pressure reading that counts as a touch. */
#define TOUCH_Z_MIN 200
