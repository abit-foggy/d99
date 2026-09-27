#ifndef D99_FALLBACK_H
#define D99_FALLBACK_H

#define D99_UPSTREAM_DIR "/usr/bin"

/* Map an argv[0] (e.g. "d99-deb" or "dpkg" after the swap) to the
 * upstream tool name it replaces.  Returns a pointer to a static or
 * per-call string; do not free. */
const char *d99_fallback_tool(const char *argv0);

/* Build the /usr/bin/<tool>.upstream path for argv[0].  malloc'd. */
char *d99_fallback_path(const char *argv0);

/* 1 if an upstream replacement exists for argv[0]. */
int d99_fallback_exists(const char *argv0);

/* execve() the upstream tool, preserving argv.  Only returns on failure
 * (-1: upstream missing or exec failed).  This is the spec'd API:
 *     d99_fallback_exec(argv[0], argv);
 */
int d99_fallback_exec(const char *argv0, char *const argv[]);

/* Like d99_fallback_exec() but prints a fatal error and exits if the
 * upstream tool is unavailable. */
void d99_fallback_or_die(const char *argv0, char *const argv[]);

const char *d99_cmd_name(const char *argv0, const char *d99_name, const char *sys_name);

#endif /* D99_FALLBACK_H */
