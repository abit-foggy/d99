#include "d99_db.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ================= selection/state names ================= */
const char *d99_sel_name(int sel)
{
    switch (sel) {
    case D99_SEL_UNKNOWN:   return "unknown";
    case D99_SEL_INSTALL:   return "install";
    case D99_SEL_HOLD:      return "hold";
    case D99_SEL_DEINSTALL: return "deinstall";
    case D99_SEL_PURGE:     return "purge";
    default:                return "unknown";
    }
}

const char *d99_state_name(int st)
{
    switch (st) {
    case D99_PS_NOTINSTALLED:    return "not-installed";
    case D99_PS_CONFIGFILES:     return "config-files";
    case D99_PS_HALFINSTALLED:   return "half-installed";
    case D99_PS_UNPACKED:        return "unpacked";
    case D99_PS_HALFCONFIGURED:  return "half-configured";
    case D99_PS_TRIGGERSPENDING: return "triggers-pending";
    case D99_PS_INSTALLED:       return "installed";
    default:                     return "not-installed";
    }
}

int d99_sel_parse(const char *s)
{
    if (!s)
        return D99_SEL_UNKNOWN;
    if (strcmp(s, "install") == 0)
        return D99_SEL_INSTALL;
    if (strcmp(s, "hold") == 0)
        return D99_SEL_HOLD;
    if (strcmp(s, "deinstall") == 0)
        return D99_SEL_DEINSTALL;
    if (strcmp(s, "purge") == 0)
        return D99_SEL_PURGE;
    return D99_SEL_UNKNOWN;
}

int d99_state_parse(const char *s)
{
    if (!s)
        return D99_PS_NOTINSTALLED;
    if (strcmp(s, "installed") == 0)
        return D99_PS_INSTALLED;
    if (strcmp(s, "config-files") == 0)
        return D99_PS_CONFIGFILES;
    if (strcmp(s, "half-installed") == 0)
        return D99_PS_HALFINSTALLED;
    if (strcmp(s, "unpacked") == 0)
        return D99_PS_UNPACKED;
    if (strcmp(s, "half-configured") == 0)
        return D99_PS_HALFCONFIGURED;
    if (strcmp(s, "triggers-pending") == 0)
        return D99_PS_TRIGGERSPENDING;
    return D99_PS_NOTINSTALLED;
}

/* ================= version comparison ================= */
static int ver_order(int c)
{
    if (c >= '0' && c <= '9')
        return 0;
    if (isalpha(c))
        return c;
    if (c == '~')
        return -1;
    if (c == 0)
        return 0;
    return c + 256;
}

static int verrevcmp(const char *a, const char *b)
{
    while (*a || *b) {
        int first_diff = 0;

        while ((*a && !isdigit((unsigned char)*a)) ||
               (*b && !isdigit((unsigned char)*b))) {
            int ac = ver_order((unsigned char)*a);
            int bc = ver_order((unsigned char)*b);
            if (ac != bc)
                return ac - bc;
            a++;
            b++;
        }
        while (*a == '0')
            a++;
        while (*b == '0')
            b++;
        while (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            if (!first_diff)
                first_diff = (int)(unsigned char)*a - (int)(unsigned char)*b;
            a++;
            b++;
        }
        if (isdigit((unsigned char)*a))
            return 1;
        if (isdigit((unsigned char)*b))
            return -1;
        if (first_diff)
            return first_diff;
    }
    return 0;
}

/* Split epoch:upstream-debian.  Returns epoch, sets pointers into
 * modified copies owned by the caller-provided bufs. */
static long ver_split(const char *v, const char **up, const char **deb,
                      char *buf1, size_t b1n, char *buf2, size_t b2n)
{
    const char *colon, *dash;
    long epoch = 0;

    snprintf(buf1, b1n, "%s", v ? v : "");
    colon = strchr(buf1, ':');
    if (colon && colon != buf1) {
        char save = *colon;
        *(char *)colon = '\0';
        epoch = strtol(buf1, NULL, 10);
        *(char *)colon = save;
        memmove(buf1, colon + 1, strlen(colon + 1) + 1);
    }
    dash = strrchr(buf1, '-');
    if (dash) {
        *(char *)dash = '\0';
        snprintf(buf2, b2n, "%s", dash + 1);
    } else {
        buf2[0] = '\0';
    }
    *up = buf1;
    *deb = buf2;
    return epoch;
}

int d99_vercmp(const char *va, const char *vb)
{
    char b1a[512], b2a[512], b1b[512], b2b[512];
    const char *upa, *deba, *upb, *debb;
    long ea, eb, r;

    ea = ver_split(va, &upa, &deba, b1a, sizeof b1a, b2a, sizeof b2a);
    eb = ver_split(vb, &upb, &debb, b1b, sizeof b1b, b2b, sizeof b2b);
    if (ea != eb)
        return ea < eb ? -1 : 1;
    r = verrevcmp(upa, upb);
    if (r)
        return (int)r;
    return (int)verrevcmp(deba, debb);
}

/* ================= dependency parsing ================= */
int d99_depop_parse(const char *op)
{
    if (!op || !*op)
        return D99_DEP_NONE;
    if (strcmp(op, "<<") == 0)
        return D99_DEP_LT;
    if (strcmp(op, "<=") == 0)
        return D99_DEP_LE;
    if (strcmp(op, "=") == 0)
        return D99_DEP_EQ;
    if (strcmp(op, ">=") == 0)
        return D99_DEP_GE;
    if (strcmp(op, ">>") == 0)
        return D99_DEP_GT;
    return D99_DEP_NONE;
}

const char *d99_depop_name(int op)
{
    switch (op) {
    case D99_DEP_LT: return "<<";
    case D99_DEP_LE: return "<=";
    case D99_DEP_EQ: return "=";
    case D99_DEP_GE: return ">=";
    case D99_DEP_GT: return ">>";
    default:         return "";
    }
}

int d99_verrel(int cmp, int op)
{
    switch (op) {
    case D99_DEP_LT: return cmp < 0;
    case D99_DEP_LE: return cmp <= 0;
    case D99_DEP_EQ: return cmp == 0;
    case D99_DEP_GE: return cmp >= 0;
    case D99_DEP_GT: return cmp > 0;
    default:         return 1;   /* unconstrained */
    }
}

int d99_arch_ok(const char *archq, const char *myarch)
{
    char buf[128];
    char *p, *tok;

    if (!archq || !*archq)
        return 1;
    snprintf(buf, sizeof buf, "%s", archq);
    p = buf;
    if (*p == '!') {
        /* forbidden list */
        int hit = 0;
        p++;
        while ((tok = strsep(&p, " ,")) != NULL)
            if (*tok && strcmp(tok, myarch) == 0)
                hit = 1;
        return !hit;
    }
    while ((tok = strsep(&p, " ,")) != NULL)
        if (*tok && (strcmp(tok, myarch) == 0 || strcmp(tok, "any") == 0))
            return 1;
    return 0;
}

/* Parse one dependency group: "name (>= ver) [arch] | name2" */
static int parse_alternative(const char *s, d99_depalternative *alt,
                             d99_arena *ar)
{
    const char *p = s;
    const char *start;
    char op[8];
    size_t n;

    while (isspace((unsigned char)*p))
        p++;
    start = p;
    while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != '[')
        p++;
    n = (size_t)(p - start);
    if (n == 0)
        return -1;
    {
        char *nm = d99_arena_alloc(ar, n + 1);
        memcpy(nm, start, n);
        nm[n] = '\0';
        /* strip :arch qualifier */
        {
            char *co = strrchr(nm, ':');
            if (co && co != nm) {
                *co = '\0';
                alt->arch = d99_arena_strdup(ar, co + 1);
            } else {
                alt->arch = NULL;
            }
        }
        alt->name = nm;
    }
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '(') {
        const char *e = strchr(p, ')');
        const char *o = p + 1;
        const char *v;
        if (!e)
            return -1;
        while (o < e && isspace((unsigned char)*o))
            o++;
        v = o;
        while (v < e && *v != ' ' && *v != '\t')
            v++;
        n = (size_t)(v - o);
        if (n >= sizeof op)
            n = sizeof op - 1;
        memcpy(op, o, n);
        op[n] = '\0';
        alt->op = d99_depop_parse(op);
        while (v < e && isspace((unsigned char)*v))
            v++;
        n = (size_t)(e - v);
        {
            char *vv = d99_arena_alloc(ar, n + 1);
            memcpy(vv, v, n);
            vv[n] = '\0';
            alt->ver = vv;
        }
        p = e + 1;
    } else {
        alt->op = D99_DEP_NONE;
        alt->ver = NULL;
    }
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '[') {
        const char *e = strchr(p, ']');
        if (!e)
            return -1;
        n = (size_t)(e - p - 1);
        {
            char *aq = d99_arena_alloc(ar, n + 1);
            memcpy(aq, p + 1, n);
            aq[n] = '\0';
            if (alt->arch)
                free(alt->arch);   /* qualifier from [] wins; [] copy arena */
            /* NB: alt->arch may have been set by the ':arch' split above,
             * which lives in the arena; simply overwrite the pointer. */
            alt->arch = d99_arena_strdup(ar, aq);
            /* aq itself was allocated but unused afterwards */
            (void)aq;
        }
        p = e + 1;
    }
    return 0;
}

int d99_deplist_parse(const char *field, d99_deplist *dl, d99_arena *ar)
{
    char *copy, *gp;

    dl->g = NULL;
    dl->n = 0;
    if (!field || !*field)
        return 0;
    copy = d99_xstrdup(field);
    gp = copy;
    while (gp) {
        char *comma = strchr(gp, ',');
        char *end;
        d99_depgroup g;
        d99_depalternative *alts = NULL;
        size_t na = 0, capa = 0;

        if (comma)
            *comma = '\0';
        g.alts = NULL;
        g.n = 0;
        end = gp;
        while (end) {
            char *pipe = strchr(end, '|');
            d99_depalternative alt;

            if (pipe)
                *pipe = '\0';
            memset(&alt, 0, sizeof(alt));
            if (parse_alternative(end, &alt, ar) == 0) {
                if (na == capa) {
                    capa = capa ? capa * 2 : 2;
                    alts = d99_xrealloc(alts, capa * sizeof(*alts));
                }
                alts[na++] = alt;
            }
            end = pipe ? pipe + 1 : NULL;
        }
        g.alts = alts;
        g.n = na;
        if (na > 0) {
            dl->g = d99_xrealloc(dl->g, (dl->n + 1) * sizeof(*dl->g));
            dl->g[dl->n++] = g;
        }
        gp = comma ? comma + 1 : NULL;
    }
    free(copy);
    return 0;
}

void d99_deplist_reset(d99_deplist *dl)
{
    size_t i;
    if (!dl)
        return;
    for (i = 0; i < dl->n; i++)
        free(dl->g[i].alts);
    free(dl->g);
    dl->g = NULL;
    dl->n = 0;
}

int d99_dep_alt_match(const d99_depalternative *alt, const char *name,
                      const char *ver)
{
    if (!alt || !name || strcmp(alt->name, name) != 0)
        return 0;
    if (alt->op != D99_DEP_NONE && ver) {
        if (!ver)
            return 0;
        return d99_verrel(d99_vercmp(ver, alt->ver), alt->op);
    }
    return 1;
}

void d99_conffiles_parse(const char *text, d99_strvec *out, d99_arena *ar)
{
    const char *p = text;

    if (!p)
        return;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_xstrndup(p, n);
        char *t = d99_trim(line);

        if (*t && *t != '#') {
            /* new format: "<md5> <size> <path>"; old format: "<path>" */
            char *sp1 = strchr(t, ' ');
            char *path = t;
            if (sp1) {
                char *sp2 = strchr(sp1 + 1, ' ');
                if (sp2)
                    path = d99_trim(sp2 + 1);
            }
            /* store canonically relative (no leading '/') so that
             * d99_path_join(root, conffile) and normalized tar paths match */
            if (path[0] == '/')
                path++;
            d99_sv_push(out, d99_arena_strdup(ar, path));
        }
        free(line);
        if (!eol)
            break;
        p = eol + 1;
    }
}

/* ================= stanza parsing ================= */
/* Iterate "Field: value" lines with ' '/'\t' continuations in-place. */
static void stanza_fields(const char *text, size_t len,
                          void (*cb)(void *ud, const char *field,
                                     const char *value),
                          void *ud)
{
    size_t pos = 0;
    char active[128];
    int has_active = 0;
    char *acc = NULL;
    size_t accn = 0, acccap = 0;

    active[0] = '\0';

    while (pos < len) {
        const char *line = text + pos;
        const char *eol = memchr(line, '\n', len - pos);
        size_t linelen = eol ? (size_t)(eol - line) : len - pos;
        const char *co;
        pos += linelen + (eol ? 1 : 0);

        if (linelen > 0 && line[linelen - 1] == '\r')
            linelen--;

        if (linelen == 0)
            break;   /* end of stanza */

        if (line[0] == ' ' || line[0] == '\t') {
            if (has_active) {
                /* continuation line */
                size_t start = 0;
                while (start < linelen && (line[start] == ' ' || line[start] == '\t'))
                    start++;
                size_t end = linelen;
                while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t'))
                    end--;
                size_t tlen = end - start;
                if (tlen > 0) {
                    if (accn + tlen + 2 > acccap) {
                        acccap = (accn + tlen + 2) * 2 + 128;
                        acc = d99_xrealloc(acc, acccap);
                    }
                    acc[accn++] = '\n';
                    memcpy(acc + accn, line + start, tlen);
                    accn += tlen;
                    acc[accn] = '\0';
                }
            }
            continue;
        }

        /* new field */
        if (has_active) {
            cb(ud, active, acc ? acc : "");
            has_active = 0;
        }

        co = memchr(line, ':', linelen);
        if (co) {
            size_t klen = (size_t)(co - line);
            if (klen < sizeof(active)) {
                const char *v = co + 1;
                size_t rem = linelen - (klen + 1);

                memcpy(active, line, klen);
                active[klen] = '\0';
                has_active = 1;

                while (rem > 0 && (*v == ' ' || *v == '\t')) {
                    v++;
                    rem--;
                }
                while (rem > 0 && (v[rem - 1] == ' ' || v[rem - 1] == '\t'))
                    rem--;

                if (rem + 1 > acccap) {
                    acccap = rem + 128;
                    acc = d99_xrealloc(acc, acccap);
                }
                memcpy(acc, v, rem);
                acc[rem] = '\0';
                accn = rem;
            }
        }
    }
    if (has_active) {
        cb(ud, active, acc ? acc : "");
    }
    free(acc);
}

struct pkgbuilder {
    d99_arena *ar;
    d99_pkg *p;
};

static void set_list(d99_pkg *p, const char *field, const char *value,
                     d99_arena *ar)
{
    d99_deplist *target = NULL;
    if (strcmp(field, "Depends") == 0)        target = &p->depends;
    else if (strcmp(field, "Pre-Depends") == 0)   target = &p->predepends;
    else if (strcmp(field, "Recommends") == 0)     target = &p->recommends;
    else if (strcmp(field, "Suggests") == 0)      target = &p->suggests;
    else if (strcmp(field, "Conflicts") == 0)     target = &p->conflicts;
    else if (strcmp(field, "Breaks") == 0)        target = &p->breaks;
    else if (strcmp(field, "Replaces") == 0)      target = &p->replaces;
    else if (strcmp(field, "Provides") == 0)      target = &p->provides;
    if (target)
        d99_deplist_parse(value, target, ar);
}

static void pkg_field_cb(void *ud, const char *field, const char *value)
{
    struct pkgbuilder *b = ud;
    d99_pkg *p = b->p;

    if (strcmp(field, "Package") == 0)
        p->name = d99_arena_strdup(b->ar, value);
    else if (strcmp(field, "Version") == 0)
        p->version = d99_arena_strdup(b->ar, value);
    else if (strcmp(field, "Architecture") == 0)
        p->arch = d99_arena_strdup(b->ar, value);
    else if (strcmp(field, "Section") == 0)
        p->section = d99_arena_strdup(b->ar, value);
    else if (strcmp(field, "Priority") == 0)
        p->priority = d99_arena_strdup(b->ar, value);
    else if (strcmp(field, "Maintainer") == 0)
        p->maintainer = d99_arena_strdup(b->ar, value);
    else if (strcmp(field, "Description") == 0) {
        const char *nl = strchr(value, '\n');
        if (nl) {
            p->summary = d99_arena_strdup(b->ar, value);
            /* summary owns the whole first line */
            {
                char *s = d99_xstrdup(value);
                char *t = strchr(s, '\n');
                if (t)
                    *t = '\0';
                p->summary = d99_arena_strdup(b->ar, s);
                free(s);
            }
            p->description = d99_arena_strdup(b->ar, nl + 1);
        } else {
            p->summary = d99_arena_strdup(b->ar, value);
            p->description = NULL;
        }
    } else if (strcmp(field, "Status") == 0) {
        d99_parse_status_words(value, &p->sel, &p->flags, &p->state);
    } else {
        set_list(p, field, value, b->ar);
    }
}

void d99_parse_status_words(const char *v, int *sel, int *errflags, int *state)
{
    char *copy = d99_xstrdup(v ? v : "");
    char *sp = NULL;
    char *t1 = strtok_r(copy, " \t", &sp);
    char *t2 = t1 ? strtok_r(NULL, " \t", &sp) : NULL;
    char *t3 = t2 ? strtok_r(NULL, " \t", &sp) : NULL;

    *sel = t1 ? d99_sel_parse(t1) : D99_SEL_UNKNOWN;
    *errflags = D99_PF_OK;
    if (t2 && strcmp(t2, "reinstreq") == 0)
        *errflags |= D99_PF_REINSTREQ;
    *state = t3 ? d99_state_parse(t3) : D99_PS_NOTINSTALLED;
    free(copy);
}

d99_pkg *d99_pkg_parse_stanza(const char *text, size_t len, d99_arena *ar)
{
    struct pkgbuilder b;
    d99_pkg *p = d99_arena_alloc(ar, sizeof(*p));
    char *raw;

    memset(p, 0, sizeof(*p));
    p->sel = D99_SEL_UNKNOWN;
    p->state = D99_PS_NOTINSTALLED;
    p->flags = 0;

    /* verbatim copy, guaranteed '\n'-terminated */
    raw = d99_arena_alloc(ar, len + 2);
    memcpy(raw, text, len);
    if (len == 0 || raw[len - 1] != '\n')
        raw[len++] = '\n';
    raw[len] = '\0';
    p->raw = raw;

    b.ar = ar;
    b.p = p;
    stanza_fields(text, len, pkg_field_cb, &b);
    if (!p->name)
        return NULL;
    return p;
}

/* ================= status database ================= */
struct d99_db {
    d99_arena *ar;
    d99_pkg **v;
    size_t n, cap;
    d99_pkg **ht;      /* name hash table */
    size_t htn;
};

const char *d99_default_admindir(void)
{
    const char *env = getenv("D99_ADMINDIR");
    return env && *env ? env : "/var/lib/dpkg";
}

char *d99_index_path_for(const char *admindir)
{
    char *parent = d99_dirname_dup(admindir);
    char *idxdir = d99_path_join(parent, "d99");
    char *idx = d99_path_join(idxdir, "index.bin");
    free(parent);
    free(idxdir);
    return idx;
}

d99_db *d99_db_new(void)
{
    d99_db *db = d99_xcalloc(1, sizeof(*db));
    db->ar = d99_arena_new();
    db->htn = 1024;
    db->ht = d99_xcalloc(db->htn, sizeof(d99_pkg *));
    return db;
}

void d99_db_free(d99_db *db)
{
    if (!db)
        return;
    d99_arena_free(db->ar);
    free(db->v);
    free(db->ht);
    free(db);
}

d99_arena *d99_db_arena(d99_db *db)
{
    return db ? db->ar : NULL;
}

size_t d99_db_count(d99_db *db)
{
    return db->n;
}

d99_pkg *d99_db_at(d99_db *db, size_t i)
{
    return i < db->n ? db->v[i] : NULL;
}

static void db_rehash(d99_db *db)
{
    size_t i, newn = db->htn * 2;
    d99_pkg **ht = d99_xcalloc(newn, sizeof(d99_pkg *));
    for (i = 0; i < db->n; i++) {
        uint64_t h = d99_fnv1a64_str(db->v[i]->name);
        size_t j = (size_t)h & (newn - 1);
        while (ht[j])
            j = (j + 1) & (newn - 1);
        ht[j] = db->v[i];
    }
    free(db->ht);
    db->ht = ht;
    db->htn = newn;
}

void d99_db_add(d99_db *db, d99_pkg *p)
{
    uint64_t h = d99_fnv1a64_str(p->name);
    size_t j = (size_t)h & (db->htn - 1);

    /* replace an existing entry of the same name and arch in place */
    while (db->ht[j]) {
        if (strcmp(db->ht[j]->name, p->name) == 0 &&
            (!p->arch || !db->ht[j]->arch || strcmp(p->arch, db->ht[j]->arch) == 0)) {
            size_t i;
            for (i = 0; i < db->n; i++)
                if (db->v[i] == db->ht[j])
                    db->v[i] = p;
            db->ht[j] = p;
            return;
        }
        j = (j + 1) & (db->htn - 1);
    }

    /* append a new entry */
    if (db->n == db->cap) {
        db->cap = db->cap ? db->cap * 2 : 64;
        db->v = d99_xrealloc(db->v, db->cap * sizeof(d99_pkg *));
    }
    db->v[db->n++] = p;
    if (db->n * 2 > db->htn)
        db_rehash(db);
    j = (size_t)h & (db->htn - 1);
    while (db->ht[j])
        j = (j + 1) & (db->htn - 1);
    db->ht[j] = p;
}

d99_pkg *d99_db_find(d99_db *db, const char *name)
{
    uint64_t h;
    size_t j;
    const char *colon;
    const char *target_arch = NULL;
    char base_name[128];
    d99_pkg *first_match = NULL;
    const char *host_arch = d99_host_arch();

    if (!name)
        return NULL;

    colon = strchr(name, ':');
    if (colon) {
        size_t nlen = (size_t)(colon - name);
        if (nlen >= sizeof(base_name))
            nlen = sizeof(base_name) - 1;
        memcpy(base_name, name, nlen);
        base_name[nlen] = '\0';
        name = base_name;
        target_arch = colon + 1;
    }

    h = d99_fnv1a64_str(name);
    j = (size_t)h & (db->htn - 1);
    while (db->ht[j]) {
        if (strcmp(db->ht[j]->name, name) == 0) {
            if (target_arch) {
                if (db->ht[j]->arch && strcmp(db->ht[j]->arch, target_arch) == 0)
                    return db->ht[j];
            } else {
                if (db->ht[j]->arch && host_arch && strcmp(db->ht[j]->arch, host_arch) == 0)
                    return db->ht[j];
                if (!first_match)
                    first_match = db->ht[j];
            }
        }
        j = (j + 1) & (db->htn - 1);
    }
    return first_match;
}

int d99_db_remove(d99_db *db, d99_pkg *p)
{
    /* removal is realized by dropping the stanza from the vector; the
     * hash table entry is cleared. */
    size_t i, j;
    uint64_t h;
    size_t slot;

    if (!p)
        return -1;
    for (i = 0; i < db->n; i++) {
        if (db->v[i] == p) {
            memmove(db->v + i, db->v + i + 1, (db->n - i - 1) * sizeof(d99_pkg *));
            db->n--;
            break;
        }
    }
    h = d99_fnv1a64_str(p->name);
    slot = (size_t)h & (db->htn - 1);
    while (db->ht[slot]) {
        if (db->ht[slot] == p) {
            /* open addressing delete: rehash the following cluster */
            db->ht[slot] = NULL;
            j = (slot + 1) & (db->htn - 1);
            while (db->ht[j]) {
                d99_pkg *q = db->ht[j];
                uint64_t h2 = d99_fnv1a64_str(q->name);
                size_t k = (size_t)h2 & (db->htn - 1);
                db->ht[j] = NULL;
                while (db->ht[k])
                    k = (k + 1) & (db->htn - 1);
                db->ht[k] = q;
                j = (j + 1) & (db->htn - 1);
            }
            return 0;
        }
        slot = (slot + 1) & (db->htn - 1);
    }
    return 0;
}

d99_db *d99_db_load_status(const char *admindir)
{
    char *path = d99_path_join(admindir, "status");
    size_t len;
    char *text = d99_read_file(path, &len);
    d99_db *db;
    size_t pos = 0;

    free(path);
    if (!text)
        return NULL;
    db = d99_db_new();
    while (pos < len) {
        size_t stanza_len = 0;
        /* a stanza is terminated by a blank line */
        size_t p = pos;
        while (p < len) {
            size_t e = p;
            while (e < len && text[e] != '\n')
                e++;
            if (e == p) {   /* empty line */
                stanza_len = e - pos;   /* excludes blank line */
                p = e + 1;
                break;
            }
            if (e >= len) {
                stanza_len = e - pos;
                p = e;
                break;
            }
            p = e + 1;
        }
        if (stanza_len > 0) {
            d99_pkg *pkg = d99_pkg_parse_stanza(text + pos, stanza_len, db->ar);
            if (pkg)
                d99_db_add(db, pkg);
        }
        pos = p;
        if (stanza_len == 0 && p >= len)
            break;
    }
    free(text);
    return db;
}

/* ================= field surgery on raw ================= */
/* Find the start of a "Field:" line in raw. */
static char *raw_find_field(char *raw, const char *field)
{
    size_t flen = strlen(field);
    char *p = raw;

    while (p) {
        char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if ((size_t)(linelen) > flen + 1 &&
            strncmp(p, field, flen) == 0 && p[flen] == ':') {
            char *pp = p;
            while (pp > raw && pp[-1] != '\n')
                pp--;
            if (pp == raw || pp[-1] == '\n')
                return p;
        }
        p = eol ? eol + 1 : NULL;
    }
    return NULL;
}

static char *raw_replace_line(d99_db *db, d99_pkg *p, char *at,
                              const char *newline)
{
    /* replace the whole logical field (value + continuations) starting
     * at `at' with `newline' */
    size_t head = (size_t)(at - p->raw);
    char *e = at;
    char *eol;

    /* advance to end of logical field: next line not starting with space */
    for (;;) {
        eol = strchr(e, '\n');
        if (!eol)
            break;
        e = eol + 1;
        if (*e == '\0')
            break;
        if (*e != ' ' && *e != '\t')
            break;
    }
    {
        size_t tail_len = strlen(e);
        size_t new_len = strlen(newline);
        char *nr = d99_arena_alloc(db->ar, head + new_len + tail_len + 1);
        memcpy(nr, p->raw, head);
        memcpy(nr + head, newline, new_len);
        memcpy(nr + head + new_len, e, tail_len);
        nr[head + new_len + tail_len] = '\0';
        return nr;
    }
}

int d99_pkg_set_status(d99_db *db, d99_pkg *p, int sel, int state, int flags)
{
    char line[128];
    char *at;

    snprintf(line, sizeof line, "Status: %s %s %s\n", d99_sel_name(sel),
             (flags & D99_PF_REINSTREQ) ? "reinstreq" : "ok",
             d99_state_name(state));
    at = raw_find_field(p->raw, "Status");
    if (at) {
        p->raw = raw_replace_line(db, p, at, line);
    } else {
        /* insert after the Package line, or prepend */
        at = raw_find_field(p->raw, "Package");
        if (at) {
            char *eol = strchr(at, '\n');
            size_t head = (size_t)(eol + 1 - p->raw);
            size_t old_len = strlen(p->raw);
            char *nr = d99_arena_alloc(db->ar, old_len + strlen(line) + 2);
            memcpy(nr, p->raw, head);
            memcpy(nr + head, line, strlen(line));
            memcpy(nr + head + strlen(line), p->raw + head, old_len - head + 1);
            p->raw = nr;
        } else {
            char *nr = d99_arena_alloc(db->ar, strlen(p->raw) + strlen(line) + 1);
            memcpy(nr, line, strlen(line));
            memcpy(nr + strlen(line), p->raw, strlen(p->raw) + 1);
            p->raw = nr;
        }
    }
    p->sel = sel;
    p->state = state;
    p->flags = (flags & D99_PF_REINSTREQ) ? D99_PF_REINSTREQ : D99_PF_OK;
    return 0;
}

int d99_pkg_set_field(d99_db *db, d99_pkg *p, const char *field,
                      const char *value)
{
    char *at, *line;

    line = d99_xasprintf("%s: %s\n", field, value ? value : "");
    at = raw_find_field(p->raw, field);
    if (at) {
        p->raw = raw_replace_line(db, p, at, line);
    } else {
        /* append at the end of the stanza */
        size_t old_len = strlen(p->raw);
        size_t line_len = strlen(line);
        char *nr = d99_arena_alloc(db->ar, old_len + line_len + 2);
        memcpy(nr, p->raw, old_len);
        if (old_len == 0 || nr[old_len - 1] != '\n') {
            nr[old_len] = '\n';
            old_len++;
        }
        memcpy(nr + old_len, line, line_len + 1);
        p->raw = nr;
    }
    free(line);
    return 0;
}

int d99_pkg_remove_field(d99_db *db, d99_pkg *p, const char *field)
{
    char *at = raw_find_field(p->raw, field);
    if (!at)
        return 0;
    p->raw = raw_replace_line(db, p, at, "");
    return 0;
}

char *d99_pkg_field_dup(const d99_pkg *p, const char *field)
{
    char *at, *e, *out;
    size_t vlen;

    if (!p || !p->raw)
        return NULL;
    at = raw_find_field(p->raw, field);
    if (!at)
        return NULL;
    at = strchr(at, ':');
    if (!at)
        return NULL;
    at++;
    if (*at == ' ')
        at++;
    e = at;
    for (;;) {
        char *eol = strchr(e, '\n');
        if (!eol)
            break;
        e = eol + 1;
        if (*e == '\0' || (*e != ' ' && *e != '\t'))
            break;
    }
    vlen = (size_t)(e - at);
    /* strip trailing newline(s) */
    while (vlen > 0 && (at[vlen - 1] == '\n' || at[vlen - 1] == ' '))
        vlen--;
    out = d99_xmalloc(vlen + 1);
    memcpy(out, at, vlen);
    out[vlen] = '\0';
    return out;
}

/* ================= save ================= */
int d99_db_save_status(d99_db *db, const char *admindir)
{
    char *path = d99_path_join(admindir, "status");
    char *old = d99_path_join(admindir, "status-old");
    char *text;
    size_t len;
    FILE *out;
    char *buf;
    size_t buflen;
    size_t i;
    int fail = 0;

    /* status-old backup (like dpkg) */
    text = d99_read_file(path, &len);
    if (text) {
        FILE *bo = fopen(old, "wb");
        if (bo) {
            if (len && fwrite(text, 1, len, bo) != len)
                fail = 1;
            if (fclose(bo) != 0)
                fail = 1;
        }
        free(text);
    }

    out = open_memstream(&buf, &buflen);
    if (!out) {
        free(path);
        free(old);
        return -1;
    }
    for (i = 0; i < db->n; i++) {
        d99_pkg *p = db->v[i];
        fputs(p->raw, out);
        if (p->raw[strlen(p->raw) - 1] != '\n')
            fputc('\n', out);
        fputc('\n', out);
    }
    fclose(out);
    if (d99_write_file_atomic(path, buf, buflen) != 0)
        fail = 1;
    free(buf);
    free(path);
    free(old);
    return fail ? -1 : 0;
}
