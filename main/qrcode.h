#pragma once
#include <stdint.h>
#include <stdbool.h>

#define QR_MAX_SIZE 57 /* version 10 */

/* Encodes text as a QR code (byte mode, error correction level M,
 * version 1-10, up to ~210 bytes). Returns the side length in modules
 * (0 if the text is too long); qr_module() reads the result. */
int qr_encode(const char *text);
bool qr_module(int x, int y);
