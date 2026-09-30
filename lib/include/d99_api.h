#ifndef D99_API_H
#define D99_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dynamic JSON buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} d99_json_buf;

void  d99_jb_init(d99_json_buf *b);
void  d99_jb_free(d99_json_buf *b);
char *d99_jb_finish(d99_json_buf *b);
void  d99_jb_raw(d99_json_buf *b, const char *s);
void  d99_jb_str(d99_json_buf *b, const char *s);
void  d99_jb_int(d99_json_buf *b, long long n);
void  d99_jb_bool(d99_json_buf *b, int val);
void  d99_jb_key_str(d99_json_buf *b, const char *key, const char *val);
void  d99_jb_key_int(d99_json_buf *b, const char *key, long long val);
void  d99_jb_key_bool(d99_json_buf *b, const char *key, int val);

char *d99_json_escape(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* D99_API_H */
