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
    double best_diff, pool_diff, hashrate;
    bool pool_connected;
    const char *hw_loop;    /* hardware SHA loop in use, NULL for software */
    volatile bool updating; /* an uploaded firmware is being written: the miners wait */

    /* network */
    bool setup; /* serving the setup network instead of mining */
    bool wifi_connected;
    char ip[16];
    char ap_ssid[20]; /* name of the setup network */

    /* bitcoin stats */
    bool have_network, have_price, have_fees;
    uint32_t height;
    double net_difficulty, net_hashrate;
    double price;
    double fee_fast, fee_mid, fee_slow; /* sat/vB for the next block, half an hour, an hour */
} state_t;

extern state_t g_state;
void state_lock(void);
void state_unlock(void);

/* settings.c: what the owner enters on the web page, kept in NVS */
typedef struct {
    char wifi_ssid[33], wifi_pass[65];
    char pool_host[64];
    uint16_t pool_port;
    char btc_address[96], worker[32];
    char currency[4];
    bool lcd_flip;       /* picture rotated 180 degrees */
    char admin_pass[33]; /* empty: the web page asks for no password */
} settings_t;

/* Not written after app_main has loaded it: a change is saved and takes effect on restart. */
extern settings_t g_settings;
/* False until a WiFi network and a payout address have been saved. */
bool settings_load(void);
bool settings_save(const settings_t *s);
/* Makes the next start, if it is a restart, come up in setup mode. */
void settings_request_setup(void);
bool settings_take_setup_request(void);

/* lcd.c: 320x240 landscape, RGB565 colors */
#define LCD_W 320
#define LCD_H 240
void lcd_init(void);
void lcd_fill(int x, int y, int w, int h, uint16_t color);
void lcd_text(int x, int y, const font_t *font, int scale, uint16_t fg, uint16_t bg, const char *s);
bool touch_pressed(void);

/* net.c */
void net_start(void);

/* dns.c: answers every name with the given address, for the setup network */
void dns_start(uint32_t ip);

/* web.c */
void web_start(void);
/* The device can be reached, so an uploaded firmware that got this far is kept. */
void web_confirm_firmware(void);
int web_idle_seconds(void);

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

/* bench.c, only built with -DSHA_BENCH=1 */
void bench_run(void);
