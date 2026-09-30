#include "d99_archive.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if HAVE_ZLIB
#include <zlib.h>
#endif
#if HAVE_LIBLZMA
#include <lzma.h>
#endif
#if HAVE_ZSTD
#include <zstd.h>
#endif

int d99_comp_support(int fmt)
{
    switch (fmt) {
    case D99_CFMT_NONE: return 1;
#if HAVE_ZLIB
    case D99_CFMT_GZ: return 1;
#endif
#if HAVE_LIBLZMA
    case D99_CFMT_XZ: return 1;
#endif
#if HAVE_ZSTD
    case D99_CFMT_ZST: return 1;
#endif
    default: return 0;
    }
}

int d99_sniff(const void *magic, size_t n)
{
    const unsigned char *p = magic;
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b)
        return D99_CFMT_GZ;
    if (n >= 6 && p[0] == 0xfd && p[1] == 0x37 && p[2] == 0x7a &&
        p[3] == 0x58 && p[4] == 0x5a && p[5] == 0x00)
        return D99_CFMT_XZ;
    if (n >= 4 && p[0] == 0x28 && p[1] == 0xb5 && p[2] == 0x2f && p[3] == 0xfd)
        return D99_CFMT_ZST;
    return D99_CFMT_NONE;
}

const char *d99_comp_ext(int fmt)
{
    switch (fmt) {
    case D99_CFMT_GZ:  return ".gz";
    case D99_CFMT_XZ:  return ".xz";
    case D99_CFMT_ZST: return ".zst";
    default:           return "";
    }
}

const char *d99_comp_name(int fmt)
{
    switch (fmt) {
    case D99_CFMT_GZ:  return "gzip";
    case D99_CFMT_XZ:  return "xz";
    case D99_CFMT_ZST: return "zstd";
    default:           return "none";
    }
}

struct d99_decomp {
    int fmt;
    int eof;
    FILE *f;                 /* NONE and XZ/ZST backends */
    long long remaining;     /* NONE only */
#if HAVE_ZLIB
    gzFile gz;               /* GZ backend (dup'd fd) */
#endif
#if HAVE_LIBLZMA
    lzma_stream xs;
    unsigned char inbuf[65536];
    size_t inpos, inlen;
    int in_eof;
    int xs_open;
#endif
#if HAVE_ZSTD
    ZSTD_DStream *zs;
    ZSTD_inBuffer zin;
    unsigned char *zinbuf;
    size_t zinbuf_n;
    int zs_open;
#endif
    int owns_f;
};

d99_decomp *d99_decomp_open_region(FILE *f, long long off, long long size, int fmt)
{
    d99_decomp *d;
    unsigned char magic[6];
    size_t got;

    if (fmt < 0) {
        if (fseeko(f, off, SEEK_SET) != 0)
            return NULL;
        memset(magic, 0, sizeof magic);
        got = fread(magic, 1, sizeof magic, f);
        fmt = d99_sniff(magic, got);
        if (fseeko(f, off, SEEK_SET) != 0)
            return NULL;
    }
    d = d99_xcalloc(1, sizeof(*d));
    d->fmt = fmt;
    d->remaining = size;

    switch (fmt) {
    case D99_CFMT_NONE:
        d->f = f;
        if (fseeko(f, off, SEEK_SET) != 0) {
            free(d);
            return NULL;
        }
        return d;
#if HAVE_ZLIB
    case D99_CFMT_GZ: {
        int fd = dup(fileno(f));
        if (fd < 0) {
            free(d);
            return NULL;
        }
        if (lseek(fd, off, SEEK_SET) < 0) {
            close(fd);
            free(d);
            return NULL;
        }
        d->gz = gzdopen(fd, "rb");
        if (!d->gz) {
            close(fd);
            free(d);
            return NULL;
        }
        return d;
    }
#endif
#if HAVE_LIBLZMA
    case D99_CFMT_XZ: {
        lzma_mt mt;
        int ok = 0;
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        if (nprocs < 1) nprocs = 1;
        if (nprocs > 8) nprocs = 8;
        memset(&mt, 0, sizeof(mt));
        mt.threads = (uint32_t)nprocs;
        mt.memlimit_stop = UINT64_MAX;
        mt.memlimit_threading = UINT64_MAX;
        if (lzma_stream_decoder_mt(&d->xs, &mt) == LZMA_OK)
            ok = 1;
        else if (lzma_stream_decoder(&d->xs, UINT64_MAX, 0) == LZMA_OK)
            ok = 1;
        if (!ok) {
            free(d);
            return NULL;
        }
        d->xs_open = 1;
        d->f = f;
        if (fseeko(f, off, SEEK_SET) != 0) {
            free(d);
            return NULL;
        }
        return d;
    }
#endif
#if HAVE_ZSTD
    case D99_CFMT_ZST:
        d->zs = ZSTD_createDStream();
        if (!d->zs || ZSTD_isError(ZSTD_initDStream(d->zs))) {
            if (d->zs)
                ZSTD_freeDStream(d->zs);
            free(d);
            return NULL;
        }
        d->zs_open = 1;
        d->zinbuf_n = ZSTD_DStreamInSize();
        d->zinbuf = d99_xmalloc(d->zinbuf_n);
        d->f = f;
        if (fseeko(f, off, SEEK_SET) != 0) {
            free(d);
            return NULL;
        }
        return d;
#endif
    default:
        free(d);
        return NULL;
    }
}

d99_decomp *d99_decomp_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    d99_decomp *d;
    long long size;
    struct stat st;

    if (!f)
        return NULL;
    if (fstat(fileno(f), &st) == 0)
        size = (long long)st.st_size;
    else
        size = 1 << 30;
    int fmt = -1;
    size_t plen = strlen(path);
    if (plen >= 3 && strcmp(path + plen - 3, ".xz") == 0)
        fmt = D99_CFMT_XZ;
    else if (plen >= 4 && strcmp(path + plen - 4, ".zst") == 0)
        fmt = D99_CFMT_ZST;
    else if (plen >= 3 && strcmp(path + plen - 3, ".gz") == 0)
        fmt = D99_CFMT_GZ;

    d = d99_decomp_open_region(f, 0, size, fmt);
    if (!d)
        fclose(f);
    else {
        d->owns_f = 1;
        if (d->fmt != D99_CFMT_NONE)
            d->f = f; /* keep the handle for close(); gz owns its own fd */
    }
    return d;
}

long d99_decomp_read(d99_decomp *d, void *buf, size_t n)
{
    if (n == 0)
        return 0;
    switch (d->fmt) {
    case D99_CFMT_NONE: {
        size_t want = d->remaining < (long long)n ? (size_t)d->remaining : n;
        size_t got;
        if (want == 0)
            return 0;
        got = fread(buf, 1, want, d->f);
        if (got == 0)
            return ferror(d->f) ? -1 : 0;
        d->remaining -= (long long)got;
        return (long)got;
    }
#if HAVE_ZLIB
    case D99_CFMT_GZ: {
        int r = gzread(d->gz, buf, (unsigned)n);
        if (r < 0)
            return -1;
        if (r == 0)
            return gzeof(d->gz) ? 0 : -1;
        return r;
    }
#endif
#if HAVE_LIBLZMA
    case D99_CFMT_XZ: {
        size_t room = n;
        for (;;) {
            if (d->inpos == d->inlen && !d->in_eof) {
                size_t want = sizeof(d->inbuf);
                if (d->remaining >= 0 && (long long)want > d->remaining)
                    want = (size_t)d->remaining;
                d->inlen = want > 0 ? fread(d->inbuf, 1, want, d->f) : 0;
                d->inpos = 0;
                if (d->remaining >= 0)
                    d->remaining -= (long long)d->inlen;
                if (d->inlen == 0)
                    d->in_eof = 1;
            }
            d->xs.next_in = d->inbuf + d->inpos;
            d->xs.avail_in = (size_t)(d->inlen - d->inpos);
            d->xs.next_out = (uint8_t *)buf + (n - room);
            d->xs.avail_out = room;
            {
                lzma_ret r = lzma_code(&d->xs, LZMA_RUN);
                d->inpos = d->inlen - d->xs.avail_in;
                room = d->xs.avail_out;
                if (r == LZMA_STREAM_END) {
                    if (room == n)
                        return 0;      /* clean eof with no output */
                    d->eof = 1;
                    return (long)(n - room);
                }
                if (r != LZMA_OK && r != LZMA_BUF_ERROR)
                    return -1;
                if (room < n)
                    return (long)(n - room);   /* produced output */
                if (d->in_eof && d->xs.avail_in == 0)
                    return -1;                 /* truncated stream */
            }
        }
    }
#endif
#if HAVE_ZSTD
    case D99_CFMT_ZST: {
        size_t room = n;
        for (;;) {
            if (d->zin.pos == d->zin.size && !d->eof) {
                size_t want = d->zinbuf_n;
                if (d->remaining >= 0 && (long long)want > d->remaining)
                    want = (size_t)d->remaining;
                size_t got = want > 0 ? fread(d->zinbuf, 1, want, d->f) : 0;
                d->zin.src = d->zinbuf;
                d->zin.size = got;
                d->zin.pos = 0;
                if (d->remaining >= 0)
                    d->remaining -= (long long)got;
                if (got == 0) {
                    if (room == n)
                        return 0;
                    return -1;   /* truncated */
                }
            }
            {
                ZSTD_outBuffer out;
                out.dst = (uint8_t *)buf + (n - room);
                out.size = room;
                out.pos = 0;
                {
                    size_t r = ZSTD_decompressStream(d->zs, &out, &d->zin);
                    if (ZSTD_isError(r))
                        return -1;
                    room -= out.pos;
                    if (out.pos > 0)
                        return (long)out.pos;
                    if (r == 0) {
                        d->eof = 1;
                        return 0;   /* frame complete */
                    }
                }
            }
        }
    }
#endif
    default:
        return -1;
    }
}

char *d99_decomp_slurp(d99_decomp *d, size_t *len)
{
    size_t cap = 1 << 16, total = 0;
    char *buf = d99_xmalloc(cap + 1);

    for (;;) {
        long r;
        if (total + 1 > cap) {
            cap *= 2;
            buf = d99_xrealloc(buf, cap + 1);
        }
        r = d99_decomp_read(d, buf + total, cap - total);
        if (r < 0) {
            free(buf);
            return NULL;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    if (len)
        *len = total;
    return buf;
}

void d99_decomp_close(d99_decomp *d)
{
    if (!d)
        return;
#if HAVE_ZLIB
    if (d->fmt == D99_CFMT_GZ && d->gz)
        gzclose(d->gz);
    else
#endif
    {
        if (d->owns_f && d->f)
            fclose(d->f);
    }
#if HAVE_LIBLZMA
    if (d->xs_open)
        lzma_end(&d->xs);
#endif
#if HAVE_ZSTD
    if (d->zs) {
        ZSTD_freeDStream(d->zs);
        free(d->zinbuf);
    }
#endif
    free(d);
}

/* Copy src to dst applying the requested compression. */
int d99_compress_file(const char *src, const char *dst, int fmt)
{
    FILE *in = fopen(src, "rb");
    unsigned char buf[65536];
    int fail = 0;

    if (!in)
        return -1;

    switch (fmt) {
    case D99_CFMT_NONE: {
        FILE *out = fopen(dst, "wb");
        size_t r;
        if (!out) {
            fclose(in);
            return -1;
        }
        while ((r = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, r, out) != r)
                fail = 1;
        if (fflush(out) != 0 || fclose(out) != 0)
            fail = 1;
        break;
    }
#if HAVE_ZLIB
    case D99_CFMT_GZ: {
        gzFile out = gzopen(dst, "wb6");
        int r;
        if (!out) {
            fclose(in);
            return -1;
        }
        while ((r = (int)fread(buf, 1, sizeof buf, in)) > 0)
            if (gzwrite(out, buf, (unsigned)r) != r)
                fail = 1;
        if (gzclose(out) != Z_OK)
            fail = 1;
        break;
    }
#endif
#if HAVE_LIBLZMA
    case D99_CFMT_XZ: {
        lzma_stream xs = LZMA_STREAM_INIT;
        FILE *out;
        if (lzma_easy_encoder(&xs, 6, LZMA_CHECK_CRC32) != LZMA_OK) {
            fclose(in);
            return -1;
        }
        out = fopen(dst, "wb");
        if (!out) {
            lzma_end(&xs);
            fclose(in);
            return -1;
        }
        for (;;) {
            size_t got = fread(buf, 1, sizeof buf, in);
            xs.next_in = buf;
            xs.avail_in = got;
            if (got == 0)
                break;
            for (;;) {
                unsigned char obuf[65536];
                lzma_ret r;
                xs.next_out = obuf;
                xs.avail_out = sizeof obuf;
                r = lzma_code(&xs, LZMA_RUN);
                if (fwrite(obuf, 1, sizeof obuf - xs.avail_out, out) !=
                    sizeof obuf - xs.avail_out)
                    fail = 1;
                if (r != LZMA_OK)
                    break;
                if (xs.avail_in == 0)
                    break;
            }
            if (fail)
                break;
        }
        for (;;) {
            unsigned char obuf[65536];
            lzma_ret r;
            xs.next_out = obuf;
            xs.avail_out = sizeof obuf;
            r = lzma_code(&xs, LZMA_FINISH);
            if (fwrite(obuf, 1, sizeof obuf - xs.avail_out, out) !=
                sizeof obuf - xs.avail_out)
                fail = 1;
            if (r == LZMA_STREAM_END || r != LZMA_OK)
                break;
        }
        if (fflush(out) != 0 || fclose(out) != 0)
            fail = 1;
        lzma_end(&xs);
        break;
    }
#endif
#if HAVE_ZSTD
    case D99_CFMT_ZST: {
        ZSTD_CStream *zcs = ZSTD_createCStream();
        FILE *out;
        if (!zcs || ZSTD_isError(ZSTD_initCStream(zcs, 3))) {
            if (zcs)
                ZSTD_freeCStream(zcs);
            fclose(in);
            return -1;
        }
        out = fopen(dst, "wb");
        if (!out) {
            ZSTD_freeCStream(zcs);
            fclose(in);
            return -1;
        }
        for (;;) {
            size_t got = fread(buf, 1, sizeof buf, in);
            ZSTD_inBuffer zib;
            zib.src = buf;
            zib.size = got;
            zib.pos = 0;
            if (got == 0)
                break;
            for (;;) {
                unsigned char obuf[65536];
                ZSTD_outBuffer zob;
                size_t r;
                zob.dst = obuf;
                zob.size = sizeof obuf;
                zob.pos = 0;
                r = ZSTD_compressStream(zcs, &zob, &zib);
                if (ZSTD_isError(r)) {
                    fail = 1;
                    break;
                }
                if (fwrite(obuf, 1, zob.pos, out) != zob.pos)
                    fail = 1;
                if (zib.pos == zib.size || fail)
                    break;
            }
            if (fail)
                break;
        }
        for (;;) {
            unsigned char obuf[65536];
            ZSTD_outBuffer zob;
            size_t r;
            zob.dst = obuf;
            zob.size = sizeof obuf;
            zob.pos = 0;
            r = ZSTD_endStream(zcs, &zob);
            if (fwrite(obuf, 1, zob.pos, out) != zob.pos)
                fail = 1;
            if (r == 0 || ZSTD_isError(r))
                break;
        }
        if (fflush(out) != 0 || fclose(out) != 0)
            fail = 1;
        ZSTD_freeCStream(zcs);
        break;
    }
#endif
    default:
        fail = 1;
        break;
    }
    fclose(in);
    if (fail) {
        unlink(dst);
        return -1;
    }
    return 0;
}
