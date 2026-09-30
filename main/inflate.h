#pragma once
#include <stdint.h>
#include <stddef.h>

/* Streaming raw DEFLATE (RFC 1951) decoder with a 32 KB window.
 * in()  returns the next input byte (0..255) or -1 at end of input.
 * out() receives decompressed data; return non-zero to stop early. */
typedef int (*inflate_in_fn)(void *ctx);
typedef int (*inflate_out_fn)(void *ctx, const uint8_t *data, size_t len);

/* returns 0 = ok, 1 = stopped by out(), <0 = corrupt / truncated data */
int inflate_stream(inflate_in_fn in, void *in_ctx, inflate_out_fn out, void *out_ctx);
