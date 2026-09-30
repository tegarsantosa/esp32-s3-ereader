#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Low level SPI TFT driver. Pixels are RGB565, stored big-endian (byte
 * swapped) in the band buffers so they can be DMA'd straight to the panel. */

int display_init(void);                 /* 0 on success */
int display_width(void);                /* logical size for current rotation */
int display_height(void);
int display_native_width(void);
int display_native_height(void);
void display_set_rotation(int rot);     /* 0..3 = 0/90/180/270 degrees */
void display_set_backlight(int percent);/* 0..100 */
void display_sleep(bool sleep);
const char *display_name(void);

/* Band rendering (used by gfx.c). Two band buffers are double-buffered: one
 * is filled while the other is being transferred by DMA. */
uint16_t *display_band_buffer(int idx, int *rows);
void display_flush_band(int idx, int y0, int rows);
void display_wait_buffer(int idx);
void display_wait_all(void);
