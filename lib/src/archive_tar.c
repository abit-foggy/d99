#include "d99_archive.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BLK 512

/* ======================= low-level stream reads ======================= */
static int rd_exact(d99_decomp *d, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        long r = d99_decomp_read(d, (char *)buf + got, n - got);
        if (r < 0)
            return -1;
        if (r == 0)
            return got == 0 ? 0 : -1;   /* 0: clean EOF at block boundary */
        got += (size_t)r;
    }
    return 1;
}

/* ======================= tar reader ======================= */
struct d99_tarr {
    d99_decomp *d;
    int own;
    long long member_left;
    long long pad_left;
    /* GNU long name / PAX overrides */
    char pend_path[4096];
    int has_pend_path;
    char pend_link[4096];
    int has_pend_link;
    char glob_path[4096];
    int has_glob_path;
    char glob_link[4096];
    int has_glob_link;
};

static unsigned long long tar_num(const char *p, size_t width)
{
    char tmp[32];

    if (width > sizeof tmp - 1)
        width = sizeof tmp - 1;
    if ((unsigned char)p[0] & 0x80) {
        /* GNU base-256 */
        unsigned long long v = (unsigned char)p[0] & 0x7fu;
        size_t i;
        for (i = 1; i < width; i++)
            v = (v << 8) | (unsigned char)p[i];
        return v;
    }
    memcpy(tmp, p, width);
    tmp[width] = '\0';
    {
        char *q = tmp;
        size_t w = width;
        while (w && (*q == ' ' || *q == '\0')) {
            q++;
            w--;
        }
        if (w == 0)
            return 0;
        /* octal, NUL/space terminated within tmp (worst case last byte) */
        return strtoull(q, NULL, 8);
    }
}

static void copy_str(char *dst, size_t dstn, const char *src, size_t n)
{
    if (n >= dstn)
        n = dstn - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int tar_skip_current(d99_tarr *t)
{
    unsigned char buf[65536];

    while (t->member_left > 0) {
        size_t want = t->member_left > (long long)sizeof buf
                      ? sizeof buf : (size_t)t->member_left;
        long r = d99_decomp_read(t->d, buf, want);
        if (r < 0)
            return -1;
        if (r == 0)
            return -1;
        t->member_left -= r;
    }
    while (t->pad_left > 0) {
        size_t want = t->pad_left > (long long)sizeof buf
                      ? sizeof buf : (size_t)t->pad_left;
        long r = d99_decomp_read(t->d, buf, want);
        if (r < 0)
            return -1;
        if (r == 0)
            return -1;
        t->pad_left -= r;
    }
    return 0;
}

/* Parse a PAX extended header record block: "<len> key=value\n". */
static void pax_parse(d99_tarr *t, char *data, size_t n, int global)
{
    char *p = data, *end = data + n;

    while (p < end) {
        char *sp = memchr(p, ' ', (size_t)(end - p));
        char *eq, *rec_end;
        long reclen;
        char *key, *val;

        if (!sp)
            break;
        reclen = strtol(p, NULL, 10);
        if (reclen <= 0 || p + reclen > end)
            break;
        rec_end = p + reclen;
        eq = memchr(sp, '=', (size_t)(rec_end - sp - 1));
        if (eq && rec_end > sp) {
            key = sp + 1;
            *eq = '\0';
            val = eq + 1;
            if (rec_end[-1] == '\n')
                rec_end[-1] = '\0';
            if (strcmp(key, "path") == 0)
                copy_str(global ? t->glob_path : t->pend_path, 4096, val,
                          (size_t)(rec_end - val));
            else if (strcmp(key, "linkpath") == 0)
                copy_str(global ? t->glob_link : t->pend_link, 4096, val,
                         (size_t)(rec_end - val));
        }
        p = rec_end;
        if (global) {
            t->has_glob_path = t->glob_path[0] ? 1 : t->has_glob_path;
            t->has_glob_link = t->glob_link[0] ? 1 : t->has_glob_link;
        } else {
            t->has_pend_path = t->pend_path[0] ? 1 : t->has_pend_path;
            t->has_pend_link = t->pend_link[0] ? 1 : t->has_pend_link;
        }
    }
}

/* Read the entire payload of an internal member (L/K/x/g), skipping pad. */
static char *read_payload(d99_tarr *t, long long size)
{
    char *buf;
    size_t left = (size_t)size;
    long long pad;

    if (size < 0 || size > (64 << 20))
        return NULL;
    buf = d99_xmalloc(left + 1);
    if (rd_exact(t->d, buf, left) != 1) {
        free(buf);
        return NULL;
    }
    buf[left] = '\0';
    pad = (BLK - ((long long)size % BLK)) % BLK;
    if (pad > 0) {
        char padbuf[BLK];
        if (rd_exact(t->d, padbuf, (size_t)pad) != 1) {
            free(buf);
            return NULL;
        }
    }
    t->pad_left = 0;
    return buf;
}

int d99_tar_next(d99_tarr *t, d99_tar_member *m)
{
    char hdr[BLK];
    int r;
    char name[4096], prefix[256];
    long long size, mtime;
    unsigned long mode;
    long uid, gid;
    char type;
    unsigned chk_stored, chk_calc;
    size_t i;
    int allzero;

    /* consume unread leftovers from the previous member */
    if (t->member_left > 0 || t->pad_left > 0) {
        if (tar_skip_current(t) != 0)
            return -1;
    }

    for (;;) {
        r = rd_exact(t->d, hdr, BLK);
        if (r == 0)
            return 0;   /* clean EOF */
        if (r < 0)
            return -1;

        if (hdr[0] == '\0') {
            allzero = 1;
            for (i = 1; i < BLK; i++)
                if (hdr[i]) { allzero = 0; break; }
            if (allzero)
                return 0;   /* end-of-archive block */
        }

        /* verify checksum (accept both unsigned and signed sums) */
        chk_stored = (unsigned)tar_num(hdr + 148, 8);
        chk_calc = 8 * (unsigned)' ';
        for (i = 0; i < 148; i++)
            chk_calc += (unsigned char)hdr[i];
        for (i = 156; i < BLK; i++)
            chk_calc += (unsigned char)hdr[i];
        if (chk_stored != chk_calc &&
            chk_stored != (unsigned)(int)chk_calc)
            d99_warn("tar header checksum mismatch (stored %u, computed %u)",
                     chk_stored, chk_calc);

        type = hdr[156];
        size = (long long)tar_num(hdr + 124, 12);

        if (type == 'L' || type == 'K') {
            char *data = read_payload(t, size);
            if (!data)
                return -1;
            copy_str(type == 'L' ? t->pend_path : t->pend_link, 4096, data, strlen(data));
            if (type == 'L')
                t->has_pend_path = t->pend_path[0] ? 1 : 0;
            else
                t->has_pend_link = t->pend_link[0] ? 1 : 0;
            free(data);
            continue;
        }
        if (type == 'x' || type == 'g') {
            char *data = read_payload(t, size);
            if (!data)
                return -1;
            pax_parse(t, data, (size_t)size, type == 'g');
            free(data);
            continue;
        }

        /* assemble name: ustar prefix, pending GNU/PAX overrides, else field */
        copy_str(prefix, sizeof prefix, hdr + 345, 155);
        copy_str(name, sizeof name, hdr, 100);
        if (prefix[0] && strncmp(hdr + 257, "ustar", 5) == 0) {
            char tmp[8192];
            snprintf(tmp, sizeof tmp, "%s/%s", prefix, name);
            copy_str(name, sizeof name, tmp, strlen(tmp));
        }

        memset(m, 0, sizeof(*m));
        if (t->has_pend_path)
            copy_str(m->name, sizeof m->name, t->pend_path, strlen(t->pend_path));
        else if (t->has_glob_path)
            copy_str(m->name, sizeof m->name, t->glob_path, strlen(t->glob_path));
        else
            copy_str(m->name, sizeof m->name, name, strlen(name));
        t->has_pend_path = 0;

        if (t->has_pend_link)
            copy_str(m->linkname, sizeof m->linkname, t->pend_link, strlen(t->pend_link));
        else if (t->has_glob_link)
            copy_str(m->linkname, sizeof m->linkname, t->glob_link, strlen(t->glob_link));
        else
            copy_str(m->linkname, sizeof m->linkname, hdr + 157, 100);
        t->has_pend_link = 0;

        mode = (unsigned long)tar_num(hdr + 100, 8);
        uid = (long)tar_num(hdr + 108, 8);
        gid = (long)tar_num(hdr + 116, 8);
        mtime = (long long)tar_num(hdr + 136, 12);
        copy_str(m->uname, sizeof m->uname, hdr + 265, 32);
        copy_str(m->gname, sizeof m->gname, hdr + 297, 32);
        if (type == '\0')
            type = '0';

        m->mode = mode;
        m->uid = uid;
        m->gid = gid;
        m->size = size;
        m->mtime = mtime;
        m->typeflag = type;
        if (type == '1' || type == '2' || type == '3' || type == '4' || type == '5' || type == '6') {
            t->member_left = 0;
            t->pad_left = 0;
        } else {
            t->member_left = size;
            t->pad_left = (BLK - (size % BLK)) % BLK;
        }
        return 1;
    }
}

int d99_tar_read_data(d99_tarr *t, void *buf, size_t n)
{
    if ((long long)n > t->member_left)
        return -1;
    if (rd_exact(t->d, buf, n) != 1)
        return -1;
    t->member_left -= (long long)n;
    return 0;
}

int d99_tar_skip(d99_tarr *t)
{
    return tar_skip_current(t);
}

d99_tarr *d99_tar_open_decomp(d99_decomp *d)
{
    d99_tarr *t = d99_xcalloc(1, sizeof(*t));
    t->d = d;
    t->own = 1;
    return t;
}

d99_tarr *d99_tar_open_read(const char *path)
{
    d99_decomp *d = d99_decomp_open(path);
    d99_tarr *t;

    if (!d)
        return NULL;
    t = d99_tar_open_decomp(d);
    t->own = 1;
    return t;
}

void d99_tar_close_read(d99_tarr *t)
{
    if (!t)
        return;
    if (t->own)
        d99_decomp_close(t->d);
    free(t);
}

/* ======================= tar writer ======================= */
struct d99_tarw {
    FILE *f;
};

static void octal_field(char *dst, int digits, unsigned long long v)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%0*llo ", digits, v);
    memcpy(dst, tmp, (size_t)digits + 1);
}

static void write_header(d99_tarw *tw, const char *name, const char *link,
                         unsigned long mode, long uid, long gid,
                         long long size, long long mtime, char type)
{
    char hdr[BLK];
    unsigned sum = 0;
    size_t i, nlen = strlen(name);
    const char *split = NULL;

    memset(hdr, 0, BLK);

    if (nlen > 100) {
        /* ustar prefix split if possible... */
        size_t p;
        for (p = nlen > 256 ? nlen - 100 : (nlen > 101 ? nlen - 101 : 0);
             p >= 1; p--) {
            if (p <= 155 && name[p] == '\0')
                break;
            if (p <= 155 && name[p - 1] == '/' && nlen - p <= 100) {
                split = name + p - 1;
                break;
            }
        }
        if (!split) {
            /* GNU longname fallback: 'L' member then truncated header */
            char lname[4096];
            long long lsize = (long long)nlen + 1;
            char lh[BLK];
            memset(lh, 0, BLK);
            memcpy(lh, "././@LongLink", 14);
            octal_field(lh + 100, 7, 0644);
            octal_field(lh + 108, 7, 0);
            octal_field(lh + 116, 7, 0);
            octal_field(lh + 124, 11, (unsigned long long)lsize);
            octal_field(lh + 136, 11, 0);
            lh[148] = lh[149] = lh[150] = lh[151] = lh[152] = lh[153] = ' ';
            lh[154] = lh[155] = ' ';
            lh[156] = 'L';
            memcpy(lh + 257, "ustar\0", 6);
            memcpy(lh + 263, "00", 2);
            memcpy(lh + 265, "root", 4);
            memcpy(lh + 297, "root", 4);
            {
                unsigned s2 = 0;
                for (i = 0; i < BLK; i++)
                    s2 += (unsigned char)lh[i];
                snprintf(lh + 148, 8, "%06o", s2);
                lh[154] = '\0';
            }
            fwrite(lh, 1, BLK, tw->f);
            memcpy(lname, name, nlen + 1);
            lname[nlen] = '\0';
            fwrite(lname, 1, (size_t)lsize, tw->f);
            for (i = 0; i < (BLK - (size_t)(lsize % BLK)) % BLK; i++)
                fputc('\0', tw->f);
            memcpy(hdr, name, nlen > 99 ? 99 : nlen);
            nlen = nlen > 99 ? 99 : nlen;
        }
    }

    if (split) {
        size_t plen = (size_t)(split - name);
        memcpy(hdr + 345, name, plen < 155 ? plen : 155);
        memcpy(hdr, split + 1, nlen - plen - 1);
    } else if (nlen <= 100) {
        memcpy(hdr, name, nlen);
    }

    octal_field(hdr + 100, 7, (mode & 07777u));
    octal_field(hdr + 108, 7, (unsigned long long)uid);
    octal_field(hdr + 116, 7, (unsigned long long)gid);
    octal_field(hdr + 124, 11, (unsigned long long)size);
    octal_field(hdr + 136, 11, (unsigned long long)mtime);
    for (i = 148; i < 156; i++)
        hdr[i] = ' ';
    hdr[156] = type;
    if (link)
        memcpy(hdr + 157, link, strlen(link) > 100 ? 100 : strlen(link));
    memcpy(hdr + 257, "ustar\0", 6);
    memcpy(hdr + 263, "00", 2);
    memcpy(hdr + 265, "root", 4);
    memcpy(hdr + 297, "root", 4);
    octal_field(hdr + 329, 7, 0);
    octal_field(hdr + 337, 7, 0);

    for (i = 0; i < BLK; i++)
        sum += (unsigned char)hdr[i];
    snprintf(hdr + 148, 8, "%06o", sum);
    hdr[154] = '\0';
    hdr[155] = ' ';
    fwrite(hdr, 1, BLK, tw->f);
}

static void write_pad(d99_tarw *tw, long long size)
{
    long long pad = (BLK - (size % BLK)) % BLK;
    long long i;
    for (i = 0; i < pad; i++)
        fputc('\0', tw->f);
}

d99_tarw *d99_tar_open_write(FILE *f)
{
    d99_tarw *tw = d99_xmalloc(sizeof(*tw));
    tw->f = f;
    return tw;
}

int d99_tarw_add_data(d99_tarw *tw, const char *arc_name, const void *data,
                      size_t len, unsigned long mode)
{
    write_header(tw, arc_name, NULL, mode ? mode : 0644, 0, 0,
                 (long long)len, 0, '0');
    if (len)
        fwrite(data, 1, len, tw->f);
    write_pad(tw, (long long)len);
    return 0;
}

int d99_tarw_add_file(d99_tarw *tw, const char *fs_path, const char *arc_name,
                      unsigned long mode)
{
    FILE *in = fopen(fs_path, "rb");
    struct stat st;
    unsigned char buf[65536];
    long long left;

    if (!in)
        return -1;
    if (fstat(fileno(in), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(in);
        return -1;
    }
    if (!mode)
        mode = (unsigned long)st.st_mode;
    write_header(tw, arc_name, NULL, mode, (long)st.st_uid, (long)st.st_gid,
                 (long long)st.st_size, (long long)st.st_mtime, '0');
    left = (long long)st.st_size;
    while (left > 0) {
        size_t want = left > (long long)sizeof buf ? sizeof buf : (size_t)left;
        size_t got = fread(buf, 1, want, in);
        if (got == 0 || fwrite(buf, 1, got, tw->f) != got) {
            fclose(in);
            return -1;
        }
        left -= (long long)got;
    }
    fclose(in);
    write_pad(tw, (long long)st.st_size);
    return 0;
}

int d99_tarw_add_dir(d99_tarw *tw, const char *arc_name, unsigned long mode)
{
    char name[4096];
    size_t n = strlen(arc_name);

    if (n + 1 >= sizeof name)
        return -1;
    memcpy(name, arc_name, n);
    if (n && name[n - 1] != '/')
        name[n++] = '/';
    name[n] = '\0';
    write_header(tw, name, NULL, mode ? mode : 0755, 0, 0, 0, 0, '5');
    return 0;
}

int d99_tarw_add_symlink(d99_tarw *tw, const char *arc_name, const char *target)
{
    write_header(tw, arc_name, target, 0777, 0, 0, 0, 0, '2');
    return 0;
}

int d99_tarw_close(d99_tarw *tw)
{
    /* two zero blocks */
    static const char zero[BLK] = {0};
    fwrite(zero, 1, BLK, tw->f);
    fwrite(zero, 1, BLK, tw->f);
    if (fflush(tw->f) != 0) {
        free(tw);
        return -1;
    }
    free(tw);
    return 0;
}

int d99_tarw_add_path(d99_tarw *tw, const char *fs_path, const char *arc_prefix)
{
    struct stat st;
    char child_arc[4096];
    DIR *dir;
    struct dirent *de;
    d99_strvec names;
    size_t i;
    size_t plen = strlen(arc_prefix);

    if (lstat(fs_path, &st) != 0) {
        d99_warn("cannot stat %s", fs_path);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        int rc = d99_tarw_add_dir(tw, arc_prefix, (unsigned long)st.st_mode);
        if (rc != 0)
            return rc;
        dir = opendir(fs_path);
        if (!dir)
            return -1;
        d99_sv_init(&names);
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            d99_sv_push(&names, de->d_name);
        }
        closedir(dir);
        d99_sv_sort(&names);
        for (i = 0; i < names.n; i++) {
            char *cfs = d99_path_join(fs_path, names.v[i]);
            int trailing = plen && arc_prefix[plen - 1] == '/';
            snprintf(child_arc, sizeof child_arc, "%s%s%s",
                     arc_prefix, trailing ? "" : "/", names.v[i]);
            rc = d99_tarw_add_path(tw, cfs, child_arc);
            free(cfs);
            if (rc != 0)
                break;
        }
        d99_sv_free(&names);
        return rc;
    }
    if (S_ISREG(st.st_mode)) {
        /* strip trailing slash from the arc name for files */
        char name[4096];
        copy_str(name, sizeof name, arc_prefix, plen);
        if (plen && name[plen - 1] == '/')
            name[plen - 1] = '\0';
        if (name[0] == '\0')
            return -1;
        return d99_tarw_add_file(tw, fs_path, name, 0);
    }
    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t n = readlink(fs_path, target, sizeof target - 1);
        char name[4096];
        if (n < 0)
            return -1;
        target[n] = '\0';
        copy_str(name, sizeof name, arc_prefix, plen);
        if (plen && name[plen - 1] == '/')
            name[plen - 1] = '\0';
        return d99_tarw_add_symlink(tw, name, target);
    }
    d99_warn("skipping special file %s", fs_path);
    return 0;
}

/* ======================= extraction ======================= */
/* Normalize a tar member name: strip leading "./" and "/", reject "..".
 * Returns length, or -1 on rejection. */
static long tar_normalize(const char *in, char *out, size_t outn)
{
    const char *p = in;
    size_t o = 0;

    while (*p == '/' || (*p == '.' && p[1] == '/'))
        p += (*p == '/') ? 1 : 2;

    while (*p) {
        const char *c = p;
        size_t cl = 0;
        if (*p == '/') {
            p++;
            continue;
        }
        while (c[cl] && c[cl] != '/')
            cl++;
        if (cl == 1 && c[0] == '.')
            goto next;
        if (cl == 2 && c[0] == '.' && c[1] == '.')
            return -1;
        if (o + cl + 2 >= outn)
            return -1;
        if (o)
            out[o++] = '/';
        memcpy(out + o, c, cl);
        o += cl;
next:
        p += cl;
    }
    out[o] = '\0';
    while (o > 1 && out[o - 1] == '/')
        out[--o] = '\0';
    return (long)o;
}

typedef struct {
    int fd;
    char path[4096];
} pdir_cache;

static int open_parent(pdir_cache *pc, int rootfd, const char *path, char *base, size_t basen,
                       int *outfd)
{
    char tmp[4096];
    char *slash;
    size_t n;
    int cur;

    n = strlen(path);
    if (n >= sizeof tmp)
        return -1;
    memcpy(tmp, path, n + 1);
    slash = strrchr(tmp, '/');

    if (!slash) {
        snprintf(base, basen, "%s", tmp);
        *outfd = dup(rootfd);
        return *outfd < 0 ? -1 : 0;
    }
    *slash = '\0';
    snprintf(base, basen, "%s", slash + 1);
    if (base[0] == '\0')
        return -1;

    if (pc && pc->fd >= 0 && strcmp(pc->path, tmp) == 0) {
        *outfd = dup(pc->fd);
        return *outfd < 0 ? -1 : 0;
    }

    cur = dup(rootfd);
    if (cur < 0)
        return -1;
    {
        char *comp = tmp;
        while (*comp) {
            char *next = strchr(comp, '/');
            char saved = 0;
            int fd;
            if (next) {
                saved = *next;
                *next = '\0';
            }
            if (*comp) {
                fd = openat(cur, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                if (fd < 0 && (errno == ELOOP || errno == ENOTDIR))
                    fd = openat(cur, comp, O_RDONLY | O_DIRECTORY);
                if (fd < 0 && errno == ENOENT) {
                    if (mkdirat(cur, comp, 0755) != 0 && errno != EEXIST) {
                        close(cur);
                        return -1;
                    }
                    fd = openat(cur, comp, O_RDONLY | O_DIRECTORY);
                }
                close(cur);
                if (fd < 0)
                    return -1;
                cur = fd;
            }
            if (next) {
                *next = saved;
                comp = next + 1;
            } else {
                break;
            }
        }
    }
    if (pc) {
        if (pc->fd >= 0)
            close(pc->fd);
        pc->fd = dup(cur);
        snprintf(pc->path, sizeof pc->path, "%s", tmp);
    }
    *outfd = cur;
    return 0;
}
static int is_zero_len(const d99_tar_member *m, const char *norm)
{
    return norm[0] == '\0' && m->size == 0;
}

long d99_tar_extract(d99_tarr *t, const d99_tar_extract_opts *o)
{
    d99_tar_member m;
    char norm[4096], mapped[4096];
    char base[4096];
    int rootfd;
    long count = 0;
    int r;
    pdir_cache pc;

    pc.fd = -1;
    pc.path[0] = '\0';

    rootfd = open(o->dest, O_RDONLY | O_DIRECTORY);
    if (rootfd < 0 && errno == ENOENT) {
        if (d99_mkdir_p(o->dest, 0755) == 0)
            rootfd = open(o->dest, O_RDONLY | O_DIRECTORY);
    }
    if (rootfd < 0) {
        d99_warn("cannot open extraction root %s: %s", o->dest, strerror(errno));
        return -1;
    }

    while ((r = d99_tar_next(t, &m)) == 1) {
        long nlen = tar_normalize(m.name, norm, sizeof norm);
        const char *use = norm;
        int is_dir = (m.typeflag == '5');

        if (nlen < 0) {
            d99_warn("rejecting unsafe member path '%s'", m.name);
            d99_tar_skip(t);
            if (!o->keep_going) {
                close(rootfd);
                return -1;
            }
            continue;
        }
        if (is_zero_len(&m, norm))
            continue;   /* the "./" root entry */

        if (o->skip && o->skip(norm, o->skip_ud)) {
            d99_tar_skip(t);
            continue;
        }
        if (o->map) {
            const char *mp = o->map(norm, is_dir, o->map_ud);
            if (!mp) {
                d99_tar_skip(t);
                continue;
            }
            snprintf(mapped, sizeof mapped, "%s", mp);
            use = mapped;
        }
        if (o->verbose)
            printf(".%s%s\n", use, is_dir && use[0] ? "/" : "");

        if (is_dir) {
            if (mkdirat(rootfd, use[0] ? use : ".", (mode_t)(m.mode & 07777)) != 0) {
                if (errno != EEXIST) {
                    d99_warn("mkdir %s: %s", use, strerror(errno));
                    if (!o->keep_going) { close(rootfd); return -1; }
                    continue;
                }
            } else {
                int pfd, rc;
                char b2[4096];
                if (open_parent(&pc, rootfd, use, b2, sizeof b2, &pfd) == 0) {
                    rc = fchmodat(pfd, b2, (mode_t)(m.mode & 07777), AT_SYMLINK_NOFOLLOW);
                    (void)rc;
                    close(pfd);
                }
            }
            if (o->record)
                o->record(use, o->record_ud);
            count++;
            continue;
        }

        if (m.typeflag == '0' || m.typeflag == '7') {
            int dfd, fd;
            if (open_parent(&pc, rootfd, use, base, sizeof base, &dfd) != 0)
                goto member_err_with_errno;
            fd = openat(dfd, base, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                        (mode_t)(m.mode & 07777) ? (mode_t)(m.mode & 07777) : 0644);
            if (fd < 0 && errno == ELOOP) {
                unlinkat(dfd, base, 0);
                fd = openat(dfd, base, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                            (mode_t)(m.mode & 07777) ? (mode_t)(m.mode & 07777) : 0644);
            }
            close(dfd);
            if (fd < 0)
                goto member_err_with_errno;
            posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
            {
                unsigned char buf[131072];
                long long left = m.size;
                while (left > 0) {
                    size_t want = left > (long long)sizeof buf
                                  ? sizeof buf : (size_t)left;
                    if (d99_tar_read_data(t, buf, want) != 0) {
                        close(fd);
                        d99_warn("truncated member %s", m.name);
                        if (!o->keep_going) {
                            if (pc.fd >= 0) close(pc.fd);
                            close(rootfd);
                            return -1;
                        }
                        goto next_member;
                    }
                    if (write(fd, buf, want) < 0) {
                        close(fd);
                        goto member_err_with_errno;
                    }
                    left -= (long long)want;
                }
                fchmod(fd, (mode_t)(m.mode & 07777));
                if (geteuid() == 0) {
                    int rcc = fchown(fd, (uid_t)m.uid, (gid_t)m.gid);
                    (void)rcc;
                }
                if (close(fd) != 0)
                    goto member_err_with_errno;
            }
            if (o->record)
                o->record(use, o->record_ud);
            count++;
            continue;
        }

        if (m.typeflag == '2') {  /* symlink */
            int dfd;
            if (m.linkname[0] == '\0') {
                d99_warn("empty symlink target for '%s'", use);
                d99_tar_skip(t);
                continue;
            }
            if (open_parent(&pc, rootfd, use, base, sizeof base, &dfd) != 0)
                goto member_err_with_errno;
            unlinkat(dfd, base, 0);
            if (symlinkat(m.linkname, dfd, base) != 0) {
                close(dfd);
                goto member_err_with_errno;
            }
            close(dfd);
            if (o->record)
                o->record(use, o->record_ud);
            count++;
            continue;
        }

        if (m.typeflag == '1') {  /* hard link */
            char target[4096], tbase[4096];
            int dfd, tfd;
            if (tar_normalize(m.linkname, target, sizeof target) < 0) {
                d99_warn("rejecting unsafe hardlink target '%s'", m.linkname);
                d99_tar_skip(t);
                if (!o->keep_going) {
                    if (pc.fd >= 0) close(pc.fd);
                    close(rootfd);
                    return -1;
                }
                continue;
            }
            if (open_parent(&pc, rootfd, target, tbase, sizeof tbase, &tfd) != 0)
                goto member_err_with_errno;
            if (open_parent(&pc, rootfd, use, base, sizeof base, &dfd) != 0) {
                close(tfd);
                goto member_err_with_errno;
            }
            unlinkat(dfd, base, 0);
            if (linkat(tfd, tbase, dfd, base, 0) != 0) {
                close(tfd);
                close(dfd);
                goto member_err_with_errno;
            }
            close(tfd);
            close(dfd);
            if (o->record)
                o->record(use, o->record_ud);
            count++;
            continue;
        }

        if (m.typeflag == '6') {  /* FIFO */
            int dfd;
            if (open_parent(&pc, rootfd, use, base, sizeof base, &dfd) != 0)
                goto member_err_with_errno;
            unlinkat(dfd, base, 0);
            if (mknodat(dfd, base, S_IFIFO | (mode_t)(m.mode & 07777), 0) != 0) {
                close(dfd);
                goto member_err_with_errno;
            }
            close(dfd);
            if (o->record)
                o->record(use, o->record_ud);
            count++;
            continue;
        }

        /* devices and unknown types: refuse (policy) */
        d99_warn("skipping unsupported member type '%c' %s", m.typeflag, m.name);
        d99_tar_skip(t);
        continue;

member_err_with_errno:
        d99_warn("%s: %s", use, strerror(errno));
        if (!o->keep_going) {
            if (pc.fd >= 0) close(pc.fd);
            close(rootfd);
            return -1;
        }
        d99_tar_skip(t);
next_member:
        continue;
    }
    if (pc.fd >= 0)
        close(pc.fd);
    if (r < 0) {
        d99_warn("truncated tar stream");
        close(rootfd);
        return -1;
    }
    close(rootfd);
    return count;
}
