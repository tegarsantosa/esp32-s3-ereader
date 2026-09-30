/*
 * SPI TFT driver for ST7735 / ST7789 / ILI9341 / ST7796 panels.
 * Resolution, offsets, colour order and orientation come from the panel
 * preset chosen in menuconfig ("E-Reader Configuration" -> "Display").
 */
#include "display.h"
#include <string.h>
#include "sdkconfig.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

enum { DRV_ST7735, DRV_ST7789, DRV_ILI9341, DRV_ST7796 };

typedef struct {
    const char *name;
    int drv;
    int w, h;          /* native (portrait) resolution */
    int xo, yo;        /* offset of the visible area in controller RAM (MADCTL = 0) */
    int ram_w, ram_h;  /* controller RAM size */
    bool bgr, invert;
    bool mx, my;       /* base mirroring that makes rotation 0 upright */
} panel_t;

static const panel_t s_panel =
#if CONFIG_EREADER_LCD_128X160
    {"ST7735S 128x160", DRV_ST7735, 128, 160, 0, 0, 128, 160, false, false, true, true};
#elif CONFIG_EREADER_LCD_128X128
    {"ST7735 128x128", DRV_ST7735, 128, 128, 2, 1, 132, 132, true, false, true, true};
#elif CONFIG_EREADER_LCD_80X160
    {"ST7735S 80x160", DRV_ST7735, 80, 160, 26, 1, 132, 162, true, true, true, true};
#elif CONFIG_EREADER_LCD_135X240
    {"ST7789 135x240", DRV_ST7789, 135, 240, 52, 40, 240, 320, false, true, false, false};
#elif CONFIG_EREADER_LCD_240X240
    {"ST7789 240x240", DRV_ST7789, 240, 240, 0, 0, 240, 320, false, true, false, false};
#elif CONFIG_EREADER_LCD_240X280
    {"ST7789 240x280", DRV_ST7789, 240, 280, 0, 20, 240, 320, false, true, false, false};
#elif CONFIG_EREADER_LCD_172X320
    {"ST7789 172x320", DRV_ST7789, 172, 320, 34, 0, 240, 320, false, true, false, false};
#elif CONFIG_EREADER_LCD_170X320
    {"ST7789 170x320", DRV_ST7789, 170, 320, 35, 0, 240, 320, false, true, false, false};
#elif CONFIG_EREADER_LCD_240X320_ST7789
    {"ST7789 240x320", DRV_ST7789, 240, 320, 0, 0, 240, 320, false, true, false, false};
#elif CONFIG_EREADER_LCD_240X320_ILI9341
    {"ILI9341 240x320", DRV_ILI9341, 240, 320, 0, 0, 240, 320, true, false, true, false};
#elif CONFIG_EREADER_LCD_320X480
    {"ST7796 320x480", DRV_ST7796, 320, 480, 0, 0, 320, 480, true, false, true, false};
#else /* custom */
    {"Custom panel",
#if CONFIG_EREADER_DRV_CUSTOM_ST7735
     DRV_ST7735,
#elif CONFIG_EREADER_DRV_CUSTOM_ILI9341
     DRV_ILI9341,
#elif CONFIG_EREADER_DRV_CUSTOM_ST7796
     DRV_ST7796,
#else
     DRV_ST7789,
#endif
     CONFIG_EREADER_LCD_CUSTOM_WIDTH, CONFIG_EREADER_LCD_CUSTOM_HEIGHT,
     CONFIG_EREADER_LCD_CUSTOM_XOFF, CONFIG_EREADER_LCD_CUSTOM_YOFF,
#if CONFIG_EREADER_DRV_CUSTOM_ST7735
     132, 162,
#elif CONFIG_EREADER_DRV_CUSTOM_ST7796
     320, 480,
#else
     240, 320,
#endif
     false, false, false, false};
#endif

/* ---- init sequences: cmd, n data bytes, data..., delay ms ---- */
#define END 0xFF
typedef struct {
    uint8_t cmd, len;
    uint8_t data[16];
    uint8_t delay;
} lcd_cmd_t;

static const lcd_cmd_t s_init_st7735[] = {
    {0x01, 0, {0}, 150}, {0x11, 0, {0}, 150},
    {0xB1, 3, {0x01, 0x2C, 0x2D}, 0}, {0xB2, 3, {0x01, 0x2C, 0x2D}, 0},
    {0xB3, 6, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 0}, {0xB4, 1, {0x07}, 0},
    {0xC0, 3, {0xA2, 0x02, 0x84}, 0}, {0xC1, 1, {0xC5}, 0}, {0xC2, 2, {0x0A, 0x00}, 0},
    {0xC3, 2, {0x8A, 0x2A}, 0}, {0xC4, 2, {0x8A, 0xEE}, 0}, {0xC5, 1, {0x0E}, 0},
    {0x3A, 1, {0x05}, 0},
    {0xE0, 16, {0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}, 0},
    {0xE1, 16, {0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}, 0},
    {0x13, 0, {0}, 10}, {END, 0, {0}, 0},
};

static const lcd_cmd_t s_init_st7789[] = {
    {0x01, 0, {0}, 150}, {0x11, 0, {0}, 120},
    {0x3A, 1, {0x55}, 10},
    {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 0}, {0xB7, 1, {0x35}, 0},
    {0x13, 0, {0}, 10}, {END, 0, {0}, 0},
};

static const lcd_cmd_t s_init_ili9341[] = {
    {0x01, 0, {0}, 150},
    {0xEF, 3, {0x03, 0x80, 0x02}, 0}, {0xCF, 3, {0x00, 0xC1, 0x30}, 0},
    {0xED, 4, {0x64, 0x03, 0x12, 0x81}, 0}, {0xE8, 3, {0x85, 0x00, 0x78}, 0},
    {0xCB, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}, 0}, {0xF7, 1, {0x20}, 0},
    {0xEA, 2, {0x00, 0x00}, 0}, {0xC0, 1, {0x23}, 0}, {0xC1, 1, {0x10}, 0},
    {0xC5, 2, {0x3e, 0x28}, 0}, {0xC7, 1, {0x86}, 0}, {0x37, 1, {0x00}, 0},
    {0x3A, 1, {0x55}, 0}, {0xB1, 2, {0x00, 0x18}, 0}, {0xB6, 3, {0x08, 0x82, 0x27}, 0},
    {0xF2, 1, {0x00}, 0}, {0x26, 1, {0x01}, 0},
    {0xE0, 15, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 0},
    {0xE1, 15, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 0},
    {0x11, 0, {0}, 150}, {END, 0, {0}, 0},
};

static const lcd_cmd_t s_init_st7796[] = {
    {0x01, 0, {0}, 120}, {0x11, 0, {0}, 120},
    {0xF0, 1, {0xC3}, 0}, {0xF0, 1, {0x96}, 0},
    {0x3A, 1, {0x55}, 0}, {0xB4, 1, {0x01}, 0}, {0xB6, 3, {0x80, 0x02, 0x3B}, 0},
    {0xE8, 8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 0},
    {0xC1, 1, {0x06}, 0}, {0xC2, 1, {0xA7}, 0}, {0xC5, 1, {0x18}, 120},
    {0xE0, 14, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, 0},
    {0xE1, 14, {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 120},
    {0xF0, 1, {0x3C}, 0}, {0xF0, 1, {0x69}, 120},
    {END, 0, {0}, 0},
};

/* ---- state ---- */
#define BAND_BYTES 16384
#define POOL 16
static spi_device_handle_t s_dev;
static uint16_t *s_band[2];
static spi_transaction_t s_pool[POOL];
static uint32_t s_queued, s_done, s_buf_seq[2];
static int s_w, s_h, s_col_off, s_row_off, s_rot;
static bool s_bl_ok;

static void IRAM_ATTR spi_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(CONFIG_EREADER_PIN_DC, (int)(intptr_t)t->user);
}

static void cmd_poll(uint8_t cmd, const uint8_t *data, int len)
{
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = (void *)0;
    spi_device_polling_transmit(s_dev, &t);
    if (len > 0) {
        memset(&t, 0, sizeof(t));
        t.length = len * 8;
        t.tx_buffer = data;
        t.user = (void *)1;
        spi_device_polling_transmit(s_dev, &t);
    }
}

static void run_cmds(const lcd_cmd_t *c)
{
    for (; c->cmd != END; c++) {
        cmd_poll(c->cmd, c->data, c->len);
        if (c->delay) vTaskDelay(pdMS_TO_TICKS(c->delay));
    }
}

static bool cfg_bgr(void)
{
#if CONFIG_EREADER_LCD_ORDER_RGB
    return false;
#elif CONFIG_EREADER_LCD_ORDER_BGR
    return true;
#else
    return s_panel.bgr;
#endif
}

static bool cfg_invert(void)
{
#if CONFIG_EREADER_LCD_INVERT_OFF
    return false;
#elif CONFIG_EREADER_LCD_INVERT_ON
    return true;
#else
    return s_panel.invert;
#endif
}

void display_wait_all(void)
{
    while (s_done < s_queued) {
        spi_transaction_t *r;
        spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
        s_done++;
    }
}

void display_wait_buffer(int idx)
{
    while (s_done < s_buf_seq[idx]) {
        spi_transaction_t *r;
        spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
        s_done++;
    }
}

void display_set_rotation(int rot)
{
    display_wait_all();
    static const uint8_t MY = 0x80, MX = 0x40, MV = 0x20;
    static const uint8_t table[4] = {0x00, 0x40 | 0x20, 0x40 | 0x80, 0x80 | 0x20};
    s_rot = rot & 3;
    uint8_t m = table[s_rot];
    bool bmx = s_panel.mx, bmy = s_panel.my;
#if CONFIG_EREADER_LCD_MIRROR_X
    bmx = !bmx;
#endif
#if CONFIG_EREADER_LCD_MIRROR_Y
    bmy = !bmy;
#endif
    if (bmx) m ^= MX;
    if (bmy) m ^= MY;
    bool mv = m & MV, mx = m & MX, my = m & MY;
    int xo = s_panel.xo, yo = s_panel.yo;
    int xo_m = s_panel.ram_w - s_panel.w - xo; /* offsets when that axis is mirrored */
    int yo_m = s_panel.ram_h - s_panel.h - yo;
    if (xo_m < 0) xo_m = 0;
    if (yo_m < 0) yo_m = 0;
    if (!mv) {
        s_w = s_panel.w;
        s_h = s_panel.h;
        s_col_off = mx ? xo_m : xo;
        s_row_off = my ? yo_m : yo;
    } else {
        s_w = s_panel.h;
        s_h = s_panel.w;
        s_col_off = my ? yo_m : yo;
        s_row_off = mx ? xo_m : xo;
    }
    if (cfg_bgr()) m |= 0x08;
    cmd_poll(0x36, &m, 1);
}

int display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_EREADER_PIN_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (CONFIG_EREADER_PIN_RST >= 0) io.pin_bit_mask |= 1ULL << CONFIG_EREADER_PIN_RST;
    gpio_config(&io);

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_EREADER_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_EREADER_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BAND_BYTES,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        return -1;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = CONFIG_EREADER_LCD_SPI_MHZ * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CONFIG_EREADER_PIN_CS,
        .queue_size = POOL,
        .pre_cb = spi_pre_cb,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi add device failed: %s", esp_err_to_name(err));
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        s_band[i] = heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_band[i]) {
            ESP_LOGE(TAG, "no memory for band buffer");
            return -1;
        }
    }

    if (CONFIG_EREADER_PIN_RST >= 0) {
        gpio_set_level(CONFIG_EREADER_PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(CONFIG_EREADER_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    switch (s_panel.drv) {
    case DRV_ST7735: run_cmds(s_init_st7735); break;
    case DRV_ILI9341: run_cmds(s_init_ili9341); break;
    case DRV_ST7796: run_cmds(s_init_st7796); break;
    default: run_cmds(s_init_st7789); break;
    }
    cmd_poll(cfg_invert() ? 0x21 : 0x20, NULL, 0);
    display_set_rotation(0);

    /* clear the whole RAM to black before switching the display on */
    int rows;
    uint16_t *buf = display_band_buffer(0, &rows);
    memset(buf, 0, BAND_BYTES);
    for (int y = 0; y < s_h; y += rows) {
        display_flush_band(0, y, (y + rows > s_h) ? s_h - y : rows);
        display_wait_all();
    }
    cmd_poll(0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    if (CONFIG_EREADER_PIN_BL >= 0) {
        ledc_timer_config_t tc = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_channel_config_t cc = {
            .gpio_num = CONFIG_EREADER_PIN_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
#if !CONFIG_EREADER_BL_ACTIVE_HIGH
            .flags.output_invert = 1,
#endif
        };
        s_bl_ok = ledc_timer_config(&tc) == ESP_OK && ledc_channel_config(&cc) == ESP_OK;
    }
    ESP_LOGI(TAG, "%s ready (%dx%d)", s_panel.name, s_w, s_h);
    return 0;
}

int display_width(void) { return s_w; }
int display_height(void) { return s_h; }
int display_native_width(void) { return s_panel.w; }
int display_native_height(void) { return s_panel.h; }
const char *display_name(void) { return s_panel.name; }

void display_set_backlight(int percent)
{
    if (!s_bl_ok) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* perceptual curve: small values stay usable in the dark */
    uint32_t duty = (uint32_t)(percent * percent * 1023 / 10000);
    if (percent > 0 && duty < 4) duty = 4;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_sleep(bool sleep)
{
    display_wait_all();
    if (sleep) {
        display_set_backlight(0);
        cmd_poll(0x28, NULL, 0);
        cmd_poll(0x10, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        cmd_poll(0x11, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        cmd_poll(0x29, NULL, 0);
    }
}

uint16_t *display_band_buffer(int idx, int *rows)
{
    *rows = BAND_BYTES / (s_w * 2);
    return s_band[idx];
}

static spi_transaction_t *next_trans(void)
{
    spi_transaction_t *t = &s_pool[s_queued % POOL];
    memset(t, 0, sizeof(*t));
    return t;
}

static void queue_small(uint8_t dc, const uint8_t *bytes, int n)
{
    spi_transaction_t *t = next_trans();
    t->flags = SPI_TRANS_USE_TXDATA;
    t->length = n * 8;
    memcpy(t->tx_data, bytes, n);
    t->user = (void *)(intptr_t)dc;
    spi_device_queue_trans(s_dev, t, portMAX_DELAY);
    s_queued++;
}

void display_flush_band(int idx, int y0, int rows)
{
    int x0 = s_col_off, x1 = s_col_off + s_w - 1;
    int r0 = s_row_off + y0, r1 = s_row_off + y0 + rows - 1;
    uint8_t c;
    uint8_t d[4];
    c = 0x2A; queue_small(0, &c, 1);
    d[0] = x0 >> 8; d[1] = x0 & 0xFF; d[2] = x1 >> 8; d[3] = x1 & 0xFF;
    queue_small(1, d, 4);
    c = 0x2B; queue_small(0, &c, 1);
    d[0] = r0 >> 8; d[1] = r0 & 0xFF; d[2] = r1 >> 8; d[3] = r1 & 0xFF;
    queue_small(1, d, 4);
    c = 0x2C; queue_small(0, &c, 1);

    spi_transaction_t *t = next_trans();
    t->length = (size_t)s_w * rows * 16;
    t->tx_buffer = s_band[idx];
    t->user = (void *)1;
    spi_device_queue_trans(s_dev, t, portMAX_DELAY);
    s_queued++;
    s_buf_seq[idx] = s_queued;
}
