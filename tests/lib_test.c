#include "d99_util.h"
#include "d99_archive.h"
#include "d99_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            g_fail++; \
        } \
    } while (0)

static void test_vercmp(void)
{
    struct { const char *a, *b; int expect; } cases[] = {
        { "1.0-1",   "1.0-1",   0 },
        { "1.0-1",   "1.0-2",  -1 },
        { "1.0-2",   "1.0-1",   1 },
        { "1.1",     "1.0",     1 },
        { "1.0~rc1", "1.0",    -1 },
        { "2:1.0",   "1:2.0",   1 },
        { "1.0-1",   "1.0",     1 },
        { "1.02",    "1.1",     1 },   /* numeric: 2 > 1 */
        { "1.0a",    "1.0",     1 },
        { "1.0~",    "1.0",    -1 },
        { "1.0-1-1", "1.0-1",   1 },
        { "0",       "0",       0 },
        { "1:0",     "0.9",     1 },
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int r = d99_vercmp(cases[i].a, cases[i].b);
        CHECK((r < 0) == (cases[i].expect < 0) && (r > 0) == (cases[i].expect > 0),
              "vercmp");
    }
}

static void test_deps(void)
{
    d99_arena *ar = d99_arena_new();
    d99_deplist dl;

    CHECK(d99_deplist_parse("libc6 (>= 2.14) [amd64], libbar | libbaz", &dl, ar) == 0,
          "deplist parse");
    CHECK(dl.n == 2, "two groups");
    CHECK(dl.g[0].n == 1 && strcmp(dl.g[0].alts[0].name, "libc6") == 0, "alt name");
    CHECK(dl.g[0].alts[0].op == D99_DEP_GE && strcmp(dl.g[0].alts[0].ver, "2.14") == 0,
          "alt relation");
    CHECK(dl.g[0].alts[0].arch && strcmp(dl.g[0].alts[0].arch, "amd64") == 0,
          "alt arch");
    CHECK(dl.g[1].n == 2, "alternatives");
    CHECK(strcmp(dl.g[1].alts[1].name, "libbaz") == 0, "alt pipe");
    d99_deplist_reset(&dl);

    CHECK(d99_depop_parse("<<") == D99_DEP_LT, "depop <<");
    CHECK(d99_depop_parse(">>") == D99_DEP_GT, "depop >>");
    CHECK(d99_verrel(d99_vercmp("2.15", "2.14"), D99_DEP_GE) == 1, "verrel ge");
    CHECK(d99_verrel(d99_vercmp("2.13", "2.14"), D99_DEP_GE) == 0, "verrel ge neg");

    CHECK(d99_arch_ok(NULL, "amd64") == 1, "arch empty ok");
    CHECK(d99_arch_ok("amd64", "amd64") == 1, "arch match");
    CHECK(d99_arch_ok("i386", "amd64") == 0, "arch mismatch");
    CHECK(d99_arch_ok("!i386,armhf", "amd64") == 1, "arch negation ok");
    CHECK(d99_arch_ok("!amd64", "amd64") == 0, "arch negation hit");

    d99_arena_free(ar);
}

static void test_stanza(void)
{
    d99_arena *ar = d99_arena_new();
    const char *text =
        "Package: hello\n"
        "Status: install ok installed\n"
        "Priority: optional\n"
        "Section: utils\n"
        "Maintainer: Foggy <f@example.org>\n"
        "Architecture: amd64\n"
        "Version: 1.0-1\n"
        "Depends: libc6 (>= 2.14)\n"
        "Description: friendly greeter\n"
        " Long description line one.\n"
        " Line two.\n";
    d99_pkg *p = d99_pkg_parse_stanza(text, strlen(text), ar);

    CHECK(p != NULL, "stanza parse");
    CHECK(strcmp(p->name, "hello") == 0, "pkg name");
    CHECK(strcmp(p->version, "1.0-1") == 0, "pkg version");
    CHECK(p->sel == D99_SEL_INSTALL && p->state == D99_PS_INSTALLED,
          "status words");
    CHECK(strcmp(p->summary, "friendly greeter") == 0, "summary");
    CHECK(p->description && strcmp(p->description,
          "Long description line one.\nLine two.") == 0, "description");
    CHECK(p->depends.n == 1 && p->depends.g[0].alts[0].op == D99_DEP_GE,
          "depends parsed");
    {
        char *v = d99_pkg_field_dup(p, "Maintainer");
        CHECK(v && strcmp(v, "Foggy <f@example.org>") == 0, "field dup");
        free(v);
        v = d99_pkg_field_dup(p, "Nonexistent");
        CHECK(v == NULL, "missing field");
        free(v);
    }
    d99_arena_free(ar);
}

static void test_digests(void)
{
    char hex[65];
    d99_sha256_hex("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256 abc");
    {
        char md[33];
        d99_md5_hex("abc", 3, md);
        CHECK(strcmp(md, "900150983cd24fb0d6963f7d28e17f72") == 0, "md5 abc");
    }
    CHECK(d99_fnv1a64_str("foobar") == 0x85944171f73967e8ULL, "fnv1a64");
}

static void test_glob(void)
{
    CHECK(d99_glob_match("he*o", "hello") == 1, "glob star");
    CHECK(d99_glob_match("h?llo", "hello") == 1, "glob q");
    CHECK(d99_glob_match("h?llo", "hallo") == 1, "glob q2");
    CHECK(d99_glob_match("hel*", "hel") == 1, "glob empty star");
    CHECK(d99_glob_match("*x", "hello") == 0, "glob miss");
    CHECK(d99_glob_match("*d99*", "the d99 tool") == 1, "glob mid");
}

static void rm_rf(const char *path)
{
    /* test-only cleanup; ignore result */
    char *cmd = d99_xasprintf("rm -rf '%s' 2>/dev/null", path);
    if (system(cmd) == -1) { /* nothing to do */ }
    free(cmd);
}

static void test_status_db(void)
{
    const char *dir = "/tmp/opencode/d99-libtest-adm";
    char *status;
    d99_db *db;
    d99_pkg *p;

    rm_rf(dir);
    CHECK(d99_mkdir_p(d99_path_join(dir, "info"), 0755) == 0, "mkdir adm");
    status = d99_path_join(dir, "status");
    {
        const char *text =
            "Package: one\nStatus: install ok installed\nVersion: 1.0\n"
            "Architecture: amd64\nDescription: first\n\n"
            "Package: two\nStatus: deinstall ok config-files\nVersion: 2.0\n"
            "Architecture: all\nDescription: second\n\n";
        CHECK(d99_write_file_atomic(status, text, strlen(text)) == 0, "write status");
    }
    db = d99_db_load_status(dir);
    CHECK(db != NULL, "load status");
    CHECK(d99_db_count(db) == 2, "count 2");
    p = d99_db_find(db, "one");
    CHECK(p && p->state == D99_PS_INSTALLED, "find one");
    CHECK(d99_pkg_set_status(db, p, D99_SEL_INSTALL, D99_PS_UNPACKED, D99_PF_OK) == 0,
          "set status");
    CHECK(d99_pkg_set_field(db, p, "Config-Version", "0.9") == 0, "set field");
    CHECK(d99_db_save_status(db, dir) == 0, "save status");
    d99_db_free(db);

    db = d99_db_load_status(dir);
    p = d99_db_find(db, "one");
    CHECK(p && p->state == D99_PS_UNPACKED, "reload state");
    {
        char *cv = d99_pkg_field_dup(p, "Config-Version");
        CHECK(cv && strcmp(cv, "0.9") == 0, "reload field");
        free(cv);
    }
    CHECK(d99_pkg_remove_field(db, p, "Config-Version") == 0, "remove field");
    CHECK(d99_db_save_status(db, dir) == 0, "save 2");
    d99_db_free(db);
    db = d99_db_load_status(dir);
    p = d99_db_find(db, "one");
    {
        char *cv = d99_pkg_field_dup(p, "Config-Version");
        CHECK(cv == NULL, "field removed");
        free(cv);
    }
    d99_db_free(db);
    free(status);
    rm_rf(dir);
}

static void test_index(void)
{
    const char *dir = "/tmp/opencode/d99-libtest-adm";
    char *idxpath;
    d99_index *ix;
    size_t i;
    size_t pkg = (size_t)-1, file_i = (size_t)-1;

    rm_rf(dir);
    CHECK(d99_mkdir_p(d99_path_join(dir, "info"), 0755) == 0, "idx: mkdir");
    {
        char *st = d99_path_join(dir, "status");
        char *listf = d99_path_join(dir, "info/one.list");
        const char *text =
            "Package: one\nStatus: install ok installed\nVersion: 1.0\n"
            "Architecture: amd64\nDescription: first\n\n";
        const char *listtext = "/usr/bin/one\n/usr/share/doc/one/README\n";
        CHECK(d99_write_file_atomic(st, text, strlen(text)) == 0, "idx: write status");
        CHECK(d99_write_file_atomic(listf, listtext, strlen(listtext)) == 0,
              "idx: write list");
        free(st);
        free(listf);
    }
    idxpath = d99_index_path_for(dir);
    CHECK(d99_index_build(dir, idxpath) == 0, "index build");

    ix = d99_index_open(idxpath, d99_path_join(dir, "status"));
    CHECK(ix != NULL, "index open");
    CHECK(d99_index_count(ix) == 1, "index count");
    CHECK(d99_index_find_pkg(ix, "one", &i) == 1, "index find pkg");
    CHECK(d99_index_find_file(ix, "usr/bin/one", &pkg, &file_i) == 1,
          "index find file");
    CHECK(pkg == 0, "file owner");
    CHECK(d99_index_find_file(ix, "usr/nope", &pkg, &file_i) == 0, "file miss");
    CHECK(d99_index_pkg_nfiles(ix, 0) == 2, "file count");
    CHECK(strcmp(d99_index_pkg_file(ix, 0, 1), "usr/share/doc/one/README") == 0,
          "file entry");
    d99_index_close(ix);
    free(idxpath);
    rm_rf(dir);
}

int main(void)
{
    test_vercmp();
    test_deps();
    test_stanza();
    test_digests();
    test_glob();
    test_status_db();
    test_index();
    if (g_fail) {
        fprintf(stderr, "lib_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("lib_test: all tests passed\n");
    return 0;
}
