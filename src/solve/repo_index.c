#include "solve.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define REPO_IDX_MAGIC "D99REPO\0"
#define REPO_IDX_VER   1

struct repo_hdr {
    char     magic[8];
    uint32_t version;
    uint32_t n_cands;
    uint32_t n_buckets;
    uint32_t str_len;
    uint64_t lists_mtime;
};
typedef struct {
    unsigned char *b;
    size_t n, cap;
} bybuf;

static void bput(bybuf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 16384;
        b->b = d99_xrealloc(b->b, b->cap);
    }
    memcpy(b->b + b->n, d, n);
    b->n += n;
}

static uint32_t put_str(bybuf *sb, const char *s)
{
    uint32_t off;
    size_t len;
    if (!s || !*s)
        return 0;
    off = (uint32_t)sb->n;
    len = strlen(s);
    bput(sb, s, len + 1);
    return off;
}

static char *extract_field_val(const char *stanza, size_t stanza_len, const char *field, int first_line_only)
{
    size_t flen = strlen(field);
    const char *p = stanza;
    const char *end = stanza + stanza_len;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);

        if (line_len > flen && strncasecmp(p, field, flen) == 0 && p[flen] == ':') {
            const char *val_start = p + flen + 1;
            while (val_start < end && (*val_start == ' ' || *val_start == '\t'))
                val_start++;
            if (first_line_only) {
                const char *val_eol = memchr(val_start, '\n', (size_t)(end - val_start));
                size_t vlen = val_eol ? (size_t)(val_eol - val_start) : (size_t)(end - val_start);
                char *res = d99_xstrndup(val_start, vlen);
                return d99_trim(res);
            } else {
                /* multiline continuation */
                const char *cur = eol ? eol + 1 : end;
                const char *val_end = eol ? eol : end;
                while (cur < end) {
                    if (*cur == ' ' || *cur == '\t') {
                        const char *neol = memchr(cur, '\n', (size_t)(end - cur));
                        val_end = neol ? neol : end;
                        cur = neol ? neol + 1 : end;
                    } else {
                        break;
                    }
                }
                {
                    char *res = d99_xstrndup(val_start, (size_t)(val_end - val_start));
                    return d99_trim(res);
                }
            }
        }
        p = eol ? eol + 1 : end;
    }
    return NULL;
}

static uint64_t get_dir_mtime(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0)
        return (uint64_t)st.st_mtime;
    return 0;
}

static char *infer_base_uri_from_path(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, "archive.ubuntu.com_ubuntu", 25) == 0)
        return d99_xstrdup("http://archive.ubuntu.com/ubuntu");
    if (strncmp(base, "security.ubuntu.com_ubuntu", 26) == 0)
        return d99_xstrdup("http://security.ubuntu.com/ubuntu");
    if (strncmp(base, "packages.linuxmint.com", 22) == 0)
        return d99_xstrdup("http://packages.linuxmint.com");
    if (strstr(base, "adityagarg8.github.io_t2-ubuntu-repo"))
        return d99_xstrdup("https://adityagarg8.github.io/t2-ubuntu-repo");
    if (strstr(base, "repo.waydro.id"))
        return d99_xstrdup("https://repo.waydro.id");
    return d99_xstrdup("");
}

int repo_build_index(const char *lists_dir)
{
    char *manifest_path = d99_path_join(lists_dir, "d99_manifest");
    char *manifest = d99_read_file(manifest_path, NULL);
    char *idx_path = d99_path_join(lists_dir, "repo_index.bin");
    char *tmp_path = d99_path_join(lists_dir, "repo_index.bin.tmp");
    DIR *d = opendir(lists_dir);
    struct dirent *de;
    bybuf sbuf;
    struct cand_disk_rec *recs = NULL;
    size_t n_cands = 0, cap_cands = 0;
    uint32_t n_buckets, i;
    uint32_t *buckets, *next;
    FILE *out;
    struct repo_hdr hdr;
    int scanned_any = 0;

    free(manifest_path);
    memset(&sbuf, 0, sizeof(sbuf));
    /* Offset 0 is empty string */
    bput(&sbuf, "", 1);

    if (d) {
        while ((de = readdir(d)) != NULL) {
            char *full;
            d99_decomp *dc;
            size_t len = 0;
            char *text;
            const char *p;
            char *base = NULL;

            if (de->d_name[0] == '.' || strcmp(de->d_name, "d99_manifest") == 0 ||
                strcmp(de->d_name, "repo_index.bin") == 0 ||
                strcmp(de->d_name, "repo_index.bin.tmp") == 0)
                continue;

            full = d99_path_join(lists_dir, de->d_name);
            dc = d99_decomp_open(full);
            text = dc ? d99_decomp_slurp(dc, &len) : NULL;
            if (dc)
                d99_decomp_close(dc);
            free(full);
            if (!text)
                continue;

            if (manifest) {
                char needle[512];
                snprintf(needle, sizeof needle, "%s\t", de->d_name);
                {
                    char *hit = strstr(manifest, needle);
                    if (hit) {
                        char *eol = strchr(hit, '\n');
                        char *v = hit + strlen(needle);
                        size_t n = eol ? (size_t)(eol - v) : strlen(v);
                        base = d99_xstrndup(v, n);
                    } else if (strncmp(de->d_name, "d99_", 4) == 0) {
                        free(text);
                        continue;
                    }
                }
            }
            if (!base)
                base = infer_base_uri_from_path(de->d_name);

            p = text;
            while (*p) {
                size_t stanza_len;
                const char *blank = strstr(p, "\n\n");
                char *pkg_name, *ver;

                if (blank && (size_t)(blank - p) < 2) {
                    p += 2;
                    continue;
                }
                stanza_len = blank ? (size_t)(blank - p) + 1 : strlen(p);

                pkg_name = extract_field_val(p, stanza_len, "Package", 1);
                ver = extract_field_val(p, stanza_len, "Version", 1);

                if (pkg_name && ver && *pkg_name && *ver) {
                    char *arch = extract_field_val(p, stanza_len, "Architecture", 1);
                    char *fn = extract_field_val(p, stanza_len, "Filename", 1);
                    char *sha = extract_field_val(p, stanza_len, "SHA256", 1);
                    char *sz = extract_field_val(p, stanza_len, "Size", 1);
                    char *desc = extract_field_val(p, stanza_len, "Description", 1);
                    char *dep = extract_field_val(p, stanza_len, "Depends", 0);
                    char *pdep = extract_field_val(p, stanza_len, "Pre-Depends", 0);
                    char *conf = extract_field_val(p, stanza_len, "Conflicts", 0);
                    char *brk = extract_field_val(p, stanza_len, "Breaks", 0);
                    char *prov = extract_field_val(p, stanza_len, "Provides", 0);

                    if (n_cands == cap_cands) {
                        cap_cands = cap_cands ? cap_cands * 2 : 1024;
                        recs = d99_xrealloc(recs, cap_cands * sizeof(*recs));
                    }
                    recs[n_cands].name_off = put_str(&sbuf, pkg_name);
                    recs[n_cands].version_off = put_str(&sbuf, ver);
                    recs[n_cands].arch_off = put_str(&sbuf, arch);
                    recs[n_cands].filename_off = put_str(&sbuf, fn);
                    recs[n_cands].sha256_off = put_str(&sbuf, sha);
                    recs[n_cands].size_off = put_str(&sbuf, sz);
                    recs[n_cands].summary_off = put_str(&sbuf, desc);
                    recs[n_cands].base_uri_off = put_str(&sbuf, base);
                    recs[n_cands].depends_off = put_str(&sbuf, dep);
                    recs[n_cands].predepends_off = put_str(&sbuf, pdep);
                    recs[n_cands].conflicts_off = put_str(&sbuf, conf);
                    recs[n_cands].breaks_off = put_str(&sbuf, brk);
                    recs[n_cands].provides_off = put_str(&sbuf, prov);
                    n_cands++;

                    free(arch);
                    free(fn);
                    free(sha);
                    free(sz);
                    free(desc);
                    free(dep);
                    free(pdep);
                    free(conf);
                    free(brk);
                    free(prov);
                    scanned_any = 1;
                }
                free(pkg_name);
                free(ver);

                p += stanza_len;
                if (blank)
                    p += 1;
                if (*p == '\n')
                    p++;
            }
            free(base);
            free(text);
        }
        closedir(d);
    }

    /* Fallback to /var/lib/apt/lists if lists_dir had no packages */
    if (!scanned_any && strcmp(lists_dir, "/var/lib/apt/lists") != 0 &&
        d99_is_dir("/var/lib/apt/lists")) {
        DIR *ad = opendir("/var/lib/apt/lists");
        if (ad) {
            while ((de = readdir(ad)) != NULL) {
                char *full;
                d99_decomp *dc;
                size_t len = 0;
                char *text;
                const char *p;
                char *base = NULL;

                if (!strstr(de->d_name, "Packages"))
                    continue;

                full = d99_path_join("/var/lib/apt/lists", de->d_name);
                dc = d99_decomp_open(full);
                text = dc ? d99_decomp_slurp(dc, &len) : NULL;
                if (dc)
                    d99_decomp_close(dc);
                free(full);
                if (!text)
                    continue;

                base = infer_base_uri_from_path(de->d_name);
                p = text;
                while (*p) {
                    size_t stanza_len;
                    const char *blank = strstr(p, "\n\n");
                    char *pkg_name, *ver;

                    if (blank && (size_t)(blank - p) < 2) {
                        p += 2;
                        continue;
                    }
                    stanza_len = blank ? (size_t)(blank - p) + 1 : strlen(p);

                    pkg_name = extract_field_val(p, stanza_len, "Package", 1);
                    ver = extract_field_val(p, stanza_len, "Version", 1);

                    if (pkg_name && ver && *pkg_name && *ver) {
                        char *arch = extract_field_val(p, stanza_len, "Architecture", 1);
                        char *fn = extract_field_val(p, stanza_len, "Filename", 1);
                        char *sha = extract_field_val(p, stanza_len, "SHA256", 1);
                        char *sz = extract_field_val(p, stanza_len, "Size", 1);
                        char *desc = extract_field_val(p, stanza_len, "Description", 1);
                        char *dep = extract_field_val(p, stanza_len, "Depends", 0);
                        char *pdep = extract_field_val(p, stanza_len, "Pre-Depends", 0);
                        char *conf = extract_field_val(p, stanza_len, "Conflicts", 0);
                        char *brk = extract_field_val(p, stanza_len, "Breaks", 0);
                        char *prov = extract_field_val(p, stanza_len, "Provides", 0);

                        if (n_cands == cap_cands) {
                            cap_cands = cap_cands ? cap_cands * 2 : 1024;
                            recs = d99_xrealloc(recs, cap_cands * sizeof(*recs));
                        }
                        recs[n_cands].name_off = put_str(&sbuf, pkg_name);
                        recs[n_cands].version_off = put_str(&sbuf, ver);
                        recs[n_cands].arch_off = put_str(&sbuf, arch);
                        recs[n_cands].filename_off = put_str(&sbuf, fn);
                        recs[n_cands].sha256_off = put_str(&sbuf, sha);
                        recs[n_cands].size_off = put_str(&sbuf, sz);
                        recs[n_cands].summary_off = put_str(&sbuf, desc);
                        recs[n_cands].base_uri_off = put_str(&sbuf, base);
                        recs[n_cands].depends_off = put_str(&sbuf, dep);
                        recs[n_cands].predepends_off = put_str(&sbuf, pdep);
                        recs[n_cands].conflicts_off = put_str(&sbuf, conf);
                        recs[n_cands].breaks_off = put_str(&sbuf, brk);
                        recs[n_cands].provides_off = put_str(&sbuf, prov);
                        n_cands++;

                        free(arch);
                        free(fn);
                        free(sha);
                        free(sz);
                        free(desc);
                        free(dep);
                        free(pdep);
                        free(conf);
                        free(brk);
                        free(prov);
                        scanned_any = 1;
                    }
                    free(pkg_name);
                    free(ver);

                    p += stanza_len;
                    if (blank)
                        p += 1;
                    if (*p == '\n')
                        p++;
                }
                free(base);
                free(text);
            }
            closedir(ad);
        }
    }
    free(manifest);

    if (n_cands == 0) {
        free(recs);
        free(sbuf.b);
        free(idx_path);
        free(tmp_path);
        return -1;
    }

    n_buckets = 1024;
    while (n_buckets < (uint32_t)(n_cands * 2))
        n_buckets <<= 1;

    buckets = d99_xmalloc(n_buckets * sizeof(uint32_t));
    memset(buckets, 0xff, n_buckets * sizeof(uint32_t));
    next = d99_xmalloc(n_cands * sizeof(uint32_t));
    memset(next, 0xff, n_cands * sizeof(uint32_t));

    for (i = 0; i < (uint32_t)n_cands; i++) {
        const char *name = (const char *)(sbuf.b + recs[i].name_off);
        uint64_t h = d99_fnv1a64_str(name);
        size_t b = (size_t)(h & (n_buckets - 1));
        next[i] = buckets[b];
        buckets[b] = i;
    }

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, REPO_IDX_MAGIC, 8);
    hdr.version = REPO_IDX_VER;
    hdr.n_cands = (uint32_t)n_cands;
    hdr.n_buckets = n_buckets;
    hdr.str_len = (uint32_t)sbuf.n;
    hdr.lists_mtime = get_dir_mtime(lists_dir);

    out = fopen(tmp_path, "wb");
    if (!out) {
        free(buckets);
        free(next);
        free(recs);
        free(sbuf.b);
        free(idx_path);
        free(tmp_path);
        return -1;
    }

    fwrite(&hdr, 1, sizeof(hdr), out);
    fwrite(buckets, 1, n_buckets * sizeof(uint32_t), out);
    fwrite(next, 1, n_cands * sizeof(uint32_t), out);
    fwrite(recs, 1, n_cands * sizeof(struct cand_disk_rec), out);
    fwrite(sbuf.b, 1, sbuf.n, out);
    {
        char pad[16] = {0};
        fwrite(pad, 1, sizeof(pad), out);
    }

    fflush(out);
    fsync(fileno(out));
    fclose(out);

    rename(tmp_path, idx_path);
    chmod(idx_path, 0644);

    free(buckets);
    free(next);
    free(recs);
    free(sbuf.b);
    free(idx_path);
    free(tmp_path);
    return 0;
}

void cand_ensure_deps(d99_cand *c, d99_arena *ar)
{
    if (!c || c->deps_parsed)
        return;
    c->deps_parsed = 1;
    if (c->raw_depends && *c->raw_depends)
        d99_deplist_parse(c->raw_depends, &c->depends, ar);
    if (c->raw_predepends && *c->raw_predepends)
        d99_deplist_parse(c->raw_predepends, &c->predepends, ar);
    if (c->raw_conflicts && *c->raw_conflicts)
        d99_deplist_parse(c->raw_conflicts, &c->conflicts, ar);
    if (c->raw_breaks && *c->raw_breaks)
        d99_deplist_parse(c->raw_breaks, &c->breaks, ar);
    if (c->raw_provides && *c->raw_provides)
        d99_deplist_parse(c->raw_provides, &c->provides, ar);
}

d99_repo *repo_load(const char *lists_dir)
{
    char *idx_path = d99_path_join(lists_dir, "repo_index.bin");
    struct stat idx_st;
    uint64_t dir_mt = get_dir_mtime(lists_dir);
    int fd;
    struct stat st;
    void *map;
    struct repo_hdr *hdr;
    const unsigned char *ptr;
    const uint32_t *buckets;
    const uint32_t *next;
    const struct cand_disk_rec *recs;
    const char *strings;
    d99_repo *r;

    if (stat(idx_path, &idx_st) != 0 || (uint64_t)idx_st.st_mtime < dir_mt) {
        repo_build_index(lists_dir);
    }

    fd = open(idx_path, O_RDONLY);
    free(idx_path);
    if (fd < 0) {
        r = d99_xcalloc(1, sizeof(*r));
        r->ar = d99_arena_new();
        return r;
    }

    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(struct repo_hdr)) {
        close(fd);
        r = d99_xcalloc(1, sizeof(*r));
        r->ar = d99_arena_new();
        return r;
    }

    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        r = d99_xcalloc(1, sizeof(*r));
        r->ar = d99_arena_new();
        return r;
    }

    hdr = (struct repo_hdr *)map;
    if (memcmp(hdr->magic, REPO_IDX_MAGIC, 8) != 0 || hdr->version != REPO_IDX_VER) {
        munmap(map, (size_t)st.st_size);
        r = d99_xcalloc(1, sizeof(*r));
        r->ar = d99_arena_new();
        return r;
    }

    ptr = (const unsigned char *)map + sizeof(struct repo_hdr);
    buckets = (const uint32_t *)ptr;
    ptr += (size_t)hdr->n_buckets * sizeof(uint32_t);
    next = (const uint32_t *)ptr;
    ptr += (size_t)hdr->n_cands * sizeof(uint32_t);
    recs = (const struct cand_disk_rec *)ptr;
    ptr += (size_t)hdr->n_cands * sizeof(struct cand_disk_rec);
    strings = (const char *)ptr;

    r = d99_xcalloc(1, sizeof(*r));
    r->ar = d99_arena_new();
    r->n = hdr->n_cands;
    r->cap = hdr->n_cands;
    r->v = d99_xcalloc(hdr->n_cands, sizeof(d99_cand *));
    r->ht = (uint32_t *)buckets;
    r->next = (uint32_t *)next;
    r->ht_size = hdr->n_buckets;
    r->map = map;
    r->map_len = (size_t)st.st_size;
    r->disk_recs = recs;
    r->strings = strings;

    return r;
}

d99_cand *repo_get(d99_repo *r, size_t idx)
{
    if (!r || idx >= r->n)
        return NULL;
    if (!r->v[idx] && r->disk_recs && r->strings) {
        const struct cand_disk_rec *recs = (const struct cand_disk_rec *)r->disk_recs;
        const char *strings = r->strings;
        d99_cand *c = d99_arena_alloc(r->ar, sizeof(d99_cand));
        memset(c, 0, sizeof(*c));
        c->name = (char *)(strings + recs[idx].name_off);
        c->version = (char *)(strings + recs[idx].version_off);
        c->arch = recs[idx].arch_off ? (char *)(strings + recs[idx].arch_off) : (char *)"all";
        c->filename = recs[idx].filename_off ? (char *)(strings + recs[idx].filename_off) : (char *)"";
        c->sha256 = recs[idx].sha256_off ? (char *)(strings + recs[idx].sha256_off) : (char *)"";
        c->size = recs[idx].size_off ? (char *)(strings + recs[idx].size_off) : (char *)"";
        c->summary = recs[idx].summary_off ? (char *)(strings + recs[idx].summary_off) : (char *)"";
        c->base_uri = recs[idx].base_uri_off ? (char *)(strings + recs[idx].base_uri_off) : (char *)"";
        c->raw_depends = recs[idx].depends_off ? (char *)(strings + recs[idx].depends_off) : NULL;
        c->raw_predepends = recs[idx].predepends_off ? (char *)(strings + recs[idx].predepends_off) : NULL;
        c->raw_conflicts = recs[idx].conflicts_off ? (char *)(strings + recs[idx].conflicts_off) : NULL;
        c->raw_breaks = recs[idx].breaks_off ? (char *)(strings + recs[idx].breaks_off) : NULL;
        c->raw_provides = recs[idx].provides_off ? (char *)(strings + recs[idx].provides_off) : NULL;
        c->deps_parsed = 0;
        c->idx = idx;
        r->v[idx] = c;
    }
    return r->v[idx];
}

void repo_free(d99_repo *r)
{
    if (!r)
        return;
    if (r->map)
        munmap(r->map, r->map_len);
    d99_arena_free(r->ar);
    free(r->v);
    free(r);
}

int cand_satisfies(d99_cand *c, const char *name, int op, const char *ver)
{
    size_t i, j;

    if (strcmp(c->name, name) == 0) {
        if (op == D99_DEP_NONE || !ver)
            return 1;
        return d99_verrel(d99_vercmp(c->version, ver), op);
    }
    if (c->raw_provides && *c->raw_provides) {
        cand_ensure_deps(c, NULL);
        for (i = 0; i < c->provides.n; i++)
            for (j = 0; j < c->provides.g[i].n; j++) {
                d99_depalternative *pa = &c->provides.g[i].alts[j];
                if (strcmp(pa->name, name) != 0)
                    continue;
                if (op == D99_DEP_NONE || !ver)
                    return 1;
                if (pa->ver)
                    return d99_verrel(d99_vercmp(pa->ver, ver), op);
                return 0;
            }
    }
    return 0;
}

static int cand_arch_ok(const d99_cand *c)
{
    const char *host;
    if (!c || !c->arch || !*c->arch)
        return 1;
    if (strcmp(c->arch, "all") == 0)
        return 1;
    host = d99_host_arch();
    return (host && strcmp(c->arch, host) == 0);
}

d99_cand *repo_find(d99_repo *r, const char *name, int op, const char *ver)
{
    d99_cand *best = NULL;
    const struct cand_disk_rec *recs = (const struct cand_disk_rec *)r->disk_recs;
    const char *strings = r->strings;

    if (strcmp(name, "dpkg") == 0 || strcmp(name, "apt") == 0 ||
        strcmp(name, "dpkg-dev") == 0 || strcmp(name, "apt-utils") == 0) {
        d99_cand *d99c = repo_find(r, "d99", D99_DEP_NONE, NULL);
        if (d99c)
            return d99c;
    }

    if (r->ht && r->ht_size > 0 && recs && strings) {
        uint64_t h = d99_fnv1a64_str(name);
        size_t b = (size_t)(h & (r->ht_size - 1));
        uint32_t idx = r->ht[b];
        while (idx != (uint32_t)-1) {
            const char *cname = strings + recs[idx].name_off;
            if (strcmp(cname, name) == 0) {
                d99_cand *c = repo_get(r, idx);
                if (c && cand_arch_ok(c) && (op == D99_DEP_NONE || !ver ||
                          d99_verrel(d99_vercmp(c->version, ver), op))) {
                    if (!best || d99_vercmp(c->version, best->version) > 0)
                        best = c;
                }
            }
            idx = r->next[idx];
        }
        if (best)
            return best;
    } else {
        size_t i;
        for (i = 0; i < r->n; i++) {
            d99_cand *c = repo_get(r, i);
            if (!c || strcmp(c->name, name) != 0 || !cand_arch_ok(c))
                continue;
            if (op != D99_DEP_NONE && ver) {
                if (!d99_verrel(d99_vercmp(c->version, ver), op))
                    continue;
            }
            if (!best || d99_vercmp(c->version, best->version) > 0)
                best = c;
        }
        if (best)
            return best;
    }

    /* Check virtual packages (provides) */
    if (recs) {
        size_t i;
        for (i = 0; i < r->n; i++) {
            if (!recs[i].provides_off)
                continue;
            d99_cand *c = repo_get(r, i);
            if (!c || !cand_arch_ok(c))
                continue;
            if (cand_satisfies(c, name, op, ver)) {
                if (!best || d99_vercmp(c->version, best->version) > 0)
                    best = c;
            }
        }
    }
    return best;
}
