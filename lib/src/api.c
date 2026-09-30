#include "d99_api.h"
#include "d99_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void d99_jb_init(d99_json_buf *b)
{
    b->cap = 256;
    b->len = 0;
    b->data = d99_xmalloc(b->cap);
    b->data[0] = '\0';
}

void d99_jb_free(d99_json_buf *b)
{
    if (b->data) {
        free(b->data);
        b->data = NULL;
    }
    b->len = 0;
    b->cap = 0;
}

char *d99_jb_finish(d99_json_buf *b)
{
    char *res = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return res;
}

static void jb_ensure(d99_json_buf *b, size_t needed)
{
    if (b->len + needed + 1 >= b->cap) {
        b->cap = (b->len + needed + 1) * 2;
        b->data = d99_xrealloc(b->data, b->cap);
    }
}

void d99_jb_raw(d99_json_buf *b, const char *s)
{
    if (!s) return;
    size_t slen = strlen(s);
    jb_ensure(b, slen);
    memcpy(b->data + b->len, s, slen);
    b->len += slen;
    b->data[b->len] = '\0';
}

void d99_jb_str(d99_json_buf *b, const char *s)
{
    if (!s) {
        d99_jb_raw(b, "null");
        return;
    }
    d99_jb_raw(b, "\"");
    const char *p;
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  d99_jb_raw(b, "\\\""); break;
        case '\\': d99_jb_raw(b, "\\\\"); break;
        case '\b': d99_jb_raw(b, "\\b"); break;
        case '\f': d99_jb_raw(b, "\\f"); break;
        case '\n': d99_jb_raw(b, "\\n"); break;
        case '\r': d99_jb_raw(b, "\\r"); break;
        case '\t': d99_jb_raw(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                d99_jb_raw(b, buf);
            } else {
                char ch[2] = { (char)c, '\0' };
                d99_jb_raw(b, ch);
            }
            break;
        }
    }
    d99_jb_raw(b, "\"");
}

void d99_jb_int(d99_json_buf *b, long long n)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", n);
    d99_jb_raw(b, buf);
}

void d99_jb_bool(d99_json_buf *b, int val)
{
    d99_jb_raw(b, val ? "true" : "false");
}

void d99_jb_key_str(d99_json_buf *b, const char *key, const char *val)
{
    d99_jb_str(b, key);
    d99_jb_raw(b, ":");
    d99_jb_str(b, val);
}

void d99_jb_key_int(d99_json_buf *b, const char *key, long long val)
{
    d99_jb_str(b, key);
    d99_jb_raw(b, ":");
    d99_jb_int(b, val);
}

void d99_jb_key_bool(d99_json_buf *b, const char *key, int val)
{
    d99_jb_str(b, key);
    d99_jb_raw(b, ":");
    d99_jb_bool(b, val);
}

char *d99_json_escape(const char *str)
{
    d99_json_buf b;
    d99_jb_init(&b);
    d99_jb_str(&b, str);
    return d99_jb_finish(&b);
}
