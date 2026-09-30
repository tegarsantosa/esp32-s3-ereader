#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "textout.h"

/* Streaming HTML/XHTML to plain text converter. */
typedef struct {
    textout_t *out;
    int state;
    char tag[160];
    int taglen;
    char quote;
    char ent[12];
    int entlen;
    int ent_ret_state;
    int dashes;          /* for comment end detection */
    int skip;            /* inside <script>/<style> */
    bool in_head, in_title, in_body_seen;
    int heading;         /* inside <h1>-<h3> */
    bool heading_done;
    char heading_text[96];
    int heading_len;
    char doc_title[96];
    int doc_title_len;
} html_t;

void html_init(html_t *h, textout_t *out);
void html_feed(html_t *h, const uint8_t *data, size_t len);
void html_finish(html_t *h);
int html_out_cb(void *ctx, const uint8_t *data, size_t len); /* inflate_out_fn adapter */

/* decodes a named or numeric entity body (without & and ;), 0 if unknown */
uint32_t html_entity(const char *name);
/* decodes entities in place (for XML attribute values) */
void html_unescape(char *s);
