#ifndef MANIFEST_H
#define MANIFEST_H

#include "d99_util.h"

typedef struct {
    char *name, *version, *arch, *section, *priority, *maintainer, *homepage;
    char *depends, *predepends, *recommends, *suggests;
    char *conflicts, *breaks, *replaces, *provides;
    char *description;
    char *desc_last;            /* current description accumulator */
    d99_strvec files_dst, files_src;   /* parallel: install path = source path */
    d99_strvec conffiles;
    d99_strvec script_names, script_paths;
    char **extra;               /* verbatim extra control fields */
    size_t extra_n;
    long long installed_size;   /* KiB, computed during staging */
    d99_arena *ar;
} d99_manifest;

int d99_manifest_load(const char *path, d99_manifest *m);
void d99_manifest_free(d99_manifest *m);

#endif /* MANIFEST_H */
