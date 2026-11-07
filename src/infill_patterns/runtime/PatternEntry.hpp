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
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

#include "libslic3r/Fill/AdaptiveInfillField.hpp"
#include "libslic3r/Fill/FillAdaptive.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/FillLightning.hpp"
#include "libslic3r/Fill/Library/BuiltinMirror.hpp"
#include "libslic3r/Fill/Library/orca_infill_builtin.h"
#include "libslic3r/Fill/Lightning/Generator.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
namespace PatternLibrary {

// Thrown in a library when the host cancels the job of prepare_state(), which then returns ORCA_INFILL_ERR_CANCELED.
struct JobCanceled {};

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

// The object state of the rows whose library prepares the kinds Kinds, (1u << ORCA_INFILL_STATE_*) for each:
//   using Type;                                  the state
//   static orca_infill_status prepare(int32_t kind, const void *data, const std::function<void()> &throw_if_canceled,
//                                     std::unique_ptr<Type> &state);
//                                                builds the state of the input data of kind; leaves it null when there is
//                                                nothing to build. throw_if_canceled throws JobCanceled.
//   static void attach(Fill &filler, const Type *state);
//                                                gives the state, or null, to the member of the filler that reads it; it
//                                                may take the Fill class of the row instead of Fill
//   static constexpr bool required;              whether a filler of the row lays its pattern only over a state, so that
//                                                run() refuses a call without one
//   static constexpr uint32_t queries;           (1u << ORCA_INFILL_QUERY_*) for each query query_state() answers, and,
//                                                when there are any,
//   static orca_infill_status query(const Type &state, const orca_infill_state_query &query, Polylines &lines);
template<uint32_t Kinds> struct ObjectState;

// The adaptive infill field of the TPMS rows. An empty field is no state, so that the filler lays its regular pattern.
template<> struct ObjectState<1u << ORCA_INFILL_STATE_ADAPTIVE_FIELD>
{
    using Type = AdaptiveInfillField;
    static constexpr uint32_t queries  = 0;
    static constexpr bool     required = false;

    static orca_infill_status prepare(int32_t, const void *data, const std::function<void()> &throw_if_canceled,
                                      std::unique_ptr<Type> &state)
    {
        const BuiltinMirror::FieldSlices field = BuiltinMirror::to_field_slices(*static_cast<const orca_infill_field_input*>(data));
        if (field.mode <= AdaptiveInfillMode::Disabled || field.mode >= AdaptiveInfillMode::Count)
            return ORCA_INFILL_ERR_INVALID;
        if (field.slices.empty() || ! field.bbox.defined)
            return ORCA_INFILL_OK;
        state = std::make_unique<Type>(field.slices, field.bbox, field.mode, throw_if_canceled);
        if (state->empty())
            state.reset();
        return ORCA_INFILL_OK;
    }

    static void attach(Fill &filler, const Type *state) { filler.adaptive_infill_field = state; }
};

// The octree of the adaptive and support cubic rows. FillAdaptive.hpp keeps Octree opaque, so the state owns it through
// its deleter.
template<> struct ObjectState<1u << ORCA_INFILL_STATE_OCTREE>
{
    struct Type
    {
        FillAdaptive::OctreePtr octree;
    };
    static constexpr uint32_t queries  = 0;
    static constexpr bool     required = false;

    static orca_infill_status prepare(int32_t, const void *data, const std::function<void()> &, std::unique_ptr<Type> &state)
    {
        const BuiltinMirror::OctreeMesh input = BuiltinMirror::to_octree_mesh(*static_cast<const orca_infill_octree_input*>(data));
        // The cubes double in size from twice the line spacing until they cover the mesh, which needs a positive spacing.
        if (! (input.line_spacing > 0.))
            return ORCA_INFILL_ERR_INVALID;
        state         = std::make_unique<Type>();
        state->octree = FillAdaptive::build_octree(input.mesh, input.overhangs, input.line_spacing, input.support_overhangs_only);
        return ORCA_INFILL_OK;
    }

    static void attach(Fill &filler, const Type *state) { filler.adapt_fill_octree = state != nullptr ? state->octree.get() : nullptr; }
};

// The lightning generators: of the infill, whose trees a filler of the row draws, and of the support base of tree
// support, whose trees the host queries as lines.
template<> struct ObjectState<(1u << ORCA_INFILL_STATE_LIGHTNING) | (1u << ORCA_INFILL_STATE_LIGHTNING_SUPPORT)>
{
    using Type = FillLightning::Generator;
    static constexpr uint32_t queries  = 1u << ORCA_INFILL_QUERY_LIGHTNING_LINES;
    static constexpr bool     required = true;

    static orca_infill_status prepare(int32_t kind, const void *data, const std::function<void()> &throw_if_canceled,
                                      std::unique_ptr<Type> &state)
    {
        BuiltinMirror::LightningLayers input = BuiltinMirror::to_lightning_layers(*static_cast<const orca_infill_lightning_input*>(data));
        // The supporting radius sets the cell size of the distance fields the trees grow over.
        if (input.params.supporting_radius <= 0)
            return ORCA_INFILL_ERR_INVALID;
        if (kind == ORCA_INFILL_STATE_LIGHTNING_SUPPORT) {
            // The trees grow down the contours of the layers to the overhangs of each; without layers there are none.
            if (input.overhangs.size() != input.outlines.size())
                return ORCA_INFILL_ERR_INVALID;
            state = std::make_unique<Type>(input.params, input.outlines, input.overhangs, throw_if_canceled);
            return ORCA_INFILL_OK;
        }
        // The trees grow down the layers of the outlines, which also give the overhangs they support.
        if (input.outlines.empty() || ! input.overhangs.empty())
            return ORCA_INFILL_ERR_INVALID;
        state = std::make_unique<Type>(input.params, input.outlines, throw_if_canceled);
        return ORCA_INFILL_OK;
    }

    static void attach(FillLightning::Filler &filler, const Type *state) { filler.generator = state; }

    // ORCA_INFILL_QUERY_LIGHTNING_LINES: the trees of the layer as lines within the area.
    static orca_infill_status query(const Type &state, const orca_infill_state_query &query, Polylines &lines)
    {
        if (query.layer >= state.layer_count())
            return ORCA_INFILL_ERR_INVALID;
        lines = state.getTreesForLayer(size_t(query.layer)).convertToLines(BuiltinMirror::to_polygons(query.area), query.overlap);
        return ORCA_INFILL_OK;
    }
};

// Row describes the row, as entry.cpp.in declares it:
//   using Filler;                                the Fill class of the row
//   static constexpr int32_t     pattern;        its InfillPattern value
//   static constexpr const char *key;            its token, nullptr for an internal row
//   static constexpr const char *name;           the file stem of the library
//   static constexpr const char *build_id;       ORCA_INFILL_BUILD_ID
//   static constexpr uint32_t    calls;          (1u << ORCA_INFILL_CALL_*) for each call run() accepts
//   static constexpr uint32_t    traits;         ORCA_INFILL_TRAIT_*
//   static constexpr uint32_t    state_kinds;    (1u << ORCA_INFILL_STATE_*) for each state prepare_state() builds;
//                                                ObjectState<state_kinds> describes them
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

    static constexpr bool has_state = Row::state_kinds != 0;
    using State                     = ObjectState<Row::state_kinds>;

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
            if constexpr (has_state) {
                const auto *state = static_cast<const typename State::Type*>(BuiltinMirror::to_state(*request->fill));
                if (state == nullptr && State::required)
                    return ORCA_INFILL_ERR_INVALID;
                State::attach(filler, state);
            }
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

    static orca_infill_status ORCA_INFILL_CALL prepare_state(int32_t pattern, const orca_infill_state_input *input, orca_infill_job *job,
                                                             void **state) noexcept
    {
        const orca_infill_builtin_host *host = s_host;
        if (state == nullptr)
            return ORCA_INFILL_ERR_INVALID;
        *state = nullptr;
        if (host == nullptr || pattern != Row::pattern || input == nullptr || input->struct_size < sizeof(orca_infill_state_input) ||
            input->kind < 0 || input->kind >= 32 || (Row::state_kinds & (1u << input->kind)) == 0 || input->data == nullptr)
            return ORCA_INFILL_ERR_INVALID;
        try {
#if ORCA_INFILL_PATTERN_MODULE
            CurrentJob scope(job);
#endif
            std::unique_ptr<typename State::Type> built;
            const orca_infill_status status = State::prepare(input->kind, input->data, [host, job] {
                if (host->is_canceled(job) != 0)
                    throw JobCanceled();
            }, built);
            *state = built.release();
            return status;
        } catch (const JobCanceled &) {
            return ORCA_INFILL_ERR_CANCELED;
        } catch (const std::bad_alloc &) {
            return ORCA_INFILL_ERR_NOMEM;
        } catch (const std::exception &ex) {
            // prepare_state() has no sink: the host takes the message of the failure from this line.
            if (host->log != nullptr)
                host->log(ORCA_INFILL_LOG_ERROR, ex.what());
            return ORCA_INFILL_ERR_EXCEPTION;
        } catch (...) {
            return ORCA_INFILL_ERR_EXCEPTION;
        }
    }

    static void ORCA_INFILL_CALL release_state(void *state) noexcept { delete static_cast<typename State::Type*>(state); }

    static orca_infill_status ORCA_INFILL_CALL query_state(void *state, const orca_infill_state_query *query, orca_infill_sink *sink) noexcept
    {
        const orca_infill_builtin_host *host = s_host;
        if (host == nullptr || state == nullptr || query == nullptr || sink == nullptr || query->struct_size < sizeof(orca_infill_state_query) ||
            query->kind < 0 || query->kind >= 32 || (State::queries & (1u << query->kind)) == 0)
            return ORCA_INFILL_ERR_INVALID;
        try {
            Polylines lines;
            if (orca_infill_status status = State::query(*static_cast<const typename State::Type*>(state), *query, lines);
                status != ORCA_INFILL_OK)
                return status;
            return emit(host, sink, lines);
        } catch (const std::bad_alloc &) {
            return ORCA_INFILL_ERR_NOMEM;
        } catch (const std::exception &ex) {
            host->set_error(sink, ex.what());
            return ORCA_INFILL_ERR_EXCEPTION;
        } catch (...) {
            return ORCA_INFILL_ERR_EXCEPTION;
        }
    }

    // The object state functions of the library, null without states, and query_state() also without queries.
    static constexpr auto prepare_state_function()
    {
        if constexpr (has_state)
            return &prepare_state;
        else
            return decltype(&prepare_state)(nullptr);
    }
    static constexpr auto release_state_function()
    {
        if constexpr (has_state)
            return &release_state;
        else
            return decltype(&release_state)(nullptr);
    }
    static constexpr auto query_state_function()
    {
        if constexpr (has_state) {
            if constexpr (State::queries != 0)
                return &query_state;
            else
                return decltype(&query_state)(nullptr);
        } else
            return decltype(&query_state)(nullptr);
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
        prepare_state_function(),
        release_state_function(),
        query_state_function(),
    };
};

} // namespace PatternLibrary
} // namespace Slic3r
