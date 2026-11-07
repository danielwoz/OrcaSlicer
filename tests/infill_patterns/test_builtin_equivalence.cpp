// The built-in pattern libraries against the Fill classes they carry: on every case of the corpus of a row a library
// serves, the filler of the row (Fill::new_from_type(), which runs in the library) returns what the class itself
// returns, bit for bit, under both scaling factors, when the cases run in any order on several threads at once, and
// when one filler runs every case of its row in turn. The object states of the cases are prepared in the library of
// their row from the inputs of the native states, however the loops of the preparation are split.
// This file is compiled with ORCA_INFILL_PATTERN_MODULE=0, so that it can link an entry of its own (PatternEntry.hpp).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "PatternEntry.hpp"
#include "orca_infill_build_id.h"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Fill/FillLibrary.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"
#include "libslic3r/Fill/InfillObjectState.hpp"
#include "libslic3r/Fill/InfillPatternLibraries.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/Library/BuiltinMirror.hpp"
#include "libslic3r/Fill/Library/orca_infill_builtin.h"
#include "libslic3r/Fill/Lightning/Generator.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r_infill_patterns.h"

#include "corpus.hpp"
#include "native_fillers.hpp"

using namespace Slic3r;
using namespace Slic3r::InfillCorpus;
using Catch::Matchers::ContainsSubstring;

namespace {

bool same_points(const Points &a, const Points &b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Point)) == 0);
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

bool same_lines(const Polylines &expected, const Polylines &got)
{
    if (expected.size() != got.size())
        return false;
    for (size_t i = 0; i < got.size(); ++ i)
        if (! same_points(expected[i].points, got[i].points) || ! got[i].fitting_result.empty())
            return false;
    return true;
}

// The first difference between two outputs of c; empty when they are equal bit for bit.
std::string difference(const Case &c, const Output &expected, const Output &got)
{
    if (expected.error != got.error)
        return "exception kind";
    if (! same_bits(expected.spacing, got.spacing))
        return "spacing";
    if (expected.polylines.size() != got.polylines.size())
        return "number of polylines";
    for (size_t i = 0; i < got.polylines.size(); ++ i)
        if (! same_points(expected.polylines[i].points, got.polylines[i].points) || ! got.polylines[i].fitting_result.empty())
            return "polyline " + std::to_string(i);
    if (expected.thick_polylines.size() != got.thick_polylines.size())
        return "number of thick polylines";
    for (size_t i = 0; i < got.thick_polylines.size(); ++ i) {
        const ThickPolyline &a = expected.thick_polylines[i], &b = got.thick_polylines[i];
        if (! same_points(a.points, b.points) || a.width.size() != b.width.size() ||
            (! a.width.empty() && std::memcmp(a.width.data(), b.width.data(), a.width.size() * sizeof(double)) != 0) ||
            a.endpoints != b.endpoints || ! b.fitting_result.empty())
            return "thick polyline " + std::to_string(i);
    }
    // The digest covers every value, the flow groups and the extrusions included.
    const Digest a = digest(c, expected), b = digest(c, got);
    if (a.hash != b.hash || a.paths != b.paths || a.points != b.points)
        return "extrusions";
    return {};
}

// The object states of the cases, prepared by the library of their row from the inputs the native states are built
// from, under the current SCALING_FACTOR.
class LibraryStates
{
public:
    explicit LibraryStates(NativeStates &native) : m_native(native) {}

    // The state of the case, nullptr when it has none or the library prepared none.
    const InfillObjectState* get(const Case &c)
    {
        if (c.state == State::None)
            return nullptr;
        const bool field = c.state == State::Field;
        const AdaptiveInfillMode mode = field ? c.params.adaptive_infill_mode : AdaptiveInfillMode::Disabled;
        if (field && mode == AdaptiveInfillMode::Disabled)
            return nullptr;
        const auto key = std::make_tuple(SCALING_FACTOR, c.pattern, c.state, mode);
        auto       it  = m_states.find(key);
        if (it == m_states.end())
            it = m_states.emplace(key, field ? this->prepare_field(c.pattern, mode) :
                                       c.state == State::Lightning ? this->prepare_lightning(c.pattern) :
                                                                     this->prepare_octree(c.pattern, c.state)).first;
        return it->second.get();
    }

    InfillObjectStatePtr prepare_field(InfillPattern row, AdaptiveInfillMode mode, const std::function<void()> &throw_if_canceled = {})
    {
        const BuiltinMirror::FieldInput input(m_native.field_slices(), m_native.field_bbox());
        const orca_infill_field_input   field = input.get(mode);
        return prepare_builtin_infill_state(row, ORCA_INFILL_STATE_ADAPTIVE_FIELD, &field, throw_if_canceled);
    }

    // The octree of state, prepared by the library of row.
    InfillObjectStatePtr prepare_octree(InfillPattern row, State state)
    {
        const orca_infill_octree_input input = octree_input(m_native, state);
        return prepare_builtin_infill_state(row, ORCA_INFILL_STATE_OCTREE, &input, {});
    }

    // The C input of the native octree of state.
    static orca_infill_octree_input octree_input(NativeStates &native, State state)
    {
        const NativeStates::OctreeInput input = native.octree_input(state);
        return BuiltinMirror::octree_input(input.mesh, input.overhangs, input.line_spacing, input.support_overhangs_only);
    }

    // The lightning generator, prepared by the library of row from the inputs of the native one.
    InfillObjectStatePtr prepare_lightning(InfillPattern row, const std::function<void()> &throw_if_canceled = {})
    {
        const NativeStates::LightningInput  native = m_native.lightning_input();
        const BuiltinMirror::LightningInput input(native.params, native.outlines);
        return prepare_builtin_infill_state(row, ORCA_INFILL_STATE_LIGHTNING, &input.get(), throw_if_canceled);
    }

    void clear() { m_states.clear(); }

private:
    NativeStates                                                                            &m_native;
    std::map<std::tuple<double, InfillPattern, State, AdaptiveInfillMode>, InfillObjectStatePtr> m_states;
};

Output run_native(const Case &c, NativeStates &states)
{
    const std::unique_ptr<Fill> filler = native_filler(c.pattern);
    apply_members(c, *filler);
    states.attach(c, *filler);
    return run(c, *filler);
}

Output run_library(const Case &c, LibraryStates &states)
{
    const std::unique_ptr<Fill> filler(Fill::new_from_type(c.pattern));
    REQUIRE(dynamic_cast<FillLibrary*>(filler.get()) != nullptr);
    apply_members(c, *filler);
    filler->object_state = states.get(c);
    return run(c, *filler);
}

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
// While it lives, the library of row runs the parallel loops of prepare_state() on the calling thread, in blocks of
// grain items or of the grain of the loop if larger, the last block first; a loop over a grid in such blocks of rows
// and of columns. A status other than ORCA_INFILL_OK fails every loop with that status instead. The library is bound
// to the application's host again afterwards.
class SplitLoops
{
public:
    SplitLoops(InfillPattern row, int64_t grain, orca_infill_status status = ORCA_INFILL_OK)
    {
        for (const BuiltinInfillLoad &load : builtin_infill_pattern_loads())
            if (load.pattern == row)
                m_entry = load.entry;
        REQUIRE(m_entry != nullptr);
        s_grain  = grain;
        s_status = status;
        s_host   = builtin_infill_host();
        s_host.parallel_for    = &split;
        s_host.parallel_for_2d = &split_2d;
        REQUIRE(m_entry(&s_host) == InfillPatternRegistry::binding(row)->library);
    }
    ~SplitLoops() { m_entry(&builtin_infill_host()); }
    SplitLoops(const SplitLoops &) = delete;
    SplitLoops& operator=(const SplitLoops &) = delete;

    // The blocks run so far, of any loop and of the loops over a grid.
    static size_t blocks() { return s_blocks; }
    static size_t grid_blocks() { return s_grid_blocks; }

private:
    static orca_infill_status ORCA_INFILL_CALL split(orca_infill_job *job, int64_t begin, int64_t end, int64_t grain,
                                                     orca_infill_range_fn body, void *user)
    {
        if (s_status != ORCA_INFILL_OK)
            return s_status;
        if (begin >= end)
            return ORCA_INFILL_OK;
        const int64_t step = std::max(grain, s_grain);
        for (int64_t first = begin + (end - begin - 1) / step * step; first >= begin; first -= step) {
            if (builtin_infill_host().is_canceled(job) != 0)
                return ORCA_INFILL_ERR_CANCELED;
            body(user, first, std::min(first + step, end));
            ++ s_blocks;
        }
        return ORCA_INFILL_OK;
    }

    static orca_infill_status ORCA_INFILL_CALL split_2d(orca_infill_job *job, int64_t row_begin, int64_t row_end, int64_t row_grain,
                                                        int64_t col_begin, int64_t col_end, int64_t col_grain,
                                                        orca_infill_range2d_fn body, void *user)
    {
        if (s_status != ORCA_INFILL_OK)
            return s_status;
        if (row_begin >= row_end || col_begin >= col_end)
            return ORCA_INFILL_OK;
        const int64_t row_step = std::max(row_grain, s_grain), col_step = std::max(col_grain, s_grain);
        for (int64_t row = row_begin + (row_end - row_begin - 1) / row_step * row_step; row >= row_begin; row -= row_step)
            for (int64_t col = col_begin + (col_end - col_begin - 1) / col_step * col_step; col >= col_begin; col -= col_step) {
                if (builtin_infill_host().is_canceled(job) != 0)
                    return ORCA_INFILL_ERR_CANCELED;
                body(user, row, std::min(row + row_step, row_end), col, std::min(col + col_step, col_end));
                ++ s_blocks;
                ++ s_grid_blocks;
            }
        return ORCA_INFILL_OK;
    }

    static inline size_t                   s_blocks = 0;
    static inline size_t                   s_grid_blocks = 0;
    static inline int64_t                  s_grain = 1;
    static inline orca_infill_status       s_status = ORCA_INFILL_OK;
    static inline orca_infill_builtin_host s_host {};
    BuiltinInfillEntry                     m_entry = nullptr;
};
#endif

// The cases of the rows a library serves, by scaling factor.
std::map<double, std::vector<const Case*>> library_cases()
{
    std::map<double, std::vector<const Case*>> out;
    for (const Case &c : cases())
        if (InfillPatternRegistry::bound(c.pattern))
            out[c.scaling_factor].push_back(&c);
    return out;
}

// Restores SCALING_FACTOR, in the application and the libraries, at the end of a test.
struct ScalingFactorGuard
{
    ~ScalingFactorGuard() { set_scaling_factor(saved); }
    double saved = SCALING_FACTOR;
};

// A canceled preparation stops with the exception of the application's cancel, at the first check or at the check
// later, which prepare passes as its throw_if_canceled.
void check_canceled(const std::function<InfillObjectStatePtr(const std::function<void()> &)> &prepare, int later)
{
    for (const int checks : { 0, later }) {
        CAPTURE(checks);
        int count = 0;
        CHECK_THROWS_AS(prepare([&count, checks] {
                            if (count++ >= checks)
                                throw CanceledException();
                        }),
                        CanceledException);
        CHECK(count > checks);
    }
}

// The copy of the library of a row linked into the executable, as ORCA_INFILL_PATTERN_LINKAGE=STATIC links it, and a
// state it prepared.
struct LinkedState
{
    BuiltinInfillLoad                  load;
    std::unique_ptr<InfillObjectState> state;
};

// The state of input, prepared on a job by the linked copy of the library of row.
LinkedState prepare_linked(InfillPattern row, const orca_infill_state_input &input)
{
    const auto linked = std::find_if(linked_builtin_infill_patterns().begin(), linked_builtin_infill_patterns().end(),
                                     [row](const LinkedBuiltinInfillLibrary &l) { return l.pattern == row; });
    REQUIRE(linked != linked_builtin_infill_patterns().end());
    LinkedState out { check_builtin_infill_entry(linked->entry, row), nullptr };
    REQUIRE(out.load.library != nullptr);
    orca_infill_job job;
    void           *raw = nullptr;
    REQUIRE(out.load.library->prepare_state(int32_t(row), &input, &job, &raw) == ORCA_INFILL_OK);
    REQUIRE(raw != nullptr);
    out.state = std::make_unique<InfillObjectState>(*out.load.library, raw);
    return out;
}

// The output of c on a filler of the linked library, with the state it prepared.
Output run_linked(const Case &c, const LinkedState &linked)
{
    FillLibrary filler(c.pattern, *linked.load.library, linked.load.file);
    apply_members(c, filler);
    filler.object_state = linked.state.get();
    return run(c, filler);
}

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
// Checks each lightning generator prepare returns while both of its loops, over the samples and over the grid around a
// sample, are split into single items and into blocks of 7.
void check_split_lightning(const std::function<InfillObjectStatePtr()>            &prepare,
                           const std::function<void(const InfillObjectState &)> &check)
{
    for (const int64_t grain : { 1, 7 }) {
        CAPTURE(grain);
        const size_t         blocks = SplitLoops::blocks(), grid_blocks = SplitLoops::grid_blocks();
        InfillObjectStatePtr generator;
        {
            const SplitLoops split(ipLightning, grain);
            generator = prepare();
        }
        CHECK(SplitLoops::grid_blocks() > grid_blocks);
        CHECK(SplitLoops::blocks() - blocks > SplitLoops::grid_blocks() - grid_blocks);
        REQUIRE(generator != nullptr);
        check(*generator);
    }
}
#endif

} // namespace

TEST_CASE("Pattern libraries reproduce the Fill classes they carry", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    const std::map<double, std::vector<const Case*>> groups = library_cases();
    REQUIRE(! groups.empty());
    // Both scaling factors of the corpus reach the libraries.
    CHECK(groups.size() == 2);
    NativeStates                    native_states;
    LibraryStates                   library_states(native_states);
    std::map<InfillPattern, size_t> per_row;
    size_t                          with_state = 0;
    for (const auto &entry : groups) {
        const std::vector<const Case*> &group = entry.second;
        set_scaling_factor(entry.first);
        std::vector<Output>                   expected;
        std::vector<const InfillObjectState*> states;
        expected.reserve(group.size());
        for (const Case *c : group) {
            CAPTURE(c->name);
            ++ per_row[c->pattern];
            expected.emplace_back(run_native(*c, native_states));
            const Output got = run_library(*c, library_states);
            CHECK(difference(*c, expected.back(), got).empty());
            // Interlocking takes every line for a segment along y (VoxelUtils.cpp, spreadDotsArea()); the rotation of
            // the lines leaves their ends a few units apart in x.
            if (c->pattern == ipAlignedRectilinear && c->name.find("/interlocking") != std::string::npos)
                for (const Polyline &line : got.polylines)
                    CHECK((line.size() == 2 && std::abs(line.points.front().x() - line.points.back().x()) < SCALED_EPSILON));
            states.push_back(library_states.get(*c));
            if (states.back() != nullptr)
                ++ with_state;
        }

        // The same cases again, shuffled over 8 threads, each case on a filler of its own.
        std::vector<size_t> order(group.size());
        for (size_t i = 0; i < order.size(); ++ i)
            order[i] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937(20261104));
        std::vector<Output>      got(group.size());
        std::atomic<size_t>      next { 0 };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++ t)
            threads.emplace_back([&] {
                for (size_t i; (i = next++) < order.size();) {
                    const Case &c = *group[order[i]];
                    const std::unique_ptr<Fill> filler(Fill::new_from_type(c.pattern));
                    apply_members(c, *filler);
                    filler->object_state = states[order[i]];
                    got[order[i]] = run(c, *filler);
                }
            });
        for (std::thread &thread : threads)
            thread.join();
        for (size_t i = 0; i < group.size(); ++ i) {
            CAPTURE(group[i]->name);
            CHECK(difference(*group[i], expected[i], got[i]).empty());
        }

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
        // The states prepared again with the loops split into single items and into blocks of 7.
        for (const int64_t grain : { 1, 7 }) {
            CAPTURE(grain);
            library_states.clear();
            const size_t blocks = SplitLoops::blocks();
            for (size_t i = 0; i < group.size(); ++ i) {
                const Case &c = *group[i];
                if (c.state == State::None)
                    continue;
                CAPTURE(c.name);
                {
                    const SplitLoops split(c.pattern, grain);
                    library_states.get(c);
                }
                CHECK(difference(c, expected[i], run_library(c, library_states)).empty());
            }
            CHECK(SplitLoops::blocks() > blocks);
        }
#endif
        library_states.clear();
    }
    // Every library row has cases of its own, and the libraries prepared states for some.
    for (const InfillPatternLibraryRow &row : InfillPatternRegistry::library_rows()) {
        CAPTURE(row.name);
        CHECK(per_row[row.pattern] > 0);
    }
    CHECK(with_state > 0);
}

TEST_CASE("Pattern libraries prepare the adaptive infill field of their row", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    set_scaling_factor(SCALING_FACTOR_INTERNAL);
    REQUIRE(InfillPatternRegistry::bound(ipGyroid));
    NativeStates  native_states;
    LibraryStates library_states(native_states);
    const orca_infill_builtin_library &library = *InfillPatternRegistry::binding(ipGyroid)->library;

    // A field to build is a state of the library; an empty one is none.
    const InfillObjectStatePtr field = library_states.prepare_field(ipGyroid, AdaptiveInfillMode::Lobes);
    REQUIRE(field != nullptr);
    CHECK(field->library == &library);
    CHECK(field->raw != nullptr);
    {
        const ExPolygons                              sliver { ExPolygon(Points{ { 0, 0 }, { scaled(5.), 0 }, { scaled(5.), scaled(0.1) } }) };
        const std::vector<AdaptiveInfillField::Slice> slices { { 0., 0.2, &sliver } };
        REQUIRE(AdaptiveInfillField(slices, get_extents(sliver), AdaptiveInfillMode::Lobes, [] {}).empty());
        const BuiltinMirror::FieldInput               input(slices, get_extents(sliver));
        const orca_infill_field_input                 thin = input.get(AdaptiveInfillMode::Lobes);
        CHECK(prepare_builtin_infill_state(ipGyroid, ORCA_INFILL_STATE_ADAPTIVE_FIELD, &thin, {}) == nullptr);
    }

    check_canceled([&library_states](const std::function<void()> &throw_if_canceled) {
        return library_states.prepare_field(ipGyroid, AdaptiveInfillMode::DistanceWarp, throw_if_canceled);
    }, 3);

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
    // A failed preparation names the library and the reason it gives.
    {
        const SplitLoops failing(ipGyroid, 1, ORCA_INFILL_ERR_EXCEPTION);
        CHECK_THROWS_WITH(library_states.prepare_field(ipGyroid, AdaptiveInfillMode::Lobes),
                          ContainsSubstring(InfillPatternRegistry::binding(ipGyroid)->file) &&
                              ContainsSubstring("The parallel loop of an infill pattern library failed"));
    }
#endif

    // The library refuses an input of another row, kind or mode, and prepares without a job on the calling thread.
    const BuiltinMirror::FieldInput input(native_states.field_slices(), native_states.field_bbox());
    const orca_infill_field_input   lobes = input.get(AdaptiveInfillMode::Lobes), disabled = input.get(AdaptiveInfillMode::Disabled);
    const orca_infill_state_input   state_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_ADAPTIVE_FIELD, &lobes };
    const orca_infill_state_input   octree_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_OCTREE, &lobes };
    const orca_infill_state_input   disabled_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_ADAPTIVE_FIELD, &disabled };
    int   stale = 0;
    void *raw   = &stale;
    CHECK(library.prepare_state(int32_t(ipTpmsD), &state_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(raw == nullptr);
    CHECK(library.prepare_state(int32_t(ipGyroid), &octree_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.prepare_state(int32_t(ipGyroid), &disabled_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.prepare_state(int32_t(ipGyroid), nullptr, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.prepare_state(int32_t(ipGyroid), &state_input, nullptr, nullptr) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.query_state == nullptr);
    REQUIRE(library.prepare_state(int32_t(ipGyroid), &state_input, nullptr, &raw) == ORCA_INFILL_OK);
    REQUIRE(raw != nullptr);
    const InfillObjectState serial(library, raw);

    // Either field grades the infill as the native field does.
    const Case &c = *std::find_if(cases().begin(), cases().end(), [](const Case &c) {
        return c.pattern == ipGyroid && c.state == State::Field && c.params.adaptive_infill_mode == AdaptiveInfillMode::Lobes &&
               c.scaling_factor == SCALING_FACTOR_INTERNAL && c.call == Call::FillSurface;
    });
    CAPTURE(c.name);
    const Output expected = run_native(c, native_states);
    REQUIRE(! expected.polylines.empty());
    for (const InfillObjectState *state : { field.get(), &serial }) {
        const std::unique_ptr<Fill> filler(Fill::new_from_type(ipGyroid));
        apply_members(c, *filler);
        filler->object_state = state;
        CHECK(difference(c, expected, run(c, *filler)).empty());
    }

    // So does the copy of the library linked into the executable, as ORCA_INFILL_PATTERN_LINKAGE=STATIC links it.
    CHECK(difference(c, expected, run_linked(c, prepare_linked(ipGyroid, state_input))).empty());
    // Without its field the row lays its regular pattern, as the native class does.
    Case regular = c;
    regular.state = State::None;
    const Output plain = run_native(regular, native_states);
    CHECK_FALSE(difference(c, expected, plain).empty());
    CHECK(difference(regular, plain, run_library(regular, library_states)).empty());
}

TEST_CASE("Pattern libraries prepare the octree of their row", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    set_scaling_factor(SCALING_FACTOR_INTERNAL);
    REQUIRE(InfillPatternRegistry::bound(ipAdaptiveCubic));
    NativeStates  native_states;
    LibraryStates library_states(native_states);
    const orca_infill_builtin_library &library = *InfillPatternRegistry::binding(ipAdaptiveCubic)->library;
    const Case &c = *std::find_if(cases().begin(), cases().end(), [](const Case &c) {
        return c.pattern == ipAdaptiveCubic && c.scaling_factor == SCALING_FACTOR_INTERNAL && c.call == Call::FillSurface;
    });
    CAPTURE(c.name);
    auto run_on = [&c](const InfillObjectState *state) {
        const std::unique_ptr<Fill> filler(Fill::new_from_type(ipAdaptiveCubic));
        apply_members(c, *filler);
        filler->object_state = state;
        return run(c, *filler);
    };

    // The library fills on the octrees it builds as the class does on the native ones. The overhangs and the support flag
    // reach it: the octree densified below the overhang lays other lines.
    Case support  = c;
    support.state = State::SupportOctree;
    const Output expected = run_native(c, native_states), expected_support = run_native(support, native_states);
    REQUIRE(! expected.polylines.empty());
    CHECK_FALSE(difference(c, expected, expected_support).empty());
    const InfillObjectStatePtr octree         = library_states.prepare_octree(ipAdaptiveCubic, State::AdaptiveOctree);
    const InfillObjectStatePtr support_octree = library_states.prepare_octree(ipAdaptiveCubic, State::SupportOctree);
    REQUIRE(octree != nullptr);
    REQUIRE(support_octree != nullptr);
    CHECK(octree->library == &library);
    CHECK(difference(c, expected, run_on(octree.get())).empty());
    CHECK(difference(support, expected_support, run_on(support_octree.get())).empty());

    // A filler reaches only the octrees of its own row.
    RegionOctrees octrees;
    octrees.sets.push_back({ ipAdaptiveCubic, octree, {} });
    octrees.region_set = { -1, 0 };
    CHECK(octrees.region(0, ipAdaptiveCubic) == nullptr);
    CHECK(octrees.region(1, ipSupportCubic) == nullptr);
    CHECK(octrees.region(2, ipAdaptiveCubic) == nullptr);
    REQUIRE(octrees.region(1, ipAdaptiveCubic) != nullptr);
    CHECK(octrees.region(1, ipAdaptiveCubic)->get(3) == octree.get());

    // The library refuses an input of another row or kind, no input and a line spacing that is not positive, and
    // prepares without a job on the calling thread.
    const orca_infill_octree_input input = LibraryStates::octree_input(native_states, State::AdaptiveOctree);
    const orca_infill_state_input  state_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_OCTREE, &input };
    const orca_infill_state_input  field_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_ADAPTIVE_FIELD, &input };
    int   stale = 0;
    void *raw   = &stale;
    CHECK(library.prepare_state(int32_t(ipSupportCubic), &state_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(raw == nullptr);
    CHECK(library.prepare_state(int32_t(ipAdaptiveCubic), &field_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.prepare_state(int32_t(ipAdaptiveCubic), nullptr, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.prepare_state(int32_t(ipAdaptiveCubic), &state_input, nullptr, nullptr) == ORCA_INFILL_ERR_INVALID);
    for (const double spacing : { 0., -2.2, std::nan("") }) {
        CAPTURE(spacing);
        orca_infill_octree_input      bad = input;
        bad.line_spacing                  = spacing;
        const orca_infill_state_input bad_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_OCTREE, &bad };
        CHECK(library.prepare_state(int32_t(ipAdaptiveCubic), &bad_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
        CHECK(raw == nullptr);
    }
    CHECK(library.query_state == nullptr);
    REQUIRE(library.prepare_state(int32_t(ipAdaptiveCubic), &state_input, nullptr, &raw) == ORCA_INFILL_OK);
    REQUIRE(raw != nullptr);
    const InfillObjectState serial(library, raw);
    CHECK(difference(c, expected, run_on(&serial)).empty());

    // So does the copy of the library linked into the executable, as ORCA_INFILL_PATTERN_LINKAGE=STATIC links it.
    CHECK(difference(c, expected, run_linked(c, prepare_linked(ipAdaptiveCubic, state_input))).empty());
}

TEST_CASE("Pattern libraries prepare the lightning generator of their row", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    set_scaling_factor(SCALING_FACTOR_INTERNAL);
    REQUIRE(InfillPatternRegistry::bound(ipLightning));
    NativeStates  native_states;
    LibraryStates library_states(native_states);
    const orca_infill_builtin_library &library = *InfillPatternRegistry::binding(ipLightning)->library;
    const Case &c = *std::find_if(cases().begin(), cases().end(), [](const Case &c) {
        return c.pattern == ipLightning && c.state == State::Lightning && c.scaling_factor == SCALING_FACTOR_INTERNAL &&
               c.call == Call::FillSurface;
    });
    CAPTURE(c.name);
    auto run_on = [&c](const InfillObjectState *state) {
        const std::unique_ptr<Fill> filler(Fill::new_from_type(ipLightning));
        apply_members(c, *filler);
        filler->object_state = state;
        return run(c, *filler);
    };

    // The library fills on the generator it builds as the class does on the native one.
    const Output expected = run_native(c, native_states);
    REQUIRE(! expected.polylines.empty());
    const InfillObjectStatePtr generator = library_states.prepare_lightning(ipLightning);
    REQUIRE(generator != nullptr);
    CHECK(generator->library == &library);
    CHECK(difference(c, expected, run_on(generator.get())).empty());

    // A filler draws only the trees of a generator: without one, the call fails.
    {
        const std::unique_ptr<Fill> filler(Fill::new_from_type(ipLightning));
        apply_members(c, *filler);
        CHECK_THROWS_AS(filler->fill_surface(&c.surface, c.params), SlicingError);
    }

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
    // Both loops of the generator, over the samples and over the grid around a sample, may be split in any way.
    check_split_lightning([&library_states] { return library_states.prepare_lightning(ipLightning); },
                          [&](const InfillObjectState &generator) { CHECK(difference(c, expected, run_on(&generator)).empty()); });
#endif

    check_canceled([&library_states](const std::function<void()> &throw_if_canceled) {
        return library_states.prepare_lightning(ipLightning, throw_if_canceled);
    }, 5);

    // The library refuses an input of another row, a tree support input without overhangs, no input, an input without
    // layers, with overhangs or with a supporting radius that is not positive, and prepares without a job on the calling
    // thread.
    const NativeStates::LightningInput  native = native_states.lightning_input();
    const BuiltinMirror::LightningInput input(native.params, native.outlines);
    const orca_infill_state_input       state_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING, &input.get() };
    const orca_infill_state_input       support_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING_SUPPORT, &input.get() };
    int   stale = 0;
    void *raw   = &stale;
    CHECK(library.prepare_state(int32_t(ipGyroid), &state_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(raw == nullptr);
    CHECK(library.prepare_state(int32_t(ipLightning), &support_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.prepare_state(int32_t(ipLightning), nullptr, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.prepare_state(int32_t(ipLightning), &state_input, nullptr, nullptr) == ORCA_INFILL_ERR_INVALID);
    {
        const BuiltinMirror::LightningInput with_overhangs(native.params, native.outlines, &native.outlines);
        const orca_infill_state_input       bad { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING, &with_overhangs.get() };
        CHECK(library.prepare_state(int32_t(ipLightning), &bad, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
        CHECK(raw == nullptr);
    }
    {
        const std::vector<Polygons>         none;
        const BuiltinMirror::LightningInput no_layers(native.params, none);
        const orca_infill_state_input       bad { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING, &no_layers.get() };
        CHECK(library.prepare_state(int32_t(ipLightning), &bad, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
        CHECK(raw == nullptr);
    }
    for (const coord_t radius : { coord_t(0), coord_t(-scaled<coord_t>(2.)) }) {
        CAPTURE(radius);
        FillLightning::GeneratorParams params = native.params;
        params.supporting_radius              = radius;
        const BuiltinMirror::LightningInput bad_radius(params, native.outlines);
        const orca_infill_state_input       bad { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING, &bad_radius.get() };
        CHECK(library.prepare_state(int32_t(ipLightning), &bad, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
        CHECK(raw == nullptr);
    }
    REQUIRE(library.prepare_state(int32_t(ipLightning), &state_input, nullptr, &raw) == ORCA_INFILL_OK);
    REQUIRE(raw != nullptr);
    const InfillObjectState serial(library, raw);
    CHECK(difference(c, expected, run_on(&serial)).empty());

    // So does the copy of the library linked into the executable, as ORCA_INFILL_PATTERN_LINKAGE=STATIC links it.
    CHECK(difference(c, expected, run_linked(c, prepare_linked(ipLightning, state_input))).empty());
}

namespace {

// The lightning library with a part taken away, as the entry of a library for check_builtin_infill_entry().
struct ChangedLightningLibrary
{
    static inline BuiltinInfillEntry             entry = nullptr;
    static inline orca_infill_builtin_library    library {};
    static inline orca_infill_builtin_row        row {};
    static inline const orca_infill_builtin_row *rows[1] {};

    static const orca_infill_builtin_library* without_query(const orca_infill_builtin_host *host)
    {
        library             = *entry(host);
        library.query_state = nullptr;
        return &library;
    }
    static const orca_infill_builtin_library* without_support(const orca_infill_builtin_host *host)
    {
        library = *entry(host);
        row     = *library.rows[0];
        row.state_kinds &= ~(1u << ORCA_INFILL_STATE_LIGHTNING_SUPPORT);
        rows[0]      = &row;
        library.rows = rows;
        return &library;
    }
};

} // namespace

TEST_CASE("Pattern libraries prepare the lightning generator of tree support and answer its lines", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    REQUIRE(InfillPatternRegistry::bound(ipLightning));
    const InfillPatternBinding        &binding = *InfillPatternRegistry::binding(ipLightning);
    const orca_infill_builtin_library &library = *binding.library;
    REQUIRE(library.query_state != nullptr);
    NativeStates native_states;

    // The queries of every layer: within its contours, within the contours shrunk as tree support shrinks its base areas,
    // within the half of them left of their centre and within nothing; with no overlap and with one.
    struct Query
    {
        size_t   layer;
        Polygons area;
        coord_t  overlap;
    };
    auto queries_of = [](const std::vector<Polygons> &contours) {
        std::vector<Query> out;
        for (size_t layer = 0; layer < contours.size(); ++ layer) {
            const BoundingBox box = get_extents(contours[layer]);
            const Polygons    half = box.defined ?
                intersection(contours[layer], Polygons { BoundingBox(box.min, Point(box.center().x(), box.max.y())).polygon() }) : Polygons();
            for (const Polygons &area : { contours[layer], offset(contours[layer], -scaled<float>(0.4)), half, Polygons() })
                for (const coord_t overlap : { coord_t(0), scaled<coord_t>(0.2) })
                    out.push_back({ layer, area, overlap });
        }
        return out;
    };
    auto query = [](const InfillObjectState &state, const Query &q) {
        return query_builtin_infill_state(ipLightning, state, ORCA_INFILL_QUERY_LIGHTNING_LINES, q.layer, q.area, q.overlap);
    };

    size_t lines = 0;
    for (const double scaling_factor : { SCALING_FACTOR_INTERNAL, SCALING_FACTOR_INTERNAL_LARGE_PRINTER }) {
        CAPTURE(scaling_factor);
        set_scaling_factor(scaling_factor);
        const NativeStates::LightningSupportInput native = native_states.lightning_support_input();
        const FillLightning::Generator           &native_generator = *native_states.lightning_support();
        const BuiltinMirror::LightningInput       input(native.params, native.contours, &native.overhangs);
        auto prepare = [&input](const std::function<void()> &throw_if_canceled = {}) {
            return prepare_builtin_infill_state(ipLightning, ORCA_INFILL_STATE_LIGHTNING_SUPPORT, &input.get(), throw_if_canceled);
        };

        // The library answers every query on the generator it builds as the native generator does.
        const std::vector<Query> queries = queries_of(native.contours);
        REQUIRE(native_generator.layer_count() == native.contours.size());
        std::vector<Polylines> expected;
        for (const Query &q : queries) {
            expected.emplace_back(native_generator.getTreesForLayer(q.layer).convertToLines(q.area, q.overlap));
            lines += expected.back().size();
        }
        const InfillObjectStatePtr generator = prepare();
        REQUIRE(generator != nullptr);
        CHECK(generator->library == &library);
        for (size_t i = 0; i < queries.size(); ++ i) {
            CAPTURE(i, queries[i].layer);
            CHECK(same_lines(expected[i], query(*generator, queries[i])));
        }

        // The same queries again, shuffled over 8 threads that share the generator.
        std::vector<size_t> order(queries.size());
        for (size_t i = 0; i < order.size(); ++ i)
            order[i] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937(20261105));
        std::vector<Polylines>   got(queries.size());
        std::atomic<size_t>      next { 0 };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++ t)
            threads.emplace_back([&] {
                for (size_t i; (i = next++) < order.size();)
                    got[order[i]] = query(*generator, queries[order[i]]);
            });
        for (std::thread &thread : threads)
            thread.join();
        for (size_t i = 0; i < queries.size(); ++ i) {
            CAPTURE(i, queries[i].layer);
            CHECK(same_lines(expected[i], got[i]));
        }

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
        // Both loops of the generator, over the samples and over the grid around a sample, may be split in any way.
        check_split_lightning(prepare, [&](const InfillObjectState &split_generator) {
            for (size_t i = 0; i < queries.size(); ++ i) {
                CAPTURE(i, queries[i].layer);
                CHECK(same_lines(expected[i], query(split_generator, queries[i])));
            }
        });
#endif

        check_canceled(prepare, 5);
    }
    // The trees reach the queries.
    CHECK(lines > 0);

    set_scaling_factor(SCALING_FACTOR_INTERNAL);
    const NativeStates::LightningSupportInput native = native_states.lightning_support_input();
    const BuiltinMirror::LightningInput       input(native.params, native.contours, &native.overhangs);
    const orca_infill_state_input             state_input { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING_SUPPORT, &input.get() };
    const std::vector<Query>                  queries = queries_of(native.contours);
    const Query                              &q = *std::max_element(queries.begin(), queries.end(), [&native_states](const Query &a, const Query &b) {
        return native_states.lightning_support()->getTreesForLayer(a.layer).convertToLines(a.area, a.overlap).size() <
               native_states.lightning_support()->getTreesForLayer(b.layer).convertToLines(b.area, b.overlap).size();
    });
    const Polylines expected = native_states.lightning_support()->getTreesForLayer(q.layer).convertToLines(q.area, q.overlap);
    REQUIRE(! expected.empty());

    // The library refuses an input of another row, one without overhangs and a supporting radius that is not positive.
    int   stale = 0;
    void *raw   = &stale;
    CHECK(library.prepare_state(int32_t(ipGyroid), &state_input, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
    CHECK(raw == nullptr);
    {
        const BuiltinMirror::LightningInput no_overhangs(native.params, native.contours);
        const orca_infill_state_input       bad { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING_SUPPORT, &no_overhangs.get() };
        CHECK(library.prepare_state(int32_t(ipLightning), &bad, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
        CHECK(raw == nullptr);
    }
    for (const coord_t radius : { coord_t(0), coord_t(-scaled<coord_t>(2.)) }) {
        CAPTURE(radius);
        FillLightning::GeneratorParams params = native.params;
        params.supporting_radius              = radius;
        const BuiltinMirror::LightningInput bad_radius(params, native.contours, &native.overhangs);
        const orca_infill_state_input       bad { sizeof(orca_infill_state_input), ORCA_INFILL_STATE_LIGHTNING_SUPPORT, &bad_radius.get() };
        CHECK(library.prepare_state(int32_t(ipLightning), &bad, nullptr, &raw) == ORCA_INFILL_ERR_INVALID);
        CHECK(raw == nullptr);
    }

    // Without layers, as for tree support without base areas, it prepares a generator that answers no layer.
    {
        const std::vector<Polygons>         none;
        const BuiltinMirror::LightningInput no_layers(native.params, none, &none);
        const InfillObjectStatePtr          empty = prepare_builtin_infill_state(ipLightning, ORCA_INFILL_STATE_LIGHTNING_SUPPORT,
                                                                                 &no_layers.get(), {});
        REQUIRE(empty != nullptr);
        CHECK_THROWS_AS(query(*empty, { 0, q.area, 0 }), SlicingError);
    }

    // It prepares without a job on the calling thread, and answers the lines query of a layer of the generator into a
    // sink, nothing else.
    REQUIRE(library.prepare_state(int32_t(ipLightning), &state_input, nullptr, &raw) == ORCA_INFILL_OK);
    REQUIRE(raw != nullptr);
    const InfillObjectState serial(library, raw);
    CHECK(same_lines(expected, query(serial, q)));
    CHECK_THROWS_WITH(query(serial, { native.contours.size(), q.area, 0 }),
                      ContainsSubstring("lightning") && ContainsSubstring(binding.file) && ContainsSubstring("error"));
    const BuiltinMirror::StateQuery lines_query(ORCA_INFILL_QUERY_LIGHTNING_LINES, q.layer, q.area, q.overlap);
    {
        BuiltinInfillOutput out;
        CHECK(library.query_state(raw, &lines_query.get(), out.sink()) == ORCA_INFILL_OK);
        CHECK(same_lines(expected, out.polylines));
    }
    BuiltinInfillOutput out;
    CHECK(library.query_state(raw, &lines_query.get(), nullptr) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.query_state(nullptr, &lines_query.get(), out.sink()) == ORCA_INFILL_ERR_INVALID);
    CHECK(library.query_state(raw, nullptr, out.sink()) == ORCA_INFILL_ERR_INVALID);
    const BuiltinMirror::StateQuery other(ORCA_INFILL_QUERY_LIGHTNING_LINES + 1, q.layer, q.area, q.overlap);
    CHECK(library.query_state(raw, &other.get(), out.sink()) == ORCA_INFILL_ERR_INVALID);
    orca_infill_state_query shorter = lines_query.get();
    shorter.struct_size             = uint32_t(offsetof(orca_infill_state_query, overlap));
    CHECK(library.query_state(raw, &shorter, out.sink()) == ORCA_INFILL_ERR_INVALID);
    CHECK(out.polylines.empty());

    // So does the copy of the library linked into the executable, as ORCA_INFILL_PATTERN_LINKAGE=STATIC links it.
    CHECK(same_lines(expected, query(*prepare_linked(ipLightning, state_input).state, q)));

    // The loader refuses a lightning library that does not answer the lines or prepare the generator of tree support.
    for (const BuiltinInfillLoad &bound : builtin_infill_pattern_loads())
        if (bound.pattern == ipLightning)
            ChangedLightningLibrary::entry = bound.entry;
    REQUIRE(ChangedLightningLibrary::entry != nullptr);
    CHECK(check_builtin_infill_entry(&ChangedLightningLibrary::without_query, ipLightning).error == "is incomplete");
    CHECK(check_builtin_infill_entry(&ChangedLightningLibrary::without_support, ipLightning).error ==
          "prepares other object states for its pattern");
}

TEST_CASE("Pattern library fillers keep their state between calls", "[InfillPatterns][BuiltinEquivalence]")
{
    // The slicer reuses a filler: make_fills() for every island of a surface fill, the supports for every layer one
    // thread fills, ironing for every region of a layer. A library holds one instance of the class per filler, so that
    // what an instance keeps from one call to the next carries over as in the class.
    ScalingFactorGuard guard;
    set_scaling_factor(SCALING_FACTOR_INTERNAL);
    NativeStates  native_states;
    LibraryStates library_states(native_states);
    // Runs c with its object state on the reused native and library fillers, checks that they agree and returns the
    // output of the class.
    auto reuse = [&](const Case &c, Fill &native, Fill &served) {
        CAPTURE(c.name);
        apply_members(c, native);
        native.adapt_fill_octree     = nullptr;
        native.adaptive_infill_field = nullptr;
        native_states.attach(c, native);
        apply_members(c, served);
        served.object_state = library_states.get(c);
        Output expected = run(c, native);
        CHECK(difference(c, expected, run(c, served)).empty());
        return expected;
    };
    for (const InfillPatternLibraryRow &row : InfillPatternRegistry::library_rows()) {
        CAPTURE(row.name);
        const std::unique_ptr<Fill> native = native_filler(row.pattern), served(Fill::new_from_type(row.pattern));
        for (const Case &c : cases())
            if (c.pattern == row.pattern && c.scaling_factor == SCALING_FACTOR_INTERNAL)
                reuse(c, *native, *served);
    }

    // Honeycomb caches its cell by density and spacing only, so a call with two lines per cell side after one with a
    // single line reuses the cell of the first call.
    if (InfillPatternRegistry::bound(ipHoneycomb)) {
        const Case &one = *std::find_if(cases().begin(), cases().end(),
                                        [](const Case &c) { return c.name == "honeycomb/base"; });
        Case two = one;
        two.params.multiline = 2;
        const std::unique_ptr<Fill> native = native_filler(ipHoneycomb), served(Fill::new_from_type(ipHoneycomb));
        reuse(one, *native, *served);
        const Output reused = reuse(two, *native, *served);
        // A filler of its own computes the cell of two lines.
        CHECK(! difference(two, run_native(two, native_states), reused).empty());
    }
}

namespace {

// Locked Zag whose skin fails to fill: fill_surface_locked_zag() fills the skin with locked_zag cleared, after the
// skeleton.
class FailingSkin : public FillLockedZag
{
public:
    Fill* clone() const override { return new FailingSkin(*this); }
    Polylines fill_surface(const Surface *surface, const FillParams &params) override
    {
        if (! params.locked_zag)
            throw InfillFailedException();
        return FillLockedZag::fill_surface(surface, params);
    }
};

struct FailingSkinRow
{
    using Filler = FailingSkin;
    static constexpr int32_t     pattern     = int32_t(ipLockedZag);
    static constexpr const char *key         = "lockedzag";
    static constexpr const char *name        = "orca_infill_lockedzag";
    static constexpr const char *build_id    = ORCA_INFILL_BUILD_ID;
    static constexpr uint32_t    calls       = (1u << ORCA_INFILL_CALL_FILL_SURFACE) | (1u << ORCA_INFILL_CALL_LOCKED_ZAG);
    static constexpr uint32_t    traits      = 0;
    static constexpr uint32_t    state_kinds = 0;
};

} // namespace

TEST_CASE("Locked Zag keeps the lines generated before a fill fails, as its class does", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    const orca_infill_builtin_library *library = PatternLibrary::PatternEntry<FailingSkinRow>::bind(&builtin_infill_host());
    REQUIRE(library != nullptr);
    size_t partial = 0;
    for (const Case &c : cases()) {
        if (c.pattern != ipLockedZag || (c.call != Call::LockedZag && c.call != Call::Extrusion))
            continue;
        CAPTURE(c.name);
        set_scaling_factor(c.scaling_factor);
        FailingSkin native;
        apply_members(c, native);
        const Output expected = run(c, native);
        FillLibrary served(ipLockedZag, *library, "orca_infill_lockedzag");
        apply_members(c, served);
        CHECK(difference(c, expected, run(c, served)).empty());
        if (c.call == Call::LockedZag && expected.error == Output::Error::InfillFailed && ! expected.flow_groups.empty())
            ++ partial;
    }
    CHECK(partial > 0);
}

TEST_CASE("Pattern library statuses become the exceptions of the Fill classes", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    const Case &c = *std::find_if(cases().begin(), cases().end(), [](const Case &c) {
        return c.pattern == ipRectilinear && c.call == Call::FillSurface && c.scaling_factor == SCALING_FACTOR_INTERNAL;
    });
    set_scaling_factor(c.scaling_factor);
    const orca_infill_builtin_library &library = *InfillPatternRegistry::binding(ipRectilinear)->library;

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
    // A library called under another scaling factor than the application's refuses the call. A linked library shares
    // the application's.
    library.set_scaling_factor(SCALING_FACTOR_INTERNAL_LARGE_PRINTER);
    {
        const std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
        apply_members(c, *filler);
        CHECK_THROWS_WITH(filler->fill_surface(&c.surface, c.params), ContainsSubstring("another scaling factor"));
    }
    set_scaling_factor(c.scaling_factor);
#endif

    // A call without a sink is invalid; the instance is unchanged and still works.
    NativeStates                native_states;
    const std::unique_ptr<Fill> native = native_filler(ipRectilinear);
    apply_members(c, *native);
    const BuiltinMirror::Request request(ORCA_INFILL_CALL_FILL_SURFACE, c.surface, c.params, *native);
    void *instance = library.create(int32_t(ipRectilinear));
    REQUIRE(instance != nullptr);
    CHECK(library.run(instance, &request.get(), nullptr) == ORCA_INFILL_ERR_INVALID);
    BuiltinInfillOutput out;
    CHECK(library.run(instance, &request.get(), out.sink()) == ORCA_INFILL_OK);
    library.destroy(instance);
    const Output expected = run_native(c, native_states);
    REQUIRE(out.polylines.size() == expected.polylines.size());
    for (size_t i = 0; i < out.polylines.size(); ++ i)
        CHECK(same_points(out.polylines[i].points, expected.polylines[i].points));
    REQUIRE(out.spacing.has_value());
    CHECK(same_bits(*out.spacing, expected.spacing));

    // An instance is only made for the row of the library.
    CHECK(library.create(int32_t(ipGyroid)) == nullptr);
}
