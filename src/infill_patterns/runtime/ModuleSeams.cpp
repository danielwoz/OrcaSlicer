// The definitions a built-in infill pattern library has in place of libslic3r's for the symbols that libslic3r_fillkit
// and the pattern objects reference outside themselves: the log goes to the host, the TBB allocator is the C runtime's,
// parallel loops run on the host's job, and the host's half of Fill stays in the host. Everything else in a library is
// the object code libslic3r links, so a library carries no TBB runtime or allocator, no Boost.Log core and no thread.
#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <new>
#include <stdexcept>

#include <oneapi/tbb/scalable_allocator.h>

#include "PatternEntry.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/Library/KitLog.hpp"
#include "libslic3r/ParallelFor.hpp"

namespace Slic3r {

namespace {

const orca_infill_builtin_host *g_host = nullptr;
thread_local orca_infill_job   *t_job  = nullptr;

// boost::log::trivial::severity_level to ORCA_INFILL_LOG_*.
int32_t log_level(int severity)
{
    switch (severity) {
    case 0:
    case 1: return ORCA_INFILL_LOG_DEBUG;
    case 2: return ORCA_INFILL_LOG_INFO;
    case 3: return ORCA_INFILL_LOG_WARNING;
    default: return ORCA_INFILL_LOG_ERROR;
    }
}

// A loop handed to the host: each subrange runs on the job of the loop and the first exception is kept, to be thrown
// on the calling thread once the host's loop returns. Subranges after it are skipped.
template<class Body> struct Trampoline
{
    Body               body;
    void              *user;
    orca_infill_job   *job;
    std::atomic<bool>  failed { false };
    std::mutex         mutex;
    std::exception_ptr error;

    template<class... Range> void run(Range... range) noexcept
    {
        if (failed.load(std::memory_order_relaxed))
            return;
        try {
            PatternLibrary::CurrentJob scope(job);
            body(user, range...);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            if (! error)
                error = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
    }

    void finish(orca_infill_status status)
    {
        if (error)
            std::rethrow_exception(error);
        if (status == ORCA_INFILL_ERR_CANCELED)
            throw PatternLibrary::JobCanceled();
        if (status == ORCA_INFILL_ERR_NOMEM)
            throw std::bad_alloc();
        if (status != ORCA_INFILL_OK)
            throw RuntimeError("The parallel loop of an infill pattern library failed");
    }
};

void ORCA_INFILL_CALL run_range(void *user, int64_t begin, int64_t end)
{
    static_cast<Trampoline<ParallelForBody>*>(user)->run(begin, end);
}

void ORCA_INFILL_CALL run_range_2d(void *user, int64_t row_begin, int64_t row_end, int64_t col_begin, int64_t col_end)
{
    static_cast<Trampoline<ParallelFor2dBody>*>(user)->run(row_begin, row_end, col_begin, col_end);
}

} // namespace

namespace PatternLibrary {

void bind_host(const orca_infill_builtin_host *host) { g_host = host; }

CurrentJob::CurrentJob(orca_infill_job *job) : m_outer(t_job) { t_job = job; }
CurrentJob::~CurrentJob() { t_job = m_outer; }

} // namespace PatternLibrary

// Fill/Library/KitLog.hpp
bool kit_log_enabled(int severity)
{
    const orca_infill_builtin_host *host = g_host;
    return host != nullptr && host->log != nullptr && (host->log_enabled == nullptr || host->log_enabled(log_level(severity)) != 0);
}

void kit_log_write(int severity, const char *message)
{
    if (const orca_infill_builtin_host *host = g_host; host != nullptr && host->log != nullptr)
        host->log(log_level(severity), message);
}

// ParallelFor.hpp: on the host's threads within a job, on this thread otherwise.
void parallel_for(int64_t begin, int64_t end, int64_t grain, ParallelForBody body, void *user)
{
    if (begin >= end)
        return;
    const orca_infill_builtin_host *host = g_host;
    if (t_job == nullptr || host == nullptr || host->parallel_for == nullptr) {
        body(user, begin, end);
        return;
    }
    Trampoline<ParallelForBody> loop { body, user, t_job };
    loop.finish(host->parallel_for(t_job, begin, end, grain, &run_range, &loop));
}

void parallel_for_2d(int64_t row_begin, int64_t row_end, int64_t row_grain, int64_t col_begin, int64_t col_end, int64_t col_grain,
                     ParallelFor2dBody body, void *user)
{
    if (row_begin >= row_end || col_begin >= col_end)
        return;
    const orca_infill_builtin_host *host = g_host;
    if (t_job == nullptr || host == nullptr || host->parallel_for_2d == nullptr) {
        body(user, row_begin, row_end, col_begin, col_end);
        return;
    }
    Trampoline<ParallelFor2dBody> loop { body, user, t_job };
    loop.finish(host->parallel_for_2d(t_job, row_begin, row_end, row_grain, col_begin, col_end, col_grain, &run_range_2d, &loop));
}

// The extrusions of a filler are made by the host (Fill/FillExtrusion.cpp); a library only generates lines.
void Fill::fill_surface_extrusion(const Surface *, const FillParams &, ExtrusionEntitiesPtr &) { std::abort(); }

} // namespace Slic3r

// Points and polygons allocate through tbb::scalable_allocator; in a library that is the C runtime's heap.
extern "C" void* __TBB_EXPORTED_FUNC scalable_malloc(size_t size) { return std::malloc(size); }
extern "C" void  __TBB_EXPORTED_FUNC scalable_free(void *ptr) { std::free(ptr); }
