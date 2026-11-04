// The parallel loops of ParallelFor.hpp on the TBB thread pool, with TBB's default partitioner.
#include "ParallelFor.hpp"

#include <cstddef>

#include <tbb/blocked_range.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>

namespace Slic3r {

void parallel_for(int64_t begin, int64_t end, int64_t grain, ParallelForBody body, void *user)
{
    tbb::parallel_for(tbb::blocked_range<int64_t>(begin, end, size_t(grain)),
                      [body, user](const tbb::blocked_range<int64_t> &range) { body(user, range.begin(), range.end()); });
}

void parallel_for_2d(int64_t row_begin, int64_t row_end, int64_t row_grain, int64_t col_begin, int64_t col_end, int64_t col_grain,
                     ParallelFor2dBody body, void *user)
{
    tbb::parallel_for(tbb::blocked_range2d<int64_t>(row_begin, row_end, size_t(row_grain), col_begin, col_end, size_t(col_grain)),
                      [body, user](const tbb::blocked_range2d<int64_t> &range) {
                          body(user, range.rows().begin(), range.rows().end(), range.cols().begin(), range.cols().end());
                      });
}

} // namespace Slic3r
