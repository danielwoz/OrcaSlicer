#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/dll/shared_library.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/FillPlugin.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/InfillPlugins.hpp"
#include "libslic3r/Module/ModuleLoader.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "infill_plugin_paths.h"
#include "test_helpers.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

Slic3r::ModuleLoad &sample_plugin_load();

namespace {

const std::string sample_key = "org.orcaslicer.sample.lines";

// Loads a plugin file once per process, so test cases that share a process see the same outcome.
const ModuleLoad &load_once(const std::string &path)
{
    static std::map<std::string, ModuleLoad> loads;
    auto it = loads.find(path);
    if (it == loads.end())
        it = loads.emplace(path, load_module(path)).first;
    return it->second;
}

InfillPattern sample_pattern()
{
    const InfillPatternDescriptor *pattern = InfillPatternRegistry::find(sample_key);
    REQUIRE(pattern != nullptr);
    return pattern->id;
}

// A UTF-8 string of a path, and back.
std::string utf8(const boost::filesystem::path &path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}
boost::filesystem::path from_utf8(const std::string &s)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(s));
#else
    return boost::filesystem::path(s);
#endif
}

// Visits every extrusion path of the fills of a layer.
template<class Visit> void for_each_fill_path(const Layer &layer, Visit &&visit)
{
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
            if (auto *path = dynamic_cast<const ExtrusionPath*>(entity))
                visit(*path);
            else if (auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
                for (const ExtrusionPath &p : multi->paths)
                    visit(p);
            else if (auto *loop = dynamic_cast<const ExtrusionLoop*>(entity))
                for (const ExtrusionPath &p : loop->paths)
                    visit(p);
        }
}

// Path count, length and a digest of every point in printing order, of the fills of one role.
struct FillShape
{
    size_t   paths { 0 };
    double   length { 0. };
    uint64_t digest { 14695981039346656037ull };
};

void add_to_shape(FillShape &shape, const Layer &layer, ExtrusionRole role)
{
    for_each_fill_path(layer, [&shape, role](const ExtrusionPath &path) {
        if (path.role() != role)
            return;
        const Points3 &pts = path.polyline.points;
        ++ shape.paths;
        for (const auto &pt : pts)
            for (const coord_t coordinate : { pt.x(), pt.y(), pt.z() })
                shape.digest = (shape.digest ^ uint64_t(coordinate)) * 1099511628211ull;
        for (size_t i = 1; i < pts.size(); ++ i)
            shape.length += (pts[i] - pts[i - 1]).head<2>().cast<double>().norm();
    });
}

FillShape fill_shape(const Layer &layer, ExtrusionRole role = erInternalInfill)
{
    FillShape shape;
    add_to_shape(shape, layer, role);
    return shape;
}

FillShape fill_shape(const Print &print, ExtrusionRole role = erInternalInfill)
{
    FillShape shape;
    for (const Layer *layer : print.objects().front()->layers())
        add_to_shape(shape, *layer, role);
    return shape;
}

// The direction in degrees [0, 180) that most of the length of a role's fill runs in, or -1 without such fill.
int dominant_angle(const Layer &layer, ExtrusionRole role)
{
    std::map<int, double> weight;
    for_each_fill_path(layer, [&weight, role](const ExtrusionPath &path) {
        if (path.role() != role)
            return;
        const Points3 &pts = path.polyline.points;
        for (size_t i = 1; i < pts.size(); ++ i) {
            const double dx = double(pts[i].x() - pts[i - 1].x()), dy = double(pts[i].y() - pts[i - 1].y());
            int deg = int(std::lround(std::atan2(dy, dx) * 180. / M_PI)) % 180;
            weight[deg < 0 ? deg + 180 : deg] += std::hypot(dx, dy);
        }
    });
    return weight.empty() ? -1 : std::max_element(weight.begin(), weight.end(), [](const auto &a, const auto &b) { return a.second < b.second; })->first;
}

// A 20 mm cube with the sample pattern as sparse infill at 20 %.
DynamicPrintConfig sample_config(const std::string &params = "", const std::string &pattern = sample_key)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "sparse_infill_pattern", pattern }, { "sparse_infill_density", "20%" },
                                    { "layer_height", 0.2 }, { "infill_plugin_params", params } });
    return config;
}

FillShape sliced_shape(const DynamicPrintConfig &config, ExtrusionRole role = erInternalInfill)
{
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    return fill_shape(print, role);
}

// The warnings of Print::validate about infill plugin parameters.
std::vector<std::string> parameter_warnings(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    std::vector<StringObjectException> warnings;
    print.validate(&warnings);
    std::vector<std::string> out;
    for (const StringObjectException &w : warnings)
        if (w.opt_key == "infill_plugin_params")
            out.emplace_back(w.string);
    return out;
}

} // namespace

TEST_CASE("Infill plugin: the sample is loaded and offered in the pattern options", "[InfillPlugin]")
{
    const ModuleLoad &load = sample_plugin_load();
    INFO(load.error);
    REQUIRE(load.error.empty());
    CHECK(load.plugin == "OrcaSlicer sample infill patterns 1.0.0");
    REQUIRE(load.patterns == std::vector<std::string>{ sample_key });

    const InfillPatternDescriptor *pattern = InfillPatternRegistry::find(sample_key);
    REQUIRE(pattern != nullptr);
    CHECK(int(pattern->id) >= int(ipCount));
    CHECK(pattern->plugin());
    CHECK(pattern->available());
    CHECK(pattern->flags == (ipfMultiline | ipfSeparable | ipfRotateTemplateSafe));
    CHECK(&infill_pattern(pattern->id) == pattern);
    CHECK(&sliced_infill_pattern(pattern->id) == pattern);
    CHECK_FALSE(infill_pattern(InfillPattern(int(ipCount) + 1000)).plugin());

    for (const auto &[opt_key, slot] : InfillPatternRegistry::options()) {
        CAPTURE(opt_key);
        std::vector<std::string> builtin;
        for (const InfillPatternDescriptor &row : InfillPatternRegistry::builtins())
            if (row.slots & slot)
                builtin.emplace_back(row.key);
        const ConfigOptionDef &def = *print_config_def.get(opt_key);
        REQUIRE(def.enum_values.size() >= builtin.size());
        CHECK(std::vector<std::string>(def.enum_values.begin(), def.enum_values.begin() + builtin.size()) == builtin);
        if (slot == ipsIroning) {
            CHECK(def.enum_values == builtin);
        } else {
            // Plugin patterns follow the built-in ones, by token; the sample's token sorts first among the tests'.
            REQUIRE(def.enum_values.size() > builtin.size());
            CHECK(def.enum_values[builtin.size()] == sample_key);
            CHECK(def.enum_labels[builtin.size()] == "Lines (sample)");
        }
        CHECK(def.enum_values.size() == def.enum_labels.size());
        CHECK(DynamicPrintAndCLIConfig().def()->get(opt_key)->enum_values == def.enum_values);
    }
    CHECK(DynamicPrintAndCLIConfig().def()->get("sparse_infill_pattern")->has_enum_value(sample_key));
}

TEST_CASE("Infill plugin: a plugin pattern round trips through configurations", "[InfillPlugin]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("sparse_infill_pattern", sample_key);
    CHECK(config.opt_serialize("sparse_infill_pattern") == sample_key);
    CHECK(config.opt_enum<InfillPattern>("sparse_infill_pattern") == sample_pattern());
    CHECK(config.validate().empty());

    FullPrintConfig full;
    full.apply(config);
    CHECK(full.sparse_infill_pattern.serialize() == sample_key);
    CHECK(Slic3r::validate(full).empty());

    std::unique_ptr<Fill> fill(Fill::new_from_type(sample_key));
    CHECK(dynamic_cast<FillPlugin*>(fill.get()) != nullptr);
}

TEST_CASE("Infill plugin: the sample pattern slices", "[InfillPlugin]")
{
    Print print;
    init_and_process_print({ cube(20) }, print, sample_config());
    const FillShape shape = fill_shape(print);
    CHECK(shape.paths > 0);

    // The sample keeps its direction on every layer.
    std::set<int> angles;
    for (const Layer *layer : print.objects().front()->layers())
        if (const int angle = dominant_angle(*layer, erInternalInfill); angle >= 0)
            angles.insert(angle);
    CHECK(angles.size() == 1);

    // The role tag depends on the printer vendor.
    const std::string gcode_text = gcode(print);
    CHECK((gcode_text.find(";TYPE:Sparse infill\n") != std::string::npos || gcode_text.find("; FEATURE: Sparse infill\n") != std::string::npos));

    // Slicing again, and with two instances, gives the very same infill.
    CHECK(sliced_shape(sample_config()).digest == shape.digest);
    Print twice;
    Model model;
    init_print({ cube(20) }, twice, model, sample_config(), nullptr, true, 2);
    twice.process();
    CHECK(fill_shape(twice).digest == shape.digest);
}

TEST_CASE("Infill plugin: parameters are read from infill_plugin_params", "[InfillPlugin]")
{
    const InfillPattern pattern = sample_pattern();

    SECTION("parsing") {
        using V = std::vector<double>;
        CHECK(infill_plugin_param_values(pattern, "") == V{ 1., 0., 0. });
        CHECK(infill_plugin_param_values(pattern, " org.orcaslicer.sample.lines / spacing_scale = 2.5 ;org.other.x/cross=1\n"
                                                  "org.orcaslicer.sample.lines/cross=true\r\norg.orcaslicer.sample.lines/phase=staggered") ==
              V{ 2.5, 1., 1. });
        // The last value wins, an invalid one falls back to the default.
        CHECK(infill_plugin_param_values(pattern, "org.orcaslicer.sample.lines/spacing_scale=2;org.orcaslicer.sample.lines/spacing_scale=x") ==
              V{ 1., 0., 0. });
        CHECK(infill_plugin_param_values(InfillPattern(ipRectilinear), "org.orcaslicer.sample.lines/cross=1").empty());

        std::vector<std::string> messages;
        CHECK(infill_plugin_param_values(pattern, "org.orcaslicer.sample.lines/spacing_scale=9", &messages) == V{ 4., 0., 0. });
        CHECK(messages.size() == 1);
        messages.clear();
        CHECK(infill_plugin_param_values(pattern, "org.orcaslicer.sample.lines/bogus=1", &messages) == V{ 1., 0., 0. });
        REQUIRE(messages.size() == 1);
        CHECK_THAT(messages.front(), Catch::Matchers::ContainsSubstring("spacing_scale, cross, phase"));
        messages.clear();
        CHECK(infill_plugin_param_values(pattern, "org.orcaslicer.sample.lines/phase=bogus", &messages) == V{ 1., 0., 0. });
        CHECK(messages.size() == 1);
        // An entry that names no pattern is reported, unlike one for another pattern.
        messages.clear();
        CHECK(infill_plugin_param_values(pattern, "spacing_scale = 2\n\norg.other.x/cross=1", &messages) == V{ 1., 0., 0. });
        REQUIRE(messages.size() == 1);
        CHECK_THAT(messages.front(), Catch::Matchers::ContainsSubstring("\"spacing_scale = 2\" is not written as"));

        // The section is canonical, so text that differs only in form does not split fill groups.
        auto section = [](InfillPattern p, const std::string &text) {
            PrintRegionConfig config;
            config.infill_plugin_params.value = text;
            return infill_plugin_params_section(p, &config);
        };
        CHECK(section(pattern, "org.orcaslicer.sample.lines/cross = true\nother.pattern/x=1") ==
              section(pattern, "org.orcaslicer.sample.lines/cross=1"));
        CHECK(section(pattern, "") == "spacing_scale=1;cross=0;phase=0");
        CHECK(section(ipGyroid, "org.orcaslicer.sample.lines/cross=1").empty());
        CHECK_THAT(infill_plugin_param_help(pattern), Catch::Matchers::ContainsSubstring("org.orcaslicer.sample.lines/spacing_scale = 1"));
    }

    SECTION("slicing") {
        const FillShape base = sliced_shape(sample_config());
        REQUIRE(base.length > 0.);
        const FillShape wide = sliced_shape(sample_config("org.orcaslicer.sample.lines/spacing_scale=2"));
        CHECK(wide.length > 0.4 * base.length);
        CHECK(wide.length < 0.6 * base.length);
        CHECK(sliced_shape(sample_config("org.orcaslicer.sample.lines/cross=1")).digest != base.digest);

        // Staggered lines move on odd layers only.
        Print aligned, staggered;
        init_and_process_print({ cube(20) }, aligned, sample_config());
        init_and_process_print({ cube(20) }, staggered, sample_config("org.orcaslicer.sample.lines/phase=staggered"));
        size_t odd_changed = 0;
        for (size_t i = 0; i < aligned.objects().front()->layer_count(); ++ i) {
            const Layer &a = *aligned.objects().front()->get_layer(int(i));
            const Layer &s = *staggered.objects().front()->get_layer(int(i));
            const FillShape shape_a = fill_shape(a), shape_s = fill_shape(s);
            if (a.id() % 2 == 0)
                CHECK(shape_a.digest == shape_s.digest);
            else if (shape_a.paths > 0 && shape_a.digest != shape_s.digest)
                ++ odd_changed;
        }
        CHECK(odd_changed > 0);
    }

    SECTION("warnings") {
        for (const char *params : { "org.orcaslicer.sample.lines/spacing_scale=9", "org.orcaslicer.sample.lines/bogus=1",
                                    "org.orcaslicer.sample.lines/phase=bogus", "spacing_scale=2" }) {
            CAPTURE(params);
            CHECK(parameter_warnings(sample_config(params)).size() == 1);
        }
        CHECK(parameter_warnings(sample_config("org.orcaslicer.sample.lines/spacing_scale=2")).empty());
    }
}

TEST_CASE("Infill plugin: regions with different parameters are filled apart", "[InfillPlugin]")
{
    // A modifier over half of the cube changes only the parameters, so without them in the fill group key both halves
    // would merge and print with the parameters of one.
    auto sparse_length = [](const std::string &object_params, const std::string &modifier_params) {
        Model model;
        ModelObject *object = model.add_object();
        object->add_volume(cube(20.));
        object->config.set_key_value("infill_plugin_params", new ConfigOptionString(object_params));
        ModelVolume *modifier = object->add_volume(make_cube(10., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER, false);
        modifier->config.set_key_value("infill_plugin_params", new ConfigOptionString(modifier_params));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        object->ensure_on_bed();
        Print print;
        print.apply(model, sample_config());
        print.process();
        return fill_shape(print).length;
    };
    const std::string narrow = "org.orcaslicer.sample.lines/spacing_scale=1", wide = "org.orcaslicer.sample.lines/spacing_scale=2";
    const double all_narrow = sparse_length(narrow, narrow);
    const double all_wide   = sparse_length(wide, wide);
    const double mixed      = sparse_length(narrow, wide);
    REQUIRE(all_wide < 0.6 * all_narrow);
    CHECK(mixed > 1.15 * all_wide);
    CHECK(mixed < 0.9 * all_narrow);
}

TEST_CASE("Infill plugin: multiline copies are drawn by the host", "[InfillPlugin]")
{
    CHECK(infill_pattern(sample_pattern()).has(ipfMultiline));
    DynamicPrintConfig doubled = sample_config();
    doubled.set_deserialize_strict("fill_multiline", "2");
    const FillShape single = sliced_shape(sample_config());
    const FillShape copies = sliced_shape(doubled);
    // The pattern lines are twice as far apart and each is drawn twice, so the density stays the same.
    CHECK(copies.digest != single.digest);
    CHECK(copies.length > 0.8 * single.length);
    CHECK(copies.length < 1.25 * single.length);

    // A pattern without CAP_MULTILINE is filled as with a single line.
    REQUIRE(load_once(INFILL_FIXTURE_INSET).error.empty());
    DynamicPrintConfig tripled = sample_config("", "org.orcaslicer.test.inset");
    tripled.set_deserialize_strict("fill_multiline", "3");
    const FillShape capless = sliced_shape(sample_config("", "org.orcaslicer.test.inset"));
    CHECK(capless.paths > 0);
    CHECK(sliced_shape(tripled).digest == capless.digest);
}

TEST_CASE("Infill plugin: the host services offset and clip regions with holes", "[InfillPlugin]")
{
    const ModuleLoad &load = load_once(INFILL_FIXTURE_INSET);
    INFO(load.error);
    REQUIRE(load.patterns == std::vector<std::string>{ "org.orcaslicer.test.inset" });
    // A 40 mm cube with a 20 mm square hole through it. Counts the sparse infill points, and those closer than 1.5 mm
    // to the outline or the hole of the infill area.
    TriangleMesh hollow = mesh(TestMesh::cube_with_hole);
    hollow.scale(2.f);
    auto count_points = [&hollow](const std::string &pattern) {
        Print print;
        init_and_process_print({ hollow }, print, sample_config("", pattern));
        size_t points = 0, near_edge = 0;
        for (const Layer *layer : print.objects().front()->layers()) {
            ExPolygons area;
            for (const LayerRegion *region : layer->regions())
                append(area, region->fill_expolygons);
            ExPolygons inner = offset_ex(area, -float(scale_(1.5)));
            for_each_fill_path(*layer, [&](const ExtrusionPath &path) {
                if (path.role() != erInternalInfill)
                    return;
                for (const auto &p : path.polyline.points) {
                    ++ points;
                    near_edge += ! expolygons_contain(inner, Point(p.x(), p.y()));
                }
            });
        }
        return std::make_pair(points, near_edge);
    };
    // The fixture keeps 2 mm away from the edges; the sample's lines are connected along them.
    const auto [points, near_edge] = count_points("org.orcaslicer.test.inset");
    CHECK(points > 0);
    CHECK(near_edge == 0);
    CHECK(count_points(sample_key).second > 0);
}

TEST_CASE("Infill plugin: surface patterns are told the role of a request", "[InfillPlugin]")
{
    const ModuleLoad &load = load_once(INFILL_FIXTURE_TOP_ONLY);
    INFO(load.error);
    REQUIRE(load.patterns == std::vector<std::string>{ "org.orcaslicer.test.toponly" });
    CHECK(! (InfillPatternRegistry::find("org.orcaslicer.test.toponly")->slots & ipsSparse));

    DynamicPrintConfig config = sample_config();
    config.set_deserialize_strict({ { "top_surface_pattern", "org.orcaslicer.test.toponly" },
                                    { "bottom_surface_pattern", "org.orcaslicer.test.toponly" },
                                    { "internal_solid_infill_pattern", "org.orcaslicer.test.toponly" } });
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    CHECK(fill_shape(print, erTopSolidInfill).paths > 0);
    CHECK(fill_shape(print, erBottomSurface).paths == 0);
}

TEST_CASE("Infill plugin: anchors follow the printed infill on a raft", "[InfillPlugin]")
{
    // Staggered lines depend on the layer number, which counts raft layers.
    DynamicPrintConfig config = sample_config("org.orcaslicer.sample.lines/phase=staggered");
    config.set_deserialize_strict("raft_layers", "3");
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    size_t printed = 0, matched = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        if (fill_shape(*layer).paths == 0)
            continue;
        std::set<std::pair<coord_t, coord_t>> anchor_points;
        for (const Polyline &polyline : layer->generate_sparse_infill_polylines_for_anchoring(nullptr, nullptr))
            for (const Point &p : polyline.points)
                anchor_points.emplace(p.x(), p.y());
        for_each_fill_path(*layer, [&](const ExtrusionPath &path) {
            if (path.role() != erInternalInfill)
                return;
            for (const auto &p : path.polyline.points) {
                ++ printed;
                matched += anchor_points.count({ p.x(), p.y() });
            }
        });
    }
    REQUIRE(printed > 0);
    CHECK(double(matched) > 0.9 * double(printed));
}

TEST_CASE("Infill plugin: failures of a pattern", "[InfillPlugin]")
{
    SECTION("a region the pattern cannot fill stays empty") {
        const ModuleLoad &load = load_once(INFILL_FIXTURE_FAILS_ODD_LAYERS);
        INFO(load.error);
        REQUIRE(load.patterns == std::vector<std::string>{ "org.orcaslicer.test.fails" });
        Print print;
        init_and_process_print({ cube(20) }, print, sample_config("", "org.orcaslicer.test.fails"));
        size_t even_filled = 0;
        for (const Layer *layer : print.objects().front()->layers()) {
            const FillShape shape = fill_shape(*layer);
            if (layer->id() % 2 == 1)
                CHECK(shape.paths == 0);
            else if (shape.paths > 0)
                ++ even_filled;
        }
        CHECK(even_filled > 0);
    }
    SECTION("an error ends the slice and names the plugin") {
        const ModuleLoad &load = load_once(INFILL_FIXTURE_THROWS);
        INFO(load.error);
        REQUIRE(load.patterns == std::vector<std::string>{ "org.orcaslicer.test.throws" });
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, sample_config("", "org.orcaslicer.test.throws"));
        std::string message;
        try {
            print.process();
        } catch (const SlicingError &error) {
            message = error.what();
        }
        CHECK_THAT(message, Catch::Matchers::ContainsSubstring("org.orcaslicer.test.throws"));
        CHECK_THAT(message, Catch::Matchers::ContainsSubstring("OrcaSlicer test fixture 1.0.0"));
    }
}

TEST_CASE("Infill plugin: broken plugins are rejected", "[InfillPlugin]")
{
    auto expect_rejected = [](const ModuleLoad &load, const char *reason) {
        CAPTURE(load.path, load.error);
        CHECK_THAT(load.error, Catch::Matchers::ContainsSubstring(reason));
        CHECK(load.patterns.empty());
        CHECK(std::any_of(module_load_failures().begin(), module_load_failures().end(),
                          [&load](const ModuleLoad &failure) { return failure.path == load.path; }));
    };
    expect_rejected(load_once(INFILL_FIXTURE_WRONG_ABI), "ABI 2");
    expect_rejected(load_once(INFILL_FIXTURE_NO_ENTRY), "not a module or infill plugin");
    expect_rejected(load_once(INFILL_FIXTURE_BAD_KEY), "nodot: the token must be two or more parts");
    expect_rejected(load_once(INFILL_FIXTURE_DUPLICATE_KEY), "already in use");
    expect_rejected(load_once(INFILL_FIXTURE_CONNECT_WITHOUT_CLIP), "ORCA_INFILL_POST_CONNECT needs ORCA_INFILL_POST_CLIP");
    expect_rejected(load_once(INFILL_FIXTURE_BAD_PARAM), "parameter count needs integral values within the range of a 32-bit integer");
    expect_rejected(load_once(INFILL_FIXTURE_DECLINES), "the plugin declined OrcaSlicer");
    CHECK(InfillPatternRegistry::find("nodot") == nullptr);
    CHECK(InfillPatternRegistry::find("org.orcaslicer.test.connect") == nullptr);

    ScopedTemporaryDir dir("orca_infill_plugin");
#ifdef _WIN32
    const boost::filesystem::path text_file = dir.path() / "x.dll";
#else
    const boost::filesystem::path text_file = dir.path() / "x.so";
#endif
    boost::nowide::ofstream(utf8(text_file)) << "not a library";
    expect_rejected(load_module(utf8(text_file)), "cannot load the library");

    // A file loaded before is skipped quietly.
    const ModuleLoad again = load_module(SAMPLE_INFILL_PLUGIN);
    CHECK(again.error.empty());
    CHECK(again.patterns.empty());

    // A copy at a path with non-ASCII characters loads, and its token is taken.
    const boost::filesystem::path copy_dir = dir.path() / from_utf8("Ger\xc3\xa4te");
    boost::filesystem::create_directories(copy_dir);
    const boost::filesystem::path copy = copy_dir / from_utf8(SAMPLE_INFILL_PLUGIN).filename();
    boost::filesystem::copy_file(from_utf8(SAMPLE_INFILL_PLUGIN), copy);
    const ModuleLoad copied = load_module(utf8(copy));
    CHECK(copied.plugin == "OrcaSlicer sample infill patterns 1.0.0");
    expect_rejected(copied, "already in use");

#ifdef __linux__
    // A hard link to a loaded plugin is the same library, so it is skipped quietly as well.
    const boost::filesystem::path alias = dir.path() / ("alias" + copy.extension().string());
    boost::system::error_code ec;
    boost::filesystem::create_hard_link(copy, alias, ec);
    if (! ec) {
        const ModuleLoad aliased = load_module(utf8(alias));
        CHECK(aliased.error.empty());
        CHECK(aliased.plugin.empty());
    }
#endif

    // A directory scan finds the plugin files of a directory and skips the files already loaded.
    const std::vector<ModuleLoad> scanned = load_modules({ utf8(from_utf8(SAMPLE_INFILL_PLUGIN).parent_path()), utf8(copy_dir) });
    CHECK(scanned.empty());
}

TEST_CASE("Infill plugin: a directory scan reports a library without the entry unless another library uses it", "[InfillPlugin]")
{
    // Two copies of a library without the entry, in directories of their own. One is kept loaded, as a library a plugin
    // depends on would be.
    ScopedTemporaryDir dir("orca_infill_plugin");
    const boost::filesystem::path fixture = from_utf8(INFILL_FIXTURE_NO_ENTRY);
    const boost::filesystem::path lone    = dir.path() / "lone" / ("lone" + fixture.extension().string());
    const boost::filesystem::path used    = dir.path() / "used" / ("used" + fixture.extension().string());
    for (const boost::filesystem::path &copy : { lone, used }) {
        boost::filesystem::create_directories(copy.parent_path());
        boost::filesystem::copy_file(fixture, copy);
    }
    const boost::dll::shared_library dependency(boost::filesystem::canonical(used));
    const std::vector<ModuleLoad> scanned = load_modules({ utf8(lone.parent_path()), utf8(used.parent_path()) });
    REQUIRE(scanned.size() == 1);
    CHECK(from_utf8(scanned.front().path).filename() == lone.filename());
    CHECK_THAT(scanned.front().error, Catch::Matchers::ContainsSubstring("not a module or infill plugin"));
}

TEST_CASE("Infill plugin: a pattern whose plugin is missing is substituted", "[InfillPlugin]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("infill_plugin_params", "org.missing.pattern/x = 1");
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    config.set_deserialize("sparse_infill_pattern", "org.missing.pattern", substitutions);
    REQUIRE(substitutions.substitutions.size() == 1);
    CHECK(substitutions.substitutions.front().old_value == "org.missing.pattern");
    CHECK(config.opt_serialize("sparse_infill_pattern") == "crosshatch");
    CHECK_THROWS_AS(config.set_deserialize_strict("sparse_infill_pattern", "org.missing.pattern"), BadOptionValueException);
    CHECK(config.opt_string("infill_plugin_params") == "org.missing.pattern/x = 1");
}

TEST_CASE("Infill plugin: patterns and parameters survive a 3MF round trip", "[InfillPlugin][3mf]")
{
    const std::string params = "org.orcaslicer.sample.lines/spacing_scale = 2\norg.orcaslicer.sample.lines/cross = 1";
    ScopedSlic3rTemporaryDir temporary_dir("orca_infill_plugin");
    const std::string path = (temporary_dir.path() / "stored.3mf").string();

    Model model;
    model.set_backup_path((temporary_dir.path() / "backup").string());
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    object->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(sample_pattern()));
    object->config.set_key_value("infill_plugin_params", new ConfigOptionString(params));
    DynamicPrintConfig config = sample_config(params);
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
    CHECK(substitutions.substitutions.empty());
    CHECK(reloaded.opt_serialize("sparse_infill_pattern") == sample_key);
    CHECK(reloaded.opt_string("infill_plugin_params") == params);
    REQUIRE(reloaded_model.objects.size() == 1);
    const auto &object_config = reloaded_model.objects.front()->config;
    CHECK(object_config.opt_serialize("top_surface_pattern") == sample_key);
    CHECK(object_config.opt_serialize("infill_plugin_params") == ConfigOptionString(params).serialize());
}

TEST_CASE("Infill plugin: infill_plugin_params is in the G-code config block only when set", "[InfillPlugin]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("layer_height", "0.2");
    CHECK(slice({ cube(20) }, config).find("; infill_plugin_params") == std::string::npos);
    config.set_deserialize_strict("infill_plugin_params", "org.orcaslicer.sample.lines/cross=1");
    CHECK(slice({ cube(20) }, config).find("; infill_plugin_params = org.orcaslicer.sample.lines/cross=1\n") != std::string::npos);
}

TEST_CASE("Infill plugin: fill groups sort plugin patterns by token", "[InfillPlugin]")
{
    for (const InfillPatternDescriptor &row : InfillPatternRegistry::builtins())
        CHECK(InfillPatternRegistry::order(row.id) == unsigned(row.id));
    CHECK(InfillPatternRegistry::order(sample_pattern()) >= unsigned(ipCount));

    // Loaded against token order: "throws" before "fails".
    REQUIRE(load_once(INFILL_FIXTURE_THROWS).error.empty());
    REQUIRE(load_once(INFILL_FIXTURE_FAILS_ODD_LAYERS).error.empty());
    std::vector<const InfillPatternDescriptor*> plugins;
    for (const char *key : { "org.orcaslicer.sample.lines", "org.orcaslicer.test.fails", "org.orcaslicer.test.throws" }) {
        plugins.push_back(InfillPatternRegistry::find(key));
        REQUIRE(plugins.back() != nullptr);
    }
    for (const InfillPatternDescriptor *a : plugins)
        for (const InfillPatternDescriptor *b : plugins)
            CHECK((InfillPatternRegistry::order(a->id) < InfillPatternRegistry::order(b->id)) == (std::strcmp(a->key, b->key) < 0));
}
