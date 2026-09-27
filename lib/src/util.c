#include "d99_util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

/* ================= messages ================= */
static int g_verbose = 0;

void d99_set_verbose(int on) { g_verbose = on; }
int  d99_verbose_enabled(void) { return g_verbose; }

static void vmsg(FILE *out, const char *fmt, va_list ap)
{
    fputs("d99: ", out);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
}

void d99_verbose(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose)
        return;
    va_start(ap, fmt);
    vmsg(stderr, fmt, ap);
    va_end(ap);
}

void d99_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stderr, fmt, ap);
    va_end(ap);
}

void d99_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

/* ================= allocation ================= */
static void *xcheck(void *p, size_t n)
{
    if (!p) {
        fprintf(stderr, "d99: out of memory (%llu bytes)\n", (unsigned long long)n);
        exit(1);
    }
    return p;
}

void *d99_xmalloc(size_t n)
{
    return xcheck(malloc(n ? n : 1), n);
}

void *d99_xcalloc(size_t n, size_t sz)
{
    return xcheck(calloc(n ? n : 1, sz ? sz : 1), n);
}

void *d99_xrealloc(void *p, size_t n)
{
    return xcheck(realloc(p, n ? n : 1), n);
}

char *d99_xstrdup(const char *s)
{
    size_t n;
    if (!s)
        s = "";
    n = strlen(s) + 1;
    return xcheck(memcpy(d99_xmalloc(n), s, n), n);
}

char *d99_xstrndup(const char *s, size_t n)
{
    char *r = d99_xmalloc(n + 1);
    if (n)
        memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

char *d99_xvasprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    char *buf;

    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        fprintf(stderr, "d99: vsnprintf failed\n");
        exit(1);
    }
    buf = d99_xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    return buf;
}

char *d99_xasprintf(const char *fmt, ...)
{
    va_list ap;
    char *r;
    va_start(ap, fmt);
    r = d99_xvasprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* ================= arena ================= */
struct d99_block {
    struct d99_block *next;
    size_t used, cap;
    /* data follows */
};

struct d99_arena {
    struct d99_block *head;
};

d99_arena *d99_arena_new(void)
{
    d99_arena *a = d99_xcalloc(1, sizeof(*a));
    return a;
}

void *d99_arena_alloc(d99_arena *a, size_t n)
{
    struct d99_block *b;
    char *p;

    if (!a)
        return d99_xmalloc(n);
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 4096 ? n : 4096;
        b = d99_xmalloc(sizeof(struct d99_block) + cap);
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    b = a->head;
    p = (char *)(b + 1) + b->used;
    b->used += n;
    return p;
}

char *d99_arena_strdup(d99_arena *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = d99_arena_alloc(a, n);
    memcpy(p, s, n);
    return p;
}

void d99_arena_free(d99_arena *a)
{
    struct d99_block *b, *next;
    if (!a)
        return;
    for (b = a->head; b; b = next) {
        next = b->next;
        free(b);
    }
    free(a);
}

/* ================= string vector ================= */
void d99_sv_init(d99_strvec *sv)
{
    sv->v = NULL;
    sv->n = sv->cap = 0;
}

void d99_sv_push_own(d99_strvec *sv, char *s)
{
    if (sv->n == sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 8;
        sv->v = d99_xrealloc(sv->v, sv->cap * sizeof(char *));
    }
    sv->v[sv->n++] = s;
}

void d99_sv_push(d99_strvec *sv, const char *s)
{
    d99_sv_push_own(sv, d99_xstrdup(s));
}

int d99_sv_contains(const d99_strvec *sv, const char *s)
{
    size_t i;
    if (!sv)
        return 0;
    for (i = 0; i < sv->n; i++)
        if (sv->v[i] && strcmp(sv->v[i], s) == 0)
            return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void d99_sv_sort(d99_strvec *sv)
{
    if (sv->n > 1)
        qsort(sv->v, sv->n, sizeof(char *), cmp_str);
}

void d99_sv_free(d99_strvec *sv)
{
    size_t i;
    for (i = 0; i < sv->n; i++)
        free(sv->v[i]);
    free(sv->v);
    d99_sv_init(sv);
}

/* ================= strings ================= */
int d99_streq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

int d99_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int d99_strcasestr_match(const char *haystack, const char *needle_lower, size_t needle_len)
{
    const char *h = haystack;
    unsigned char first, f_lower, f_upper;
    if (!h || !needle_lower || needle_len == 0)
        return 0;
    first = (unsigned char)needle_lower[0];
    f_lower = first;
    f_upper = (unsigned char)toupper(first);

#if defined(__SSE2__)
    __m128i vl = _mm_set1_epi8((char)f_lower);
    __m128i vu = _mm_set1_epi8((char)f_upper);
    __m128i vz = _mm_setzero_si128();

    while (((uintptr_t)h & 15) != 0) {
        if (*h == '\0')
            return 0;
        if ((unsigned char)*h == f_lower || (unsigned char)*h == f_upper) {
            if (strncasecmp(h, needle_lower, needle_len) == 0)
                return 1;
        }
        h++;
    }

    for (;;) {
        __m128i blk = _mm_load_si128((const __m128i *)h);
        __m128i eq_l = _mm_cmpeq_epi8(blk, vl);
        __m128i eq_u = _mm_cmpeq_epi8(blk, vu);
        __m128i eq_z = _mm_cmpeq_epi8(blk, vz);
        int hit_mask = _mm_movemask_epi8(_mm_or_si128(eq_l, eq_u));
        int zero_mask = _mm_movemask_epi8(eq_z);

        if (hit_mask != 0) {
            int pos;
            for (pos = 0; pos < 16; pos++) {
                if (zero_mask & (1 << pos))
                    return 0;
                if (hit_mask & (1 << pos)) {
                    if (strncasecmp(h + pos, needle_lower, needle_len) == 0)
                        return 1;
                }
            }
        }
        if (zero_mask != 0)
            return 0;
        h += 16;
    }
#else
    while (*h) {
        if ((unsigned char)*h == f_lower || (unsigned char)*h == f_upper) {
            if (strncasecmp(h, needle_lower, needle_len) == 0)
                return 1;
        }
        h++;
    }
    return 0;
#endif
}

char *d99_trim(char *s)
{
    size_t n;
    if (!s)
        return s;
    while (*s && isspace((unsigned char)*s))
        s++;
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

void d99_strip_suffix(char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (ls >= lx && strcmp(s + ls - lx, suffix) == 0)
        s[ls - lx] = '\0';
}

uint64_t d99_fnv1a64(const void *data, size_t n)
{
    const unsigned char *p = data;
    uint64_t h = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t d99_fnv1a64_str(const char *s)
{
    return d99_fnv1a64(s, strlen(s));
}

/* Iterative glob matcher supporting '*' and '?'. */
int d99_glob_match(const char *pat, const char *s)
{
    const char *p = pat, *str = s;
    const char *star_p = NULL, *star_s = NULL;

    while (*str) {
        if (*p == '*') {
            star_p = ++p;
            star_s = str;
        } else if (*p == '?' || *p == *str) {
            p++;
            str++;
        } else if (star_p) {
            p = star_p;
            str = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

/* ================= filesystem ================= */
char *d99_path_join(const char *a, const char *b)
{
    size_t la;
    int need;

    if (!a || !*a)
        return d99_xstrdup(b ? b : "");
    if (!b)
        return d99_xstrdup(a);
    if (*b == '/')
        return d99_xstrdup(b);
    la = strlen(a);
    need = (la > 0 && a[la - 1] != '/');
    return d99_xasprintf("%s%s%s", a, need ? "/" : "", b);
}

char *d99_dirname_dup(const char *path)
{
    char *tmp = d99_xstrdup(path);
    size_t n = strlen(tmp);
    char *slash;

    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';
    slash = strrchr(tmp, '/');
    if (!slash) {
        free(tmp);
        return d99_xstrdup(".");
    }
    if (slash == tmp) {
        free(tmp);
        return d99_xstrdup("/");
    }
    *slash = '\0';
    return tmp;
}

int d99_mkdir_p(const char *path, int mode)
{
    char *tmp = d99_xstrdup(path);
    size_t n = strlen(tmp);
    size_t i;
    struct stat st;

    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) {
                int e = errno;
                free(tmp);
                errno = e;
                return -1;
            }
            tmp[i] = saved;
        }
    }
    free(tmp);
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int d99_ensure_parent(const char *path, int mode)
{
    char *d = d99_dirname_dup(path);
    int r = d99_mkdir_p(d, mode);
    free(d);
    return r;
}

char *d99_read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    size_t cap, total = 0;
    char *buf;

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    cap = (S_ISREG(st.st_mode) && st.st_size > 0) ? (size_t)st.st_size : 65536;
    buf = d99_xmalloc(cap + 1);
    for (;;) {
        ssize_t r;
        if (total + 1 > cap) {
            cap = cap * 2 + 65536;
            buf = d99_xrealloc(buf, cap + 1);
        }
        r = read(fd, buf + total, cap - total);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    close(fd);
    buf[total] = '\0';
    if (len)
        *len = total;
    return buf;
}

int d99_write_file_atomic(const char *path, const void *buf, size_t len)
{
    char *tmpl = d99_xasprintf("%s.d99tmpXXXXXX", path);
    int fd = mkstemp(tmpl);
    const char *p = buf;
    size_t left = len;
    int fail = 0;

    if (fd < 0) {
        free(tmpl);
        return -1;
    }
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            fail = 1;
            break;
        }
        p += w;
        left -= (size_t)w;
    }
    if (!fail && fsync(fd) != 0)
        fail = 1;
    if (!fail && fchmod(fd, 0644) != 0)
        fail = 1;
    if (close(fd) != 0)
        fail = 1;
    if (fail || rename(tmpl, path) != 0) {
        unlink(tmpl);
        free(tmpl);
        return -1;
    }
    free(tmpl);
    /* fsync the containing directory so the rename is durable */
    {
        char *d = d99_dirname_dup(path);
        int dfd = open(d, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
        free(d);
    }
    return 0;
}

int d99_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int d99_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

const char *d99_host_arch(void)
{
    static char buf[64];
    static int done = 0;
    const char *env;
    FILE *f;
    struct utsname u;

    if (done)
        return buf;
    done = 1;
    buf[0] = '\0';

    env = getenv("D99_ARCH");
    if (env && *env) {
        snprintf(buf, sizeof buf, "%s", env);
        return buf;
    }
    /* /var/lib/dpkg/arch lists the primary architecture first */
    f = fopen("/var/lib/dpkg/arch", "r");
    if (f) {
        if (fgets(buf, sizeof buf, f)) {
            char *t = d99_trim(buf);
            if (*t) {
                fclose(f);
                return t;
            }
        }
        fclose(f);
        buf[0] = '\0';
    }
    if (uname(&u) != 0) {
        snprintf(buf, sizeof buf, "unknown");
        return buf;
    }
    if (strcmp(u.machine, "x86_64") == 0)
        snprintf(buf, sizeof buf, "amd64");
    else if (strncmp(u.machine, "i386", 4) == 0 || strncmp(u.machine, "i486", 4) == 0 ||
             strncmp(u.machine, "i586", 4) == 0 || strncmp(u.machine, "i686", 4) == 0)
        snprintf(buf, sizeof buf, "i386");
    else if (strcmp(u.machine, "aarch64") == 0 || strcmp(u.machine, "arm64") == 0)
        snprintf(buf, sizeof buf, "arm64");
    else if (strcmp(u.machine, "armv7l") == 0 || strcmp(u.machine, "armv6l") == 0)
        snprintf(buf, sizeof buf, "armhf");
    else if (strcmp(u.machine, "ppc64le") == 0)
        snprintf(buf, sizeof buf, "ppc64el");
    else
        snprintf(buf, sizeof buf, "%.32s", u.machine);
    return buf;
}

/* ================= SHA-256 ================= */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t bufn;
} sha256_ctx;

static const uint32_t sha256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t ror32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    uint32_t w[64];
    uint32_t a, b, bb, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; bb = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_K[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & bb) ^ (b & bb);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = bb; bb = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += bb; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au; c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->len = 0;
    c->bufn = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const unsigned char *p = data;
    c->len += n;
    while (n) {
        size_t k = 64 - c->bufn;
        if (k > n)
            k = n;
        memcpy(c->buf + c->bufn, p, k);
        c->bufn += k;
        p += k;
        n -= k;
        if (c->bufn == 64) {
            sha256_block(c, c->buf);
            c->bufn = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80, z = 0;
    unsigned char lenb[8];
    int i;

    sha256_update(c, &pad, 1);
    while (c->bufn != 56)
        sha256_update(c, &z, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->h[i]);
    }
}

void d99_sha256(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = hexd[in[i] >> 4];
        out[2 * i + 1] = hexd[in[i] & 15];
    }
    out[2 * n] = '\0';
}

void d99_sha256_hex(const void *data, size_t len, char out[65])
{
    uint8_t d[32];
    d99_sha256(data, len, d);
    hex_encode(d, 32, out);
}

int d99_sha256_file(const char *path, char hex[65])
{
    int fd = open(path, O_RDONLY);
    unsigned char buf[65536];
    sha256_ctx c;
    uint8_t d[32];

    if (fd < 0)
        return -1;
    sha256_init(&c);
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        sha256_update(&c, buf, (size_t)r);
    }
    close(fd);
    sha256_final(&c, d);
    hex_encode(d, 32, hex);
    return 0;
}

/* ================= MD5 (for dpkg-compatible md5sums) ================= */
typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    unsigned char buf[64];
    size_t bufn;
} md5_ctx;

static const uint32_t md5_K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};
static const int md5_R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t rol32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void md5_block(md5_ctx *c, const unsigned char *p)
{
    uint32_t M[16], A, B, C, D;
    int i;

    for (i = 0; i < 16; i++)
        M[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
               ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    A = c->a; B = c->b; C = c->c; D = c->d;
    for (i = 0; i < 64; i++) {
        uint32_t F;
        int g;
        if (i < 16) {
            F = (B & C) | (~B & D);
            g = i;
        } else if (i < 32) {
            F = (D & B) | (~D & C);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            F = B ^ C ^ D;
            g = (3 * i + 5) % 16;
        } else {
            F = C ^ (B | ~D);
            g = (7 * i) % 16;
        }
        F = F + A + md5_K[i] + M[g];
        A = D; D = C; C = B;
        B = B + rol32(F, md5_R[i]);
    }
    c->a += A; c->b += B; c->c += C; c->d += D;
}

static void md5_update(md5_ctx *c, const void *data, size_t n)
{
    const unsigned char *p = data;
    c->len += n;
    while (n) {
        size_t k = 64 - c->bufn;
        if (k > n)
            k = n;
        memcpy(c->buf + c->bufn, p, k);
        c->bufn += k;
        p += k;
        n -= k;
        if (c->bufn == 64) {
            md5_block(c, c->buf);
            c->bufn = 0;
        }
    }
}

static void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80, z = 0;
    unsigned char lenb[8];
    int i;

    md5_update(c, &pad, 1);
    while (c->bufn != 56)
        md5_update(c, &z, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (unsigned char)(bits >> (8 * i));
    md5_update(c, lenb, 8);
    for (i = 0; i < 4; i++) {
        out[i]      = (uint8_t)(c->a >> (8 * i));
        out[4 + i]  = (uint8_t)(c->b >> (8 * i));
        out[8 + i]  = (uint8_t)(c->c >> (8 * i));
        out[12 + i] = (uint8_t)(c->d >> (8 * i));
    }
}

void d99_md5_hex(const void *data, size_t len, char out[33])
{
    md5_ctx c;
    uint8_t d[16];
    c.a = 0x67452301u; c.b = 0xefcdab89u; c.c = 0x98badcfeu; c.d = 0x10325476u;
    c.len = 0; c.bufn = 0;
    md5_update(&c, data, len);
    md5_final(&c, d);
    hex_encode(d, 16, out);
}

int d99_md5_file(const char *path, char hex[33])
{
    int fd = open(path, O_RDONLY);
    unsigned char buf[65536];
    md5_ctx c;
    uint8_t d[16];

    if (fd < 0)
        return -1;
    c.a = 0x67452301u; c.b = 0xefcdab89u; c.c = 0x98badcfeu; c.d = 0x10325476u;
    c.len = 0; c.bufn = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        md5_update(&c, buf, (size_t)r);
    }
    close(fd);
    md5_final(&c, d);
    hex_encode(d, 16, hex);
    return 0;
}
