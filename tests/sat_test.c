#include "d99_sat.h"
#include "d99_util.h"

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

/* tiny clause store for model checking */
typedef struct { int *lits; int n; } tclause;
static tclause store[65536];
static int nstore;

static void add(int *lits, int n)
{
    store[nstore].lits = d99_xmalloc((size_t)n * sizeof(int));
    memcpy(store[nstore].lits, lits, (size_t)n * sizeof(int));
    store[nstore].n = n;
    nstore++;
}

static int model_ok(d99_sat *s)
{
    int i, j;
    for (i = 0; i < nstore; i++) {
        int sat = 0;
        for (j = 0; j < store[i].n; j++) {
            int lit = store[i].lits[j];
            int v = d99_sat_value(s, lit >> 1);
            int val = (v < 0) ? 0 : v;
            if ((lit & 1) ? !val : val) {
                sat = 1;
                break;
            }
        }
        if (!sat)
            return 0;
    }
    return 1;
}

static void reset(void)
{
    int i;
    for (i = 0; i < nstore; i++)
        free(store[i].lits);
    nstore = 0;
}

int main(void)
{
    d99_sat *s;
    int a, b, c;
    int lits[8];

    /* 1. trivial satisfiability */
    reset();
    s = d99_sat_new();
    a = d99_sat_var(s);
    CHECK(d99_sat_solve(s, NULL, 0) == 1, "trivial SAT");
    CHECK(d99_sat_value(s, a) >= 0, "trivial value assigned");
    d99_sat_free(s);

    /* 2. (a v b) & (!a)  =>  b must be true */
    reset();
    s = d99_sat_new();
    a = d99_sat_var(s);
    b = d99_sat_var(s);
    lits[0] = D99_LIT_POS(a);
    lits[1] = D99_LIT_POS(b);
    add(lits, 2);
    d99_sat_clause(s, lits, 2);
    lits[0] = D99_LIT_NEG(a);
    add(lits, 1);
    d99_sat_clause(s, lits, 1);
    CHECK(d99_sat_solve(s, NULL, 0) == 1, "unit propagation SAT");
    CHECK(d99_sat_value(s, b) == 1, "b forced true");
    CHECK(model_ok(s), "model satisfies clauses");
    d99_sat_free(s);

    /* 3. (a) & (!a) => UNSAT */
    reset();
    s = d99_sat_new();
    a = d99_sat_var(s);
    lits[0] = D99_LIT_POS(a);
    d99_sat_clause(s, lits, 1);
    lits[0] = D99_LIT_NEG(a);
    d99_sat_clause(s, lits, 1);
    CHECK(d99_sat_solve(s, NULL, 0) == 0, "unit conflict UNSAT");
    d99_sat_free(s);

    /* 4. pigeonhole PHP(3,2): UNSAT */
    reset();
    {
        int x[3][2];
        int i, j, k;
        s = d99_sat_new();
        for (i = 0; i < 3; i++)
            for (j = 0; j < 2; j++)
                x[i][j] = d99_sat_var(s);
        /* ALO: each pigeon in some hole */
        for (i = 0; i < 3; i++) {
            lits[0] = D99_LIT_POS(x[i][0]);
            lits[1] = D99_LIT_POS(x[i][1]);
            add(lits, 2);
            d99_sat_alo(s, lits, 2);
        }
        /* AMO: at most one pigeon per hole */
        for (j = 0; j < 2; j++) {
            int vl[3];
            for (i = 0; i < 3; i++)
                vl[i] = D99_LIT_POS(x[i][j]);
            /* store implied amo clauses for model check */
            for (i = 0; i < 3; i++)
                for (k = i + 1; k < 3; k++) {
                    lits[0] = D99_LIT_NEG(x[i][j]);
                    lits[1] = D99_LIT_NEG(x[k][j]);
                    add(lits, 2);
                }
            d99_sat_amo(s, vl, 3);
        }
        CHECK(d99_sat_solve(s, NULL, 0) == 0, "PHP(3,2) UNSAT");
        d99_sat_free(s);
    }

    /* 5. implications with assumptions */
    reset();
    s = d99_sat_new();
    a = d99_sat_var(s);
    b = d99_sat_var(s);
    c = d99_sat_var(s);
    {
        int pair[2];
        pair[0] = D99_LIT_POS(a);
        pair[1] = D99_LIT_POS(b);
        add(pair, 2);
        d99_sat_imply(s, D99_LIT_POS(a), D99_LIT_POS(b));
        pair[0] = D99_LIT_POS(b);
        pair[1] = D99_LIT_POS(c);
        add(pair, 2);
        d99_sat_imply(s, D99_LIT_POS(b), D99_LIT_POS(c));
    }
    {
        int assume[1];
        assume[0] = D99_LIT_POS(a);
        CHECK(d99_sat_solve(s, assume, 1) == 1, "chain SAT under a");
        CHECK(d99_sat_value(s, b) == 1, "a -> b");
        CHECK(d99_sat_value(s, c) == 1, "b -> c");
        CHECK(model_ok(s), "chain model ok");
        /* a and !b together: UNSAT */
        assume[0] = D99_LIT_POS(a);
        {
            int assume2[2];
            assume2[0] = D99_LIT_POS(a);
            assume2[1] = D99_LIT_NEG(b);
            CHECK(d99_sat_solve(s, assume2, 2) == 0, "chain UNSAT under a,!b");
        }
    }
    d99_sat_free(s);

    /* 6. harder: random-ish 3-SAT loop known SAT */
    reset();
    s = d99_sat_new();
    {
        int v[12];
        unsigned seed = 42;
        int i;
        for (i = 0; i < 12; i++)
            v[i] = d99_sat_var(s);
        /* deterministic pseudo-random clauses with a known solution:
         * force v[i] = true for even i, false for odd i, then add
         * clauses consistent with that assignment plus a few units */
        for (i = 0; i < 12; i += 2) {
            int lit = D99_LIT_POS(v[i]);
            d99_sat_clause(s, &lit, 1);
        }
        for (i = 0; i < 40; i++) {
            int x1, x2, x3;
            int cl[3];
            seed = seed * 1103515245 + 12345;
            x1 = (seed >> 16) % 12;
            seed = seed * 1103515245 + 12345;
            x2 = (seed >> 16) % 12;
            seed = seed * 1103515245 + 12345;
            x3 = (seed >> 16) % 12;
            /* build clause that is satisfied by v[even]=true */
            cl[0] = (x1 % 2 == 0) ? D99_LIT_POS(v[x1]) : D99_LIT_NEG(v[x1]);
            cl[1] = (x2 % 2 == 0) ? D99_LIT_POS(v[x2]) : D99_LIT_NEG(v[x2]);
            cl[2] = (x3 % 2 == 0) ? D99_LIT_POS(v[x3]) : D99_LIT_NEG(v[x3]);
            add(cl, 3);
            d99_sat_clause(s, cl, 3);
        }
        CHECK(d99_sat_solve(s, NULL, 0) == 1, "constructed 3-SAT satisfiable");
        CHECK(model_ok(s), "3-SAT model ok");
    }
    d99_sat_free(s);

    /* 7. larger stress: 240 vars, satisfiable by construction; forces
     * conflicts, 1-UIP learning, VSIDS and restarts. */
    reset();
    s = d99_sat_new();
    {
        enum { NV = 240, NC = 2400 };
        int v[NV];
        unsigned seed = 12345;
        int i, j;
        for (i = 0; i < NV; i++)
            v[i] = d99_sat_var(s);
        for (i = 0; i < NC; i++) {
            int cl[3];
            for (j = 0; j < 3; j++) {
                seed = seed * 1103515245u + 12345u;
                {
                    int x = (int)((seed >> 17) % (unsigned)NV);
                    /* satisfied by v[i] = true for even i, false for odd */
                    cl[j] = (x % 2 == 0) ? D99_LIT_POS(v[x]) : D99_LIT_NEG(v[x]);
                }
            }
            add(cl, 3);
            d99_sat_clause(s, cl, 3);
        }
        CHECK(d99_sat_solve(s, NULL, 0) == 1, "stress 3-SAT satisfiable");
        CHECK(model_ok(s), "stress model ok");
    }
    d99_sat_free(s);

    reset();
    if (g_fail) {
        fprintf(stderr, "sat_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("sat_test: all tests passed\n");
    return 0;
}
