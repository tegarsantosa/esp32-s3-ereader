#pragma once
#include <stdint.h>
#include <stdbool.h>

enum { THEME_LIGHT, THEME_SEPIA, THEME_DARK, THEME_NIGHT, THEME_COUNT };

typedef struct {
    uint8_t version;
    uint8_t font_idx;    /* reader font, index into g_fonts */
    uint8_t line_sp;     /* 0 compact, 1 normal, 2 relaxed */
    uint8_t margin;      /* 0 small, 1 normal, 2 wide */
    uint8_t para;        /* 0 indent, 1 spaced */
    uint8_t justify;
    uint8_t theme;
    uint8_t brightness;  /* 5..100 */
    uint8_t status_bar;
    uint8_t rotation;    /* 0..3 */
    uint8_t sleep_min;   /* 0 = never */
    uint8_t wifi_mode;   /* 0 = home WiFi if saved, else hotspot; 1 = hotspot only */
    uint8_t ui_font_idx;
    uint8_t seen_help;
    char last_book[128];
    char sta_ssid[33];
    char sta_pass[65];
} settings_t;

extern settings_t g_set;

void settings_load(void);
void settings_save(void);
void settings_reset(void);
int settings_default_font(void); /* depends on screen size */
