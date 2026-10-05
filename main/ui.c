/* Two screens, stats and miner; tap the display to switch. Setup mode has one of its own. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "config.h"

#define BLACK 0x0000
#define WHITE 0xFFFF
#define GREY 0x8410
#define ORANGE 0xF483 /* bitcoin orange */
#define GREEN 0x07E0
#define RED 0xF800

#define HALVING_INTERVAL 210000
/* Size of an everyday transaction, which is what mempool.space prices its fees for too. */
#define TYPICAL_TX_VBYTES 140

#define TICK_MS 50
#define HOLD_TICKS (SETUP_HOLD_SECONDS * 1000 / TICK_MS)

enum { SCREEN_STATS, SCREEN_MINER, SCREEN_COUNT };
enum { LEFT, CENTER, RIGHT };

/* Draws formatted text padded to a fixed number of characters, so old text is overwritten. */
static void field(int x, int y, const font_t *font, int scale, uint16_t fg, uint16_t bg, int chars, int align,
                  const char *fmt, ...)
{
    char text[48], padded[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    if (chars > (int)sizeof(padded) - 1) chars = sizeof(padded) - 1;
    int len = (int)strlen(text);
    if (len > chars) len = chars;
    int before = align == LEFT ? 0 : align == RIGHT ? chars - len : (chars - len) / 2;
    memset(padded, ' ', chars);
    memcpy(padded + before, text, len);
    padded[chars] = 0;
    lcd_text(x, y, font, scale, fg, bg, padded);
}

static void with_commas(char *out, size_t size, uint64_t v)
{
    char digits[24];
    int n = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)v);
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < size; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

/* 1234567 -> "1.23 M" */
static void with_prefix(char *out, size_t size, double v, const char *sep)
{
    static const char *prefix[] = {"", "k", "M", "G", "T", "P", "E", "Z"};
    int p = 0;
    while (v >= 1000 && p < 7) {
        v /= 1000;
        p++;
    }
    snprintf(out, size, v >= 100 ? "%.1f%s%s" : "%.2f%s%s", v, sep, prefix[p]);
}

static void format_difficulty(char *out, size_t size, double d)
{
    if (d >= 1000) with_prefix(out, size, d, " ");
    else snprintf(out, size, "%.4g", d);
}

/* The font has no sign for the other currencies; the title bar names them. */
static const char *currency_sign(void)
{
    const char *c = g_settings.currency;
    return !strcmp(c, "USD") || !strcmp(c, "CAD") || !strcmp(c, "AUD") ? "$" : "";
}

/* What the bottom line says while there is no WiFi: two texts that take turns. */
static void wifi_down_text(char *out, size_t size, int tick)
{
    if (tick / 80 % 2) snprintf(out, size, "Hold screen %ds for setup", SETUP_HOLD_SECONDS);
    else snprintf(out, size, "WiFi: joining %s", g_settings.wifi_ssid);
}

static void title_bar(const char *left, const char *right)
{
    lcd_fill(0, 0, LCD_W, 24, ORANGE);
    lcd_text(8, 4, &font_small, 1, BLACK, ORANGE, left);
    lcd_text(LCD_W - 8 - (int)strlen(right) * 8, 4, &font_small, 1, BLACK, ORANGE, right);
}

/* A labelled value cell in the 2x2 grid below the headline number. */
static void cell_label(int col, int row, const char *label)
{
    lcd_text(col ? 168 : 8, 108 + row * 56, &font_small, 1, GREY, BLACK, label);
}

static void cell_value(int col, int row, uint16_t color, const char *text)
{
    field(col ? 168 : 8, 126 + row * 56, &font_big, 1, color, BLACK, 9, LEFT, "%s", text);
}

static void draw_frame(int screen)
{
    char text[40];
    lcd_fill(0, 24, LCD_W, LCD_H - 24, BLACK);
    if (screen == SCREEN_STATS) {
        snprintf(text, sizeof(text), "BTC/%s", g_settings.currency);
        title_bar(text, ""); /* draw_stats puts the device's address on the right */
        cell_label(0, 0, "BLOCK HEIGHT");
        cell_label(0, 1, "DIFFICULTY");
        cell_label(1, 1, "NETWORK H/S");
    } else {
        snprintf(text, sizeof(text), "%.24s:%u", g_settings.pool_host, g_settings.pool_port);
        title_bar("MINER", text);
        cell_label(0, 0, "SHARES OK/BAD");
        cell_label(1, 0, "BEST DIFF");
        cell_label(0, 1, "POOL DIFF");
        cell_label(1, 1, "UPTIME");
    }
}

static void draw_stats(const state_t *s, double hashrate, int tick)
{
    char a[32], b[64];

    /* where the web page is */
    field(LCD_W - 8 - 15 * 8, 4, &font_small, 1, BLACK, ORANGE, 15, RIGHT, "%s", s->wifi_connected ? s->ip : "");

    if (s->have_price) {
        with_commas(a, sizeof(a), (uint64_t)(s->price + 0.5));
        field(0, 34, &font_big, 2, WHITE, BLACK, 10, CENTER, "%s%s", currency_sign(), a);
    } else {
        field(0, 34, &font_big, 2, GREY, BLACK, 10, CENTER, "%s", s->wifi_connected ? "..." : "no wifi");
    }

    if (s->have_network) {
        with_commas(a, sizeof(a), s->height);
        cell_value(0, 0, WHITE, a);
        with_prefix(a, sizeof(a), s->net_difficulty, " ");
        cell_value(0, 1, WHITE, a);
        with_prefix(a, sizeof(a), s->net_hashrate, " ");
        cell_value(1, 1, WHITE, a);
    } else {
        cell_value(0, 0, GREY, "-");
        cell_value(0, 1, GREY, "-");
        cell_value(1, 1, GREY, "-");
    }

    /* what a transaction costs to get into the next block, and the rate that comes from */
    if (s->have_fees) {
        field(168, 108, &font_small, 1, GREY, BLACK, 19, LEFT,
              s->fee_fast < 9.95 ? "TX FEE @ %.1f sat/vB" : "TX FEE @ %.0f sat/vB", s->fee_fast);
    } else {
        field(168, 108, &font_small, 1, GREY, BLACK, 19, LEFT, "TX FEE");
    }
    if (s->have_fees && s->have_price) {
        double cost = s->price * s->fee_fast * TYPICAL_TX_VBYTES / 1e8;
        snprintf(a, sizeof(a), cost < 100 ? "%s%.2f" : "%s%.0f", currency_sign(), cost);
        cell_value(1, 0, WHITE, a);
    } else {
        cell_value(1, 0, GREY, "-");
    }

    if (!s->wifi_connected) {
        wifi_down_text(b, sizeof(b), tick);
    } else if (s->have_network) {
        with_commas(a, sizeof(a), HALVING_INTERVAL - s->height % HALVING_INTERVAL);
        snprintf(b, sizeof(b), "Halving in %s blocks", a);
    } else {
        b[0] = 0;
    }
    field(8, 222, &font_small, 1, s->wifi_connected ? GREY : RED, BLACK, 26, LEFT, "%s", b);
    with_prefix(a, sizeof(a), hashrate, " ");
    field(216, 222, &font_small, 1, s->pool_connected ? GREEN : RED, BLACK, 12, RIGHT, "%sH/s", a);
}

static void draw_miner(const state_t *s, double hashrate, int tick)
{
    char a[64];
    int64_t up = esp_timer_get_time() / 1000000;

    with_prefix(a, sizeof(a), hashrate, " ");
    field(0, 34, &font_big, 2, s->pool_connected ? WHITE : GREY, BLACK, 10, CENTER, "%sH/s", a);

    snprintf(a, sizeof(a), "%u/%u", (unsigned)s->shares_ok, (unsigned)s->shares_bad);
    cell_value(0, 0, s->shares_bad ? ORANGE : WHITE, a);
    format_difficulty(a, sizeof(a), s->best_diff);
    cell_value(1, 0, WHITE, a);
    format_difficulty(a, sizeof(a), s->pool_diff);
    cell_value(0, 1, WHITE, a);
    if (up >= 86400) snprintf(a, sizeof(a), "%dd %02d:%02d", (int)(up / 86400), (int)(up / 3600 % 24), (int)(up / 60 % 60));
    else snprintf(a, sizeof(a), "%02d:%02d:%02d", (int)(up / 3600), (int)(up / 60 % 60), (int)(up % 60));
    cell_value(1, 1, WHITE, a);

    wifi_ap_record_t ap;
    if (s->wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        field(8, 222, &font_small, 1, GREY, BLACK, 26, LEFT, "%s %ddBm", s->ip, ap.rssi);
    } else {
        wifi_down_text(a, sizeof(a), tick);
        field(8, 222, &font_small, 1, RED, BLACK, 26, LEFT, "%s", a);
    }
    field(216, 222, &font_small, 1, s->pool_connected ? GREEN : RED, BLACK, 12, RIGHT,
          s->pool_connected ? "pool online" : "pool offline");
}

/* Shown once the screen has been held long enough; letting go then restarts into setup mode. */
static void draw_hold(void)
{
    lcd_fill(0, 24, LCD_W, LCD_H - 24, BLACK);
    title_bar("SETUP", "");
    field(0, 76, &font_big, 1, WHITE, BLACK, 20, CENTER, "Let go to set up");
    field(0, 132, &font_small, 1, GREY, BLACK, 40, CENTER, "The miner restarts with a WiFi network");
    field(0, 150, &font_small, 1, GREY, BLACK, 40, CENTER, "of its own, for changing its settings.");
}

/* Setup mode's one screen: how to reach the setup page. Does not return. */
static void setup_screen(void)
{
    bool can_leave = g_settings.wifi_ssid[0] && g_settings.btc_address[0];
    bool was_pressed = true; /* a finger still down from asking for setup is not a tap */
    char url[32];

    state_lock();
    state_t s = g_state;
    state_unlock();
    snprintf(url, sizeof(url), "http://%s", s.ip);
    title_bar("SETUP", esp_app_get_description()->version);
    lcd_text(8, 36, &font_small, 1, GREY, BLACK, "1. Join this WiFi network on your phone");
    lcd_text(8, 58, &font_big, 1, WHITE, BLACK, s.ap_ssid);
    lcd_text(8, 106, &font_small, 1, GREY, BLACK, "2. A setup page opens. If it does not,");
    lcd_text(8, 124, &font_small, 1, GREY, BLACK, "   open this address in a browser");
    lcd_text(8, 146, &font_big, 1, ORANGE, BLACK, url);
    if (can_leave) lcd_text(LCD_W - 8 - 13 * 8, 222, &font_small, 1, GREY, BLACK, "Tap to cancel");

    for (int tick = 0;; tick++) {
        bool pressed = touch_pressed();
        bool tapped = pressed && !was_pressed;
        was_pressed = pressed;
        /* the owner thought better of it, or nobody came: back to mining */
        if (can_leave && (tapped || web_idle_seconds() > SETUP_IDLE_SECONDS)) esp_restart();

        if (tick % 20 == 0) {
            wifi_sta_list_t phones = {0};
            esp_wifi_ap_get_sta_list(&phones);
            field(8, 222, &font_small, 1, phones.num ? GREEN : GREY, BLACK, 24, LEFT,
                  phones.num ? "Phone joined" : "Waiting for a phone");
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

static void ui_task(void *arg)
{
    int screen = SCREEN_STATS, tick = 0, held = 0;
    bool was_pressed = true;
    uint32_t last_hashes = 0;
    int64_t last_time = esp_timer_get_time();
    double hashrate = 0;

    if (g_state.setup) setup_screen();

    draw_frame(screen);
    for (;; tick++) {
        bool pressed = touch_pressed();
        bool tapped = pressed && !was_pressed;
        was_pressed = pressed;
        if (held >= HOLD_TICKS && !pressed) {
            settings_request_setup();
            esp_restart();
        }
        held = pressed ? held + 1 : 0;
        if (held == HOLD_TICKS) draw_hold();
        if (tapped) {
            screen = (screen + 1) % SCREEN_COUNT;
            draw_frame(screen);
        }

        if (tick % 20 == 0) {
            uint32_t hashes = g_state.hashes[0] + g_state.hashes[1];
            int64_t now = esp_timer_get_time();
            double rate = (double)(uint32_t)(hashes - last_hashes) * 1e6 / (double)(now - last_time);
            hashrate = hashrate == 0 ? rate : hashrate * 0.8 + rate * 0.2;
            last_hashes = hashes;
            last_time = now;
            state_lock();
            g_state.hashrate = hashrate;
            state_unlock();
        }
        if ((tapped || tick % 20 == 0) && held < HOLD_TICKS) {
            state_lock();
            state_t s = g_state;
            state_unlock();
            if (screen == SCREEN_STATS) draw_stats(&s, hashrate, tick);
            else draw_miner(&s, hashrate, tick);
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void ui_start(void)
{
    xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 2, NULL, 0);
}
