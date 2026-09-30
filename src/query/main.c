#include "d99_api.h"
#include "d99_archive.h"
#include "d99_db.h"
#include "d99_fallback.h"
#include "d99_util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_json_mode = 0;

static void usage(void)
{
    fputs(
"Usage: d99-query [<option>...] <command>\n"
"\n"
"Commands:\n"
"  -l|--list [<pattern>...]    list packages with status flags\n"
"  -s|--status <pkg>...        dump the RFC-822 status stanza\n"
"  -L|--listfiles <pkg>...     list files owned by packages\n"
"  -S|--search <path>...       find packages owning files\n"
"  -W|--show [<pkg>...]        machine-readable records\n"
"\n"
"Options:\n"
"  -f|--showformat <format>    output format for -W (${Field}, \\n, \\t)\n"
"  --admindir <dir>            package database directory\n"
"  --rebuild                   (re)build the binary index and exit\n"
"  --json                      output results in JSON format\n"
"  --version | --help\n",
        stdout);
}

/* ---------- unified record view over index or text DB ---------- */
typedef struct {
    d99_index *ix;
    d99_db *db;
    const char *admindir;
} src;

typedef struct {
    const char *name;
    char *version;   /* malloc'd when filled from db, borrowed from index */
    const char *arch;
    const char *summary;
    const char *stanza;
    int sel, state, flags;
    size_t ixidx;
} qpkg;

static void fill_ix(qpkg *q, d99_index *ix, size_t i)
{
    memset(q, 0, sizeof *q);
    q->name = d99_index_name(ix, i);
    q->version = (char *)d99_index_version(ix, i);
    q->arch = d99_index_arch(ix, i);
    q->summary = d99_index_summary(ix, i);
    q->stanza = d99_index_stanza(ix, i);
    d99_index_status(ix, i, &q->sel, &q->state, &q->flags);
    q->ixidx = i;
}

static void fill_db(qpkg *q, d99_pkg *p)
{
    memset(q, 0, sizeof *q);
    q->name = p->name;
    q->version = p->version;
    q->arch = p->arch ? p->arch : "";
    q->summary = p->summary ? p->summary : "";
    q->stanza = p->raw;
    q->sel = p->sel;
    q->state = p->state;
    q->flags = p->flags;
}

/* Field lookup directly in a stanza blob (index fast path). */
static char *stanza_field_dup(const char *stanza, const char *field)
{
    size_t flen = strlen(field);
    const char *p = stanza;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > flen + 1 && strncmp(p, field, flen) == 0 && p[flen] == ':') {
            const char *v = p + flen + 1;
            const char *e;
            size_t n;
            char *out;
            if (*v == ' ')
                v++;
            e = v;
            for (;;) {
                const char *nl = strchr(e, '\n');
                if (!nl)
                    break;
                e = nl + 1;
                if (*e == '\0' || (*e != ' ' && *e != '\t'))
                    break;
            }
            n = (size_t)(e - v);
            while (n && (v[n - 1] == '\n' || v[n - 1] == ' '))
                n--;
            out = d99_xmalloc(n + 1);
            memcpy(out, v, n);
            out[n] = '\0';
            return out;
        }
        p = eol ? eol + 1 : NULL;
        while (p && (*p == ' ' || *p == '\t')) {
            eol = strchr(p, '\n');
            p = eol ? eol + 1 : NULL;
        }
    }
    return NULL;
}

static char *field_dup(const qpkg *q, const char *field)
{
    return stanza_field_dup(q->stanza, field);
}

/* ---------- status letter codes (dpkg-query compatible) ---------- */
static char sel_char(int sel)
{
    switch (sel) {
    case D99_SEL_INSTALL:   return 'i';
    case D99_SEL_HOLD:      return 'h';
    case D99_SEL_DEINSTALL: return 'r';
    case D99_SEL_PURGE:     return 'p';
    default:                return 'u';
    }
}

static char state_char(int st)
{
    switch (st) {
    case D99_PS_INSTALLED:       return 'i';
    case D99_PS_CONFIGFILES:     return 'c';
    case D99_PS_HALFINSTALLED:   return 'H';
    case D99_PS_UNPACKED:        return 'U';
    case D99_PS_HALFCONFIGURED:  return 'F';
    case D99_PS_TRIGGERSPENDING: return 't';
    default:                     return 'n';
    }
}

/* ---------- -l ---------- */
static void print_list_row(const qpkg *q)
{
    char errc = (q->flags & D99_PF_REINSTREQ) ? 'R' : ' ';
    printf("%c%c%c %-15.15s %-12.12s %-11.11s %s\n",
           sel_char(q->sel), state_char(q->state), errc,
           q->name, q->version ? q->version : "",
           q->arch ? q->arch : "",
           q->summary ? q->summary : "");
}

static int cmd_list(src *s, char **pats, int npat)
{
    size_t i;

    if (g_json_mode) {
        d99_json_buf b;
        d99_jb_init(&b);
        d99_jb_raw(&b, "[");
        size_t count = 0;
        if (s->ix) {
            for (i = 0; i < d99_index_count(s->ix); i++) {
                const char *pkg_name = d99_index_name(s->ix, i);
                int match = (npat == 0);
                for (int k = 0; k < npat && !match; k++)
                    if (d99_glob_match(pats[k], pkg_name ? pkg_name : ""))
                        match = 1;
                if (!match) continue;
                qpkg q;
                fill_ix(&q, s->ix, i);
                if (count > 0) d99_jb_raw(&b, ",");
                d99_jb_raw(&b, "{\"package\":");
                d99_jb_str(&b, q.name);
                d99_jb_raw(&b, ",\"version\":");
                d99_jb_str(&b, q.version);
                d99_jb_raw(&b, ",\"architecture\":");
                d99_jb_str(&b, q.arch);
                d99_jb_raw(&b, ",\"status\":");
                d99_jb_str(&b, d99_state_name(q.state));
                d99_jb_raw(&b, ",\"summary\":");
                d99_jb_str(&b, q.summary);
                d99_jb_raw(&b, "}");
                count++;
            }
        } else if (s->db) {
            for (i = 0; i < d99_db_count(s->db); i++) {
                d99_pkg *p = d99_db_at(s->db, i);
                int match = (npat == 0);
                for (int k = 0; k < npat && !match; k++)
                    if (d99_glob_match(pats[k], p->name ? p->name : ""))
                        match = 1;
                if (!match) continue;
                qpkg q;
                fill_db(&q, p);
                if (count > 0) d99_jb_raw(&b, ",");
                d99_jb_raw(&b, "{\"package\":");
                d99_jb_str(&b, q.name);
                d99_jb_raw(&b, ",\"version\":");
                d99_jb_str(&b, q.version);
                d99_jb_raw(&b, ",\"architecture\":");
                d99_jb_str(&b, q.arch);
                d99_jb_raw(&b, ",\"status\":");
                d99_jb_str(&b, d99_state_name(q.state));
                d99_jb_raw(&b, ",\"summary\":");
                d99_jb_str(&b, q.summary);
                d99_jb_raw(&b, "}");
                count++;
            }
        }
        d99_jb_raw(&b, "]");
        char *out = d99_jb_finish(&b);
        printf("%s\n", out);
        free(out);
        return 0;
    }

    fputs(
"Desired=Unknown/Install/Remove/Hold/Purge\n"
"| Status=Not/Inst/Conf-files/Unpacked/halfC/Half-inst/trig-aWait/Trig-pend\n"
"|/ Err?=(none)/Reinstate/req\n"
"||/ Name           Version      Architecture Description\n"
"+++-==============-============-============-========================================\n",
          stdout);

    if (s->ix) {
        for (i = 0; i < d99_index_count(s->ix); i++) {
            const char *pkg_name = d99_index_name(s->ix, i);
            int match = (npat == 0);
            int k;
            for (k = 0; k < npat && !match; k++)
                if (d99_glob_match(pats[k], pkg_name ? pkg_name : ""))
                    match = 1;
            if (!match)
                continue;
            qpkg q;
            fill_ix(&q, s->ix, i);
            print_list_row(&q);
        }
    } else if (s->db) {
        for (i = 0; i < d99_db_count(s->db); i++) {
            d99_pkg *p = d99_db_at(s->db, i);
            qpkg q;
            int match = (npat == 0);
            int k;
            for (k = 0; k < npat && !match; k++)
                if (d99_glob_match(pats[k], p->name ? p->name : ""))
                    match = 1;
            if (!match)
                continue;
            fill_db(&q, p);
            print_list_row(&q);
        }
    }
    return 0;
}

/* ---------- lookup helpers ---------- */
static int find_pkg(src *s, const char *name, qpkg *out)
{
    size_t i;
    if (s->ix && d99_index_find_pkg(s->ix, name, &i) == 1) {
        fill_ix(out, s->ix, i);
        return 1;
    }
    if (s->db) {
        d99_pkg *p = d99_db_find(s->db, name);
        if (p) {
            fill_db(out, p);
            return 1;
        }
    }
    return 0;
}

/* ---------- -s ---------- */
static int cmd_status(src *s, char **names, int nnames)
{
    int k, rc = 0;
    for (k = 0; k < nnames; k++) {
        qpkg q;
        if (!find_pkg(s, names[k], &q)) {
            fprintf(stderr, "d99-query: no packages found matching '%s'\n",
                    names[k]);
            rc = 1;
            continue;
        }
        fputs(q.stanza, stdout);
        printf("\n");
    }
    return rc;
}

/* ---------- -L ---------- */
static int cmd_listfiles(src *s, char **names, int nnames)
{
    int k, rc = 0;
    for (k = 0; k < nnames; k++) {
        char *path = d99_xasprintf("%s/info/%s.list", s->admindir, names[k]);
        size_t len = 0;
        char *text = d99_read_file(path, &len);
        if (!text) {
            fprintf(stderr, "d99-query: package '%s' has no file list\n",
                    names[k]);
            rc = 1;
        } else {
            if (len > 0) {
                fwrite(text, 1, len, stdout);
                if (text[len - 1] != '\n')
                    putchar('\n');
            }
            free(text);
        }
        free(path);
    }
    return rc;
}

/* ---------- -S ---------- */

static int cmd_search(src *s, char **paths, int npaths)
{
    int k, rc = 0;

    for (k = 0; k < npaths; k++) {
        char norm[4096];
        const char *p = paths[k];
        int found = 0;
        size_t i, j;

        while (*p == '/')
            p++;
        snprintf(norm, sizeof norm, "%s", p);

        if (strchr(norm, '*') || strchr(norm, '?')) {
            /* glob scan */
            if (s->ix) {
                for (i = 0; i < d99_index_count(s->ix); i++) {
                    for (j = 0; j < d99_index_pkg_nfiles(s->ix, i); j++) {
                        const char *f = d99_index_pkg_file(s->ix, i, j);
                        char full[4096];
                        if (!f)
                            continue;
                        snprintf(full, sizeof full, "/%s", f);
                        if (d99_glob_match(norm, f) || d99_glob_match(norm, full)) {
                            printf("/%s: %s\n", f, d99_index_name(s->ix, i));
                            found = 1;
                        }
                    }
                }
            } else {
                fprintf(stderr, "d99-query: glob search needs the binary "
                        "index (run: d99-query --rebuild)\n");
                rc = 1;
                continue;
            }
        } else if (s->ix) {
            size_t pi, fi;
            if (d99_index_find_file(s->ix, norm, &pi, &fi) == 1) {
                printf("/%s: %s\n", norm, d99_index_name(s->ix, pi));
                found = 1;
            }
        } else {
            /* linear scan of info .list files */
            char *infodir = d99_xasprintf("%s/info", s->admindir);
            DIR *d = opendir(infodir);
            struct dirent *de;
            size_t norm_len = strlen(norm);
            if (d) {
                while ((de = readdir(d)) != NULL) {
                    size_t nl = strlen(de->d_name);
                    char *full, *text;
                    const char *scan;
                    if (nl < 6 || strcmp(de->d_name + nl - 5, ".list") != 0)
                        continue;
                    full = d99_path_join(infodir, de->d_name);
                    text = d99_read_file(full, NULL);
                    scan = text;
                    while (scan && *scan) {
                        const char *eol = strchr(scan, '\n');
                        size_t ln = eol ? (size_t)(eol - scan) : strlen(scan);
                        const char *line = scan;
                        if (ln > 0 && line[0] == '/') {
                            line++;
                            ln--;
                        }
                        if (ln == norm_len && memcmp(line, norm, norm_len) == 0) {
                            char *owner = d99_xstrndup(de->d_name, nl - 5);
                            printf("/%s: %s\n", norm, owner);
                            free(owner);
                            found = 1;
                        }
                        if (!eol)
                            break;
                        scan = eol + 1;
                    }
                    free(text);
                    free(full);
                    if (found)
                        break;
                }
                closedir(d);
            }
            free(infodir);
        }
        if (!found) {
            fprintf(stderr, "d99-query: no path found matching '%s'\n",
                    paths[k]);
            rc = 1;
        }
    }
    return rc;
}

/* ---------- -W with showformat ---------- */
static void print_format(const char *fmt, const qpkg *q)
{
    const char *p = fmt;

    while (*p) {
        if (*p == '\\' && p[1]) {
            if (p[1] == 'n') {
                putchar('\n');
                p += 2;
                continue;
            }
            if (p[1] == 't') {
                putchar('\t');
                p += 2;
                continue;
            }
            if (p[1] == '\\') {
                putchar('\\');
                p += 2;
                continue;
            }
        }
        if (p[0] == '$' && p[1] == '{') {
            const char *e = strchr(p + 2, '}');
            if (e) {
                char *field = d99_xstrndup(p + 2, (size_t)(e - p - 2));
                if (strcmp(field, "Package") == 0)
                    fputs(q->name, stdout);
                else {
                    char *v = field_dup(q, field);
                    if (v) {
                        fputs(v, stdout);
                        free(v);
                    }
                }
                free(field);
                p = e + 1;
                continue;
            }
        }
        putchar(*p);
        p++;
    }
}

static int cmd_show(src *s, char **names, int nnames, const char *fmt)
{
    int k, rc = 0;

    if (nnames == 0) {
        size_t i;
        if (s->ix) {
            for (i = 0; i < d99_index_count(s->ix); i++) {
                qpkg q;
                fill_ix(&q, s->ix, i);
                print_format(fmt, &q);
            }
        } else if (s->db) {
            for (i = 0; i < d99_db_count(s->db); i++) {
                qpkg q;
                fill_db(&q, d99_db_at(s->db, i));
                print_format(fmt, &q);
            }
        }
        return 0;
    }
    for (k = 0; k < nnames; k++) {
        qpkg q;
        if (!find_pkg(s, names[k], &q)) {
            fprintf(stderr, "d99-query: no packages found matching '%s'\n",
                    names[k]);
            rc = 1;
            continue;
        }
        print_format(fmt, &q);
    }
    return rc;
}

/* ---------- main ---------- */
enum action { A_NONE, A_LIST, A_STATUS, A_LISTFILES, A_SEARCH, A_SHOW };

int main(int argc, char **argv)
{
    enum action act = A_NONE;
    const char *admindir = d99_default_admindir();
    const char *showformat = NULL;
    int want_rebuild = 0;
    d99_strvec ops;
    src s;
    int i, rc;
    char *idxpath, *statuspath;

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
                if (strcmp(a, "--list") == 0)
                    act = A_LIST;
                else if (strcmp(a, "--status") == 0)
                    act = A_STATUS;
                else if (strcmp(a, "--listfiles") == 0)
                    act = A_LISTFILES;
                else if (strcmp(a, "--search") == 0)
                    act = A_SEARCH;
                else if (strcmp(a, "--show") == 0)
                    act = A_SHOW;
                else if (strcmp(a, "--admindir") == 0) {
                    if (i + 1 >= argc) {
                        fprintf(stderr, "d99-query: --admindir needs a value\n");
                        return 2;
                    }
                    admindir = argv[++i];
                } else if (strncmp(a, "--admindir=", 11) == 0) {
                    admindir = a + 11;
                } else if (strncmp(a, "--showformat=", 13) == 0) {
                    showformat = a + 13;
                } else if (strcmp(a, "--showformat") == 0) {
                    if (i + 1 >= argc) {
                        fprintf(stderr, "d99-query: --showformat needs a value\n");
                        return 2;
                    }
                    showformat = argv[++i];
                } else if (strcmp(a, "--rebuild") == 0) {
                    want_rebuild = 1;
                } else if (strcmp(a, "--json") == 0) {
                    g_json_mode = 1;
                } else if (strcmp(a, "--help") == 0) {
                    usage();
                    return 0;
                } else if (strcmp(a, "--version") == 0) {
                    printf("d99-query (d99) %s\n", D99_VERSION);
                    return 0;
                } else {
                    d99_fallback_or_die(argv[0], argv);
                }
            } else {
                const char *p;
                for (p = a + 1; *p; p++) {
                    switch (*p) {
                    case 'l': act = A_LIST; break;
                    case 's':
                    case 'p': act = A_STATUS; break;
                    case 'L': act = A_LISTFILES; break;
                    case 'S': act = A_SEARCH; break;
                    case 'W': act = A_SHOW; break;
                    case 'f':
                        if (p[1]) {
                            showformat = p + 1;
                        } else if (i + 1 < argc) {
                            showformat = argv[++i];
                        } else {
                            fprintf(stderr, "d99-query: -f needs a value\n");
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

    statuspath = d99_path_join(admindir, "status");
    idxpath = d99_index_path_for(admindir);

    if (want_rebuild) {
        if (d99_index_build(admindir, idxpath) != 0) {
            fprintf(stderr, "d99-query: cannot build index at %s\n", idxpath);
            return 1;
        }
        if (act == A_NONE) {
            printf("d99-query: index rebuilt at %s\n", idxpath);
            free(idxpath);
            free(statuspath);
            return 0;
        }
    }

    memset(&s, 0, sizeof s);
    s.admindir = admindir;
    s.ix = d99_index_open(idxpath, statuspath);
    if (!s.ix) {
        d99_verbose("index missing or stale; using slow text path");
        s.db = d99_db_load_status(admindir);
        if (!s.db) {
            fprintf(stderr, "d99-query: cannot open package database in %s\n",
                    admindir);
            return 1;
        }
    }

    if (!showformat)
        showformat = "${Package}\t${Version}\n";

    rc = 0;
    switch (act) {
    case A_LIST:
        rc = cmd_list(&s, ops.v, (int)ops.n);
        break;
    case A_STATUS:
        if (ops.n == 0) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_status(&s, ops.v, (int)ops.n);
        break;
    case A_LISTFILES:
        if (ops.n == 0) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_listfiles(&s, ops.v, (int)ops.n);
        break;
    case A_SEARCH:
        if (ops.n == 0) {
            usage();
            rc = 2;
            break;
        }
        rc = cmd_search(&s, ops.v, (int)ops.n);
        break;
    case A_SHOW:
        rc = cmd_show(&s, ops.v, (int)ops.n, showformat);
        break;
    default:
        usage();
        rc = 2;
        break;
    }

    if (s.ix)
        d99_index_close(s.ix);
    if (s.db)
        d99_db_free(s.db);
    d99_sv_free(&ops);
    free(idxpath);
    free(statuspath);
    return rc;
}
