#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BOOKS_DIR "/books"
#define CACHE_DIR "/books/.cache"
#define MAX_NAME 100

typedef struct {
    char name[MAX_NAME + 1];
    uint32_t size;
    int pct;           /* reading progress, -1 = never opened */
} lib_entry_t;

int storage_init(void);                          /* mounts /books */
bool storage_info(uint64_t *total, uint64_t *free_bytes);

int library_scan(lib_entry_t **list);            /* malloc'd, sorted; returns count */
void library_free(lib_entry_t *list);
void library_paths(const char *name, char *src, char *txt, char *toc, char *pg, size_t sz);
void library_forget(const char *name);           /* delete cache + progress */
void library_clear_cache(void);
bool library_name_ok(const char *name);
void library_display_name(const char *name, char *out, size_t sz);

bool progress_get(const char *name, uint32_t *offset, int *pct);
void progress_set(const char *name, uint32_t offset, int pct);
