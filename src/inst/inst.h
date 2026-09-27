#ifndef INST_H
#define INST_H

#include "d99_state.h"
#include "d99_fallback.h"
#include "d99_archive.h"

struct d99_ctx {
    char *root;        /* --instdir: filesystem target */
    char *admindir;    /* package database directory */
    char *indexpath;   /* binary index location */
    d99_db *db;
    int force_depends;
    int force_configure;
    struct d99_diversions *div;
};

int inst_unpack_deb(struct d99_ctx *c, const char *deb,
                    struct d99_pkg **batch, size_t nbatch);

/* hooks.c — maintainer script fork/execve dispatcher */
int hook_run(struct d99_ctx *c, const char *pkg, const char *version,
             const char *arch, const char *script, char *const args[]);

/* diversions.c — /var/lib/dpkg/diversions enforcer */
struct d99_diversions;
struct d99_diversions *diversions_load(const char *admindir);
const char *diversions_map(struct d99_diversions *dv, const char *orig,
                           const char *my_pkg);
void diversions_free(struct d99_diversions *dv);

/* triggers.c — trigger consolidation engine */
void triggers_add(struct d99_ctx *c, const char *name, const char *args);
int triggers_process(struct d99_ctx *c);

#endif /* INST_H */
