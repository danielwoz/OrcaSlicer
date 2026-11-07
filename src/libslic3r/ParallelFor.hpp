#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>

namespace Slic3r {

// Parallel loops for code that must not depend on a particular thread pool. A loop calls its body on subranges that
// together cover the range exactly once, possibly on several threads at once, and splits the range no finer than grain
// items. The result of a loop must not depend on how its range is split.
using ParallelForBody   = void (*)(void *user, int64_t begin, int64_t end);
using ParallelFor2dBody = void (*)(void *user, int64_t row_begin, int64_t row_end, int64_t col_begin, int64_t col_end);

// Calls body(user, begin, end) for subranges of [begin, end).
void parallel_for(int64_t begin, int64_t end, int64_t grain, ParallelForBody body, void *user);
// Calls body(user, row_begin, row_end, col_begin, col_end) for the blocks of a grid of rows [row_begin, row_end) and
// columns [col_begin, col_end).
void parallel_for_2d(int64_t row_begin, int64_t row_end, int64_t row_grain, int64_t col_begin, int64_t col_end, int64_t col_grain,
                     ParallelFor2dBody body, void *user);

// Calls fn(begin, end) for subranges of [begin, end).
template<class Fn> void parallel_for(int64_t begin, int64_t end, int64_t grain, Fn &&fn)
{
    using F = std::remove_reference_t<Fn>;
    parallel_for(begin, end, grain, [](void *user, int64_t b, int64_t e) { (*static_cast<F *>(user))(b, e); },
                 const_cast<void *>(static_cast<const void *>(std::addressof(fn))));
}

// Calls fn(row_begin, row_end, col_begin, col_end) for the blocks of the grid.
template<class Fn>
void parallel_for_2d(int64_t row_begin, int64_t row_end, int64_t row_grain, int64_t col_begin, int64_t col_end, int64_t col_grain, Fn &&fn)
{
    using F = std::remove_reference_t<Fn>;
    parallel_for_2d(row_begin, row_end, row_grain, col_begin, col_end, col_grain,
                    [](void *user, int64_t rb, int64_t re, int64_t cb, int64_t ce) { (*static_cast<F *>(user))(rb, re, cb, ce); },
                    const_cast<void *>(static_cast<const void *>(std::addressof(fn))));
}

} // namespace Slic3r
