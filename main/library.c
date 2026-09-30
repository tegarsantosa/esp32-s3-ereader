#include "library.h"
#include "convert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "library";
static wl_handle_t s_wl = WL_INVALID_HANDLE;

int storage_init(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BOOKS_DIR, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return -1;
    }
    mkdir(CACHE_DIR, 0775);
    uint64_t total = 0, fr = 0;
    storage_info(&total, &fr);
    ESP_LOGI(TAG, "storage: %llu KB total, %llu KB free", total / 1024, fr / 1024);
    return 0;
}

bool storage_info(uint64_t *total, uint64_t *free_bytes)
{
    return esp_vfs_fat_info(BOOKS_DIR, total, free_bytes) == ESP_OK;
}

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

void library_paths(const char *name, char *src, char *txt, char *toc, char *pg, size_t sz)
{
    uint32_t h = name_hash(name);
    if (src) snprintf(src, sz, BOOKS_DIR "/%s", name);
    if (txt) snprintf(txt, sz, CACHE_DIR "/%08lx.txt", (unsigned long)h);
    if (toc) snprintf(toc, sz, CACHE_DIR "/%08lx.toc", (unsigned long)h);
    if (pg) snprintf(pg, sz, CACHE_DIR "/%08lx.pg", (unsigned long)h);
}

static void progress_key(const char *name, char *key)
{
    snprintf(key, 16, "p%08lx", (unsigned long)name_hash(name));
}

typedef struct {
    uint32_t offset;
    int16_t pct;
    uint16_t _r;
} prog_t;

bool progress_get(const char *name, uint32_t *offset, int *pct)
{
    nvs_handle_t h;
    char key[16];
    progress_key(name, key);
    if (nvs_open("progress", NVS_READONLY, &h) != ESP_OK) return false;
    prog_t p;
    size_t len = sizeof(p);
    bool ok = nvs_get_blob(h, key, &p, &len) == ESP_OK && len == sizeof(p);
    nvs_close(h);
    if (!ok) return false;
    if (offset) *offset = p.offset;
    if (pct) *pct = p.pct;
    return true;
}

void progress_set(const char *name, uint32_t offset, int pct)
{
    nvs_handle_t h;
    char key[16];
    progress_key(name, key);
    if (nvs_open("progress", NVS_READWRITE, &h) != ESP_OK) return;
    prog_t p = {offset, (int16_t)pct, 0};
    nvs_set_blob(h, key, &p, sizeof(p));
    nvs_commit(h);
    nvs_close(h);
}

void library_forget(const char *name)
{
    char txt[160], toc[160], pg[160];
    library_paths(name, NULL, txt, toc, pg, sizeof(txt));
    remove(txt);
    remove(toc);
    remove(pg);
    nvs_handle_t h;
    char key[16];
    progress_key(name, key);
    if (nvs_open("progress", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
}

void library_clear_cache(void)
{
    DIR *d = opendir(CACHE_DIR);
    if (!d) return;
    struct dirent *e;
    char path[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), CACHE_DIR "/%s", e->d_name);
        remove(path);
    }
    closedir(d);
}

bool library_name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > MAX_NAME || n[0] == '.') return false;
    for (const char *p = n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) return false;
    }
    return convert_supported(n);
}

void library_display_name(const char *name, char *out, size_t sz)
{
    snprintf(out, sz, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

static int cmp_entry(const void *a, const void *b)
{
    return strcasecmp(((const lib_entry_t *)a)->name, ((const lib_entry_t *)b)->name);
}

int library_scan(lib_entry_t **list)
{
    *list = NULL;
    DIR *d = opendir(BOOKS_DIR);
    if (!d) return 0;
    int n = 0, cap = 0;
    lib_entry_t *arr = NULL;
    struct dirent *e;
    char path[300];
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR || !library_name_ok(e->d_name)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            lib_entry_t *na = realloc(arr, cap * sizeof(lib_entry_t));
            if (!na) break;
            arr = na;
        }
        lib_entry_t *x = &arr[n++];
        snprintf(x->name, sizeof(x->name), "%s", e->d_name);
        snprintf(path, sizeof(path), BOOKS_DIR "/%s", e->d_name);
        struct stat st;
        x->size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
        x->pct = -1;
        progress_get(x->name, NULL, &x->pct);
    }
    closedir(d);
    if (n > 1) qsort(arr, n, sizeof(lib_entry_t), cmp_entry);
    *list = arr;
    return n;
}

void library_free(lib_entry_t *list) { free(list); }
