#include "solve.h"
#include "d99_api.h"
#include "d99_sat.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ==================== Helpers ==================== */

static void json_error(d99_json_buf *b, const char *msg)
{
    d99_jb_raw(b, "{\"status\":\"error\",\"error\":");
    d99_jb_str(b, msg);
    d99_jb_raw(b, "}");
}

/* ==================== api search ==================== */

struct api_search_hit {
    char *name;
    char *version;
    char *summary;
    int score;
};

static int api_hit_cmp(const void *a, const void *b)
{
    const struct api_search_hit *ha = a;
    const struct api_search_hit *hb = b;
    if (ha->score != hb->score)
        return hb->score - ha->score;
    return strcmp(ha->name, hb->name);
}

static char *api_search_json(paths *p, const char *query)
{
    d99_json_buf b;
    d99_jb_init(&b);

    if (!query || !*query) {
        json_error(&b, "search query cannot be empty");
        return d99_jb_finish(&b);
    }

    d99_repo *repo = repo_load(p->lists_dir);
    if (!repo || repo->n == 0) {
        if (repo) repo_free(repo);
        json_error(&b, "package lists are empty; run update first");
        return d99_jb_finish(&b);
    }

    d99_db *db = d99_db_load_status(p->admindir);

    char *needle = d99_xstrdup(query);
    for (char *q = needle; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    size_t needle_len = strlen(needle);

    struct api_search_hit *hits = NULL;
    size_t nhits = 0, hit_cap = 0;

    if (repo->disk_recs && repo->strings) {
        const struct cand_disk_rec *recs = (const struct cand_disk_rec *)repo->disk_recs;
        const char *strings = repo->strings;
        for (size_t i = 0; i < repo->n; i++) {
            const char *name = strings + recs[i].name_off;
            const char *summary = recs[i].summary_off ? strings + recs[i].summary_off : "";
            int score = 0;

            if (strcasecmp(name, needle) == 0)
                score += 1000;
            else if (strncasecmp(name, needle, needle_len) == 0)
                score += 500;
            else if (d99_strcasestr_match(name, needle, needle_len))
                score += 200;

            if (summary[0] && d99_strcasestr_match(summary, needle, needle_len))
                score += 50;

            if (score > 0) {
                if (nhits == hit_cap) {
                    hit_cap = hit_cap ? hit_cap * 2 : 64;
                    hits = d99_xrealloc(hits, hit_cap * sizeof(struct api_search_hit));
                }
                hits[nhits].name = d99_xstrdup(name);
                hits[nhits].version = d99_xstrdup(strings + recs[i].version_off);
                hits[nhits].summary = d99_xstrdup(summary);
                hits[nhits].score = score;
                nhits++;
            }
        }
    } else {
        for (size_t i = 0; i < repo->n; i++) {
            d99_cand *c = repo_get(repo, i);
            if (!c) continue;
            const char *name = c->name;
            const char *summary = c->summary ? c->summary : "";
            int score = 0;

            if (strcasecmp(name, needle) == 0)
                score += 1000;
            else if (strncasecmp(name, needle, needle_len) == 0)
                score += 500;
            else if (d99_strcasestr_match(name, needle, needle_len))
                score += 200;

            if (summary[0] && d99_strcasestr_match(summary, needle, needle_len))
                score += 50;

            if (score > 0) {
                if (nhits == hit_cap) {
                    hit_cap = hit_cap ? hit_cap * 2 : 64;
                    hits = d99_xrealloc(hits, hit_cap * sizeof(struct api_search_hit));
                }
                hits[nhits].name = d99_xstrdup(name);
                hits[nhits].version = d99_xstrdup(c->version);
                hits[nhits].summary = d99_xstrdup(summary);
                hits[nhits].score = score;
                nhits++;
            }
        }
    }

    if (nhits > 1)
        qsort(hits, nhits, sizeof(struct api_search_hit), api_hit_cmp);

    d99_jb_raw(&b, "{\"status\":\"ok\",\"query\":");
    d99_jb_str(&b, query);
    d99_jb_raw(&b, ",\"count\":");
    d99_jb_int(&b, (long long)nhits);
    d99_jb_raw(&b, ",\"results\":[");

    for (size_t i = 0; i < nhits; i++) {
        if (i > 0) d99_jb_raw(&b, ",");
        d99_jb_raw(&b, "{\"name\":");
        d99_jb_str(&b, hits[i].name);
        d99_jb_raw(&b, ",\"version\":");
        d99_jb_str(&b, hits[i].version);
        d99_jb_raw(&b, ",\"summary\":");
        d99_jb_str(&b, hits[i].summary);
        d99_jb_raw(&b, ",\"score\":");
        d99_jb_int(&b, hits[i].score);

        d99_pkg *pp = db ? d99_db_find(db, hits[i].name) : NULL;
        int inst = (pp && pp->state == D99_PS_INSTALLED);
        d99_jb_raw(&b, ",\"installed\":");
        d99_jb_bool(&b, inst);
        if (inst && pp->version) {
            d99_jb_raw(&b, ",\"installed_version\":");
            d99_jb_str(&b, pp->version);
        }

        d99_jb_raw(&b, "}");
        free(hits[i].name);
        free(hits[i].version);
        free(hits[i].summary);
    }
    d99_jb_raw(&b, "]}");

    free(hits);
    free(needle);
    if (db) d99_db_free(db);
    repo_free(repo);
    return d99_jb_finish(&b);
}

/* ==================== api show ==================== */

static char *api_show_json(paths *p, const char *pkg_name)
{
    d99_json_buf b;
    d99_jb_init(&b);

    if (!pkg_name || !*pkg_name) {
        json_error(&b, "package name required");
        return d99_jb_finish(&b);
    }

    d99_repo *repo = repo_load(p->lists_dir);
    d99_db *db = d99_db_load_status(p->admindir);

    d99_cand *c = repo ? repo_find(repo, pkg_name, D99_DEP_NONE, NULL) : NULL;
    d99_pkg *pp = db ? d99_db_find(db, pkg_name) : NULL;

    if (!c && !pp) {
        if (db) d99_db_free(db);
        if (repo) repo_free(repo);
        json_error(&b, "package not found");
        return d99_jb_finish(&b);
    }

    d99_jb_raw(&b, "{\"status\":\"ok\",\"package\":");
    d99_jb_str(&b, pkg_name);

    if (c) {
        d99_jb_raw(&b, ",\"candidate_version\":");
        d99_jb_str(&b, c->version);
        d99_jb_raw(&b, ",\"architecture\":");
        d99_jb_str(&b, c->arch ? c->arch : "");
        d99_jb_raw(&b, ",\"size\":");
        d99_jb_int(&b, c->size ? atoll(c->size) : 0);
        d99_jb_raw(&b, ",\"filename\":");
        d99_jb_str(&b, c->filename ? c->filename : "");
        d99_jb_raw(&b, ",\"base_uri\":");
        d99_jb_str(&b, c->base_uri ? c->base_uri : "");
        d99_jb_raw(&b, ",\"sha256\":");
        d99_jb_str(&b, c->sha256 ? c->sha256 : "");
        d99_jb_raw(&b, ",\"summary\":");
        d99_jb_str(&b, c->summary ? c->summary : "");
    }

    int installed = (pp && (pp->state == D99_PS_INSTALLED || pp->state == D99_PS_UNPACKED));
    d99_jb_raw(&b, ",\"installed\":");
    d99_jb_bool(&b, installed);

    if (pp) {
        d99_jb_raw(&b, ",\"installed_version\":");
        d99_jb_str(&b, pp->version ? pp->version : "");
        d99_jb_raw(&b, ",\"status_state\":");
        d99_jb_str(&b, d99_state_name(pp->state));
    }

    d99_jb_raw(&b, "}");

    if (db) d99_db_free(db);
    if (repo) repo_free(repo);
    return d99_jb_finish(&b);
}

/* ==================== api updates ==================== */

static char *api_updates_json(paths *p)
{
    d99_json_buf b;
    d99_jb_init(&b);

    d99_db *db = d99_db_load_status(p->admindir);
    d99_repo *repo = repo_load(p->lists_dir);

    if (!db) {
        if (repo) repo_free(repo);
        json_error(&b, "cannot load package database");
        return d99_jb_finish(&b);
    }
    if (!repo) {
        d99_db_free(db);
        json_error(&b, "cannot load repository index; run update first");
        return d99_jb_finish(&b);
    }

    size_t count = 0;
    d99_jb_raw(&b, "{\"status\":\"ok\",\"updates\":[");

    for (size_t i = 0; i < d99_db_count(db); i++) {
        d99_pkg *pp = d99_db_at(db, i);
        if (pp->state != D99_PS_INSTALLED && pp->state != D99_PS_UNPACKED)
            continue;
        if (pp->sel == D99_SEL_HOLD)
            continue;
        d99_cand *c = repo_find(repo, pp->name, D99_DEP_NONE, NULL);
        if (c && c->version && pp->version && d99_vercmp(c->version, pp->version) > 0) {
            if (count > 0) d99_jb_raw(&b, ",");
            d99_jb_raw(&b, "{\"name\":");
            d99_jb_str(&b, pp->name);
            d99_jb_raw(&b, ",\"installed_version\":");
            d99_jb_str(&b, pp->version);
            d99_jb_raw(&b, ",\"candidate_version\":");
            d99_jb_str(&b, c->version);
            d99_jb_raw(&b, ",\"size\":");
            d99_jb_int(&b, c->size ? atoll(c->size) : 0);
            d99_jb_raw(&b, ",\"architecture\":");
            d99_jb_str(&b, c->arch ? c->arch : "");
            d99_jb_raw(&b, "}");
            count++;
        }
    }

    d99_jb_raw(&b, "],\"count\":");
    d99_jb_int(&b, (long long)count);
    d99_jb_raw(&b, "}");

    d99_db_free(db);
    repo_free(repo);
    return d99_jb_finish(&b);
}

/* ==================== api plan ==================== */

static char *api_plan_json(const char *argv0, paths *p, const char *action_name,
                           const char **pkgs, size_t npkgs)
{
    d99_json_buf b;
    d99_jb_init(&b);

    if (strcmp(action_name, "install") == 0) {
        if (npkgs == 0) {
            json_error(&b, "install requires at least one target package");
            return d99_jb_finish(&b);
        }
        struct target_spec *targets = d99_xmalloc(npkgs * sizeof(struct target_spec));
        int bad = 0;
        for (size_t i = 0; i < npkgs; i++) {
            if (parse_target(pkgs[i], &targets[i]) != 0) bad = 1;
        }
        if (bad) {
            for (size_t i = 0; i < npkgs; i++) {
                free(targets[i].name);
                free(targets[i].ver);
            }
            free(targets);
            json_error(&b, "invalid target specification format");
            return d99_jb_finish(&b);
        }

        action *acts = NULL;
        size_t nacts = 0;
        int r = plan_install("d99-solve", p, targets, (int)npkgs, &acts, &nacts);
        for (size_t i = 0; i < npkgs; i++) {
            free(targets[i].name);
            free(targets[i].ver);
        }
        free(targets);

        if (r != 0) {
            json_error(&b, "dependency resolution failed; unsatisfiable constraints");
            return d99_jb_finish(&b);
        }

        long long total_size = 0;
        d99_jb_raw(&b, "{\"status\":\"ok\",\"action\":\"install\",\"count\":");
        d99_jb_int(&b, (long long)nacts);
        d99_jb_raw(&b, ",\"operations\":[");

        for (size_t i = 0; i < nacts; i++) {
            if (i > 0) d99_jb_raw(&b, ",");
            d99_jb_raw(&b, "{\"op\":");
            d99_jb_str(&b, acts[i].old_version ? "upgrade" : "install");
            d99_jb_raw(&b, ",\"name\":");
            d99_jb_str(&b, acts[i].name);
            d99_jb_raw(&b, ",\"version\":");
            d99_jb_str(&b, acts[i].version);
            d99_jb_raw(&b, ",\"old_version\":");
            d99_jb_str(&b, acts[i].old_version);
            d99_jb_raw(&b, ",\"architecture\":");
            d99_jb_str(&b, acts[i].arch);
            d99_jb_raw(&b, ",\"size\":");
            long long sz = acts[i].size ? atoll(acts[i].size) : 0;
            total_size += sz;
            d99_jb_int(&b, sz);
            d99_jb_raw(&b, ",\"sha256\":");
            d99_jb_str(&b, acts[i].sha256);
            if (acts[i].base_uri && acts[i].filename) {
                char *uri = d99_xasprintf("%s/%s", acts[i].base_uri, acts[i].filename);
                d99_jb_raw(&b, ",\"uri\":");
                d99_jb_str(&b, uri);
                free(uri);
            }
            d99_jb_raw(&b, "}");
        }

        d99_jb_raw(&b, "],\"total_download_size\":");
        d99_jb_int(&b, total_size);
        d99_jb_raw(&b, "}");

        free_actions(acts, nacts);
        return d99_jb_finish(&b);

    } else if (strcmp(action_name, "upgrade") == 0 ||
               strcmp(action_name, "dist-upgrade") == 0 ||
               strcmp(action_name, "full-upgrade") == 0) {

        d99_db *db = d99_db_load_status(p->admindir);
        d99_repo *repo = repo_load(p->lists_dir);
        if (!db || !repo) {
            if (db) d99_db_free(db);
            if (repo) repo_free(repo);
            json_error(&b, "cannot load database or repository indexes");
            return d99_jb_finish(&b);
        }

        struct target_spec *targets = NULL;
        size_t ntargets = 0, target_cap = 0;

        for (size_t i = 0; i < d99_db_count(db); i++) {
            d99_pkg *pp = d99_db_at(db, i);
            if (pp->state != D99_PS_INSTALLED && pp->state != D99_PS_UNPACKED)
                continue;
            if (pp->sel == D99_SEL_HOLD)
                continue;
            d99_cand *c = repo_find(repo, pp->name, D99_DEP_NONE, NULL);
            if (c && c->version && pp->version && d99_vercmp(c->version, pp->version) > 0) {
                if (ntargets == target_cap) {
                    target_cap = target_cap ? target_cap * 2 : 16;
                    targets = d99_xrealloc(targets, target_cap * sizeof(struct target_spec));
                }
                targets[ntargets].name = d99_xstrdup(pp->name);
                targets[ntargets].op = D99_DEP_NONE;
                targets[ntargets].ver = d99_xstrdup(c->version);
                ntargets++;
            }
        }
        repo_free(repo);
        d99_db_free(db);

        if (ntargets == 0) {
            d99_jb_raw(&b, "{\"status\":\"ok\",\"action\":\"upgrade\",\"count\":0,\"total_download_size\":0,\"operations\":[]}");
            return d99_jb_finish(&b);
        }

        action *acts = NULL;
        size_t nacts = 0;
        int r = plan_install("d99-solve", p, targets, (int)ntargets, &acts, &nacts);
        for (size_t i = 0; i < ntargets; i++) {
            free(targets[i].name);
            free(targets[i].ver);
        }
        free(targets);

        if (r != 0) {
            json_error(&b, "upgrade resolution failed; conflict detected");
            return d99_jb_finish(&b);
        }

        long long total_size = 0;
        d99_jb_raw(&b, "{\"status\":\"ok\",\"action\":\"upgrade\",\"count\":");
        d99_jb_int(&b, (long long)nacts);
        d99_jb_raw(&b, ",\"operations\":[");

        for (size_t i = 0; i < nacts; i++) {
            if (i > 0) d99_jb_raw(&b, ",");
            d99_jb_raw(&b, "{\"op\":");
            d99_jb_str(&b, acts[i].old_version ? "upgrade" : "install");
            d99_jb_raw(&b, ",\"name\":");
            d99_jb_str(&b, acts[i].name);
            d99_jb_raw(&b, ",\"version\":");
            d99_jb_str(&b, acts[i].version);
            d99_jb_raw(&b, ",\"old_version\":");
            d99_jb_str(&b, acts[i].old_version);
            d99_jb_raw(&b, ",\"architecture\":");
            d99_jb_str(&b, acts[i].arch);
            d99_jb_raw(&b, ",\"size\":");
            long long sz = acts[i].size ? atoll(acts[i].size) : 0;
            total_size += sz;
            d99_jb_int(&b, sz);
            d99_jb_raw(&b, ",\"sha256\":");
            d99_jb_str(&b, acts[i].sha256);
            if (acts[i].base_uri && acts[i].filename) {
                char *uri = d99_xasprintf("%s/%s", acts[i].base_uri, acts[i].filename);
                d99_jb_raw(&b, ",\"uri\":");
                d99_jb_str(&b, uri);
                free(uri);
            }
            d99_jb_raw(&b, "}");
        }

        d99_jb_raw(&b, "],\"total_download_size\":");
        d99_jb_int(&b, total_size);
        d99_jb_raw(&b, "}");

        free_actions(acts, nacts);
        return d99_jb_finish(&b);

    } else if (strcmp(action_name, "remove") == 0 || strcmp(action_name, "purge") == 0) {
        (void)argv0;
        d99_db *db = d99_db_load_status(p->admindir);
        if (!db) {
            json_error(&b, "cannot load package database");
            return d99_jb_finish(&b);
        }
        d99_jb_raw(&b, "{\"status\":\"ok\",\"action\":\"remove\",\"operations\":[");
        size_t count = 0;
        for (size_t i = 0; i < npkgs; i++) {
            d99_pkg *pp = d99_db_find(db, pkgs[i]);
            if (pp && pp->state == D99_PS_INSTALLED) {
                if (count > 0) d99_jb_raw(&b, ",");
                d99_jb_raw(&b, "{\"op\":\"remove\",\"name\":");
                d99_jb_str(&b, pp->name);
                d99_jb_raw(&b, ",\"version\":");
                d99_jb_str(&b, pp->version);
                d99_jb_raw(&b, "}");
                count++;
            }
        }
        d99_jb_raw(&b, "],\"count\":");
        d99_jb_int(&b, (long long)count);
        d99_jb_raw(&b, "}");
        d99_db_free(db);
        return d99_jb_finish(&b);
    } else {
        json_error(&b, "unsupported plan action (use install, upgrade, or remove)");
        return d99_jb_finish(&b);
    }
}

/* ==================== api list ==================== */

static char *api_list_json(paths *p, const char *type, const char *pattern)
{
    d99_json_buf b;
    d99_jb_init(&b);

    d99_db *db = d99_db_load_status(p->admindir);
    if (!db) {
        json_error(&b, "cannot load package database");
        return d99_jb_finish(&b);
    }

    size_t count = 0;
    d99_jb_raw(&b, "{\"status\":\"ok\",\"packages\":[");

    for (size_t i = 0; i < d99_db_count(db); i++) {
        d99_pkg *pp = d99_db_at(db, i);
        if (!type || strcmp(type, "installed") == 0) {
            if (pp->state != D99_PS_INSTALLED && pp->state != D99_PS_UNPACKED)
                continue;
        }
        if (pattern && *pattern) {
            if (!d99_glob_match(pattern, pp->name) &&
                strstr(pp->name, pattern) == NULL)
                continue;
        }

        if (count > 0) d99_jb_raw(&b, ",");
        d99_jb_raw(&b, "{\"name\":");
        d99_jb_str(&b, pp->name);
        d99_jb_raw(&b, ",\"version\":");
        d99_jb_str(&b, pp->version ? pp->version : "");
        d99_jb_raw(&b, ",\"architecture\":");
        d99_jb_str(&b, pp->arch ? pp->arch : "");
        d99_jb_raw(&b, ",\"status\":");
        d99_jb_str(&b, pp->state == D99_PS_INSTALLED ? "installed" : "other");
        d99_jb_raw(&b, "}");
        count++;
    }

    d99_jb_raw(&b, "],\"count\":");
    d99_jb_int(&b, (long long)count);
    d99_jb_raw(&b, "}");

    d99_db_free(db);
    return d99_jb_finish(&b);
}

/* ==================== api info ==================== */

static char *api_info_json(paths *p, const char *arch)
{
    d99_json_buf b;
    d99_jb_init(&b);

    d99_db *db = d99_db_load_status(p->admindir);
    d99_repo *repo = repo_load(p->lists_dir);

    size_t installed = db ? d99_db_count(db) : 0;
    size_t available = repo ? repo->n : 0;

    d99_jb_raw(&b, "{\"status\":\"ok\",\"version\":");
    d99_jb_str(&b, D99_VERSION);
    d99_jb_raw(&b, ",\"architecture\":");
    d99_jb_str(&b, arch);
    d99_jb_raw(&b, ",\"root\":");
    d99_jb_str(&b, p->root);
    d99_jb_raw(&b, ",\"admindir\":");
    d99_jb_str(&b, p->admindir);
    d99_jb_raw(&b, ",\"lists_dir\":");
    d99_jb_str(&b, p->lists_dir);
    d99_jb_raw(&b, ",\"cache_dir\":");
    d99_jb_str(&b, p->cache_dir);
    d99_jb_raw(&b, ",\"installed_count\":");
    d99_jb_int(&b, (long long)installed);
    d99_jb_raw(&b, ",\"available_count\":");
    d99_jb_int(&b, (long long)available);
    d99_jb_raw(&b, "}");

    if (db) d99_db_free(db);
    if (repo) repo_free(repo);
    return d99_jb_finish(&b);
}

/* ==================== Request Dispatcher (JSON-RPC & Text) ==================== */

static char *api_dispatch_request(paths *p, const char *arch, const char *line,
                                  const char *argv0)
{
    char *trimmed = d99_trim(d99_xstrdup(line));
    if (!*trimmed) {
        free(trimmed);
        return d99_xstrdup("{\"status\":\"ok\"}");
    }

    /* Check for JSON format */
    if (trimmed[0] == '{') {
        char *id_val = NULL;
        char *method = NULL;
        char *params_val = NULL;

        /* Extract id */
        char *id_pos = strstr(trimmed, "\"id\"");
        if (id_pos) {
            char *colon = strchr(id_pos, ':');
            if (colon) {
                while (*colon == ':' || *colon == ' ' || *colon == '\t') colon++;
                if (*colon == '"') {
                    char *endq = strchr(colon + 1, '"');
                    if (endq) id_val = d99_xstrndup(colon, (size_t)(endq - colon + 1));
                } else {
                    char *endn = colon;
                    while (*endn && *endn != ',' && *endn != '}' && *endn != ' ' && *endn != '\t')
                        endn++;
                    id_val = d99_xstrndup(colon, (size_t)(endn - colon));
                }
            }
        }

        /* Extract method */
        char *m_pos = strstr(trimmed, "\"method\"");
        if (m_pos) {
            char *q1 = strchr(m_pos + 8, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) method = d99_xstrndup(q1 + 1, (size_t)(q2 - q1 - 1));
            }
        }

        /* Extract params (either array or string) */
        char *p_pos = strstr(trimmed, "\"params\"");
        if (p_pos) {
            char *colon = strchr(p_pos, ':');
            if (colon) {
                while (*colon == ':' || *colon == ' ') colon++;
                params_val = d99_xstrdup(colon);
            }
        }

        char *inner = NULL;
        if (!method) {
            inner = d99_xstrdup("{\"status\":\"error\",\"error\":\"missing method in JSON-RPC request\"}");
        } else if (strcmp(method, "search") == 0) {
            char query[256] = {0};
            if (params_val) {
                char *q1 = strchr(params_val, '"');
                if (q1) {
                    char *q2 = strchr(q1 + 1, '"');
                    if (q2) {
                        size_t l = (size_t)(q2 - q1 - 1);
                        if (l >= sizeof(query)) l = sizeof(query) - 1;
                        memcpy(query, q1 + 1, l);
                        query[l] = '\0';
                    }
                }
            }
            inner = api_search_json(p, query);
        } else if (strcmp(method, "show") == 0) {
            char pkg[256] = {0};
            if (params_val) {
                char *q1 = strchr(params_val, '"');
                if (q1) {
                    char *q2 = strchr(q1 + 1, '"');
                    if (q2) {
                        size_t l = (size_t)(q2 - q1 - 1);
                        if (l >= sizeof(pkg)) l = sizeof(pkg) - 1;
                        memcpy(pkg, q1 + 1, l);
                        pkg[l] = '\0';
                    }
                }
            }
            inner = api_show_json(p, pkg);
        } else if (strcmp(method, "updates") == 0 || strcmp(method, "check-upgrade") == 0) {
            inner = api_updates_json(p);
        } else if (strcmp(method, "info") == 0) {
            inner = api_info_json(p, arch);
        } else if (strcmp(method, "plan") == 0) {
            inner = api_plan_json(argv0, p, "upgrade", NULL, 0);
        } else if (strcmp(method, "ping") == 0) {
            inner = d99_xstrdup("{\"status\":\"ok\",\"result\":\"pong\"}");
        } else {
            inner = d99_xasprintf("{\"status\":\"error\",\"error\":\"unknown method '%s'\"}", method);
        }

        char *resp = NULL;
        if (id_val && inner) {
            /* Insert "id": ... right after open brace */
            if (inner[0] == '{') {
                resp = d99_xasprintf("{\"id\":%s,%s", id_val, inner + 1);
                free(inner);
            } else {
                resp = inner;
            }
        } else {
            resp = inner;
        }

        free(id_val);
        free(method);
        free(params_val);
        free(trimmed);
        return resp;
    }

    /* Plain text line parsing: <cmd> [<args>...] */
    d99_strvec sv;
    d99_sv_init(&sv);
    char *tok_save = NULL;
    char *tok = strtok_r(trimmed, " \t\r\n", &tok_save);
    while (tok) {
        d99_sv_push(&sv, tok);
        tok = strtok_r(NULL, " \t\r\n", &tok_save);
    }

    char *resp = NULL;
    if (sv.n == 0) {
        resp = d99_xstrdup("{\"status\":\"ok\"}");
    } else if (strcmp(sv.v[0], "search") == 0) {
        resp = api_search_json(p, sv.n > 1 ? sv.v[1] : "");
    } else if (strcmp(sv.v[0], "show") == 0) {
        resp = api_show_json(p, sv.n > 1 ? sv.v[1] : "");
    } else if (strcmp(sv.v[0], "updates") == 0 || strcmp(sv.v[0], "check-upgrade") == 0) {
        resp = api_updates_json(p);
    } else if (strcmp(sv.v[0], "plan") == 0) {
        const char *act = (sv.n > 1) ? sv.v[1] : "install";
        const char **pkgs = (sv.n > 2) ? (const char **)&sv.v[2] : NULL;
        size_t npkgs = (sv.n > 2) ? sv.n - 2 : 0;
        resp = api_plan_json(argv0, p, act, pkgs, npkgs);
    } else if (strcmp(sv.v[0], "list") == 0) {
        const char *type = sv.n > 1 ? sv.v[1] : "installed";
        const char *pat = sv.n > 2 ? sv.v[2] : NULL;
        resp = api_list_json(p, type, pat);
    } else if (strcmp(sv.v[0], "info") == 0) {
        resp = api_info_json(p, arch);
    } else if (strcmp(sv.v[0], "ping") == 0) {
        resp = d99_xstrdup("{\"status\":\"ok\",\"result\":\"pong\"}");
    } else if (strcmp(sv.v[0], "quit") == 0 || strcmp(sv.v[0], "exit") == 0) {
        resp = d99_xstrdup("{\"status\":\"ok\",\"quit\":true}");
    } else {
        resp = d99_xasprintf("{\"status\":\"error\",\"error\":\"unknown api command '%s'\"}", sv.v[0]);
    }

    d99_sv_free(&sv);
    free(trimmed);
    return resp;
}

/* ==================== Interactive RPC Loop ==================== */

static int api_rpc_loop(paths *p, const char *arch, FILE *in, FILE *out,
                        const char *argv0)
{
    char buf[8192];
    while (fgets(buf, sizeof(buf), in)) {
        char *resp = api_dispatch_request(p, arch, buf, argv0);
        fputs(resp, out);
        fputc('\n', out);
        fflush(out);
        int is_quit = (strstr(resp, "\"quit\":true") != NULL);
        free(resp);
        if (is_quit)
            break;
    }
    return 0;
}

/* ==================== UNIX Domain Socket Server ==================== */

static volatile sig_atomic_t g_srv_running = 1;
static void sig_handler(int sig) { (void)sig; g_srv_running = 0; }

static int api_listen_socket(paths *p, const char *arch, const char *sock_path,
                             const char *argv0)
{
    if (!sock_path || !*sock_path) {
        fprintf(stderr, "d99-solve: api listen requires a socket path\n");
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("d99-solve socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    unlink(sock_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("d99-solve bind");
        close(fd);
        return 1;
    }

    if (listen(fd, 16) < 0) {
        perror("d99-solve listen");
        close(fd);
        unlink(sock_path);
        return 1;
    }

    chmod(sock_path, 0666);
    printf("d99-solve: API listening on unix domain socket %s\n", sock_path);
    fflush(stdout);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    while (g_srv_running) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;

        int client_fd = accept(fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        FILE *cin = fdopen(client_fd, "r");
        FILE *cout = fdopen(dup(client_fd), "w");
        if (cin && cout) {
            api_rpc_loop(p, arch, cin, cout, argv0);
        }
        if (cin) fclose(cin);
        if (cout) fclose(cout);
        close(client_fd);
    }

    close(fd);
    unlink(sock_path);
    return 0;
}

/* ==================== Entry point: cmd_api ==================== */

static void api_usage(const char *cmd_name)
{
    printf(
"Usage: %s api <command> [<args>...]\n"
"\n"
"Programmatic API Commands for external binaries:\n"
"  search <query>                 Search index, returns JSON array of hits\n"
"  show <package>                 Return complete metadata in JSON\n"
"  plan <action> [<pkgs>...]      SAT dependency solver transaction plan in JSON\n"
"  updates | check-upgrade        List upgradable packages in JSON\n"
"  list [installed|all] [pat]     List packages in JSON\n"
"  info                           Return system & repository config in JSON\n"
"  rpc                            Interactive JSON-RPC loop over stdin/stdout\n"
"  listen <socket_path>           Run UNIX domain socket API server\n",
        cmd_name);
}

int cmd_api(const char *argv0, const char *cmd_name, paths *p, const char *arch,
            const char **args, size_t nargs, int json_mode, int status_fd)
{
    (void)json_mode;
    if (nargs == 0) {
        if (!isatty(STDIN_FILENO)) {
            return api_rpc_loop(p, arch, stdin, stdout, argv0);
        }
        api_usage(cmd_name);
        return 0;
    }

    const char *sub = args[0];
    char *resp = NULL;

    if (strcmp(sub, "search") == 0 || strcmp(sub, "s") == 0) {
        if (nargs < 2) {
            d99_json_buf eb;
            d99_jb_init(&eb);
            json_error(&eb, "search requires a query string");
            resp = d99_jb_finish(&eb);
        } else {
            resp = api_search_json(p, args[1]);
        }
    } else if (strcmp(sub, "show") == 0) {
        if (nargs < 2) {
            d99_json_buf eb;
            d99_jb_init(&eb);
            json_error(&eb, "show requires a package name");
            resp = d99_jb_finish(&eb);
        } else {
            resp = api_show_json(p, args[1]);
        }
    } else if (strcmp(sub, "updates") == 0 || strcmp(sub, "check-upgrade") == 0) {
        resp = api_updates_json(p);
    } else if (strcmp(sub, "plan") == 0) {
        const char *act = (nargs > 1) ? args[1] : "install";
        const char **pkgs = (nargs > 2) ? &args[2] : NULL;
        size_t npkgs = (nargs > 2) ? nargs - 2 : 0;
        resp = api_plan_json(argv0, p, act, pkgs, npkgs);
    } else if (strcmp(sub, "list") == 0) {
        const char *type = (nargs > 1) ? args[1] : "installed";
        const char *pat = (nargs > 2) ? args[2] : NULL;
        resp = api_list_json(p, type, pat);
    } else if (strcmp(sub, "info") == 0) {
        resp = api_info_json(p, arch);
    } else if (strcmp(sub, "rpc") == 0) {
        return api_rpc_loop(p, arch, stdin, stdout, argv0);
    } else if (strcmp(sub, "listen") == 0 || strcmp(sub, "serve") == 0) {
        if (nargs < 2) {
            fprintf(stderr, "d99-solve: api listen requires a socket path\n");
            return 1;
        }
        return api_listen_socket(p, arch, args[1], argv0);
    } else {
        d99_json_buf eb;
        d99_jb_init(&eb);
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown api command '%s'", sub);
        json_error(&eb, msg);
        resp = d99_jb_finish(&eb);
    }

    if (resp) {
        if (status_fd >= 0) {
            dprintf(status_fd, "%s\n", resp);
        }
        fputs(resp, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(resp);
    }

    return 0;
}
