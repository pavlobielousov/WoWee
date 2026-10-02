// Byte-count helpers that must not wrap in a 32-bit size_t.
//
// `offset + length > size` is the obvious bounds check and the wrong one: with a 32-bit size_t, an offset of
// 0xFFFFFFFF and a length of 2 add up to 1, which "fits". These run on 64-bit as well; the interesting
// results are on the armhf check (tools/vita/check32).
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <limits>

#include "core/size_utils.hpp"

using wowee::core::clampToSizeT;
using wowee::core::rangeFits;

TEST_CASE("rangeFits accepts ranges inside the buffer", "[size-utils]") {
    CHECK(rangeFits(0, 0, 0));
    CHECK(rangeFits(0, 10, 10));
    CHECK(rangeFits(4, 6, 10));
    CHECK(rangeFits(10, 0, 10));  // an empty range at the end is inside
}

TEST_CASE("rangeFits refuses ranges that end past the buffer", "[size-utils]") {
    CHECK_FALSE(rangeFits(0, 11, 10));
    CHECK_FALSE(rangeFits(5, 6, 10));
    CHECK_FALSE(rangeFits(11, 0, 10));  // starts past the end
}

TEST_CASE("rangeFits does not wrap near 2^32", "[size-utils]") {
    CHECK_FALSE(rangeFits(0xFFFFFFFFu, 2, 10));
    CHECK_FALSE(rangeFits(0xFFFFFFFCu, 8, 10));
    CHECK_FALSE(rangeFits(8, 0xFFFFFFFFu, 10));
    CHECK_FALSE(rangeFits(0xFFFFFFFFull, 0xFFFFFFFFull, 10));
    // 64-bit inputs that would wrap a 64-bit sum too.
    CHECK_FALSE(rangeFits(std::numeric_limits<uint64_t>::max(), 2, 10));
    CHECK_FALSE(rangeFits(2, std::numeric_limits<uint64_t>::max(), 10));
}

TEST_CASE("clampToSizeT saturates instead of wrapping", "[size-utils]") {
    CHECK(clampToSizeT(0) == 0);
    CHECK(clampToSizeT(1234) == 1234);
    CHECK(clampToSizeT(std::numeric_limits<uint64_t>::max()) == std::numeric_limits<size_t>::max());
    // 4 GiB: 0 if it were truncated on a 32-bit target.
    CHECK(clampToSizeT(1ull << 32) != 0);
}
