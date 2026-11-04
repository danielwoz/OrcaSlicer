#pragma once

// The entry of a built-in infill pattern library (src/libslic3r/Fill/Library/orca_infill_builtin.h). A library serves
// one row of the pattern table: it rebuilds the C++ state of a filler call from the C request (BuiltinMirror) and runs
// the row's Fill class on it, unchanged. entry.cpp.in instantiates PatternEntry for the row of each library.
//
// ORCA_INFILL_PATTERN_MODULE is 1 in a loadable library, whose log and parallel loops go to its host
// (ModuleSeams.cpp), and 0 for an entry linked into the application.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/Library/BuiltinMirror.hpp"
#include "libslic3r/Fill/Library/orca_infill_builtin.h"
#include "libslic3r/Surface.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
namespace PatternLibrary {

#if ORCA_INFILL_PATTERN_MODULE
// Routes the log and the parallel loops of this library to host (ModuleSeams.cpp).
void bind_host(const orca_infill_builtin_host *host);

// Makes job the job whose loops the parallel loops of this thread run on, until destruction. Without a job, a loop runs
// on the calling thread.
class CurrentJob
{
public:
    explicit CurrentJob(orca_infill_job *job);
    ~CurrentJob();
    CurrentJob(const CurrentJob &) = delete;
    CurrentJob& operator=(const CurrentJob &) = delete;

private:
    orca_infill_job *m_outer;
};
#endif

// Row describes the row, as entry.cpp.in declares it:
//   using Filler;                                the Fill class of the row
//   static constexpr int32_t     pattern;        its InfillPattern value
//   static constexpr const char *key;            its token, nullptr for an internal row
//   static constexpr const char *name;           the file stem of the library
//   static constexpr const char *build_id;       ORCA_INFILL_BUILD_ID
//   static constexpr uint32_t    calls;          (1u << ORCA_INFILL_CALL_*) for each call run() accepts
//   static constexpr uint32_t    traits;         ORCA_INFILL_TRAIT_*
//   static constexpr uint32_t    state_kinds;    (1u << ORCA_INFILL_STATE_*) for each state prepare_state() builds
template<class Row>
class PatternEntry
{
public:
    // ORCA_INFILL_BUILTIN_ENTRY_V1: the library, or nullptr after logging its own build id when the host is of another
    // build.
    static const orca_infill_builtin_library* bind(const orca_infill_builtin_host *host) noexcept
    {
        if (host == nullptr || host->struct_size < sizeof(orca_infill_builtin_host))
            return nullptr;
        if (host->abi_major != ORCA_INFILL_BUILTIN_ABI_MAJOR || host->build_id == nullptr ||
            std::strcmp(host->build_id, Row::build_id) != 0) {
            if (host->log != nullptr)
                host->log(ORCA_INFILL_LOG_ERROR, Row::build_id);
            return nullptr;
        }
        s_host = host;
#if ORCA_INFILL_PATTERN_MODULE
        bind_host(host);
#endif
        return &s_library;
    }

private:
    using Filler = typename Row::Filler;

    static constexpr uint32_t served_calls = (1u << ORCA_INFILL_CALL_FILL_SURFACE) | (1u << ORCA_INFILL_CALL_FILL_ARACHNE) |
                                             (1u << ORCA_INFILL_CALL_LOCKED_ZAG) | (1u << ORCA_INFILL_CALL_CONCENTRIC_INTERNAL);
    static_assert(Row::calls != 0 && (Row::calls & ~served_calls) == 0,
                  "PatternEntry runs fill_surface(), fill_surface_arachne(), fill_surface_locked_zag() and "
                  "concentric_internal_lines()");
    static constexpr bool locked_zag          = (Row::calls & (1u << ORCA_INFILL_CALL_LOCKED_ZAG)) != 0;
    static constexpr bool concentric_internal = (Row::calls & (1u << ORCA_INFILL_CALL_CONCENTRIC_INTERNAL)) != 0;
    static_assert(Row::state_kinds == 0, "PatternEntry prepares no object state");

    static void ORCA_INFILL_CALL set_scaling_factor(double scaling_factor) noexcept { SCALING_FACTOR = scaling_factor; }

    static void* ORCA_INFILL_CALL create(int32_t pattern) noexcept
    {
        if (pattern != Row::pattern)
            return nullptr;
        try {
            return new Filler();
        } catch (...) {
            return nullptr;
        }
    }

    static void ORCA_INFILL_CALL destroy(void *instance) noexcept { delete static_cast<Filler*>(instance); }

    static orca_infill_status ORCA_INFILL_CALL run(void *instance, const orca_infill_builtin_request *request,
                                                   orca_infill_sink *sink) noexcept
    {
        const orca_infill_builtin_host *host = s_host;
        if (host == nullptr || instance == nullptr || request == nullptr || sink == nullptr ||
            request->struct_size < sizeof(orca_infill_builtin_request) || request->call < 0 || request->call >= 32 ||
            (Row::calls & (1u << request->call)) == 0 || request->surface == nullptr || request->params == nullptr ||
            request->fill == nullptr)
            return ORCA_INFILL_ERR_INVALID;
        // The host sets this library's SCALING_FACTOR whenever it changes its own; a request made under another one
        // would be scaled differently.
        if (request->scaling_factor != SCALING_FACTOR)
            return ORCA_INFILL_ERR_STATE;

        Filler &filler = *static_cast<Filler*>(instance);
        try {
            const Surface    surface = BuiltinMirror::to_surface(*request->surface);
            const FillParams params  = BuiltinMirror::to_fill_params(*request->params);
            BuiltinMirror::apply_fill_state(*request->fill, filler);
            if (request->lock != nullptr)
                filler.set_lock_region_param(BuiltinMirror::to_lock_region_param(*request->lock));
            if (request->call == ORCA_INFILL_CALL_FILL_SURFACE) {
                const Polylines lines = filler.fill_surface(&surface, params);
                host->set_spacing(sink, filler.spacing);
                return emit(host, sink, lines);
            }
            if constexpr (locked_zag) {
                if (request->call == ORCA_INFILL_CALL_LOCKED_ZAG) {
                    // The lines generated before an InfillFailedException are kept, as FillLockedZag keeps them.
                    std::vector<std::pair<Polylines, Flow>> groups;
                    orca_infill_status                      status = ORCA_INFILL_OK;
                    try {
                        filler.fill_surface_locked_zag(&surface, params, groups);
                    } catch (const InfillFailedException &) {
                        status = ORCA_INFILL_ERR_FAILED;
                    }
                    host->set_spacing(sink, filler.spacing);
                    const orca_infill_status emitted = emit(host, sink, filler.lock_param, groups);
                    return emitted != ORCA_INFILL_OK ? emitted : status;
                }
            }
            if constexpr (concentric_internal) {
                if (request->call == ORCA_INFILL_CALL_CONCENTRIC_INTERNAL) {
                    const ThickPolylines lines = filler.concentric_internal_lines(params);
                    host->set_spacing(sink, filler.spacing);
                    return emit(host, sink, lines);
                }
            }
            const ThickPolylines lines = filler.fill_surface_arachne(&surface, params);
            host->set_spacing(sink, filler.spacing);
            return emit(host, sink, lines);
        } catch (const InfillFailedException &) {
            host->set_spacing(sink, filler.spacing);
            return ORCA_INFILL_ERR_FAILED;
        } catch (const std::bad_alloc &) {
            return ORCA_INFILL_ERR_NOMEM;
        } catch (const std::exception &ex) {
            host->set_error(sink, ex.what());
            return ORCA_INFILL_ERR_EXCEPTION;
        } catch (...) {
            return ORCA_INFILL_ERR_EXCEPTION;
        }
    }

    static orca_infill_status emit(const orca_infill_builtin_host *host, orca_infill_sink *sink, const Polylines &lines)
    {
        for (const Polyline &line : lines)
            if (orca_infill_status status = host->emit_polyline(sink, BuiltinMirror::view(line.points), line.points.size());
                status != ORCA_INFILL_OK)
                return status;
        return ORCA_INFILL_OK;
    }

    static orca_infill_status emit(const orca_infill_builtin_host *host, orca_infill_sink *sink, const ThickPolylines &lines)
    {
        for (const ThickPolyline &line : lines) {
            const orca_infill_thick_path path = BuiltinMirror::view(line);
            if (orca_infill_status status = host->emit_thick_polyline(sink, &path); status != ORCA_INFILL_OK)
                return status;
        }
        return ORCA_INFILL_OK;
    }

    // Group i of fill_surface_locked_zag() holds the lines of flow i, the skeleton flows of lock and then its skin flows,
    // which is the flow index of begin_group().
    static orca_infill_status emit(const orca_infill_builtin_host *host, orca_infill_sink *sink, const LockRegionParam &lock,
                                   const std::vector<std::pair<Polylines, Flow>> &groups)
    {
        std::vector<const Flow*> flows;
        for (const auto *map : { &lock.skeleton_flow_params, &lock.skin_flow_params })
            for (const auto &entry : *map)
                flows.emplace_back(&entry.first);
        for (size_t i = 0; i < groups.size(); ++ i)
            if (i >= flows.size() || groups[i].second != *flows[i])
                throw std::logic_error("Locked Zag lines of a flow out of the order of the lock maps");
        for (size_t i = 0; i < groups.size(); ++ i) {
            if (orca_infill_status status = host->begin_group(sink, i); status != ORCA_INFILL_OK)
                return status;
            if (orca_infill_status status = emit(host, sink, groups[i].first); status != ORCA_INFILL_OK)
                return status;
        }
        return ORCA_INFILL_OK;
    }

    static inline const orca_infill_builtin_host *s_host = nullptr;

    static inline const orca_infill_builtin_row s_row = {
        sizeof(orca_infill_builtin_row), Row::pattern, Row::key, Row::calls, Row::traits, Row::state_kinds };
    static inline const orca_infill_builtin_row *const s_rows[] = { &s_row };

    static inline const orca_infill_builtin_library s_library = {
        sizeof(orca_infill_builtin_library),
        ORCA_INFILL_BUILTIN_ABI_MAJOR,
        ORCA_INFILL_BUILTIN_ABI_MINOR,
        Row::build_id,
        Row::name,
        s_rows,
        1,
        &set_scaling_factor,
        &create,
        &destroy,
        &run,
        nullptr, // prepare_state
        nullptr, // release_state
        nullptr, // query_state
    };
};

} // namespace PatternLibrary
} // namespace Slic3r
