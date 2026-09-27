#include "d99_archive.h"
#include "d99_db.h"
#include "d99_fallback.h"
#include "d99_util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void usage(void)
{
    fputs(
"Usage: d99-deb [<option>...] <command>\n"
"\n"
"Commands:\n"
"  -b|--build <dir> [<deb>]     build a package from a staged tree\n"
"  -c|--contents <deb>          list package contents (tar -tvf style)\n"
"  -f|--field <deb> [<field>...]  show control fields\n"
"  -e|--control <deb> <dir>     extract control files to a directory\n"
"  -x|--extract <deb> <dir>     extract filesystem data\n"
"  -X|--vextract <deb> <dir>    extract, listing files as they go\n"
"  -I|--info <deb> [<ctrlfile>...]  show control information\n"
"  --ctrl-tarfile <deb>         dump control.tar to stdout\n"
"  --fsys-tarfile <deb>         dump data.tar to stdout\n"
"\n"
"Options:\n"
"  -Z|--compression <none|gzip|xz|zstd>  build compression\n"
"  --verbose                    chatty operation\n"
"  --version                    show version\n"
"  --help                       this message\n",
        stdout);
}

/* ---------- helpers ---------- */

static void norm_name(const char *in, char *out, size_t outn)
{
    const char *p = in;
    size_t n;

    while (p[0] == '.' && p[1] == '/')
        p += 2;
    while (*p == '/')
        p++;
    n = strlen(p);
    if (n >= outn)
        n = outn - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static int open_deb(const char *path, d99_ar **ar_out,
                    d99_ar_member *ctrl, d99_ar_member *data)
{
    d99_ar *ar = d99_ar_open_read(path);
    d99_ar_member m;
    int have_ctrl = 0, have_data = 0, have_ver = 0;

    if (!ar) {
        fprintf(stderr, "d99-deb: cannot open archive '%s'\n", path);
        return -1;
    }
    while (d99_ar_next(ar, &m) == 1) {
        if (strcmp(m.name, "debian-binary") == 0)
            have_ver = 1;
        else if (strncmp(m.name, "control.tar", 11) == 0) {
            *ctrl = m;
            have_ctrl = 1;
        } else if (strncmp(m.name, "data.tar", 8) == 0) {
            *data = m;
            have_data = 1;
        }
        if (have_ver && have_ctrl && have_data)
            break;
    }
    if (!have_ver || !have_ctrl || !have_data) {
        fprintf(stderr, "d99-deb: '%s' is not a valid debian package\n", path);
        d99_ar_close(ar);
        return -1;
    }
    *ar_out = ar;
    return 0;
}

static d99_decomp *open_member_decomp(d99_ar *ar, const d99_ar_member *m)
{
    return d99_decomp_open_region(d99_ar_stream(ar), m->data_off, m->size, -1);
}

/* Read a named control-archive member into memory; NULL if absent. */
static char *read_ctrl_member(const char *deb, const char *want, size_t *len)
{
    d99_ar *ar;
    d99_ar_member ctrl, data;
    d99_decomp *d;
    d99_tarr *t;
    d99_tar_member m;
    char *result = NULL;
    char name[4096];

    if (open_deb(deb, &ar, &ctrl, &data) != 0)
        return NULL;
    d = open_member_decomp(ar, &ctrl);
    if (!d) {
        d99_ar_close(ar);
        return NULL;
    }
    t = d99_tar_open_decomp(d);
    while (d99_tar_next(t, &m) == 1) {
        norm_name(m.name, name, sizeof name);
        if (strcmp(name, want) == 0 && (m.typeflag == '0' || m.typeflag == '\0')) {
            result = d99_xmalloc((size_t)m.size + 1);
            if (d99_tar_read_data(t, result, (size_t)m.size) == 0) {
                result[m.size] = '\0';
                if (len)
                    *len = (size_t)m.size;
            } else {
                free(result);
                result = NULL;
            }
            break;
        }
        d99_tar_skip(t);
    }
    d99_tar_close_read(t);
    d99_ar_close(ar);
    return result;
}

static int default_fmt(void)
{
    if (d99_comp_support(D99_CFMT_XZ))
        return D99_CFMT_XZ;
    if (d99_comp_support(D99_CFMT_GZ))
        return D99_CFMT_GZ;
    return D99_CFMT_NONE;
}

static int parse_fmt(const char *s)
{
    if (strcmp(s, "none") == 0 || strcmp(s, "uncompressed") == 0)
        return D99_CFMT_NONE;
    if (strcmp(s, "gzip") == 0 || strcmp(s, "gz") == 0)
        return D99_CFMT_GZ;
    if (strcmp(s, "xz") == 0)
        return D99_CFMT_XZ;
    if (strcmp(s, "zstd") == 0 || strcmp(s, "zst") == 0)
        return D99_CFMT_ZST;
    return -1;
}

static int rm_rf(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;

    if (!d)
        return unlink(path);
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        {
            char *full = d99_path_join(path, de->d_name);
            struct stat st;
            if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode))
                rm_rf(full);
            else
                unlink(full);
            free(full);
        }
    }
    closedir(d);
    return rmdir(path);
}

/* ---------- commands ---------- */

static int cmd_build(const char *dir, const char *out_opt, int fmt)
{
    char *ctrlpath = d99_path_join(dir, "DEBIAN/control");
    size_t textlen = 0;
    char *text;
    d99_arena *ar;
    d99_pkg *p;
    char tmp[] = "/tmp/d99-deb.XXXXXX";
    char *tmpdir;
    char *data_tar, *ctrl_tar, *data_c, *ctrl_c;
    const char *ext;
    FILE *f;
    d99_tarw *tw;
    d99_ar *out;
    char *outpath;
    int rc = 0;

    text = d99_read_file(ctrlpath, &textlen);
    free(ctrlpath);    if (!text) {
        fprintf(stderr, "d99-deb: cannot read %s/DEBIAN/control\n", dir);
        return 1;
    }
    ar = d99_arena_new();
    p = d99_pkg_parse_stanza(text, textlen, ar);
    if (!p || !p->name || !p->version || !*p->version) {
        fprintf(stderr, "d99-deb: control file lacks Package or Version\n");
        return 1;
    }
    if (strchr(p->name, ' ') || p->name[0] == '\0') {
        fprintf(stderr, "d99-deb: invalid package name '%s'\n", p->name);
        return 1;
    }

    tmpdir = mkdtemp(tmp);
    if (!tmpdir) {
        fprintf(stderr, "d99-deb: cannot create temp dir\n");
        return 1;
    }
    data_tar = d99_path_join(tmpdir, "data.tar");
    ctrl_tar = d99_path_join(tmpdir, "control.tar");
    ext = d99_comp_ext(fmt);
    data_c = d99_xasprintf("%s%s", data_tar, ext);
    ctrl_c = d99_xasprintf("%s%s", ctrl_tar, ext);

    /* data.tar: staged tree minus DEBIAN */
    f = fopen(data_tar, "wb");
    if (!f) {
        fprintf(stderr, "d99-deb: cannot write %s\n", data_tar);
        return 1;
    }
    tw = d99_tar_open_write(f);
    d99_tarw_add_dir(tw, "./", 0755);
    {
        DIR *d = opendir(dir);
        struct dirent *de;
        d99_strvec kids;
        size_t i;
        if (!d) {
            fprintf(stderr, "d99-deb: cannot open %s\n", dir);
            return 1;
        }
        d99_sv_init(&kids);
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 ||
                strcmp(de->d_name, "DEBIAN") == 0)
                continue;
            d99_sv_push(&kids, de->d_name);
        }
        closedir(d);
        d99_sv_sort(&kids);
        for (i = 0; i < kids.n; i++) {
            char *fs = d99_path_join(dir, kids.v[i]);
            char *arc = d99_xasprintf("./%s", kids.v[i]);
            d99_tarw_add_path(tw, fs, arc);
            free(fs);
            free(arc);
        }
        d99_sv_free(&kids);
    }
    d99_tarw_close(tw);
    fclose(f);

    /* control.tar: the DEBIAN directory */
    f = fopen(ctrl_tar, "wb");
    if (!f) {
        fprintf(stderr, "d99-deb: cannot write %s\n", ctrl_tar);
        return 1;
    }
    tw = d99_tar_open_write(f);
    {
        char *debiandir = d99_path_join(dir, "DEBIAN");
        d99_tarw_add_path(tw, debiandir, "./");
        free(debiandir);
    }
    d99_tarw_close(tw);
    fclose(f);

    /* compress */
    if (fmt != D99_CFMT_NONE) {
        if (d99_compress_file(data_tar, data_c, fmt) != 0 ||
            d99_compress_file(ctrl_tar, ctrl_c, fmt) != 0) {
            fprintf(stderr, "d99-deb: compression failed (%s)\n", d99_comp_name(fmt));
            return 1;
        }
    }

    if (out_opt)
        outpath = d99_xstrdup(out_opt);
    else
        outpath = d99_xasprintf("%s_%s_%s.deb", p->name, p->version,
                               p->arch && *p->arch ? p->arch : "all");

    out = d99_ar_open_write(outpath);
    if (!out) {
        fprintf(stderr, "d99-deb: cannot create %s\n", outpath);
        return 1;
    }
    d99_ar_add_mem(out, "debian-binary", "2.0\n", 4);
    {
        char *mname = d99_xasprintf("control.tar%s", ext);
        d99_ar_add_file(out, mname, fmt == D99_CFMT_NONE ? ctrl_tar : ctrl_c);
        free(mname);
        mname = d99_xasprintf("data.tar%s", ext);
        d99_ar_add_file(out, mname, fmt == D99_CFMT_NONE ? data_tar : data_c);
        free(mname);
    }
    d99_ar_close(out);
    printf("d99-deb: built package '%s'\n", outpath);

    free(data_tar);
    free(ctrl_tar);
    free(data_c);
    free(ctrl_c);
    free(outpath);
    rm_rf(tmpdir);
    d99_arena_free(ar);
    free(text);
    return rc;
}

static void mode_str(char *buf, size_t bufn, unsigned long mode, char type)
{
    static const char rwx[] = "rwxrwxrwx";
    size_t i, o = 0;

    buf[o++] = type == '5' ? 'd' : type == '2' ? 'l' : type == '6' ? 'p' :
               type == '3' ? 'c' : type == '4' ? 'b' : '-';
    for (i = 0; i < 9; i++)
        buf[o++] = (mode & (1u << (8 - (int)i))) ? rwx[i] : '-';
    buf[o] = '\0';
    (void)bufn;
}

static int cmd_contents(const char *deb)
{
    d99_ar *ar;
    d99_ar_member ctrl, data;
    d99_decomp *d;
    d99_tarr *t;
    d99_tar_member m;
    int rc = 0;

    if (open_deb(deb, &ar, &ctrl, &data) != 0)
        return 1;
    d = open_member_decomp(ar, &data);
    if (!d) {
        fprintf(stderr, "d99-deb: cannot open data member of %s\n", deb);
        d99_ar_close(ar);
        return 1;
    }
    t = d99_tar_open_decomp(d);
    while (d99_tar_next(t, &m) == 1) {
        char ms[16];
        char name[4096];
        struct tm *tm;
        time_t tt = (time_t)m.mtime;

        tm = localtime(&tt);
        norm_name(m.name, name, sizeof name);
        if (!name[0]) {
            d99_tar_skip(t);
            continue;
        }
        mode_str(ms, sizeof ms, m.mode, m.typeflag);
        printf("%s %s/%s %8lld ", ms,
               m.uname[0] ? m.uname : "root", m.gname[0] ? m.gname : "root",
               m.size);
        if (tm)
            printf("%04d-%02d-%02d %02d:%02d ", tm->tm_year + 1900,
                   tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
        printf("%s", name);
        if (m.typeflag == '2')
            printf(" -> %s", m.linkname);
        printf("\n");
        d99_tar_skip(t);
    }
    d99_tar_close_read(t);
    d99_ar_close(ar);
    return rc;
}

static int cmd_extract(const char *deb, const char *dir, int verbose)
{
    d99_ar *ar;
    d99_ar_member ctrl, data;
    d99_decomp *d;
    d99_tarr *t;
    d99_tar_extract_opts o;
    long n;

    if (d99_mkdir_p(dir, 0755) != 0) {
        fprintf(stderr, "d99-deb: cannot create directory %s\n", dir);
        return 1;
    }
    if (open_deb(deb, &ar, &ctrl, &data) != 0)
        return 1;
    d = open_member_decomp(ar, &data);
    if (!d) {
        fprintf(stderr, "d99-deb: cannot open data member of %s\n", deb);
        d99_ar_close(ar);
        return 1;
    }
    t = d99_tar_open_decomp(d);
    memset(&o, 0, sizeof o);
    o.dest = dir;
    o.verbose = verbose;
    o.keep_going = 0;
    n = d99_tar_extract(t, &o);
    d99_tar_close_read(t);
    d99_ar_close(ar);
    if (n < 0) {
        fprintf(stderr, "d99-deb: extraction of %s failed\n", deb);
        return 1;
    }
    return 0;
}

static int cmd_control(const char *deb, const char *dir)
{
    d99_ar *ar;
    d99_ar_member ctrl, data;
    d99_decomp *d;
    d99_tarr *t;
    d99_tar_member m;

    if (d99_mkdir_p(dir, 0755) != 0) {
        fprintf(stderr, "d99-deb: cannot create directory %s\n", dir);
        return 1;
    }
    if (open_deb(deb, &ar, &ctrl, &data) != 0)
        return 1;
    d = open_member_decomp(ar, &ctrl);
    if (!d) {
        d99_ar_close(ar);
        return 1;
    }
    t = d99_tar_open_decomp(d);
    while (d99_tar_next(t, &m) == 1) {
        char name[4096];
        char *dst;
        FILE *out;
        unsigned char *buf;

        norm_name(m.name, name, sizeof name);
        if (!name[0])
            continue;
        if (m.typeflag == '5') {
            d99_mkdir_p(dst = d99_path_join(dir, name), 0755);
            free(dst);
            continue;
        }
        if (m.typeflag != '0' && m.typeflag != '\0') {
            d99_tar_skip(t);
            continue;
        }
        dst = d99_path_join(dir, name);
        d99_ensure_parent(dst, 0755);
        out = fopen(dst, "wb");
        if (!out) {
            fprintf(stderr, "d99-deb: cannot write %s\n", dst);
            free(dst);
            return 1;
        }
        buf = d99_xmalloc((size_t)m.size ? (size_t)m.size : 1);
        if (d99_tar_read_data(t, buf, (size_t)m.size) == 0)
            fwrite(buf, 1, (size_t)m.size, out);
        free(buf);
        fclose(out);
        chmod(dst, (mode_t)(m.mode & 07777));
        free(dst);
    }
    d99_tar_close_read(t);
    d99_ar_close(ar);
    return 0;
}

static int cmd_info(const char *deb, char **files, int nfiles)
{
    d99_ar *ar;
    d99_ar_member ctrl, data;
    d99_decomp *d;
    d99_tarr *t;
    d99_tar_member m;
    int i;
    int rc = 0;

    if (open_deb(deb, &ar, &ctrl, &data) != 0)
        return 1;
    printf(" new debian package, version 2.0.\n");
    printf(" size %lld bytes: control archive=%lld bytes.\n", data.size, ctrl.size);

    d = open_member_decomp(ar, &ctrl);
    if (!d) {
        d99_ar_close(ar);
        return 1;
    }
    t = d99_tar_open_decomp(d);
    while (d99_tar_next(t, &m) == 1) {
        char name[4096];
        int wanted = 0;

        norm_name(m.name, name, sizeof name);
        if (!name[0] || (m.typeflag != '0' && m.typeflag != '\0')) {
            d99_tar_skip(t);
            continue;
        }
        if (nfiles == 0) {
            wanted = strcmp(name, "control") == 0;
        } else {
            for (i = 0; i < nfiles; i++)
                if (strcmp(files[i], name) == 0)
                    wanted = 1;
        }
        if (!wanted) {
            d99_tar_skip(t);
            continue;
        }
        {
            unsigned char *buf = d99_xmalloc((size_t)m.size ? (size_t)m.size : 1);
            long long lines = 0;
            long long k;
            if (d99_tar_read_data(t, buf, (size_t)m.size) == 0) {
                for (k = 0; k < m.size; k++)
                    if (buf[k] == '\n')
                        lines++;
                printf(" %8lld bytes, %6lld lines   %s\n", m.size, lines, name);
                fwrite(buf, 1, (size_t)m.size, stdout);
                if (m.size && buf[m.size - 1] != '\n')
                    printf("\n");
            }
            free(buf);
        }
    }
    d99_tar_close_read(t);
    d99_ar_close(ar);
    return rc;
}

static int cmd_field(const char *deb, char **fields, int nfields)
{
    size_t len = 0;
    char *text = read_ctrl_member(deb, "control", &len);
    d99_arena *ar;
    d99_pkg *p;
    int i;
    int rc = 0;

    if (!text) {
        fprintf(stderr, "d99-deb: cannot read control of %s\n", deb);
        return 1;
    }
    if (nfields == 0) {
        fwrite(text, 1, len, stdout);
        free(text);
        return 0;
    }
    ar = d99_arena_new();
    p = d99_pkg_parse_stanza(text, len, ar);
    if (!p) {
        fprintf(stderr, "d99-deb: malformed control stanza in %s\n", deb);
        free(text);
        return 1;
    }
    for (i = 0; i < nfields; i++) {
        char *v = d99_pkg_field_dup(p, fields[i]);
        if (v) {
            printf("%s\n", v);
            free(v);
        } else {
            fprintf(stderr, "d99-deb: field '%s' not present\n", fields[i]);
            rc = 1;
        }
    }
    d99_arena_free(ar);
    free(text);
    return rc;
}

static int cmd_tarfile(const char *deb, int want_ctrl)
{
    d99_ar *ar;
    d99_ar_member ctrl, data;
    d99_ar_member *sel;
    d99_decomp *d;
    unsigned char buf[65536];
    long r;

    if (open_deb(deb, &ar, &ctrl, &data) != 0)
        return 1;
    sel = want_ctrl ? &ctrl : &data;
    d = open_member_decomp(ar, sel);
    if (!d) {
        d99_ar_close(ar);
        return 1;
    }
    while ((r = d99_decomp_read(d, buf, sizeof buf)) > 0)
        fwrite(buf, 1, (size_t)r, stdout);
    d99_decomp_close(d);
    d99_ar_close(ar);
    return 0;
}

/* ---------- main ---------- */

enum action {
    A_NONE, A_BUILD, A_EXTRACT, A_VEXTRACT, A_CONTENTS, A_INFO,
    A_FIELD, A_CONTROL, A_CTRL_TAR, A_FSYS_TAR
};

int main(int argc, char **argv)
{
    enum action act = A_NONE;
    int fmt = default_fmt();
    const char *compress_arg = NULL;
    d99_strvec ops;
    int i, rc;

    d99_sv_init(&ops);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
            /* "--": everything after is an operand */
            for (i++; i < argc; i++)
                d99_sv_push(&ops, argv[i]);
            break;
        }
        if (a[0] == '-' && a[1]) {
            if (a[1] == '-') {
                if (strcmp(a, "--build") == 0)
                    act = A_BUILD;
                else if (strcmp(a, "--extract") == 0)
                    act = A_EXTRACT;
                else if (strcmp(a, "--vextract") == 0)
                    act = A_VEXTRACT;
                else if (strcmp(a, "--contents") == 0)
                    act = A_CONTENTS;
                else if (strcmp(a, "--info") == 0)
                    act = A_INFO;
                else if (strcmp(a, "--field") == 0)
                    act = A_FIELD;
                else if (strcmp(a, "--control") == 0)
                    act = A_CONTROL;
                else if (strcmp(a, "--ctrl-tarfile") == 0)
                    act = A_CTRL_TAR;
                else if (strcmp(a, "--fsys-tarfile") == 0)
                    act = A_FSYS_TAR;
                else if (strcmp(a, "--verbose") == 0)
                    d99_set_verbose(1);
                else if (strcmp(a, "--version") == 0) {
                    printf("d99-deb (d99) %s\n", D99_VERSION);
                    return 0;
                } else if (strcmp(a, "--help") == 0) {
                    usage();
                    return 0;
                } else if (strcmp(a, "--compression") == 0) {
                    if (i + 1 >= argc) {
                        fprintf(stderr, "d99-deb: --compression needs a value\n");
                        return 2;
                    }
                    compress_arg = argv[++i];
                } else if (strncmp(a, "--compression=", 14) == 0) {
                    compress_arg = a + 14;
                } else if (strcmp(a, "--raw-extract") == 0 ||
                           strcmp(a, "--showinfo") == 0) {
                    /* recognized upstream options we don't implement */
                    d99_fallback_or_die(argv[0], argv);
                } else {
                    d99_fallback_or_die(argv[0], argv);
                }
            } else {
                const char *p;
                for (p = a + 1; *p; p++) {
                    switch (*p) {
                    case 'b': act = A_BUILD; break;
                    case 'x': act = A_EXTRACT; break;
                    case 'X': act = A_VEXTRACT; break;
                    case 'c': act = A_CONTENTS; break;
                    case 'I': act = A_INFO; break;
                    case 'f': act = A_FIELD; break;
                    case 'e': act = A_CONTROL; break;
                    case 'v': d99_set_verbose(1); break;
                    case 'Z':
                        if (p[1]) {
                            compress_arg = p + 1;
                        } else if (i + 1 < argc) {
                            compress_arg = argv[++i];
                        } else {
                            fprintf(stderr, "d99-deb: -Z needs a value\n");
                            return 2;
                        }
                        p = "\0";
                        break;
                    default:
                        d99_fallback_or_die(argv[0], argv);
                    }
                    if (*p == '\0')
                        break;
                }
            }
        } else {
            d99_sv_push(&ops, a);
        }
    }

    if (compress_arg) {
        fmt = parse_fmt(compress_arg);
        if (fmt < 0) {
            fprintf(stderr, "d99-deb: unknown compression '%s'\n", compress_arg);
            return 2;
        }
        if (!d99_comp_support(fmt)) {
            fprintf(stderr, "d99-deb: compression '%s' not compiled in "
                    "(install the dev headers and rebuild)\n",
                    d99_comp_name(fmt));
            return 1;
        }
    }

    rc = 0;
    switch (act) {
    case A_BUILD:
        if (ops.n < 1 || ops.n > 2) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_build(ops.v[0], ops.n == 2 ? ops.v[1] : NULL, fmt);
        break;
    case A_EXTRACT:
    case A_VEXTRACT:
        if (ops.n != 2) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_extract(ops.v[0], ops.v[1], act == A_VEXTRACT);
        break;
    case A_CONTENTS:
        if (ops.n < 1) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_contents(ops.v[0]);
        break;
    case A_INFO:
        if (ops.n < 1) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_info(ops.v[0], ops.v + 1, (int)ops.n - 1);
        break;
    case A_FIELD:
        if (ops.n < 1) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_field(ops.v[0], ops.v + 1, (int)ops.n - 1);
        break;
    case A_CONTROL:
        if (ops.n != 2) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_control(ops.v[0], ops.v[1]);
        break;
    case A_CTRL_TAR:
    case A_FSYS_TAR:
        if (ops.n != 1) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_tarfile(ops.v[0], act == A_CTRL_TAR);
        break;
    default:
        usage();
        rc = 2;
        break;
    }
    d99_sv_free(&ops);
    return rc;
}
