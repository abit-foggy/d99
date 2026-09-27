#ifndef D99_UTIL_H
#define D99_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* ---- messages --------------------------------------------------------- */
void d99_set_verbose(int on);
int  d99_verbose_enabled(void);
void d99_verbose(const char *fmt, ...);
void d99_warn(const char *fmt, ...);
void d99_die(const char *fmt, ...);          /* exits with status 2 */

/* ---- allocation (abort on failure) ------------------------------------ */
void *d99_xmalloc(size_t n);
void *d99_xcalloc(size_t n, size_t sz);
void *d99_xrealloc(void *p, size_t n);
char *d99_xstrdup(const char *s);
char *d99_xstrndup(const char *s, size_t n);
char *d99_xasprintf(const char *fmt, ...);
char *d99_xvasprintf(const char *fmt, va_list ap);

/* ---- arena allocator --------------------------------------------------- */
typedef struct d99_arena d99_arena;
d99_arena *d99_arena_new(void);
void  *d99_arena_alloc(d99_arena *a, size_t n);
char  *d99_arena_strdup(d99_arena *a, const char *s);
void   d99_arena_free(d99_arena *a);

/* ---- growable string vector ------------------------------------------- */
typedef struct { char **v; size_t n, cap; } d99_strvec;
void d99_sv_init(d99_strvec *sv);
void d99_sv_push(d99_strvec *sv, const char *s);   /* copies */
void d99_sv_push_own(d99_strvec *sv, char *s);     /* takes ownership */
int  d99_sv_contains(const d99_strvec *sv, const char *s);
void d99_sv_sort(d99_strvec *sv);
void d99_sv_free(d99_strvec *sv);

/* ---- strings ----------------------------------------------------------- */
int      d99_streq(const char *a, const char *b);
int      d99_starts_with(const char *s, const char *prefix);
int      d99_strcasestr_match(const char *haystack, const char *needle_lower, size_t needle_len);
char    *d99_trim(char *s);                          /* in place */
void     d99_strip_suffix(char *s, const char *suffix);
uint64_t d99_fnv1a64(const void *data, size_t n);
uint64_t d99_fnv1a64_str(const char *s);
int      d99_glob_match(const char *pat, const char *s);   /* '*' and '?' */

/* ---- filesystem -------------------------------------------------------- */
char *d99_path_join(const char *a, const char *b);       /* malloc'd */
char *d99_dirname_dup(const char *path);                /* malloc'd */
int   d99_mkdir_p(const char *path, int mode);
int   d99_ensure_parent(const char *path, int mode);
char *d99_read_file(const char *path, size_t *len);     /* malloc'd, NUL-terminated */
int   d99_write_file_atomic(const char *path, const void *buf, size_t len);
int   d99_file_exists(const char *path);
int   d99_is_dir(const char *path);
const char *d99_host_arch(void);       /* Debian architecture name, static buf */

/* ---- digests ----------------------------------------------------------- */
void d99_sha256(const void *data, size_t len, uint8_t out[32]);
void d99_sha256_hex(const void *data, size_t len, char out[65]);
int  d99_sha256_file(const char *path, char hex[65]);
void d99_md5_hex(const void *data, size_t len, char out[33]);
int  d99_md5_file(const char *path, char hex[33]);

#endif /* D99_UTIL_H */
