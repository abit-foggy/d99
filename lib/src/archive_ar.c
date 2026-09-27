#include "d99_archive.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AR_MAGIC "!<arch>\n"
#define AR_HDR   60

struct d99_ar {
    FILE *f;
    int writing;
    long long next_off;      /* offset of the next member header */
    char *extnames;          /* GNU "//" long-name table, NULL if none */
};

d99_ar *d99_ar_open_read(const char *path)
{
    char magic[8];
    d99_ar *ar;
    FILE *f = fopen(path, "rb");

    if (!f)
        return NULL;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, AR_MAGIC, 8) != 0) {
        fclose(f);
        return NULL;
    }
    posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL);
    ar = d99_xcalloc(1, sizeof(*ar));
    ar->f = f;
    ar->next_off = 8;
    return ar;
}

d99_ar *d99_ar_open_write(const char *path)
{
    d99_ar *ar;
    FILE *f = fopen(path, "wb");

    if (!f)
        return NULL;
    if (fwrite(AR_MAGIC, 1, 8, f) != 8) {
        fclose(f);
        return NULL;
    }
    ar = d99_xcalloc(1, sizeof(*ar));
    ar->f = f;
    ar->writing = 1;
    return ar;
}

FILE *d99_ar_stream(d99_ar *ar)
{
    return ar->f;
}

/* Load the GNU extended-name table on demand by scanning the archive. */
static int ar_load_extnames(d99_ar *ar)
{
    long long save = ar->next_off;
    long long off = 8;
    char hdr[AR_HDR];

    ar->next_off = 8;
    for (;;) {
        char nbuf[17];
        long long size, data_off;

        if (fseeko(ar->f, off, SEEK_SET) != 0)
            break;
        if (fread(hdr, 1, AR_HDR, ar->f) != AR_HDR)
            break;
        memcpy(nbuf, hdr, 16);
        nbuf[16] = '\0';
        size = strtoll(hdr + 48, NULL, 10);
        data_off = off + AR_HDR;
        {
            char *sp = strchr(nbuf, '/');
            char *name = nbuf;
            if (sp && sp == nbuf) {
                /* "//" extended name table */
                ar->extnames = d99_xmalloc((size_t)size + 1);
                if (fseeko(ar->f, data_off, SEEK_SET) != 0 ||
                    fread(ar->extnames, 1, (size_t)size, ar->f) != (size_t)size) {
                    free(ar->extnames);
                    ar->extnames = NULL;
                } else {
                    ar->extnames[size] = '\0';
                }
                break;
            }
            (void)name;
        }
        off = data_off + size + (size & 1);
    }
    ar->next_off = save;
    if (fseeko(ar->f, save, SEEK_SET) != 0)
        return -1;
    return ar->extnames ? 0 : -1;
}

int d99_ar_next(d99_ar *ar, d99_ar_member *m)
{
    char hdr[AR_HDR];
    char name[17];
    long long size, data_off;
    size_t i;
    int bsd_namelen = 0;

    if (ar->writing)
        return -1;
    if (fseeko(ar->f, ar->next_off, SEEK_SET) != 0)
        return -1;
    if (fread(hdr, 1, AR_HDR, ar->f) != AR_HDR) {
        if (feof(ar->f))
            return 0;
        return -1;
    }
    if (hdr[58] != '`' || hdr[59] != '\n') {
        d99_warn("corrupt ar member header at offset %lld", ar->next_off);
        return -1;
    }
    memcpy(name, hdr, 16);
    name[16] = '\0';
    size = strtoll(hdr + 48, NULL, 10);
    if (size < 0)
        return -1;
    data_off = ar->next_off + AR_HDR;

    /* BSD '#1/<len>' — name lives in the first <len> bytes of payload. */
    if (name[0] == '#' && name[1] == '1' && name[2] == '/') {
        bsd_namelen = (int)strtol(name + 3, NULL, 10);
        if (bsd_namelen <= 0 || bsd_namelen > (int)sizeof(m->name) - 1 ||
            bsd_namelen > size)
            return -1;
        if (fseeko(ar->f, data_off, SEEK_SET) != 0)
            return -1;
        if (fread(m->name, 1, (size_t)bsd_namelen, ar->f) != (size_t)bsd_namelen)
            return -1;
        m->name[bsd_namelen] = '\0';
        size -= bsd_namelen;
        data_off += bsd_namelen;
        /* strip trailing spaces */
        for (i = strlen(m->name); i > 0 && m->name[i - 1] == ' '; i--)
            m->name[i - 1] = '\0';
    } else if (name[0] == '/' && name[1] == '/') {
        /* GNU long-name table: load it lazily and skip. */
        if (!ar->extnames && ar_load_extnames(ar) != 0) {
            ar->next_off = data_off + size + (size & 1);
            return d99_ar_next(ar, m);
        }
        ar->next_off = data_off + size + (size & 1);
        return d99_ar_next(ar, m);
    } else if (name[0] == '/' && name[1] >= '0' && name[1] <= '9') {
        /* GNU long name by offset into "//" table. */
        long noff = strtol(name + 1, NULL, 10);
        if (!ar->extnames && ar_load_extnames(ar) != 0)
            return -1;
        {
            const char *p = ar->extnames + noff;
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= sizeof(m->name))
                n = sizeof(m->name) - 1;
            memcpy(m->name, p, n);
            m->name[n] = '\0';
        }
    } else {
        /* Normal member: strip trailing '/' and spaces. */
        char *p = name;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == '/' || p[n - 1] == ' '))
            p[--n] = '\0';
        if (n >= sizeof(m->name))
            n = sizeof(m->name) - 1;
        memcpy(m->name, p, n + 1);
    }

    m->size = size;
    m->data_off = data_off;
    ar->next_off = data_off + size + (size & 1);
    if (m->name[0] == '\0')
        return -1;
    return 1;
}

int d99_ar_extract_member(d99_ar *ar, const d99_ar_member *m, const char *dest)
{
    FILE *out = fopen(dest, "wb");
    unsigned char buf[65536];
    long long left = m->size;

    if (!out)
        return -1;
    if (fseeko(ar->f, m->data_off, SEEK_SET) != 0) {
        fclose(out);
        return -1;
    }
    while (left > 0) {
        size_t want = left > (long long)sizeof buf ? sizeof buf : (size_t)left;
        size_t got = fread(buf, 1, want, ar->f);
        if (got == 0 || fwrite(buf, 1, got, out) != got) {
            fclose(out);
            unlink(dest);
            return -1;
        }
        left -= (long long)got;
    }
    if (fclose(out) != 0) {
        unlink(dest);
        return -1;
    }
    return 0;
}

int d99_ar_add_mem(d99_ar *ar, const char *name, const void *data, size_t len)
{
    char hdr[AR_HDR];
    size_t nlen = strlen(name);

    if (nlen > 15) {
        d99_warn("ar member name too long: %s", name);
        return -1;
    }
    memset(hdr, ' ', sizeof hdr);
    memcpy(hdr, name, nlen);
    hdr[nlen] = '/';
    snprintf(hdr + 16, 13, "%-12d", 0);     /* mtime */
    snprintf(hdr + 28, 7, "%-6d", 0);       /* uid */
    snprintf(hdr + 34, 7, "%-6d", 0);        /* gid */
    snprintf(hdr + 40, 9, "%-8o", 0644u);    /* mode */
    snprintf(hdr + 48, 11, "%-10d", (int)len);
    hdr[58] = '`';
    hdr[59] = '\n';
    if (fwrite(hdr, 1, AR_HDR, ar->f) != AR_HDR)
        return -1;
    if (len && fwrite(data, 1, len, ar->f) != len)
        return -1;
    if (len & 1 && fputc('\n', ar->f) == EOF)
        return -1;
    return 0;
}

int d99_ar_add_file(d99_ar *ar, const char *name, const char *src)
{
    FILE *in = fopen(src, "rb");
    struct stat st;
    unsigned char buf[65536];
    char hdr[AR_HDR];
    size_t nlen = strlen(name);
    long long size, left;

    if (!in)
        return -1;
    if (fstat(fileno(in), &st) != 0) {
        fclose(in);
        return -1;
    }
    size = (long long)st.st_size;
    if (nlen > 15) {
        fclose(in);
        d99_warn("ar member name too long: %s", name);
        return -1;
    }
    memset(hdr, ' ', sizeof hdr);
    memcpy(hdr, name, nlen);
    hdr[nlen] = '/';
    snprintf(hdr + 16, 13, "%-12d", 0);
    snprintf(hdr + 28, 7, "%-6d", 0);
    snprintf(hdr + 34, 7, "%-6d", 0);
    snprintf(hdr + 40, 9, "%-8o", 0644u);
    snprintf(hdr + 48, 11, "%-10lld", size);
    hdr[58] = '`';
    hdr[59] = '\n';
    if (fwrite(hdr, 1, AR_HDR, ar->f) != AR_HDR) {
        fclose(in);
        return -1;
    }
    left = size;
    while (left > 0) {
        size_t want = left > (long long)sizeof buf ? sizeof buf : (size_t)left;
        size_t got = fread(buf, 1, want, in);
        if (got == 0 || fwrite(buf, 1, got, ar->f) != got) {
            fclose(in);
            return -1;
        }
        left -= (long long)got;
    }
    fclose(in);
    if (size & 1 && fputc('\n', ar->f) == EOF)
        return -1;
    return 0;
}

int d99_ar_close(d99_ar *ar)
{
    int r;
    if (!ar)
        return 0;
    r = (fflush(ar->f) == 0 && fclose(ar->f) == 0) ? 0 : -1;
    free(ar->extnames);
    free(ar);
    return r;
}
