#include "d99_sat.h"
#include "d99_util.h"

#include <stdlib.h>
#include <string.h>

typedef struct clause {
    int *lits;
    int n;
    int learnt;
    int removed;
    int locked;   /* currently the reason of a trail variable */
    double act;
} clause;

typedef struct {
    clause **v;
    int n, cap;
} wvec;

struct d99_sat {
    int nvars, varcap;
    signed char *val;        /* per var: 0 undef, 1 true, -1 false */
    signed char *phase;      /* saved polarity, default -1 (prefer false) */
    int *level;
    clause **reason;
    int *trail;
    int ntrail, trailcap;
    int *lim;                /* lim[l] = trail index where level l starts */
    int nlim;
    int limcap;
    int qhead;
    wvec *w;                 /* watches, indexed by literal */
    int wcap;
    double *act;             /* VSIDS activity per var */
    double act_inc;
    int *heap;               /* max-heap of vars by activity */
    int heapn, heapcap;
    int *heap_pos;
    clause **cls;            /* all clauses; [0,nprob) are problem clauses */
    int ncls, clscap, nprob;
    int nalive_learnts;
    int ok;
    int *seen;
    int *clearv;             /* analysis scratch: vars to un-see */
    int nclear;
    int *learnbuf;
    int learncap;
    long long conflicts;
    int restart_count;
    int conflicts_at_restart;
    int learnt_limit;
};

/* ---------------- heap ---------------- */
static void heap_swap(d99_sat *s, int i, int j)
{
    int a = s->heap[i], b = s->heap[j];
    s->heap[i] = b;
    s->heap[j] = a;
    s->heap_pos[a] = j;
    s->heap_pos[b] = i;
}

static void heap_up(d99_sat *s, int i)
{
    while (i > 0) {
        int par = (i - 1) >> 1;
        if (s->act[s->heap[i]] <= s->act[s->heap[par]])
            break;
        heap_swap(s, i, par);
        i = par;
    }
}

static void heap_down(d99_sat *s, int i)
{
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, top = i;
        if (l < s->heapn && s->act[s->heap[l]] > s->act[s->heap[top]])
            top = l;
        if (r < s->heapn && s->act[s->heap[r]] > s->act[s->heap[top]])
            top = r;
        if (top == i)
            break;
        heap_swap(s, i, top);
        i = top;
    }
}

static void heap_insert(d99_sat *s, int v)
{
    if (s->heap_pos[v] >= 0)
        return;
    if (s->heapn == s->heapcap) {
        s->heapcap = s->heapcap ? s->heapcap * 2 : 256;
        s->heap = d99_xrealloc(s->heap, (size_t)s->heapcap * sizeof(int));
    }
    s->heap[s->heapn] = v;
    s->heap_pos[v] = s->heapn;
    s->heapn++;
    heap_up(s, s->heapn - 1);
}

static int heap_pop(d99_sat *s)
{
    int top;
    if (!s->heapn)
        return -1;
    top = s->heap[0];
    s->heap_pos[top] = -1;
    s->heapn--;
    if (s->heapn) {
        s->heap[0] = s->heap[s->heapn];
        s->heap_pos[s->heap[0]] = 0;
        heap_down(s, 0);
    }
    return top;
}

static void var_bump(d99_sat *s, int v)
{
    s->act[v] += s->act_inc;
    if (s->act[v] > 1e100) {
        int i;
        for (i = 0; i < s->nvars; i++)
            s->act[i] *= 1e-100;
        s->act_inc *= 1e-100;
    }
    if (s->heap_pos[v] >= 0)
        heap_up(s, s->heap_pos[v]);
}

/* ---------------- watches ---------------- */
static void wv_push(wvec *ws, clause *c)
{
    if (ws->n == ws->cap) {
        ws->cap = ws->cap ? ws->cap * 2 : 4;
        ws->v = d99_xrealloc(ws->v, (size_t)ws->cap * sizeof(clause *));
    }
    ws->v[ws->n++] = c;
}

/* ---------------- trail ---------------- */
static void enqueue(d99_sat *s, int lit, clause *reason)
{
    int var = lit >> 1;
    s->val[var] = (lit & 1) ? -1 : 1;
    s->level[var] = s->nlim;
    s->reason[var] = reason;
    if (s->ntrail == s->trailcap) {
        s->trailcap = s->trailcap ? s->trailcap * 2 : 1024;
        s->trail = d99_xrealloc(s->trail, (size_t)s->trailcap * sizeof(int));
    }
    s->trail[s->ntrail++] = lit;
}

static int lit_val(const d99_sat *s, int lit)
{
    signed char v = s->val[lit >> 1];
    if (v == 0)
        return 0;
    return (lit & 1) ? -v : v;
}

static void new_level(d99_sat *s)
{
    if (s->nlim + 2 >= s->limcap) {
        s->limcap = s->limcap ? s->limcap * 2 : s->nvars + 16;
        s->lim = d99_xrealloc(s->lim, (size_t)s->limcap * sizeof(int));
    }
    s->lim[s->nlim++] = s->ntrail;
}

static void cancel_until(d99_sat *s, int lvl)
{
    int i;
    if (s->nlim <= lvl)
        return;
    for (i = s->ntrail - 1; i >= s->lim[lvl]; i--) {
        int v = s->trail[i] >> 1;
        s->phase[v] = s->val[v];
        s->val[v] = 0;
        s->reason[v] = NULL;
        heap_insert(s, v);
    }
    s->ntrail = s->lim[lvl];
    s->nlim = lvl;
    s->qhead = s->ntrail;
}

/* ---------------- propagate ---------------- */
static clause *propagate(d99_sat *s)
{
    while (s->qhead < s->ntrail) {
        int p = s->trail[s->qhead++];
        int false_lit = p ^ 1;
        wvec *ws = &s->w[false_lit];
        clause **i = ws->v, **j = ws->v, **end = ws->v + ws->n;
        clause *confl = NULL;

        while (i != end) {
            clause *c = *i++;
            int first;

            if (c->lits[0] == false_lit) {
                c->lits[0] = c->lits[1];
                c->lits[1] = false_lit;
            }
            first = c->lits[0];
            if (lit_val(s, first) == 1) {
                *j++ = c;
                continue;
            }
            {
                int k, found = 0;
                for (k = 2; k < c->n; k++) {
                    if (lit_val(s, c->lits[k]) != -1) {
                        int lk = c->lits[k];
                        c->lits[1] = lk;
                        c->lits[k] = false_lit;
                        wv_push(&s->w[lk], c);
                        found = 1;
                        break;
                    }
                }
                if (found)
                    continue;
            }
            /* unit or conflict */
            *j++ = c;
            if (lit_val(s, first) == -1) {
                confl = c;
                while (i != end)
                    *j++ = *i++;
                ws->n = (int)(j - ws->v);
                s->qhead = s->ntrail;
                return confl;
            }
            enqueue(s, first, c);
        }
        ws->n = (int)(j - ws->v);
    }
    return NULL;
}

/* ---------------- analyze (1-UIP) ---------------- */
static int analyze(d99_sat *s, clause *confl, int *out_n, int *out_bt)
{
    int nlearned = 0, pathC = 0, p = -1;
    int idx = s->ntrail - 1;
    int i;

    s->nclear = 0;
    s->learnbuf[nlearned++] = 0;   /* UIP slot */

    do {
        clause *c = (p < 0) ? confl : s->reason[p];
        int start = (p < 0) ? 0 : 1;
        for (i = start; i < c->n; i++) {
            int q = c->lits[i];
            int v = q >> 1;
            if (s->seen[v] || s->val[v] == 0)
                continue;
            var_bump(s, v);
            s->seen[v] = 1;
            s->clearv[s->nclear++] = v;
            if (s->level[v] >= s->nlim)
                pathC++;
            else
                s->learnbuf[nlearned++] = q ^ 1;
        }
        while (!s->seen[s->trail[idx] >> 1])
            idx--;
        {
            int lit = s->trail[idx];
            p = lit >> 1;
            s->learnbuf[0] = lit ^ 1;
            idx--;
        }
        pathC--;
    } while (pathC > 0);

    /* clear seen flags */
    for (i = 0; i < s->nclear; i++)
        s->seen[s->clearv[i]] = 0;

    /* backjump level = max level among the non-UIP literals */
    {
        int bt = 0, maxi = 1;
        for (i = 1; i < nlearned; i++)
            if (s->level[s->learnbuf[i] >> 1] > bt) {
                bt = s->level[s->learnbuf[i] >> 1];
                maxi = i;
            }
        if (nlearned > 2) {
            int t = s->learnbuf[1];
            s->learnbuf[1] = s->learnbuf[maxi];
            s->learnbuf[maxi] = t;
        }
        *out_bt = bt;
    }
    *out_n = nlearned;
    return 0;
}

/* ---------------- clause db ---------------- */
static clause *clause_new(const int *lits, int n, int learnt)
{
    clause *c = d99_xmalloc(sizeof(*c));
    c->lits = d99_xmalloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (n)
        memcpy(c->lits, lits, (size_t)n * sizeof(int));
    c->n = n;
    c->learnt = learnt;
    c->removed = 0;
    c->locked = 0;
    c->act = 0.0;
    return c;
}

static int cmp_clause_act(const void *a, const void *b)
{
    const clause *ca = *(clause *const *)a, *cb = *(clause *const *)b;
    if (ca->act < cb->act)
        return -1;
    if (ca->act > cb->act)
        return 1;
    return 0;
}

static void reduce_db(d99_sat *s)
{
    clause **learnt;
    int nlearnt = 0, i, k;

    /* mark clauses that are currently justifying trail assignments */
    for (i = 0; i < s->ntrail; i++) {
        clause *r = s->reason[s->trail[i] >> 1];
        if (r)
            r->locked = 1;
    }
    learnt = d99_xmalloc((size_t)(s->ncls - s->nprob + 1) * sizeof(clause *));
    for (i = s->nprob; i < s->ncls; i++) {
        clause *c = s->cls[i];
        if (c->removed || c->locked || c->n == 2)
            continue;
        learnt[nlearnt++] = c;
    }
    qsort(learnt, (size_t)nlearnt, sizeof(clause *), cmp_clause_act);
    {
        int toremove = nlearnt / 2;
        for (k = 0; k < toremove; k++) {
            learnt[k]->removed = 1;
            s->nalive_learnts--;
        }
    }
    /* rebuild watch lists without removed clauses */
    for (k = 0; k < s->wcap; k++) {
        wvec *ws = &s->w[k];
        int nn = 0;
        for (i = 0; i < ws->n; i++)
            if (!ws->v[i]->removed)
                ws->v[nn++] = ws->v[i];
        ws->n = nn;
    }
    for (i = s->nprob; i < s->ncls; i++)
        s->cls[i]->locked = 0;
    free(learnt);
    s->learnt_limit = s->learnt_limit + s->learnt_limit / 8 + 1024;
}

/* integer Luby restart sequence */
static unsigned luby2(unsigned x)
{
    unsigned size = 1, seq = 0;
    while (size < x + 1) {
        seq++;
        size = 2 * size + 1;
    }
    while (size - 1 != x) {
        size = (size - 1) >> 1;
        seq--;
        x = x % size;
    }
    return 1u << seq;
}

/* ---------------- public API ---------------- */
d99_sat *d99_sat_new(void)
{
    d99_sat *s = d99_xcalloc(1, sizeof(*s));
    s->ok = 1;
    s->act_inc = 1.0;
    s->learnt_limit = 2048;
    s->limcap = 16;
    s->lim = d99_xmalloc((size_t)s->limcap * sizeof(int));
    s->lim[0] = 0;
    s->nlim = 0;
    return s;
}

void d99_sat_free(d99_sat *s)
{
    int i;
    if (!s)
        return;
    for (i = 0; i < s->ncls; i++) {
        free(s->cls[i]->lits);
        free(s->cls[i]);
    }
    free(s->cls);
    for (i = 0; i < s->wcap; i++)
        free(s->w[i].v);
    free(s->w);
    free(s->val);
    free(s->phase);
    free(s->level);
    free(s->reason);
    free(s->trail);
    free(s->lim);
    free(s->act);
    free(s->heap);
    free(s->heap_pos);
    free(s->seen);
    free(s->clearv);
    free(s->learnbuf);
    free(s);
}

int d99_sat_var(d99_sat *s)
{
    int v = s->nvars;

    if (s->nvars == s->varcap) {
        s->varcap = s->varcap ? s->varcap * 2 : 64;
        s->val = d99_xrealloc(s->val, (size_t)s->varcap);
        s->phase = d99_xrealloc(s->phase, (size_t)s->varcap);
        s->level = d99_xrealloc(s->level, (size_t)s->varcap * sizeof(int));
        s->reason = d99_xrealloc(s->reason, (size_t)s->varcap * sizeof(clause *));
        s->act = d99_xrealloc(s->act, (size_t)s->varcap * sizeof(double));
        s->heap_pos = d99_xrealloc(s->heap_pos, (size_t)s->varcap * sizeof(int));
        s->seen = d99_xrealloc(s->seen, (size_t)s->varcap * sizeof(int));
        s->clearv = d99_xrealloc(s->clearv, (size_t)s->varcap * sizeof(int));
        s->learncap = s->varcap + 2;
        s->learnbuf = d99_xrealloc(s->learnbuf, (size_t)s->learncap * sizeof(int));
        /* grow watch table */
        if (2 * s->varcap > s->wcap) {
            int old = s->wcap;
            int i;
            s->wcap = 2 * s->varcap;
            s->w = d99_xrealloc(s->w, (size_t)s->wcap * sizeof(wvec));
            for (i = old; i < s->wcap; i++) {
                s->w[i].v = NULL;
                s->w[i].n = s->w[i].cap = 0;
            }
        }
    }
    s->val[v] = 0;
    s->phase[v] = -1;
    s->level[v] = 0;
    s->reason[v] = NULL;
    s->act[v] = 0.0;
    s->heap_pos[v] = -1;
    s->seen[v] = 0;
    s->nvars++;
    heap_insert(s, v);
    if (s->nlim == 0 && s->ntrail == 0)
        s->lim[0] = 0;
    return v;
}

int d99_sat_clause(d99_sat *s, const int *lits, int n)
{
    clause *c;
    int i, j;

    if (!s->ok)
        return -1;
    /* dedupe + tautology check */
    {
        int clean[64] = {0};
        int nn = 0;
        if (n > 64) {
            /* large clauses: allocate */
            int *big = d99_xmalloc((size_t)n * sizeof(int));
            memcpy(big, lits, (size_t)n * sizeof(int));
            for (i = 0; i < n; i++) {
                int dup = 0;
                for (j = 0; j < nn; j++)
                    if (big[j] == lits[i])
                        dup = 1;
                if (!dup)
                    big[nn++] = lits[i];
            }
            for (i = 0; i < nn; i++)
                for (j = 0; j < nn; j++)
                    if (i != j && big[i] == (big[j] ^ 1)) {
                        free(big);
                        return 0;   /* tautology */
                    }
            c = clause_new(big, nn, 0);
            free(big);
        } else {
            for (i = 0; i < n; i++) {
                int dup = 0;
                for (j = 0; j < nn; j++)
                    if (clean[j] == lits[i])
                        dup = 1;
                if (!dup)
                    clean[nn++] = lits[i];
            }
            for (i = 0; i < nn; i++)
                for (j = 0; j < nn; j++)
                    if (i != j && clean[i] == (clean[j] ^ 1))
                        return 0;   /* tautology */
            c = clause_new(clean, nn, 0);
        }
        if (nn == 0) {
            s->ok = 0;
            free(c->lits);
            free(c);
            return -1;
        }
        if (nn == 1) {
            int lit = c->lits[0];
            free(c->lits);
            free(c);
            if (s->val[lit >> 1] == 0)
                enqueue(s, lit, NULL);
            else if (lit_val(s, lit) == -1)
                s->ok = 0;
            return 0;
        }
    }
    if (s->ncls == s->clscap) {
        s->clscap = s->clscap ? s->clscap * 2 : 256;
        s->cls = d99_xrealloc(s->cls, (size_t)s->clscap * sizeof(clause *));
    }
    s->cls[s->ncls++] = c;
    s->nprob++;
    wv_push(&s->w[c->lits[0]], c);
    wv_push(&s->w[c->lits[1]], c);
    return 0;
}

int d99_sat_solve(d99_sat *s, const int *assume, int n)
{
    int i;

    if (!s->ok)
        return 0;
    cancel_until(s, 0);

    for (i = 0; i < n; i++) {
        int lit = assume[i];
        int v = lit >> 1;
        if (s->val[v] != 0) {
            if (lit_val(s, lit) == 1)
                continue;
            cancel_until(s, 0);
            return 0;
        }
        new_level(s);
        enqueue(s, lit, NULL);
        if (propagate(s)) {
            cancel_until(s, 0);
            return 0;
        }
    }

    for (;;) {
        clause *confl = propagate(s);
        if (confl) {
            s->conflicts++;
            if (s->nlim == 0) {
                s->ok = 0;
                cancel_until(s, 0);
                return 0;
            }
            {
                int nl, bt;
                analyze(s, confl, &nl, &bt);
                if (n > 0 && bt < n) {
                    /* would need to undo assumptions */
                    cancel_until(s, 0);
                    return 0;
                }
                cancel_until(s, bt);
                if (nl == 1) {
                    enqueue(s, s->learnbuf[0], NULL);
                } else {
                    clause *lc = clause_new(s->learnbuf, nl, 1);
                    if (s->ncls == s->clscap) {
                        s->clscap = s->clscap ? s->clscap * 2 : 256;
                        s->cls = d99_xrealloc(s->cls, (size_t)s->clscap * sizeof(clause *));
                    }
                    s->cls[s->ncls++] = lc;
                    s->nalive_learnts++;
                    wv_push(&s->w[lc->lits[0]], lc);
                    wv_push(&s->w[lc->lits[1]], lc);
                    enqueue(s, lc->lits[0], lc);
                    s->act_inc *= 1.0 / 0.95;
                }
            }
            if (s->nalive_learnts >= s->learnt_limit)
                reduce_db(s);
            if (s->conflicts - s->conflicts_at_restart >=
                100LL * (long long)luby2((unsigned)s->restart_count + 1)) {
                s->restart_count++;
                s->conflicts_at_restart = (int)s->conflicts;
                cancel_until(s, n);
            }
        } else {
            int v = -1;
            while (s->heapn) {
                int cand = heap_pop(s);
                if (s->val[cand] == 0) {
                    v = cand;
                    break;
                }
            }
            if (v < 0)
                return 1;   /* all variables assigned: SAT */
            new_level(s);
            enqueue(s, s->phase[v] >= 0 ? D99_LIT_POS(v) : D99_LIT_NEG(v), NULL);
        }
    }
}

int d99_sat_value(d99_sat *s, int var)
{
    if (var < 0 || var >= s->nvars || s->val[var] == 0)
        return -1;
    return s->val[var] > 0 ? 1 : 0;
}
