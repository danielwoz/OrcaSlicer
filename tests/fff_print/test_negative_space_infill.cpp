// Negative space infill: the gaps between the sparse infill lines printed with a second filament.

#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

using ConfigItems = std::vector<ConfigBase::SetDeserializeItem>;

TriangleMesh cube(double size) { return make_cube(size, size, size); }

// Single-nozzle, `filaments`-filament config from defaults; `extra` is applied last.
DynamicPrintConfig multifilament_config(unsigned int filaments, std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    // Filament grouping treats filaments of the same colour as one, so every filament gets its own.
    static const char *palette[] = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF", "#FF8000", "#8000FF" };
    std::string diameters, colours, flush;
    for (unsigned int i = 0; i < filaments; ++ i) {
        diameters += (i ? "," : "") + std::string("1.75");
        colours   += (i ? ";" : "") + std::string(palette[i % (sizeof(palette) / sizeof(palette[0]))]);
        for (unsigned int j = 0; j < filaments; ++ j)
            flush += ((i || j) ? "," : "") + std::string(i == j ? "0" : "280");
    }
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "filament_diameter", diameters } });
    config.set_num_filaments(filaments);
    // Read per filament during export but not resized with the filament options.
    const auto &defaults = FullPrintConfig::defaults();
    for (const char *key : { "filament_type", "filament_vendor", "filament_start_gcode", "filament_change_length",
                             "filament_ramming_travel_time", "filament_ramming_volumetric_speed",
                             "nozzle_temperature", "nozzle_temperature_initial_layer" })
        if (ConfigOption *option = config.option(key, true); option != nullptr && option->is_vector())
            static_cast<ConfigOptionVectorBase *>(option)->resize(filaments, defaults.option(key));
    config.set_deserialize_strict({ { "filament_colour", colours }, { "flush_volumes_matrix", flush } });
    if (extra.size() > 0)
        config.set_deserialize_strict(extra);
    return config;
}

// One flush block and one flush multiplier per nozzle, as the GUI sizes them for more than one nozzle.
void size_flush_to_nozzles(DynamicPrintConfig &config)
{
    const size_t nozzles = config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    auto        &flush   = config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values;
    const std::vector<double> block = flush;
    for (size_t i = 1; i < nozzles; ++ i)
        flush.insert(flush.end(), block.begin(), block.end());
    config.option<ConfigOptionFloats>("flush_multiplier", true)->resize(nozzles);
}

// The full print config with `config_in` applied, ready for the G-code export.
DynamicPrintConfig full_config(const DynamicPrintConfig &config_in)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);
    // The vector enum options built from the defaults carry no key map, which the G-code export needs to write them.
    for (const std::string &key : config.keys()) {
        ConfigOption *option = config.option(key);
        if (auto *enums = dynamic_cast<ConfigOptionEnumsGeneric *>(option); enums != nullptr && enums->keys_map == nullptr)
            enums->keys_map = print_config_def.get(key)->enum_keys_map;
        else if (auto *nullable = dynamic_cast<ConfigOptionEnumsGenericNullable *>(option); nullable != nullptr && nullable->keys_map == nullptr)
            nullable->keys_map = print_config_def.get(key)->enum_keys_map;
    }
    return config;
}

// Applies `meshes` and `config_in` to `print`/`model`, with optional per-object overrides. Unless `arrange` is false, the
// objects are centred on the bed side by side; otherwise they stay where the meshes put them.
void init_negative_space_print(std::vector<TriangleMesh> &&meshes, Print &print, Model &model, const DynamicPrintConfig &config_in,
                const std::vector<ConfigItems> *per_object_overrides = nullptr, bool arrange = true)
{
    const DynamicPrintConfig config = full_config(config_in);
    // A generic printer: ";TYPE:" feature tags and the generic wipe tower.
    print.is_BBL_printer() = false;
    size_t object_idx = 0;
    for (const TriangleMesh &mesh : meshes) {
        ModelObject *object = model.add_object();
        object->name += "object.stl";
        object->add_volume(mesh);
        object->add_instance();
        if (per_object_overrides != nullptr && object_idx < per_object_overrides->size() && ! (*per_object_overrides)[object_idx].empty()) {
            DynamicPrintConfig object_config;
            for (const ConfigBase::SetDeserializeItem &item : (*per_object_overrides)[object_idx])
                object_config.set_deserialize_strict(item.opt_key, item.opt_value);
            object->config.apply(object_config);
        }
        ++ object_idx;
    }
    if (arrange)
        for (size_t i = 0; i < model.objects.size(); ++ i) {
            ModelObject *object = model.objects[i];
            object->center_around_origin();
            object->instances.front()->set_offset(Vec3d(100. + 30. * double(i), 100., 0.));
        }
    for (ModelObject *object : model.objects) {
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
}

// Processes `print` and returns its exported G-code, filling `result` when one is given.
std::string export_gcode(Print &print, GCodeProcessorResult *result = nullptr)
{
    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("%%%%-%%%%-%%%%.gcode");
    // The export always reports into a processor result.
    GCodeProcessorResult own_result;
    print.set_status_silent();
    print.process();
    print.export_gcode(temp.string(), result != nullptr ? result : &own_result, nullptr);
    std::ifstream file(temp.string(), std::ios::binary);
    std::string   gcode((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    boost::nowide::remove(temp.string().c_str());
    return gcode;
}

std::string slice_meshes(std::vector<TriangleMesh> meshes, const DynamicPrintConfig &config, const std::vector<ConfigItems> *per_object_overrides = nullptr)
{
    Print print;
    Model model;
    init_negative_space_print(std::move(meshes), print, model, config, per_object_overrides);
    return export_gcode(print);
}

std::string slice_with_object_overrides(std::vector<TriangleMesh> meshes, const DynamicPrintConfig &config, const std::vector<ConfigItems> &per_object_overrides)
{
    return slice_meshes(std::move(meshes), config, &per_object_overrides);
}

// Tools extruding under the ";TYPE:" marker of `feature`.
std::set<int> tools_for_feature(const std::string &gcode, std::string_view feature)
{
    std::set<int> tools;
    int  current_tool = 0;
    bool in_feature   = false;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (cmd.empty() && line.comment().substr(0, 5) == "TYPE:")
            in_feature = line.comment().substr(5) == feature;
        // Toolchange purges extrude in place under the previous feature's marker.
        else if (in_feature && line.extruding(self) && line.dist_XY(self) > 0)
            tools.insert(current_tool);
    });
    return tools;
}

// Tool changes on a layer after which that tool extrudes nothing before the next tool or layer change.
int idle_tool_changes(const std::string &gcode)
{
    int  idle = 0;
    bool in_layers = false, changed = false, extruded = false;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        const bool tool_change = cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]);
        if (tool_change || line.comment() == "LAYER_CHANGE") {
            if (changed && ! extruded)
                ++ idle;
            in_layers |= ! tool_change;
            changed  = tool_change && in_layers;
            extruded = false;
        } else if (line.extruding(self) && line.dist_XY(self) > 0)
            extruded = true;
    });
    return idle + (changed && ! extruded ? 1 : 0);
}

// The G-code without what differs between any two slices, the time and object ids, and without the config block,
// which lists every setting.
std::string masked_gcode(const std::string &gcode)
{
    std::string out = gcode;
    if (const size_t begin = out.find("; CONFIG_BLOCK_START"); begin != std::string::npos)
        if (const size_t end = out.find("; CONFIG_BLOCK_END", begin); end != std::string::npos)
            out.erase(begin, end - begin);
    return std::regex_replace(out, std::regex("; generated by .*| id: ?\\d+"), "");
}

// Print heights, in hundredths of a millimetre, of the layers extruding under the ";TYPE:" marker of `feature`.
std::set<int> layers_for_feature(const std::string &gcode, std::string_view feature)
{
    std::set<int> layers;
    bool in_feature = false;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        if (cmd.empty() && line.comment().substr(0, 5) == "TYPE:")
            in_feature = line.comment().substr(5) == feature;
        else if (in_feature && line.extruding(self) && line.dist_XY(self) > 0)
            layers.insert(int(std::lround(self.z() * 100.)));
    });
    return layers;
}

// A 20 mm cube at 0.2 mm has 100 layers; 3 bottom and 3 top shell layers leave the layers at 0.8 to 19.4 mm sparse.
// Every feature prints with filament 1 and the negative space infill with `negative_space`; `extra` is applied last.
DynamicPrintConfig negative_space_config(int negative_space, const ConfigItems &extra = {})
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 },
        { "top_shell_layers", 3 }, { "bottom_shell_layers", 3 }, { "top_shell_thickness", 0 }, { "bottom_shell_thickness", 0 },
        { "sparse_infill_density", "20%" }, { "sparse_infill_pattern", "rectilinear" },
        { "outer_wall_filament_id", 1 }, { "inner_wall_filament_id", 1 }, { "sparse_infill_filament_id", 1 },
        { "internal_solid_filament_id", 1 }, { "top_surface_filament_id", 1 }, { "bottom_surface_filament_id", 1 },
        { "negative_space_infill_filament_id", negative_space },
        { "enable_prime_tower", 0 }, { "skirt_loops", 0 }, { "brim_type", "no_brim" },
    });
    for (const ConfigBase::SetDeserializeItem &item : extra)
        config.set_deserialize_strict(item.opt_key, item.opt_value);
    return config;
}

// Masked G-code of a cube whose left half is a modifier with a denser sparse infill and `modifier_extra` applied on top.
std::string modifier_gcode(const DynamicPrintConfig &config, const ConfigItems &modifier_extra)
{
    DynamicPrintConfig modifier;
    modifier.set_deserialize_strict({ { "sparse_infill_density", "40%" } });
    for (const ConfigBase::SetDeserializeItem &item : modifier_extra)
        modifier.set_deserialize_strict(item.opt_key, item.opt_value);
    Print print;
    Model model;
    init_negative_space_print({ cube(20) }, print, model, config, nullptr, false);
    model.objects.front()->add_volume(make_cube(10, 20, 20), ModelVolumeType::PARAMETER_MODIFIER, false)->config.apply(modifier);
    print.apply(model, full_config(config));
    return masked_gcode(export_gcode(print));
}

// Two filaments, a 20% sparse infill between 3 bottom and 3 top shells, and negative space infill on `negative_space`.
DynamicPrintConfig negative_space_fill_config(int negative_space, const char *pattern = "rectilinear", bool symmetric = false)
{
    return multifilament_config(2, {
        { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 },
        { "top_shell_layers", 3 }, { "bottom_shell_layers", 3 }, { "top_shell_thickness", 0 }, { "bottom_shell_thickness", 0 },
        { "sparse_infill_density", "20%" }, { "sparse_infill_pattern", pattern }, { "symmetric_infill_y_axis", symmetric },
        { "negative_space_infill_filament_id", negative_space },
        { "enable_prime_tower", 0 }, { "skirt_loops", 0 }, { "brim_type", "no_brim" },
    });
}

// Slices the meshes as the parts of one object, where they are, with modifiers of their own config.
Print &slice_parts(Print &print, DynamicPrintConfig config, const std::vector<TriangleMesh> &parts,
                   const std::vector<std::pair<TriangleMesh, DynamicPrintConfig>> &modifiers = {})
{
    config.set_deserialize_strict({ { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 }, { "elefant_foot_compensation", 0 },
                                    { "top_shell_thickness", 0 }, { "bottom_shell_thickness", 0 } });
    Model model;
    init_negative_space_print({ parts.front() }, print, model, config, nullptr, false);
    for (size_t i = 1; i < parts.size(); ++ i)
        model.objects.front()->add_volume(TriangleMesh(parts[i]), ModelVolumeType::MODEL_PART, false);
    for (const auto &[mesh, modifier_config] : modifiers)
        model.objects.front()->add_volume(TriangleMesh(mesh), ModelVolumeType::PARAMETER_MODIFIER, false)->config.apply(modifier_config);
    print.apply(model, full_config(config));
    print.process();
    return print;
}

// Two physical filaments plus one mixed slot (1-based id 3) blending them 60/40.
DynamicPrintConfig mixed_config()
{
    DynamicPrintConfig config = multifilament_config(3);
    config.set_deserialize_strict({
        { "filament_is_mixed",                "0,0,1" },
        { "filament_mixed_components",        ";;1,2" },
        { "filament_mixed_sublayer_ratios",   ";;0.6,0.4" },
        { "filament_mixed_gradient",          "0,0,0" },
        { "filament_mixed_gradient_range",    ";;" },
        { "filament_mixed_gradient_curve",    ";;" },
        { "filament_mixed_gradient_per_part", "0,0,0" },
        { "enable_mixed_color_sublayer",      "0" },
    });
    return config;
}

} // namespace

// Narrow internal solid infill is left to the line pattern in the G-code comparisons below: the concentric fill that
// takes it over drops short lines depending on an Arachne parameter it leaves unset, so two slices of one config can differ.
static const ConfigItems repeatable_slicing{ { "detect_narrow_internal_solid_infill", 0 } };

TEST_CASE("Negative space infill stays off when Disabled or set past the last filament", "[MultiFilament][NegativeSpace][Regression]")
{
    SECTION("on the print, with an object filament that fills the features left on Default") {
        const std::vector<ConfigItems> object_filament{ { { "extruder", 2 } } };
        const std::string disabled = masked_gcode(slice_with_object_overrides({ cube(20) }, negative_space_config(0, repeatable_slicing), object_filament));
        CHECK(tools_for_feature(disabled, "Negative space infill").empty());
        CHECK(masked_gcode(slice_with_object_overrides({ cube(20) }, negative_space_config(7, repeatable_slicing), object_filament)) == disabled);
    }
    SECTION("on a modifier, which keeps the regions of a project without it") {
        const std::string disabled = modifier_gcode(negative_space_config(0, repeatable_slicing), {});
        REQUIRE(tools_for_feature(disabled, "Negative space infill").empty());
        CHECK(modifier_gcode(negative_space_config(0, repeatable_slicing), { { "negative_space_infill_filament_id", 7 } }) == disabled);
        CHECK(modifier_gcode(negative_space_config(7, repeatable_slicing), {}) == disabled);
    }
}

TEST_CASE("Negative space infill leaves the G-code unchanged where the sparse infill has no gaps", "[MultiFilament][NegativeSpace][Regression]")
{
    // Spiral vase forces the print density to 0%, so the object keeps a sparse infill of its own there.
    const std::vector<std::pair<ConfigItems, ConfigItems>> cases{
        { { { "sparse_infill_density", "0%" } }, {} },
        { { { "sparse_infill_density", "100%" } }, {} },
        { { { "spiral_mode", 1 }, { "wall_loops", 1 }, { "top_shell_layers", 0 } }, { { "sparse_infill_density", "20%" } } },
    };
    const size_t i = size_t(GENERATE(0, 1, 2));
    DYNAMIC_SECTION("case " << i) {
        const auto &[print_settings, object_settings] = cases[i];
        ConfigItems settings = repeatable_slicing;
        settings.insert(settings.end(), print_settings.begin(), print_settings.end());
        const std::string off = masked_gcode(slice_with_object_overrides({ cube(20) }, negative_space_config(0, settings), { object_settings }));
        const std::string on  = masked_gcode(slice_with_object_overrides({ cube(20) }, negative_space_config(2, settings), { object_settings }));
        CHECK(on == off);
        CHECK(tools_for_feature(on, "Negative space infill").empty());
        CHECK(idle_tool_changes(on) == 0);
    }
}

TEST_CASE("Negative space infill lines are sized for the nozzle of their own filament", "[MultiFilament][NegativeSpace]")
{
    // Line widths left on auto follow the nozzle: filament 1 prints from the 0.4 mm nozzle, filament 2 from the 0.6 mm one.
    // Narrow areas are left to the line pattern, as the concentric fill fits its beads to the width of the area.
    DynamicPrintConfig config = negative_space_config(2, {
        { "detect_narrow_internal_solid_infill", 0 },
        { "nozzle_diameter", "0.4,0.6" }, { "printer_extruder_id", "1,2" },
        { "printer_extruder_variant", "Direct Drive Standard,Direct Drive Standard" }, { "extruder_printable_height", "0,0" },
        { "single_extruder_multi_material", 0 }, { "filament_map_mode", "Manual" }, { "filament_map", "1,2" } });
    size_flush_to_nozzles(config);
    Print print;
    Model model;
    init_negative_space_print({ cube(20) }, print, model, config);
    print.process();
    std::vector<double> negative_widths, sparse_widths;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layerm : layer->regions())
            for (const ExtrusionEntity *entity : layerm->fills.flatten().entities)
                if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity); path != nullptr) {
                    if (path->role() == erNegativeSpaceInfill)
                        negative_widths.push_back(path->width);
                    else if (path->role() == erInternalInfill)
                        sparse_widths.push_back(path->width);
                }
    REQUIRE_FALSE(negative_widths.empty());
    REQUIRE_FALSE(sparse_widths.empty());
    const auto median = [](std::vector<double> &v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
    CHECK(median(negative_widths) > 0.55);
    CHECK(median(sparse_widths) < 0.5);
}

TEST_CASE("Negative space infill fills the gaps of every sparse infill layer with its own filament", "[MultiFilament][NegativeSpace]")
{
    const char *pattern = GENERATE("rectilinear", "grid", "gyroid", "adaptivecubic", "lightning");
    CAPTURE(pattern);
    Print print;
    Model model;
    init_negative_space_print({ cube(20) }, print, model, negative_space_config(2, { { "sparse_infill_pattern", pattern } }));
    GCodeProcessorResult result;
    const std::string gcode = export_gcode(print, &result);
    CHECK(tools_for_feature(gcode, "Sparse infill") == std::set<int>{ 0 });
    CHECK(tools_for_feature(gcode, "Negative space infill") == std::set<int>{ 1 });
    CHECK(tools_for_feature(gcode, "Internal solid infill") == std::set<int>{ 0 });
    CHECK(tools_for_feature(gcode, "Outer wall") == std::set<int>{ 0 });
    const std::set<int> sparse   = layers_for_feature(gcode, "Sparse infill");
    const std::set<int> negative = layers_for_feature(gcode, "Negative space infill");
    REQUIRE(negative.size() > 80); // 94 sparse layers
    // Lightning may leave layers without sparse lines; the negative space infill still prints there.
    CHECK(std::includes(negative.begin(), negative.end(), sparse.begin(), sparse.end()));
    CHECK(*negative.begin() >= 80);    // not on the bottom shells
    CHECK(*negative.rbegin() <= 1940); // not on the top shells
    CHECK(idle_tool_changes(gcode) == 0);
    // The G-code processor reads the feature back with its filament.
    size_t moves = 0, elsewhere = 0;
    for (const auto &move : result.moves)
        if (move.type == EMoveType::Extrude && move.extrusion_role == erNegativeSpaceInfill) {
            ++ moves;
            elsewhere += move.extruder_id != 1;
        }
    CHECK(moves > 0);
    CHECK(elsewhere == 0);
}

TEST_CASE("No internal bridge is printed over negative space infill", "[MultiFilament][NegativeSpace]")
{
    REQUIRE_FALSE(tools_for_feature(slice_meshes({ cube(20) }, negative_space_config(0)), "Internal Bridge").empty());
    CHECK(tools_for_feature(slice_meshes({ cube(20) }, negative_space_config(2)), "Internal Bridge").empty());
}

TEST_CASE("Flushing into infill leaves the sparse lines around negative space infill on their filament", "[MultiFilament][NegativeSpace]")
{
    // A prime tower on the bed, so tool changes are flushed and flush_into_infill takes part.
    auto config = [](int walls, int negative_space) {
        return negative_space_config(negative_space, {
            { "outer_wall_filament_id", walls }, { "inner_wall_filament_id", walls }, { "flush_into_infill", 1 },
            { "enable_prime_tower", 1 }, { "wipe_tower_x", "50" }, { "wipe_tower_y", "50" }, { "layer_change_gcode", "G92 E0\n" } });
    };
    SECTION("without negative space infill the incoming filament takes over sparse lines") {
        CHECK(tools_for_feature(slice_meshes({ cube(20) }, config(2, 0)), "Sparse infill").count(1) == 1);
    }
    SECTION("with negative space infill they keep their filament") {
        const std::string gcode = slice_meshes({ cube(20) }, config(1, 2));
        CHECK(tools_for_feature(gcode, "Sparse infill") == std::set<int>{ 0 });
        CHECK(tools_for_feature(gcode, "Negative space infill") == std::set<int>{ 1 });
    }
}

TEST_CASE("Infill combination is skipped where negative space infill is used", "[MultiFilament][NegativeSpace]")
{
    // Combined (up to 100% of the 0.4 mm nozzle), the sparse infill prints on every other layer only.
    REQUIRE(layers_for_feature(slice_meshes({ cube(20) }, negative_space_config(0, { { "infill_combination", 1 } })), "Sparse infill").size() < 60);
    const std::string gcode = slice_meshes({ cube(20) }, negative_space_config(2, { { "infill_combination", 1 } }));
    const std::set<int> sparse = layers_for_feature(gcode, "Sparse infill");
    CHECK(sparse.size() > 80);
    CHECK(layers_for_feature(gcode, "Negative space infill") == sparse);
}

TEST_CASE("Negative space infill takes the most specific filament set and stays off without a valid one", "[MultiFilament][NegativeSpace]")
{
    auto [global, key, value, expected] = GENERATE(table<int, const char *, int, int>({
        { 0, "extruder", 2, 0 },                          // the object filament does not switch it on
        { 0, "negative_space_infill_filament_id", 3, 0 }, // past the last filament
        { 2, "negative_space_infill_filament_id", 0, 0 }, // Disabled on the object overrides the global filament
        { 2, "extruder", 1, 2 },
        { 0, "negative_space_infill_filament_id", 2, 2 },
    }));
    DYNAMIC_SECTION(key << " " << value << " over " << global) {
        const std::vector<ConfigItems> overrides{ { { key, value } } };
        Print print;
        Model model;
        init_negative_space_print({ cube(20) }, print, model, negative_space_config(global), &overrides);
        REQUIRE(print.objects().front()->num_printing_regions() > 0);
        CHECK(print.objects().front()->printing_region(0).config().negative_space_infill_filament_id.value == expected);
    }
}

TEST_CASE("Negative space infill is active only for a partly dense sparse infill outside spiral vase mode", "[MultiFilament][NegativeSpace]")
{
    PrintConfig       print_config;
    PrintRegionConfig region_config;
    region_config.negative_space_infill_filament_id.value = 2;
    region_config.sparse_infill_density.value             = 20.;
    CHECK(PrintRegion::negative_space_infill_filament(print_config, region_config) == 2);
    SECTION("empty sparse infill") {
        region_config.sparse_infill_density.value = 0.;
        CHECK(PrintRegion::negative_space_infill_filament(print_config, region_config) == 0);
    }
    SECTION("solid sparse infill") {
        region_config.sparse_infill_density.value = 100.;
        CHECK(PrintRegion::negative_space_infill_filament(print_config, region_config) == 0);
    }
    SECTION("spiral vase") {
        print_config.spiral_mode.value = true;
        CHECK(PrintRegion::negative_space_infill_filament(print_config, region_config) == 0);
    }
}

TEST_CASE("Sparse infill and negative space infill together fill the sparse infill area", "[Fill][NegativeSpace]")
{
    // The L (10 mm tall) is not symmetric in X, so a negative space cut from the mirrored area would miss the lines.
    auto [pattern, symmetric] = GENERATE(table<const char *, bool>({ { "rectilinear", false }, { "grid", false }, { "gyroid", false },
        { "honeycomb", false }, { "lightning", false }, { "concentric", false }, { "zigzag", true }, { "lockedzag", true } }));
    CAPTURE(pattern, symmetric);
    Print print;
    Model model;
    init_negative_space_print({ mesh(TestMesh::L) }, print, model, negative_space_fill_config(2, pattern, symmetric));
    print.process();
    size_t checked = 0;
    double uncovered_area = 0., total_area = 0.;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layerm : layer->regions()) {
            Polygons sparse_area;
            layerm->fill_surfaces.filter_by_type(stInternal, &sparse_area);
            if (sparse_area.empty())
                continue;
            Polygons lines, negative;
            for (const ExtrusionEntity *ee : layerm->fills.entities) {
                const auto &fill = static_cast<const ExtrusionEntityCollection &>(*ee);
                if (std::any_of(fill.entities.begin(), fill.entities.end(), [](const ExtrusionEntity *e) { return e->role() == erNegativeSpaceInfill; }))
                    fill.polygons_covered_by_spacing(negative, 0.f);
                else if (fill.role() == erInternalInfill)
                    fill.polygons_covered_by_spacing(lines, 0.f);
            }
            CAPTURE(layer->print_z);
            REQUIRE_FALSE(negative.empty());
            negative = union_(negative);
            lines    = union_(lines);
            const double negative_area = area(negative);
            CHECK(area(diff(negative, offset(sparse_area, float(scale_(0.05))))) < 0.01 * negative_area); // stays inside
            CHECK(area(intersection(negative, lines)) < 0.05 * negative_area);                         // stays off the lines
            // About 80% of the area is empty without it. Line ends meeting curved cells (gyroid) leave the most behind.
            const double uncovered = area(diff(sparse_area, union_(lines, negative)));
            CHECK(uncovered < 0.15 * area(sparse_area));
            uncovered_area += uncovered;
            total_area += area(sparse_area);
            ++ checked;
        }
    CHECK(checked > 30); // 44 sparse layers
    CHECK(uncovered_area < 0.10 * total_area);
}

TEST_CASE("Negative space infill set on a modifier fills only the sparse infill inside it", "[Fill][NegativeSpace]")
{
    DynamicPrintConfig modifier_config;
    modifier_config.set_deserialize_strict({ { "negative_space_infill_filament_id", 2 } });
    Print print;
    slice_parts(print, negative_space_fill_config(0), { cube(20) }, { { make_cube(10, 20, 20), modifier_config } });
    size_t with_negative = 0;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layerm : layer->regions()) {
            const bool enabled = layerm->region().config().negative_space_infill_filament_id.value == 2;
            const ExtrusionEntityCollection fills = layerm->fills.flatten();
            const bool has_negative = std::any_of(fills.entities.begin(), fills.entities.end(),
                                                  [](const ExtrusionEntity *e) { return e->role() == erNegativeSpaceInfill; });
            CAPTURE(layer->print_z, enabled);
            if (! enabled)
                CHECK_FALSE(has_negative);
            else if (layerm->fill_surfaces.has(stInternal)) {
                CHECK(has_negative);
                ++ with_negative;
            }
        }
    CHECK(with_negative > 80); // 94 sparse layers
}

TEST_CASE("Print::validate rejects a mixed filament as the negative space infill filament", "[MixedFilament][NegativeSpace]")
{
    // Every region role on physical filament 1, so only negative space infill may use the mixed slot. The
    // layer-change reset avoids an unrelated relative-extrusion error.
    DynamicPrintConfig config = mixed_config();
    config.set_deserialize_strict({
        { "enable_prime_tower",         "0" },
        { "sparse_infill_density",      "20%" },
        { "layer_change_gcode",         "G92 E0\n" },
        { "outer_wall_filament_id",     "1" },
        { "inner_wall_filament_id",     "1" },
        { "sparse_infill_filament_id",  "1" },
        { "internal_solid_filament_id", "1" },
        { "top_surface_filament_id",    "1" },
        { "bottom_surface_filament_id", "1" },
    });

    SECTION("a physical filament validates") {
        config.set_deserialize_strict({ { "negative_space_infill_filament_id", "2" } });
        Print print;
        Model model;
        init_negative_space_print({ cube(20) }, print, model, config);
        const StringObjectException err = print.validate();
        INFO(err.string);
        CHECK(err.string.empty());
    }

    SECTION("the mixed slot is refused") {
        config.set_deserialize_strict({ { "negative_space_infill_filament_id", "3" } });
        Print print;
        Model model;
        init_negative_space_print({ cube(20) }, print, model, config);
        const StringObjectException err = print.validate();
        CHECK_FALSE(err.string.empty());
        CHECK(err.opt_key == "negative_space_infill_filament_id");
    }
}
