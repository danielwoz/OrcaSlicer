#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Module/ModuleHooks.hpp"
#include "libslic3r/Module/ModuleLoader.hpp"
#include "libslic3r/Module/ModuleRegistry.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <orca_module.h>

#include "module_paths.h"
#include "test_helpers.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

std::vector<Slic3r::ModuleLoad> &startup_loads();

namespace {

const std::string sample_token = "org.orcaslicer.sample.lines";

// Loads a module file once per process, so test cases that share a process see the same outcome.
const ModuleLoad &load_once(const std::string &path)
{
    static std::map<std::string, ModuleLoad> loads;
    auto it = loads.find(path);
    if (it == loads.end())
        it = loads.emplace(path, load_module(path)).first;
    return it->second;
}

// Every registered module setting at its default.
DynamicPrintConfig module_defaults()
{
    DynamicPrintConfig out;
    for (ModuleScope scope : { ModuleScope::Print, ModuleScope::Region })
        for (const std::string &key : ModuleRegistry::option_keys(scope))
            out.set_key_value(key, print_config_def.get(key)->default_value->clone());
    return out;
}

// Sparse infill at 20 %, with the given settings.
DynamicPrintConfig sparse_config(std::initializer_list<ConfigBase::SetDeserializeItem> items = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "sparse_infill_density", "20%" }, { "layer_height", 0.2 } });
    config.set_deserialize_strict(items);
    return config;
}

// The length of the sparse infill of each object, mm.
std::vector<double> sparse_lengths(const Print &print)
{
    std::vector<double> out;
    for (const PrintObject *object : print.objects()) {
        double length = 0.;
        for (const Layer *layer : object->layers())
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                    if (entity->role() == erInternalInfill)
                        length += unscale<double>(entity->length());
        out.emplace_back(length);
    }
    return out;
}

double sparse_length(const DynamicPrintConfig &config)
{
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    return sparse_lengths(print).front();
}

// The sparse infill of a layer, a polyline per path.
Polylines sparse_polylines(const Layer &layer)
{
    Polylines out;
    auto add = [&out](const ExtrusionPath &path) {
        if (path.role() == erInternalInfill)
            out.emplace_back(path.polyline.to_polyline());
    };
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity))
                add(*path);
            else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
                std::for_each(multi->paths.begin(), multi->paths.end(), add);
            else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity))
                std::for_each(loop->paths.begin(), loop->paths.end(), add);
    return out;
}

// A digest of every point of the sparse infill of the first object, in print order.
uint64_t sparse_digest(const Print &print)
{
    uint64_t digest = 14695981039346656037ull;
    for (const Layer *layer : print.objects().front()->layers())
        for (const Polyline &polyline : sparse_polylines(*layer))
            for (const Point &p : polyline.points)
                for (const coord_t c : { p.x(), p.y() })
                    digest = (digest ^ uint64_t(c)) * 1099511628211ull;
    return digest;
}

// Checks the infill walls of an object: on every layer, each sparse area has `loops` closed sparse paths inside the band
// of `loops` spacings along its outline, and no other sparse path has a point in that band.
void check_infill_walls(const PrintObject &object, int loops)
{
    size_t layers_with_areas = 0, wrong_loop_counts = 0, stray_points = 0;
    for (const Layer *layer : object.layers()) {
        ExPolygons bands;
        size_t     areas = 0;
        for (const LayerRegion *region : layer->regions()) {
            const double spacing = region->flow(frInfill).spacing();
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == stInternal) {
                    ++ areas;
                    append(bands, diff_ex(offset_ex(surface.expolygon, -float(scale_(0.01))),
                                          offset_ex(surface.expolygon, -float(scale_(loops * spacing - 0.01)))));
                }
        }
        auto in_band = [&bands](const Point &p) {
            return std::any_of(bands.begin(), bands.end(), [&p](const ExPolygon &band) { return band.contains(p); });
        };
        size_t found = 0;
        for (const Polyline &polyline : sparse_polylines(*layer)) {
            const size_t inside = size_t(std::count_if(polyline.points.begin(), polyline.points.end(), in_band));
            if (polyline.size() > 2 && polyline.first_point() == polyline.last_point() && inside == polyline.size())
                ++ found;
            else
                stray_points += inside;
        }
        layers_with_areas += areas > 0 ? 1 : 0;
        wrong_loop_counts += found == size_t(loops) * areas ? 0 : 1;
    }
    CHECK(layers_with_areas > 10);
    CHECK(wrong_loop_counts == 0);
    CHECK(stray_points == 0);
}

// The executable block of the G-code, without the lines that differ between two runs of the same slice.
std::string masked_body(const std::string &gcode)
{
    const size_t begin = gcode.find("; EXECUTABLE_BLOCK_START"), end = gcode.find("; EXECUTABLE_BLOCK_END");
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    return strip_nondeterministic_gcode_lines(gcode.substr(begin, end - begin));
}

std::string read_file(const std::string &path)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

size_t occurrences(const std::string &text, const std::string &what)
{
    size_t count = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
        ++ count;
    return count;
}

// The warnings of Print::validate about module settings.
std::vector<StringObjectException> module_warnings(Print &print)
{
    std::vector<StringObjectException> warnings, out;
    print.validate(&warnings);
    std::copy_if(warnings.begin(), warnings.end(), std::back_inserter(out),
                 [](const StringObjectException &w) { return w.type == STRING_EXCEPT_MODULE_SETTINGS; });
    return out;
}

} // namespace

TEST_CASE("Module: the modules loaded at start-up register their settings", "[Module]")
{
    REQUIRE(startup_loads().size() == 4);
    for (const ModuleLoad &load : startup_loads()) {
        INFO(load.path << ": " << load.error);
        CHECK(load.error.empty());
    }
    const ModuleLoad &sample = startup_loads()[0];
    CHECK(sample.module == "sample_lines");
    CHECK(sample.plugin == "Lines (sample) 1.0.0");
    CHECK(sample.patterns == std::vector<std::string>{ sample_token });
    CHECK(sample.options == std::vector<std::string>{ "sample_lines__spacing_scale", "sample_lines__cross", "sample_lines__phase" });
    CHECK(startup_loads()[1].module == "infill_walls");
    CHECK(startup_loads()[1].options == std::vector<std::string>{ "infill_walls__loops", "infill_walls__min_width" });
    CHECK(startup_loads()[2].module == "change_at_z");
    CHECK(startup_loads()[2].options ==
          std::vector<std::string>{ "change_at_z__height", "change_at_z__nozzle_temperature", "change_at_z__flow" });
    CHECK(startup_loads()[3].options ==
          std::vector<std::string>{ "fixture_options__loops", "fixture_options__width", "fixture_options__mode",
                                    "fixture_options__flag", "fixture_options__height", "fixture_options__layers" });

    SECTION("definitions") {
        const ConfigOptionDef *loops = print_config_def.get("fixture_options__loops");
        REQUIRE(loops != nullptr);
        CHECK(loops->type == coInt);
        CHECK(loops->default_value->getInt() == 0);
        CHECK(loops->min == 0.f);
        CHECK(loops->max == 5.f);
        CHECK(loops->category == "Fixture options");
        CHECK(loops->mode == comSimple);
        CHECK(loops->cli_args(loops->opt_key) == std::vector<std::string>{ "fixture-options--loops" });
        const ConfigOptionDef *width = print_config_def.get("fixture_options__width");
        REQUIRE(width != nullptr);
        CHECK(width->type == coFloat);
        CHECK(width->default_value->getFloat() == 5.);
        CHECK(width->sidetext == "mm");
        CHECK(width->tooltip == "Width of something.");
        CHECK(width->mode == comAdvanced);
        const ConfigOptionDef *mode = print_config_def.get("fixture_options__mode");
        REQUIRE(mode != nullptr);
        CHECK(mode->type == coEnum);
        CHECK(mode->enum_values == std::vector<std::string>{ "a", "b", "c" });
        CHECK(mode->enum_labels == std::vector<std::string>{ "A", "B", "C" });
        CHECK(mode->default_value->serialize() == "b");
        CHECK(print_config_def.get("fixture_options__flag")->type == coBool);
        const ConfigOptionDef *phase = print_config_def.get("sample_lines__phase");
        REQUIRE(phase != nullptr);
        CHECK(phase->enum_values == std::vector<std::string>{ "aligned", "staggered" });
        CHECK(phase->category == "Lines (sample)");
        CHECK(print_config_def.get("sample_lines__spacing_scale")->max == 4.f);
        const ConfigOptionDef *walls = print_config_def.get("infill_walls__loops");
        REQUIRE(walls != nullptr);
        CHECK(walls->type == coInt);
        CHECK(walls->default_value->getInt() == 0);
        CHECK(walls->max == 5.f);
        CHECK(walls->category == "Infill walls");
        const ConfigOptionDef *min_width = print_config_def.get("infill_walls__min_width");
        REQUIRE(min_width != nullptr);
        CHECK(min_width->type == coFloat);
        CHECK(min_width->default_value->getFloat() == 5.);
        CHECK(min_width->sidetext == "mm");
        CHECK(min_width->mode == comAdvanced);
        const ConfigOptionDef *height = print_config_def.get("change_at_z__height");
        REQUIRE(height != nullptr);
        CHECK(height->type == coFloat);
        CHECK(height->max == 2000.f);
        CHECK(height->category == "Change at height");
        CHECK(print_config_def.get("change_at_z__nozzle_temperature")->type == coInt);
        const ConfigOptionDef *flow = print_config_def.get("change_at_z__flow");
        REQUIRE(flow != nullptr);
        CHECK(flow->default_value->getInt() == 100);
        CHECK(flow->min == 50.f);
        CHECK(flow->max == 150.f);
        CHECK(flow->sidetext == "%");

        const DynamicPrintAndCLIConfig cli;
        const std::vector<std::string> &preset_keys = Preset::print_options();
        for (const ModuleRecord *module : ModuleRegistry::modules())
            for (const ModuleOption *option : module->settings) {
                INFO(option->key);
                CHECK(cli.def()->has(option->key));
                CHECK(std::find(preset_keys.begin(), preset_keys.end(), option->key) != preset_keys.end());
                CHECK(option->def == print_config_def.get(option->key));
            }
        CHECK(std::find(preset_keys.begin(), preset_keys.end(), "missing_module_options") != preset_keys.end());
    }

    SECTION("records") {
        std::vector<std::string> keys;
        for (const ModuleRecord *module : ModuleRegistry::modules())
            keys.emplace_back(module->key);
        CHECK(std::is_sorted(keys.begin(), keys.end()));
        const ModuleOption *loops = ModuleRegistry::option("fixture_options__loops");
        REQUIRE(loops != nullptr);
        CHECK(loops->scope == ModuleScope::Region);
        CHECK(loops->invalidates == msInfill);
        CHECK(loops->enabled_if == "sparse_infill_density > 0");
        CHECK(! loops->hide_when_disabled);
        CHECK(loops->module->key == "fixture_options");
        CHECK(loops->module->options.front() == loops);
        CHECK(ModuleRegistry::option("fixture_options__mode")->hide_when_disabled);
        const ModuleOption *height = ModuleRegistry::option("fixture_options__height");
        REQUIRE(height != nullptr);
        CHECK(height->scope == ModuleScope::Print);
        CHECK(height->invalidates == msGCode);
        CHECK(ModuleRegistry::option("fixture_options__layers")->invalidates == msSlice);
        CHECK(ModuleRegistry::option("sparse_infill_density") == nullptr);

        const ModuleOption *walls = ModuleRegistry::option("infill_walls__loops");
        REQUIRE(walls != nullptr);
        CHECK(walls->scope == ModuleScope::Region);
        CHECK(walls->invalidates == msInfill);
        CHECK(walls->module->has_fill_hook);
        CHECK(! walls->module->has_gcode_hook);
        const ModuleOption *change = ModuleRegistry::option("change_at_z__height");
        REQUIRE(change != nullptr);
        CHECK(change->scope == ModuleScope::Print);
        CHECK(change->invalidates == msGCode);
        CHECK(change->module->has_gcode_hook);
        CHECK(! change->module->has_fill_hook);
        CHECK(ModuleRegistry::has_fill_hooks());

        const ModuleOption *cross = ModuleRegistry::option("sample_lines__cross");
        REQUIRE(cross != nullptr);
        CHECK(cross->scope == ModuleScope::Region);
        CHECK(cross->invalidates == msPrepareInfill);
        CHECK(cross->hide_when_disabled);
        CHECK(cross->enabled_if == "sparse_infill_pattern == \"" + sample_token + "\" or top_surface_pattern == \"" + sample_token +
                                   "\" or bottom_surface_pattern == \"" + sample_token + "\" or internal_solid_infill_pattern == \"" +
                                   sample_token + "\"");
        CHECK(cross->module->options.empty());
        CHECK(cross->module->settings.size() == 3);

        const std::vector<std::string> print_keys = ModuleRegistry::option_keys(ModuleScope::Print);
        CHECK(std::find(print_keys.begin(), print_keys.end(), "fixture_options__height") != print_keys.end());
        CHECK(std::find(print_keys.begin(), print_keys.end(), "fixture_options__loops") == print_keys.end());
        CHECK(ModuleRegistry::has_options());
    }
}

TEST_CASE("Module: settings round trip through text and the command line", "[Module]")
{
    DynamicPrintConfig config;
    for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>>{
             { "fixture_options__loops", "3" }, { "fixture_options__width", "2.5" }, { "fixture_options__mode", "c" },
             { "fixture_options__flag", "1" }, { "fixture_options__height", "12.5" }, { "sample_lines__spacing_scale", "2" },
             { "sample_lines__cross", "1" }, { "sample_lines__phase", "staggered" }, { "infill_walls__loops", "2" },
             { "infill_walls__min_width", "7.5" }, { "change_at_z__height", "5" }, { "change_at_z__flow", "95" } }) {
        config.set_deserialize_strict(key, value);
        CHECK(config.opt_serialize(key) == value);
    }
    CHECK_THROWS_AS(config.set_deserialize_strict("fixture_options__mode", "d"), BadOptionValueException);

    DynamicPrintAndCLIConfig cli;
    const char *argv[] = { "orca-slicer", "--fixture-options--loops=2", "--sample-lines--phase", "staggered", "--fixture-options--flag",
                           "--infill-plugin-dir", "a", "--module-dir=b", "--no-infill-plugins", "--infill-walls--loops=2",
                           "--change-at-z--height=5" };
    std::vector<std::string> extra;
    REQUIRE(cli.read_cli(int(std::size(argv)), argv, &extra));
    CHECK(cli.opt_int("fixture_options__loops") == 2);
    CHECK(cli.opt_serialize("sample_lines__phase") == "staggered");
    CHECK(cli.opt_bool("fixture_options__flag"));
    CHECK(cli.opt_int("infill_walls__loops") == 2);
    CHECK(cli.opt_float("change_at_z__height") == 5.);
    CHECK(cli.option<ConfigOptionStrings>("module_dir")->values == std::vector<std::string>{ "a", "b" });
    CHECK(cli.opt_bool("no_modules"));
}

TEST_CASE("Module: start-up reads the module flags and those of infill plugins", "[Module]")
{
    char program[] = "orca-slicer", dir1[] = "--infill-plugin-dir", dir2[] = "a", dir3[] = "--module-dir=b", data1[] = "--datadir",
         data2[] = "data", off[] = "--no-infill-plugins=0";
    char *argv[] = { program, dir1, dir2, dir3, data1, data2, off };
    const ModuleStartup startup = module_startup(int(std::size(argv)), argv, boost::filesystem::path("bin") / "orca-slicer");
    CHECK(! startup.disabled);
    REQUIRE(startup.dirs.size() >= 6);
    CHECK(startup.dirs[0] == "a");
    CHECK(startup.dirs[1] == "b");
    CHECK(startup.dirs[startup.dirs.size() - 2] == module_data_dirs("data")[0]);
    CHECK(startup.dirs.back() == module_data_dirs("data")[1]);
    CHECK(boost::filesystem::path(module_data_dirs("data")[0]).filename() == "modules");
    CHECK(boost::filesystem::path(module_data_dirs("data")[1]).filename() == "infill_plugins");

    char flag[] = "--no-modules";
    char *disabled_argv[] = { program, flag };
    CHECK(module_startup(2, disabled_argv, "orca-slicer").disabled);
}

TEST_CASE("Module: the default print preset holds every module setting at its default", "[Module]")
{
    PresetBundle bundle;
    const DynamicPrintConfig &config = bundle.prints.default_preset().config;
    for (const ModuleRecord *module : ModuleRegistry::modules())
        for (const ModuleOption *option : module->settings) {
            INFO(option->key);
            REQUIRE(config.has(option->key));
            CHECK(*config.option(option->key) == *option->def->default_value);
        }
    CHECK(config.opt_string("missing_module_options").empty());
}

TEST_CASE("Module: settings of modules that are not loaded are kept", "[Module]")
{
    auto read = [](DynamicPrintConfig &config, const std::string &key, const std::string &value) {
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
        config.set_deserialize(key, value, substitutions);
        return substitutions.unrecogized_keys;
    };

    SECTION("an unknown module setting goes into missing_module_options") {
        DynamicPrintConfig config;
        CHECK(read(config, "absent_mod__x", "3").empty());
        CHECK(read(config, "absent_mod__a", "on").empty());
        CHECK(read(config, "absent_mod__x", "4").empty());
        CHECK(! config.has("absent_mod__x"));
        CHECK(config.opt_string("missing_module_options") == "absent_mod__a = on\nabsent_mod__x = 4");
        // Other unknown keys are reported as before.
        CHECK(read(config, "no_such_key", "1") == std::vector<std::string>{ "no_such_key" });
    }
    SECTION("it survives a JSON round trip") {
        DynamicPrintConfig config = sparse_config();
        read(config, "absent_mod__x", "3");
        ScopedTemporaryFile file(".json");
        config.save_to_json(file.string(), "kept", "User", "2.4.0.0");
        // Module settings at their defaults and the bags are left out, so a build without the module keeps nothing.
        config.apply(module_defaults());
        config.set_key_value("fixture_options__loops", new ConfigOptionInt(2));
        ScopedTemporaryFile defaults_file(".json");
        config.save_to_json(defaults_file.string(), "kept", "User", "2.4.0.0");
        const std::string saved = read_file(defaults_file.string());
        CHECK(saved.find("\"fixture_options__loops\"") != std::string::npos);
        for (const char *key : { "\"fixture_options__width\"", "\"infill_walls__loops\"", "\"module_print_values\"", "\"module_region_values\"" }) {
            INFO(key);
            CHECK(saved.find(key) == std::string::npos);
        }
        config.set_key_value("missing_module_options", new ConfigOptionString(""));
        config.save_to_json(defaults_file.string(), "kept", "User", "2.4.0.0");
        CHECK(read_file(defaults_file.string()).find("missing_module_options") == std::string::npos);
        DynamicPrintConfig loaded;
        std::map<std::string, std::string> key_values;
        std::string reason;
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
        REQUIRE(loaded.load_from_json(file.string(), substitutions, true, key_values, reason) == 0);
        CHECK(loaded.opt_string("missing_module_options") == "absent_mod__x = 3");
        CHECK(std::find(substitutions.unrecogized_keys.begin(), substitutions.unrecogized_keys.end(), "absent_mod__x") ==
              substitutions.unrecogized_keys.end());
    }
    SECTION("the values of loaded modules are restored") {
        DynamicPrintConfig config;
        read(config, "missing_module_options", "fixture_options__loops = 2\nabsent_mod__x = 3\nnot a line\nplain_key = 1");
        CHECK(config.opt_int("fixture_options__loops") == 2);
        CHECK(config.opt_string("missing_module_options") == "absent_mod__x = 3");
    }
    SECTION("an explicit value wins over a kept one, in either order") {
        DynamicPrintConfig before, after;
        read(before, "fixture_options__loops", "4");
        read(before, "missing_module_options", "fixture_options__loops = 2");
        read(after, "missing_module_options", "fixture_options__loops = 2");
        read(after, "fixture_options__loops", "4");
        CHECK(before.opt_int("fixture_options__loops") == 4);
        CHECK(after.opt_int("fixture_options__loops") == 4);
        CHECK(! before.has("missing_module_options"));
    }
    SECTION("lists of settings that differ from the system preset keep module settings") {
        DynamicPrintConfig config;
        const std::string listed = "absent_mod__x;absent_mod__xy;missing_module_options;sparse_infill_density";
        read(config, "different_settings_to_system", "\"" + listed + "\";\"\"");
        CHECK(config.option<ConfigOptionStrings>("different_settings_to_system")->values == std::vector<std::string>{ listed, "" });
    }
    SECTION("the derived bags are never read back") {
        DynamicPrintConfig config;
        CHECK(read(config, "module_region_values", "x=1").empty());
        CHECK(read(config, "module_print_values", "fixture_options__height=3").empty());
        CHECK(! config.has("module_region_values"));
        CHECK(! config.has("module_print_values"));
        CHECK(! config.has("missing_module_options"));
    }
}

TEST_CASE("Module: a project over a system preset keeps its module settings and kept settings", "[Module]")
{
    // The project lists a difference, so the values it does not list are taken from the system preset; module settings
    // and kept ones are never set by system presets, so they stay.
    PresetBundle bundle;
    const DynamicPrintConfig &defaults = bundle.prints.default_preset().config;
    DynamicPrintConfig system = defaults;
    DynamicPrintConfig project = defaults;
    project.set_deserialize_strict({ { "inherits", "System" }, { "sparse_infill_density", "25%" }, { "fixture_options__loops", "2" },
                                     { "layer_height", "0.3" } });
    project.set_key_value("missing_module_options", new ConfigOptionString("absent_mod__x = 3"));
    for (const std::set<std::string> &listed : { std::set<std::string>{ "sparse_infill_density", "fixture_options__loops" },
                                                 std::set<std::string>{ "sparse_infill_density", "missing_module_options" } }) {
        const DynamicPrintConfig loaded = Preset::load_external_config(Preset::TYPE_PRINT, defaults, project, listed,
                                                                       [&system](const std::string &) { return &system; });
        CHECK(loaded.opt_int("fixture_options__loops") == 2);
        CHECK(loaded.opt_string("missing_module_options") == "absent_mod__x = 3");
        CHECK(loaded.opt_float("layer_height") == system.opt_float("layer_height"));
        CHECK(loaded.option("sparse_infill_density")->serialize() == "25%");
    }
}

TEST_CASE("Module: kept settings survive a 3MF round trip in objects, volumes and height ranges", "[Module][3mf]")
{
    // Files other than the project settings store a line break of a text as "\n".
    const std::string kept = "absent_mod__x = 3\nabsent_mod__y = 4";
    ScopedSlic3rTemporaryDir temporary_dir("orca_modules");
    const std::string path = (temporary_dir.path() / "stored.3mf").string();

    Model model;
    model.set_backup_path((temporary_dir.path() / "backup").string());
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    ModelVolume *modifier = object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER);
    object->add_instance();
    object->config.set_key_value("missing_module_options", new ConfigOptionString(kept));
    // A kept value of a loaded module is restored on reading, the others stay kept.
    modifier->config.set_key_value("missing_module_options", new ConfigOptionString("absent_mod__x = 3\nfixture_options__width = 7\nabsent_mod__y = 4"));
    object->layer_config_ranges[{ 2., 4. }].set_key_value("missing_module_options", new ConfigOptionString(kept));
    object->config.set_key_value("fixture_options__loops", new ConfigOptionInt(2));
    DynamicPrintConfig config = sparse_config();
    config.set_key_value("missing_module_options", new ConfigOptionString(kept));
    {
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = 0;
        StoreParams store_params;
        store_params.path = path.c_str();
        store_params.model = &model;
        store_params.config = &config;
        store_params.plate_data_list.push_back(plate.get());
        store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        REQUIRE(store_bbs_3mf(store_params));
    }

    Model reloaded_model;
    reloaded_model.set_backup_path((temporary_dir.path() / "reloaded_backup").string());
    DynamicPrintConfig reloaded;
    ConfigSubstitutionContext substitutions{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs plates;
    std::vector<Preset*> project_presets;
    bool is_bbl_3mf = false, is_orca_3mf = false;
    Semver file_version;
    const bool loaded = load_bbs_3mf(path.c_str(), &reloaded, &substitutions, &reloaded_model, &plates, &project_presets,
                                     &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    REQUIRE(loaded);
    CHECK(reloaded.opt_string("missing_module_options") == kept);
    REQUIRE(reloaded_model.objects.size() == 1);
    const ModelObject &object_out = *reloaded_model.objects.front();
    CHECK(object_out.config.opt_serialize("missing_module_options") == ConfigOptionString(kept).serialize());
    CHECK(object_out.config.opt_int("fixture_options__loops") == 2);
    REQUIRE(object_out.volumes.size() == 2);
    CHECK(object_out.volumes[1]->config.opt_serialize("missing_module_options") == ConfigOptionString(kept).serialize());
    CHECK(object_out.volumes[1]->config.opt_float("fixture_options__width") == 7.);
    REQUIRE(object_out.layer_config_ranges.size() == 1);
    CHECK(object_out.layer_config_ranges.begin()->second.opt_serialize("missing_module_options") == ConfigOptionString(kept).serialize());
}

TEST_CASE("Module: bags hold the module values that differ from the defaults", "[Module]")
{
    SECTION("functions") {
        const ModuleOption &loops = *ModuleRegistry::option("fixture_options__loops");
        const ModuleOption &mode  = *ModuleRegistry::option("fixture_options__mode");
        ConfigOptionString bag;
        ModuleRegistry::set_bag_value(bag, mode, ConfigOptionEnumGeneric(mode.def->enum_keys_map, 2));
        ModuleRegistry::set_bag_value(bag, loops, ConfigOptionInt(3));
        CHECK(bag.value == "fixture_options__loops=3\nfixture_options__mode=c");
        CHECK(ModuleRegistry::bag_value(bag.value, loops) == 3.);
        CHECK(ModuleRegistry::bag_value(bag.value, mode) == 2.);
        CHECK(ModuleRegistry::bag_value("", mode) == 1.);
        CHECK(ModuleRegistry::bag_value("fixture_options__loops=9", loops) == 5.);
        CHECK(ModuleRegistry::bag_value("fixture_options__loops=x", loops) == 0.);
        ModuleRegistry::set_bag_value(bag, loops, ConfigOptionInt(0));
        CHECK(bag.value == "fixture_options__mode=c");
        CHECK(ModuleRegistry::changed_steps("", "") == 0);
        CHECK(ModuleRegistry::changed_steps("fixture_options__mode=c", "fixture_options__mode=c\nfixture_options__flag=1") == msPerimeters);
        CHECK(ModuleRegistry::changed_steps("fixture_options__mode=c\nsample_lines__cross=1", "fixture_options__mode=a") ==
              (msPrepareInfill));
        CHECK(ModuleRegistry::changed_steps("absent_mod__x=1", "") == (msGCode | msInfill | msPrepareInfill | msPerimeters | msSlice));
        CHECK(ModuleRegistry::is_default_value("fixture_options__loops", ConfigOptionInt(0)));
        CHECK(! ModuleRegistry::is_default_value("fixture_options__loops", ConfigOptionInt(1)));
        CHECK(! ModuleRegistry::is_default_value("sparse_infill_density", ConfigOptionPercent(20)));
    }
    SECTION("module settings at their defaults leave the bags empty") {
        DynamicPrintConfig config = sparse_config();
        config.apply(module_defaults());
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        CHECK(print.config().module_print_values.value.empty());
        CHECK(print.default_region_config().module_region_values.value.empty());
        CHECK(print.objects().front()->printing_region(0).config().module_region_values.value.empty());
    }
    SECTION("module settings reach the print and its regions") {
        Print print;
        Model model;
        init_print({ cube(20) }, print, model,
                   sparse_config({ { "fixture_options__loops", "2" }, { "fixture_options__height", "5" }, { "fixture_options__mode", "a" } }));
        CHECK(print.config().module_print_values.value == "fixture_options__height=5");
        CHECK(print.objects().front()->printing_region(0).config().module_region_values.value ==
              "fixture_options__loops=2\nfixture_options__mode=a");
        const ModuleRecord &fixture = *ModuleRegistry::option("fixture_options__loops")->module;
        CHECK(ModuleRegistry::option_values(fixture, print.config(), print.objects().front()->printing_region(0).config()) ==
              std::vector<double>{ 2., 5., 0., 0., 5., 0. });
    }
    SECTION("a sample module's setting reaches the regions") {
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, sparse_config({ { "infill_walls__loops", "2" } }));
        CHECK(print.default_region_config().module_region_values.value == "infill_walls__loops=2");
        CHECK(print.config().module_print_values.value.empty());
    }
    SECTION("object overrides give regions of their own") {
        auto region_hashes = [](const char *a, const char *b) {
            Print print;
            Model model;
            const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides { { { "fixture_options__loops", a } },
                                                                                        { { "fixture_options__loops", b } } };
            init_print({ cube(20), cube(20) }, print, model, sparse_config(), &overrides);
            REQUIRE(print.objects().size() == 2);
            return std::make_pair(print.objects()[0]->printing_region(0).config().hash(), print.objects()[1]->printing_region(0).config().hash());
        };
        const auto different = region_hashes("1", "2");
        CHECK(different.first != different.second);
        const auto equal = region_hashes("2", "2");
        CHECK(equal.first == equal.second);
    }
    SECTION("a modifier splits the region inside it only") {
        Print print;
        Model model;
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(20., 20., 20.));
        ModelVolume *modifier = object->add_volume(make_cube(5., 5., 20.), ModelVolumeType::PARAMETER_MODIFIER);
        modifier->config.set_key_value("fixture_options__loops", new ConfigOptionInt(2));
        object->add_instance();
        object->ensure_on_bed();
        print.apply(model, sparse_config());
        const PrintObject &print_object = *print.objects().front();
        REQUIRE(print_object.num_printing_regions() == 2);
        std::set<std::string> bags;
        for (size_t i = 0; i < print_object.num_printing_regions(); ++ i)
            bags.insert(print_object.printing_region(i).config().module_region_values.value);
        CHECK(bags == std::set<std::string>{ "", "fixture_options__loops=2" });
    }
}

TEST_CASE("Module: a changed module setting recomputes from the steps it declares", "[Module]")
{
    // Slices and exports with config, applies the same with one setting changed, and returns the print.
    auto reapply = [](DynamicPrintConfig config, const std::string &key, const std::string &value, Print &print, Model &model) {
        // init_print() turns G-code comments on; so does the config applied afterwards.
        config.set_deserialize_strict("gcode_comments", "1");
        init_print({ cube(20) }, print, model, config);
        gcode(print);
        REQUIRE(print.is_step_done(psGCodeExport));
        config.set_deserialize_strict(key, value);
        print.apply(model, config);
        return print.objects().front();
    };
    Print print;
    Model model;
    SECTION("a region setting recomputing the infill") {
        const PrintObject *object = reapply(sparse_config(), "fixture_options__loops", "2", print, model);
        CHECK(! object->is_step_done(posInfill));
        CHECK(object->is_step_done(posPrepareInfill));
        CHECK(object->is_step_done(posPerimeters));
    }
    SECTION("a pattern parameter recomputing the infill areas") {
        const PrintObject *object = reapply(sparse_config({ { "sparse_infill_pattern", sample_token } }), "sample_lines__spacing_scale", "2",
                                            print, model);
        CHECK(! object->is_step_done(posPrepareInfill));
        CHECK(object->is_step_done(posPerimeters));
    }
    SECTION("a print setting recomputing the G-code") {
        const PrintObject *object = reapply(sparse_config(), "fixture_options__height", "5", print, model);
        for (PrintObjectStep step : { posSlice, posPerimeters, posPrepareInfill, posInfill })
            CHECK(object->is_step_done(step));
        CHECK(! print.is_step_done(psGCodeExport));
    }
    SECTION("the loops of infill walls recompute the infill") {
        const PrintObject *object = reapply(sparse_config(), "infill_walls__loops", "2", print, model);
        CHECK(! object->is_step_done(posInfill));
        CHECK(object->is_step_done(posPrepareInfill));
        CHECK(object->is_step_done(posPerimeters));
    }
    SECTION("the height of change at height recomputes the G-code only") {
        const PrintObject *object = reapply(sparse_config(), "change_at_z__height", "5", print, model);
        for (PrintObjectStep step : { posSlice, posPerimeters, posPrepareInfill, posInfill })
            CHECK(object->is_step_done(step));
        CHECK(! print.is_step_done(psGCodeExport));
    }
    SECTION("a print setting recomputing the slices") {
        const PrintObject *object = reapply(sparse_config(), "fixture_options__layers", "3", print, model);
        CHECK(! object->is_step_done(posSlice));
    }
    SECTION("a region setting recomputing the perimeters") {
        const PrintObject *object = reapply(sparse_config(), "fixture_options__flag", "1", print, model);
        CHECK(! object->is_step_done(posPerimeters));
        CHECK(object->is_step_done(posSlice));
    }
}

TEST_CASE("Module: the parameters of a module's pattern are module settings", "[Module]")
{
    const DynamicPrintConfig base = sparse_config({ { "sparse_infill_pattern", sample_token } });
    auto with = [&base](std::initializer_list<ConfigBase::SetDeserializeItem> items) {
        DynamicPrintConfig config = base;
        config.set_deserialize_strict(items);
        return config;
    };
    const double plain = sparse_length(base);
    REQUIRE(plain > 0.);

    SECTION("a wider spacing") {
        const double wide = sparse_length(with({ { "sample_lines__spacing_scale", "2" } }));
        CHECK(wide > 0.4 * plain);
        CHECK(wide < 0.6 * plain);
    }
    SECTION("the text parameters of infill plugins do not apply") {
        CHECK(sparse_length(with({ { "infill_plugin_params", sample_token + "/spacing_scale = 2" } })) == plain);
    }
    SECTION("object overrides are filled apart") {
        Print print;
        Model model;
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides { { { "sample_lines__spacing_scale", "1" } },
                                                                                    { { "sample_lines__spacing_scale", "2" } } };
        init_print({ cube(20), cube(20) }, print, model, base, &overrides);
        print.process();
        const std::vector<double> lengths = sparse_lengths(print);
        REQUIRE(lengths.size() == 2);
        CHECK(lengths[1] < 0.6 * lengths[0]);
    }
    SECTION("a value outside the range is clamped, with one warning") {
        const DynamicPrintConfig clamped = with({ { "sample_lines__spacing_scale", "9" } });
        CHECK(sparse_length(clamped) == sparse_length(with({ { "sample_lines__spacing_scale", "4" } })));
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, clamped);
        const std::vector<StringObjectException> warnings = module_warnings(print);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().opt_key == "sample_lines__spacing_scale");
        CHECK_THAT(warnings.front().string, Catch::Matchers::ContainsSubstring("sample_lines__spacing_scale = 9 is outside 0.5 to 4; 4 is used"));
    }
}

TEST_CASE("Module: conditions follow the configuration", "[Module]")
{
    const ModuleOption &width = *ModuleRegistry::option("fixture_options__width");
    const ModuleOption &cross = *ModuleRegistry::option("sample_lines__cross");
    DynamicPrintConfig config = sparse_config();
    config.apply(module_defaults());
    const ModuleOption &min_width = *ModuleRegistry::option("infill_walls__min_width");
    CHECK(! ModuleRegistry::condition_holds(min_width, config));
    DynamicPrintConfig walls = config;
    walls.set_deserialize_strict("infill_walls__loops", "1");
    CHECK(ModuleRegistry::condition_holds(min_width, walls));
    CHECK(ModuleRegistry::condition_holds(*ModuleRegistry::option("change_at_z__flow"), sparse_config({ { "change_at_z__height", "5" } })));
    CHECK(! ModuleRegistry::condition_holds(width, config));
    config.set_deserialize_strict("fixture_options__loops", "1");
    CHECK(ModuleRegistry::condition_holds(width, config));
    config.set_deserialize_strict("sparse_infill_density", "0%");
    CHECK(! ModuleRegistry::condition_holds(width, config));

    CHECK(! ModuleRegistry::condition_holds(cross, config));
    for (const char *opt_key : { "sparse_infill_pattern", "top_surface_pattern", "bottom_surface_pattern", "internal_solid_infill_pattern" }) {
        DynamicPrintConfig selected = config;
        selected.set_deserialize_strict(opt_key, sample_token);
        CHECK(ModuleRegistry::condition_holds(cross, selected));
    }
    // A condition that cannot be evaluated holds.
    CHECK(ModuleRegistry::condition_holds(width, DynamicPrintConfig()));

    const std::vector<const ModuleOption*> &conditional = ModuleRegistry::options_with_condition();
    CHECK(std::find(conditional.begin(), conditional.end(), &width) != conditional.end());
    CHECK(std::find(conditional.begin(), conditional.end(), ModuleRegistry::option("fixture_options__flag")) == conditional.end());
}

TEST_CASE("Module: broken and unusual modules", "[Module]")
{
    auto expect_rejected = [](const char *path, const char *reason) {
        const ModuleLoad &load = load_once(path);
        CAPTURE(load.path, load.error);
        CHECK_THAT(load.error, Catch::Matchers::ContainsSubstring(reason));
        CHECK(load.module.empty());
        CHECK(load.options.empty());
        CHECK(std::any_of(module_load_failures().begin(), module_load_failures().end(),
                          [&load](const ModuleLoad &failure) { return failure.path == load.path; }));
    };
    expect_rejected(FIXTURE_WRONG_MAJOR, "built for module ABI 2");
    expect_rejected(FIXTURE_BAD_KEY, "the module key must match");
    expect_rejected(FIXTURE_DUPLICATE_KEY, "infill_walls is already in use");
    expect_rejected(FIXTURE_DUPLICATE_OPTION, "parameter x is declared twice");
    expect_rejected(FIXTURE_RESERVED_SCOPE, "option fixture_scope__x has an unknown scope");
    expect_rejected(FIXTURE_NO_STEPS, "option fixture_no_steps__x must invalidate");
    expect_rejected(FIXTURE_BAD_CONDITION, "the condition of option fixture_bad__x is invalid");
    expect_rejected(FIXTURE_FOREIGN_CONDITION, "the condition of option fixture_foreign__x is invalid");
    for (const char *key : { "fixture_twice__x", "fixture_scope__x", "fixture_no_steps__x", "fixture_bad__x", "fixture_foreign__x" })
        CHECK(! print_config_def.has(key));

    const ModuleLoad &collides = load_once(FIXTURE_PARAM_COLLIDES);
    CHECK_THAT(collides.error, Catch::Matchers::ContainsSubstring("org.orcaslicer.test.collides: the setting fixture_collides__x"));
    CHECK(collides.module == "fixture_collides");
    CHECK(collides.patterns.empty());
    CHECK(collides.options == std::vector<std::string>{ "fixture_collides__x" });
    CHECK(InfillPatternRegistry::find(std::string("org.orcaslicer.test.collides")) == nullptr);
    CHECK(print_config_def.get("fixture_collides__x")->type == coInt);

    const ModuleLoad &future = load_once(FIXTURE_FUTURE_MINOR);
    CHECK(future.error.empty());
    CHECK(future.options == std::vector<std::string>{ "fixture_future__x" });

    const ModuleLoad &minimal = load_once(FIXTURE_MINIMAL);
    CHECK(minimal.error.empty());
    CHECK(minimal.module == "fixture_minimal");
    CHECK(minimal.options.empty());
    const auto &modules = ModuleRegistry::modules();
    const auto  record  = std::find_if(modules.begin(), modules.end(), [](const ModuleRecord *m) { return m->key == "fixture_minimal"; });
    REQUIRE(record != modules.end());
    CHECK((*record)->settings.empty());
}

TEST_CASE("Module: the G-code config block lists module settings only when they are used", "[Module]")
{
    DynamicPrintConfig config = sparse_config();
    config.apply(module_defaults());
    const std::string unused = slice({ cube(20) }, config);
    for (const char *prefix : { "; fixture_options__", "; infill_walls__", "; change_at_z__", "; sample_lines__", "; module_",
                                "; missing_module_options" }) {
        INFO(prefix);
        CHECK(unused.find(prefix) == std::string::npos);
    }
    config.set_deserialize_strict("fixture_options__loops", "2");
    CHECK(slice({ cube(20) }, config).find("\n; fixture_options__loops = 2\n") != std::string::npos);
    config.set_deserialize_strict("infill_walls__loops", "2");
    CHECK(slice({ cube(20) }, config).find("\n; infill_walls__loops = 2\n") != std::string::npos);
}

TEST_CASE("Module: kept settings and values outside their ranges are reported", "[Module]")
{
    SECTION("kept settings") {
        DynamicPrintConfig config = sparse_config();
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
        config.set_deserialize("absent_mod__x", "3", substitutions);
        Print print;
        Model model;
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides { { { "absent_mod__y", "1" } } };
        init_print({ cube(20) }, print, model, config, &overrides);
        const std::vector<StringObjectException> warnings = module_warnings(print);
        REQUIRE(warnings.size() == 2);
        CHECK_THAT(warnings[0].string, Catch::Matchers::ContainsSubstring("not installed"));
        CHECK_THAT(warnings[0].string, Catch::Matchers::EndsWith(": absent_mod__x."));
        CHECK(warnings[0].object == nullptr);
        CHECK_THAT(warnings[1].string, Catch::Matchers::EndsWith(": absent_mod__y."));
        CHECK(warnings[1].object == print.objects().front());
    }
    SECTION("values outside their ranges") {
        DynamicPrintConfig config = sparse_config();
        config.set_key_value("fixture_options__loops", new ConfigOptionInt(9));
        config.set_key_value("fixture_options__height", new ConfigOptionFloat(-1.));
        Print print;
        Model model;
        init_print({ cube(20), cube(20) }, print, model, config);
        const std::vector<StringObjectException> warnings = module_warnings(print);
        REQUIRE(warnings.size() == 2);
        CHECK(warnings[0].opt_key == "fixture_options__height");
        CHECK(warnings[0].string == "fixture_options__height = -1 is outside 0 to 2000; 0 is used.");
        CHECK(warnings[1].opt_key == "fixture_options__loops");
        CHECK(warnings[1].string == "fixture_options__loops = 9 is outside 0 to 5; 5 is used.");
    }
    SECTION("a value of a sample module outside its range") {
        DynamicPrintConfig config = sparse_config();
        config.set_key_value("infill_walls__loops", new ConfigOptionInt(9));
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        const std::vector<StringObjectException> warnings = module_warnings(print);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().opt_key == "infill_walls__loops");
        CHECK(warnings.front().string == "infill_walls__loops = 9 is outside 0 to 5; 5 is used.");
    }
}

TEST_CASE("Module: module settings at their defaults change nothing", "[Module]")
{
    auto compare = [](auto &&slice_config, DynamicPrintConfig config) {
        const std::string without = strip_nondeterministic_gcode_lines(slice_config(config));
        config.apply(module_defaults());
        CHECK(strip_nondeterministic_gcode_lines(slice_config(config)) == without);
    };
    compare([](const DynamicPrintConfig &config) { return slice({ cube(20) }, config); }, sparse_config());
    DynamicPrintConfig supported = DynamicPrintConfig::full_print_config();
    supported.set_deserialize_strict({ { "raft_layers", 2 }, { "enable_support", true } });
    compare([](const DynamicPrintConfig &config) { return slice({ TestMesh::bridge }, config); }, supported);
}

TEST_CASE("Module: infill walls trim the sparse infill and print loops around it", "[Module]")
{
    const DynamicPrintConfig plain = sparse_config({ { "sparse_infill_density", "15%" }, { "sparse_infill_pattern", "rectilinear" } });
    auto with = [&plain](std::initializer_list<ConfigBase::SetDeserializeItem> items) {
        DynamicPrintConfig config = plain;
        config.set_deserialize_strict(items);
        return config;
    };
    auto digest = [](TriangleMesh &&mesh, const DynamicPrintConfig &config) {
        Print print;
        init_and_process_print({ std::move(mesh) }, print, config);
        REQUIRE(sparse_lengths(print).front() > 0.);
        return sparse_digest(print);
    };

    SECTION("loops around the sparse areas, on which the sparse lines end") {
        const DynamicPrintConfig config = with({ { "infill_walls__loops", "2" } });
        Print print;
        init_and_process_print({ cube(20) }, print, config);
        check_infill_walls(*print.objects().front(), 2);
        // The same slice again gives the same infill, whatever order the layers were filled in.
        CHECK(digest(cube(20), config) == sparse_digest(print));
    }
    SECTION("the loops of a concentric infill are cut as well") {
        Print print;
        init_and_process_print({ cube(20) }, print, with({ { "infill_walls__loops", "2" }, { "sparse_infill_pattern", "concentric" } }));
        check_infill_walls(*print.objects().front(), 2);
    }
    SECTION("no loops change nothing") {
        CHECK(digest(cube(20), with({ { "infill_walls__loops", "0" } })) == digest(cube(20), plain));
        CHECK(digest(cube(20), with({ { "infill_walls__loops", "1" } })) != digest(cube(20), plain));
    }
    SECTION("an area narrower than the minimum width keeps its plain infill") {
        CHECK(digest(make_cube(20., 3., 10.), with({ { "infill_walls__loops", "2" } })) == digest(make_cube(20., 3., 10.), plain));
    }
    SECTION("each island of a layer keeps its own loops") {
        // One object of two boxes: G-code prints each fills collection with the island holding its first point.
        TriangleMesh mesh = make_cube(15., 15., 10.), other = make_cube(15., 15., 10.);
        other.translate(25., 0., 0.);
        mesh.merge(other);
        Print print;
        init_and_process_print({ std::move(mesh) }, print, with({ { "infill_walls__loops", "2" } }));
        check_infill_walls(*print.objects().front(), 2);
        size_t collections = 0, spanning = 0;
        for (const Layer *layer : print.objects().front()->layers()) {
            REQUIRE(layer->lslices.size() == 2);
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->fills.entities) {
                    BoundingBox box;
                    for (const Polyline &polyline : entity->as_polylines())
                        box.merge(get_extents(polyline));
                    ++ collections;
                    spanning += std::none_of(layer->lslices.begin(), layer->lslices.end(), [&box](const ExPolygon &island) {
                        const BoundingBox island_box = get_extents(island);
                        return island_box.contains(box.min) && island_box.contains(box.max);
                    }) ? 1 : 0;
                }
        }
        CHECK(collections > 0);
        CHECK(spanning == 0);
    }
}

TEST_CASE("Module: each path a fill hook adds keeps the role of its kind for the tool ordering", "[Module]")
{
    REQUIRE(load_once(FIXTURE_FILL_KINDS).error.empty());
    Print print;
    init_and_process_print({ cube(20) }, print, sparse_config({ { "fixture_kinds__add", "1" } }));
    // ToolOrdering picks one filament per fills collection, from its role: sparse and top paths must not share one.
    size_t added_top = 0, mixed = 0;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.entities) {
                std::set<ExtrusionRole> roles;
                for (const ExtrusionEntity *path : static_cast<const ExtrusionEntityCollection*>(entity)->flatten().entities)
                    roles.insert(path->role());
                mixed += roles.count(erInternalInfill) > 0 && roles.count(erTopSolidInfill) > 0 ? 1 : 0;
                if (entity->role() == erTopSolidInfill)
                    for (const Polyline &polyline : entity->as_polylines())
                        added_top += polyline.size() > 2 && polyline.first_point() == polyline.last_point() ? 1 : 0;
            }
    CHECK(added_top > 10);
    CHECK(mixed == 0);
}

TEST_CASE("Module: change at height inserts its lines in the first layer at the height, once per pass", "[Module]")
{
    DynamicPrintConfig config = sparse_config({ { "change_at_z__height", "5" }, { "change_at_z__nozzle_temperature", "200" },
                                                { "change_at_z__flow", "95" } });
    const std::string lines = "\n; change_at_z: from 5.000 mm\nM104 S200\nM221 S95\n";

    SECTION("by layer") {
        const std::string gcode = slice({ cube(20) }, config);
        REQUIRE(occurrences(gcode, lines) == 1);
        CHECK(occurrences(gcode, "\nM104 S200\n") == 1);
        CHECK(occurrences(gcode, "\nM221 S95\n") == 1);
        // After the layer change of the first layer at the height, before its extrusions.
        const size_t at = gcode.find(lines), layer = gcode.rfind("\n;Z:", at), below = gcode.rfind("\n;Z:", layer - 1);
        REQUIRE(layer != std::string::npos);
        REQUIRE(below != std::string::npos);
        CHECK(std::stod(gcode.substr(layer + 4, 16)) > 5. - 1e-6);
        CHECK(std::stod(gcode.substr(below + 4, 16)) < 5. - 1e-6);
        CHECK(gcode.find("\n;TYPE:", layer) > at);
        // The flow is reset after the last layer.
        REQUIRE(occurrences(gcode, "\nM221 S100\n") == 1);
        CHECK(gcode.find("\nM221 S100\n") > gcode.rfind("\n;Z:"));
    }
    SECTION("by object") {
        config.set_deserialize_strict("print_sequence", "by object");
        const std::string gcode = slice({ cube(20), cube(20) }, config);
        CHECK(occurrences(gcode, lines) == 2);
        CHECK(occurrences(gcode, "\nM221 S100\n") == 2);
    }
    SECTION("a height within the first two layers keeps its temperature") {
        // The second layer sets the nozzle temperature of its own; the inserted one follows it.
        config.set_deserialize_strict({ { "change_at_z__height", "0.3" }, { "change_at_z__nozzle_temperature", "235" },
                                        { "initial_layer_print_height", "0.2" }, { "nozzle_temperature_initial_layer", "210" },
                                        { "nozzle_temperature", "200" } });
        const std::string gcode = masked_body(slice({ cube(20) }, config));
        const size_t      at    = gcode.find("\nM104 S235\n");
        REQUIRE(at != std::string::npos);
        CHECK(gcode.find("\nM104 S200", at) == std::string::npos);
        CHECK(gcode.rfind("\nM104 S200", at) != std::string::npos);
    }
    SECTION("height 0 turns it off") {
        config.set_deserialize_strict("change_at_z__height", "0");
        CHECK(masked_body(slice({ cube(20) }, config)) == masked_body(slice({ cube(20) }, sparse_config())));
    }
}

TEST_CASE("Module: a failing hook ends the slice with an error naming the module", "[Module]")
{
    auto error_of = [](auto &&run) {
        std::string message;
        try {
            run();
        } catch (const SlicingError &error) {
            message = error.what();
        }
        return message;
    };
    SECTION("fill hook") {
        REQUIRE(load_once(FIXTURE_FILL_FAILS).error.empty());
        // Off, it changes nothing.
        CHECK(sparse_length(sparse_config()) == sparse_length(sparse_config({ { "fixture_fill__fail", "0" } })));
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, sparse_config({ { "fixture_fill__fail", "1" } }));
        CHECK_THAT(error_of([&print] { print.process(); }),
                   Catch::Matchers::StartsWith("The module \"Fixture fill\" failed on the layer at ") &&
                   Catch::Matchers::EndsWith(" mm (error -1)."));
    }
    SECTION("G-code hook") {
        REQUIRE(load_once(FIXTURE_GCODE_THROWS).error.empty());
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, sparse_config({ { "fixture_gcode__throw", "1" } }));
        print.process();
        CHECK_THAT(error_of([&print] { gcode(print); }),
                   Catch::Matchers::StartsWith("The module \"Fixture G-code\" failed on the layer at ") &&
                   Catch::Matchers::EndsWith(" mm (error -4)."));
    }
    SECTION("G-code with a NUL byte, which the host rejects") {
        REQUIRE(load_once(FIXTURE_GCODE_THROWS).error.empty());
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, sparse_config({ { "fixture_gcode__nul", "1" } }));
        print.process();
        CHECK_THAT(error_of([&print] { gcode(print); }),
                   Catch::Matchers::StartsWith("The module \"Fixture G-code\" failed on the layer at ") &&
                   Catch::Matchers::EndsWith(" mm (error -1)."));
    }
}

TEST_CASE("Module: the host services reject invalid arguments", "[Module]")
{
    const orca_module_host &host = module_host();
    CHECK(host.struct_size == sizeof(orca_module_host));
    CHECK(host.abi_major == ORCA_MODULE_ABI_MAJOR);
    REQUIRE(host.infill != nullptr);
    const orca_area_visitor  visit = [](void *, const orca_infill_region *, double) {};
    const orca_infill_point  points[2] { { 0, 0 }, { 10, 0 } };
    const orca_infill_region region { { points, 2 }, nullptr, 0 };
    const orca_infill_region *regions[] { &region };
    CHECK(host.visit_fill_areas(nullptr, 0, ORCA_FILL_SPARSE, visit, nullptr) == ORCA_INFILL_ERR_INVALID);
    CHECK(host.erase_fills(nullptr, ORCA_FILL_MASK(ORCA_FILL_SPARSE), regions, 1, nullptr, 0) == ORCA_INFILL_ERR_INVALID);
    CHECK(host.add_fill(nullptr, 0, ORCA_FILL_SPARSE, points, 2, 0, 0.) == ORCA_INFILL_ERR_INVALID);
    CHECK(host.append_text(nullptr, "M117", 4) == ORCA_INFILL_ERR_INVALID);
}
