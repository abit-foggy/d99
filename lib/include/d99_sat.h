#ifndef D99_SAT_H
#define D99_SAT_H

typedef struct d99_sat d99_sat;

#define D99_LIT_POS(v) (2 * (v))
#define D99_LIT_NEG(v) (2 * (v) + 1)

d99_sat *d99_sat_new(void);
void     d99_sat_free(d99_sat *s);

/* Create a variable; returns its 0-based id. */
int  d99_sat_var(d99_sat *s);

/* Add a problem clause.  Returns 0, or -1 if the empty clause was
 * derivable (solver is then permanently UNSAT).  All clauses must be
 * added before the first solve. */
int  d99_sat_clause(d99_sat *s, const int *lits, int n);

/* Solve under assumptions.  Returns 1 (SAT) or 0 (UNSAT). */
int  d99_sat_solve(d99_sat *s, const int *assume, int n);

/* Model value after a SAT result: 1 true, 0 false, -1 unassigned. */
int  d99_sat_value(d99_sat *s, int var);

/* ---- CNF construction helpers (sat_cnf.c) ---- */
/* from -> to  as  (¬from ∨ to) */
void d99_sat_imply(d99_sat *s, int from_lit, int to_lit);
void d99_sat_alo(d99_sat *s, const int *lits, int n);   /* at least one */
void d99_sat_amo(d99_sat *s, const int *lits, int n);   /* at most one (pairwise) */

#endif /* D99_SAT_H */
