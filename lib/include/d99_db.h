#ifndef D99_DB_H
#define D99_DB_H

#include "d99_util.h"

/* ---- selection / state / flags (dpkg-compatible) ---- */
enum d99_sel {
    D99_SEL_UNKNOWN, D99_SEL_INSTALL, D99_SEL_HOLD, D99_SEL_DEINSTALL,
    D99_SEL_PURGE
};
enum d99_pstate {
    D99_PS_NOTINSTALLED, D99_PS_CONFIGFILES, D99_PS_HALFINSTALLED,
    D99_PS_UNPACKED, D99_PS_HALFCONFIGURED, D99_PS_TRIGGERSPENDING,
    D99_PS_INSTALLED
};
#define D99_PF_OK        1
#define D99_PF_REINSTREQ 2

const char *d99_sel_name(int sel);
const char *d99_state_name(int st);
int d99_sel_parse(const char *s);
int d99_state_parse(const char *s);

/* ---- Debian version comparison (policy 5.6.12) ---- */
int d99_vercmp(const char *a, const char *b);   /* <0, 0, >0 */

/* ---- dependency model ---- */
enum d99_depop { D99_DEP_NONE, D99_DEP_LT, D99_DEP_LE, D99_DEP_EQ,
                 D99_DEP_GE, D99_DEP_GT };
int d99_depop_parse(const char *op);
const char *d99_depop_name(int op);
/* does `cmp' (a vercmp result) satisfy the relation? */
int d99_verrel(int cmp, int op);

typedef struct {
    char *name;     /* package name (without :arch) */
    char *arch;     /* architecture qualifier, may be NULL */
    int   op;       /* d99_depop */
    char *ver;      /* version constraint, may be NULL */
} d99_depalternative;

typedef struct { d99_depalternative *alts; size_t n; } d99_depgroup;
typedef struct { d99_depgroup *g; size_t n; } d99_deplist;

int  d99_deplist_parse(const char *field, d99_deplist *dl, d99_arena *ar);
void d99_deplist_reset(d99_deplist *dl);
/* Does package `name'/`ver' (or a provider) satisfy an alternative? */
int  d99_dep_alt_match(const d99_depalternative *alt, const char *name,
                       const char *ver);
/* Debian architecture list check, e.g. "[amd64]" / "[!i386,armhf]". */
int d99_arch_ok(const char *archq, const char *myarch);

/* ---- package record ---- */
typedef struct d99_pkg {
    char *name, *version, *arch, *section, *priority, *maintainer;
    char *summary, *description;
    int   sel, state, flags;
    d99_deplist depends, predepends, recommends, suggests;
    d99_deplist conflicts, breaks, replaces, provides;
    char *raw;                /* verbatim RFC-822 stanza, '\n'-terminated */
} d99_pkg;

/* Parse one RFC-822 stanza (raw pointer need not be NUL-terminated). */
d99_pkg *d99_pkg_parse_stanza(const char *text, size_t len, d99_arena *ar);
/* Parse the "Status:" line words. */
void d99_parse_status_words(const char *v, int *sel, int *errflags, int *state);

/* ---- status database (legacy text format) ---- */
typedef struct d99_db d99_db;

d99_db  *d99_db_new(void);
void     d99_db_free(d99_db *db);
d99_arena *d99_db_arena(d99_db *db);   /* the db's allocation arena */
size_t   d99_db_count(d99_db *db);
d99_pkg *d99_db_at(d99_db *db, size_t i);
d99_pkg *d99_db_find(d99_db *db, const char *name);
void     d99_db_add(d99_db *db, d99_pkg *p);        /* takes ownership */
int      d99_db_remove(d99_db *db, d99_pkg *p);      /* drop at next save */

d99_db *d99_db_load_status(const char *admindir);    /* <admindir>/status */
int     d99_db_save_status(d99_db *db, const char *admindir);

/* Field surgery on the raw stanza (allocated from the db arena). */
int  d99_pkg_set_status(d99_db *db, d99_pkg *p, int sel, int state, int flags);
int  d99_pkg_set_field(d99_db *db, d99_pkg *p, const char *field,
                       const char *value);
int  d99_pkg_remove_field(d99_db *db, d99_pkg *p, const char *field);
/* malloc'd value with continuation lines joined by '\n'; NULL if absent */
char *d99_pkg_field_dup(const d99_pkg *p, const char *field);

/* conffiles list parser ("path" or "md5 size path" lines) */
void d99_conffiles_parse(const char *text, d99_strvec *out, d99_arena *ar);

/* admindir helpers */
const char *d99_default_admindir(void);
char *d99_index_path_for(const char *admindir);  /* sibling /d99/index.bin */

/* ---- binary mmap index (fast read-only queries) ---- */
typedef struct d99_index d99_index;

d99_index *d99_index_open(const char *index_path, const char *status_path);
void       d99_index_close(d99_index *ix);
size_t     d99_index_count(d99_index *ix);
const char *d99_index_name(d99_index *ix, size_t i);
const char *d99_index_version(d99_index *ix, size_t i);
const char *d99_index_arch(d99_index *ix, size_t i);
const char *d99_index_stanza(d99_index *ix, size_t i);
const char *d99_index_summary(d99_index *ix, size_t i);
int  d99_index_status(d99_index *ix, size_t i, int *sel, int *state, int *flags);
int  d99_index_find_pkg(d99_index *ix, const char *name, size_t *out);
size_t d99_index_pkg_nfiles(d99_index *ix, size_t i);
const char *d99_index_pkg_file(d99_index *ix, size_t i, size_t j);
/* file ownership: exact path lookup via hash table */
int  d99_index_find_file(d99_index *ix, const char *path,
                         size_t *pkg_out, size_t *file_out);
int  d99_index_build(const char *admindir, const char *index_path);

#endif /* D99_DB_H */
