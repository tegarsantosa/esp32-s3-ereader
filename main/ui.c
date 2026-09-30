/*
 * User interface for a one-button e-reader.
 *
 *   tap          next page / next item
 *   double tap   previous page / previous item
 *   hold+release open menu / select   (a bar at the top shows "release now")
 *   hold 3 s     sleep (press to wake)
 */
#include "ui.h"
#include "gfx.h"
#include "display.h"
#include "button.h"
#include "settings.h"
#include "library.h"
#include "reader.h"
#include "portal.h"
#include "qrcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { SCR_HOME, SCR_LIBRARY, SCR_READER, SCR_SETTINGS, SCR_WIFI, SCR_HELP, SCR_BACK };

/* ---------------------------------------------------------------- theme */

typedef struct {
    uint16_t bg, fg, dim, acc, acc_fg, line;
} theme_t;

static theme_t T;
static const font_t *F;   /* menu font */
static const font_t *FS;  /* small font */
static book_t *s_book;    /* open book, saved before sleeping */
static int64_t s_last_input;
static bool s_no_autosleep;
static bool s_layout_dirty;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void apply_theme(void)
{
    switch (g_set.theme) {
    case THEME_SEPIA:
        T = (theme_t){RGB565(0xF4, 0xEC, 0xD8), RGB565(0x3B, 0x2F, 0x20), RGB565(0x8A, 0x7B, 0x66),
                      RGB565(0x9C, 0x5B, 0x2E), RGB565(0xFF, 0xF8, 0xEE), RGB565(0xDB, 0xCF, 0xB5)};
        break;
    case THEME_DARK:
        T = (theme_t){RGB565(0, 0, 0), RGB565(0xDC, 0xDC, 0xDC), RGB565(0x86, 0x86, 0x86),
                      RGB565(0x2F, 0x6F, 0xD0), RGB565(0xFF, 0xFF, 0xFF), RGB565(0x30, 0x30, 0x30)};
        break;
    case THEME_NIGHT:
        T = (theme_t){RGB565(0, 0, 0), RGB565(0xF0, 0xA0, 0x40), RGB565(0x90, 0x5A, 0x20),
                      RGB565(0x6A, 0x3A, 0x0C), RGB565(0xFF, 0xC0, 0x70), RGB565(0x30, 0x1C, 0x08)};
        break;
    default:
        T = (theme_t){RGB565(0xFF, 0xFF, 0xFF), RGB565(0x14, 0x14, 0x14), RGB565(0x6E, 0x6E, 0x6E),
                      RGB565(0x21, 0x5F, 0xC8), RGB565(0xFF, 0xFF, 0xFF), RGB565(0xDD, 0xDD, 0xDD)};
        break;
    }
}

static void apply_fonts(void)
{
    int mn = gfx_w() < gfx_h() ? gfx_w() : gfx_h();
    F = g_fonts[g_set.ui_font_idx < FONT_COUNT ? g_set.ui_font_idx : 0];
    FS = g_fonts[mn <= 160 ? 0 : 1];
    if (FS->px > F->px) FS = F;
}

/* ---------------------------------------------------------------- frames & overlays */

enum { OV_NONE, OV_READY, OV_SLEEP, OV_TOAST };
static int s_ov;
static char s_toast[64];
static int64_t s_toast_until;
static gfx_draw_fn s_fn;
static void *s_arg;

static void draw_box_text(const char *msg)
{
    int W = gfx_w(), H = gfx_h();
    int bw = W - 16;
    int tw = gfx_text_width(F, msg, strlen(msg));
    int lines = 1;
    if (tw > bw - 12) {
        size_t len = strlen(msg), next;
        const char *s = msg;
        lines = 0;
        while (len) {
            gfx_wrap_line(F, s, len, bw - 12, &next);
            s += next;
            len -= next;
            lines++;
        }
    } else {
        bw = tw + 24;
    }
    int bh = lines * F->line_h + 12;
    int x = (W - bw) / 2, y = (H - bh) / 2;
    gfx_fill_round(x - 1, y - 1, bw + 2, bh + 2, 6, T.fg);
    gfx_fill_round(x, y, bw, bh, 5, T.bg);
    if (lines == 1) gfx_text(F, x + 12, y + 6, msg, strlen(msg), T.fg);
    else gfx_text_box(F, x + 6, y + 6, bw - 12, msg, T.fg, F->line_h);
}

static void frame(void *unused)
{
    (void)unused;
    s_fn(s_arg);
    if (s_ov == OV_READY) gfx_fill(0, 0, gfx_w(), 3, T.acc);
    else if (s_ov == OV_SLEEP) draw_box_text("Keep holding to sleep");
    else if (s_ov == OV_TOAST) draw_box_text(s_toast);
}

static void show(gfx_draw_fn fn, void *arg)
{
    s_fn = fn;
    s_arg = arg;
    gfx_render(frame, NULL);
}

static void redraw(void)
{
    if (s_fn) gfx_render(frame, NULL);
}

static void toast(const char *msg, int ms)
{
    snprintf(s_toast, sizeof(s_toast), "%s", msg);
    s_ov = OV_TOAST;
    s_toast_until = now_ms() + ms;
    redraw();
}

/* ---------------------------------------------------------------- sleep */

static void draw_sleep(void *a)
{
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    int y = H / 2 - F->line_h;
    gfx_text_center(F, y, "Sleeping", T.fg);
    gfx_text_center(FS, y + F->line_h + 4, button_pressed() ? "Release the button" : "Press the button to wake", T.dim);
}

static void go_sleep(void)
{
    if (s_book) book_save_progress(s_book);
    portal_stop();
    s_ov = OV_NONE;
    show(draw_sleep, NULL);
    button_wait_release();
    show(draw_sleep, NULL);
    vTaskDelay(pdMS_TO_TICKS(700));
    display_sleep(true);

#if CONFIG_EREADER_PIN_BL >= 0
    /* keep the backlight off while sleeping */
    gpio_reset_pin(CONFIG_EREADER_PIN_BL);
    gpio_set_direction(CONFIG_EREADER_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_EREADER_PIN_BL, CONFIG_EREADER_BL_ACTIVE_HIGH ? 0 : 1);
    gpio_hold_en(CONFIG_EREADER_PIN_BL);
    gpio_deep_sleep_hold_en();
#endif
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    uint64_t mask = button_wake_mask();
    for (int p = 0; p <= 21; p++) {
        if (!(mask & (1ULL << p))) continue;
#if CONFIG_EREADER_BTN_ACTIVE_LOW
        rtc_gpio_pullup_en(p);
        rtc_gpio_pulldown_dis(p);
#else
        rtc_gpio_pulldown_en(p);
        rtc_gpio_pullup_dis(p);
#endif
    }
#if CONFIG_EREADER_BTN_ACTIVE_LOW
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
#else
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
#endif
    esp_deep_sleep_start();
}

/* waits for a gesture, handling overlays, sleep and auto-sleep */
static btn_event_t next_event(int timeout_ms)
{
    int64_t start = now_ms();
    for (;;) {
        int wait = 400;
        if (timeout_ms >= 0) {
            int left = (int)(timeout_ms - (now_ms() - start));
            if (left <= 0) return BTN_NONE;
            if (left < wait) wait = left;
        }
        btn_event_t e = button_wait(wait);
        int64_t t = now_ms();
        if (e == BTN_NONE) {
            if (s_ov == OV_TOAST && t > s_toast_until) {
                s_ov = OV_NONE;
                redraw();
            }
            if (!s_no_autosleep && g_set.sleep_min && t - s_last_input > (int64_t)g_set.sleep_min * 60000) go_sleep();
            continue;
        }
        s_last_input = t;
        if (e == BTN_LONG_READY) { s_ov = OV_READY; redraw(); continue; }
        if (e == BTN_HOLD_HINT) { s_ov = OV_SLEEP; redraw(); continue; }
        if (e == BTN_POWER) go_sleep();
        s_ov = OV_NONE;
        return e;
    }
}

/* ---------------------------------------------------------------- common widgets */

static int header_h(void) { return F->line_h + 5; }
static int footer_h(void) { return FS->line_h + 3; }
static int row_h(void) { return F->line_h + (gfx_h() >= 200 ? 8 : 4); }

static void draw_header(const char *title)
{
    int W = gfx_w();
    gfx_fill(0, 0, W, header_h(), T.acc);
    gfx_text_fit(F, 5, 2, title, W - 10, T.acc_fg);
}

static void draw_footer(const char *const *hints, int n)
{
    int W = gfx_w(), H = gfx_h();
    int y = H - footer_h();
    gfx_fill(0, y, W, 1, T.line);
    const char *h = hints[n - 1];
    for (int i = 0; i < n; i++) {
        if (gfx_text_width(FS, hints[i], strlen(hints[i])) <= W - 8) { h = hints[i]; break; }
    }
    int tw = gfx_text_width(FS, h, strlen(h));
    int x = (W - tw) / 2;
    gfx_text_fit(FS, x < 3 ? 3 : x, y + 2, h, W - 6, T.dim);
}

/* footer hints; the first variant that fits the screen width is used */
enum { HK_LIST, HK_EDIT, HK_OK, HK_PAGER, HK_GOTO, HK_FINISH, HK_TEST, HK_COUNT };

static const char *const HINTS_1BTN[HK_COUNT][3] = {
    {"Tap: down · Double: up · Hold: select", "Tap ▼  Double ▲  Hold ✓", "▼tap ▲2× ✓hold"},
    {"Tap: next · Double: back · Hold: done", "Tap ▶  Double ◀  Hold ✓", "▶tap ◀2× ✓hold"},
    {"Hold to continue", "Hold ✓", "✓"},
    {"Tap: next · Double: back · Hold: close", "Tap ▶  Double ◀  Hold ✓", "▶ ◀ ✓"},
    {"Tap: +5% · Double: −5% · Hold: go", "Tap +  Double −  Hold ✓", "+ − ✓"},
    {"Hold to finish", "Hold ✓", "✓"},
    {"Hold 2 s to finish", "Hold 2 s ✓", "2 s ✓"},
};
static const char *const HINTS_3BTN[HK_COUNT][3] = {
    {"▲ ▼ move · OK select · hold OK back", "▲▼ move  OK ✓  hold ←", "▲▼ OK"},
    {"▲ ▼ change · OK done", "▲▼ change  OK ✓", "▲▼ OK"},
    {"OK to continue", "OK", "OK"},
    {"▲ ▼ page · OK close", "▲▼  OK ✓", "▲▼ OK"},
    {"▲ −5% · ▼ +5% · OK go", "▲▼ ±5%  OK ✓", "▲▼ OK"},
    {"OK to finish", "OK", "OK"},
    {"Hold OK 2 s to finish", "Hold OK 2 s", "OK 2 s"},
};

static void draw_hint(int kind)
{
    draw_footer(button_has_nav() ? HINTS_3BTN[kind] : HINTS_1BTN[kind], 3);
}

typedef void (*item_fn)(void *ctx, int i, char *label, size_t lsz, char *value, size_t vsz);

typedef struct {
    const char *title;
    int n, sel, top, editing;
    item_fn item;
    void *ctx;
    int (*progress)(void *ctx, int i); /* optional: 0-100 bar under the label, -1 = none */
} list_t;

static int list_rows(void)
{
    int r = (gfx_h() - header_h() - footer_h() - 2) / row_h();
    return r < 1 ? 1 : r;
}

static void list_fix(list_t *l)
{
    if (l->n <= 0) { l->sel = l->top = 0; return; }
    if (l->sel < 0) l->sel = l->n - 1;
    if (l->sel >= l->n) l->sel = 0;
    int rows = list_rows();
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
    if (l->top > l->n - rows) l->top = l->n - rows;
    if (l->top < 0) l->top = 0;
}

static void draw_list(void *a)
{
    list_t *l = a;
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    draw_header(l->title);
    int rows = list_rows(), rh = row_h();
    int y0 = header_h() + 1;
    bool scroll = l->n > rows;
    int right = W - (scroll ? 5 : 2);
    char label[112], value[40];
    for (int r = 0; r < rows && l->top + r < l->n; r++) {
        int i = l->top + r;
        int y = y0 + r * rh;
        label[0] = value[0] = 0;
        l->item(l->ctx, i, label, sizeof(label), value, sizeof(value));
        bool sel = i == l->sel;
        bool edit = sel && l->editing == i;
        uint16_t fg = sel ? T.acc_fg : T.fg, vc = sel ? T.acc_fg : T.dim;
        if (sel) gfx_fill_round(2, y + 1, right - 2, rh - 2, 4, T.acc);
        int ty = y + (rh - F->line_h) / 2;
        char vbuf[48];
        if (edit) snprintf(vbuf, sizeof(vbuf), "◀ %s ▶", value);
        else snprintf(vbuf, sizeof(vbuf), "%s", value);
        int vw = vbuf[0] ? gfx_text_width(F, vbuf, strlen(vbuf)) : 0;
        int lw = right - 8 - 6 - (vw ? vw + 6 : 0);
        if (lw < 16) {
            lw = right - 14;
            vw = 0;
        }
        int pct = l->progress ? l->progress(l->ctx, i) : -1;
        if (pct >= 0) ty -= 1;
        gfx_text_fit(F, 7, ty, label, lw, fg);
        if (vw) gfx_text(F, right - 6 - vw, ty, vbuf, strlen(vbuf), vc);
        if (pct >= 0) {
            int bw = right - 14, by = y + rh - 4;
            uint16_t track = sel ? gfx_mix(T.acc, T.acc_fg, 70) : T.line;
            gfx_fill(7, by, bw, 2, track);
            gfx_fill(7, by, bw * pct / 100, 2, sel ? T.acc_fg : T.acc);
        }
    }
    if (scroll) {
        int area = rows * rh;
        int th = area * rows / l->n;
        if (th < 6) th = 6;
        int ty = y0 + (area - th) * l->top / (l->n - rows);
        gfx_fill(W - 3, y0, 2, area, T.line);
        gfx_fill(W - 3, ty, 2, th, T.dim);
    }
    draw_hint(l->editing >= 0 ? HK_EDIT : HK_LIST);
}

/* runs a plain list: returns the chosen index, or -1 for back */
static int list_run(list_t *l)
{
    for (;;) {
        list_fix(l);
        show(draw_list, l);
        btn_event_t e = next_event(-1);
        if (e == BTN_NEXT) l->sel++;
        else if (e == BTN_PREV) l->sel--;
        else if (e == BTN_SELECT) { list_fix(l); return l->sel; }
        else if (e == BTN_BACK) return -1;
    }
}

/* plain message with a title; waits for a hold (or tap) */
typedef struct {
    const char *title, *body;
} msg_t;

static void draw_msg(void *a)
{
    msg_t *m = a;
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    draw_header(m->title);
    gfx_text_box(F, 6, header_h() + 4, W - 12, m->body, T.fg, F->line_h);
    draw_hint(HK_OK);
}

static void message(const char *title, const char *body)
{
    msg_t m = {title, body};
    for (;;) {
        show(draw_msg, &m);
        btn_event_t e = next_event(-1);
        if (e == BTN_SELECT || e == BTN_BACK || e == BTN_NEXT || e == BTN_PREV) return;
    }
}

/* progress screen (used while preparing a book) */
typedef struct {
    const char *title;
    const char *what;
    int pct;
    int64_t last;
} prog_t;

static void draw_progress(void *a)
{
    prog_t *p = a;
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    int y = H / 2 - F->line_h * 2;
    gfx_text_box(F, 8, y, W - 16, p->title, T.fg, F->line_h);
    y = H / 2 + 4;
    char t[48];
    snprintf(t, sizeof(t), "%s… %d%%", p->what, p->pct);
    gfx_text_center(FS, y, t, T.dim);
    y += FS->line_h + 4;
    int bw = W - 24;
    gfx_fill_round(12, y, bw, 6, 3, T.line);
    gfx_fill_round(12, y, bw * p->pct / 100 < 6 ? 6 : bw * p->pct / 100, 6, 3, T.acc);
}

static void progress_cb(void *arg, const char *what, int pct)
{
    prog_t *p = arg;
    int64_t t = now_ms();
    if (p->what == what && pct < 100 && t - p->last < 250) return;
    p->what = what;
    p->pct = pct;
    p->last = t;
    show(draw_progress, p);
}

/* ---------------------------------------------------------------- menu items */

enum {
    I_BACK, I_CONTINUE, I_LIBRARY, I_WIFI, I_SETTINGS, I_HELP, I_SLEEP, I_ABOUT,
    I_RESUME, I_CHAPTERS, I_GOTO, I_HOME,
    I_FONT, I_LINESP, I_MARGIN, I_PARA, I_JUSTIFY, I_THEME, I_BRIGHT, I_STATUS,
    I_ROTATE, I_AUTOSLEEP, I_UIFONT, I_WIFIMODE,
    I_FORGET_WIFI, I_TEST, I_BTNTEST, I_CLEAR_CACHE, I_RESET,
    I_YES, I_NO,
};

static int s_book_count;

static const char *item_label(int id)
{
    switch (id) {
    case I_BACK: return "‹ Back";
    case I_CONTINUE: return "Continue reading";
    case I_LIBRARY: return "Library";
    case I_WIFI: return "WiFi transfer";
    case I_SETTINGS: return "Settings";
    case I_HELP: return "Help";
    case I_SLEEP: return "Sleep";
    case I_ABOUT: return "About";
    case I_RESUME: return "Resume";
    case I_CHAPTERS: return "Chapters";
    case I_GOTO: return "Go to…";
    case I_HOME: return "Home";
    case I_FONT: return "Text size";
    case I_LINESP: return "Spacing";
    case I_MARGIN: return "Margins";
    case I_PARA: return "Paragraph";
    case I_JUSTIFY: return "Justify";
    case I_THEME: return "Theme";
    case I_BRIGHT: return "Brightness";
    case I_STATUS: return "Status bar";
    case I_ROTATE: return "Screen";
    case I_AUTOSLEEP: return "Auto sleep";
    case I_UIFONT: return "Menu size";
    case I_WIFIMODE: return "WiFi";
    case I_FORGET_WIFI: return "Forget home WiFi";
    case I_TEST: return "Display test";
    case I_BTNTEST: return "Button test";
    case I_CLEAR_CACHE: return "Clear book cache";
    case I_RESET: return "Reset settings";
    case I_YES: return "Yes";
    case I_NO: return "No";
    }
    return "?";
}

static bool item_has_value(int id) { return id >= I_FONT && id <= I_WIFIMODE; }

static int max_font(int chars)
{
    int best = 0;
    for (int i = 0; i < FONT_COUNT; i++)
        if (g_fonts[i]->px * chars <= gfx_w() * 2 * 10 / 11) best = i;
    return best;
}

static const uint8_t SLEEP_STEPS[] = {0, 2, 5, 10, 15, 30, 60};

static void item_value(int id, char *v, size_t sz)
{
    static const char *const LS[] = {"Compact", "Normal", "Relaxed"};
    static const char *const MG[] = {"Narrow", "Normal", "Wide"};
    static const char *const TH[] = {"Light", "Sepia", "Dark", "Night"};
    v[0] = 0;
    switch (id) {
    case I_FONT: snprintf(v, sz, "%d px", g_fonts[g_set.font_idx]->px); break;
    case I_UIFONT: snprintf(v, sz, "%d px", g_fonts[g_set.ui_font_idx]->px); break;
    case I_LINESP: snprintf(v, sz, "%s", LS[g_set.line_sp % 3]); break;
    case I_MARGIN: snprintf(v, sz, "%s", MG[g_set.margin % 3]); break;
    case I_PARA: snprintf(v, sz, "%s", g_set.para ? "Spaced" : "Indent"); break;
    case I_JUSTIFY: snprintf(v, sz, "%s", g_set.justify ? "On" : "Off"); break;
    case I_THEME: snprintf(v, sz, "%s", TH[g_set.theme % THEME_COUNT]); break;
    case I_BRIGHT: snprintf(v, sz, "%d%%", g_set.brightness); break;
    case I_STATUS: snprintf(v, sz, "%s", g_set.status_bar ? "On" : "Off"); break;
    case I_ROTATE: {
        bool land = gfx_w() > gfx_h();
        snprintf(v, sz, "%s%s", land ? "Landscape" : "Portrait", g_set.rotation >= 2 ? " ↓" : "");
        break;
    }
    case I_AUTOSLEEP:
        if (g_set.sleep_min) snprintf(v, sz, "%d min", g_set.sleep_min);
        else snprintf(v, sz, "Never");
        break;
    case I_WIFIMODE: snprintf(v, sz, "%s", g_set.wifi_mode ? "Hotspot" : "Auto"); break;
    case I_LIBRARY: snprintf(v, sz, "%d", s_book_count); break;
    case I_RESUME: if (s_book) snprintf(v, sz, "%d%%", book_percent(s_book)); break;
    }
}

static int wrap(int v, int dir, int n) { return ((v + dir) % n + n) % n; }

static void item_step(int id, int dir)
{
    switch (id) {
    case I_FONT: g_set.font_idx = wrap(g_set.font_idx, dir, max_font(12) + 1); s_layout_dirty = true; break;
    case I_UIFONT: g_set.ui_font_idx = wrap(g_set.ui_font_idx, dir, max_font(14) + 1); apply_fonts(); break;
    case I_LINESP: g_set.line_sp = wrap(g_set.line_sp, dir, 3); s_layout_dirty = true; break;
    case I_MARGIN: g_set.margin = wrap(g_set.margin, dir, 3); s_layout_dirty = true; break;
    case I_PARA: g_set.para ^= 1; s_layout_dirty = true; break;
    case I_JUSTIFY: g_set.justify ^= 1; if (s_book) s_book->justify = g_set.justify; break;
    case I_THEME: g_set.theme = wrap(g_set.theme, dir, THEME_COUNT); apply_theme(); break;
    case I_BRIGHT: {
        int b = g_set.brightness;
        if (dir > 0) b = b >= 100 ? 5 : b < 10 ? 10 : b + 10;
        else b = b <= 5 ? 100 : b <= 10 ? 5 : b - 10;
        g_set.brightness = b;
        display_set_backlight(b);
        break;
    }
    case I_STATUS: g_set.status_bar ^= 1; s_layout_dirty = true; break;
    case I_ROTATE:
        g_set.rotation = wrap(g_set.rotation, dir, 4);
        display_set_rotation(g_set.rotation);
        apply_fonts();
        if (g_set.font_idx > max_font(12)) g_set.font_idx = max_font(12);
        if (g_set.ui_font_idx > max_font(14)) g_set.ui_font_idx = max_font(14);
        apply_fonts();
        s_layout_dirty = true;
        break;
    case I_AUTOSLEEP: {
        int k = 0;
        for (size_t i = 0; i < sizeof(SLEEP_STEPS); i++)
            if (SLEEP_STEPS[i] == g_set.sleep_min) k = (int)i;
        g_set.sleep_min = SLEEP_STEPS[wrap(k, dir, sizeof(SLEEP_STEPS))];
        break;
    }
    case I_WIFIMODE: g_set.wifi_mode ^= 1; break;
    }
}

typedef struct {
    const int *ids;
} menu_ctx_t;

static void menu_item(void *ctx, int i, char *label, size_t lsz, char *value, size_t vsz)
{
    menu_ctx_t *m = ctx;
    snprintf(label, lsz, "%s", item_label(m->ids[i]));
    item_value(m->ids[i], value, vsz);
}

/* runs a menu; value items are edited in place; returns the chosen action id */
static int menu_run(const char *title, const int *ids, int n, int *sel_io)
{
    menu_ctx_t ctx = {ids};
    list_t l = {.title = title, .n = n, .sel = sel_io ? *sel_io : 0, .editing = -1, .item = menu_item, .ctx = &ctx};
    for (;;) {
        list_fix(&l);
        show(draw_list, &l);
        btn_event_t e = next_event(-1);
        if (l.editing >= 0) {
            int id = ids[l.editing];
            if (e == BTN_CLICK) item_step(id, +1);
            else if (e == BTN_DOUBLE) item_step(id, -1);
            else if (e == BTN_LONG || e == BTN_BACK) { l.editing = -1; settings_save(); }
            continue;
        }
        if (e == BTN_BACK) {
            if (sel_io) *sel_io = l.sel;
            return I_BACK;
        }
        if (e == BTN_CLICK) l.sel++;
        else if (e == BTN_DOUBLE) l.sel--;
        else if (e == BTN_LONG) {
            list_fix(&l);
            int id = ids[l.sel];
            if (item_has_value(id)) { l.editing = l.sel; continue; }
            if (sel_io) *sel_io = l.sel;
            return id;
        }
    }
}

static bool confirm(const char *question)
{
    static const int ids[] = {I_NO, I_YES};
    return menu_run(question, ids, 2, NULL) == I_YES;
}

/* ---------------------------------------------------------------- simple screens */

static void draw_test(void *a)
{
    int W = gfx_w(), H = gfx_h();
    static const uint16_t cols[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    static const char *const names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
    int bh = (H - header_h()) / 5;
    draw_header("Display test");
    for (int i = 0; i < 5; i++) {
        int y = header_h() + i * bh;
        gfx_fill(0, y, W, i == 4 ? H - y : bh, cols[i]);
        gfx_text(F, 6, y + (bh - F->line_h) / 2, names[i], strlen(names[i]), i == 3 ? 0x0000 : 0xFFFF);
    }
    gfx_rect(0, header_h(), W, H - header_h(), 0x07E0);
}

static void display_test(void)
{
    for (;;) {
        show(draw_test, NULL);
        btn_event_t e = next_event(-1);
        if (e == BTN_SELECT || e == BTN_BACK || e == BTN_NEXT) break;
    }
    message("Display test",
            "Labels should match the colours and the green frame should touch every edge. "
            "If red and blue are swapped, change 'Colour order' in menuconfig. If white looks black, "
            "change 'Colour inversion'. Mirrored text: enable 'Mirror horizontally'.");
}

static void about(void)
{
    char body[400];
    uint64_t total = 0, fr = 0;
    storage_info(&total, &fr);
    snprintf(body, sizeof(body),
             "E-Reader firmware © tegarsantosa.com %s\n%s, %dx%d\nStorage: %llu KB free of %llu KB\nMemory: %u KB free\nIDF %s",
             EREADER_VERSION, display_name(), gfx_w(), gfx_h(), fr / 1024, total / 1024,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024), esp_get_idf_version());
    message("About", body);
}

/* multi-page text viewer */
static const char HELP_TEXT[] =
    "CONTROLS\n"
    "Tap: next page or next item.\n"
    "Double tap: previous page or item.\n"
    "Hold until a bar appears at the top, then release: open the menu or select.\n"
    "Hold 3 seconds: sleep. Press to wake - you return to your page.\n"
    "\n"
    "ADD BOOKS\n"
    "1. Choose WiFi transfer on the home screen.\n"
    "2. Join the WiFi network shown on screen with your phone or computer.\n"
    "3. Open the address shown (for example http://192.168.4.1) in a browser.\n"
    "4. Upload EPUB, TXT or Markdown files.\n"
    "\n"
    "READING\n"
    "Hold while reading to open the book menu: chapters, go to a position, text size, screen orientation, "
    "theme and brightness. A book is prepared once, the first time you open it. Your page is saved automatically.";

static const char HELP_TEXT_3BTN[] =
    "CONTROLS\n"
    "Down: next page or next item.\n"
    "Up: previous page or item.\n"
    "OK: open the menu or select.\n"
    "Hold OK, then release: go back.\n"
    "Hold OK 3 seconds: sleep. Press OK to wake - you return to your page.\n"
    "\n"
    "ADD BOOKS\n"
    "1. Choose WiFi transfer on the home screen.\n"
    "2. Join the WiFi network shown on screen with your phone or computer.\n"
    "3. Open the address shown (for example http://192.168.4.1) in a browser.\n"
    "4. Upload EPUB, TXT or Markdown files.\n"
    "\n"
    "READING\n"
    "Press OK while reading to open the book menu: chapters, go to a position, text size, screen "
    "orientation, theme and brightness. A book is prepared once, the first time you open it. "
    "Your page is saved automatically.";

static const char *help_text(void) { return button_has_nav() ? HELP_TEXT_3BTN : HELP_TEXT; }

typedef struct {
    const char *title;
    const char *text;
    int page, pages, lines_per_page;
    int line_start[160];
    int nlines;
} pager_t;

static void pager_layout(pager_t *p)
{
    int W = gfx_w();
    int avail = gfx_h() - header_h() - footer_h() - 6;
    p->lines_per_page = avail / FS->line_h > 0 ? avail / FS->line_h : 1;
    const char *s = p->text;
    size_t len = strlen(s), pos = 0;
    p->nlines = 0;
    while (pos < len && p->nlines < 160) {
        size_t next;
        p->line_start[p->nlines++] = (int)pos;
        gfx_wrap_line(FS, s + pos, len - pos, W - 12, &next);
        pos += next ? next : 1;
    }
    p->pages = (p->nlines + p->lines_per_page - 1) / p->lines_per_page;
    if (p->pages < 1) p->pages = 1;
    if (p->page >= p->pages) p->page = p->pages - 1;
}

static void draw_pager(void *a)
{
    pager_t *p = a;
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    char t[64];
    snprintf(t, sizeof(t), "%s  %d/%d", p->title, p->page + 1, p->pages);
    draw_header(t);
    int y = header_h() + 3;
    size_t len = strlen(p->text);
    for (int i = 0; i < p->lines_per_page; i++) {
        int li = p->page * p->lines_per_page + i;
        if (li >= p->nlines) break;
        size_t next;
        const char *s = p->text + p->line_start[li];
        size_t n = gfx_wrap_line(FS, s, len - p->line_start[li], W - 12, &next);
        bool heading = n > 0 && s[0] >= 'A' && s[0] <= 'Z' && n > 3 && s[1] >= 'A' && s[1] <= 'Z';
        gfx_text(FS, 6, y, s, n, heading ? T.acc : T.fg);
        y += FS->line_h;
    }
    draw_hint(HK_PAGER);
}

static void pager(const char *title, const char *text)
{
    pager_t *p = calloc(1, sizeof(pager_t));
    if (!p) return;
    p->title = title;
    p->text = text;
    pager_layout(p);
    for (;;) {
        show(draw_pager, p);
        btn_event_t e = next_event(-1);
        if (e == BTN_CLICK) {
            if (p->page + 1 < p->pages) p->page++;
            else break;
        } else if (e == BTN_DOUBLE) {
            if (p->page > 0) p->page--;
        } else if (e == BTN_SELECT || e == BTN_BACK) break;
    }
    free(p);
}

/* shows which GPIO each button is wired to */
typedef struct {
    int pressed[8], npressed;
    int seen[8], nseen;
} btntest_t;

static void draw_btntest(void *a)
{
    btntest_t *b = a;
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    draw_header("Button test");
    int y = header_h() + 3;
    gfx_text_box(FS, 6, y, W - 12, "Press each button. Its GPIO number appears below.", T.dim, FS->line_h);
    const font_t *big = g_fonts[font_index_for_px(W / 6)];
    char t[64];
    if (b->npressed) snprintf(t, sizeof(t), "GPIO %d", b->pressed[0]);
    else snprintf(t, sizeof(t), "-");
    int by = (H - big->line_h) / 2;
    gfx_text_center(big, by, t, b->npressed ? T.acc : T.dim);
    int n = snprintf(t, sizeof(t), "Seen:");
    for (int i = 0; i < b->nseen && n < (int)sizeof(t) - 5; i++) n += snprintf(t + n, sizeof(t) - n, " %d", b->seen[i]);
    gfx_text_center(FS, by + big->line_h + 4, b->nseen ? t : "Seen: none yet", T.fg);
    draw_hint(HK_TEST);
}

static void button_test(void)
{
    btntest_t b = {0};
    button_probe_begin();
    for (;;) {
        b.npressed = button_probe_read(b.pressed, 8);
        for (int i = 0; i < b.npressed; i++) {
            bool known = false;
            for (int k = 0; k < b.nseen; k++) known |= b.seen[k] == b.pressed[i];
            if (!known && b.nseen < 8) b.seen[b.nseen++] = b.pressed[i];
        }
        show(draw_btntest, &b);
        btn_event_t e = button_wait(60);
        if (e == BTN_HOLD_HINT || e == BTN_POWER) break;
    }
    button_probe_end();
    s_ov = OV_NONE;
    button_wait_release();
    button_flush();
    s_last_input = now_ms();
}

/* ---------------------------------------------------------------- settings */

static void settings_screen(void)
{
    static const int ids[] = {I_BACK, I_FONT, I_LINESP, I_MARGIN, I_PARA, I_JUSTIFY, I_STATUS,
                              I_THEME, I_BRIGHT, I_ROTATE, I_UIFONT, I_AUTOSLEEP, I_WIFIMODE,
                              I_FORGET_WIFI, I_TEST, I_BTNTEST, I_CLEAR_CACHE, I_RESET, I_ABOUT};
    int sel = 0;
    for (;;) {
        int id = menu_run("Settings", ids, sizeof(ids) / sizeof(ids[0]), &sel);
        switch (id) {
        case I_BACK: return;
        case I_FORGET_WIFI:
            if (!g_set.sta_ssid[0]) { toast("No home WiFi saved", 1500); break; }
            if (confirm("Forget home WiFi?")) {
                g_set.sta_ssid[0] = g_set.sta_pass[0] = 0;
                settings_save();
            }
            break;
        case I_TEST: display_test(); break;
        case I_BTNTEST: button_test(); break;
        case I_CLEAR_CACHE:
            if (confirm("Clear book cache?")) {
                library_clear_cache();
                toast("Cache cleared", 1200);
            }
            break;
        case I_RESET:
            if (confirm("Reset all settings?")) {
                settings_reset();
                display_set_rotation(g_set.rotation);
                display_set_backlight(g_set.brightness);
                apply_theme();
                apply_fonts();
                s_layout_dirty = true;
            }
            break;
        case I_ABOUT: about(); break;
        }
    }
}

/* ---------------------------------------------------------------- reader */

static void draw_reader(void *a)
{
    reader_colors_t c = {T.bg, T.fg, T.dim, T.acc};
    book_draw((book_t *)a, &c);
}

typedef struct {
    book_t *b;
} chap_ctx_t;

static void chap_item(void *ctx, int i, char *label, size_t lsz, char *value, size_t vsz)
{
    book_t *b = ((chap_ctx_t *)ctx)->b;
    if (i == 0) { snprintf(label, lsz, "‹ Back"); return; }
    toc_entry_t *t = &b->toc[i - 1];
    snprintf(label, lsz, "%s", t->title);
    snprintf(value, vsz, "%lu", (unsigned long)book_page_for_offset(b, t->off) + 1);
}

static bool chapters_screen(book_t *b)
{
    chap_ctx_t ctx = {b};
    int cur = book_chapter_index(b, b->page);
    list_t l = {.title = "Chapters", .n = b->ntoc + 1, .sel = cur + 1, .editing = -1, .item = chap_item, .ctx = &ctx};
    int i = list_run(&l);
    if (i <= 0) return false;
    book_goto_page(b, book_page_for_offset(b, b->toc[i - 1].off));
    return true;
}

typedef struct {
    book_t *b;
    uint32_t target;
} goto_t;

static void draw_goto(void *a)
{
    goto_t *g = a;
    book_t *b = g->b;
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    draw_header("Go to");
    int pct = b->npages > 1 ? (int)((uint64_t)g->target * 100 / (b->npages - 1)) : 100;
    char t[32];
    snprintf(t, sizeof(t), "%d%%", pct);
    const font_t *big = g_fonts[FONT_COUNT - 1];
    for (int i = FONT_COUNT - 1; i >= 0; i--) {
        big = g_fonts[i];
        if (big->line_h * 3 < H / 2 && gfx_text_width(big, "100%", 4) < W - 20) break;
    }
    int y = header_h() + (H - header_h() - footer_h()) / 2 - big->line_h;
    gfx_text_center(big, y, t, T.fg);
    y += big->line_h + 2;
    snprintf(t, sizeof(t), "Page %lu of %lu", (unsigned long)g->target + 1, (unsigned long)b->npages);
    gfx_text_center(FS, y, t, T.dim);
    y += FS->line_h + 4;
    int bw = W - 20;
    gfx_fill_round(10, y, bw, 6, 3, T.line);
    gfx_fill_round(10, y, 6 + (bw - 6) * pct / 100, 6, 3, T.acc);
    y += 10;
    int ch = book_chapter_index(b, g->target);
    if (ch >= 0) {
        int tw = gfx_text_width(FS, b->toc[ch].title, strlen(b->toc[ch].title));
        int x = (W - tw) / 2;
        gfx_text_fit(FS, x < 6 ? 6 : x, y, b->toc[ch].title, W - 12, T.dim);
    }
    draw_hint(HK_GOTO);
}

static void goto_screen(book_t *b)
{
    goto_t g = {b, b->page};
    uint32_t step = b->npages / 20 ? b->npages / 20 : 1;
    for (;;) {
        show(draw_goto, &g);
        btn_event_t e = next_event(-1);
        if (e == BTN_CLICK) g.target = g.target + step >= b->npages ? 0 : g.target + step;
        else if (e == BTN_DOUBLE) g.target = g.target >= step ? g.target - step : b->npages - 1;
        else if (e == BTN_SELECT) {
            book_goto_page(b, g.target);
            return;
        } else if (e == BTN_BACK) return;
    }
}

/* returns the next screen, or -1 to keep reading */
static int reader_menu(book_t *b)
{
    int ids[16], n = 0;
    ids[n++] = I_RESUME;
    if (b->ntoc) ids[n++] = I_CHAPTERS;
    ids[n++] = I_GOTO;
    ids[n++] = I_FONT;
    ids[n++] = I_ROTATE;
    ids[n++] = I_THEME;
    ids[n++] = I_BRIGHT;
    ids[n++] = I_SETTINGS;
    ids[n++] = I_LIBRARY;
    ids[n++] = I_HOME;
    ids[n++] = I_SLEEP;
    int sel = 0;
    for (;;) {
        int id = menu_run(b->title, ids, n, &sel);
        switch (id) {
        case I_RESUME:
        case I_BACK: return -1;
        case I_CHAPTERS: if (chapters_screen(b)) return -1; break;
        case I_GOTO: goto_screen(b); return -1;
        case I_SETTINGS: settings_screen(); break;
        case I_LIBRARY: return SCR_LIBRARY;
        case I_HOME: return SCR_HOME;
        case I_SLEEP: go_sleep(); break;
        }
    }
}

static int reader_screen(const char *name)
{
    book_t *b = calloc(1, sizeof(book_t));
    if (!b) return SCR_HOME;
    char err[96] = "";
    char title[104];
    library_display_name(name, title, sizeof(title));
    prog_t p = {.title = title, .what = "Opening", .pct = 0};
    show(draw_progress, &p);
    if (book_open(b, name, progress_cb, &p, err, sizeof(err)) != 0) {
        free(b);
        message("Cannot open book", err[0] ? err : "Unknown error");
        return SCR_LIBRARY;
    }
    s_book = b;
    if (strcmp(g_set.last_book, name) != 0) {
        snprintf(g_set.last_book, sizeof(g_set.last_book), "%s", name);
        settings_save();
    }
    bool dirty = false;
    int next = SCR_LIBRARY;
    for (;;) {
        if (s_layout_dirty) {
            s_layout_dirty = false;
            prog_t rp = {.title = b->title, .what = "Paginating", .pct = 0};
            if (book_relayout(b, progress_cb, &rp) != 0) {
                message("Storage full", "Cannot store the page index. Delete some books or clear the cache.");
                break;
            }
        }
        show(draw_reader, b);
        btn_event_t e = next_event(dirty ? 4000 : -1);
        if (e == BTN_NONE) {
            if (dirty) book_save_progress(b);
            dirty = false;
            continue;
        }
        if (e == BTN_CLICK) {
            if (book_next(b)) dirty = true;
            else toast("End of book", 1200);
        } else if (e == BTN_DOUBLE) {
            if (book_prev(b)) dirty = true;
            else toast("Start of book", 1000);
        } else if (e == BTN_LONG) {
            int r = reader_menu(b);
            dirty = true;
            if (r >= 0) { next = r; break; }
        } else if (e == BTN_BACK) {
            next = SCR_BACK; /* back to wherever the book was opened from */
            break;
        }
    }
    book_save_progress(b);
    s_book = NULL;
    book_close(b);
    free(b);
    return next;
}

/* ---------------------------------------------------------------- library */

typedef struct {
    lib_entry_t *list;
    int n;
} lib_ctx_t;

static void lib_item(void *ctx, int i, char *label, size_t lsz, char *value, size_t vsz)
{
    lib_ctx_t *c = ctx;
    if (i == 0) { snprintf(label, lsz, "‹ Back"); return; }
    lib_entry_t *e = &c->list[i - 1];
    library_display_name(e->name, label, lsz);
    if (e->pct < 0) snprintf(value, vsz, "new");
    else if (e->pct >= 100) snprintf(value, vsz, "✓");
    else snprintf(value, vsz, "%d%%", e->pct);
}

static int lib_progress(void *ctx, int i)
{
    lib_ctx_t *c = ctx;
    if (i == 0) return -1;
    int p = c->list[i - 1].pct;
    return p < 0 ? -1 : p > 100 ? 100 : p;
}

static int library_screen(void)
{
    int sel = -1;
    for (;;) {
        lib_ctx_t c;
        c.n = library_scan(&c.list);
        s_book_count = c.n;
        if (c.n == 0) {
            library_free(c.list);
            static const int ids[] = {I_WIFI, I_BACK};
            int id = menu_run("No books yet", ids, 2, NULL);
            return id == I_WIFI ? SCR_WIFI : SCR_HOME;
        }
        if (sel < 0) {
            sel = 1;
            for (int i = 0; i < c.n; i++)
                if (!strcmp(c.list[i].name, g_set.last_book)) sel = i + 1;
        }
        char title[32];
        snprintf(title, sizeof(title), "Library (%d)", c.n);
        list_t l = {.title = title, .n = c.n + 1, .sel = sel, .editing = -1,
                    .item = lib_item, .ctx = &c, .progress = lib_progress};
        int chosen = list_run(&l);
        if (chosen <= 0) {
            library_free(c.list);
            return SCR_HOME;
        }
        sel = chosen;
        char name[MAX_NAME + 1];
        snprintf(name, sizeof(name), "%s", c.list[chosen - 1].name);
        library_free(c.list);
        int next = reader_screen(name);
        if (next != SCR_LIBRARY && next != SCR_BACK) return next;
    }
}

/* ---------------------------------------------------------------- wifi */

enum { WP_INFO, WP_JOIN, WP_OPEN, WP_COUNT };

typedef struct {
    int page;
    int qr_size;
    const char *caption;
} wifi_view_t;

/* WIFI:T:WPA;S:<ssid>;P:<pass>;;  with \ ; , : " escaped */
static void wifi_qr_text(char *out, size_t sz, const char *ssid, const char *pass)
{
    size_t j = 0;
    const char *parts[2] = {ssid, pass};
    j += snprintf(out, sz, "WIFI:T:%s;S:", pass[0] ? "WPA" : "nopass");
    for (int k = 0; k < 2; k++) {
        if (k == 1) {
            if (!pass[0]) break;
            j += snprintf(out + j, sz - j, ";P:");
        }
        for (const char *p = parts[k]; *p && j + 3 < sz; p++) {
            if (strchr("\\;,:\"", *p)) out[j++] = '\\';
            out[j++] = *p;
        }
    }
    snprintf(out + j, sz - j, ";;");
}

static void draw_qr_page(wifi_view_t *v)
{
    int W = gfx_w(), H = gfx_h();
    int top = header_h() + 2, bottom = H - footer_h() - FS->line_h - 4;
    int avail = (bottom - top) < (W - 8) ? (bottom - top) : (W - 8);
    int n = v->qr_size + 4; /* 2 modules of quiet zone each side */
    int scale = avail / n;
    if (scale < 1) scale = 1;
    int px = n * scale;
    int x0 = (W - px) / 2, y0 = top + ((bottom - top) - px) / 2;
    gfx_fill(x0, y0, px, px, 0xFFFF); /* QR codes need a light background */
    for (int y = 0; y < v->qr_size; y++)
        for (int x = 0; x < v->qr_size; x++)
            if (qr_module(x, y)) gfx_fill(x0 + (x + 2) * scale, y0 + (y + 2) * scale, scale, scale, 0x0000);
    gfx_text_center(FS, y0 + px + 2, v->caption, T.fg);
}

static void draw_wifi(void *a)
{
    wifi_view_t *v = a;
    portal_status_t *st = portal_status();
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    char t[120];
    snprintf(t, sizeof(t), "WiFi transfer  %d/%d", v->page + 1, st->mode == PORTAL_AP ? 3 : 2);
    draw_header(st->mode == PORTAL_CONNECTING ? "WiFi transfer" : t);
    int x = 6, y = header_h() + 3, w = W - 12;
    const font_t *S = FS;
    static const char *const HW1[] = {"Tap: QR code · Hold: finish", "Tap ▶ QR  Hold ✓", "▶ ✓"};
    static const char *const HW3[] = {"▲ ▼ QR code · OK finish", "▲▼ QR  OK ✓", "▲▼ OK"};
    if (st->mode == PORTAL_CONNECTING) {
        snprintf(t, sizeof(t), "Connecting to %s…", g_set.sta_ssid);
        gfx_text_box(F, x, y, w, t, T.fg, F->line_h);
        return;
    }
    if (v->page != WP_INFO && v->qr_size) {
        draw_qr_page(v);
        draw_footer(button_has_nav() ? HW3 : HW1, 3);
        return;
    }
    if (st->mode == PORTAL_AP) {
        gfx_text(S, x, y, "1. Join WiFi", 12, T.dim);
        y += S->line_h;
        gfx_text_fit(F, x, y, st->ssid, w, T.fg);
        y += F->line_h;
        if (st->pass[0]) {
            snprintf(t, sizeof(t), "Password: %s", st->pass);
            y += gfx_text_box(S, x, y, w, t, T.fg, S->line_h);
        }
        y += 2;
        gfx_text(S, x, y, "2. Open in browser", 18, T.dim);
        y += S->line_h;
    } else {
        snprintf(t, sizeof(t), "On %s. Open in a browser:", st->ssid);
        y += gfx_text_box(S, x, y, w, t, T.dim, S->line_h);
    }
    snprintf(t, sizeof(t), "http://%s", st->ip);
    gfx_text_fit(F, x, y, t, w, T.acc);
    y += F->line_h + 3;
    gfx_fill(x, y, w, 1, T.line);
    y += 3;
    if (st->uploading) {
        int pct = st->up_total ? (int)((uint64_t)st->up_recv * 100 / st->up_total) : 0;
        snprintf(t, sizeof(t), "Receiving %d%%", pct);
        gfx_text(S, x, y, t, strlen(t), T.fg);
        y += S->line_h;
        gfx_text_fit(S, x, y, st->up_name, w, T.dim);
        y += S->line_h + 1;
        gfx_fill_round(x, y, w, 5, 2, T.line);
        gfx_fill_round(x, y, 5 + (w - 5) * pct / 100, 5, 2, T.acc);
    } else if (st->message[0]) {
        gfx_text_box(S, x, y, w, st->message, T.fg, S->line_h);
    } else {
        snprintf(t, sizeof(t), st->mode == PORTAL_AP && st->clients == 0 ? "Waiting for a device…" : "Ready for uploads");
        gfx_text(S, x, y, t, strlen(t), T.dim);
    }
    draw_footer(button_has_nav() ? HW3 : HW1, 3);
}

static void wifi_set_page(wifi_view_t *v, int page)
{
    portal_status_t *st = portal_status();
    int pages = st->mode == PORTAL_AP ? WP_COUNT : 2;
    v->page = ((page % pages) + pages) % pages;
    char text[200];
    v->qr_size = 0;
    if (v->page == WP_JOIN && st->mode == PORTAL_AP) {
        wifi_qr_text(text, sizeof(text), st->ssid, st->pass);
        v->caption = "Scan to join the WiFi";
    } else if (v->page != WP_INFO) {
        snprintf(text, sizeof(text), "http://%s/", st->ip);
        v->caption = "Scan to open the page";
    } else {
        return;
    }
    v->qr_size = qr_encode(text);
}

static int wifi_screen(void)
{
    portal_status_t *st = portal_status();
    wifi_view_t v = {0};
    s_no_autosleep = true;
    st->mode = (g_set.wifi_mode == 0 && g_set.sta_ssid[0]) ? PORTAL_CONNECTING : PORTAL_AP;
    st->message[0] = 0;
    st->uploading = false;
    if (st->mode == PORTAL_AP) snprintf(st->ip, sizeof(st->ip), "192.168.4.1");
    show(draw_wifi, &v);
    portal_start(g_set.wifi_mode == 0);
    /* start on the "scan to join" QR code: the fastest way to connect a phone */
    wifi_set_page(&v, st->mode == PORTAL_AP ? WP_JOIN : WP_INFO);
    for (;;) {
        if (st->uploading && v.page != WP_INFO) wifi_set_page(&v, WP_INFO);
        show(draw_wifi, &v); /* refreshed often to show upload progress */
        btn_event_t e = next_event(700);
        if (e == BTN_NEXT) wifi_set_page(&v, v.page + 1);
        else if (e == BTN_PREV) wifi_set_page(&v, v.page - 1);
        else if (e == BTN_SELECT || e == BTN_BACK) {
            if (st->uploading) {
                toast("Upload in progress", 1500);
                continue;
            }
            break;
        }
    }
    show(draw_msg, &(msg_t){"WiFi transfer", "Stopping WiFi…"});
    portal_stop();
    s_no_autosleep = false;
    s_last_input = now_ms();
    return st->received > 0 ? SCR_LIBRARY : SCR_HOME;
}

/* ---------------------------------------------------------------- home */

static bool file_exists(const char *name)
{
    char src[160];
    library_paths(name, src, NULL, NULL, NULL, sizeof(src));
    FILE *f = fopen(src, "rb");
    if (f) fclose(f);
    return f != NULL;
}

typedef struct {
    int ids[8], n, sel;
    bool card;          /* ids[0] is the "continue reading" card */
    char title[104];
    int pct;
} home_t;

static int home_card_h(void) { return 6 + FS->line_h + F->line_h + 9; }

static void draw_home(void *a)
{
    home_t *h = a;
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    int y;
    if (h->card) {
        int ch = home_card_h();
        bool sel = h->sel == 0;
        uint16_t bg = sel ? T.acc : gfx_mix(T.bg, T.acc, 38);
        uint16_t fg = sel ? T.acc_fg : T.fg, dim = sel ? gfx_mix(T.acc, T.acc_fg, 200) : T.dim;
        gfx_fill(0, 0, W, ch, bg);
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", h->pct < 0 ? 0 : h->pct);
        int pw = gfx_text_width(FS, pct, strlen(pct));
        gfx_text_fit(FS, 7, 3, "Continue reading", W - 20 - pw, dim);
        gfx_text(FS, W - 7 - pw, 3, pct, strlen(pct), dim);
        gfx_text_fit(F, 7, 3 + FS->line_h, h->title, W - 14, fg);
        int by = 3 + FS->line_h + F->line_h + 2, bw = W - 14;
        gfx_fill_round(7, by, bw, 4, 2, sel ? gfx_mix(T.acc, T.acc_fg, 70) : T.line);
        int fill = h->pct <= 0 ? 0 : bw * h->pct / 100;
        if (fill > 3) gfx_fill_round(7, by, fill, 4, 2, sel ? T.acc_fg : T.acc);
        y = ch + 2;
    } else {
        draw_header("E-Reader");
        y = header_h() + 1;
    }
    int first = h->card ? 1 : 0;
    int rh = row_h();
    int rows = (H - footer_h() - y) / rh;
    if (rows < 1) rows = 1;
    int m = h->n - first;
    int rsel = h->sel - first;
    int top = rsel >= rows ? rsel - rows + 1 : 0;
    char value[24];
    for (int r = 0; r < rows && top + r < m; r++) {
        int i = first + top + r;
        int ry = y + r * rh;
        bool sel = i == h->sel;
        if (sel) gfx_fill_round(2, ry + 1, W - 4, rh - 2, 4, T.acc);
        int ty = ry + (rh - F->line_h) / 2;
        item_value(h->ids[i], value, sizeof(value));
        int vw = value[0] ? gfx_text_width(F, value, strlen(value)) : 0;
        gfx_text_fit(F, 7, ty, item_label(h->ids[i]), W - 20 - vw, sel ? T.acc_fg : T.fg);
        if (vw) gfx_text(F, W - 8 - vw, ty, value, strlen(value), sel ? T.acc_fg : T.dim);
    }
    draw_hint(HK_LIST);
}

static int home_screen(void)
{
    home_t h = {0};
    lib_entry_t *list;
    s_book_count = library_scan(&list);
    library_free(list);
    if (g_set.last_book[0] && file_exists(g_set.last_book)) {
        h.card = true;
        library_display_name(g_set.last_book, h.title, sizeof(h.title));
        h.pct = -1;
        progress_get(g_set.last_book, NULL, &h.pct);
        h.ids[h.n++] = I_CONTINUE;
    }
    h.ids[h.n++] = I_LIBRARY;
    h.ids[h.n++] = I_WIFI;
    h.ids[h.n++] = I_SETTINGS;
    h.ids[h.n++] = I_HELP;
    h.ids[h.n++] = I_SLEEP;
    for (;;) {
        show(draw_home, &h);
        btn_event_t e = next_event(-1);
        if (e == BTN_NEXT) h.sel = (h.sel + 1) % h.n;
        else if (e == BTN_PREV) h.sel = (h.sel + h.n - 1) % h.n;
        else if (e == BTN_SELECT) {
            switch (h.ids[h.sel]) {
            case I_CONTINUE: {
                int next = reader_screen(g_set.last_book);
                return next == SCR_BACK ? SCR_HOME : next;
            }
            case I_LIBRARY: return SCR_LIBRARY;
            case I_WIFI: return SCR_WIFI;
            case I_SETTINGS: settings_screen(); return SCR_HOME;
            case I_HELP: pager("Help", help_text()); break;
            case I_SLEEP: go_sleep(); break;
            }
        }
    }
}

/* ---------------------------------------------------------------- main loop */

static void draw_splash(void *a)
{
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, T.bg);
    const font_t *big = g_fonts[font_index_for_px(gfx_w() / 7)];
    int y = H / 2 - big->line_h;
    gfx_text_center(big, y, "E-Reader", T.fg);
    gfx_fill((W - W / 3) / 2, y + big->line_h + 2, W / 3, 2, T.acc);
    gfx_text_center(FS, y + big->line_h + 8, (const char *)a, T.dim);
}

void ui_run(bool woke)
{
    apply_theme();
    apply_fonts();
    s_last_input = now_ms();
    uint64_t t, f;
    const char *sub = storage_info(&t, &f) ? "v" EREADER_VERSION : "Storage not mounted";
    show(draw_splash, (void *)sub);
    display_set_backlight(g_set.brightness);
    int scr = SCR_HOME;
    if (woke && g_set.last_book[0] && file_exists(g_set.last_book)) {
        scr = reader_screen(g_set.last_book);
        if (scr == SCR_BACK) scr = SCR_HOME;
    } else {
        vTaskDelay(pdMS_TO_TICKS(700));
        if (!g_set.seen_help) {
            g_set.seen_help = 1;
            settings_save();
            pager("Welcome", help_text());
        }
    }
    button_flush();
    for (;;) {
        switch (scr) {
        case SCR_LIBRARY: scr = library_screen(); break;
        case SCR_WIFI: scr = wifi_screen(); break;
        case SCR_HOME:
        default: scr = home_screen(); break;
        }
    }
}
