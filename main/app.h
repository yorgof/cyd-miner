#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "font.h"
#include "work.h"

/* Everything the screens show. Take state_lock() around the non-atomic fields. */
typedef struct {
    /* miner */
    volatile uint32_t hashes[2]; /* per mining task, wraps */
    uint32_t shares_ok, shares_bad;
    double best_diff, pool_diff;
    bool pool_connected;

    /* network */
    bool wifi_connected;
    char ip[16];

    /* bitcoin stats */
    bool have_network, have_price, have_fees;
    uint32_t height;
    double net_difficulty, net_hashrate;
    double price;
    int fee_fast, fee_mid, fee_slow;
} state_t;

extern state_t g_state;
void state_lock(void);
void state_unlock(void);

/* lcd.c: 320x240 landscape, RGB565 colors */
#define LCD_W 320
#define LCD_H 240
void lcd_init(void);
void lcd_fill(int x, int y, int w, int h, uint16_t color);
void lcd_text(int x, int y, const font_t *font, int scale, uint16_t fg, uint16_t bg, const char *s);
bool touch_pressed(void);

/* net.c */
void net_start(void);

/* stratum.c */
void stratum_start(void);
uint32_t stratum_generation(void);
/* Hands out a fresh header template; false while there is no job. */
bool stratum_get_work(uint32_t *generation, work_t *work, double *difficulty);
void stratum_submit(const work_t *work, uint32_t nonce);

/* miner.c, stats.c, ui.c */
void miner_start(void);
void stats_start(void);
void ui_start(void);
