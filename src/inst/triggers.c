#include "inst.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char *trig_file(struct d99_ctx *c)
{
    char *trigdir = d99_path_join(c->admindir, "triggers");
    char *f = d99_path_join(trigdir, "Unincorp.d99");
    free(trigdir);
    return f;
}

void triggers_add(struct d99_ctx *c, const char *name, const char *args)
{
    char *tf = trig_file(c);
    char *trigdir = d99_dirname_dup(tf);
    size_t old_len = 0;
    char *old = d99_read_file(tf, &old_len);
    char *line;

    d99_mkdir_p(trigdir, 0755);
    free(trigdir);
    if (args && *args)
        line = d99_xasprintf("%s%s %s\n", old ? old : "", name, args);
    else
        line = d99_xasprintf("%s%s\n", old ? old : "", name);
    d99_write_file_atomic(tf, line, strlen(line));
    free(line);
    free(old);
    free(tf);
}

/* Is `pkg' interested in trigger `name'? */
static int pkg_interested(struct d99_ctx *c, const char *pkg, const char *name)
{
    char *path = d99_xasprintf("%s/info/%s.triggers", c->admindir, pkg);
    size_t len = 0;
    char *text = d99_read_file(path, &len);
    const char *p;
    int found = 0;

    free(path);
    if (!text)
        return 0;
    p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_xstrndup(p, ln);
        char *t = d99_trim(line);
        if (d99_starts_with(t, "interest ")) {
            char *tn = d99_trim(t + 9);
            if (strcmp(tn, name) == 0)
                found = 1;
        } else if (d99_starts_with(t, "interest-noawait ")) {
            char *tn = d99_trim(t + 18);
            if (strcmp(tn, name) == 0)
                found = 1;
        }
        free(line);
        if (found)
            break;
        if (!eol)
            break;
        p = eol + 1;
    }
    free(text);
    return found;
}

int triggers_process(struct d99_ctx *c)
{
    char *tf = trig_file(c);
    size_t len = 0;
    char *text = d99_read_file(tf, &len);
    d99_strvec done, pending;
    const char *p;
    size_t i;

    if (!text || len == 0) {
        free(text);
        free(tf);
        return 0;
    }
    d99_sv_init(&done);
    d99_sv_init(&pending);

    p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        char *line = d99_trim(d99_xstrndup(p, ln));
        char name[256];
        char args[1024];
        int ok = 1;

        if (*line) {
            /* split name/args */
            {
                char *sp = strchr(line, ' ');
                size_t nl = sp ? (size_t)(sp - line) : strlen(line);
                if (nl >= sizeof name)
                    nl = sizeof name - 1;
                memcpy(name, line, nl);
                name[nl] = '\0';
                snprintf(args, sizeof args, "%s", sp ? sp + 1 : "");
            }
            if (!d99_sv_contains(&done, name)) {
                /* run every interested package's postinst */
                char *infodir = d99_path_join(c->admindir, "info");
                DIR *d = opendir(infodir);
                struct dirent *de;
                free(infodir);
                if (d) {
                    while ((de = readdir(d)) != NULL) {
                        size_t dnl = strlen(de->d_name);
                        char *owner;
                        d99_pkg *pp;
                        if (dnl < 10 ||
                            strcmp(de->d_name + dnl - 9, ".triggers") != 0)
                            continue;
                        owner = d99_xstrndup(de->d_name, dnl - 9);
                        pp = d99_db_find(c->db, owner);
                        if (pp && pp->state != D99_PS_NOTINSTALLED &&
                            pp->state != D99_PS_CONFIGFILES &&
                            pkg_interested(c, owner, name)) {
                            char *targs[3];
                            int na = 0;
                            targs[na++] = (char *)"triggered";
                            targs[na++] = name;
                            if (args[0])
                                targs[na++] = args;
                            targs[na] = NULL;
                            if (isatty(STDOUT_FILENO))
                                printf("\r\033[K");
                            printf("Processing triggers for %s (%s) ...\n", owner, pp->version ? pp->version : "");
                            {
                                char desc[128];
                                snprintf(desc, sizeof desc, "Processing triggers for %s", owner);
                                d99_inst_update_progress(c, desc);
                            }
                            if (hook_run(c, owner, pp->version, pp->arch,
                                         "postinst", targs) != 0) {
                                d99_warn("trigger processing script for "
                                         "package %s failed", owner);
                                ok = 0;
                            }
                        }
                        free(owner);
                    }
                    closedir(d);
                }
                d99_sv_push(&done, name);
            }
            if (!ok)
                d99_sv_push(&pending, line);
        }
        free(line);
        if (!eol)
            break;
        p = eol + 1;
    }

    if (pending.n == 0) {
        unlink(tf);
    } else {
        FILE *out = fopen(tf, "wb");
        if (out) {
            for (i = 0; i < pending.n; i++)
                fprintf(out, "%s\n", pending.v[i]);
            fclose(out);
        }
    }
    {
        int rc = pending.n ? 1 : 0;
        d99_sv_free(&done);
        d99_sv_free(&pending);
        free(text);
        free(tf);
        return rc;
    }
}
