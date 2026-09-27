#include "manifest.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strip a trailing " ; comment" / " # comment" from a value line */
static void strip_trailing_comment(char *s)
{
    char *p = s;
    while ((p = strpbrk(p, ";#")) != NULL) {
        if (p > s && isspace((unsigned char)p[-1])) {
            *p = '\0';
            return;
        }
        p++;
    }
}

static void set_pkg_field(d99_manifest *m, const char *key, char *value)
{
    if (strcmp(key, "name") == 0 || strcmp(key, "package") == 0)
        m->name = value;
    else if (strcmp(key, "version") == 0)
        m->version = value;
    else if (strcmp(key, "architecture") == 0 || strcmp(key, "arch") == 0)
        m->arch = value;
    else if (strcmp(key, "section") == 0)
        m->section = value;
    else if (strcmp(key, "priority") == 0)
        m->priority = value;
    else if (strcmp(key, "maintainer") == 0)
        m->maintainer = value;
    else if (strcmp(key, "homepage") == 0)
        m->homepage = value;
    else if (strcmp(key, "depends") == 0)
        m->depends = value;
    else if (strcmp(key, "pre-depends") == 0 || strcmp(key, "predepends") == 0)
        m->predepends = value;
    else if (strcmp(key, "recommends") == 0)
        m->recommends = value;
    else if (strcmp(key, "suggests") == 0)
        m->suggests = value;
    else if (strcmp(key, "conflicts") == 0)
        m->conflicts = value;
    else if (strcmp(key, "breaks") == 0)
        m->breaks = value;
    else if (strcmp(key, "replaces") == 0)
        m->replaces = value;
    else if (strcmp(key, "provides") == 0)
        m->provides = value;
    else if (strcmp(key, "description") == 0) {
        m->description = value;   /* may be appended to by continuations */
        m->desc_last = value;
    } else {
        /* unknown keys are kept verbatim as extra control fields */
        size_t n = m->extra_n;
        m->extra = d99_xrealloc(m->extra, (n + 2) * sizeof(char *));
        m->extra[n] = d99_xstrdup(key);
        m->extra[n + 1] = d99_arena_strdup(m->ar, value);
        m->extra_n = n + 2;
    }
}

int d99_manifest_load(const char *path, d99_manifest *m)
{
    size_t len = 0;
    char *text = d99_read_file(path, &len);
    const char *p;
    char section[64] = "";
    int rc = 0;

    memset(m, 0, sizeof(*m));
    if (!text) {
        fprintf(stderr, "d99-build: cannot read manifest %s\n", path);
        return -1;
    }
    m->ar = d99_arena_new();
    d99_sv_init(&m->files_dst);
    d99_sv_init(&m->files_src);
    d99_sv_init(&m->conffiles);
    d99_sv_init(&m->script_names);
    d99_sv_init(&m->script_paths);

    p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_xstrndup(p, ln);
        char *t = d99_trim(line);

        if (*t == '\0' || *t == '#' || *t == ';') {
            free(line);
            if (!eol)
                break;
            p = eol + 1;
            continue;
        }
        if (*t == '[') {
            char *close = strchr(t, ']');
            if (close) {
                *close = '\0';
                snprintf(section, sizeof section, "%s", d99_trim(t + 1));
            }
        } else if (*t == ' ' || *t == '\t') {
            /* continuation: append to the last description value */
            if (strcmp(section, "package") == 0 && m->desc_last) {
                char *cont = d99_xstrdup(d99_trim(t));
                strip_trailing_comment(cont);
                if (*cont) {
                    char *joined = d99_xasprintf("%s\n %s", m->desc_last, cont);
                    m->description = joined;
                    m->desc_last = joined;
                }
                free(cont);
            } else if (strcmp(section, "conffiles") == 0 && *d99_trim(t)) {
                d99_sv_push(&m->conffiles, d99_arena_strdup(m->ar, d99_trim(t)));
            }
        } else {
            strip_trailing_comment(t);
            if (strcmp(section, "package") == 0) {
                char *eq = strchr(t, '=');
                if (eq) {
                    *eq = '\0';
                    set_pkg_field(m, d99_trim(t),
                                  d99_arena_strdup(m->ar, d99_trim(eq + 1)));
                }
            } else if (strcmp(section, "files") == 0) {
                char *eq = strchr(t, '=');
                if (eq) {
                    *eq = '\0';
                    d99_sv_push(&m->files_dst,
                                d99_arena_strdup(m->ar, d99_trim(t)));
                    d99_sv_push(&m->files_src,
                                d99_arena_strdup(m->ar, d99_trim(eq + 1)));
                }
            } else if (strcmp(section, "conffiles") == 0) {
                char *eq = strchr(t, '=');
                char *val = eq ? eq + 1 : t;
                if (*d99_trim(val))
                    d99_sv_push(&m->conffiles,
                                d99_arena_strdup(m->ar, d99_trim(val)));
            } else if (strcmp(section, "scripts") == 0) {
                char *eq = strchr(t, '=');
                if (eq) {
                    *eq = '\0';
                    d99_sv_push(&m->script_names,
                                d99_arena_strdup(m->ar, d99_trim(t)));
                    d99_sv_push(&m->script_paths,
                                d99_arena_strdup(m->ar, d99_trim(eq + 1)));
                }
            }
        }
        free(line);
        if (!eol)
            break;
        p = eol + 1;
    }
    free(text);

    if (!m->name || !m->version) {
        fprintf(stderr, "d99-build: manifest must define at least "
                "[package] name and version\n");
        rc = -1;
    }
    return rc;
}

void d99_manifest_free(d99_manifest *m)
{
    if (!m)
        return;
    d99_sv_free(&m->files_dst);
    d99_sv_free(&m->files_src);
    d99_sv_free(&m->conffiles);
    d99_sv_free(&m->script_names);
    d99_sv_free(&m->script_paths);
    free(m->extra);
    d99_arena_free(m->ar);
}
