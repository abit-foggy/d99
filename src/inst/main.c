#include "inst.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ================= deb member helpers (same layout as d99-deb) ======== */

static int open_deb(const char *path, d99_ar **ar_out,
                    d99_ar_member *ctrl, d99_ar_member *data)
{
    d99_ar *ar = d99_ar_open_read(path);
    d99_ar_member m;
    int have_ctrl = 0, have_data = 0, have_ver = 0;

    if (!ar) {
        fprintf(stderr, "d99-inst: cannot open archive '%s'\n", path);
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
        fprintf(stderr, "d99-inst: '%s' is not a valid debian package\n", path);
        d99_ar_close(ar);
        return -1;
    }
    *ar_out = ar;
    return 0;
}

static d99_decomp *member_decomp(d99_ar *ar, const d99_ar_member *m)
{
    return d99_decomp_open_region(d99_ar_stream(ar), m->data_off, m->size, -1);
}

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

/* ================= unpack context ====================================== */

struct mapctx {
    struct d99_ctx *c;
    const char *pkg;
    d99_strvec *conffiles;
    char backup_buf[4096];
    char new_buf[4096];
    char abuf[4096];
};

static int is_standard_tool_path(const char *abuf, const char **canonical_tool)
{
    const char *p = abuf;
    if (strncmp(p, "/usr/bin/", 9) == 0)
        p += 9;
    else if (strncmp(p, "/bin/", 5) == 0)
        p += 5;
    else
        return 0;

    if (strcmp(p, "dpkg") == 0 ||
        strcmp(p, "dpkg-deb") == 0 ||
        strcmp(p, "dpkg-query") == 0 ||
        strcmp(p, "apt") == 0 ||
        strcmp(p, "apt-get") == 0 ||
        strcmp(p, "apt-mark") == 0) {
        if (canonical_tool)
            *canonical_tool = p;
        return 1;
    }
    return 0;
}

static void ensure_tool_symlinks(const char *root)
{
    static const struct {
        const char *tool_rel;
        const char *our_bin;
    } tools[] = {
        { "usr/bin/dpkg", "d99-inst" },
        { "usr/bin/dpkg-deb", "d99-deb" },
        { "usr/bin/dpkg-query", "d99-query" },
        { "usr/bin/apt", "d99-solve" },
        { "usr/bin/apt-get", "d99-solve" },
        { "usr/bin/apt-mark", "d99-solve" },
        { NULL, NULL }
    };
    int i;
    for (i = 0; tools[i].tool_rel; i++) {
        char *backup_rel = d99_xasprintf("%s.upstream", tools[i].tool_rel);
        char *backup_full = d99_path_join(root, backup_rel);
        if (d99_file_exists(backup_full)) {
            char *target = d99_path_join(root, tools[i].tool_rel);
            char *our_usr_bin = d99_path_join(root, "usr/bin");
            char *our_usr_file = d99_path_join(our_usr_bin, tools[i].our_bin);
            char *our_local_bin = d99_path_join(root, "usr/local/bin");
            char *our_local_file = d99_path_join(our_local_bin, tools[i].our_bin);

            if (d99_file_exists(our_usr_file)) {
                unlink(target);
                (void)!symlink(tools[i].our_bin, target);
            } else if (d99_file_exists(our_local_file)) {
                const char *dest_prefix = (!root || strcmp(root, "/") == 0) ?
                                          "/usr/local/bin/" : "../local/bin/";
                char *sym_dest = d99_xasprintf("%s%s", dest_prefix, tools[i].our_bin);
                unlink(target);
                (void)!symlink(sym_dest, target);
                free(sym_dest);
            }
            free(our_local_file);
            free(our_local_bin);
            free(our_usr_file);
            free(our_usr_bin);
            free(target);
        }
        free(backup_full);
        free(backup_rel);
    }
}

static int is_superseded_pkg(const char *name)
{
    return (strcmp(name, "dpkg") == 0 ||
            strcmp(name, "apt") == 0 ||
            strcmp(name, "dpkg-dev") == 0 ||
            strcmp(name, "apt-utils") == 0);
}

/* Extraction callback: diversions + standard tools + ".dpkg-new" conffile semantics.
 * Tar member paths arrive normalized and relative ("usr/bin/dpkg"),
 * while /var/lib/dpkg/diversions records absolute paths
 * ("/usr/bin/dpkg"); bridge the two forms here. */
static const char *inst_map(const char *path, int is_dir, void *ud)
{
    struct mapctx *m = ud;
    const char *tool = NULL;

    if (is_dir)
        return path;
    snprintf(m->abuf, sizeof m->abuf, "/%s", path);
    {
        const char *dv = diversions_map(m->c->div, m->abuf, m->pkg);
        if (dv) {
            path = dv + 1;   /* back to extractor-relative form */
        } else if (is_standard_tool_path(m->abuf, &tool)) {
            /* Standard tool update: automatically link updates to their backup version (.upstream)
             * If no backup version is found on disk or in diversions, then don't update. */
            char *backup_rel = d99_xasprintf("%s.upstream", path);
            char *backup_full = d99_path_join(m->c->root, backup_rel);
            int backup_exists = d99_file_exists(backup_full);
            free(backup_full);

            if (backup_exists) {
                snprintf(m->backup_buf, sizeof m->backup_buf, "%.4000s.upstream", path);
                free(backup_rel);
                path = m->backup_buf;
            } else {
                free(backup_rel);
                fprintf(stderr, "d99-inst: standard tool '%s' has no backup version (.upstream); skipping update\n", m->abuf);
                return NULL;
            }
        }
    }
    if (m->conffiles && d99_sv_contains(m->conffiles, path)) {
        char *full = d99_path_join(m->c->root, path);
        if (d99_file_exists(full)) {
            snprintf(m->new_buf, sizeof m->new_buf, "%.4000s.dpkg-new", path);
            free(full);
            return m->new_buf;
        }
        free(full);
    }
    return path;
}

static void inst_record(const char *ondisk, void *ud)
{
    d99_strvec *list = ud;
    d99_sv_push(list, ondisk);
}

/* ================= dependency checking ================================ */

static int pkg_satisfies(d99_pkg *p, const d99_depalternative *alt,
                         int installed_only)
{
    size_t i;

    if (installed_only && p->state != D99_PS_INSTALLED)
        return 0;
    if (strcmp(p->name, "d99") == 0 && is_superseded_pkg(alt->name))
        return 1;
    if (d99_dep_alt_match(alt, p->name, p->version))
        return 1;
    for (i = 0; i < p->provides.n; i++) {
        size_t j;
        for (j = 0; j < p->provides.g[i].n; j++) {
            if (d99_dep_alt_match(alt, p->provides.g[i].alts[j].name,
                                  p->provides.g[i].alts[j].ver))
                return 1;
        }
    }
    return 0;
}

static int group_ok(struct d99_ctx *c, const d99_depgroup *g,
                    d99_pkg **batch, size_t nbatch)
{
    size_t a, i;

    /* an alternative scoped to other architectures is not applicable */
    {
        int any_applicable = 0;
        for (a = 0; a < g->n; a++)
            if (d99_arch_ok(g->alts[a].arch, d99_host_arch()))
                any_applicable = 1;
        if (!any_applicable)
            return 1;
    }
    for (a = 0; a < g->n; a++) {
        const d99_depalternative *alt = &g->alts[a];
        if (!d99_arch_ok(alt->arch, d99_host_arch()))
            continue;
        /* installed database */
        {
            size_t n = d99_db_count(c->db);
            for (i = 0; i < n; i++) {
                if (pkg_satisfies(d99_db_at(c->db, i), alt, 1))
                    return 1;
            }
        }
        /* this transaction's batch */
        for (i = 0; i < nbatch; i++)
            if (pkg_satisfies(batch[i], alt, 0))
                return 1;
    }
    return 0;
}

static char *fmt_group(const d99_depgroup *g)
{
    size_t i;
    char *s = d99_xstrdup("");
    for (i = 0; i < g->n; i++) {
        const d99_depalternative *alt = &g->alts[i];
        char *next;
        if (alt->op != D99_DEP_NONE)
            next = d99_xasprintf("%s%s%s (%s %s)", s, i ? " | " : "",
                                 alt->name, d99_depop_name(alt->op), alt->ver);
        else
            next = d99_xasprintf("%s%s%s", s, i ? " | " : "", alt->name);
        free(s);
        s = next;
    }
    return s;
}

static int check_missing(struct d99_ctx *c, d99_pkg *p, const char *what,
                         d99_pkg **batch, size_t nbatch)
{
    size_t i;
    int missing = 0;

    for (i = 0; i < p->predepends.n; i++)
        if (!group_ok(c, &p->predepends.g[i], batch, nbatch))
            missing += 1;
    if (strcmp(what, "pre-depends") != 0)
        for (i = 0; i < p->depends.n; i++)
            if (!group_ok(c, &p->depends.g[i], batch, nbatch))
                missing += 1;
    if (missing) {
        fprintf(stderr, "d99-inst: %s problems for %s:\n", what, p->name);
        for (i = 0; i < p->predepends.n; i++)
            if (!group_ok(c, &p->predepends.g[i], batch, nbatch)) {
                char *s = fmt_group(&p->predepends.g[i]);
                fprintf(stderr, "  %s is not satisfied: %s\n", what, s);
                free(s);
            }
        if (strcmp(what, "pre-depends") != 0)
            for (i = 0; i < p->depends.n; i++)
                if (!group_ok(c, &p->depends.g[i], batch, nbatch)) {
                    char *s = fmt_group(&p->depends.g[i]);
                    fprintf(stderr, "  depends is not satisfied: %s\n", s);
                    free(s);
                }
        return -1;
    }
    return 0;
}

/* ================= info-file helpers ================================== */

static int is_maintscript(const char *name)
{
    return strcmp(name, "preinst") == 0 || strcmp(name, "postinst") == 0 ||
           strcmp(name, "prerm") == 0 || strcmp(name, "postrm") == 0 ||
           strcmp(name, "config") == 0;
}

static void write_md5sums_verify(struct d99_ctx *c, const char *pkg,
                                  char *md5text)
{
    const char *p;

    if (!md5text)
        return;
    if (d99_info_write(c->admindir, pkg, ".md5sums", md5text,
                       strlen(md5text), 0644) != 0)
        d99_warn("cannot write md5sums for %s", pkg);
    p = md5text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_xstrndup(p, ln);
        char *sp = strstr(line, "  ");
        char *path;

        if (sp) {
            char want[33];
            char got[33];
            size_t hl = (size_t)(sp - line);
            if (hl > 0 && hl <= 32) {
                memcpy(want, line, hl);
                want[hl] = '\0';
                path = d99_trim(sp + 2);
                {
                    char *full = d99_path_join(c->root, path);
                    if (d99_md5_file(full, got) == 0 && strcmp(want, got) != 0)
                        d99_warn("md5sum mismatch for %s", path);
                    free(full);
                }
            }
        }
        free(line);
        if (!eol)
            break;
        p = eol + 1;
    }
}

/* ================= transactions ====================================== */

int inst_unpack_deb(struct d99_ctx *c, const char *deb,
                    d99_pkg **batch, size_t nbatch)
{
    d99_ar *ar;
    d99_ar_member ctrl, data;
    d99_decomp *d;
    d99_tarr *t;
    d99_tar_member m;
    char name[4096];
    size_t ctrl_len = 0;
    char *ctrl_text = NULL;
    d99_arena *tmpar;
    d99_pkg *np;
    d99_pkg *old;
    d99_strvec conffiles, list;
    char *md5text = NULL;
    int had_preinst = 0;

    if (open_deb(deb, &ar, &ctrl, &data) != 0)
        return 1;

    /* ---- read the control stanza and buffer control members ---- */
    struct {
        char name[128];
        char *buf;
        size_t size;
    } ctrl_entries[64];
    size_t n_ctrl = 0, ci;

    d = member_decomp(ar, &ctrl);
    if (!d) {
        d99_ar_close(ar);
        return 1;
    }
    t = d99_tar_open_decomp(d);
    while (d99_tar_next(t, &m) == 1) {
        norm_name(m.name, name, sizeof name);
        if (!name[0] || (m.typeflag != '0' && m.typeflag != '\0')) {
            d99_tar_skip(t);
            continue;
        }
        if (strcmp(name, "control") == 0) {
            ctrl_text = d99_xmalloc((size_t)m.size + 1);
            if (d99_tar_read_data(t, ctrl_text, (size_t)m.size) == 0) {
                ctrl_text[m.size] = '\0';
                ctrl_len = (size_t)m.size;
            } else {
                free(ctrl_text);
                ctrl_text = NULL;
            }
        } else if (n_ctrl < sizeof(ctrl_entries) / sizeof(ctrl_entries[0])) {
            char *buf = d99_xmalloc((size_t)m.size + 1);
            if (d99_tar_read_data(t, buf, (size_t)m.size) == 0) {
                buf[m.size] = '\0';
                size_t nl = strlen(name);
                if (nl >= sizeof(ctrl_entries[n_ctrl].name))
                    nl = sizeof(ctrl_entries[n_ctrl].name) - 1;
                memcpy(ctrl_entries[n_ctrl].name, name, nl);
                ctrl_entries[n_ctrl].name[nl] = '\0';
                ctrl_entries[n_ctrl].buf = buf;
                ctrl_entries[n_ctrl].size = (size_t)m.size;
                n_ctrl++;
            } else {
                free(buf);
            }
        } else {
            d99_tar_skip(t);
        }
    }
    d99_tar_close_read(t);
    if (!ctrl_text) {
        fprintf(stderr, "d99-inst: %s has no control file\n", deb);
        for (ci = 0; ci < n_ctrl; ci++) free(ctrl_entries[ci].buf);
        d99_ar_close(ar);
        return 1;
    }

    tmpar = d99_arena_new();
    np = d99_pkg_parse_stanza(ctrl_text, ctrl_len, tmpar);
    if (!np || !np->name || !np->version) {
        fprintf(stderr, "d99-inst: malformed control file in %s\n", deb);
        for (ci = 0; ci < n_ctrl; ci++) free(ctrl_entries[ci].buf);
        d99_arena_free(tmpar);
        free(ctrl_text);
        d99_ar_close(ar);
        return 1;
    }
    old = d99_db_find(c->db, np->name);
    if (old && old->sel == D99_SEL_HOLD) {
        fprintf(stderr, "d99-inst: package '%s' is on hold; "
                "use --force-hold to override\n", np->name);
        for (ci = 0; ci < n_ctrl; ci++) free(ctrl_entries[ci].buf);
        d99_arena_free(tmpar);
        free(ctrl_text);
        d99_ar_close(ar);
        return 1;
    }
    if (old && old->state == D99_PS_INSTALLED &&
        d99_vercmp(old->version, np->version) == 0) {
        d99_verbose("%s %s already installed", np->name, np->version);
        for (ci = 0; ci < n_ctrl; ci++) free(ctrl_entries[ci].buf);
        d99_arena_free(tmpar);
        free(ctrl_text);
        d99_ar_close(ar);
        return 0;
    }

    /* ---- dependency gates (pre-depends strictly, depends normally) ---- */
    if (!c->force_depends) {
        if (check_missing(c, np, "pre-depends", batch, nbatch) != 0 ||
            check_missing(c, np, "depends", batch, nbatch) != 0) {
            fprintf(stderr, "d99-inst: use --force-depends to proceed "
                    "anyway\n");
            for (ci = 0; ci < n_ctrl; ci++) free(ctrl_entries[ci].buf);
            d99_arena_free(tmpar);
            free(ctrl_text);
            d99_ar_close(ar);
            return 1;
        }
    }

    if (isatty(STDOUT_FILENO))
        printf("\r\033[K");
    if (old && old->version) {
        printf("Preparing to unpack %s ...\n", deb);
        printf("Unpacking %s (%s) over (%s) ...\n", np->name, np->version, old->version);
    } else {
        printf("Selecting previously unselected package %s.\n", np->name);
        printf("Preparing to unpack %s ...\n", deb);
        printf("Unpacking %s (%s) ...\n", np->name, np->version);
    }

    /* ---- preinst ---- */
    {
        char *args[3];
        int na = 0;
        if (old && old->state == D99_PS_INSTALLED) {
            args[na++] = (char *)"upgrade";
            args[na++] = old->version;
        } else if (old && old->version) {
            args[na++] = (char *)"install";
            args[na++] = old->version;
        } else {
            args[na++] = (char *)"install";
        }
        args[na] = NULL;
        if (hook_run(c, np->name, np->version, np->arch, "preinst", args) != 0) {
            fprintf(stderr, "d99-inst: preinst failure for %s; aborting\n",
                    np->name);
            for (ci = 0; ci < n_ctrl; ci++) free(ctrl_entries[ci].buf);
            d99_arena_free(tmpar);
            free(ctrl_text);
            d99_ar_close(ar);
            return 1;
        }
        had_preinst = 1;
    }

    /* ---- extract control members -> admindir/info ---- */
    d99_sv_init(&conffiles);
    for (ci = 0; ci < n_ctrl; ci++) {
        char *buf = ctrl_entries[ci].buf;
        size_t sz = ctrl_entries[ci].size;
        const char *ename = ctrl_entries[ci].name;
        if (strcmp(ename, "conffiles") == 0) {
            buf[sz] = '\0';
            d99_conffiles_parse(buf, &conffiles, tmpar);
        } else if (strcmp(ename, "md5sums") == 0) {
            buf[sz] = '\0';
            md5text = d99_xstrdup(buf);
        } else {
            char *sfx = d99_xasprintf(".%s", ename);
            d99_info_write(c->admindir, np->name, sfx, buf, sz,
                           is_maintscript(ename) ? 0755 : 0644);
            free(sfx);
        }
        free(buf);
    }
    n_ctrl = 0;

    /* ---- unpack the filesystem data ---- */
    d99_sv_init(&list);
    {
        d99_tar_extract_opts o;
        struct mapctx mc;
        long n;

        memset(&o, 0, sizeof o);
        o.dest = c->root;
        mc.c = c;
        mc.pkg = np->name;
        mc.conffiles = &conffiles;
        mc.backup_buf[0] = '\0';
        mc.new_buf[0] = '\0';
        o.map = inst_map;
        o.map_ud = &mc;
        o.record = inst_record;
        o.record_ud = &list;
        o.verbose = 0;
        o.keep_going = 0;

        d = member_decomp(ar, &data);
        if (!d) {
            fprintf(stderr, "d99-inst: cannot open data member of %s\n", deb);
            return 1;
        }
        t = d99_tar_open_decomp(d);
        n = d99_tar_extract(t, &o);
        d99_tar_close_read(t);
        if (n < 0) {
            fprintf(stderr, "d99-inst: unpacking %s failed\n", deb);
            return 1;
        }
        d99_verbose("unpacked %ld files for %s", n, np->name);
    }

    ensure_tool_symlinks(c->root);

    /* write the file list */
    {
        char *lp = d99_info_path(c->admindir, np->name, ".list");
        FILE *lf;
        size_t i;

        d99_ensure_parent(lp, 0755);
        lf = fopen(lp, "wb");
        if (!lf) {
            d99_warn("cannot write %s", lp);
        } else {
            for (i = 0; i < list.n; i++)
                fprintf(lf, "/%s\n", list.v[i]);
            fclose(lf);
        }
        free(lp);
    }
    write_md5sums_verify(c, np->name, md5text);
    free(md5text);

    /* ---- update the status DB: unpacked ---- */
    {
        int was_installed = old && old->state == D99_PS_INSTALLED;
        const char *configver = was_installed ? old->version : NULL;

        /* rebind the parsed control into the db arena */
        {
            d99_pkg *dbp = d99_pkg_parse_stanza(ctrl_text, ctrl_len,
                                                d99_db_arena(c->db));
            if (!dbp) {
                fprintf(stderr, "d99-inst: cannot rebind %s\n", deb);
                return 1;
            }
            if (configver)
                d99_pkg_set_field(c->db, dbp, "Config-Version", configver);
            d99_pkg_set_status(c->db, dbp, D99_SEL_INSTALL, D99_PS_UNPACKED,
                               D99_PF_OK);
            d99_db_add(c->db, dbp);
        }
    }

    if (d99_db_save_status(c->db, c->admindir) != 0)
        d99_warn("cannot save status database");
    if (d99_index_build(c->admindir, c->indexpath) != 0)
        d99_verbose("index rebuild skipped");

    d99_sv_free(&conffiles);
    d99_sv_free(&list);
    d99_arena_free(tmpar);
    free(ctrl_text);
    d99_ar_close(ar);
    (void)had_preinst;
    return 0;
}

static int inst_configure(struct d99_ctx *c, const char *name)
{
    d99_pkg *p = d99_db_find(c->db, name);
    char *args[3];
    int na = 0;
    char *oldver;

    if (!p) {
        fprintf(stderr, "d99-inst: package '%s' is not installed\n", name);
        return 1;
    }
    if (p->state == D99_PS_INSTALLED) {
        d99_verbose("%s already configured", name);
        return 0;
    }
    if (p->state != D99_PS_UNPACKED && p->state != D99_PS_HALFCONFIGURED &&
        p->state != D99_PS_TRIGGERSPENDING) {
        fprintf(stderr, "d99-inst: package '%s' is in state '%s'; cannot "
                "configure\n", name, d99_state_name(p->state));
        return 1;
    }
    oldver = d99_pkg_field_dup(p, "Config-Version");
    if (isatty(STDOUT_FILENO))
        printf("\r\033[K");
    printf("Setting up %s (%s) ...\n", p->name, p->version ? p->version : "");
    args[na++] = (char *)"configure";
    if (oldver)
        args[na++] = oldver;
    args[na] = NULL;

    if (hook_run(c, p->name, p->version, p->arch, "postinst", args) != 0) {
        d99_warn("postinst for %s failed; leaving half-configured", p->name);
        d99_pkg_set_status(c->db, p, D99_SEL_INSTALL, D99_PS_HALFCONFIGURED,
                           D99_PF_REINSTREQ);
        d99_db_save_status(c->db, c->admindir);
        free(oldver);
        return 1;
    }
    d99_pkg_remove_field(c->db, p, "Config-Version");
    d99_pkg_set_status(c->db, p, D99_SEL_INSTALL, D99_PS_INSTALLED, D99_PF_OK);
    if (d99_db_save_status(c->db, c->admindir) != 0)
        d99_warn("cannot save status database");
    if (d99_index_build(c->admindir, c->indexpath) != 0)
        d99_verbose("index rebuild skipped");
    free(oldver);
    return 0;
}

static void print_dpkg_progress(size_t cur, size_t total)
{
    if (!isatty(STDOUT_FILENO) || total == 0) return;
    int pct = (int)((cur * 100) / total);
    if (pct > 100) pct = 100;
    int width = 30;
    int filled = (pct * width) / 100;
    printf("\r\033[KProgress: [%3d%%] [", pct);
    for (int i = 0; i < filled; i++) putchar('#');
    for (int i = filled; i < width; i++) putchar('.');
    printf("]");
    if (cur >= total)
        putchar('\n');
    else
        putchar('\r');
    fflush(stdout);
}

/* shared file-removal helper; keep_conffiles: 0 on purge */
static int remove_files(struct d99_ctx *c, const char *pkg, int keep_conffiles)
{
    char *listpath = d99_info_path(c->admindir, pkg, ".list");
    size_t len = 0;
    char *text = d99_read_file(listpath, &len);
    d99_strvec conffiles, dirs;
    const char *p;
    size_t i;

    free(listpath);
    d99_sv_init(&conffiles);
    d99_sv_init(&dirs);
    {
        size_t cl = 0;
        char *ct = d99_info_read(c->admindir, pkg, ".conffiles", &cl);
        if (ct) {
            d99_arena *ar = d99_arena_new();
            d99_conffiles_parse(ct, &conffiles, ar);
            d99_arena_free(ar);
            free(ct);
        }
    }
    if (!text)
        return 0;

    p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_xstrndup(p, ln);
        char *t = d99_trim(line);

        if (t[0] == '/') {
            char *full = d99_path_join(c->root, t + 1);
            struct stat st;

            if (lstat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    d99_sv_push(&dirs, t);
                } else if (keep_conffiles && d99_sv_contains(&conffiles, t + 1)) {
                    /* preserve conffile on remove */
                } else {
                    if (unlink(full) != 0)
                        d99_warn("cannot remove %s: %s", full, strerror(errno));
                }
            }
            free(full);
        }
        free(line);
        if (!eol)
            break;
        p = eol + 1;
    }
    free(text);

    /* remove now-empty directories, deepest first (reverse alphabetical) */
    d99_sv_sort(&dirs);
    for (i = dirs.n; i > 0; i--) {
        char *full = d99_path_join(c->root, dirs.v[i - 1]);
        rmdir(full);   /* best effort */
        free(full);
    }
    d99_sv_free(&conffiles);
    d99_sv_free(&dirs);
    return 0;
}

static int inst_remove(struct d99_ctx *c, const char *name)
{
    d99_pkg *p = d99_db_find(c->db, name);
    char *args[2];
    int next;

    if (!p || p->state == D99_PS_NOTINSTALLED || p->state == D99_PS_CONFIGFILES) {
        printf("Package '%s' is not installed, so not removed\n", name);
        return 0;
    }
    if (isatty(STDOUT_FILENO))
        printf("\r\033[K");
    printf("Removing %s (%s) ...\n", p->name, p->version ? p->version : "");
    if (d99_state_next(p->state, D99_EVT_REMOVE, &next) != 0) {
        fprintf(stderr, "d99-inst: cannot remove '%s' in state '%s'\n",
                name, d99_state_name(p->state));
        return 1;
    }
    args[0] = (char *)"remove";
    args[1] = NULL;
    if (hook_run(c, p->name, p->version, p->arch, "prerm", args) != 0) {
        fprintf(stderr, "d99-inst: prerm failure for %s; aborting\n", name);
        return 1;
    }
    remove_files(c, name, 1);
    {
        static const char *drop[] = { "preinst", "prerm", "postinst",
                                      "triggers", "shlibs", "templates",
                                      "config", NULL };
        int i;
        for (i = 0; drop[i]; i++) {
            char *sfx = d99_xasprintf(".%s", drop[i]);
            char *path = d99_info_path(c->admindir, name, sfx);
            unlink(path);
            free(path);
            free(sfx);
        }
    }
    args[0] = (char *)"remove";
    args[1] = NULL;
    if (hook_run(c, p->name, p->version, p->arch, "postrm", args) != 0)
        d99_warn("postrm for %s failed", name);
    d99_pkg_set_status(c->db, p, D99_SEL_DEINSTALL, D99_PS_CONFIGFILES,
                       D99_PF_OK);
    if (d99_db_save_status(c->db, c->admindir) != 0)
        d99_warn("cannot save status database");
    if (d99_index_build(c->admindir, c->indexpath) != 0)
        d99_verbose("index rebuild skipped");
    return 0;
}

static int inst_purge(struct d99_ctx *c, const char *name)
{
    d99_pkg *p = d99_db_find(c->db, name);
    d99_strvec conffiles;
    size_t i;

    if (!p || p->state == D99_PS_NOTINSTALLED) {
        printf("Package '%s' is not installed, so not purged\n", name);
        return 0;
    }
    if (isatty(STDOUT_FILENO))
        printf("\r\033[K");
    printf("Purging configuration files for %s (%s) ...\n", p->name, p->version ? p->version : "");
    if (p->state != D99_PS_CONFIGFILES && p->state != D99_PS_NOTINSTALLED) {
        char *args[2];
        /* fully remove first (policy: prerm remove, then purge conffiles) */
        args[0] = (char *)"remove";
        args[1] = NULL;
        if (hook_run(c, p->name, p->version, p->arch, "prerm", args) != 0) {
            fprintf(stderr, "d99-inst: prerm failure for %s; aborting\n", name);
            return 1;
        }
        remove_files(c, name, 1);
    }

    /* delete conffiles and their dpkg variants */
    d99_sv_init(&conffiles);
    {
        size_t cl = 0;
        char *ct = d99_info_read(c->admindir, name, ".conffiles", &cl);
        if (ct) {
            d99_arena *ar = d99_arena_new();
            d99_conffiles_parse(ct, &conffiles, ar);
            d99_arena_free(ar);
            free(ct);
        }
    }
    for (i = 0; i < conffiles.n; i++) {
        char *full = d99_path_join(c->root, conffiles.v[i]);
        unlink(full);
        {
            static const char *suf[] = { ".dpkg-new", ".dpkg-old",
                                         ".dpkg-dist", ".dpkg-bak", NULL };
            int k;
            for (k = 0; suf[k]; k++) {
                char *v = d99_xasprintf("%s%s", full, suf[k]);
                unlink(v);
                free(v);
            }
        }
        free(full);
    }
    d99_sv_free(&conffiles);
    remove_files(c, name, 0);

    {
        char *args[2];
        args[0] = (char *)"purge";
        args[1] = NULL;
        if (hook_run(c, p->name, p->version, p->arch, "postrm", args) != 0)
            d99_warn("postrm purge for %s failed", name);
    }
    d99_db_remove(c->db, p);
    if (d99_db_save_status(c->db, c->admindir) != 0)
        d99_warn("cannot save status database");
    d99_info_delete_all(c->admindir, name);
    if (d99_index_build(c->admindir, c->indexpath) != 0)
        d99_verbose("index rebuild skipped");
    return 0;
}

/* ================= main =============================================== */

static void usage(void)
{
    fputs(
"Usage: d99-inst [<option>...] <action>\n"
"\n"
"Actions:\n"
"  -i|--install <deb>...        unpack and configure packages\n"
"  --unpack <deb>...            unpack only\n"
"  --configure <pkg>...          run postinst and configure\n"
"  -r|--remove <pkg>...         remove (keep conffiles)\n"
"  -P|--purge <pkg>...          remove everything\n"
"  --triggers-only               process pending triggers\n"
"  --print-architecture          print the host Debian architecture\n"
"\n"
"Options:\n"
"  --root=<dir>                  chroot: sets both instdir and admindir\n"
"  --instdir=<dir>               filesystem target\n"
"  --admindir=<dir>              package database directory\n"
"  --force-depends               ignore dependency problems\n"
"  --force-hold                  allow operations on held packages\n"
"  -E|--skip-same-version        skip if same version installed\n"
"  --no-triggers                 do not run pending triggers\n"
"  --add-trigger <name> [args]   record a pending trigger (d99 ext)\n"
"  --version | --help\n",
        stdout);
}

enum action { A_NONE, A_INSTALL, A_UNPACK, A_CONFIGURE, A_REMOVE, A_PURGE,
              A_TRIGGERS_ONLY, A_ADD_TRIGGER, A_PRINT_ARCH };

static void delegate_to_query(char *const argv[])
{
    if (d99_fallback_exists(argv[0]))
        d99_fallback_exec(argv[0], argv);
    if (d99_file_exists("/usr/local/bin/d99-query"))
        execv("/usr/local/bin/d99-query", argv);
    if (d99_file_exists("/usr/bin/dpkg-query"))
        execv("/usr/bin/dpkg-query", argv);
    execvp("d99-query", argv);
    execvp("dpkg-query", argv);
    d99_die("cannot execute d99-query for query action");
}

static void delegate_to_deb(char *const argv[])
{
    if (d99_fallback_exists(argv[0]))
        d99_fallback_exec(argv[0], argv);
    if (d99_file_exists("/usr/local/bin/d99-deb"))
        execv("/usr/local/bin/d99-deb", argv);
    if (d99_file_exists("/usr/bin/dpkg-deb"))
        execv("/usr/bin/dpkg-deb", argv);
    execvp("d99-deb", argv);
    execvp("dpkg-deb", argv);
    d99_die("cannot execute d99-deb for deb action");
}

int main(int argc, char **argv)
{
    struct d99_ctx c;
    enum action act = A_NONE;
    d99_strvec ops;
    char *root = NULL, *instdir = NULL, *admindir = NULL;
    const char *trigger_name = NULL, *trigger_args = NULL;
    int no_triggers = 0, skip_same = 0;
    int force_hold = 0;
    int i, rc = 0;

    d99_sv_init(&ops);
    memset(&c, 0, sizeof c);
    c.force_depends = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
            for (i++; i < argc; i++)
                d99_sv_push(&ops, argv[i]);
            break;
        }
        if (a[0] == '-' && a[1]) {
            if (a[1] == '-') {
                if (strcmp(a, "--install") == 0)
                    act = A_INSTALL;
                else if (strcmp(a, "--unpack") == 0)
                    act = A_UNPACK;
                else if (strcmp(a, "--configure") == 0)
                    act = A_CONFIGURE;
                else if (strcmp(a, "--remove") == 0)
                    act = A_REMOVE;
                else if (strcmp(a, "--purge") == 0)
                    act = A_PURGE;
                else if (strcmp(a, "--triggers-only") == 0)
                    act = A_TRIGGERS_ONLY;
                else if (strcmp(a, "--print-architecture") == 0) {
                    printf("%s\n", d99_host_arch());
                    return 0;
                } else if (strcmp(a, "--add-trigger") == 0) {
                    act = A_ADD_TRIGGER;
                    if (i + 1 < argc)
                        trigger_name = argv[++i];
                    if (i + 1 < argc && argv[i + 1][0] != '-')
                        trigger_args = argv[++i];
                } else if (strncmp(a, "--root=", 7) == 0)
                    root = d99_xstrdup(a + 7);
                else if (strcmp(a, "--root") == 0 && i + 1 < argc)
                    root = d99_xstrdup(argv[++i]);
                else if (strncmp(a, "--instdir=", 10) == 0)
                    instdir = d99_xstrdup(a + 10);
                else if (strncmp(a, "--admindir=", 11) == 0)
                    admindir = d99_xstrdup(a + 11);
                else if (strcmp(a, "--force-depends") == 0)
                    c.force_depends = 1;
                else if (strcmp(a, "--force-hold") == 0)
                    force_hold = 1;
                else if (strcmp(a, "--force-configure") == 0)
                    c.force_configure = 1;
                else if (strcmp(a, "--no-triggers") == 0)
                    no_triggers = 1;
                else if (strcmp(a, "--skip-same-version") == 0)
                    skip_same = 1;
                else if (strcmp(a, "--verbose") == 0)
                    d99_set_verbose(1);
                else if (strcmp(a, "--version") == 0) {
                    printf("d99-inst (d99) %s\n", D99_VERSION);
                    return 0;
                } else if (strcmp(a, "--help") == 0) {
                    usage();
                    return 0;
                } else if (strcmp(a, "--listfiles") == 0 ||
                           strcmp(a, "--status") == 0 ||
                           strcmp(a, "--list") == 0 ||
                           strcmp(a, "--search") == 0 ||
                           strcmp(a, "--print-avail") == 0 ||
                           strcmp(a, "--show") == 0) {
                    delegate_to_query(argv);
                } else if (strcmp(a, "--contents") == 0 ||
                           strcmp(a, "--info") == 0 ||
                           strcmp(a, "--extract") == 0 ||
                           strcmp(a, "--vextract") == 0 ||
                           strcmp(a, "--control") == 0 ||
                           strcmp(a, "--build") == 0) {
                    delegate_to_deb(argv);
                } else if (strcmp(a, "--dry-run") == 0 ||
                           strcmp(a, "--verify") == 0 ||
                           strcmp(a, "--audit") == 0 ||
                           strcmp(a, "--yet-to-unpack") == 0 ||
                           strcmp(a, "--update-avail") == 0 ||
                           strcmp(a, "--merge-avail") == 0) {
                    /* recognized upstream actions we deliberately delegate */
                    d99_fallback_or_die(argv[0], argv);
                } else {
                    d99_fallback_or_die(argv[0], argv);
                }
            } else {
                const char *p;
                for (p = a + 1; *p; p++) {
                    switch (*p) {
                    case 'i': act = A_INSTALL; break;
                    case 'r': act = A_REMOVE; break;
                    case 'P': act = A_PURGE; break;
                    case 'E': skip_same = 1; break;
                    case 'v': d99_set_verbose(1); break;
                    case 'L':
                    case 'l':
                    case 's':
                    case 'S':
                    case 'p':
                    case 'W':
                        delegate_to_query(argv);
                        break;
                    case 'c':
                    case 'I':
                    case 'x':
                    case 'X':
                    case 'e':
                    case 'b':
                        delegate_to_deb(argv);
                        break;
                    default:
                        d99_fallback_or_die(argv[0], argv);
                    }
                }
            }
        } else {
            d99_sv_push(&ops, a);
        }
    }

    /* resolve the three roots (c.* members own their allocations) */
    c.root = d99_xstrdup(instdir ? instdir : (root ? root : "/"));
    if (admindir)
        c.admindir = d99_xstrdup(admindir);
    else
        c.admindir = d99_path_join(root ? root : "/", "var/lib/dpkg");
    c.indexpath = d99_index_path_for(c.admindir);
    (void)skip_same;
    (void)force_hold;   /* hold check honors this via c (see below) */

    if (strcmp(c.root, "/") == 0 && geteuid() != 0) {
        fprintf(stderr, "d99-inst: requested operation requires superuser "
                "privilege\n");
        return 2;
    }
    if (d99_mkdir_p(d99_path_join(c.admindir, "info"), 0755) != 0) {
        fprintf(stderr, "d99-inst: cannot create admin dir %s\n", c.admindir);
        return 1;
    }
    if (!d99_file_exists(d99_path_join(c.admindir, "status"))) {
        /* fresh database */
        char *st = d99_path_join(c.admindir, "status");
        d99_write_file_atomic(st, "", 0);
        free(st);
    }

    c.db = d99_db_load_status(c.admindir);
    if (!c.db) {
        fprintf(stderr, "d99-inst: cannot load status database from %s\n",
                c.admindir);
        return 1;
    }
    c.div = diversions_load(c.admindir);
    if (force_hold) {
        /* release holds for this transaction */
        size_t k;
        for (k = 0; k < d99_db_count(c.db); k++) {
            d99_pkg *p = d99_db_at(c.db, k);
            if (p->sel == D99_SEL_HOLD)
                p->sel = D99_SEL_INSTALL;
        }
    }

    switch (act) {
    case A_ADD_TRIGGER:
        if (!trigger_name) {
            fprintf(stderr, "d99-inst: --add-trigger needs a name\n");
            rc = 2;
            break;
        }
        triggers_add(&c, trigger_name, trigger_args);
        rc = 0;
        break;
    case A_TRIGGERS_ONLY:
        rc = triggers_process(&c);
        break;
    case A_INSTALL:
    case A_UNPACK: {
        /* pre-parse every control stanza for batch-aware dependency checks */
        d99_arena *bar = d99_arena_new();
        d99_pkg **batch = d99_xmalloc((ops.n ? ops.n : 1) * sizeof(d99_pkg *));
        size_t nbatch = 0, k;

        for (k = 0; k < ops.n; k++) {
            d99_ar *ar;
            d99_ar_member ctrl, data;
            d99_decomp *d;
            d99_tarr *t;
            d99_tar_member m;

            if (open_deb(ops.v[k], &ar, &ctrl, &data) != 0) {
                rc = 1;
                continue;
            }
            d = member_decomp(ar, &ctrl);
            if (d) {
                t = d99_tar_open_decomp(d);
                while (d99_tar_next(t, &m) == 1) {
                    char nm[4096];
                    norm_name(m.name, nm, sizeof nm);
                    if (strcmp(nm, "control") == 0 &&
                        (m.typeflag == '0' || m.typeflag == '\0')) {
                        char *buf = d99_xmalloc((size_t)m.size + 1);
                        if (d99_tar_read_data(t, buf, (size_t)m.size) == 0) {
                            d99_pkg *bp;
                            buf[m.size] = '\0';
                            bp = d99_pkg_parse_stanza(buf, (size_t)m.size, bar);
                            if (bp)
                                batch[nbatch++] = bp;
                        }
                        free(buf);
                        break;
                    }
                    d99_tar_skip(t);
                }
                d99_tar_close_read(t);
            }
            d99_ar_close(ar);
        }

        size_t total_steps = ops.n + (act == A_INSTALL ? nbatch : 0);
        size_t cur_step = 0;

        for (k = 0; k < ops.n; k++) {
            if (inst_unpack_deb(&c, ops.v[k], batch, nbatch) != 0)
                rc = 1;
            cur_step++;
            print_dpkg_progress(cur_step, total_steps);
        }

        if (act == A_INSTALL) {
            /* configure in argument order */
            for (k = 0; k < nbatch; k++) {
                if (batch[k]->name) {
                    if (inst_configure(&c, batch[k]->name) != 0)
                        rc = 1;
                    cur_step++;
                    print_dpkg_progress(cur_step, total_steps);
                }
            }
        }
        if (!no_triggers && act == A_INSTALL)
            if (triggers_process(&c) != 0)
                rc = 1;
        d99_arena_free(bar);
        free(batch);
        break;
    }
    case A_CONFIGURE: {
        if (ops.n == 0) {
            fprintf(stderr, "d99-inst: --configure needs a package name\n");
            rc = 2;
            break;
        }
        size_t total_steps = ops.n;
        for (i = 0; (size_t)i < ops.n; i++) {
            if (inst_configure(&c, ops.v[i]) != 0)
                rc = 1;
            print_dpkg_progress(i + 1, total_steps);
        }
        if (!no_triggers)
            if (triggers_process(&c) != 0)
                rc = 1;
        break;
    }
    case A_REMOVE:
    case A_PURGE: {
        if (ops.n == 0) {
            fprintf(stderr, "d99-inst: needs a package name\n");
            rc = 2;
            break;
        }
        size_t total_steps = ops.n;
        for (i = 0; (size_t)i < ops.n; i++) {
            if (act == A_PURGE) {
                if (inst_purge(&c, ops.v[i]) != 0)
                    rc = 1;
            } else {
                if (inst_remove(&c, ops.v[i]) != 0)
                    rc = 1;
            }
            print_dpkg_progress(i + 1, total_steps);
        }
        break;
    }
    default:
        usage();
        rc = 2;
        break;
    }

    d99_sv_free(&ops);
    diversions_free(c.div);
    d99_db_free(c.db);
    free(c.root);
    free(c.admindir);
    free(c.indexpath);
    free(root);
    free(instdir);
    free(admindir);
    return rc;
}
