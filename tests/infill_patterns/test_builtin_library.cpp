// The built-in pattern libraries of src/infill_patterns, called through their entry like the application calls them:
// each one serves its row and reproduces its native filler bit for bit.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/Library/BuiltinMirror.hpp"
#include "libslic3r/Fill/Library/orca_infill_builtin.h"
#include "libslic3r/Utils.hpp"
#include "libslic3r_infill_patterns.h"
#include "orca_infill_build_id.h"
#include "orca_infill_manifest.h"

#include "corpus.hpp"
#include "infill_pattern_libraries.h"

using namespace Slic3r;

namespace {

// The output of one run(), behind the host's opaque sink.
struct Output
{
    Polylines      polylines;
    ThickPolylines thick_polylines;
    double         spacing { -1. };
    std::string    error;
    std::vector<std::string> log;
};

Output& output(orca_infill_sink *sink) { return *reinterpret_cast<Output*>(sink); }

orca_infill_status ORCA_INFILL_CALL emit_polyline(orca_infill_sink *sink, const orca_infill_point *points, size_t count)
{
    Polyline polyline;
    polyline.points = BuiltinMirror::to_points(points, count);
    output(sink).polylines.emplace_back(std::move(polyline));
    return ORCA_INFILL_OK;
}

orca_infill_status ORCA_INFILL_CALL emit_thick_polyline(orca_infill_sink *sink, const orca_infill_thick_path *path)
{
    output(sink).thick_polylines.emplace_back(BuiltinMirror::to_thick_polyline(*path));
    return ORCA_INFILL_OK;
}

orca_infill_status ORCA_INFILL_CALL begin_group(orca_infill_sink *, size_t) { return ORCA_INFILL_ERR_INVALID; }
void ORCA_INFILL_CALL set_spacing(orca_infill_sink *sink, double spacing) { output(sink).spacing = spacing; }
void ORCA_INFILL_CALL set_error(orca_infill_sink *sink, const char *message) { output(sink).error = message; }

std::vector<std::string> g_log;
int32_t ORCA_INFILL_CALL host_log_enabled(int32_t) { return 1; }
void ORCA_INFILL_CALL host_log(int32_t, const char *message) { g_log.emplace_back(message); }

orca_infill_builtin_host make_host(const char *build_id)
{
    return { sizeof(orca_infill_builtin_host), ORCA_INFILL_BUILTIN_ABI_MAJOR, ORCA_INFILL_BUILTIN_ABI_MINOR, build_id,
             &emit_polyline, &emit_thick_polyline, &begin_group, &set_spacing, &set_error, &host_log_enabled, &host_log,
             nullptr, nullptr, nullptr };
}

const orca_infill_builtin_host g_host = make_host(ORCA_INFILL_BUILD_ID);

} // namespace

#if ORCA_INFILL_PATTERN_LINKAGE_STATIC
#define ORCA_INFILL_ENTRY(row, token, file, entry) \
    extern "C" const orca_infill_builtin_library *ORCA_INFILL_CALL entry(const orca_infill_builtin_host *host);
ORCA_INFILL_MANIFEST(ORCA_INFILL_ENTRY)
#undef ORCA_INFILL_ENTRY
#endif

namespace {

// The entry of the library of pattern, named file_stem; the library stays loaded.
orca_infill_builtin_entry_v1_fn entry_of(InfillPattern pattern, const std::string &file_stem)
{
#if ORCA_INFILL_PATTERN_LINKAGE_STATIC
#define ORCA_INFILL_ENTRY(row, token, file, entry) if (pattern == row) return &entry;
    ORCA_INFILL_MANIFEST(ORCA_INFILL_ENTRY)
#undef ORCA_INFILL_ENTRY
    (void) file_stem;
    return nullptr;
#else
    (void) pattern;
    const std::string path = std::string(ORCA_TEST_INFILL_PATTERN_DIR) + "/" + file_stem + ORCA_TEST_INFILL_PATTERN_SUFFIX;
#ifdef _WIN32
    HMODULE library = ::LoadLibraryA(path.c_str());
    INFO(path);
    REQUIRE(library != nullptr);
    return reinterpret_cast<orca_infill_builtin_entry_v1_fn>(::GetProcAddress(library, ORCA_INFILL_BUILTIN_ENTRY_V1));
#else
    void *library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    INFO(path << ": " << (library == nullptr ? ::dlerror() : ""));
    REQUIRE(library != nullptr);
    return reinterpret_cast<orca_infill_builtin_entry_v1_fn>(::dlsym(library, ORCA_INFILL_BUILTIN_ENTRY_V1));
#endif
#endif
}

bool same_points(const Points &a, const Points &b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Point)) == 0);
}

struct Pilot
{
    InfillPattern pattern;
    const char   *token;
};

} // namespace

TEST_CASE("A pattern library binds only to the build that made it", "[InfillPatterns][BuiltinLibrary]")
{
    for (const Pilot pilot : { Pilot { ipRectilinear, "rectilinear" }, Pilot { ipCrossHatch, "crosshatch" } }) {
        const std::string name = std::string("orca_infill_") + pilot.token;
        orca_infill_builtin_entry_v1_fn entry = entry_of(pilot.pattern, name);
        REQUIRE(entry != nullptr);

        g_log.clear();
        const orca_infill_builtin_host other = make_host("another build");
        CHECK(entry(&other) == nullptr);
        REQUIRE(g_log.size() == 1);
        CHECK(g_log.front().find(name) != std::string::npos);

        const orca_infill_builtin_library *library = entry(&g_host);
        REQUIRE(library != nullptr);
        CHECK(library->abi_major == ORCA_INFILL_BUILTIN_ABI_MAJOR);
        CHECK(std::string(library->build_id) == ORCA_INFILL_BUILD_ID);
        CHECK(std::string(library->name) == name);
        REQUIRE(library->row_count == 1);
        const orca_infill_builtin_row &row = *library->rows[0];
        CHECK(row.pattern == int32_t(pilot.pattern));
        CHECK(std::string(row.key) == pilot.token);
        CHECK(row.calls == (1u << ORCA_INFILL_CALL_FILL_SURFACE));
        CHECK(row.traits == 0);
        CHECK(row.state_kinds == 0);
        CHECK(library->prepare_state == nullptr);
        CHECK(library->create(int32_t(ipGyroid)) == nullptr);
    }
}

TEST_CASE("A pattern library reproduces its native filler", "[InfillPatterns][BuiltinLibrary]")
{
    const double saved = SCALING_FACTOR;
    for (const Pilot pilot : { Pilot { ipRectilinear, "rectilinear" }, Pilot { ipCrossHatch, "crosshatch" } }) {
        orca_infill_builtin_entry_v1_fn entry = entry_of(pilot.pattern, std::string("orca_infill_") + pilot.token);
        REQUIRE(entry != nullptr);
        const orca_infill_builtin_library *library = entry(&g_host);
        REQUIRE(library != nullptr);

        size_t cases = 0;
        for (const InfillCorpus::Case &c : InfillCorpus::cases()) {
            if (c.pattern != pilot.pattern || c.call != InfillCorpus::Call::FillSurface || c.state != InfillCorpus::State::None)
                continue;
            INFO(c.name);
            ++ cases;
            set_scaling_factor(c.scaling_factor);
            library->set_scaling_factor(c.scaling_factor);

            std::unique_ptr<Fill> native(Fill::new_from_type(c.pattern));
            InfillCorpus::apply_members(c, *native);
            // The request mirrors the native filler before it runs.
            const BuiltinMirror::Request request(ORCA_INFILL_CALL_FILL_SURFACE, c.surface, c.params, *native,
                                                 c.has_lock ? &c.lock : nullptr);
            void *instance = library->create(int32_t(c.pattern));
            REQUIRE(instance != nullptr);
            Output                         out;
            const orca_infill_status       status = library->run(instance, &request.get(), reinterpret_cast<orca_infill_sink*>(&out));
            const InfillCorpus::Output     expected = InfillCorpus::run(c, *native);
            library->destroy(instance);

            switch (expected.error) {
            case InfillCorpus::Output::Error::None: CHECK(status == ORCA_INFILL_OK); break;
            case InfillCorpus::Output::Error::InfillFailed: CHECK(status == ORCA_INFILL_ERR_FAILED); break;
            case InfillCorpus::Output::Error::BadAlloc: CHECK(status == ORCA_INFILL_ERR_NOMEM); break;
            default: CHECK(status == ORCA_INFILL_ERR_EXCEPTION); break;
            }
            CHECK(std::memcmp(&out.spacing, &expected.spacing, sizeof(double)) == 0);
            REQUIRE(out.polylines.size() == expected.polylines.size());
            for (size_t i = 0; i < out.polylines.size(); ++ i)
                CHECK(same_points(out.polylines[i].points, expected.polylines[i].points));
            CHECK(out.thick_polylines.empty());
        }
        CHECK(cases > 0);

        // A request made under another scaling factor than the library's is refused.
        const InfillCorpus::Case &c = InfillCorpus::cases().front();
        set_scaling_factor(SCALING_FACTOR_INTERNAL);
        library->set_scaling_factor(SCALING_FACTOR_INTERNAL_LARGE_PRINTER);
        std::unique_ptr<Fill> native(Fill::new_from_type(pilot.pattern));
        InfillCorpus::apply_members(c, *native);
        const BuiltinMirror::Request request(ORCA_INFILL_CALL_FILL_SURFACE, c.surface, c.params, *native);
        void  *instance = library->create(int32_t(pilot.pattern));
        Output out;
        CHECK(library->run(instance, &request.get(), reinterpret_cast<orca_infill_sink*>(&out)) == ORCA_INFILL_ERR_STATE);
        CHECK(library->run(instance, &request.get(), nullptr) == ORCA_INFILL_ERR_INVALID);
        library->destroy(instance);
        library->set_scaling_factor(saved);
    }
    set_scaling_factor(saved);
}
