#include "catch_amalgamated.hpp"
#include "version_compare.hpp"   // real production semver helpers

TEST_CASE("Semver parsing", "[semver]") {
    int mj, mn, pt;

    SECTION("Standard version") {
        REQUIRE(parse_semver("1.2.3", &mj, &mn, &pt));
        REQUIRE(mj == 1);
        REQUIRE(mn == 2);
        REQUIRE(pt == 3);
    }

    SECTION("Version with v prefix") {
        REQUIRE(parse_semver("v1.2.3", &mj, &mn, &pt));
        REQUIRE(mj == 1);
        REQUIRE(mn == 2);
        REQUIRE(pt == 3);
    }

    SECTION("Version with V prefix") {
        REQUIRE(parse_semver("V2.0.1", &mj, &mn, &pt));
        REQUIRE(mj == 2);
        REQUIRE(mn == 0);
        REQUIRE(pt == 1);
    }

    SECTION("Version with spaces") {
        REQUIRE(parse_semver("  1.2.3", &mj, &mn, &pt));
        REQUIRE(mj == 1);
    }

    SECTION("Two-part version") {
        REQUIRE(parse_semver("1.2", &mj, &mn, &pt));
        REQUIRE(mj == 1);
        REQUIRE(mn == 2);
        REQUIRE(pt == 0);
    }

    SECTION("Single-part version") {
        REQUIRE(parse_semver("5", &mj, &mn, &pt));
        REQUIRE(mj == 5);
        REQUIRE(mn == 0);
        REQUIRE(pt == 0);
    }

    SECTION("Version with git suffix") {
        REQUIRE(parse_semver("1.2.3-5-gabc1234", &mj, &mn, &pt));
        REQUIRE(mj == 1);
        REQUIRE(mn == 2);
        REQUIRE(pt == 3);
    }

    SECTION("Invalid versions") {
        REQUIRE_FALSE(parse_semver(nullptr, &mj, &mn, &pt));
        REQUIRE_FALSE(parse_semver("", &mj, &mn, &pt));
        REQUIRE_FALSE(parse_semver("abc", &mj, &mn, &pt));
    }
}

TEST_CASE("Semver comparison", "[semver]") {
    SECTION("Equal versions") {
        REQUIRE(semver_cmp("1.2.3", "1.2.3") == 0);
    }

    SECTION("Major version difference") {
        REQUIRE(semver_cmp("2.0.0", "1.9.9") > 0);
        REQUIRE(semver_cmp("1.9.9", "2.0.0") < 0);
    }

    SECTION("Minor version difference") {
        REQUIRE(semver_cmp("1.3.0", "1.2.9") > 0);
        REQUIRE(semver_cmp("1.2.9", "1.3.0") < 0);
    }

    SECTION("Patch version difference") {
        REQUIRE(semver_cmp("1.2.4", "1.2.3") > 0);
        REQUIRE(semver_cmp("1.2.3", "1.2.4") < 0);
    }

    SECTION("Invalid versions") {
        REQUIRE(semver_cmp("invalid", "1.2.3") < 0);
        REQUIRE(semver_cmp("1.2.3", "invalid") > 0);
    }
}

TEST_CASE("Minor/major newer check", "[semver]") {
    SECTION("Major bump") {
        REQUIRE(is_minor_or_major_newer("2.0.0", "1.9.9"));
    }

    SECTION("Minor bump") {
        REQUIRE(is_minor_or_major_newer("1.3.0", "1.2.9"));
    }

    SECTION("Patch only - not newer") {
        REQUIRE_FALSE(is_minor_or_major_newer("1.2.4", "1.2.3"));
    }

    SECTION("Same version") {
        REQUIRE_FALSE(is_minor_or_major_newer("1.2.3", "1.2.3"));
    }

    SECTION("Older version") {
        REQUIRE_FALSE(is_minor_or_major_newer("1.2.3", "1.2.4"));
        REQUIRE_FALSE(is_minor_or_major_newer("1.2.3", "1.3.0"));
        REQUIRE_FALSE(is_minor_or_major_newer("1.2.3", "2.0.0"));
    }

    SECTION("Invalid versions") {
        REQUIRE_FALSE(is_minor_or_major_newer("invalid", "1.2.3"));
        REQUIRE_FALSE(is_minor_or_major_newer("1.2.3", "invalid"));
    }
}
