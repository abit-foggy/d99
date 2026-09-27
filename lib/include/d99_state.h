#ifndef D99_STATE_H
#define D99_STATE_H

#include "d99_db.h"

/* transaction events */
enum d99_evt {
    D99_EVT_UNPACK,
    D99_EVT_CONFIGURE,
    D99_EVT_REMOVE,
    D99_EVT_PURGE,
    D99_EVT_FAIL
};

/* Validate a transition; sets *next and returns 0, or -1 if invalid. */
int d99_state_next(int cur, enum d99_evt ev, int *next);

/* Apply a state change and persist the status DB atomically
 * (tmp + rename + status-old backup). */
int d99_state_commit(d99_db *db, d99_pkg *p, int sel, int state, int flags,
                     const char *admindir);

/* /var/lib/dpkg/info helpers */
char *d99_info_path(const char *admindir, const char *pkg, const char *suffix);
int   d99_info_write(const char *admindir, const char *pkg, const char *suffix,
                     const void *data, size_t len, int mode);
char *d99_info_read(const char *admindir, const char *pkg, const char *suffix,
                    size_t *len);
/* unlink every info/PKG.* file (exact prefix match on "PKG.") */
int   d99_info_delete_all(const char *admindir, const char *pkg);

#endif /* D99_STATE_H */
