#include "manifest.h"
#include "d99_archive.h"
#include "d99_db.h"
#include "d99_elf.h"
#include "d99_fallback.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(void)
{
    fputs(
"Usage: d99-build [<option>...] [<manifest>]\n"
"\n"
"Options:\n"
"  -f|--manifest <d99.ini>     manifest to read (default ./d99.ini)\n"
"  -o|--output <file.deb>      output path\n"
"  -Z|--compression <fmt>       none|gzip|xz|zstd\n"
"  --no-auto-depends           disable ELF DT_NEEDED inference\n"
"  --version | --help\n",
        stdout);
}

/* ---------- soname -> owning package ---------- */

typedef struct {
    char *path;
    char *owner;
} owned;

/* Build a path -> owner map from the dpkg info .list files (lazily, once). */
static struct {
    owned *v;
    size_t n, cap;
    int built;
} g_ownermap;

static int ownermap_build(void)
{
    const char *admindir = d99_default_admindir();
    char *infodir = d99_path_join(admindir, "info");
    DIR *d = opendir(infodir);
    struct dirent *de;

    free(infodir);
    if (g_ownermap.built)
        return g_ownermap.n > 0;
    g_ownermap.built = 1;
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        char *full, *text;
        const char *scan;
        char *owner;

        if (nl < 6 || strcmp(de->d_name + nl - 5, ".list") != 0)
            continue;
        owner = d99_xstrndup(de->d_name, nl - 5);
        /* dpkg names foreign/native lists as pkg:arch.list; strip arch */
        {
            char *co = strrchr(owner, ':');
            if (co && co != owner && strchr(co, '.') == NULL)
                *co = '\0';
        }
        full = d99_xasprintf("%s/info/%s", admindir, de->d_name);
        text = d99_read_file(full, NULL);
        scan = text;
        while (scan && *scan) {
            const char *eol = strchr(scan, '\n');
            size_t ln = eol ? (size_t)(eol - scan) : strlen(scan);
            char *line = d99_xstrndup(scan, ln);
            char *t = d99_trim(line);
            if (*t) {
                if (g_ownermap.n == g_ownermap.cap) {
                    g_ownermap.cap = g_ownermap.cap ? g_ownermap.cap * 2 : 1024;
                    g_ownermap.v = d99_xrealloc(g_ownermap.v,
                                                g_ownermap.cap * sizeof(owned));
                }
                g_ownermap.v[g_ownermap.n].path = d99_xstrdup(t);
                g_ownermap.v[g_ownermap.n].owner = d99_xstrdup(owner);
                g_ownermap.n++;
            }
            free(line);
            if (!eol)
                break;
            scan = eol + 1;
        }
        free(text);
        free(full);
        free(owner);
    }
    closedir(d);
    return g_ownermap.n > 0;
}

static const char *owner_of(const char *path)
{
    size_t i;
    if (!ownermap_build())
        return NULL;
    for (i = 0; i < g_ownermap.n; i++)
        if (strcmp(g_ownermap.v[i].path, path) == 0)
            return g_ownermap.v[i].owner;
    return NULL;
}

/* builtin soname -> package hints for the most common libc-level libs */
static const struct { const char *soname, *pkg; } builtin_deps[] = {
    { "libc.so.6",        "libc6" },
    { "libm.so.6",        "libc6" },
    { "libdl.so.2",       "libc6" },
    { "libpthread.so.0",  "libc6" },
    { "librt.so.1",       "libc6" },
    { "libresolv.so.2",    "libc6" },
    { "libutil.so.1",     "libc6" },
    { "libgcc_s.so.1",    "libgcc-s1" },
    { "libstdc++.so.6",   "libstdc++6" },
    { "libz.so.1",        "zlib1g" },
    { "liblzma.so.5",     "liblzma5" },
    { "libzstd.so.1",     "libzstd1" },
    { "libssl.so.3",      "libssl3" },
    { "libcrypto.so.3",   "libcrypto3" },
    { "libcrypt.so.1",    "libcrypt1" },
    { NULL, NULL }
};

/* Find the package providing a soname. */
static char *soname_provider(const char *soname)
{
    static const char *dirs[] = {
        "/usr/lib", "/usr/lib64", "/lib", "/lib64", NULL
    };
    char *triplet_dirs[8];
    size_t i, ndir = 0;
    const char *host = d99_host_arch();
    char *result = NULL;

    /* arch-triplet library dirs, e.g. /usr/lib/x86_64-linux-gnu */
    {
        char trip[64];
        if (strcmp(host, "amd64") == 0)
            snprintf(trip, sizeof trip, "x86_64-linux-gnu");
        else if (strcmp(host, "arm64") == 0)
            snprintf(trip, sizeof trip, "aarch64-linux-gnu");
        else if (strcmp(host, "i386") == 0)
            snprintf(trip, sizeof trip, "i386-linux-gnu");
        else if (strcmp(host, "armhf") == 0)
            snprintf(trip, sizeof trip, "arm-linux-gnueabihf");
        else
            snprintf(trip, sizeof trip, "%s-linux-gnu", host);
        triplet_dirs[ndir++] = d99_xasprintf("/usr/lib/%s", trip);
        triplet_dirs[ndir++] = d99_xasprintf("/lib/%s", trip);
    }
    triplet_dirs[ndir] = NULL;

    for (i = 0; dirs[i] && !result; i++) {
        char *full = d99_xasprintf("%s/%s", dirs[i], soname);
        if (d99_file_exists(full)) {
            const char *owner = owner_of(full);
            if (owner)
                result = d99_xstrdup(owner);
            free(full);
            break;
        }
        free(full);
    }
    for (i = 0; i < ndir && !result; i++) {
        char *full = d99_xasprintf("%s/%s", triplet_dirs[i], soname);
        if (d99_file_exists(full)) {
            const char *owner = owner_of(full);
            if (owner)
                result = d99_xstrdup(owner);
        }
        free(full);
    }
    for (i = 0; i < ndir; i++)
        free(triplet_dirs[i]);
    if (!result) {
        for (i = 0; builtin_deps[i].soname; i++)
            if (strcmp(builtin_deps[i].soname, soname) == 0) {
                result = d99_xstrdup(builtin_deps[i].pkg);
                break;
            }
    }
    return result;
}

/* does the user-provided Depends already mention `pkg'? */
static int deps_mention(const char *deps, const char *pkg)
{
    const char *p = deps;
    size_t n = strlen(pkg);

    if (!deps)
        return 0;
    while ((p = strstr(p, pkg)) != NULL) {
        int left_ok = (p == deps || !isalnum((unsigned char)p[-1]));
        int right_ok = (p[n] == '\0' || !isalnum((unsigned char)p[n]));
        if (left_ok && right_ok)
            return 1;
        p += n;
    }
    return 0;
}

/* ---------- staging helpers ---------- */

static long long add_tree_size(const char *fs_path)
{
    struct stat st;
    if (lstat(fs_path, &st) != 0)
        return 0;
    if (S_ISREG(st.st_mode))
        return (long long)st.st_size;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(fs_path);
        struct dirent *de;
        long long total = 0;
        if (!d)
            return 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            {
                char *full = d99_path_join(fs_path, de->d_name);
                total += add_tree_size(full);
                free(full);
            }
        }
        closedir(d);
        return total;
    }
    return 0;
}

/* collect ELF DT_NEEDED from a staged path (file or tree) */
static void collect_needed(const char *fs_path, d99_strvec *out)
{
    struct stat st;

    if (lstat(fs_path, &st) != 0)
        return;
    if (S_ISREG(st.st_mode)) {
        if (d99_elf_is_elf(fs_path)) {
            d99_strvec got;
            size_t i;
            d99_sv_init(&got);
            d99_elf_needed(fs_path, &got);
            for (i = 0; i < got.n; i++) {
                if (!d99_sv_contains(out, got.v[i]))
                    d99_sv_push_own(out, got.v[i]);
                else
                    free(got.v[i]);
            }
            free(got.v);
        }
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(fs_path);
        struct dirent *de;
        if (!d)
            return;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            {
                char *full = d99_path_join(fs_path, de->d_name);
                collect_needed(full, out);
                free(full);
            }
        }
        closedir(d);
    }
}

/* md5sums entries for a staged path (regular files only, skipping
 * conffiles); appends "hash  path\n" lines to the memstream */
static void md5_tree(FILE *out, const char *fs_path, const char *arc_base,
                     d99_strvec *conffiles)
{
    struct stat st;

    if (lstat(fs_path, &st) != 0)
        return;
    if (S_ISREG(st.st_mode)) {
        char rel[4096];
        const char *p = arc_base[0] == '/' ? arc_base + 1 : arc_base;
        snprintf(rel, sizeof rel, "%s", p);
        if (!d99_sv_contains(conffiles, rel)) {
            char hex[33];
            if (d99_md5_file(fs_path, hex) == 0)
                fprintf(out, "%s  %s\n", hex, rel);
        }
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(fs_path);
        struct dirent *de;
        if (!d)
            return;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            {
                char *full = d99_path_join(fs_path, de->d_name);
                char *arc = d99_xasprintf("%s/%s", arc_base, de->d_name);
                md5_tree(out, full, arc, conffiles);
                free(arc);
                free(full);
            }
        }
        closedir(d);
    }
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    const char *manifest_path = "d99.ini";
    const char *out_opt = NULL;
    int fmt = D99_CFMT_GZ;
    int no_auto = 0;
    d99_manifest m;
    int i, rc;
    char *outpath;
    char tmp[] = "/tmp/d99-build.XXXXXX";
    char *tmpdir;
    FILE *f;
    d99_tarw *tw;
    d99_ar *ar;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
            for (i++; i < argc; i++)
                manifest_path = argv[i];
            break;
        }
        if (a[0] == '-' && a[1]) {
            if (a[1] == '-') {
                if (strcmp(a, "--manifest") == 0 && i + 1 < argc)
                    manifest_path = argv[++i];
                else if (strncmp(a, "--manifest=", 11) == 0)
                    manifest_path = a + 11;
                else if (strcmp(a, "--output") == 0 && i + 1 < argc)
                    out_opt = argv[++i];
                else if (strncmp(a, "--output=", 9) == 0)
                    out_opt = a + 9;
                else if (strcmp(a, "--compression") == 0 && i + 1 < argc) {
                    const char *v = argv[++i];
                    if (strcmp(v, "none") == 0)
                        fmt = D99_CFMT_NONE;
                    else if (strcmp(v, "gzip") == 0 || strcmp(v, "gz") == 0)
                        fmt = D99_CFMT_GZ;
                    else if (strcmp(v, "xz") == 0)
                        fmt = D99_CFMT_XZ;
                    else if (strcmp(v, "zstd") == 0 || strcmp(v, "zst") == 0)
                        fmt = D99_CFMT_ZST;
                    else {
                        fprintf(stderr, "d99-build: unknown compression %s\n", v);
                        return 2;
                    }
                } else if (strcmp(a, "--no-auto-depends") == 0) {
                    no_auto = 1;
                } else if (strcmp(a, "--version") == 0) {
                    printf("d99-build (d99) %s\n", D99_VERSION);
                    return 0;
                } else if (strcmp(a, "--help") == 0) {
                    usage();
                    return 0;
                } else {
                    d99_fallback_or_die(argv[0], argv);
                }
            } else {
                const char *p;
                for (p = a + 1; *p; p++) {
                    switch (*p) {
                    case 'f':
                        if (i + 1 >= argc) {
                            fprintf(stderr, "d99-build: -f needs a value\n");
                            return 2;
                        }
                        manifest_path = argv[++i];
                        break;
                    case 'o':
                        if (i + 1 >= argc) {
                            fprintf(stderr, "d99-build: -o needs a value\n");
                            return 2;
                        }
                        out_opt = argv[++i];
                        break;
                    case 'Z':
                        if (p[1]) {
                            const char *v = p + 1;
                            p = "\0";
                            if (strcmp(v, "none") == 0)
                                fmt = D99_CFMT_NONE;
                            else if (strcmp(v, "gzip") == 0 || strcmp(v, "gz") == 0)
                                fmt = D99_CFMT_GZ;
                            else if (strcmp(v, "xz") == 0)
                                fmt = D99_CFMT_XZ;
                            else if (strcmp(v, "zstd") == 0 || strcmp(v, "zst") == 0)
                                fmt = D99_CFMT_ZST;
                            else {
                                fprintf(stderr, "d99-build: unknown compression %s\n", v);
                                return 2;
                            }
                        } else if (i + 1 < argc) {
                            const char *v = argv[++i];
                            if (strcmp(v, "none") == 0)
                                fmt = D99_CFMT_NONE;
                            else if (strcmp(v, "gzip") == 0 || strcmp(v, "gz") == 0)
                                fmt = D99_CFMT_GZ;
                            else if (strcmp(v, "xz") == 0)
                                fmt = D99_CFMT_XZ;
                            else if (strcmp(v, "zstd") == 0 || strcmp(v, "zst") == 0)
                                fmt = D99_CFMT_ZST;
                            else {
                                fprintf(stderr, "d99-build: unknown compression %s\n", v);
                                return 2;
                            }
                        }
                        break;
                    case 'v':
                        d99_set_verbose(1);
                        break;
                    default:
                        d99_fallback_or_die(argv[0], argv);
                    }
                    if (*p == '\0')
                        break;
                }
            }
        } else {
            manifest_path = a;
        }
    }

    if (!d99_comp_support(fmt)) {
        fprintf(stderr, "d99-build: compression %s not compiled in\n",
                d99_comp_name(fmt));
        return 1;
    }
    if (d99_manifest_load(manifest_path, &m) != 0)
        return 1;

    if (!m.arch || strcmp(m.arch, "auto") == 0)
        m.arch = (char *)d99_host_arch();

    tmpdir = mkdtemp(tmp);
    if (!tmpdir) {
        fprintf(stderr, "d99-build: cannot create temp dir\n");
        return 1;
    }

    /* ---- ELF auto-depends ---- */
    if (!no_auto) {
        d99_strvec needed;
        size_t k;
        d99_sv_init(&needed);
        for (k = 0; k < m.files_src.n; k++)
            collect_needed(m.files_src.v[k], &needed);
        for (k = 0; k < needed.n; k++) {
            char *prov = soname_provider(needed.v[k]);
            if (prov) {
                if (!deps_mention(m.depends, prov)) {
                    d99_verbose("auto-dependency: %s needs %s (%s)",
                                m.name ? m.name : "package", prov, needed.v[k]);
                    m.depends = d99_xasprintf("%s%s%s", m.depends ? m.depends : "",
                                               m.depends ? ", " : "", prov);
                }
                free(prov);
            } else {
                fprintf(stderr, "d99-build: warning: cannot resolve provider "
                        "for DT_NEEDED '%s'\n", needed.v[k]);
            }
        }
        d99_sv_free(&needed);
    }

    /* ---- data tar ---- */
    {
        char *data_tar = d99_path_join(tmpdir, "data.tar");
        const char *ext = d99_comp_ext(fmt);
        char *data_c = d99_xasprintf("%s%s", data_tar, ext);
        size_t k;
        long long total = 0;

        f = fopen(data_tar, "wb");
        if (!f) {
            fprintf(stderr, "d99-build: cannot write %s\n", data_tar);
            return 1;
        }
        tw = d99_tar_open_write(f);
        d99_tarw_add_dir(tw, "./", 0755);
        for (k = 0; k < m.files_dst.n; k++) {
            const char *dst = m.files_dst.v[k];
            const char *src = m.files_src.v[k];
            char *arc = d99_xasprintf("./%s", dst[0] == '/' ? dst + 1 : dst);
            struct stat st;

            /* strip trailing slash on dst for files */
            {
                size_t al = strlen(arc);
                if (al > 2 && arc[al - 1] == '/')
                    arc[al - 1] = '\0';
            }
            if (lstat(src, &st) != 0) {
                fprintf(stderr, "d99-build: missing source %s for %s\n",
                        src, dst);
                return 1;
            }
            if (S_ISDIR(st.st_mode))
                d99_tarw_add_path(tw, src, arc);
            else if (S_ISREG(st.st_mode))
                d99_tarw_add_file(tw, src, arc, 0);
            else if (S_ISLNK(st.st_mode)) {
                char target[4096];
                ssize_t n = readlink(src, target, sizeof target - 1);
                if (n >= 0) {
                    target[n] = '\0';
                    d99_tarw_add_symlink(tw, arc, target);
                }
            }
            total += add_tree_size(src);
            free(arc);
        }
        d99_tarw_close(tw);
        fclose(f);
        if (fmt != D99_CFMT_NONE && d99_compress_file(data_tar, data_c, fmt) != 0) {
            fprintf(stderr, "d99-build: data compression failed\n");
            return 1;
        }
        free(data_tar);
        free(data_c);
        m.installed_size = total;   /* bytes; control file converts to KiB */
    }

    /* ---- control tar ---- */
    {
        char *ctrl_tar = d99_path_join(tmpdir, "control.tar");
        const char *cext = d99_comp_ext(fmt);
        char *ctrl_c = d99_xasprintf("%s%s", ctrl_tar, cext);
        char *buf = NULL;
        size_t buflen = 0;
        char *md5buf = NULL;
        size_t md5len = 0;
        FILE *mf;
        size_t k;

        f = open_memstream(&buf, &buflen);
        fprintf(f, "Package: %s\n", m.name);
        fprintf(f, "Version: %s\n", m.version);
        fprintf(f, "Architecture: %s\n", m.arch);
        if (m.installed_size > 0)
            fprintf(f, "Installed-Size: %lld\n",
                    (m.installed_size + 1023) / 1024);
        if (m.section)
            fprintf(f, "Section: %s\n", m.section);
        if (m.priority)
            fprintf(f, "Priority: %s\n", m.priority);
        if (m.maintainer)
            fprintf(f, "Maintainer: %s\n", m.maintainer);
        if (m.homepage)
            fprintf(f, "Homepage: %s\n", m.homepage);
        if (m.depends && *m.depends)
            fprintf(f, "Depends: %s\n", m.depends);
        if (m.predepends && *m.predepends)
            fprintf(f, "Pre-Depends: %s\n", m.predepends);
        if (m.recommends && *m.recommends)
            fprintf(f, "Recommends: %s\n", m.recommends);
        if (m.suggests && *m.suggests)
            fprintf(f, "Suggests: %s\n", m.suggests);
        if (m.conflicts && *m.conflicts)
            fprintf(f, "Conflicts: %s\n", m.conflicts);
        if (m.breaks && *m.breaks)
            fprintf(f, "Breaks: %s\n", m.breaks);
        if (m.replaces && *m.replaces)
            fprintf(f, "Replaces: %s\n", m.replaces);
        if (m.provides && *m.provides)
            fprintf(f, "Provides: %s\n", m.provides);
        for (k = 0; k + 1 < m.extra_n; k += 2)
            fprintf(f, "%s: %s\n", m.extra[k], m.extra[k + 1]);
        if (m.description)
            fprintf(f, "Description: %s\n", m.description);
        else
            fprintf(f, "Description: built by d99-build\n");
        fclose(f);

        /* md5sums (conffiles are excluded, like dpkg) */
        mf = open_memstream(&md5buf, &md5len);
        for (k = 0; k < m.files_src.n; k++)
            md5_tree(mf, m.files_src.v[k],
                     m.files_dst.v[k][0] == '/' ? m.files_dst.v[k] + 1
                                                 : m.files_dst.v[k],
                     &m.conffiles);
        fclose(mf);

        f = fopen(ctrl_tar, "wb");
        if (!f) {
            fprintf(stderr, "d99-build: cannot write %s\n", ctrl_tar);
            return 1;
        }
        tw = d99_tar_open_write(f);
        d99_tarw_add_dir(tw, "./", 0755);
        d99_tarw_add_data(tw, "./control", buf, buflen, 0644);
        if (m.conffiles.n) {
            char *cbuf = NULL;
            size_t clen = 0;
            FILE *cf = open_memstream(&cbuf, &clen);
            for (k = 0; k < m.conffiles.n; k++)
                fprintf(cf, "%s\n", m.conffiles.v[k]);
            fclose(cf);
            d99_tarw_add_data(tw, "./conffiles", cbuf, clen, 0644);
            free(cbuf);
        }
        if (md5len > 0)
            d99_tarw_add_data(tw, "./md5sums", md5buf, md5len, 0644);
        for (k = 0; k < m.script_names.n; k++) {
            struct stat st;
            char *arc;
            if (stat(m.script_paths.v[k], &st) != 0) {
                fprintf(stderr, "d99-build: missing script %s\n",
                        m.script_paths.v[k]);
                return 1;
            }
            arc = d99_xasprintf("./%s", m.script_names.v[k]);
            if (d99_tarw_add_file(tw, m.script_paths.v[k], arc, 0755) != 0) {
                fprintf(stderr, "d99-build: cannot add script %s\n",
                        m.script_paths.v[k]);
                free(arc);
                return 1;
            }
            free(arc);
        }
        d99_tarw_close(tw);
        fclose(f);
        free(buf);
        free(md5buf);
        if (fmt != D99_CFMT_NONE && d99_compress_file(ctrl_tar, ctrl_c, fmt) != 0) {
            fprintf(stderr, "d99-build: control compression failed\n");
            return 1;
        }
        free(ctrl_tar);
        free(ctrl_c);
    }

    /* ---- assemble the .deb ---- */
    if (out_opt)
        outpath = d99_xstrdup(out_opt);
    else
        outpath = d99_xasprintf("%s_%s_%s.deb", m.name, m.version, m.arch);

    ar = d99_ar_open_write(outpath);
    if (!ar) {
        fprintf(stderr, "d99-build: cannot create %s\n", outpath);
        return 1;
    }
    d99_ar_add_mem(ar, "debian-binary", "2.0\n", 4);
    {
        const char *ext = d99_comp_ext(fmt);
        char *mname = d99_xasprintf("control.tar%s", ext);
        char *src = d99_xasprintf("%s/control.tar%s", tmpdir, ext);
        d99_ar_add_file(ar, mname, src);
        free(mname);
        free(src);
        mname = d99_xasprintf("data.tar%s", ext);
        src = d99_xasprintf("%s/data.tar%s", tmpdir, ext);
        d99_ar_add_file(ar, mname, src);
        free(mname);
        free(src);
    }
    d99_ar_close(ar);
    printf("d99-build: built package '%s'\n", outpath);
    free(outpath);
    rc = 0;

    d99_manifest_free(&m);
    return rc;
}
