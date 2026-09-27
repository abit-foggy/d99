/*
 * index.bin layout (little-endian):
 *
 *   Header (80 bytes):
 *     [0..5]   magic "D99IDX"
 *     [6..7]   u16 version (=1)
 *     [8]      u32 n_pkg
 *     [12]     u32 n_pkgslots   (power of two >= 2*n_pkg)
 *     [16]     u32 n_fileslots  (power of two >= 2*n_files)
 *     [20]     u32 pad
 *     [24]     u64 off_pkgslots
 *     [32]     u64 off_fileslots
 *     [40]     u64 off_pkgdata
 *     [48]     u64 status_mtime_sec
 *     [56]     u64 status_mtime_nsec
 *     [64]     u64 total_len
 *     [72..79] pad
 *
 *   Slot (16 bytes): u64 FNV-1a64 hash, u32 pkg index, u32 pad.
 *   Slot hash 0 marks an empty bucket.
 *
 *   Per-package record in pkgdata:
 *     u32 name_len, u32 stanza_len, u32 n_files, u32 files_len
 *     name bytes | stanza bytes | (u32 len | bytes | NUL) * n_files
 */
#include "d99_db.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define IDX_MAGIC   "D99IDX"
#define IDX_VERSION 1
#define HDR_SIZE    80
#define SLOT_SIZE   16

typedef struct {
    const char *name;
    const char *version;
    const char *arch;
    const char *stanza;
    const char *summary;
    int sel, state, flags;
    int status_parsed;
    size_t n_files;
    const char *file0;     /* first file string; records follow */
} idx_pkg;

struct d99_index {
    void *map;
    size_t map_len;
    uint32_t n_pkg, n_pkgslots, n_fileslots;
    uint64_t off_pkgslots, off_fileslots, off_pkgdata;
    idx_pkg *pkg;
};

/* ---- little-endian scalar access on the mmap ---- */
static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const unsigned char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

typedef struct {
    unsigned char *b;
    size_t n, cap;
} bybuf;

static void bput(bybuf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->b = d99_xrealloc(b->b, b->cap);
    }
    memcpy(b->b + b->n, d, n);
    b->n += n;
}

static void bput_u32(bybuf *b, uint32_t v)
{
    unsigned char t[4] = { (unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
                           (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff) };
    bput(b, t, 4);
}

static void bput_u64(bybuf *b, uint64_t v)
{
    unsigned char t[8];
    int i;
    for (i = 0; i < 8; i++)
        t[i] = (unsigned char)((v >> (8 * i)) & 0xff);
    bput(b, t, 8);
}

static uint32_t next_pow2(uint32_t v)
{
    uint32_t r = 16;
    while (r < v)
        r <<= 1;
    return r;
}

/* ============================ build ============================ */
int d99_index_build(const char *admindir, const char *index_path)
{
    d99_db *db = d99_db_load_status(admindir);
    d99_arena *ar = d99_arena_new();
    d99_strvec *files = NULL;
    size_t i, j, n_files_total = 0;
    bybuf data = { NULL, 0, 0 };
    bybuf out = { NULL, 0, 0 };
    uint32_t n_pkg, n_pkgslots, n_fileslots;
    uint64_t off_pkgslots, off_fileslots, off_pkgdata, total;
    uint32_t *pslot_h, *pslot_i, *fslot_h, *fslot_i;
    struct stat st;
    unsigned char hdr[HDR_SIZE];
    char *status_path;
    int fail = 0;

    if (!db)
        return -1;
    n_pkg = (uint32_t)d99_db_count(db);
    files = d99_xcalloc(n_pkg ? n_pkg : 1, sizeof(d99_strvec));
    for (i = 0; i < n_pkg; i++) {
        d99_pkg *p = d99_db_at(db, i);
        char *path = d99_xasprintf("%s/info/%s.list", admindir, p->name);
        char *text = d99_read_file(path, NULL);
        const char *q = text;

        d99_sv_init(&files[i]);
        free(path);
        if (!q)
            continue;
        while (*q) {
            const char *eol = strchr(q, '\n');
            size_t ln = eol ? (size_t)(eol - q) : strlen(q);
            if (ln && q[0] == '/')
                ln--, q++;   /* stored without leading '/' */
            if (ln) {
                char *one = d99_xstrndup(q, ln);
                d99_sv_push_own(&files[i], d99_trim(one));
            }
            if (!eol)
                break;
            q = eol + 1;
        }
        free(text);
        n_files_total += files[i].n;
    }

    /* ---- serialize pkgdata ---- */
    for (i = 0; i < (size_t)n_pkg; i++) {
        d99_pkg *p = d99_db_at(db, i);
        size_t stanza_len = strlen(p->raw);
        size_t name_len = strlen(p->name);
        size_t flen = 0;

        for (j = 0; j < files[i].n; j++)
            flen += 4 + strlen(files[i].v[j]) + 1;
        bput_u32(&data, (uint32_t)(name_len + 1));
        bput_u32(&data, (uint32_t)stanza_len);
        bput_u32(&data, (uint32_t)files[i].n);
        bput_u32(&data, (uint32_t)flen);
        /* name stored with its NUL terminator so mappers can strcmp it */
        bput(&data, p->name, name_len + 1);
        bput(&data, p->raw, stanza_len);
        for (j = 0; j < files[i].n; j++) {
            size_t l = strlen(files[i].v[j]);
            bput_u32(&data, (uint32_t)l);
            bput(&data, files[i].v[j], l + 1);
        }
    }

    n_pkgslots = next_pow2((uint32_t)(n_pkg * 2));
    n_fileslots = next_pow2((uint32_t)(n_files_total * 2));
    off_pkgslots = HDR_SIZE;
    off_fileslots = off_pkgslots + (uint64_t)n_pkgslots * SLOT_SIZE;
    off_pkgdata = off_fileslots + (uint64_t)n_fileslots * SLOT_SIZE;
    total = off_pkgdata + data.n + 16;

    pslot_h = d99_xcalloc(n_pkgslots, sizeof(uint32_t) * 2);
    pslot_i = pslot_h + n_pkgslots;
    fslot_h = d99_xcalloc(n_fileslots, sizeof(uint32_t) * 2);
    fslot_i = fslot_h + n_fileslots;

    for (i = 0; i < (size_t)n_pkg; i++) {
        d99_pkg *p = d99_db_at(db, i);
        uint64_t h = d99_fnv1a64_str(p->name);
        uint32_t s = (uint32_t)(h & (n_pkgslots - 1));
        while (pslot_h[s])
            s = (s + 1) & (n_pkgslots - 1);
        pslot_h[s] = (uint32_t)h | 1u;   /* nonzero marker keeps hash 0 free */
        pslot_i[s] = (uint32_t)i;
    }
    for (i = 0; i < (size_t)n_pkg; i++) {
        for (j = 0; j < files[i].n; j++) {
            uint64_t h = d99_fnv1a64_str(files[i].v[j]);
            uint32_t s = (uint32_t)(h & (n_fileslots - 1));
            while (fslot_h[s])
                s = (s + 1) & (n_fileslots - 1);
            fslot_h[s] = (uint32_t)h | 1u;
            fslot_i[s] = (uint32_t)i;
        }
    }

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, IDX_MAGIC, 6);
    hdr[6] = IDX_VERSION & 0xff;
    hdr[7] = (IDX_VERSION >> 8) & 0xff;
    {
        unsigned char *p = hdr;
        uint32_t v;
        p += 8;
        v = n_pkg;
        *p++ = (unsigned char)(v & 0xff); *p++ = (unsigned char)((v >> 8) & 0xff);
        *p++ = (unsigned char)((v >> 16) & 0xff); *p++ = (unsigned char)((v >> 24) & 0xff);
        v = n_pkgslots;
        *p++ = (unsigned char)(v & 0xff); *p++ = (unsigned char)((v >> 8) & 0xff);
        *p++ = (unsigned char)((v >> 16) & 0xff); *p++ = (unsigned char)((v >> 24) & 0xff);
        v = n_fileslots;
        *p++ = (unsigned char)(v & 0xff); *p++ = (unsigned char)((v >> 8) & 0xff);
        *p++ = (unsigned char)((v >> 16) & 0xff); *p++ = (unsigned char)((v >> 24) & 0xff);
        p += 4;  /* pad */
    }
    {
        status_path = d99_path_join(admindir, "status");
        if (stat(status_path, &st) != 0)
            memset(&st, 0, sizeof st);
        free(status_path);
    }
    /* splice the u64 fields into the header at fixed offsets */
    {
        unsigned char tmp[8];
        int k;
        uint64_t fields[5] = { off_pkgslots, off_fileslots, off_pkgdata,
                               (uint64_t)st.st_mtim.tv_sec,
                               (uint64_t)st.st_mtim.tv_nsec };
        for (k = 0; k < 5; k++) {
            uint64_t v = fields[k];
            int m;
            for (m = 0; m < 8; m++)
                tmp[m] = (unsigned char)((v >> (8 * m)) & 0xff);
            memcpy(hdr + 24 + 8 * k, tmp, 8);
        }
        {
            uint64_t v = total;
            int m;
            for (m = 0; m < 8; m++)
                tmp[m] = (unsigned char)((v >> (8 * m)) & 0xff);
            memcpy(hdr + 64, tmp, 8);
        }
    }

    bput(&out, hdr, HDR_SIZE);
    for (i = 0; i < (size_t)n_pkgslots; i++) {
        bput_u64(&out, pslot_h[i] ? (uint64_t)pslot_h[i] : 0);
        bput_u32(&out, pslot_h[i] ? pslot_i[i] : 0);
        bput_u32(&out, 0);
    }
    for (i = 0; i < (size_t)n_fileslots; i++) {
        bput_u64(&out, fslot_h[i] ? (uint64_t)fslot_h[i] : 0);
        bput_u32(&out, fslot_h[i] ? fslot_i[i] : 0);
        bput_u32(&out, 0);
    }
    bput(&out, data.b, data.n);
    {
        unsigned char pad[16] = {0};
        bput(&out, pad, sizeof(pad));
    }

    if (d99_ensure_parent(index_path, 0755) != 0 ||
        d99_write_file_atomic(index_path, out.b, out.n) != 0)
        fail = 1;

    for (i = 0; i < (size_t)n_pkg; i++)
        d99_sv_free(&files[i]);
    free(files);
    free(pslot_h);
    free(fslot_h);
    free(data.b);
    free(out.b);
    d99_arena_free(ar);
    d99_db_free(db);
    return fail ? -1 : 0;
}

/* ============================ open/parse ============================ */

/* Light non-mutating line scan inside a stanza blob. */
static const char *scan_field(const char *stanza, const char *field)
{
    size_t flen = strlen(field);
    const char *p = stanza;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > flen + 1 && strncmp(p, field, flen) == 0 && p[flen] == ':') {
            const char *v = p + flen + 1;
            if (*v == ' ')
                v++;
            return v;
        }
        /* skip continuation lines */
        p = eol ? eol + 1 : NULL;
        while (p && (*p == ' ' || *p == '\t')) {
            eol = strchr(p, '\n');
            p = eol ? eol + 1 : NULL;
        }
    }
    return NULL;
}

static char *dup_field_value(const char *stanza, const char *field)
{
    const char *v = scan_field(stanza, field);
    const char *e;
    size_t n;
    char *out;

    if (!v)
        return NULL;
    e = strchr(v, '\n');
    n = e ? (size_t)(e - v) : strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
        n--;
    out = d99_xmalloc(n + 1);
    memcpy(out, v, n);
    out[n] = '\0';
    return out;
}

d99_index *d99_index_open(const char *index_path, const char *status_path)
{
    int fd = open(index_path, O_RDONLY);
    struct stat st;
    void *map;
    const unsigned char *base;
    d99_index *ix;
    uint32_t i;
    uint64_t off, total;
    struct stat sst;

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(HDR_SIZE + SLOT_SIZE)) {
        close(fd);
        return NULL;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return NULL;
    base = map;

    if (memcmp(base, IDX_MAGIC, 6) != 0 ||
        (unsigned)base[6] + ((unsigned)base[7] << 8) != IDX_VERSION) {
        munmap(map, (size_t)st.st_size);
        return NULL;
    }
    total = rd_u64(base + 64);
    if ((size_t)st.st_size < (size_t)total) {
        munmap(map, (size_t)st.st_size);
        return NULL;
    }

    /* staleness check against the status file */
    if (status_path && stat(status_path, &sst) == 0) {
        if ((uint64_t)sst.st_mtim.tv_sec != rd_u64(base + 48) ||
            (uint64_t)sst.st_mtim.tv_nsec != rd_u64(base + 56)) {
            munmap(map, (size_t)st.st_size);
            return NULL;
        }
    }

    ix = d99_xcalloc(1, sizeof(*ix));
    ix->map = map;
    ix->map_len = (size_t)st.st_size;
    ix->n_pkg = rd_u32(base + 8);
    ix->n_pkgslots = rd_u32(base + 12);
    ix->n_fileslots = rd_u32(base + 16);
    ix->off_pkgslots = rd_u64(base + 24);
    ix->off_fileslots = rd_u64(base + 32);
    ix->off_pkgdata = rd_u64(base + 40);

    /* sanity: offsets must fit */
    if (ix->off_pkgslots + (uint64_t)ix->n_pkgslots * SLOT_SIZE > total ||
        ix->off_fileslots + (uint64_t)ix->n_fileslots * SLOT_SIZE > total ||
        ix->off_pkgdata > total) {
        munmap(map, ix->map_len);
        free(ix);
        return NULL;
    }

    ix->pkg = d99_xcalloc(ix->n_pkg ? ix->n_pkg : 1, sizeof(idx_pkg));
    off = ix->off_pkgdata;
    for (i = 0; i < ix->n_pkg; i++) {
        const unsigned char *rec = base + off;
        uint32_t name_len = rd_u32(rec);
        uint32_t stanza_len = rd_u32(rec + 4);
        uint32_t n_files = rd_u32(rec + 8);
        uint32_t files_len = rd_u32(rec + 12);
        const char *name = (const char *)(rec + 16);
        const char *stanza = name + name_len;
        const char *file0 = stanza + stanza_len;

        if (off + 16 + name_len + stanza_len + files_len > total) {
            ix->n_pkg = i;
            break;
        }
        ix->pkg[i].name = name;
        ix->pkg[i].stanza = stanza;
        ix->pkg[i].n_files = n_files;
        ix->pkg[i].file0 = file0;
        off += 16 + name_len + stanza_len + files_len;
    }
    return ix;
}

void d99_index_close(d99_index *ix)
{
    size_t i;
    if (!ix)
        return;
    for (i = 0; i < ix->n_pkg; i++) {
        if (ix->pkg[i].version && ix->pkg[i].version[0])
            free((void *)ix->pkg[i].version);
        if (ix->pkg[i].arch && ix->pkg[i].arch[0])
            free((void *)ix->pkg[i].arch);
        if (ix->pkg[i].summary && ix->pkg[i].summary[0])
            free((void *)ix->pkg[i].summary);
    }
    free(ix->pkg);
    munmap(ix->map, ix->map_len);
    free(ix);
}

size_t d99_index_count(d99_index *ix)
{
    return ix ? ix->n_pkg : 0;
}

const char *d99_index_name(d99_index *ix, size_t i)
{
    return i < ix->n_pkg ? ix->pkg[i].name : NULL;
}

const char *d99_index_version(d99_index *ix, size_t i)
{
    if (i >= ix->n_pkg)
        return NULL;
    if (!ix->pkg[i].version) {
        char *sv = dup_field_value(ix->pkg[i].stanza, "Version");
        ix->pkg[i].version = sv ? sv : "";
    }
    return ix->pkg[i].version;
}

const char *d99_index_arch(d99_index *ix, size_t i)
{
    if (i >= ix->n_pkg)
        return NULL;
    if (!ix->pkg[i].arch) {
        char *sv = dup_field_value(ix->pkg[i].stanza, "Architecture");
        ix->pkg[i].arch = sv ? sv : "";
    }
    return ix->pkg[i].arch;
}

const char *d99_index_stanza(d99_index *ix, size_t i)
{
    return i < ix->n_pkg ? ix->pkg[i].stanza : NULL;
}

const char *d99_index_summary(d99_index *ix, size_t i)
{
    if (i >= ix->n_pkg)
        return NULL;
    if (!ix->pkg[i].summary) {
        char *sv = dup_field_value(ix->pkg[i].stanza, "Description");
        ix->pkg[i].summary = sv ? sv : "";
    }
    return ix->pkg[i].summary;
}

int d99_index_status(d99_index *ix, size_t i, int *sel, int *state, int *flags)
{
    if (i >= ix->n_pkg)
        return -1;
    if (!ix->pkg[i].status_parsed) {
        char *stt = dup_field_value(ix->pkg[i].stanza, "Status");
        if (stt) {
            d99_parse_status_words(stt, &ix->pkg[i].sel, &ix->pkg[i].flags,
                                   &ix->pkg[i].state);
            free(stt);
        } else {
            ix->pkg[i].sel = D99_SEL_UNKNOWN;
            ix->pkg[i].state = D99_PS_NOTINSTALLED;
            ix->pkg[i].flags = D99_PF_OK;
        }
        ix->pkg[i].status_parsed = 1;
    }
    *sel = ix->pkg[i].sel;
    *state = ix->pkg[i].state;
    *flags = ix->pkg[i].flags;
    return 0;
}

int d99_index_find_pkg(d99_index *ix, const char *name, size_t *out)
{
    uint32_t h32 = (uint32_t)d99_fnv1a64_str(name);
    const unsigned char *base = ix->map;
    uint32_t mask = ix->n_pkgslots - 1;
    uint32_t s = h32 & mask;
    uint32_t i;

    for (;;) {
        uint64_t sh = rd_u64(base + ix->off_pkgslots + (uint64_t)s * SLOT_SIZE);
        if (sh == 0)
            return 0;
        if ((uint32_t)sh == (h32 | 1u)) {
            i = rd_u32(base + ix->off_pkgslots + (uint64_t)s * SLOT_SIZE + 8);
            if (i < ix->n_pkg && strcmp(ix->pkg[i].name, name) == 0) {
                *out = i;
                return 1;
            }
        }
        s = (s + 1) & mask;
    }
}

size_t d99_index_pkg_nfiles(d99_index *ix, size_t i)
{
    return i < ix->n_pkg ? ix->pkg[i].n_files : 0;
}

const char *d99_index_pkg_file(d99_index *ix, size_t i, size_t j)
{
    const char *p;
    size_t k;

    if (i >= ix->n_pkg || j >= ix->pkg[i].n_files)
        return NULL;
    p = ix->pkg[i].file0;
    for (k = 0; k < j; k++)
        p += 4 + rd_u32((const unsigned char *)p) + 1;
    return p + 4;
}

int d99_index_find_file(d99_index *ix, const char *path,
                        size_t *pkg_out, size_t *file_out)
{
    uint32_t h32 = (uint32_t)d99_fnv1a64_str(path);
    const unsigned char *base = ix->map;
    uint32_t mask = ix->n_fileslots - 1;
    uint32_t s = h32 & mask;
    size_t j;

    for (;;) {
        uint64_t sh = rd_u64(base + ix->off_fileslots + (uint64_t)s * SLOT_SIZE);
        uint32_t i;
        if (sh == 0)
            return 0;
        if ((uint32_t)sh == (h32 | 1u)) {
            i = rd_u32(base + ix->off_fileslots + (uint64_t)s * SLOT_SIZE + 8);
            if (i < ix->n_pkg) {
                const char *p = ix->pkg[i].file0;
                for (j = 0; j < ix->pkg[i].n_files; j++) {
                    uint32_t flen = rd_u32((const unsigned char *)p);
                    const char *f = p + 4;
                    if (strcmp(f, path) == 0) {
                        *pkg_out = i;
                        *file_out = j;
                        return 1;
                    }
                    p += 4 + flen + 1;
                }
            }
        }
        s = (s + 1) & mask;
    }
}
