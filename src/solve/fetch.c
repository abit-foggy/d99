#include "solve.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FETCH_MAX_REDIRECTS 8

static int copy_file(const char *src, const char *dest)
{
    int in = open(src, O_RDONLY);
    int out;
    char buf[65536];
    ssize_t r;
    int fail = 0;

    if (in < 0)
        return -1;
    out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                fail = 1;
                break;
            }
            off += w;
        }
        if (fail)
            break;
    }
    if (r < 0)
        fail = 1;
    close(in);
    if (close(out) != 0)
        fail = 1;
    return fail ? -1 : 0;
}

static int save_tmp_from_stream(FILE *in, const char *dest,
                                long long content_length, int chunked)
{
    char tmpl[4096];
    char *dir = d99_dirname_dup(dest);
    int fd;
    FILE *out;
    unsigned char buf[65536];
    long long total = 0;
    int fail = 0;

    snprintf(tmpl, sizeof tmpl, "%s/.d99fetch.XXXXXX", dir ? dir : ".");
    free(dir);
    fd = mkstemp(tmpl);
    if (fd < 0)
        return -1;
    fchmod(fd, 0644);
    out = fdopen(fd, "wb");
    if (!out) {
        close(fd);
        unlink(tmpl);
        return -1;
    }

    if (chunked) {
        char *line = NULL;
        size_t cap = 0;
        for (;;) {
            long long chunk;
            if (getline(&line, &cap, in) < 0) {
                fail = 1;
                break;
            }
            chunk = strtoll(d99_trim(line), NULL, 16);
            if (chunk == 0) {
                ssize_t gl = getline(&line, &cap, in);   /* trailing CRLF */
                (void)gl;
                break;
            }
            while (chunk > 0) {
                size_t want = chunk > (long long)sizeof buf
                              ? sizeof buf : (size_t)chunk;
                size_t got = fread(buf, 1, want, in);
                if (got == 0) {
                    fail = 1;
                    break;
                }
                fwrite(buf, 1, got, out);
                chunk -= (long long)got;
                total += (long long)got;
            }
            if (fail)
                break;
            {
                ssize_t gl = getline(&line, &cap, in);   /* chunk CRLF */
                (void)gl;
            }
        }
        free(line);
    } else if (content_length >= 0) {
        while (total < content_length) {
            size_t want = content_length - total > (long long)sizeof buf
                          ? sizeof buf : (size_t)(content_length - total);
            size_t got = fread(buf, 1, want, in);
            if (got == 0) {
                fail = 1;
                break;
            }
            fwrite(buf, 1, got, out);
            total += (long long)got;
        }
    } else {
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, got, out);
    }

    if (fflush(out) != 0 || fsync(fd) != 0)
        fail = 1;
    fclose(out);
    if (fail || (content_length >= 0 && !chunked && total != content_length)) {
        unlink(tmpl);
        return -1;
    }
    if (rename(tmpl, dest) != 0) {
        unlink(tmpl);
        return -1;
    }
    return 0;
}

static int http_get(const char *url, const char *dest, int depth, int quiet);

/* Resolve a possibly-relative Location header. */
static char *resolve_url(const char *base, const char *loc)
{
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0 ||
        strncmp(loc, "file://", 7) == 0)
        return d99_xstrdup(loc);
    if (loc[0] == '/') {
        /* same host */
        const char *host = strstr(base, "://");
        const char *path;
        char *res;
        host += 3;
        path = strchr(host, '/');
        if (path) {
            size_t n = (size_t)(path - base);
            res = d99_xmalloc(n + strlen(loc) + 1);
            memcpy(res, base, n);
            memcpy(res + n, loc, strlen(loc) + 1);
        } else {
            res = d99_xasprintf("%s%s", base, loc);
        }
        return res;
    }
    return d99_xasprintf("%s/%s", base, loc);
}

static int http_get(const char *url, const char *dest, int depth, int quiet)
{
    const char *p = url + 7;   /* skip http:// */
    const char *path;
    char *colon;
    char host[256];
    int port = 80;
    char portstr[16];
    struct addrinfo hints, *res = NULL, *ai;
    int sock = -1;
    FILE *in = NULL;
    char req[4096];
    char status[512];
    int code = 0;
    char *location = NULL;
    long long content_length = -1;
    int chunked = 0;
    char *line = NULL;
    size_t cap = 0;
    int rc = -1;

    if (depth > FETCH_MAX_REDIRECTS) {
        if (!quiet)
            fprintf(stderr, "d99-solve: too many redirects fetching %s\n", url);
        return -1;
    }

    path = strchr(p, '/');
    if (!path)
        path = "/";
    {
        size_t hostn = (size_t)(path - p);
        if (hostn >= sizeof host)
            hostn = sizeof host - 1;
        memcpy(host, p, hostn);
        host[hostn] = '\0';
    }
    colon = strchr(host, ':');
    if (colon) {
        port = atoi(colon + 1);
        if (port <= 0)
            port = 80;
        *colon = '\0';
    }
    snprintf(portstr, sizeof portstr, "%d", port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        if (!quiet)
            fprintf(stderr, "d99-solve: cannot resolve %s\n", host);
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
            continue;
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        if (!quiet)
            fprintf(stderr, "d99-solve: cannot connect to %s:%d\n", host, port);
        return -1;
    }
    {
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    in = fdopen(sock, "r+");
    if (!in) {
        close(sock);
        return -1;
    }

    struct stat dst_st;
    char ims_hdr[128] = "";
    if (stat(dest, &dst_st) == 0 && dst_st.st_size > 0) {
        struct tm tm;
        gmtime_r(&dst_st.st_mtime, &tm);
        char date_buf[64];
        strftime(date_buf, sizeof date_buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
        snprintf(ims_hdr, sizeof ims_hdr, "If-Modified-Since: %s\r\n", date_buf);
    }

    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: d99-solve/%s\r\n"
             "Accept: */*\r\n"
             "%s"
             "Connection: close\r\n"
             "\r\n", path, host, D99_VERSION, ims_hdr);
    fputs(req, in);
    fflush(in);

    if (!fgets(status, sizeof status, in)) {
        if (!quiet)
            fprintf(stderr, "d99-solve: empty response from %s\n", host);
        fclose(in);
        return -1;
    }
    {
        char *sp = strchr(status, ' ');
        if (sp)
            code = atoi(sp + 1);
    }
    while (getline(&line, &cap, in) > 0) {
        char *t = d99_trim(line);
        if (*t == '\0')
            break;
        if (strncasecmp(t, "Content-Length:", 15) == 0)
            content_length = strtoll(t + 15, NULL, 10);
        else if (strncasecmp(t, "Transfer-Encoding:", 18) == 0 &&
                 strstr(t, "chunked"))
            chunked = 1;
        else if (strncasecmp(t, "Location:", 9) == 0) {
            free(location);
            location = d99_xstrdup(d99_trim(t + 9));
        }
    }
    free(line);

    if (code >= 301 && code <= 308 && location) {
        char *next = resolve_url(url, location);
        fclose(in);
        free(location);
        rc = d99_fetch(next, dest, quiet);
        free(next);
        return rc;
    }
    if (code == 304) {
        /* Not Modified (cache hit) */
        fclose(in);
        free(location);
        return 1;
    }
    if (code != 200) {
        if (!quiet)
            fprintf(stderr, "d99-solve: HTTP %d fetching %s\n", code, url);
        fclose(in);
        free(location);
        return -1;
    }
    rc = save_tmp_from_stream(in, dest, content_length, chunked);
    fclose(in);
    free(location);
    if (rc != 0)
        if (!quiet)
            fprintf(stderr, "d99-solve: transfer failed for %s\n", url);
    return rc;
}

static int fetch_https(const char *url, const char *dest, int quiet)
{
    char tmpl[4096];
    char *dir = d99_dirname_dup(dest);
    int fd;
    pid_t pid;
    int status;
    struct stat st;
    int has_dest = (stat(dest, &st) == 0 && st.st_size > 0);

    snprintf(tmpl, sizeof tmpl, "%s/.d99fetch.XXXXXX", dir ? dir : ".");
    free(dir);
    fd = mkstemp(tmpl);
    if (fd < 0)
        return -1;
    close(fd);

    pid = fork();
    if (pid < 0) {
        unlink(tmpl);
        return -1;
    }
    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        if (has_dest)
            execlp("curl", "curl", "-fsSL", "-z", dest, "-o", tmpl, url, (char *)NULL);
        else
            execlp("curl", "curl", "-fsSL", "-o", tmpl, url, (char *)NULL);
        execlp("wget", "wget", "-q", "-N", "-O", tmpl, url, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmpl);
        return -1;
    }
    if (has_dest) {
        struct stat tmp_st;
        if (stat(tmpl, &tmp_st) != 0 || tmp_st.st_size == 0) {
            unlink(tmpl);
            return 1; /* Not modified (cache hit) */
        }
    }
    if (rename(tmpl, dest) != 0) {
        unlink(tmpl);
        return -1;
    }
    return 0;
}

int d99_fetch(const char *url, const char *dest, int quiet)
{
    if (strncmp(url, "file://", 7) == 0) {
        const char *src = url + 7;
        /* file://localhost/path and file:///path */
        if (strncmp(src, "localhost/", 10) == 0)
            src += 9;
        struct stat st_src, st_dst;
        if (stat(src, &st_src) == 0 && stat(dest, &st_dst) == 0) {
            if (st_src.st_mtime <= st_dst.st_mtime && st_src.st_size == st_dst.st_size)
                return 1; /* Cache hit */
        }
        if (copy_file(src, dest) != 0) {
            if (!quiet)
                fprintf(stderr, "d99-solve: cannot copy file://%s\n", src);
            return -1;
        }
        return 0;
    }
    if (strncmp(url, "http://", 7) == 0)
        return http_get(url, dest, 0, quiet);
    if (strncmp(url, "https://", 8) == 0)
        return fetch_https(url, dest, quiet);
    fprintf(stderr, "d99-solve: unsupported URL scheme in '%s'\n", url);
    return -1;
}
