#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#include "libslic3r/ParallelFor.hpp"

using namespace Slic3r;

namespace {

bool each_once(const std::vector<std::atomic<int>> &hits)
{
    return std::all_of(hits.begin(), hits.end(), [](const std::atomic<int> &h) { return h == 1; });
}

} // namespace

TEST_CASE("Parallel loops cover their range exactly once", "[InfillPatterns][ParallelFor]")
{
    const int64_t grain = GENERATE(int64_t(1), int64_t(7), int64_t(1000));
    CAPTURE(grain);

    std::vector<std::atomic<int>> hits(501);
    std::atomic<bool>             empty_part { false };
    parallel_for(-200, 301, grain, [&](int64_t begin, int64_t end) {
        empty_part = empty_part || begin >= end;
        for (int64_t i = begin; i < end; ++ i)
            ++ hits[size_t(i + 200)];
    });
    CHECK(each_once(hits));
    CHECK(! empty_part);

    std::vector<std::atomic<int>> cells(37 * 23);
    parallel_for_2d(-5, 32, grain, 10, 33, grain, [&cells](int64_t row_begin, int64_t row_end, int64_t col_begin, int64_t col_end) {
        for (int64_t r = row_begin; r < row_end; ++ r)
            for (int64_t c = col_begin; c < col_end; ++ c)
                ++ cells[size_t((r + 5) * 23 + (c - 10))];
    });
    CHECK(each_once(cells));

    std::atomic<bool> called { false };
    parallel_for(4, 4, grain, [&called](int64_t, int64_t) { called = true; });
    CHECK(! called);
}
