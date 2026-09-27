#include "d99_sat.h"

#include <stdlib.h>

void d99_sat_imply(d99_sat *s, int from_lit, int to_lit)
{
    int lits[2];
    lits[0] = from_lit ^ 1;   /* ¬from */
    lits[1] = to_lit;
    d99_sat_clause(s, lits, 2);
}

void d99_sat_alo(d99_sat *s, const int *lits, int n)
{
    d99_sat_clause(s, lits, n);
}

void d99_sat_amo(d99_sat *s, const int *lits, int n)
{
    int i, j;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            int pair[2];
            pair[0] = lits[i] ^ 1;
            pair[1] = lits[j] ^ 1;
            d99_sat_clause(s, pair, 2);
        }
}
