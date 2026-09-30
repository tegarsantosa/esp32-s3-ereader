#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Button input. Events have the same meaning in both modes:
 *   BTN_NEXT    next page / next item          (tap, or the "down" button)
 *   BTN_PREV    previous page / previous item  (double tap, or the "up" button)
 *   BTN_SELECT  open menu / select             (hold+release, or press OK)
 *   BTN_BACK    go back                        (3-button mode: hold OK, release)
 *   BTN_POWER   sleep                          (hold the main button 3 s)
 * With a single button the gestures are tap / double tap / hold+release;
 * BTN_LONG_READY fires while holding to show that releasing will select. */
typedef enum {
    BTN_NONE = 0,
    BTN_NEXT,
    BTN_PREV,
    BTN_SELECT,
    BTN_BACK,         /* 3-button mode: hold OK, release */
    BTN_LONG_READY,
    BTN_HOLD_HINT,
    BTN_POWER,
} btn_event_t;

/* legacy names used by the single-button UI code */
#define BTN_CLICK BTN_NEXT
#define BTN_DOUBLE BTN_PREV
#define BTN_LONG BTN_SELECT

void button_init(void);
btn_event_t button_wait(int timeout_ms); /* BTN_NONE on timeout; -1 = forever */
void button_flush(void);
bool button_pressed(void);               /* any button held */
void button_wait_release(void);
bool button_has_nav(void);               /* up/down buttons fitted */
uint64_t button_wake_mask(void);         /* RTC GPIOs that can wake from sleep */

/* Button test: watches every free GPIO and reports the ones that change. */
void button_probe_begin(void);
int button_probe_read(int *pins, int max); /* GPIOs currently pressed */
void button_probe_end(void);
