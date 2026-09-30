#include "button.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "button";

#define PIN_MAIN CONFIG_EREADER_BTN_GPIO
#define PIN_NEXT CONFIG_EREADER_BTN_NEXT_GPIO
#define PIN_PREV CONFIG_EREADER_BTN_PREV_GPIO
#define POLL_MS 10
#define HINT_MS ((CONFIG_EREADER_BTN_POWER_MS) / 2)
#define REPEAT_DELAY_MS 450
#define REPEAT_MS 160

static QueueHandle_t s_q;

static bool pin_pressed(int pin)
{
    if (pin < 0) return false;
    int level = gpio_get_level(pin);
#if CONFIG_EREADER_BTN_ACTIVE_LOW
    return level == 0;
#else
    return level == 1;
#endif
}

bool button_has_nav(void) { return PIN_NEXT >= 0 || PIN_PREV >= 0; }

bool button_pressed(void)
{
    return pin_pressed(PIN_MAIN) || pin_pressed(PIN_NEXT) || pin_pressed(PIN_PREV);
}

uint64_t button_wake_mask(void)
{
    uint64_t m = 0;
    const int pins[] = {PIN_MAIN, PIN_NEXT, PIN_PREV};
    for (int i = 0; i < 3; i++)
        if (pins[i] >= 0 && pins[i] <= 21) m |= 1ULL << pins[i];
    return m;
}

static void send(btn_event_t e)
{
    uint8_t v = (uint8_t)e;
    xQueueSend(s_q, &v, 0);
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* two equal samples in a row = debounced */
typedef struct {
    int pin;
    bool stable, last_raw, changed;
} deb_t;

static void deb_init(deb_t *d, int pin)
{
    d->pin = pin;
    d->stable = d->last_raw = pin_pressed(pin);
    d->changed = false;
}

static void deb_update(deb_t *d)
{
    bool raw = pin_pressed(d->pin);
    d->changed = raw == d->last_raw && raw != d->stable;
    if (d->changed) d->stable = raw;
    d->last_raw = raw;
}

/* up/down buttons: event on press, auto-repeat while held */
typedef struct {
    deb_t d;
    btn_event_t ev;
    bool ignore;      /* held since boot (e.g. used to wake up) */
    int64_t next_rep;
} nav_t;

static void nav_update(nav_t *n, int64_t t)
{
    if (n->d.pin < 0) return;
    deb_update(&n->d);
    if (n->ignore) {
        if (!n->d.stable) n->ignore = false;
        return;
    }
    if (n->d.changed && n->d.stable) {
        send(n->ev);
        n->next_rep = t + REPEAT_DELAY_MS;
    } else if (n->d.stable && t >= n->next_rep) {
        send(n->ev);
        n->next_rep = t + REPEAT_MS;
    }
}

static void button_task(void *arg)
{
    const bool nav_mode = button_has_nav();
    deb_t m;
    deb_init(&m, PIN_MAIN);
    nav_t next = {.ev = BTN_NEXT}, prev = {.ev = BTN_PREV};
    deb_init(&next.d, PIN_NEXT);
    deb_init(&prev.d, PIN_PREV);
    next.ignore = next.d.stable;
    prev.ignore = prev.d.stable;

    enum { IDLE, DOWN, WAIT2, DOWN2, IGNORE } st = m.stable ? IGNORE : IDLE;
    int64_t t_down = 0, t_up = 0;
    bool ready_sent = false, hint_sent = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int64_t t = now_ms();
        nav_update(&next, t);
        nav_update(&prev, t);
        deb_update(&m);
        switch (st) {
        case IDLE:
            if (m.changed && m.stable) {
                st = DOWN;
                t_down = t;
                ready_sent = hint_sent = false;
            }
            break;
        case DOWN: {
            int64_t held = t - t_down;
            if (m.stable) {
                if (held >= CONFIG_EREADER_BTN_POWER_MS) {
                    send(BTN_POWER);
                    st = IGNORE;
                } else if (held >= HINT_MS && !hint_sent) {
                    send(BTN_HOLD_HINT);
                    hint_sent = true;
                } else if (held >= CONFIG_EREADER_BTN_LONG_MS && !ready_sent) {
                    send(BTN_LONG_READY);
                    ready_sent = true;
                }
            } else if (m.changed) {
                if (nav_mode) {
                    /* OK button: press = select, hold + release = back */
                    send(held >= CONFIG_EREADER_BTN_LONG_MS ? BTN_BACK : BTN_SELECT);
                    st = IDLE;
                } else if (held >= CONFIG_EREADER_BTN_LONG_MS) {
                    send(BTN_SELECT);
                    st = IDLE;
                } else {
                    st = WAIT2;
                    t_up = t;
                }
            }
            break;
        }
        case WAIT2:
            if (m.changed && m.stable) {
                st = DOWN2;
                t_down = t;
            } else if (t - t_up > CONFIG_EREADER_BTN_DOUBLE_MS) {
                send(BTN_NEXT);
                st = IDLE;
            }
            break;
        case DOWN2:
            if (m.stable && t - t_down >= CONFIG_EREADER_BTN_POWER_MS) {
                send(BTN_POWER);
                st = IGNORE;
            } else if (m.changed && !m.stable) {
                send(BTN_PREV);
                st = IDLE;
            }
            break;
        case IGNORE:
            if (!m.stable) st = IDLE;
            break;
        }
    }
}

static void config_input(int pin)
{
    if (pin < 0) return;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_EREADER_BTN_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
#else
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
    };
    gpio_config(&io);
}

void button_init(void)
{
    config_input(PIN_MAIN);
    config_input(PIN_NEXT);
    config_input(PIN_PREV);
    s_q = xQueueCreate(16, 1);
    xTaskCreate(button_task, "button", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "main=%d next=%d prev=%d (%s)", PIN_MAIN, PIN_NEXT, PIN_PREV,
             button_has_nav() ? "3-button mode" : "1-button gestures");
}

btn_event_t button_wait(int timeout_ms)
{
    uint8_t v;
    TickType_t to = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_q, &v, to) == pdTRUE) return (btn_event_t)v;
    return BTN_NONE;
}

void button_flush(void) { xQueueReset(s_q); }

void button_wait_release(void)
{
    while (button_pressed()) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---------------- button test ---------------- */

/* GPIOs that may be free for buttons: skips flash/PSRAM (26-37), USB (19/20),
 * console UART (43/44) and the display pins. */
static const int PROBE_CANDIDATES[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                       16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47, 48};
#define NCAND (sizeof(PROBE_CANDIDATES) / sizeof(PROBE_CANDIDATES[0]))
static bool s_probe_on[NCAND], s_probe_base[NCAND], s_probe_last[NCAND], s_probe_own[NCAND];

static bool used_by_display(int p)
{
    return p == CONFIG_EREADER_PIN_MOSI || p == CONFIG_EREADER_PIN_SCLK || p == CONFIG_EREADER_PIN_CS ||
           p == CONFIG_EREADER_PIN_DC || p == CONFIG_EREADER_PIN_RST || p == CONFIG_EREADER_PIN_BL;
}

void button_probe_begin(void)
{
    for (size_t i = 0; i < NCAND; i++) {
        int p = PROBE_CANDIDATES[i];
        s_probe_on[i] = !used_by_display(p);
        s_probe_own[i] = false;
        if (!s_probe_on[i]) continue;
        if (p != PIN_MAIN && p != PIN_NEXT && p != PIN_PREV) {
            gpio_config_t io = {.pin_bit_mask = 1ULL << p, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
            gpio_config(&io);
            s_probe_own[i] = true;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    for (size_t i = 0; i < NCAND; i++) {
        if (!s_probe_on[i]) continue;
        s_probe_base[i] = gpio_get_level(PROBE_CANDIDATES[i]);
        s_probe_last[i] = false;
    }
    ESP_LOGI(TAG, "button test started: press each button");
}

int button_probe_read(int *pins, int max)
{
    int n = 0;
    for (size_t i = 0; i < NCAND; i++) {
        if (!s_probe_on[i]) continue;
        int p = PROBE_CANDIDATES[i];
        bool active = gpio_get_level(p) != s_probe_base[i];
        if (active != s_probe_last[i]) {
            ESP_LOGI(TAG, "GPIO %d %s", p, active ? "pressed" : "released");
            s_probe_last[i] = active;
        }
        if (active && n < max) pins[n++] = p;
    }
    return n;
}

void button_probe_end(void)
{
    for (size_t i = 0; i < NCAND; i++)
        if (s_probe_own[i]) gpio_reset_pin(PROBE_CANDIDATES[i]);
    ESP_LOGI(TAG, "button test finished");
}
