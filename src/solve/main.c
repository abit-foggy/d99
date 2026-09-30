#include "solve.h"
#include "d99_sat.h"
#include "d99_fallback.h"

#include <ctype.h>
#include <dirent.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ==================== paths ==================== */

typedef struct {
    char *root, *lists_dir, *cache_dir, *sources_file, *sources_dir, *admindir;
} paths;

static void paths_init(paths *p, const char *root)
{
    p->root = d99_xstrdup(root);
    p->lists_dir = d99_path_join(root, "var/lib/d99/lists");
    p->cache_dir = d99_path_join(root, "var/cache/d99/archives");
    p->sources_file = d99_path_join(root, "etc/apt/sources.list");
    p->sources_dir = d99_path_join(root, "etc/apt/sources.list.d");
    p->admindir = d99_path_join(root, "var/lib/dpkg");
}

/* ==================== sources.list ==================== */

typedef struct {
    char *uri, *suite;
    d99_strvec comps;
} source_entry;

static void parse_sources_file(const char *path, source_entry **v, size_t *n,
                               size_t *cap)
{
    size_t len = 0;
    char *text = d99_read_file(path, &len);
    const char *p;

    if (!text)
        return;
    p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_xstrndup(p, ln);
        char *t = d99_trim(line);
        char *save = NULL;
        char *tok;

        if (*t && *t != '#') {
            tok = strtok_r(t, " \t", &save);
            if (tok && strcmp(tok, "deb") == 0) {
                tok = strtok_r(NULL, " \t", &save);
                if (tok && tok[0] == '[') {
                    while (tok && !strchr(tok, ']'))
                        tok = strtok_r(NULL, " \t", &save);
                    if (tok)
                        tok = strtok_r(NULL, " \t", &save);
                }
                if (tok && (strncmp(tok, "http://", 7) == 0 ||
                            strncmp(tok, "https://", 8) == 0 ||
                            strncmp(tok, "file://", 7) == 0)) {
                    source_entry e;
                    size_t ulen;
                    memset(&e, 0, sizeof e);
                    d99_sv_init(&e.comps);
                    e.uri = d99_xstrdup(tok);
                    ulen = strlen(e.uri);
                    while (ulen > 0 && e.uri[ulen - 1] == '/') {
                        if (ulen >= 3 && e.uri[ulen - 2] == '/' && e.uri[ulen - 3] == ':')
                            break;
                        e.uri[--ulen] = '\0';
                    }
                    tok = strtok_r(NULL, " \t", &save);
                    if (tok)
                        e.suite = d99_xstrdup(tok);
                    while ((tok = strtok_r(NULL, " \t", &save)) != NULL)
                        d99_sv_push(&e.comps, tok);
                    if (*n == *cap) {
                        *cap = *cap ? *cap * 2 : 8;
                        *v = d99_xrealloc(*v, *cap * sizeof(source_entry));
                    }
                    (*v)[(*n)++] = e;
                }
            }
        }
        free(line);
        if (!eol)
            break;
        p = eol + 1;
    }
    free(text);
}

static source_entry *parse_all_sources(paths *p, size_t *nout)
{
    source_entry *v = NULL;
    size_t n = 0, cap = 0;
    DIR *d;

    parse_sources_file(p->sources_file, &v, &n, &cap);
    d = opendir(p->sources_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t nl = strlen(de->d_name);
            if (nl < 6 || strcmp(de->d_name + nl - 5, ".list") != 0)
                continue;
            {
                char *full = d99_path_join(p->sources_dir, de->d_name);
                parse_sources_file(full, &v, &n, &cap);
                free(full);
            }
        }
        closedir(d);
    }
    *nout = n;
    return v;
}

static void free_sources(source_entry *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        free(v[i].uri);
        free(v[i].suite);
        d99_sv_free(&v[i].comps);
    }
    free(v);
}

/* ==================== update ==================== */

static int fetch_index(const char *base, const char *repo_root,
                       const char *lists_dir, FILE *mf)
{
    static const char *suffixes[] = { ".xz", ".zst", ".gz", "" };
    int s;

    for (s = 0; s < 4; s++) {
        char *url, *dest, name[128], hex[17];
        uint64_t h;

        if (s == 0 && !d99_comp_support(D99_CFMT_XZ))
            continue;
        if (s == 1 && !d99_comp_support(D99_CFMT_ZST))
            continue;
        if (s == 2 && !d99_comp_support(D99_CFMT_GZ))
            continue;
        url = d99_xasprintf("%s/Packages%s", base, suffixes[s]);
        h = d99_fnv1a64_str(url);
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
        snprintf(name, sizeof name, "d99_%s_Packages%s", hex, suffixes[s]);
        dest = d99_path_join(lists_dir, name);
        d99_verbose("fetching %s", url);
        if (d99_fetch(url, dest, 1) == 0) {
            printf("Get: %s\n", url);
            fprintf(mf, "%s\t%s\n", name, repo_root);
            free(dest);
            free(url);
            return 0;
        }
        free(dest);
        free(url);
    }
    return -1;
}

static int cmd_update(paths *p, const char *arch)
{
    source_entry *ents;
    size_t nents, i, c;
    char *manifest;
    FILE *mf;

    d99_mkdir_p(p->lists_dir, 0755);
    ents = parse_all_sources(p, &nents);
    if (nents == 0) {
        fprintf(stderr, "d99-solve: no usable deb entries (checked %s and "
                "%s)\n", p->sources_file, p->sources_dir);
        return 1;
    }
    manifest = d99_path_join(p->lists_dir, "d99_manifest");
    mf = fopen(manifest, "wb");
    if (!mf) {
        fprintf(stderr, "d99-solve: cannot write %s\n", manifest);
        free_sources(ents, nents);
        return 1;
    }

    for (i = 0; i < nents; i++) {
        source_entry *e = &ents[i];
        int flat = (e->comps.n == 0) ||
                  (e->suite && strcmp(e->suite, "./") == 0);
        int any = 0;

        if (flat) {
            if (fetch_index(e->uri, e->uri, p->lists_dir, mf) == 0)
                any = 1;
        } else {
            for (c = 0; c < e->comps.n; c++) {
                char *base = d99_xasprintf("%s/dists/%s/%s/binary-%s",
                                           e->uri, e->suite, e->comps.v[c],
                                           arch);
                if (fetch_index(base, e->uri, p->lists_dir, mf) == 0)
                    any = 1;
                free(base);

                if (strcmp(arch, "all") != 0) {
                    char *base_all = d99_xasprintf("%s/dists/%s/%s/binary-all",
                                                   e->uri, e->suite, e->comps.v[c]);
                    if (fetch_index(base_all, e->uri, p->lists_dir, mf) == 0)
                        any = 1;
                    free(base_all);
                }
            }
        }
        if (!any)
            fprintf(stderr, "d99-solve: no Packages index found for %s\n",
                    e->uri);
    }
    fclose(mf);
    chmod(manifest, 0644);
    free(manifest);
    free_sources(ents, nents);
    repo_build_index(p->lists_dir);
    printf("d99-solve: index update complete\n");
    return 0;
}

/* ==================== SAT transaction planner ==================== */

typedef struct {
    int is_installed;
    d99_cand *cand;
    d99_pkg *ipkg;
    const char *name;
    const char *version;
    int var;
} pnode;

/* name -> node index multimap */
typedef struct {
    char **keys;
    int **vals;
    int *vn;
    int *vcap;
    size_t cap;
} multimap;

static void mm_init(multimap *m, size_t want)
{
    m->cap = 256;
    while (m->cap < want * 2)
        m->cap <<= 1;
    m->keys = d99_xcalloc(m->cap, sizeof(char *));
    m->vals = d99_xcalloc(m->cap, sizeof(int *));
    m->vn = d99_xcalloc(m->cap, sizeof(int));
    m->vcap = d99_xcalloc(m->cap, sizeof(int));
}

static void mm_free(multimap *m)
{
    size_t j;
    for (j = 0; j < m->cap; j++) {
        free(m->keys[j]);
        free(m->vals[j]);
    }
    free(m->keys);
    free(m->vals);
    free(m->vn);
    free(m->vcap);
}

static void mm_put(multimap *m, const char *key, int val)
{
    uint64_t h = d99_fnv1a64_str(key);
    size_t j = (size_t)h & (m->cap - 1);

    while (m->keys[j]) {
        if (strcmp(m->keys[j], key) == 0)
            break;
        j = (j + 1) & (m->cap - 1);
    }
    if (!m->keys[j]) {
        m->keys[j] = d99_xstrdup(key);
        m->vals[j] = NULL;
        m->vn[j] = 0;
        m->vcap[j] = 0;
    }
    if (m->vn[j] == m->vcap[j]) {
        m->vcap[j] = m->vcap[j] ? m->vcap[j] * 2 : 4;
        m->vals[j] = d99_xrealloc(m->vals[j],
                                  (size_t)m->vcap[j] * sizeof(int));
    }
    m->vals[j][m->vn[j]++] = val;
}

static int mm_get(multimap *m, const char *key, int **out)
{
    uint64_t h = d99_fnv1a64_str(key);
    size_t j = (size_t)h & (m->cap - 1);
    while (m->keys[j]) {
        if (strcmp(m->keys[j], key) == 0) {
            *out = m->vals[j];
            return m->vn[j];
        }
        j = (j + 1) & (m->cap - 1);
    }
    *out = NULL;
    return 0;
}

static int node_satisfies(pnode *nd, const char *name, int op, const char *ver)
{
    if (nd->is_installed) {
        size_t i, j;
        if (strcmp(nd->name, name) == 0 &&
            (op == D99_DEP_NONE || !ver ||
             d99_verrel(d99_vercmp(nd->version, ver), op)))
            return 1;
        for (i = 0; i < nd->ipkg->provides.n; i++)
            for (j = 0; j < nd->ipkg->provides.g[i].n; j++) {
                d99_depalternative *pa = &nd->ipkg->provides.g[i].alts[j];
                if (strcmp(pa->name, name) == 0 &&
                    (op == D99_DEP_NONE || !ver ||
                     (pa->ver && d99_verrel(d99_vercmp(pa->ver, ver), op))))
                    return 1;
            }
        return 0;
    }
    return cand_satisfies(nd->cand, name, op, ver);
}

static int installed_deps_satisfied(d99_db *db, d99_pkg *p)
{
    d99_deplist *dls[2];
    int di;
    size_t k, j;

    dls[0] = &p->predepends;
    dls[1] = &p->depends;
    for (di = 0; di < 2; di++) {
        for (k = 0; k < dls[di]->n; k++) {
            d99_depgroup *g = &dls[di]->g[k];
            int group_sat = 0;
            for (j = 0; j < g->n; j++) {
                d99_depalternative *alt = &g->alts[j];
                size_t pidx;
                if (!d99_arch_ok(alt->arch, d99_host_arch()))
                    continue;
                for (pidx = 0; pidx < d99_db_count(db); pidx++) {
                    d99_pkg *cand_pkg = d99_db_at(db, pidx);
                    if (cand_pkg->state != D99_PS_INSTALLED)
                        continue;
                    if (strcmp(cand_pkg->name, alt->name) == 0 &&
                        (alt->op == D99_DEP_NONE || !alt->ver ||
                         d99_verrel(d99_vercmp(cand_pkg->version, alt->ver), alt->op))) {
                        group_sat = 1;
                        break;
                    }
                    /* check provides */
                    size_t pi, pj;
                    for (pi = 0; pi < cand_pkg->provides.n; pi++) {
                        for (pj = 0; pj < cand_pkg->provides.g[pi].n; pj++) {
                            d99_depalternative *pa = &cand_pkg->provides.g[pi].alts[pj];
                            if (strcmp(pa->name, alt->name) == 0 &&
                                (alt->op == D99_DEP_NONE || !alt->ver ||
                                 (pa->ver && d99_verrel(d99_vercmp(pa->ver, alt->ver), alt->op)))) {
                                group_sat = 1;
                                break;
                            }
                        }
                        if (group_sat) break;
                    }
                    if (group_sat) break;
                }
                if (group_sat) break;
            }
            if (!group_sat)
                return 0;
        }
    }
    return 1;
}

struct target_spec {
    char *name;
    int op;
    char *ver;
};

static int parse_target(const char *s, struct target_spec *t)
{
    char *eq = strpbrk(s, "=<>");

    t->op = D99_DEP_NONE;
    t->ver = NULL;
    if (!eq) {
        t->name = d99_xstrdup(s);
        return 0;
    }
    t->name = d99_xstrndup(s, (size_t)(eq - s));
    if (eq[0] == '=' && eq[1] != '>') {
        t->op = D99_DEP_EQ;
    } else if (eq[0] == '>' && eq[1] == '=') {
        t->op = D99_DEP_GE;
    } else if (eq[0] == '<' && eq[1] == '=') {
        t->op = D99_DEP_LE;
    } else {
        fprintf(stderr, "d99-solve: bad version constraint in '%s'\n", s);
        free(t->name);
        return -1;
    }
    t->ver = d99_xstrdup(strchr(eq, '=') + 1);
    return 0;
}

typedef struct {
    char *name, *version, *arch;
    char *filename, *sha256, *size, *base_uri, *summary;
    char *old_version;   /* NULL for a fresh install */
} action;

static void free_actions(action *a, size_t n)
{
    size_t i;
    if (!a)
        return;
    for (i = 0; i < n; i++) {
        free(a[i].name);
        free(a[i].version);
        free(a[i].arch);
        free(a[i].filename);
        free(a[i].sha256);
        free(a[i].size);
        free(a[i].base_uri);
        free(a[i].summary);
        free(a[i].old_version);
    }
    free(a);
}

static int solve_with(d99_sat *sat, int *base, int nbase, int *bans, int nbans,
                      int extra, int use_extra)
{
    int n = nbase + nbans + (use_extra ? 1 : 0);
    int *assume;
    int i, r;

    if (n == 0)
        return d99_sat_solve(sat, NULL, 0);
    assume = d99_xmalloc((size_t)n * sizeof(int));
    for (i = 0; i < nbase; i++)
        assume[i] = base[i];
    for (i = 0; i < nbans; i++)
        assume[nbase + i] = bans[i];
    if (use_extra)
        assume[nbase + nbans] = extra;
    r = d99_sat_solve(sat, assume, n);
    free(assume);
    return r;
}

static int plan_install(const char *cmd_name, paths *p, struct target_spec *targets, int ntargets,
                        action **out_actions, size_t *out_n)
{
    d99_repo *repo = repo_load(p->lists_dir);
    d99_db *db = NULL;
    pnode *nodes = NULL;
    size_t nnodes = 0, ncap = 0, i, k, j;
    multimap byname, best;
    d99_sat *sat;
    int *base = NULL;
    int nbase = 0, basecap = 0;
    int *bans = NULL;
    int nbans = 0, bancap = 0;
    char *required = NULL;
    action *acts = NULL;
    size_t nacts = 0, actcap = 0;
    int rc = 0;
    int iter;

    if (repo->n == 0) {
        fprintf(stderr, "d99-solve: package lists are empty; "
                "run '%s update' first\n", cmd_name);
        repo_free(repo);
        return -1;
    }
    db = d99_db_load_status(p->admindir);
    if (!db)
        db = d99_db_new();

    /* ---- candidate pruning: collect candidates in targets' dependency closure ---- */
    mm_init(&best, 1024);
    {
        d99_strvec q;
        d99_sv_init(&q);
        for (i = 0; (int)i < ntargets; i++) {
            d99_cand *tc = repo_find(repo, targets[i].name, targets[i].op, targets[i].ver);
            if (tc) {
                int *dummy;
                if (mm_get(&best, tc->name, &dummy) == 0) {
                    mm_put(&best, tc->name, (int)tc->idx);
                    d99_sv_push(&q, tc->name);
                }
            }
        }
        while (q.n > 0) {
            char *pkg_name = d99_xstrdup(q.v[--q.n]);
            int *ids;
            int n = mm_get(&best, pkg_name, &ids);
            if (n > 0) {
                d99_cand *c = repo_get(repo, (size_t)ids[0]);
                cand_ensure_deps(c, repo->ar);
                d99_deplist *dls[2];
                int di;
                dls[0] = &c->predepends;
                dls[1] = &c->depends;
                for (di = 0; di < 2; di++) {
                    for (k = 0; k < dls[di]->n; k++) {
                        d99_depgroup *g = &dls[di]->g[k];
                        for (j = 0; j < g->n; j++) {
                            d99_depalternative *alt = &g->alts[j];
                            if (!d99_arch_ok(alt->arch, d99_host_arch()))
                                continue;
                            if (mm_get(&best, alt->name, &ids) == 0) {
                                d99_cand *dep_c = repo_find(repo, alt->name, alt->op, alt->ver);
                                if (dep_c && mm_get(&best, dep_c->name, &ids) == 0) {
                                    mm_put(&best, dep_c->name, (int)dep_c->idx);
                                    d99_sv_push(&q, dep_c->name);
                                }
                            }
                        }
                    }
                }
            }
            free(pkg_name);
        }
        d99_sv_free(&q);
    }

    /* ---- nodes: installed pseudo-nodes then repo nodes ---- */
    for (i = 0; i < d99_db_count(db); i++) {
        d99_pkg *pp = d99_db_at(db, i);
        if (pp->state != D99_PS_INSTALLED)
            continue;
        if (nnodes == ncap) {
            ncap = ncap ? ncap * 2 : 64;
            nodes = d99_xrealloc(nodes, ncap * sizeof(pnode));
        }
        memset(&nodes[nnodes], 0, sizeof(pnode));
        nodes[nnodes].is_installed = 1;
        nodes[nnodes].ipkg = pp;
        nodes[nnodes].name = pp->name;
        nodes[nnodes].version = pp->version ? pp->version : "";
        nnodes++;
    }
    for (j = 0; j < best.cap; j++) {
        if (best.keys[j]) {
            d99_cand *c = repo_get(repo, (size_t)best.vals[j][0]);
            if (nnodes == ncap) {
                ncap = ncap ? ncap * 2 : 64;
                nodes = d99_xrealloc(nodes, ncap * sizeof(pnode));
            }
            memset(&nodes[nnodes], 0, sizeof(pnode));
            nodes[nnodes].is_installed = 0;
            nodes[nnodes].cand = c;
            nodes[nnodes].name = c->name;
            nodes[nnodes].version = c->version;
            nnodes++;
        }
    }
    mm_free(&best);

    /* ---- name/provides multimap ---- */
    mm_init(&byname, nnodes + 16);
    for (i = 0; i < nnodes; i++) {
        mm_put(&byname, nodes[i].name, (int)i);
        {
            d99_deplist *dl;
            if (nodes[i].is_installed) {
                dl = &nodes[i].ipkg->provides;
            } else {
                cand_ensure_deps(nodes[i].cand, repo->ar);
                dl = &nodes[i].cand->provides;
            }
            for (k = 0; k < dl->n; k++) {
                for (j = 0; j < dl->g[k].n; j++) {
                    const char *pname = dl->g[k].alts[j].name;
                    if (strcmp(pname, nodes[i].name) != 0)
                        mm_put(&byname, pname, (int)i);
                }
            }
        }
    }

    /* ---- SAT variables (node i gets var i) ---- */
    sat = d99_sat_new();
    for (i = 0; i < nnodes; i++)
        nodes[i].var = d99_sat_var(sat);

    for (i = 0; i < nnodes; i++) {
        d99_deplist *dls[2];
        int di;
        if (nodes[i].is_installed) {
            if (installed_deps_satisfied(db, nodes[i].ipkg))
                continue;
            dls[0] = &nodes[i].ipkg->predepends;
            dls[1] = &nodes[i].ipkg->depends;
        } else {
            cand_ensure_deps(nodes[i].cand, repo->ar);
            dls[0] = &nodes[i].cand->predepends;
            dls[1] = &nodes[i].cand->depends;
        }
        for (di = 0; di < 2; di++) {
            for (k = 0; k < dls[di]->n; k++) {
                d99_depgroup *g = &dls[di]->g[k];
                int lits[512];
                int nl = 0;
                lits[nl++] = D99_LIT_NEG(nodes[i].var);
                for (j = 0; j < g->n; j++) {
                    d99_depalternative *alt = &g->alts[j];
                    int *ids;
                    int n, q;
                    if (!d99_arch_ok(alt->arch, d99_host_arch()))
                        continue;
                    n = mm_get(&byname, alt->name, &ids);
                    for (q = 0; q < n && nl < 512; q++) {
                        if (node_satisfies(&nodes[ids[q]], alt->name,
                                           alt->op, alt->ver)) {
                            int pos_lit = D99_LIT_POS(nodes[ids[q]].var);
                            int dup = 0;
                            int l;
                            for (l = 0; l < nl; l++) {
                                if (lits[l] == pos_lit) {
                                    dup = 1;
                                    break;
                                }
                            }
                            if (!dup)
                                lits[nl++] = pos_lit;
                        }
                    }
                }
                d99_sat_clause(sat, lits, nl);
            }
        }
        if (!nodes[i].is_installed) {
            d99_deplist *dls_c[2];
            int di_c;
            dls_c[0] = &nodes[i].cand->conflicts;
            dls_c[1] = &nodes[i].cand->breaks;
            for (di_c = 0; di_c < 2; di_c++) {
                for (k = 0; k < dls_c[di_c]->n; k++) {
                    d99_depgroup *g = &dls_c[di_c]->g[k];
                    for (j = 0; j < g->n; j++) {
                        d99_depalternative *alt = &g->alts[j];
                        int *ids;
                        int n, q;
                        if (!d99_arch_ok(alt->arch, d99_host_arch()))
                            continue;
                        n = mm_get(&byname, alt->name, &ids);
                        for (q = 0; q < n; q++) {
                            pnode *nd = &nodes[ids[q]];
                            if (nd == &nodes[i] || nd->var == nodes[i].var)
                                continue;
                            if (node_satisfies(nd, alt->name, alt->op, alt->ver)) {
                                int pair[2];
                                pair[0] = D99_LIT_NEG(nodes[i].var);
                                pair[1] = D99_LIT_NEG(nd->var);
                                d99_sat_clause(sat, pair, 2);
                            }
                        }
                    }
                }
            }
        }
    }

    /* same-name exclusivity */
    for (j = 0; j < byname.cap; j++) {
        if (!byname.keys[j])
            continue;
        {
            int same[64];
            int nsame = 0;
            for (k = 0; (int)k < byname.vn[j] && nsame < 64; k++) {
                int var = nodes[byname.vals[j][k]].var;
                int exists = 0;
                size_t s;
                if (strcmp(nodes[byname.vals[j][k]].name, byname.keys[j]) != 0)
                    continue;
                for (s = 0; s < (size_t)nsame; s++) {
                    if (same[s] == var) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists)
                    same[nsame++] = var;
            }
            for (k = 0; k < (size_t)nsame; k++) {
                for (i = k + 1; i < (size_t)nsame; i++) {
                    if (same[k] == same[i])
                        continue;
                    int pair[2];
                    pair[0] = D99_LIT_NEG(same[k]);
                    pair[1] = D99_LIT_NEG(same[i]);
                    d99_sat_clause(sat, pair, 2);
                }
            }
        }
    }

    /* ---- resolve targets ---- */
    required = d99_xcalloc(nnodes ? nnodes : 1, 1);
    for (i = 0; i < (size_t)ntargets; i++) {
        struct target_spec *t = &targets[i];
        d99_cand *c = repo_find(repo, t->name, t->op, t->ver);
        d99_pkg *inst = d99_db_find(db, t->name);
        pnode *rnode = NULL;

        if (!c) {
            if (inst && inst->state == D99_PS_INSTALLED &&
                (t->op == D99_DEP_NONE || !t->ver ||
                 d99_verrel(d99_vercmp(inst->version, t->ver), t->op))) {
                printf("d99-solve: %s is already at the requested version.\n",
                       t->name);
                continue;
            }
            fprintf(stderr, "d99-solve: unable to locate package '%s'\n",
                    t->name);
            rc = -1;
            goto out;
        }
        for (k = 0; k < nnodes; k++)
            if (!nodes[k].is_installed && nodes[k].cand == c)
                rnode = &nodes[k];
        if (!rnode)
            continue;
        if (inst && inst->state == D99_PS_INSTALLED &&
            d99_vercmp(inst->version, c->version) == 0) {
            if (installed_deps_satisfied(db, inst)) {
                printf("d99-solve: %s %s is already installed.\n",
                       t->name, c->version);
                continue;
            }
        }
        if (nbase == basecap) {
            basecap = basecap ? basecap * 2 : 16;
            base = d99_xrealloc(base, (size_t)basecap * sizeof(int));
        }
        base[nbase++] = D99_LIT_POS(rnode->var);
        required[rnode - nodes] = 1;
    }
    if (nbase == 0) {
        printf("d99-solve: nothing to do\n");
        rc = 0;   /* nothing requested is not an error (apt-get semantics) */
        goto out;
    }

    /* keep every non-target installed package in place */
    for (i = 0; i < nnodes; i++) {
        int is_target = 0;
        if (!nodes[i].is_installed)
            continue;
        for (k = 0; k < (size_t)ntargets; k++)
            if (strcmp(targets[k].name, nodes[i].name) == 0)
                is_target = 1;
        if (is_target)
            continue;
        if (nbase == basecap) {
            basecap = basecap ? basecap * 2 : 16;
            base = d99_xrealloc(base, (size_t)basecap * sizeof(int));
        }
        base[nbase++] = D99_LIT_POS(nodes[i].var);
    }

    /* ---- solve (allow upgrades on the retry) ---- */
    if (solve_with(sat, base, nbase, NULL, 0, 0, 0) != 1) {
        int *tbase = NULL;
        int ntbase = 0, tcap = 0;
        d99_verbose("strict solve failed; retrying with upgrades allowed");
        for (i = 0; i < (size_t)nbase; i++) {
            if (nodes[base[i] >> 1].is_installed)
                continue;
            if (ntbase == tcap) {
                tcap = tcap ? tcap * 2 : 16;
                tbase = d99_xrealloc(tbase, (size_t)tcap * sizeof(int));
            }
            tbase[ntbase++] = base[i];
        }
        if (solve_with(sat, tbase, ntbase, NULL, 0, 0, 0) != 1) {
            fprintf(stderr, "d99-solve: the requested transaction is "
                    "unsatisfiable (dependency conflict)\n");
            free(tbase);
            rc = -1;
            goto out;
        }
        free(base);
        base = tbase;
        nbase = ntbase;
        basecap = tcap;
    }

    /* ---- greedy minimization (bounded) ---- */
    for (iter = 0; iter < 512; iter++) {
        int found = -1, kind = 0, lit = 0;

        if (solve_with(sat, base, nbase, bans, nbans, 0, 0) != 1) {
            fprintf(stderr, "d99-solve: internal minimization error\n");
            rc = -1;
            goto out;
        }
        for (i = 0; i < nnodes && found < 0; i++) {
            int is_target_name = 0;
            for (k = 0; k < (size_t)ntargets; k++)
                if (strcmp(targets[k].name, nodes[i].name) == 0)
                    is_target_name = 1;
            if (is_target_name || required[i])
                continue;
            if (!nodes[i].is_installed && d99_sat_value(sat, nodes[i].var) == 1) {
                found = (int)i;
                kind = 1;
                lit = D99_LIT_NEG(nodes[i].var);
            }
        }
        if (found < 0) {
            for (i = 0; i < nnodes; i++) {
                int is_target_name = 0;
                for (k = 0; k < (size_t)ntargets; k++)
                    if (strcmp(targets[k].name, nodes[i].name) == 0)
                        is_target_name = 1;
                if (is_target_name || required[i])
                    continue;
                if (nodes[i].is_installed &&
                    d99_sat_value(sat, nodes[i].var) == 0) {
                    found = (int)i;
                    kind = 2;
                    lit = D99_LIT_POS(nodes[i].var);
                }
            }
        }
        (void)kind;
        if (found < 0)
            break;
        if (solve_with(sat, base, nbase, bans, nbans, lit, 1) == 1) {
            if (nbans == bancap) {
                bancap = bancap ? bancap * 2 : 16;
                bans = d99_xrealloc(bans, (size_t)bancap * sizeof(int));
            }
            bans[nbans++] = lit;
        } else {
            required[found] = 1;
        }
    }

    /* ---- actions from the final model ---- */
    for (i = 0; i < nnodes; i++) {
        pnode *old = NULL;
        d99_cand *c;
        action *a;

        if (nodes[i].is_installed || d99_sat_value(sat, nodes[i].var) != 1)
            continue;
        for (k = 0; k < nnodes; k++)
            if (nodes[k].is_installed &&
                strcmp(nodes[k].name, nodes[i].name) == 0)
                old = &nodes[k];
        if (old && d99_sat_value(sat, old->var) == 1 &&
            d99_vercmp(old->version, nodes[i].version) == 0)
            continue;
        if (nacts == actcap) {
            actcap = actcap ? actcap * 2 : 16;
            acts = d99_xrealloc(acts, (size_t)actcap * sizeof(action));
        }
        c = nodes[i].cand;
        a = &acts[nacts];
        memset(a, 0, sizeof(*a));
        a->name = d99_xstrdup(c->name);
        a->version = d99_xstrdup(c->version);
        a->arch = d99_xstrdup(c->arch ? c->arch : "all");
        a->filename = d99_xstrdup(c->filename ? c->filename : "");
        a->sha256 = d99_xstrdup(c->sha256 ? c->sha256 : "");
        a->size = d99_xstrdup(c->size ? c->size : "0");
        a->base_uri = d99_xstrdup(c->base_uri ? c->base_uri : "");
        a->summary = d99_xstrdup(c->summary ? c->summary : "");
        a->old_version = old ? d99_xstrdup(old->version) : NULL;
        nacts++;
    }
    *out_actions = acts;
    *out_n = nacts;

out:
    if (rc != 0)
        free_actions(acts, nacts);
    mm_free(&byname);
    d99_sat_free(sat);
    free(nodes);
    free(base);
    free(bans);
    free(required);
    repo_free(repo);
    d99_db_free(db);
    return rc;
}

/* ==================== commands ==================== */

static int verify_sha(const char *path, const char *want)
{
    char hex[65];
    if (d99_sha256_file(path, hex) != 0)
        return 0;
    return strcasecmp(hex, want) == 0;
}

static int confirm(int yes)
{
    char buf[64];

    if (yes)
        return 1;
    if (!isatty(0)) {
        printf("d99-solve: non-interactive; proceeding\n");
        return 1;
    }
    printf("Continue? [Y/n] ");
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin))
        return 0;
    if (buf[0] == '\n' || buf[0] == '\0')
        return 1;
    return buf[0] == 'y' || buf[0] == 'Y';
}

static char *find_inst_tool(const char *argv0)
{
    const char *env = getenv("D99_INST");
    char *tmp, *dir, *cand;

    if (env && *env)
        return d99_xstrdup(env);
    tmp = d99_xstrdup(argv0 ? argv0 : "d99-solve");
    dir = dirname(tmp);
    cand = d99_xasprintf("%s/d99-inst", dir);
    free(tmp);
    if (d99_file_exists(cand))
        return cand;
    free(cand);
    if (d99_file_exists("/usr/local/bin/d99-inst"))
        return d99_xstrdup("/usr/local/bin/d99-inst");
    if (d99_file_exists("/usr/bin/dpkg"))
        return d99_xstrdup("/usr/bin/dpkg");
    return d99_xstrdup("d99-inst");
}

static int run_inst(char *const argv[])
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "d99-solve: cannot execute %s\n", argv[0]);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return 1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

static int cmd_install(const char *argv0, const char *cmd_name, paths *p,
                        struct target_spec *targets, int ntargets,
                        int yes, int download_only, int print_uris, int simulate)
{
    action *acts = NULL;
    size_t nacts = 0, i;
    long long total_size = 0;
    d99_strvec files;
    char *inst = find_inst_tool(argv0);
    char **av;
    int na = 0, rc;
    char *ro = NULL;

    if (plan_install(cmd_name, p, targets, ntargets, &acts, &nacts) != 0)
        return 1;
    if (nacts == 0) {
        printf("d99-solve: 0 newly installed; nothing to do\n");
        free_actions(acts, nacts);
        free(inst);
        return 0;
    }

    printf("The following changes will be applied:\n");
    for (i = 0; i < nacts; i++) {
        if (acts[i].old_version)
            printf("  upgrade %s (%s -> %s)\n", acts[i].name,
                   acts[i].old_version, acts[i].version);
        else
            printf("  install %s (%s)\n", acts[i].name, acts[i].version);
        total_size += atoll(acts[i].size);
    }
    printf("%lu package(s), %lld bytes of archives\n",
           (unsigned long)nacts, total_size);

    if (simulate) {
        free_actions(acts, nacts);
        free(inst);
        return 0;
    }

    if (!print_uris && !confirm(yes)) {
        printf("d99-solve: aborted\n");
        free_actions(acts, nacts);
        free(inst);
        return 1;
    }

    d99_mkdir_p(p->cache_dir, 0755);
    d99_sv_init(&files);
    rc = 0;
    {
        char **dests = d99_xcalloc(nacts, sizeof(char *));
        char **urls = d99_xcalloc(nacts, sizeof(char *));
        int *needed = d99_xcalloc(nacts, sizeof(int));
        size_t n_needed = 0;

        for (i = 0; i < nacts; i++) {
            action *a = &acts[i];
            if (!a->filename || !*a->filename) {
                fprintf(stderr, "d99-solve: %s has no Filename in the index\n",
                        a->name);
                rc = 1;
                break;
            }
            urls[i] = d99_xasprintf("%s/%s", a->base_uri, a->filename);
            dests[i] = d99_xasprintf("%s/%s_%s_%s.deb", p->cache_dir, a->name,
                                     a->version, a->arch);
            if (print_uris) {
                printf("'%s' %s\n", urls[i], dests[i]);
                needed[i] = 0;
            } else if (d99_file_exists(dests[i]) &&
                       (!*a->sha256 || verify_sha(dests[i], a->sha256))) {
                needed[i] = 0;
            } else {
                needed[i] = 1;
                n_needed++;
            }
        }

        if (rc == 0 && !print_uris && n_needed > 0) {
            #define D99_MAX_DOWNLOAD_WORKERS 4
            struct {
                pid_t pid;
                size_t idx;
            } workers[D99_MAX_DOWNLOAD_WORKERS];
            size_t n_running = 0;
            size_t next_pkg = 0;
            size_t w;
            for (w = 0; w < D99_MAX_DOWNLOAD_WORKERS; w++) {
                workers[w].pid = 0;
                workers[w].idx = 0;
            }

            while (next_pkg < nacts || n_running > 0) {
                while (n_running < D99_MAX_DOWNLOAD_WORKERS && next_pkg < nacts) {
                    size_t cur = next_pkg++;
                    action *a;
                    pid_t pid;
                    if (!needed[cur])
                        continue;

                    a = &acts[cur];
                    printf("Get:%zu %s %s %s [%s B]\n", cur + 1, a->base_uri, a->name,
                           a->version, a->size ? a->size : "0");
                    fflush(stdout);

                    pid = fork();
                    if (pid < 0) {
                        if (d99_fetch(urls[cur], dests[cur], 0) != 0) {
                            fprintf(stderr, "d99-solve: failed to fetch %s\n", urls[cur]);
                            unlink(dests[cur]);
                            rc = 1;
                            break;
                        }
                        if (*a->sha256 && !verify_sha(dests[cur], a->sha256)) {
                            fprintf(stderr, "d99-solve: sha256 mismatch for %s\n", urls[cur]);
                            unlink(dests[cur]);
                            rc = 1;
                            break;
                        }
                        chmod(dests[cur], 0644);
                    } else if (pid == 0) {
                        int fret = d99_fetch(urls[cur], dests[cur], 0);
                        _exit(fret == 0 ? 0 : 1);
                    } else {
                        for (w = 0; w < D99_MAX_DOWNLOAD_WORKERS; w++) {
                            if (workers[w].pid == 0) {
                                workers[w].pid = pid;
                                workers[w].idx = cur;
                                n_running++;
                                break;
                            }
                        }
                    }
                }

                if (rc != 0)
                    break;

                if (n_running > 0) {
                    int status = 0;
                    pid_t done_pid = waitpid(-1, &status, 0);
                    if (done_pid > 0) {
                        for (w = 0; w < D99_MAX_DOWNLOAD_WORKERS; w++) {
                            if (workers[w].pid == done_pid) {
                                size_t idx = workers[w].idx;
                                action *a = &acts[idx];
                                workers[w].pid = 0;
                                n_running--;
                                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                                    fprintf(stderr, "d99-solve: failed to fetch %s\n", urls[idx]);
                                    unlink(dests[idx]);
                                    rc = 1;
                                } else if (*a->sha256 && !verify_sha(dests[idx], a->sha256)) {
                                    fprintf(stderr, "d99-solve: sha256 mismatch for %s\n", urls[idx]);
                                    unlink(dests[idx]);
                                    rc = 1;
                                } else {
                                    chmod(dests[idx], 0644);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            if (rc != 0) {
                for (w = 0; w < D99_MAX_DOWNLOAD_WORKERS; w++) {
                    if (workers[w].pid > 0) {
                        kill(workers[w].pid, SIGTERM);
                        waitpid(workers[w].pid, NULL, 0);
                        unlink(dests[workers[w].idx]);
                        workers[w].pid = 0;
                    }
                }
            }
        }

        if (rc == 0) {
            for (i = 0; i < nacts; i++)
                d99_sv_push(&files, dests[i]);
        }

        for (i = 0; i < nacts; i++) {
            free(dests[i]);
            free(urls[i]);
        }
        free(dests);
        free(urls);
        free(needed);
    }
    if (rc != 0)
        goto out;
    if (print_uris || download_only)
        goto out;

    av = d99_xmalloc((files.n + 8) * sizeof(char *));
    av[na++] = inst;
    if (strcmp(p->root, "/") != 0) {
        ro = d99_xasprintf("--root=%s", p->root);
        av[na++] = ro;
    }
    av[na++] = (char *)"-i";
    for (i = 0; i < files.n; i++)
        av[na++] = files.v[i];
    av[na] = NULL;
    rc = run_inst(av);
    free(av);
    free(ro);
    ro = NULL;

out:
    d99_sv_free(&files);
    free_actions(acts, nacts);
    free(inst);
    return rc;
}

static int cmd_remove_purge(const char *argv0, paths *p, char **names,
                            int nnames, int purge)
{
    d99_db *db = d99_db_load_status(p->admindir);
    char **av;
    int na = 0, i, rc;
    char *inst = find_inst_tool(argv0);

    if (!db)
        db = d99_db_new();
    for (i = 0; i < nnames; i++) {
        d99_pkg *pp = d99_db_find(db, names[i]);
        if (!pp || (pp->state != D99_PS_INSTALLED &&
                    pp->state != D99_PS_CONFIGFILES &&
                    pp->state != D99_PS_UNPACKED &&
                    pp->state != D99_PS_HALFCONFIGURED)) {
            printf("Package '%s' is not installed, so not removed\n", names[i]);
            d99_db_free(db);
            free(inst);
            return 0;
        }
        if (pp->state == D99_PS_CONFIGFILES && !purge) {
            printf("Package '%s' is not installed, so not removed\n", names[i]);
            d99_db_free(db);
            free(inst);
            return 0;
        }
    }
    d99_db_free(db);
    av = d99_xmalloc((size_t)(nnames + 8) * sizeof(char *));
    av[na++] = inst;
    if (strcmp(p->root, "/") != 0) {
        char *ro = d99_xasprintf("--root=%s", p->root);
        av[na++] = ro;
    }
    av[na++] = purge ? (char *)"-P" : (char *)"-r";
    for (i = 0; i < nnames; i++)
        av[na++] = names[i];
    av[na] = NULL;
    rc = run_inst(av);
    free(av);
    free(inst);
    return rc;
}

static int cmd_search(const char *cmd_name, paths *p, const char *term)
{
    d99_repo *repo = repo_load(p->lists_dir);
    size_t i;
    char *needle = d99_xstrdup(term);
    char *q;

    for (q = needle; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    if (repo->n == 0) {
        fprintf(stderr, "d99-solve: no package lists; run '%s update' first\n", cmd_name);
        free(needle);
        repo_free(repo);
        return 1;
    }
    size_t needle_len = strlen(needle);
    if (repo->disk_recs && repo->strings) {
        const struct cand_disk_rec *recs = (const struct cand_disk_rec *)repo->disk_recs;
        const char *strings = repo->strings;
        for (i = 0; i < repo->n; i++) {
            const char *name = strings + recs[i].name_off;
            const char *summary = recs[i].summary_off ? strings + recs[i].summary_off : "";
            int hit = 0;

            if (strstr(name, needle))
                hit = 1;
            else if (summary[0] && d99_strcasestr_match(summary, needle, needle_len))
                hit = 1;

            if (hit)
                printf("%s/%s - %s\n", name, strings + recs[i].version_off, summary);
        }
    } else {
        for (i = 0; i < repo->n; i++) {
            d99_cand *c = repo_get(repo, i);
            int hit = 0;

            if (!c)
                continue;
            if (strstr(c->name, needle))
                hit = 1;
            else if (c->summary && d99_strcasestr_match(c->summary, needle, needle_len))
                hit = 1;

            if (hit)
                printf("%s/%s - %s\n", c->name, c->version,
                       c->summary ? c->summary : "");
        }
    }
    free(needle);
    repo_free(repo);
    return 0;
}

static int cmd_show(const char *cmd_name, paths *p, const char *name)
{
    d99_repo *repo = repo_load(p->lists_dir);
    d99_cand *c;

    if (repo->n == 0) {
        fprintf(stderr, "d99-solve: package lists are empty; run '%s update' first\n", cmd_name);
        repo_free(repo);
        return 1;
    }
    c = repo_find(repo, name, D99_DEP_NONE, NULL);

    if (!c) {
        fprintf(stderr, "d99-solve: unable to locate package '%s'\n", name);
        repo_free(repo);
        return 1;
    }
    printf("Package: %s\nVersion: %s\nArchitecture: %s\n", c->name, c->version,
           c->arch ? c->arch : "unknown");
    printf("Filename: %s\n", c->filename);
    printf("Size: %s\n", c->size ? c->size : "");
    printf("Description: %s\n", c->summary ? c->summary : "");
    repo_free(repo);
    return 0;
}

/* ==================== main ==================== */

static void usage(const char *cmd_name)
{
    printf(
"Usage: %s [<option>...] <command>\n"
"\n"
"Commands:\n"
"  update                       fetch package indexes from the mirrors\n"
"  install <pkg>[=<ver>]...     SAT-solve, download and install\n"
"  remove <pkg>...              remove packages (via d99-inst)\n"
"  purge <pkg>...               purge packages (via d99-inst)\n"
"  search <term>                search names and descriptions\n"
"  show <pkg>                   show index details\n"
"\n"
"Options:\n"
"  --root=<dir>                 operate on a chroot\n"
"  -y|--yes                     assume yes\n"
"  -d|--download-only           download but do not install\n"
"  --print-uris                 print download URIs instead of fetching\n"
"  --arch=<arch>                override the host architecture\n"
"  --version | --help\n",
        cmd_name);
}

int main(int argc, char **argv)
{
    const char *cmd_name = d99_cmd_name(argv[0], "d99-solve", "apt");
    paths p;
    const char *root = "/";
    const char *arch = NULL;
    int yes = 0, download_only = 0, print_uris = 0, simulate = 0, fix_broken = 0;
    const char *cmd = NULL;
    d99_strvec ops;
    int i, rc;

    d99_sv_init(&ops);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
            for (i++; i < argc; i++)
                d99_sv_push(&ops, argv[i]);
            break;
        }
        if (a[0] == '-' && a[1]) {
            if (a[1] == '-') {
                if (strncmp(a, "--root=", 7) == 0)
                    root = a + 7;
                else if (strcmp(a, "--root") == 0 && i + 1 < argc)
                    root = argv[++i];
                else if (strncmp(a, "--arch=", 7) == 0)
                    arch = a + 7;
                else if (strcmp(a, "--yes") == 0)
                    yes = 1;
                else if (strcmp(a, "--download-only") == 0)
                    download_only = 1;
                else if (strcmp(a, "--print-uris") == 0)
                    print_uris = 1;
                else if (strcmp(a, "--simulate") == 0 ||
                         strcmp(a, "--dry-run") == 0 ||
                         strcmp(a, "--just-print") == 0 ||
                         strcmp(a, "--recon") == 0 ||
                         strcmp(a, "--no-act") == 0)
                    simulate = 1;
                else if (strcmp(a, "--quiet") == 0)
                    ;
                else if (strcmp(a, "--fix-broken") == 0)
                    fix_broken = 1;
                else if (strcmp(a, "--reinstall") == 0)
                    ;   /* accepted; same-version reinstalls are a no-op */
                else if (strcmp(a, "--verbose") == 0)
                    d99_set_verbose(1);
                else if (strcmp(a, "--version") == 0) {
                    printf("d99-solve (d99) %s\n", D99_VERSION);
                    return 0;
                } else if (strcmp(a, "--help") == 0) {
                    usage(cmd_name);
                    return 0;
                } else {
                    d99_fallback_or_die(argv[0], argv);
                }
            } else {
                const char *q;
                for (q = a + 1; *q; q++) {
                    switch (*q) {
                    case 'y': yes = 1; break;
                    case 'd': download_only = 1; break;
                    case 's': simulate = 1; break;
                    case 'f': fix_broken = 1; break;
                    case 'q': break;
                    case 'v': d99_set_verbose(1); break;
                    default: d99_fallback_or_die(argv[0], argv);
                    }
                }
            }
        } else if (!cmd) {
            cmd = a;
        } else {
            d99_sv_push(&ops, a);
        }
    }

    if (!cmd) {
        usage(cmd_name);
        return 2;
    }
    paths_init(&p, root);
    if (!arch)
        arch = d99_host_arch();

    rc = 0;
    if (strcmp(cmd, "update") == 0 || strcmp(cmd, "u") == 0 || strcmp(cmd, "up") == 0) {
        rc = cmd_update(&p, arch);
    } else if (strcmp(cmd, "install") == 0 || strcmp(cmd, "i") == 0 || strcmp(cmd, "in") == 0) {
        struct target_spec *targets;
        int k;
        if (ops.n == 0 && !fix_broken) {
            fprintf(stderr, "d99-solve: install needs a package name\n");
            rc = 2;
        } else if (ops.n == 0 && fix_broken) {
            d99_db *db = d99_db_load_status(p.admindir);
            d99_strvec broken;
            size_t bi;
            d99_sv_init(&broken);
            if (db) {
                for (bi = 0; bi < d99_db_count(db); bi++) {
                    d99_pkg *pp = d99_db_at(db, bi);
                    if (pp->state == D99_PS_INSTALLED && !installed_deps_satisfied(db, pp)) {
                        d99_sv_push(&broken, pp->name);
                    }
                }
            }
            if (broken.n == 0) {
                printf("d99-solve: 0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n");
                rc = 0;
            } else {
                targets = d99_xmalloc(broken.n * sizeof(struct target_spec));
                for (k = 0; (size_t)k < broken.n; k++) {
                    targets[k].name = d99_xstrdup(broken.v[k]);
                    targets[k].op = D99_DEP_NONE;
                    targets[k].ver = NULL;
                }
                rc = cmd_install(argv[0], cmd_name, &p, targets, (int)broken.n, yes,
                                 download_only, print_uris, simulate);
                for (k = 0; (size_t)k < broken.n; k++) {
                    free(targets[k].name);
                }
                free(targets);
            }
            d99_sv_free(&broken);
            if (db)
                d99_db_free(db);
        } else {
            int bad = 0;
            targets = d99_xmalloc(ops.n * sizeof(struct target_spec));
            for (k = 0; (size_t)k < ops.n; k++)
                if (parse_target(ops.v[k], &targets[k]) != 0)
                    bad = 1;
            if (!bad)
                rc = cmd_install(argv[0], cmd_name, &p, targets, (int)ops.n, yes,
                                 download_only, print_uris, simulate);
            for (k = 0; (size_t)k < ops.n; k++) {
                free(targets[k].name);
                free(targets[k].ver);
            }
            free(targets);
        }
    } else if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "r") == 0 ||
               strcmp(cmd, "purge") == 0 || strcmp(cmd, "p") == 0) {
        if (ops.n == 0) {
            fprintf(stderr, "d99-solve: %s needs a package name\n", cmd);
            rc = 2;
        } else {
            rc = cmd_remove_purge(argv[0], &p, ops.v, (int)ops.n,
                                  strcmp(cmd, "purge") == 0 || strcmp(cmd, "p") == 0);
        }
    } else if (strcmp(cmd, "search") == 0 || strcmp(cmd, "s") == 0) {
        if (ops.n == 0) {
            fprintf(stderr, "d99-solve: search needs a term\n");
            rc = 2;
        } else {
            rc = cmd_search(cmd_name, &p, ops.v[0]);
        }
    } else if (strcmp(cmd, "show") == 0) {
        if (ops.n == 0) {
            fprintf(stderr, "d99-solve: show needs a package name\n");
            rc = 2;
        } else {
            rc = cmd_show(cmd_name, &p, ops.v[0]);
        }
    } else {
        fprintf(stderr, "d99-solve: unknown command '%s'\n", cmd);
        rc = 2;
    }

    d99_sv_free(&ops);
    free(p.root);
    free(p.lists_dir);
    free(p.cache_dir);
    free(p.sources_file);
    free(p.sources_dir);
    free(p.admindir);
    return rc;
}
