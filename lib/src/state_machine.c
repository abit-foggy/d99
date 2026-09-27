#include "d99_state.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int d99_state_next(int cur, enum d99_evt ev, int *next)
{
    switch (ev) {
    case D99_EVT_UNPACK:
        switch (cur) {
        case D99_PS_NOTINSTALLED:
        case D99_PS_CONFIGFILES:
        case D99_PS_HALFINSTALLED:
        case D99_PS_UNPACKED:
        case D99_PS_HALFCONFIGURED:
        case D99_PS_TRIGGERSPENDING:
        case D99_PS_INSTALLED:
            *next = D99_PS_UNPACKED;
            return 0;
        default:
            return -1;
        }
    case D99_EVT_CONFIGURE:
        switch (cur) {
        case D99_PS_UNPACKED:
        case D99_PS_HALFCONFIGURED:
        case D99_PS_TRIGGERSPENDING:
        case D99_PS_INSTALLED:
            *next = D99_PS_INSTALLED;
            return 0;
        default:
            return -1;
        }
    case D99_EVT_REMOVE:
        switch (cur) {
        case D99_PS_HALFINSTALLED:
        case D99_PS_UNPACKED:
        case D99_PS_HALFCONFIGURED:
        case D99_PS_TRIGGERSPENDING:
        case D99_PS_INSTALLED:
            *next = D99_PS_CONFIGFILES;
            return 0;
        default:
            return -1;
        }
    case D99_EVT_PURGE:
        *next = D99_PS_NOTINSTALLED;
        return 0;
    case D99_EVT_FAIL:
        /* callers decide between half-installed / half-configured */
        *next = cur;
        return 0;
    }
    return -1;
}

int d99_state_commit(d99_db *db, d99_pkg *p, int sel, int state, int flags,
                     const char *admindir)
{
    if (d99_pkg_set_status(db, p, sel, state, flags) != 0)
        return -1;
    return d99_db_save_status(db, admindir);
}

char *d99_info_path(const char *admindir, const char *pkg, const char *suffix)
{
    char *info = d99_path_join(admindir, "info");
    char *p = d99_xasprintf("%s/%s%s", info, pkg, suffix);
    free(info);
    return p;
}

int d99_info_write(const char *admindir, const char *pkg, const char *suffix,
                   const void *data, size_t len, int mode)
{
    char *path = d99_info_path(admindir, pkg, suffix);
    char *dir = d99_dirname_dup(path);
    int r;

    if (d99_mkdir_p(dir, 0755) != 0) {
        free(dir);
        free(path);
        return -1;
    }
    free(dir);
    r = d99_write_file_atomic(path, data, len);
    if (r == 0 && mode != 0644)
        chmod(path, (mode_t)mode);
    free(path);
    return r;
}

char *d99_info_read(const char *admindir, const char *pkg, const char *suffix,
                    size_t *len)
{
    char *path = d99_info_path(admindir, pkg, suffix);
    char *r = d99_read_file(path, len);
    free(path);
    return r;
}

int d99_info_delete_all(const char *admindir, const char *pkg)
{
    char *infodir = d99_path_join(admindir, "info");
    size_t plen = strlen(pkg);
    DIR *d = opendir(infodir);
    struct dirent *de;

    if (!d) {
        free(infodir);
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        if (strncmp(n, pkg, plen) == 0 && n[plen] == '.' && n[plen + 1]) {
            char *full = d99_path_join(infodir, n);
            unlink(full);
            free(full);
        }
    }
    closedir(d);
    free(infodir);
    return 0;
}
