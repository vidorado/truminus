#pragma once

// Semantic-version helpers used by the OTA flow. Pure (no ESP-IDF deps) so they
// can be unit-tested on the host — see test/host/test_semver.cpp.

// Parse a leading [v]MAJOR.MINOR[.PATCH] from `s`. Returns false if no numeric
// major could be read. A trailing git-describe suffix ("-3-gabc1") is ignored.
bool parse_semver(const char* s, int* mj, int* mn, int* pt);

// >0 if `a` newer than `b`, 0 if equal, <0 if older. Unparseable is treated
// conservatively as "not newer".
int semver_cmp(const char* a, const char* b);

// True if `latest` is a newer MINOR or MAJOR than `running` (a patch-only bump
// returns false). Gates the proactive auto-notification so patch releases don't
// nag the user.
bool is_minor_or_major_newer(const char* latest, const char* running);
