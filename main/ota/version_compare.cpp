#include "version_compare.hpp"
#include <cstdio>

bool parse_semver(const char* s, int* mj, int* mn, int* pt) {
    if (!s) return false;
    while (*s == 'v' || *s == 'V' || *s == ' ') s++;
    int a = 0, b = 0, c = 0;
    int n = sscanf(s, "%d.%d.%d", &a, &b, &c);
    if (n < 1) return false;
    *mj = a; *mn = b; *pt = c;
    return true;
}

int semver_cmp(const char* a, const char* b) {
    int am, an, ap, bm, bn, bp;
    if (!parse_semver(a, &am, &an, &ap)) return -1;
    if (!parse_semver(b, &bm, &bn, &bp)) return 1;
    if (am != bm) return am - bm;
    if (an != bn) return an - bn;
    return ap - bp;
}

bool is_minor_or_major_newer(const char* latest, const char* running) {
    int lm, ln, lp, rm, rn, rp;
    if (!parse_semver(latest, &lm, &ln, &lp)) return false;
    if (!parse_semver(running, &rm, &rn, &rp)) return false;
    if (lm != rm) return lm > rm;   // major bump
    return ln > rn;                 // same major: only a minor increase counts
}
