#include "html.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

enum { S_TEXT, S_TAG, S_COMMENT, S_ENTITY };

static const struct { const char *n; uint16_t cp; } ENTITIES[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0xA0}, {"shy", 0xAD}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"laquo", 0xAB},
    {"raquo", 0xBB}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"copy", 0xA9},
    {"reg", 0xAE}, {"trade", 0x2122}, {"deg", 0xB0}, {"middot", 0xB7},
    {"bull", 0x2022}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"sect", 0xA7},
    {"para", 0xB6}, {"times", 0xD7}, {"divide", 0xF7}, {"euro", 0x20AC},
    {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"iexcl", 0xA1},
    {"iquest", 0xBF}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE},
    {"eacute", 0xE9}, {"egrave", 0xE8}, {"ecirc", 0xEA}, {"euml", 0xEB},
    {"aacute", 0xE1}, {"agrave", 0xE0}, {"acirc", 0xE2}, {"auml", 0xE4},
    {"atilde", 0xE3}, {"aring", 0xE5}, {"ccedil", 0xE7}, {"iacute", 0xED},
    {"igrave", 0xEC}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"ntilde", 0xF1},
    {"oacute", 0xF3}, {"ograve", 0xF2}, {"ocirc", 0xF4}, {"ouml", 0xF6},
    {"otilde", 0xF5}, {"oslash", 0xF8}, {"uacute", 0xFA}, {"ugrave", 0xF9},
    {"ucirc", 0xFB}, {"uuml", 0xFC}, {"szlig", 0xDF}, {"aelig", 0xE6},
    {"oelig", 0x153}, {"Eacute", 0xC9}, {"Agrave", 0xC0}, {"Ccedil", 0xC7},
    {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"prime", 0x2032},
    {"minus", 0x2212}, {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003},
    {"zwnj", 0x200C}, {"zwj", 0x200D}, {"larr", 0x2190}, {"rarr", 0x2192},
};

uint32_t html_entity(const char *name)
{
    if (name[0] == '#') {
        long v = (name[1] == 'x' || name[1] == 'X') ? strtol(name + 2, NULL, 16) : strtol(name + 1, NULL, 10);
        if (v <= 0 || v > 0x10FFFF) return 0xFFFD;
        /* windows-1252 range used by broken encoders */
        if (v >= 0x80 && v <= 0x9F) {
            static const uint16_t w[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                                           0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
            v = w[v - 0x80];
        }
        return (uint32_t)v;
    }
    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
        if (strcmp(ENTITIES[i].n, name) == 0) return ENTITIES[i].cp;
    }
    return 0;
}

void html_unescape(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '&') {
            char *semi = strchr(s, ';');
            if (semi && semi - s < 12) {
                char name[12];
                memcpy(name, s + 1, semi - s - 1);
                name[semi - s - 1] = 0;
                uint32_t cp = html_entity(name);
                if (cp) {
                    w += utf8_encode(cp, w);
                    s = semi + 1;
                    continue;
                }
            }
        }
        *w++ = *s++;
    }
    *w = 0;
}

void html_init(html_t *h, textout_t *out)
{
    memset(h, 0, sizeof(*h));
    h->out = out;
}

static bool is_block(const char *n)
{
    static const char *const B[] = {
        "p", "div", "br", "li", "ul", "ol", "dl", "dt", "dd", "tr", "table", "blockquote",
        "section", "article", "header", "footer", "figure", "figcaption", "pre", "aside",
        "nav", "body", "center", "address", "main", "h4", "h5", "h6", "caption", "tbody",
        "thead", "form", "fieldset", "legend", "details", "summary", "hgroup", "title"};
    for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); i++)
        if (strcmp(n, B[i]) == 0) return true;
    return false;
}

static void heading_add(html_t *h, uint32_t cp)
{
    char b[4];
    int n = utf8_encode(cp, b);
    if (cp <= ' ') {
        if (h->heading_len == 0 || h->heading_text[h->heading_len - 1] == ' ') return;
        b[0] = ' ';
        n = 1;
    }
    if (h->heading_len + n < (int)sizeof(h->heading_text)) {
        memcpy(h->heading_text + h->heading_len, b, n);
        h->heading_len += n;
        h->heading_text[h->heading_len] = 0;
    }
}

static void emit_cp(html_t *h, uint32_t cp)
{
    if (h->in_title) {
        if (h->doc_title_len < (int)sizeof(h->doc_title) - 4) {
            if (cp <= ' ') cp = ' ';
            h->doc_title_len += utf8_encode(cp, h->doc_title + h->doc_title_len);
            h->doc_title[h->doc_title_len] = 0;
        }
        return;
    }
    if (h->skip || h->in_head) return;
    if (h->heading) heading_add(h, cp);
    tout_cp(h->out, cp);
}

static void emit_byte(html_t *h, uint8_t c)
{
    if (h->in_title) {
        if (h->doc_title_len < (int)sizeof(h->doc_title) - 1) {
            h->doc_title[h->doc_title_len++] = (char)(c <= ' ' ? ' ' : c);
            h->doc_title[h->doc_title_len] = 0;
        }
        return;
    }
    if (h->skip || h->in_head) return;
    if (h->heading) {
        if (c < 0x80) heading_add(h, c);
        else if (h->heading_len + 1 < (int)sizeof(h->heading_text) - 4) {
            h->heading_text[h->heading_len++] = (char)c;
            h->heading_text[h->heading_len] = 0;
        }
    }
    char ch = (char)c;
    tout_text(h->out, &ch, 1);
}

static void process_tag(html_t *h)
{
    h->tag[h->taglen] = 0;
    const char *p = h->tag;
    bool closing = false;
    if (*p == '/') { closing = true; p++; }
    if (*p == '!' || *p == '?') return; /* doctype, xml decl */
    char name[16];
    int n = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == ':' || *p == '-') && n < 15) {
        name[n++] = (char)tolower((unsigned char)*p);
        p++;
    }
    name[n] = 0;
    char *colon = strchr(name, ':');
    const char *nm = colon ? colon + 1 : name;
    bool selfclose = h->taglen > 0 && h->tag[h->taglen - 1] == '/';

    if (!strcmp(nm, "script") || !strcmp(nm, "style")) {
        if (!selfclose) h->skip += closing ? -1 : 1;
        if (h->skip < 0) h->skip = 0;
        return;
    }
    if (!strcmp(nm, "head")) { h->in_head = !closing; return; }
    if (!strcmp(nm, "title")) {
        if (h->in_head || !h->in_body_seen) { h->in_title = !closing && !selfclose; return; }
    }
    if (!strcmp(nm, "body")) { h->in_head = false; h->in_body_seen = true; }

    if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '3' && nm[2] == 0) {
        tout_para(h->out);
        if (!closing) {
            h->heading = !h->heading_done; /* only the first heading names the chapter */
        } else {
            h->heading = 0;
            if (h->heading_len) h->heading_done = true;
        }
        return;
    }
    if (!strcmp(nm, "hr")) {
        tout_blank(h->out);
        tout_text(h->out, "* * *", 5);
        tout_blank(h->out);
        return;
    }
    if (!strcmp(nm, "li") && !closing) {
        tout_para(h->out);
        tout_cp(h->out, 0x2022);
        tout_space(h->out);
        return;
    }
    if (!strcmp(nm, "td") || !strcmp(nm, "th")) {
        tout_space(h->out);
        return;
    }
    if (!strcmp(nm, "img") && !h->skip && !h->in_head) {
        return;
    }
    if (is_block(nm)) {
        tout_para(h->out);
        if (h->heading) heading_add(h, ' ');
    }
}

void html_feed(html_t *h, const uint8_t *d, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = d[i];
        switch (h->state) {
        case S_TEXT:
            if (c == '<') { h->state = S_TAG; h->taglen = 0; h->quote = 0; }
            else if (c == '&') { h->state = S_ENTITY; h->entlen = 0; }
            else emit_byte(h, c);
            break;
        case S_TAG:
            if (h->quote) {
                if (c == h->quote) h->quote = 0;
            } else if (c == '"' || c == '\'') {
                if (h->taglen > 0) h->quote = (char)c;
            } else if (c == '>') {
                h->state = S_TEXT;
                process_tag(h);
                break;
            }
            if (h->taglen < (int)sizeof(h->tag) - 1) h->tag[h->taglen++] = (char)c;
            if (h->taglen == 3 && memcmp(h->tag, "!--", 3) == 0) {
                h->state = S_COMMENT;
                h->dashes = 0;
            }
            break;
        case S_COMMENT:
            if (c == '-') h->dashes++;
            else if (c == '>' && h->dashes >= 2) h->state = S_TEXT;
            else h->dashes = 0;
            break;
        case S_ENTITY:
            if (c == ';' || h->entlen >= (int)sizeof(h->ent) - 1 ||
                !(isalnum(c) || c == '#')) {
                h->ent[h->entlen] = 0;
                uint32_t cp = (c == ';') ? html_entity(h->ent) : 0;
                h->state = S_TEXT;
                if (cp) {
                    if (cp != 0xAD) emit_cp(h, cp);
                } else {
                    emit_byte(h, '&');
                    for (int k = 0; k < h->entlen; k++) emit_byte(h, (uint8_t)h->ent[k]);
                    if (c == '<') { h->state = S_TAG; h->taglen = 0; h->quote = 0; }
                    else emit_byte(h, c);
                }
            } else {
                h->ent[h->entlen++] = (char)c;
            }
            break;
        }
    }
}

void html_finish(html_t *h)
{
    tout_para(h->out);
}

int html_out_cb(void *ctx, const uint8_t *data, size_t len)
{
    html_feed((html_t *)ctx, data, len);
    return 0;
}
