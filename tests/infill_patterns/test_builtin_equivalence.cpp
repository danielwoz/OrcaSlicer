// The built-in pattern libraries against the Fill classes they carry: on every case of the corpus of a row a library
// serves, the filler of the row (Fill::new_from_type(), which runs in the library) returns what the class itself
// returns, bit for bit, under both scaling factors, when the cases run in any order on several threads at once, and
// when one filler runs every case of its row in turn.
// This file is compiled with ORCA_INFILL_PATTERN_MODULE=0, so that it can link an entry of its own (PatternEntry.hpp).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "PatternEntry.hpp"
#include "orca_infill_build_id.h"

#include "libslic3r/Exception.hpp"
#include "libslic3r/Fill/FillLibrary.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"
#include "libslic3r/Fill/InfillPatternLibraries.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/Library/BuiltinMirror.hpp"
#include "libslic3r/Fill/Library/orca_infill_builtin.h"
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

Output run_native(const Case &c)
{
    const std::unique_ptr<Fill> filler = native_filler(c.pattern);
    apply_members(c, *filler);
    return run(c, *filler);
}

Output run_library(const Case &c)
{
    const std::unique_ptr<Fill> filler(Fill::new_from_type(c.pattern));
    REQUIRE(dynamic_cast<FillLibrary*>(filler.get()) != nullptr);
    apply_members(c, *filler);
    return run(c, *filler);
}

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

} // namespace

TEST_CASE("Pattern libraries reproduce the Fill classes they carry", "[InfillPatterns][BuiltinEquivalence]")
{
    ScalingFactorGuard guard;
    const std::map<double, std::vector<const Case*>> groups = library_cases();
    REQUIRE(! groups.empty());
    // Both scaling factors of the corpus reach the libraries.
    CHECK(groups.size() == 2);
    std::map<InfillPattern, size_t> per_row;
    for (const auto &entry : groups) {
        const std::vector<const Case*> &group = entry.second;
        set_scaling_factor(entry.first);
        std::vector<Output> expected;
        expected.reserve(group.size());
        for (const Case *c : group) {
            CAPTURE(c->name);
            // The object states are prepared in the library of their row, which a row with states checks here.
            REQUIRE(c->state == State::None);
            ++ per_row[c->pattern];
            expected.emplace_back(run_native(*c));
            const Output got = run_library(*c);
            CHECK(difference(*c, expected.back(), got).empty());
            // Interlocking takes every line for a segment along y (VoxelUtils.cpp, spreadDotsArea()); the rotation of
            // the lines leaves their ends a few units apart in x.
            if (c->pattern == ipAlignedRectilinear && c->name.find("/interlocking") != std::string::npos)
                for (const Polyline &line : got.polylines)
                    CHECK((line.size() == 2 && std::abs(line.points.front().x() - line.points.back().x()) < SCALED_EPSILON));
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
                    got[order[i]] = run(c, *filler);
                }
            });
        for (std::thread &thread : threads)
            thread.join();
        for (size_t i = 0; i < group.size(); ++ i) {
            CAPTURE(group[i]->name);
            CHECK(difference(*group[i], expected[i], got[i]).empty());
        }
    }
    // Every library row has cases of its own.
    for (const InfillPatternLibraryRow &row : InfillPatternRegistry::library_rows()) {
        CAPTURE(row.name);
        CHECK(per_row[row.pattern] > 0);
    }
}

TEST_CASE("Pattern library fillers keep their state between calls", "[InfillPatterns][BuiltinEquivalence]")
{
    // The slicer reuses a filler: make_fills() for every island of a surface fill, the supports for every layer one
    // thread fills, ironing for every region of a layer. A library holds one instance of the class per filler, so that
    // what an instance keeps from one call to the next carries over as in the class.
    ScalingFactorGuard guard;
    set_scaling_factor(SCALING_FACTOR_INTERNAL);
    // Runs c on the reused native and library fillers, checks that they agree and returns the output of the class.
    auto reuse = [](const Case &c, Fill &native, Fill &served) {
        CAPTURE(c.name);
        apply_members(c, native);
        apply_members(c, served);
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
        CHECK(! difference(two, run_native(two), reused).empty());
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
    const std::unique_ptr<Fill> native = native_filler(ipRectilinear);
    apply_members(c, *native);
    const BuiltinMirror::Request request(ORCA_INFILL_CALL_FILL_SURFACE, c.surface, c.params, *native);
    void *instance = library.create(int32_t(ipRectilinear));
    REQUIRE(instance != nullptr);
    CHECK(library.run(instance, &request.get(), nullptr) == ORCA_INFILL_ERR_INVALID);
    BuiltinInfillOutput out;
    CHECK(library.run(instance, &request.get(), out.sink()) == ORCA_INFILL_OK);
    library.destroy(instance);
    const Output expected = run_native(c);
    REQUIRE(out.polylines.size() == expected.polylines.size());
    for (size_t i = 0; i < out.polylines.size(); ++ i)
        CHECK(same_points(out.polylines[i].points, expected.polylines[i].points));
    REQUIRE(out.spacing.has_value());
    CHECK(same_bits(*out.spacing, expected.spacing));

    // An instance is only made for the row of the library.
    CHECK(library.create(int32_t(ipGyroid)) == nullptr);
}
