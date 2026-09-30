#include "settings.h"
#include "font.h"
#include "display.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_VERSION 2

settings_t g_set;

int settings_default_font(void)
{
    int w = display_native_width(), h = display_native_height();
    int m = w < h ? w : h;
    int px = m <= 100 ? 10 : m <= 140 ? 12 : m <= 180 ? 14 : m <= 250 ? 16 : 20;
    return font_index_for_px(px);
}

static void defaults(void)
{
    memset(&g_set, 0, sizeof(g_set));
    g_set.version = SETTINGS_VERSION;
    g_set.font_idx = settings_default_font();
    g_set.ui_font_idx = settings_default_font();
    g_set.line_sp = 1;
    g_set.margin = 1;
    g_set.para = 0;
    g_set.justify = 1;
    g_set.theme = THEME_LIGHT;
    g_set.brightness = 70;
    g_set.status_bar = 1;
    g_set.rotation = 0;
    g_set.sleep_min = 10;
    g_set.wifi_mode = 0;
}

void settings_load(void)
{
    defaults();
    nvs_handle_t h;
    if (nvs_open("ereader", NVS_READONLY, &h) != ESP_OK) return;
    settings_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "settings", &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
        tmp.version == SETTINGS_VERSION) {
        g_set = tmp;
        if (g_set.font_idx >= FONT_COUNT) g_set.font_idx = settings_default_font();
        if (g_set.ui_font_idx >= FONT_COUNT) g_set.ui_font_idx = settings_default_font();
        if (g_set.theme >= THEME_COUNT) g_set.theme = THEME_LIGHT;
        g_set.rotation &= 3;
        if (g_set.brightness < 5 || g_set.brightness > 100) g_set.brightness = 70;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ereader", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "settings", &g_set, sizeof(g_set));
    nvs_commit(h);
    nvs_close(h);
}

void settings_reset(void)
{
    char ssid[33], pass[65];
    memcpy(ssid, g_set.sta_ssid, sizeof(ssid));
    memcpy(pass, g_set.sta_pass, sizeof(pass));
    defaults();
    memcpy(g_set.sta_ssid, ssid, sizeof(ssid));
    memcpy(g_set.sta_pass, pass, sizeof(pass));
    settings_save();
}
