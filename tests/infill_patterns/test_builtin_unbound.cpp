// Slicing without the built-in pattern libraries: this executable loads them from an empty folder, so every row a
// library serves stays unbound. Its filler cannot be created and every print is refused, naming the files.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/InfillPatternLibraries.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

TEST_CASE("A row whose pattern library is missing slices nothing", "[InfillPatterns][BuiltinUnbound]")
{
    const fs::path empty = fs::temp_directory_path() / fs::unique_path("orca-infill-patterns-%%%%-%%%%-%%%%");
    fs::create_directories(empty);
    const std::vector<BuiltinInfillLoad> &loads = load_builtin_infill_patterns({ empty });
    fs::remove_all(empty);
    REQUIRE(! loads.empty());
    CHECK_FALSE(InfillPatternRegistry::all_bound());
    CHECK(InfillPatternRegistry::unbound().size() == loads.size());

    std::string expected;
    for (const BuiltinInfillLoad &load : loads) {
        CAPTURE(load.name);
        CHECK(load.error == "missing");
        CHECK(load.library == nullptr);
        CHECK_FALSE(InfillPatternRegistry::bound(load.pattern));
        const std::string message = "Infill pattern library " + load.file + " is missing or damaged (missing). Reinstall OrcaSlicer.";
        CHECK(builtin_infill_library_error(load.pattern) == message);
        CHECK_THROWS_WITH(Fill::new_from_type(load.pattern), message);
        // The table is unchanged: the pattern is still available and offered.
        CHECK(infill_pattern(load.pattern).available());
        expected += (expected.empty() ? "" : "\n") + message;
    }

    // Every print is refused, whichever patterns it uses.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "sparse_infill_pattern", "gyroid" }, { "top_surface_pattern", "monotonicline" },
                                    { "bottom_surface_pattern", "monotonicline" }, { "internal_solid_infill_pattern", "monotonic" } });
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    CHECK(print.validate().string == expected);
}
