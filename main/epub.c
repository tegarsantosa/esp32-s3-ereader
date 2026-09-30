/* EPUB 2/3 -> text: container.xml -> OPF (manifest + spine) -> XHTML chapters.
 * Chapter titles come from the NCX / nav table of contents, or the first
 * heading of each chapter. */
#include "convert.h"
#include "zip.h"
#include "html.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MAX_XML (2 * 1024 * 1024)

typedef struct {
    char *id, *path, *media, *props;
} item_t;

typedef struct {
    char *path, *label;
} navent_t;

typedef struct {
    item_t *items;
    int nitems, cap_items;
    char **spine;
    int nspine, cap_spine;
    navent_t *nav;
    int nnav, cap_nav;
    char ncx_id[64];
} book_t;

static void *grow(void *p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return p;
    int nc = *cap ? *cap * 2 : 32;
    while (nc < need) nc *= 2;
    void *np = realloc(p, nc * elem);
    if (np) *cap = nc;
    return np;
}

/* Finds the next tag. Returns pointer to '<' or NULL. Sets local name (after
 * any namespace prefix), whether it is a closing tag, and the end ('>'). */
static const char *next_tag(const char *p, char *name, size_t nsz, bool *closing, const char **end)
{
    for (;;) {
        p = strchr(p, '<');
        if (!p) return NULL;
        if (!strncmp(p, "<!--", 4)) {
            const char *e = strstr(p + 4, "-->");
            if (!e) return NULL;
            p = e + 3;
            continue;
        }
        const char *q = p + 1;
        *closing = false;
        if (*q == '/') { *closing = true; q++; }
        size_t n = 0;
        const char *local = q;
        while (*q && (isalnum((unsigned char)*q) || *q == ':' || *q == '-' || *q == '_')) {
            if (*q == ':') local = q + 1;
            q++;
        }
        n = (size_t)(q - local);
        if (n >= nsz) n = nsz - 1;
        memcpy(name, local, n);
        name[n] = 0;
        const char *e = strchr(q, '>');
        if (!e) return NULL;
        *end = e;
        return p;
    }
}

static bool attr(const char *tag, const char *end, const char *name, char *out, size_t sz)
{
    size_t nl = strlen(name);
    for (const char *p = tag + 1; p + nl < end; p++) {
        if (!isspace((unsigned char)p[-1]) || strncmp(p, name, nl) != 0) continue;
        const char *q = p + nl;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (*q != '=') continue;
        q++;
        while (q < end && isspace((unsigned char)*q)) q++;
        char quote = *q;
        if (quote != '"' && quote != '\'') continue;
        q++;
        const char *e = memchr(q, quote, end - q);
        if (!e) return false;
        size_t n = (size_t)(e - q);
        if (n >= sz) n = sz - 1;
        memcpy(out, q, n);
        out[n] = 0;
        html_unescape(out);
        return true;
    }
    return false;
}

/* text content right after a tag, entities decoded, whitespace collapsed */
static void inner_text(const char *p, char *out, size_t sz, const char *stop_tag)
{
    size_t j = 0;
    bool sp = false;
    bool intag = false;
    while (*p && j + 1 < sz) {
        if (*p == '<') {
            if (stop_tag && !strncmp(p, stop_tag, strlen(stop_tag))) break;
            if (!stop_tag) break;
            intag = true;
        } else if (*p == '>' && intag) {
            intag = false;
        } else if (!intag) {
            if (isspace((unsigned char)*p)) sp = j > 0;
            else {
                if (sp && j + 2 < sz) out[j++] = ' ';
                sp = false;
                out[j++] = *p;
            }
        }
        p++;
    }
    out[j] = 0;
    html_unescape(out);
}

static void percent_decode(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *w++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *w++ = *s;
        }
    }
    *w = 0;
}

/* joins dir + href, strips #fragment, percent-decodes and normalises ./.. */
static char *resolve(const char *dir, const char *href)
{
    size_t n = strlen(dir) + strlen(href) + 2;
    char *tmp = malloc(n);
    if (!tmp) return NULL;
    if (href[0] == '/') snprintf(tmp, n, "%s", href + 1);
    else snprintf(tmp, n, "%s%s", dir, href);
    char *hash = strchr(tmp, '#');
    if (hash) *hash = 0;
    char *qm = strchr(tmp, '?');
    if (qm) *qm = 0;
    percent_decode(tmp);
    /* normalise path segments */
    char *out = malloc(n);
    if (!out) { free(tmp); return NULL; }
    size_t o = 0;
    char *save = NULL;
    for (char *seg = strtok_r(tmp, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (!strcmp(seg, ".")) continue;
        if (!strcmp(seg, "..")) {
            if (o > 0) {
                o--;
                while (o > 0 && out[o - 1] != '/') o--;
            }
            continue;
        }
        size_t l = strlen(seg);
        memcpy(out + o, seg, l);
        o += l;
        out[o++] = '/';
    }
    if (o > 0) o--; /* drop trailing slash */
    out[o] = 0;
    free(tmp);
    return out;
}

static void dir_of(const char *path, char *dir, size_t sz)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path + 1) : 0;
    if (n >= sz) n = sz - 1;
    memcpy(dir, path, n);
    dir[n] = 0;
}

static void add_nav(book_t *b, char *path, const char *label)
{
    if (!path || !label[0]) { free(path); return; }
    for (int i = 0; i < b->nnav; i++) {
        if (!strcmp(b->nav[i].path, path)) { free(path); return; }
    }
    navent_t *nv = grow(b->nav, &b->cap_nav, b->nnav + 1, sizeof(navent_t));
    if (!nv) { free(path); return; }
    b->nav = nv;
    b->nav[b->nnav].path = path;
    b->nav[b->nnav].label = strdup(label);
    b->nnav++;
}

static void parse_opf(book_t *b, const char *opf, const char *opf_dir)
{
    const char *p = opf, *end;
    char name[32];
    bool closing;
    char id[128], href[512], media[96], props[96];
    while ((p = next_tag(p, name, sizeof(name), &closing, &end))) {
        if (!closing && !strcmp(name, "item")) {
            if (attr(p, end, "id", id, sizeof(id)) && attr(p, end, "href", href, sizeof(href))) {
                if (!attr(p, end, "media-type", media, sizeof(media))) media[0] = 0;
                if (!attr(p, end, "properties", props, sizeof(props))) props[0] = 0;
                item_t *it = grow(b->items, &b->cap_items, b->nitems + 1, sizeof(item_t));
                if (it) {
                    b->items = it;
                    item_t *x = &b->items[b->nitems++];
                    x->id = strdup(id);
                    x->path = resolve(opf_dir, href);
                    x->media = strdup(media);
                    x->props = strdup(props);
                }
            }
        } else if (!closing && !strcmp(name, "itemref")) {
            if (attr(p, end, "idref", id, sizeof(id))) {
                char **sp = grow(b->spine, &b->cap_spine, b->nspine + 1, sizeof(char *));
                if (sp) {
                    b->spine = sp;
                    b->spine[b->nspine++] = strdup(id);
                }
            }
        } else if (!closing && !strcmp(name, "spine")) {
            if (!attr(p, end, "toc", b->ncx_id, sizeof(b->ncx_id))) b->ncx_id[0] = 0;
        }
        p = end + 1;
    }
}

static void parse_ncx(book_t *b, const char *x, const char *dir)
{
    const char *p = x, *end;
    char name[32], label[160] = "", src[512];
    bool closing;
    while ((p = next_tag(p, name, sizeof(name), &closing, &end))) {
        if (!closing && !strcmp(name, "text")) {
            inner_text(end + 1, label, sizeof(label), NULL);
        } else if (!closing && !strcmp(name, "content")) {
            if (attr(p, end, "src", src, sizeof(src))) add_nav(b, resolve(dir, src), label);
            label[0] = 0;
        }
        p = end + 1;
    }
}

static void parse_nav(book_t *b, const char *x, const char *dir)
{
    const char *p = x, *end;
    char name[32], label[160], href[512], type[64];
    bool closing, in_toc = false;
    while ((p = next_tag(p, name, sizeof(name), &closing, &end))) {
        if (!strcmp(name, "nav")) {
            if (closing) {
                if (in_toc) break;
            } else {
                in_toc = attr(p, end, "epub:type", type, sizeof(type)) ? strstr(type, "toc") != NULL
                         : attr(p, end, "role", type, sizeof(type)) ? strstr(type, "toc") != NULL : false;
            }
        } else if (in_toc && !closing && !strcmp(name, "a")) {
            if (attr(p, end, "href", href, sizeof(href))) {
                inner_text(end + 1, label, sizeof(label), "</a");
                add_nav(b, resolve(dir, href), label);
            }
        }
        p = end + 1;
    }
}

static const char *nav_label(book_t *b, const char *path)
{
    for (int i = 0; i < b->nnav; i++)
        if (!strcmp(b->nav[i].path, path)) return b->nav[i].label;
    return NULL;
}

static item_t *find_item(book_t *b, const char *id)
{
    for (int i = 0; i < b->nitems; i++)
        if (b->items[i].id && !strcmp(b->items[i].id, id)) return &b->items[i];
    return NULL;
}

static void free_book(book_t *b)
{
    for (int i = 0; i < b->nitems; i++) {
        free(b->items[i].id); free(b->items[i].path);
        free(b->items[i].media); free(b->items[i].props);
    }
    for (int i = 0; i < b->nspine; i++) free(b->spine[i]);
    for (int i = 0; i < b->nnav; i++) { free(b->nav[i].path); free(b->nav[i].label); }
    free(b->items); free(b->spine); free(b->nav);
}

static char *load(zip_t *z, const char *path)
{
    zip_entry_t e;
    if (!path || zip_find(z, path, &e)) return NULL;
    return zip_extract_alloc(z, &e, MAX_XML, NULL);
}

int conv_epub(const char *src, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t esz)
{
    zip_t z;
    book_t b;
    memset(&b, 0, sizeof(b));
    if (zip_open(&z, src)) {
        snprintf(err, esz, "Not a valid EPUB (zip) file");
        return -1;
    }
    int ret = -1;
    char *container = load(&z, "META-INF/container.xml");
    char opf_path[256] = "";
    if (container) {
        const char *p = container, *end;
        char name[32];
        bool closing;
        while ((p = next_tag(p, name, sizeof(name), &closing, &end))) {
            if (!closing && !strcmp(name, "rootfile") && attr(p, end, "full-path", opf_path, sizeof(opf_path))) break;
            p = end + 1;
        }
        free(container);
    }
    if (!opf_path[0]) {
        snprintf(err, esz, "EPUB has no container.xml");
        goto done;
    }
    char *opf = load(&z, opf_path);
    if (!opf) {
        snprintf(err, esz, "Cannot read %s", opf_path);
        goto done;
    }
    char opf_dir[256];
    dir_of(opf_path, opf_dir, sizeof(opf_dir));
    parse_opf(&b, opf, opf_dir);
    free(opf);

    /* table of contents: NCX (EPUB2) then nav document (EPUB3) */
    for (int i = 0; i < b.nitems; i++) {
        item_t *it = &b.items[i];
        bool is_ncx = (b.ncx_id[0] && !strcmp(it->id, b.ncx_id)) || strstr(it->media, "dtbncx");
        bool is_nav = strstr(it->props, "nav") != NULL;
        if (!is_ncx && !is_nav) continue;
        char *x = load(&z, it->path);
        if (!x) continue;
        char dir[256];
        dir_of(it->path, dir, sizeof(dir));
        if (is_ncx) parse_ncx(&b, x, dir);
        else parse_nav(&b, x, dir);
        free(x);
    }

    html_t *h = malloc(sizeof(html_t));
    if (!h) { snprintf(err, esz, "Out of memory"); goto done; }
    int chapters = 0;
    for (int i = 0; i < b.nspine; i++) {
        if (cb) cb(arg, i * 100 / (b.nspine ? b.nspine : 1));
        item_t *it = find_item(&b, b.spine[i]);
        if (!it || !it->path) continue;
        if (it->media[0] && !strstr(it->media, "html") && !strstr(it->media, "xml")) continue;
        zip_entry_t e;
        if (zip_find(&z, it->path, &e)) continue;
        uint32_t start = tout_mark(out);
        html_init(h, out);
        zip_extract(&z, &e, html_out_cb, h);
        html_finish(h);
        if (out->off > start) {
            const char *label = nav_label(&b, it->path);
            if (!label && h->heading_len) label = h->heading_text;
            if (label) tout_toc(out, start, label);
            chapters++;
        }
    }
    free(h);
    if (out->off == 0) snprintf(err, esz, "No readable text in this EPUB");
    else ret = 0;
    (void)chapters;
done:
    free_book(&b);
    zip_close(&z);
    return ret;
}
