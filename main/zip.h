#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "inflate.h"

typedef struct {
    FILE *f;
    uint8_t *cd;       /* central directory, loaded in RAM */
    uint32_t cd_size;
    uint32_t count;
} zip_t;

typedef struct {
    uint16_t method;
    uint32_t csize, usize, lho;
} zip_entry_t;

int zip_open(zip_t *z, const char *path);
void zip_close(zip_t *z);
/* exact match first, then case-insensitive; returns 0 when found */
int zip_find(zip_t *z, const char *name, zip_entry_t *e);
/* streams the uncompressed data to out(); returns 0 on success */
int zip_extract(zip_t *z, const zip_entry_t *e, inflate_out_fn out, void *ctx);
/* extracts into a malloc'd, NUL terminated buffer (NULL if larger than max) */
char *zip_extract_alloc(zip_t *z, const zip_entry_t *e, size_t max, size_t *len);
