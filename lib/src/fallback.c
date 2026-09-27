#include "d99_fallback.h"
#include "d99_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* d99 tool -> upstream tool it replaces */
static const struct {
    const char *d99;
    const char *upstream;
} tool_map[] = {
    { "d99-deb",   "dpkg-deb" },
    { "d99-query", "dpkg-query" },
    { "d99-inst",  "dpkg" },
    { "d99-solve", "apt-get" },
    { "d99-build", "dpkg-buildpackage" },
};

const char *d99_fallback_tool(const char *argv0)
{
    const char *base = strrchr(argv0, '/');
    size_t i;

    base = base ? base + 1 : argv0;
    for (i = 0; i < sizeof(tool_map) / sizeof(tool_map[0]); i++)
        if (strcmp(base, tool_map[i].d99) == 0)
            return tool_map[i].upstream;
    /* Already invoked under an upstream name (post-swap symlink). */
    return base;
}

char *d99_fallback_path(const char *argv0)
{
    return d99_xasprintf("%s/%s.upstream", D99_UPSTREAM_DIR, d99_fallback_tool(argv0));
}

int d99_fallback_exists(const char *argv0)
{
    char *path = d99_fallback_path(argv0);
    int ok = access(path, X_OK) == 0;
    free(path);
    return ok;
}

int d99_fallback_exec(const char *argv0, char *const argv[])
{
    char *path = d99_fallback_path(argv0);

    if (access(path, X_OK) != 0) {
        free(path);
        return -1;
    }
    d99_verbose("falling back to %s", path);
    execve(path, argv, environ);
    /* only reached on exec failure */
    d99_warn("exec %s failed", path);
    free(path);
    return -1;
}

void d99_fallback_or_die(const char *argv0, char *const argv[])
{
    if (d99_fallback_exec(argv0, argv) != 0) {
        char *path = d99_fallback_path(argv0);
        d99_die("no d99 handler for this invocation and no upstream fallback at %s",
                path);
    }
}

const char *d99_cmd_name(const char *argv0, const char *d99_name, const char *sys_name)
{
    const char *base;
    if (!argv0 || !*argv0)
        return d99_name ? d99_name : (sys_name ? sys_name : "");
    base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;

    if (sys_name && (strcmp(base, sys_name) == 0 ||
                     (strcmp(sys_name, "apt") == 0 && strcmp(base, "apt-get") == 0)))
        return base;

    if (sys_name) {
        char linkpath[256];
        char target[256];
        snprintf(linkpath, sizeof(linkpath), "%s/%s", D99_UPSTREAM_DIR, sys_name);
        ssize_t n = readlink(linkpath, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            if (d99_name && strstr(target, d99_name))
                return sys_name;
        }
    }
    return base;
}
