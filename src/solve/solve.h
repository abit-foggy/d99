#ifndef SOLVE_H
#define SOLVE_H

#include "d99_api.h"
#include "d99_archive.h"
#include "d99_db.h"
#include "d99_util.h"

/* Paths used across d99-solve and its API */
typedef struct {
    char *root, *lists_dir, *cache_dir, *sources_file, *sources_dir, *admindir;
    char *log_file, *backups_dir, *ext_states;
} paths;

void paths_init(paths *p, const char *root);
void paths_free(paths *p);

struct target_spec {
    char *name;
    int op;
    char *ver;
};

int parse_target(const char *s, struct target_spec *t);

typedef struct action {
    char *name, *version, *arch;
    char *filename, *sha256, *size, *base_uri, *summary;
    char *old_version;   /* NULL for a fresh install */
} action;

void free_actions(action *a, size_t n);

int plan_install(const char *cmd_name, paths *p, struct target_spec *targets, int ntargets,
                 action **out_actions, size_t *out_n);

/* A repository candidate: one package version from a Packages index. */
typedef struct d99_cand {
    char *name, *version, *arch;
    char *filename;      /* path relative to the mirror root */
    char *sha256, *size;
    char *summary;
    char *base_uri;      /* mirror root for building download URLs */
    d99_deplist depends, predepends, conflicts, breaks, provides;
    char *raw_depends, *raw_predepends, *raw_conflicts, *raw_breaks, *raw_provides;
    int deps_parsed;
    size_t idx;
} d99_cand;

struct cand_disk_rec {
    uint32_t name_off;
    uint32_t version_off;
    uint32_t arch_off;
    uint32_t filename_off;
    uint32_t sha256_off;
    uint32_t size_off;
    uint32_t summary_off;
    uint32_t base_uri_off;
    uint32_t depends_off;
    uint32_t predepends_off;
    uint32_t conflicts_off;
    uint32_t breaks_off;
    uint32_t provides_off;
};

typedef struct d99_repo {
    d99_arena *ar;
    d99_cand **v;
    size_t n, cap;
    uint32_t *ht;
    uint32_t *next;
    size_t ht_size;
    void *map;
    size_t map_len;
    const void *disk_recs;
    const char *strings;
} d99_repo;

d99_repo *repo_load(const char *lists_dir);
int repo_build_index(const char *lists_dir);
void cand_ensure_deps(d99_cand *c, d99_arena *ar);
void repo_free(d99_repo *r);
d99_cand *repo_get(d99_repo *r, size_t idx);

d99_cand *repo_find(d99_repo *r, const char *name, int op, const char *ver);
int cand_satisfies(d99_cand *c, const char *name, int op, const char *ver);

/* fetch.c — file:// and http:// retrieval (atomic temp+rename).
 * `quiet' suppresses per-attempt errors (used while probing index
 * formats during `update'). */
int d99_fetch(const char *url, const char *dest, int quiet);

/* api.c — API handler for other binaries (CLI, JSON-RPC, UNIX domain socket) */
int cmd_api(const char *argv0, const char *cmd_name, paths *p, const char *arch,
            const char **args, size_t nargs, int json_mode, int status_fd);

#endif /* SOLVE_H */
