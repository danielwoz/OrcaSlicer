// Modules for the tests: broken, unusual or test-only; the definition FIXTURE_<NAME> selects one.
#include "orca_module_sdk.hpp"

#include <stdexcept>
#include <string>

namespace {

[[maybe_unused]] constexpr orca_infill_param x = orca_infill::int_param("x", "X", 1, 0, 9);

#if defined(FIXTURE_OPTIONS)

constexpr const char *mode_values[] = { "a", "b", "c" };
constexpr const char *mode_labels[] = { "A", "B", "C" };

// Settings of every type in both scopes, each recomputing from another step.
struct Fixture
{
    static constexpr const char *key     = "fixture_options";
    static constexpr const char *label   = "Fixture options";
    static constexpr const char *version = "1.0.0";
    static constexpr orca_infill_param loops  = orca_infill::int_param("loops", "Loops", 0, 0, 5);
    static constexpr orca_infill_param width  = orca_infill::float_param("width", "Width", 5., 0., 50., "mm", "Width of something.");
    static constexpr orca_infill_param mode   = orca_infill::enum_param("mode", "Mode", mode_values, mode_labels, 1);
    static constexpr orca_infill_param flag   = orca_infill::bool_param("flag", "Flag", false);
    static constexpr orca_infill_param height = orca_infill::float_param("height", "Height", 0., 0., 2000., "mm");
    static constexpr orca_infill_param layers = orca_infill::int_param("layers", "Layers", 0, 0, 100);
    static constexpr orca_module_option options[] = {
        orca_mod::region_option(loops, ORCA_STEP_INFILL, "sparse_infill_density > 0"),
        orca_mod::region_option(width, ORCA_STEP_INFILL, "sparse_infill_density > 0 and fixture_options__loops > 0", ORCA_OPTION_ADVANCED),
        orca_mod::region_option(mode, ORCA_STEP_PREPARE_INFILL, nullptr, ORCA_OPTION_HIDE_WHEN_DISABLED),
        orca_mod::region_option(flag, ORCA_STEP_PERIMETERS),
        orca_mod::print_option(height),
        orca_mod::print_option(layers, nullptr, 0, ORCA_STEP_SLICE),
    };
};

#elif defined(FIXTURE_FILL_FAILS)

// A fill hook that fails on the layer of index 3 while fixture_fill__fail is on.
struct Fixture
{
    static constexpr const char *key     = "fixture_fill";
    static constexpr const char *label   = "Fixture fill";
    static constexpr const char *version = "1.0.0";
    static constexpr orca_infill_param fail = orca_infill::bool_param("fail", "Fail", false);
    static constexpr orca_module_option options[] = { orca_mod::region_option(fail, ORCA_STEP_INFILL) };

    static void fill_layer(const orca_mod::FillLayer &layer, orca_mod::FillEdit &)
    {
        for (const orca_mod::FillRegion &region : layer.regions())
            if (region.option(0) != 0. && layer.layer_index() == 3)
                throw orca_infill::Failed();
    }
};

#elif defined(FIXTURE_FILL_KINDS)

// A fill hook that adds, in one call, a sparse loop and a top surface loop inside each sparse area while
// fixture_kinds__add is on.
struct Fixture
{
    static constexpr const char *key     = "fixture_kinds";
    static constexpr const char *label   = "Fixture kinds";
    static constexpr const char *version = "1.0.0";
    static constexpr orca_infill_param add = orca_infill::bool_param("add", "Add", false);
    static constexpr orca_module_option options[] = { orca_mod::region_option(add, ORCA_STEP_INFILL) };

    static void fill_layer(const orca_mod::FillLayer &layer, orca_mod::FillEdit &edit)
    {
        for (const orca_mod::FillRegion &region : layer.regions())
            if (region.option(0) != 0.)
                for (const orca_mod::Area &area : layer.areas(region, ORCA_FILL_SPARSE))
                    for (const orca_infill::Region &ring : edit.offset(area.region, -0.5 * region.sparse_spacing()))
                        for (int32_t kind : { ORCA_FILL_SPARSE, ORCA_FILL_TOP })
                            edit.add(region, kind, ring.contour, true, area.height);
    }
};

#elif defined(FIXTURE_GCODE_THROWS)

// A G-code hook that throws while fixture_gcode__throw is on, and inserts a line with a NUL byte while
// fixture_gcode__nul is on.
struct Fixture
{
    static constexpr const char *key     = "fixture_gcode";
    static constexpr const char *label   = "Fixture G-code";
    static constexpr const char *version = "1.0.0";
    static constexpr orca_infill_param raise = orca_infill::bool_param("throw", "Throw", false);
    static constexpr orca_infill_param nul   = orca_infill::bool_param("nul", "NUL", false);
    static constexpr orca_module_option options[] = { orca_mod::print_option(raise), orca_mod::print_option(nul) };

    static void gcode_layer(const orca_mod::GCodeLayer &layer, orca_mod::Text &before, orca_mod::Text &)
    {
        if (layer.option(0) != 0.)
            throw std::runtime_error("fixture failure");
        if (layer.option(1) != 0.)
            before.line(std::string("M117 a\0b", 8));
    }
};

#elif defined(FIXTURE_FUTURE_MINOR)
// Written without the SDK, which checks the host's version and fills in the current one.
#elif defined(FIXTURE_PARAM_COLLIDES)

// A pattern whose parameter x is named like the module's option x.
struct Pattern
{
    static constexpr const char *key   = "org.orcaslicer.test.collides";
    static constexpr const char *label = "Collides";
    static constexpr uint32_t    slots = ORCA_INFILL_SLOT_SPARSE;
    static constexpr uint32_t    caps  = 0;
    static constexpr uint32_t    post  = ORCA_INFILL_POST_DEFAULT;
    static constexpr orca_infill_param params[] = { orca_infill::float_param("x", "X", 1., 0., 2.) };
    static void generate(const orca_infill::Request &, orca_infill::Output &) {}
};

struct Fixture
{
    static constexpr const char *key     = "fixture_collides";
    static constexpr const char *label   = "Fixture collides";
    static constexpr const char *version = "1.0.0";
    static constexpr orca_module_option options[] = { orca_mod::region_option(x, ORCA_STEP_INFILL) };
    using patterns = std::tuple<Pattern>;
};

#else

#if defined(FIXTURE_BAD_KEY)
#define FIXTURE_KEY "Bad-Key"
#elif defined(FIXTURE_DUPLICATE_KEY)
#define FIXTURE_KEY "infill_walls"
#elif defined(FIXTURE_DUPLICATE_OPTION)
#define FIXTURE_KEY     "fixture_twice"
#define FIXTURE_OPTION_LIST orca_mod::region_option(x, ORCA_STEP_INFILL), orca_mod::print_option(x)
#elif defined(FIXTURE_RESERVED_SCOPE)
#define FIXTURE_KEY     "fixture_scope"
#define FIXTURE_OPTION_LIST { sizeof(orca_module_option), &x, 2, ORCA_STEP_INFILL, 0, nullptr }
#elif defined(FIXTURE_NO_STEPS)
#define FIXTURE_KEY     "fixture_no_steps"
#define FIXTURE_OPTION_LIST orca_mod::region_option(x, 0)
#elif defined(FIXTURE_BAD_CONDITION)
#define FIXTURE_KEY     "fixture_bad"
#define FIXTURE_OPTION_LIST orca_mod::region_option(x, ORCA_STEP_INFILL, "fixture_bad__x >")
#elif defined(FIXTURE_FOREIGN_CONDITION)
#define FIXTURE_KEY     "fixture_foreign"
#define FIXTURE_OPTION_LIST orca_mod::region_option(x, ORCA_STEP_INFILL, "infill_walls__loops > 0")
#elif defined(FIXTURE_MINIMAL)
#define FIXTURE_KEY "fixture_minimal"
#endif

struct Fixture
{
    static constexpr const char *key     = FIXTURE_KEY;
    static constexpr const char *label   = "Fixture";
    static constexpr const char *version = "1.0.0";
#ifdef FIXTURE_OPTION_LIST
    static constexpr orca_module_option options[] = { FIXTURE_OPTION_LIST };
#endif
};

#endif

} // namespace

#if defined(FIXTURE_FUTURE_MINOR)

// A module of a later minor version: its module and option structs have a field this host does not know.
struct FutureOption { orca_module_option option; double later; };
struct FutureModule { orca_module module; const void *later; };

extern "C" ORCA_INFILL_EXPORT const orca_module *ORCA_INFILL_CALL orca_module_entry_v1(const orca_module_host *host)
{
    if (host == nullptr || host->abi_major != ORCA_MODULE_ABI_MAJOR)
        return nullptr;
    static const FutureOption              option { { sizeof(FutureOption), &x, ORCA_SCOPE_REGION, ORCA_STEP_INFILL, 0, nullptr }, 1. };
    static const orca_module_option *const options[] = { &option.option };
    static const FutureModule              module { { sizeof(FutureModule), ORCA_MODULE_ABI_MAJOR, 7, "fixture_future", "Fixture",
                                                      "1.0.0", nullptr, 0, options, 1, nullptr, nullptr, nullptr }, nullptr };
    return &module.module;
}

#else

ORCA_MODULE(Fixture)

#endif
