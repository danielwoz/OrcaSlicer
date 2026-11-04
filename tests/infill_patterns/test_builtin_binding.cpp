// The binding of the built-in pattern libraries to the rows of the pattern table (Fill/InfillPatternLibraries.hpp): every
// row a library serves is bound to the library of this build, and the loader rejects a library that breaks any rule.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Fill/FillLibrary.hpp"
#include "libslic3r/Fill/InfillPatternLibraries.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/InfillPlugins.hpp"
#include "libslic3r/Fill/Library/orca_infill_builtin.h"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r_infill_patterns.h"
#include "orca_infill_build_id.h"
#include "orca_infill_manifest.h"

#include "infill_pattern_libraries.h"
#include "native_fillers.hpp"

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
#include "infill_pattern_fixtures.h"
#endif

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
namespace fs = boost::filesystem;

namespace {

// The rows of the manifest: row, token and name.
struct LibraryRow
{
    InfillPattern pattern;
    const char   *token;
    const char   *name;
};
const std::vector<LibraryRow> &library_rows()
{
#define ORCA_INFILL_LIBRARY_ROW(row, token, name, file, linked) { row, token, name },
    static const std::vector<LibraryRow> rows { ORCA_INFILL_MANIFEST(ORCA_INFILL_LIBRARY_ROW) };
#undef ORCA_INFILL_LIBRARY_ROW
    return rows;
}

std::string library_file(const std::string &name)
{
#if ORCA_INFILL_PATTERN_LINKAGE_STATIC
    return name;
#else
    return name + ORCA_TEST_INFILL_PATTERN_SUFFIX;
#endif
}

// A folder of its own below the temporary folder, removed with the object.
struct TemporaryFolder
{
    TemporaryFolder() : path(fs::temp_directory_path() / fs::unique_path("orca-infill-patterns-%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TemporaryFolder()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

std::vector<std::string> g_log;
void ORCA_INFILL_CALL capture_log(int32_t, const char *message) { g_log.emplace_back(message); }

} // namespace

TEST_CASE("Every row a pattern library serves is bound to the library of this build", "[InfillPatterns][BuiltinBinding]")
{
    // The first libraries, rectilinear and cross hatch.
    REQUIRE(InfillPatternRegistry::binding(ipRectilinear) != nullptr);
    REQUIRE(InfillPatternRegistry::binding(ipCrossHatch) != nullptr);

    const std::vector<BuiltinInfillLoad> &loads = builtin_infill_pattern_loads();
    REQUIRE(loads.size() == library_rows().size());
    REQUIRE(InfillPatternRegistry::library_rows().size() == library_rows().size());
    for (size_t i = 0; i < loads.size(); ++ i) {
        const BuiltinInfillLoad &load = loads[i];
        const LibraryRow        &row  = library_rows()[i];
        CAPTURE(row.name, load.error);
        CHECK(InfillPatternRegistry::library_rows()[i].pattern == row.pattern);
        CHECK(std::string(InfillPatternRegistry::library_rows()[i].name) == row.name);
        CHECK(load.pattern == row.pattern);
        CHECK(load.name == row.name);
        CHECK(load.error.empty());
        REQUIRE(InfillPatternRegistry::bound(row.pattern));
        const InfillPatternBinding &binding = *InfillPatternRegistry::binding(row.pattern);
        CHECK(binding.file == library_file(row.name));
        CHECK(binding.error.empty());
        REQUIRE(binding.library == load.library);
        const orca_infill_builtin_library &library = *binding.library;
        CHECK(library.abi_major == ORCA_INFILL_BUILTIN_ABI_MAJOR);
        CHECK(std::string(library.build_id) == ORCA_INFILL_BUILD_ID);
        CHECK(std::string(library.name) == row.name);
        REQUIRE(library.row_count == 1);
        CHECK(library.rows[0]->pattern == int32_t(row.pattern));
        CHECK((library.rows[0]->key == nullptr ? std::string() : std::string(library.rows[0]->key)) ==
              (row.token == nullptr ? std::string() : std::string(row.token)));
        CHECK(load.milliseconds >= 0.);

        // The filler of the row runs in its library.
        const std::unique_ptr<Fill> filler(Fill::new_from_type(row.pattern));
        const auto *served = dynamic_cast<const FillLibrary*>(filler.get());
        REQUIRE(served != nullptr);
        CHECK(served->pattern() == row.pattern);
        CHECK(served->file() == binding.file);
    }
    CHECK(InfillPatternRegistry::all_bound());
    CHECK(InfillPatternRegistry::unbound().empty());
#if ORCA_INFILL_PATTERN_LINKAGE_STATIC
    CHECK(builtin_infill_pattern_folder().empty());
#else
    CHECK(fs::equivalent(builtin_infill_pattern_folder(), fs::path(ORCA_TEST_INFILL_PATTERN_DIR)));
#endif

    // Loading again returns the first outcome.
    CHECK(&load_builtin_infill_patterns({ fs::path("no-such-folder") }) == &loads);
}

TEST_CASE("A row a library serves keeps its table entry", "[InfillPatterns][BuiltinBinding]")
{
    const t_config_enum_values &keys = ConfigOptionEnum<InfillPattern>::get_enum_values();
    for (const InfillPatternDescriptor &pattern : InfillPatternRegistry::builtins()) {
        CAPTURE(int(pattern.id));
        const bool served = InfillPatternRegistry::binding(pattern.id) != nullptr;
        if (! served) {
            CHECK_FALSE(InfillPatternRegistry::bound(pattern.id));
            continue;
        }
        // Values, tokens, availability and fallback are those of the table; a library row is never a plugin row, so its
        // anchors keep the layer number without the raft (Layer::generate_sparse_infill_polylines_for_anchoring()).
        CHECK(pattern.available());
        CHECK_FALSE(pattern.plugin());
        CHECK(&sliced_infill_pattern(pattern.id) == &pattern);
        if (! pattern.internal()) {
            CAPTURE(pattern.key);
            CHECK(keys.at(pattern.key) == int(pattern.id));
            CHECK(InfillPatternRegistry::find(std::string(pattern.key)) == &pattern);
        }
    }
}

TEST_CASE("ipfNoSort is the no_sort() of the Fill class of every row", "[InfillPatterns][BuiltinBinding]")
{
    for (const InfillPatternDescriptor &pattern : InfillPatternRegistry::builtins()) {
        if (! pattern.available())
            continue;
        CAPTURE(int(pattern.id));
        const std::unique_ptr<Fill> native = InfillCorpus::native_filler(pattern.id);
        REQUIRE(native != nullptr);
        CHECK(native->no_sort() == pattern.has(ipfNoSort));
        const std::unique_ptr<Fill> created(Fill::new_from_type(pattern.id));
        CHECK(created->no_sort() == native->no_sort());
    }
}

TEST_CASE("A pattern library refuses a host of another build", "[InfillPatterns][BuiltinBinding]")
{
    orca_infill_builtin_host other = builtin_infill_host();
    other.build_id                 = "another build";
    other.log                      = &capture_log;
    for (const BuiltinInfillLoad &load : builtin_infill_pattern_loads()) {
        CAPTURE(load.name);
        REQUIRE(load.entry != nullptr);
        g_log.clear();
        CHECK(load.entry(&other) == nullptr);
        REQUIRE(g_log.size() == 1);
        CHECK(g_log.front() == ORCA_INFILL_BUILD_ID);
    }
}

TEST_CASE("The pattern libraries linked into an executable pass every check", "[InfillPatterns][BuiltinBinding]")
{
    const std::vector<LinkedBuiltinInfillLibrary> &linked = linked_builtin_infill_patterns();
    REQUIRE(linked.size() == library_rows().size());
    for (size_t i = 0; i < linked.size(); ++ i) {
        CAPTURE(linked[i].name);
        CHECK(linked[i].pattern == library_rows()[i].pattern);
        CHECK(std::string(linked[i].name) == library_rows()[i].name);
        const BuiltinInfillLoad load = check_builtin_infill_entry(linked[i].entry, linked[i].pattern);
        CHECK(load.error.empty());
        CHECK(load.library != nullptr);
    }
}

TEST_CASE("The host runs the parallel loops of a pattern library and stops them on cancel", "[InfillPatterns][BuiltinBinding]")
{
    const orca_infill_builtin_host &host = builtin_infill_host();
    std::vector<int>                hits(1000, 0);
    auto body = [](void *user, int64_t begin, int64_t end) {
        for (int64_t i = begin; i < end; ++ i)
            ++ (*static_cast<std::vector<int>*>(user))[size_t(i)];
    };
    orca_infill_job job;
    CHECK(host.parallel_for(&job, 0, 1000, 7, body, &hits) == ORCA_INFILL_OK);
    CHECK(std::all_of(hits.begin(), hits.end(), [](int hit) { return hit == 1; }));
    CHECK(host.is_canceled(&job) == 0);

    job.throw_if_canceled = [] { throw CanceledException(); };
    CHECK(host.is_canceled(&job) == 1);
    CHECK(host.parallel_for(&job, 0, 1000, 7, body, &hits) == ORCA_INFILL_ERR_CANCELED);
    CHECK(hits.front() == 1);
}

#if ! ORCA_INFILL_PATTERN_LINKAGE_STATIC
TEST_CASE("The loader rejects a library that breaks a rule", "[InfillPatterns][BuiltinBinding]")
{
    struct Fixture
    {
        const char *path;
        const char *error;
    };
    for (const Fixture fixture : { Fixture { INFILL_PATTERN_FIXTURE_BAD_BUILD_ID, "belongs to another OrcaSlicer build (another build)" },
                                   Fixture { INFILL_PATTERN_FIXTURE_WRONG_ROW, "serves another pattern (20 crosshatch)" },
                                   Fixture { INFILL_PATTERN_FIXTURE_WRONG_TOKEN, "serves another pattern (2 rectilinear-fixture)" },
                                   Fixture { INFILL_PATTERN_FIXTURE_CALLS_MISMATCH, "serves other calls of its pattern" },
                                   Fixture { INFILL_PATTERN_FIXTURE_TRAIT_MISMATCH, "describes its pattern differently" },
                                   Fixture { INFILL_PATTERN_FIXTURE_NO_ENTRY, "not an OrcaSlicer pattern library" },
                                   Fixture { INFILL_PATTERN_FIXTURE_ALSO_PLUGIN_ENTRY, "not an OrcaSlicer pattern library" } }) {
        CAPTURE(fixture.path);
        const BuiltinInfillLoad load = open_builtin_infill_library(fs::path(fixture.path), ipRectilinear);
        CHECK(load.error == fixture.error);
        CHECK(load.library == nullptr);
    }
    // A library that also exports the entry of a plugin is closed again without being called.
    CHECK(open_builtin_infill_library(fs::path(INFILL_PATTERN_FIXTURE_ALSO_PLUGIN_ENTRY), ipRectilinear).entry == nullptr);

    TemporaryFolder folder;
    const BuiltinInfillLoad missing = open_builtin_infill_library(folder.path / "orca_infill_rectilinear.so", ipRectilinear);
    CHECK(missing.error == "missing");
    {
        boost::nowide::ofstream garbage((folder.path / "garbage").string(), std::ios::binary);
        garbage << "not a library, only some text that is long enough to be read as a header";
    }
    CHECK_THAT(open_builtin_infill_library(folder.path / "garbage", ipRectilinear).error, ContainsSubstring("cannot be loaded: "));
}

TEST_CASE("A failure inside a pattern library is a SlicingError naming the pattern and the file", "[InfillPatterns][BuiltinBinding]")
{
    const BuiltinInfillLoad load = open_builtin_infill_library(fs::path(INFILL_PATTERN_FIXTURE_THROWS_IN_RUN), ipRectilinear);
    REQUIRE(load.error.empty());
    REQUIRE(load.library != nullptr);
    FillLibrary filler(ipRectilinear, *load.library, load.file);
    const Surface surface(stInternal, ExPolygon(Polygon({ Point(0, 0), Point(1000000, 0), Point(1000000, 1000000), Point(0, 1000000) })));
    try {
        filler.fill_surface(&surface, FillParams());
        FAIL("no exception");
    } catch (const SlicingError &error) {
        CHECK_THAT(error.what(), ContainsSubstring("rectilinear") && ContainsSubstring(load.file) && ContainsSubstring("fixture failure"));
    }
}

TEST_CASE("A pattern library in an infill plugin folder is ignored", "[InfillPatterns][BuiltinBinding]")
{
    const BuiltinInfillLoad &rectilinear = *std::find_if(builtin_infill_pattern_loads().begin(), builtin_infill_pattern_loads().end(),
                                                         [](const BuiltinInfillLoad &load) { return load.pattern == ipRectilinear; });
    TemporaryFolder folder;
    fs::copy_file(rectilinear.path, folder.path / rectilinear.path.filename());
    const bool had_runtime_patterns = InfillPatternRegistry::has_runtime_patterns();
    const std::vector<InfillPluginLoad> loads = load_infill_plugins({ folder.path.string() });
    REQUIRE(loads.size() == 1);
    CHECK(loads.front().error == "a built-in pattern library outside the application folder; ignored");
    CHECK(loads.front().patterns.empty());
    CHECK(InfillPatternRegistry::has_runtime_patterns() == had_runtime_patterns);
    CHECK(InfillPatternRegistry::bound(ipRectilinear));
}
#endif
