#include "inst.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int hook_run(struct d99_ctx *c, const char *pkg, const char *version,
             const char *arch, const char *script, char *const args[])
{
    char *path = d99_xasprintf("%s/info/%s.%s", c->admindir, pkg, script);
    pid_t pid;
    int status;
    int n, i;

    if (!d99_file_exists(path)) {
        d99_verbose("no %s script for %s", script, pkg);
        free(path);
        return 0;
    }
    d99_verbose("running %s for %s", path, pkg);

    pid = fork();
    if (pid < 0) {
        d99_warn("fork failed: %s", strerror(errno));
        free(path);
        return -1;
    }
    if (pid == 0) {
        char **av;
        /* build argv: [script, args..., NULL] */
        n = 0;
        while (args && args[n])
            n++;
        av = d99_xmalloc(((size_t)n + 2) * sizeof(char *));
        av[0] = path;
        for (i = 0; i < n; i++)
            av[i + 1] = args[i];
        av[n + 1] = NULL;

        setenv("DPKG_MAINTSCRIPT_NAME", script, 1);
        setenv("DPKG_MAINTSCRIPT_PACKAGE", pkg, 1);
        setenv("DPKG_MAINTSCRIPT_ARCH", arch && *arch ? arch : "all", 1);
        setenv("DPKG_ROOT", c->root, 1);
        setenv("DPKG_ADMINDIR", c->admindir, 1);
        (void)version;

        if (chdir("/") != 0)
            _exit(127);
        {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, 0);
                close(devnull);
            }
        }
        execve(path, av, environ);
        fprintf(stderr, "d99: cannot execute maintainer script %s: %s\n",
                path, strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        d99_warn("waitpid failed for %s", path);
        free(path);
        return -1;
    }
    free(path);
    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc == 127)
            d99_warn("maintainer script could not be executed");
        return rc;
    }
    if (WIFSIGNALED(status)) {
        d99_warn("maintainer script killed by signal %d", WTERMSIG(status));
        return -1;
    }
    return 0;
}
