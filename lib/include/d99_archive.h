#ifndef D99_ARCHIVE_H
#define D99_ARCHIVE_H

#include <stdio.h>
#include <stdint.h>
#include "d99_util.h"

/* ar */
typedef struct d99_ar d99_ar;

typedef struct {
    char      name[256];
    long long size;
    long long data_off;   /* absolute offset of the member payload */
} d99_ar_member;

d99_ar *d99_ar_open_read(const char *path);
d99_ar *d99_ar_open_write(const char *path);
/* Iterate members.  After a successful call the underlying FILE* is
 * positioned at the member payload.  1=found, 0=eof, -1=error. */
int   d99_ar_next(d99_ar *ar, d99_ar_member *m);
FILE *d99_ar_stream(d99_ar *ar);
/* Copy a member payload out to a file. */
int   d99_ar_extract_member(d99_ar *ar, const d99_ar_member *m, const char *dest);
int   d99_ar_add_file(d99_ar *ar, const char *name, const char *src);
int   d99_ar_add_mem(d99_ar *ar, const char *name, const void *data, size_t len);
int   d99_ar_close(d99_ar *ar);

/* ===================== compression ===================== */
enum { D99_CFMT_NONE = 0, D99_CFMT_GZ, D99_CFMT_XZ, D99_CFMT_ZST };

int         d99_comp_support(int fmt);       /* 1 if compiled in */
int         d99_sniff(const void *magic, size_t n);
const char *d99_comp_ext(int fmt);           /* "", ".gz", ".xz", ".zst" */
const char *d99_comp_name(int fmt);          /* "none", "gzip", "xz", "zstd" */

typedef struct d99_decomp d99_decomp;

d99_decomp *d99_decomp_open(const char *path);   /* sniffs format */
/* Open a compressed stream starting at byte offset `off' of an open FILE*.
 * `size' bounds D99_CFMT_NONE regions; codecs terminate on their own frame
 * footer.  fmt < 0 means sniff. */
d99_decomp *d99_decomp_open_region(FILE *f, long long off, long long size, int fmt);
long        d99_decomp_read(d99_decomp *d, void *buf, size_t n); /* >0 bytes, 0=eof, -1=err */
char       *d99_decomp_slurp(d99_decomp *d, size_t *len);        /* whole stream */
void        d99_decomp_close(d99_decomp *d);
int         d99_compress_file(const char *src, const char *dst, int fmt);

/* ===================== tar ===================== */
typedef struct {
    char          name[4096];
    char          linkname[4096];
    char          uname[33], gname[33];
    long long     size;
    unsigned long mode;
    long          uid, gid;
    long long     mtime;
    char          typeflag;
} d99_tar_member;

typedef struct d99_tarr d99_tarr;

d99_tarr *d99_tar_open_read(const char *path);     /* sniffs compression */
d99_tarr *d99_tar_open_decomp(d99_decomp *d);      /* takes ownership of d */
int  d99_tar_next(d99_tarr *t, d99_tar_member *m); /* 1=member, 0=eof, -1=err */
int  d99_tar_read_data(d99_tarr *t, void *buf, size_t n);
int  d99_tar_skip(d99_tarr *t);
void d99_tar_close_read(d99_tarr *t);

typedef struct d99_tarw d99_tarw;

d99_tarw *d99_tar_open_write(FILE *f);
int  d99_tarw_add_file(d99_tarw *tw, const char *fs_path, const char *arc_name,
                       unsigned long mode /* 0 = use fs mode */);
int  d99_tarw_add_dir(d99_tarw *tw, const char *arc_name, unsigned long mode);
int  d99_tarw_add_symlink(d99_tarw *tw, const char *arc_name, const char *target);
int  d99_tarw_add_data(d99_tarw *tw, const char *arc_name, const void *data,
                       size_t len, unsigned long mode);
/* Recursively add a filesystem path under an archive prefix ("./"). */
int  d99_tarw_add_path(d99_tarw *tw, const char *fs_path, const char *arc_prefix);
int  d99_tarw_close(d99_tarw *tw);   /* writes trailer; caller owns FILE */

/* extraction */
typedef struct {
    const char *dest;
    /* Optional: remap a member path (diversions / .dpkg-new conffiles).
     * May return the same buffer, a caller-owned buffer, or NULL to skip. */
    const char *(*map)(const char *path, int is_dir, void *ud);
    void *map_ud;
    /* Optional: record each installed on-disk path. */
    void (*record)(const char *ondisk_path, void *ud);
    void *record_ud;
    /* Optional: skip member entirely when this returns 1. */
    int  (*skip)(const char *path, void *ud);
    void *skip_ud;
    int verbose;
    int keep_going;   /* warn+skip on per-member errors instead of aborting */
} d99_tar_extract_opts;

/* Returns the number of extracted members, or -1 on fatal error. */
long d99_tar_extract(d99_tarr *t, const d99_tar_extract_opts *o);

#endif /* D99_ARCHIVE_H */
