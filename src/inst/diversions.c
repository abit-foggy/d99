#include "inst.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct d99_div {
    char *local;   /* on-disk diverted path */
    char *orig;    /* original path (as packaged) */
    char *pkg;     /* diverter package */
};

struct d99_diversions {
    d99_arena *ar;
    struct d99_div *v;
    size_t n;
};

/* dpkg may append ":<arch>" to paths; strip such qualifiers. */
static void strip_arch(char *s)
{
    char *colon = strrchr(s, ':');
    if (colon && strchr(colon, '/') == NULL)
        *colon = '\0';
}

struct d99_diversions *diversions_load(const char *admindir)
{
    char *path = d99_path_join(admindir, "diversions");
    size_t len = 0;
    char *text = d99_read_file(path, &len);
    struct d99_diversions *dv;
    d99_strvec lines;
    const char *p;
    size_t i;

    free(path);
    dv = d99_xcalloc(1, sizeof(*dv));
    dv->ar = d99_arena_new();
    dv->v = NULL;
    dv->n = 0;
    if (!text)
        return dv;

    d99_sv_init(&lines);
    p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_xstrndup(p, ln);
        char *t = d99_trim(line);
        if (*t)
            d99_sv_push(&lines, t);
        free(line);
        if (!eol)
            break;
        p = eol + 1;
    }
    free(text);

    for (i = 0; i + 2 < lines.n; i += 3) {
        dv->v = d99_xrealloc(dv->v, (dv->n + 1) * sizeof(struct d99_div));
        dv->v[dv->n].local = d99_arena_strdup(dv->ar, lines.v[i]);
        dv->v[dv->n].orig = d99_arena_strdup(dv->ar, lines.v[i + 1]);
        dv->v[dv->n].pkg = d99_arena_strdup(dv->ar, lines.v[i + 2]);
        strip_arch(dv->v[dv->n].local);
        strip_arch(dv->v[dv->n].orig);
        dv->n++;
    }
    d99_sv_free(&lines);
    return dv;
}

const char *diversions_map(struct d99_diversions *dv, const char *orig,
                           const char *my_pkg)
{
    size_t i;
    if (!dv)
        return NULL;
    for (i = 0; i < dv->n; i++)
        if (strcmp(dv->v[i].orig, orig) == 0 &&
            strcmp(dv->v[i].pkg, my_pkg ? my_pkg : "") != 0)
            return dv->v[i].local;
    return NULL;
}

void diversions_free(struct d99_diversions *dv)
{
    if (!dv)
        return;
    d99_arena_free(dv->ar);
    free(dv->v);
    free(dv);
}
