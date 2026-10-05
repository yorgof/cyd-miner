/* Minimal ST7789 display and XPT2046 touch driver, both on one SPI bus. */
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "config.h"

#define BUF_PIXELS 2048

static spi_device_handle_t lcd, touch;
static uint8_t *buf; /* DMA-capable pixel buffer, big-endian RGB565 */

static void lcd_write(bool is_data, const uint8_t *bytes, size_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = bytes};
    gpio_set_level(PIN_LCD_DC, is_data);
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &t));
}

static void lcd_cmd(uint8_t cmd, const uint8_t *args, size_t len)
{
    lcd_write(false, &cmd, 1);
    if (len) lcd_write(true, args, len);
}

/* Sends w*h pixels from buf to the given rectangle. */
static void lcd_blit(int x, int y, int w, int h)
{
    int x2 = x + w - 1, y2 = y + h - 1;
    uint8_t col[4] = {x >> 8, x, x2 >> 8, x2}, row[4] = {y >> 8, y, y2 >> 8, y2};
    lcd_cmd(0x2A, col, 4);
    lcd_cmd(0x2B, row, 4);
    lcd_cmd(0x2C, buf, (size_t)w * h * 2);
}

void lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUF_PIXELS * 2,
    };
    spi_device_interface_config_t lcd_dev = {
        .clock_speed_hz = LCD_SPI_HZ,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    spi_device_interface_config_t touch_dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .spics_io_num = PIN_TOUCH_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &lcd_dev, &lcd));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &touch_dev, &touch));

    gpio_set_direction(PIN_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
    buf = heap_caps_malloc(BUF_PIXELS * 2, MALLOC_CAP_DMA);
    assert(buf);

    /* landscape, BGR panel; the two values are 180 degrees apart */
    uint8_t madctl = g_settings.lcd_flip ? 0xA8 : 0x68, colmod = 0x55;
    lcd_cmd(0x01, NULL, 0); /* software reset */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11, NULL, 0); /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x3A, &colmod, 1); /* 16 bits per pixel */
    lcd_cmd(0x36, &madctl, 1);
    lcd_cmd(0x21, NULL, 0); /* inversion on (IPS panel) */
    lcd_cmd(0x13, NULL, 0); /* normal display mode */
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
    lcd_cmd(0x29, NULL, 0); /* display on */
    gpio_set_level(PIN_LCD_BL, 1);
}

void lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    int rows = BUF_PIXELS / w;
    int n = (h < rows ? h : rows) * w;
    for (int i = 0; i < n; i++) {
        buf[i * 2] = color >> 8;
        buf[i * 2 + 1] = (uint8_t)color;
    }
    for (int done = 0; done < h; done += rows) {
        int chunk = h - done < rows ? h - done : rows;
        lcd_blit(x, y + done, w, chunk);
    }
}

void lcd_text(int x, int y, const font_t *font, int scale, uint16_t fg, uint16_t bg, const char *s)
{
    int gw = font->width * scale, gh = font->height * scale;
    int bytes_per_row = (font->width + 7) / 8;
    int max_rows = BUF_PIXELS / gw;

    for (; *s && x + gw <= LCD_W; s++, x += gw) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *glyph = font->data + (ch - 32) * bytes_per_row * font->height;

        int rows = 0, top = 0;
        uint8_t *p = buf;
        for (int r = 0; r < gh; r++) {
            const uint8_t *bits = glyph + (r / scale) * bytes_per_row;
            for (int c = 0; c < gw; c++) {
                int src = c / scale;
                uint16_t color = bits[src / 8] & (0x80 >> (src % 8)) ? fg : bg;
                *p++ = color >> 8;
                *p++ = (uint8_t)color;
            }
            if (++rows == max_rows || r == gh - 1) {
                lcd_blit(x, y + top, gw, rows);
                top += rows;
                rows = 0;
                p = buf;
            }
        }
    }
}

bool touch_pressed(void)
{
    /* Z1 pressure measurement: near zero when untouched */
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 24,
        .tx_data = {0xB0, 0, 0},
    };
    if (spi_device_polling_transmit(touch, &t) != ESP_OK) return false;
    int z = ((t.rx_data[1] << 8) | t.rx_data[2]) >> 3;
    return z > TOUCH_Z_MIN;
}
