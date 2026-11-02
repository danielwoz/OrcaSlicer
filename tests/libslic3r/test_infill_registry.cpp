#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include "libslic3r/Fill/FillRectilinear.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r_infill_patterns.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {

// The tokens and labels of the built-in patterns, in InfillPattern order.
const std::vector<std::string> frozen_tokens{
    "monotonic", "monotonicline", "rectilinear", "alignedrectilinear", "zigzag", "crosszag", "lockedzag", "line", "grid",
    "triangles", "tri-hexagon", "cubic", "adaptivecubic", "quartercubic", "supportcubic", "lightning", "honeycomb",
    "3dhoneycomb", "lateral-honeycomb", "lateral-lattice", "crosshatch", "tpmsd", "tpmsfk", "gyroid", "concentric",
    "spiralinset", "hilbertcurve", "archimedeanchords", "octagramspiral"};
const std::vector<std::string> frozen_labels{
    "Monotonic", "Monotonic line", "Rectilinear", "Aligned Rectilinear", "Zig Zag", "Cross Zag", "Locked Zag", "Line", "Grid",
    "Triangles", "Tri-hexagon", "Cubic", "Adaptive Cubic", "Quarter Cubic", "Support Cubic", "Lightning", "Honeycomb",
    "3D Honeycomb", "Lateral Honeycomb", "Lateral Lattice", "Cross Hatch", "TPMS-D", "TPMS-FK", "Gyroid", "Concentric",
    "Spiral Inset", "Hilbert Curve", "Archimedean Chords", "Octagram Spiral"};

// The value and label lists of the pattern options.
const std::vector<std::string> frozen_sparse_values{
    "rectilinear", "alignedrectilinear", "zigzag", "crosszag", "lockedzag", "line", "grid", "triangles", "tri-hexagon",
    "cubic", "adaptivecubic", "quartercubic", "supportcubic", "lightning", "honeycomb", "3dhoneycomb", "lateral-honeycomb",
    "lateral-lattice", "crosshatch", "tpmsd", "tpmsfk", "gyroid", "concentric", "hilbertcurve", "archimedeanchords",
    "octagramspiral"};
const std::vector<std::string> frozen_sparse_labels{
    "Rectilinear", "Aligned Rectilinear", "Zig Zag", "Cross Zag", "Locked Zag", "Line", "Grid", "Triangles", "Tri-hexagon",
    "Cubic", "Adaptive Cubic", "Quarter Cubic", "Support Cubic", "Lightning", "Honeycomb", "3D Honeycomb", "Lateral Honeycomb",
    "Lateral Lattice", "Cross Hatch", "TPMS-D", "TPMS-FK", "Gyroid", "Concentric", "Hilbert Curve", "Archimedean Chords",
    "Octagram Spiral"};
const std::vector<std::string> frozen_surface_values{
    "monotonic", "monotonicline", "rectilinear", "alignedrectilinear", "concentric", "spiralinset", "hilbertcurve",
    "archimedeanchords", "octagramspiral"};
const std::vector<std::string> frozen_surface_labels{
    "Monotonic", "Monotonic line", "Rectilinear", "Aligned Rectilinear", "Concentric", "Spiral Inset", "Hilbert Curve",
    "Archimedean Chords", "Octagram Spiral"};
const std::vector<std::string> frozen_ironing_values{"rectilinear", "concentric"};
const std::vector<std::string> frozen_ironing_labels{"Rectilinear", "Concentric"};

struct FrozenOption
{
    const char                     *key;
    const std::vector<std::string> &values;
    const std::vector<std::string> &labels;
    InfillPattern                   default_value;
};
const std::vector<FrozenOption> frozen_options{
    {"sparse_infill_pattern", frozen_sparse_values, frozen_sparse_labels, ipCrossHatch},
    {"top_surface_pattern", frozen_surface_values, frozen_surface_labels, ipMonotonicLine},
    {"bottom_surface_pattern", frozen_surface_values, frozen_surface_labels, ipMonotonic},
    {"internal_solid_infill_pattern", frozen_surface_values, frozen_surface_labels, ipMonotonic},
    {"ironing_pattern", frozen_ironing_values, frozen_ironing_labels, ipRectilinear},
    {"support_ironing_pattern", frozen_ironing_values, frozen_ironing_labels, ipRectilinear}};

using Patterns = std::set<InfillPattern>;

// Pattern traits of the built-in patterns.
const Patterns frozen_separable{ipMonotonic, ipMonotonicLine, ipRectilinear, ipAlignedRectilinear, ipZigZag, ipCrossZag,
                                ipLockedZag, ipLine, ipGrid, ipTriangles, ipStars, ipCubic, ipQuarterCubic, ipHoneycomb,
                                ip3DHoneycomb, ipLateralHoneycomb, ipLateralLattice, ipCrossHatch, ipTpmsD, ipTpmsFK, ipGyroid,
                                ipHilbertCurve, ipArchimedeanChords, ipOctagramSpiral};
const Patterns frozen_octree{ipAdaptiveCubic, ipSupportCubic};
const Patterns frozen_adaptive{ipGyroid, ipTpmsD, ipTpmsFK};
const Patterns frozen_smoothable{ipHilbertCurve, ipOctagramSpiral, ipLightning, ipHoneycomb, ip3DHoneycomb, ipConcentric, ipCrossHatch};
const Patterns frozen_smoothable_multiline{ipGrid, ipTriangles, ipStars, ipCubic};
const Patterns frozen_multiline{ipGyroid, ipGrid, ipRectilinear, ipTpmsD, ipTpmsFK, ipCrossHatch, ipHoneycomb, ipLateralLattice,
                                ipLateralHoneycomb, ipConcentric, ipCubic, ipStars, ipAlignedRectilinear, ipLightning, ip3DHoneycomb,
                                ipAdaptiveCubic, ipSupportCubic, ipTriangles, ipQuarterCubic, ipArchimedeanChords, ipHilbertCurve,
                                ipOctagramSpiral};
const Patterns frozen_no_anchors{ipConcentric, ipSpiralInset};
const Patterns frozen_no_anchor_max{ipLine};
const Patterns frozen_fill_order{ipConcentric, ipSpiralInset, ipArchimedeanChords, ipOctagramSpiral};
const Patterns frozen_centered_surface{ipArchimedeanChords, ipOctagramSpiral};
const Patterns frozen_line_based_solid{ipRectilinear, ipMonotonic, ipMonotonicLine, ipAlignedRectilinear};
const Patterns frozen_arachne{ipConcentric, ipSpiralInset, ipConcentricInternal};
const Patterns frozen_no_reverse{ipGrid};
const Patterns frozen_rotate_template_safe{ipRectilinear, ipLine, ipZigZag, ipCrossZag, ipLockedZag};
const Patterns frozen_small_area_flow_comp{ipRectilinear, ipAlignedRectilinear, ipMonotonic, ipMonotonicLine};
const Patterns frozen_no_surface_retract{ipHilbertCurve};
const Patterns frozen_curved_bridge_anchors{ipHilbertCurve, ipOctagramSpiral};
const Patterns frozen_void_area_4{ipGrid, ipLateralLattice, ipHoneycomb, ipLightning};
const Patterns frozen_void_area_4_5{ipCubic, ipAdaptiveCubic, ipTriangles, ipStars, ipSupportCubic};
const Patterns frozen_clearance_1_5{ipRectilinear, ipMonotonic, ipGrid, ipLateralLattice, ipLine, ipHoneycomb, ipLateralHoneycomb};
const std::map<InfillPattern, std::set<std::string>> frozen_owned_options{
    {ipZigZag, {"symmetric_infill_y_axis"}},
    {ipCrossZag, {"infill_shift_step", "symmetric_infill_y_axis"}},
    {ipLockedZag, {"infill_shift_step", "symmetric_infill_y_axis", "skeleton_infill_density", "skin_infill_density",
                   "infill_lock_depth", "skin_infill_depth", "skin_infill_line_width", "skeleton_infill_line_width"}},
    {ipLightning, {"lightning_overhang_angle", "lightning_prune_angle", "lightning_straightening_angle"}},
    {ip3DHoneycomb, {"infill_complete_top"}},
    {ipLateralHoneycomb, {"infill_overhang_angle"}},
    {ipLateralLattice, {"lateral_lattice_angle_1", "lateral_lattice_angle_2"}},
    {ipGyroid, {"gyroid_optimized"}}};

// The patterns a build may leave out, with their fallbacks and CMake options.
struct FrozenOptional
{
    InfillPattern pattern;
    InfillPattern fallback;
    bool          built;
};
const std::vector<FrozenOptional> frozen_optional{
    {ipZigZag, ipRectilinear, ORCA_INFILL_ZIGZAG != 0},
    {ipCrossZag, ipRectilinear, ORCA_INFILL_CROSSZAG != 0},
    {ipLockedZag, ipRectilinear, ORCA_INFILL_LOCKEDZAG != 0},
    {ipLine, ipRectilinear, ORCA_INFILL_LINE != 0},
    {ipStars, ipTriangles, ORCA_INFILL_TRIHEXAGON != 0},
    {ipQuarterCubic, ipCubic, ORCA_INFILL_QUARTERCUBIC != 0},
    {ipSupportCubic, ipAdaptiveCubic, ORCA_INFILL_SUPPORTCUBIC != 0},
    {ipLateralHoneycomb, ipHoneycomb, ORCA_INFILL_LATERALHONEYCOMB != 0},
    {ipLateralLattice, ipGrid, ORCA_INFILL_LATERALLATTICE != 0},
    {ipTpmsD, ipGyroid, ORCA_INFILL_TPMSD != 0},
    {ipTpmsFK, ipGyroid, ORCA_INFILL_TPMSFK != 0},
    {ipHilbertCurve, ipRectilinear, ORCA_INFILL_HILBERTCURVE != 0},
    {ipOctagramSpiral, ipArchimedeanChords, ORCA_INFILL_OCTAGRAMSPIRAL != 0}};

const FrozenOptional *find_optional(InfillPattern pattern)
{
    for (const FrozenOptional &optional : frozen_optional)
        if (optional.pattern == pattern)
            return &optional;
    return nullptr;
}

// The pattern options PrintRegionConfig holds.
const std::vector<std::string> region_pattern_options{"sparse_infill_pattern", "top_surface_pattern", "bottom_surface_pattern",
                                                      "internal_solid_infill_pattern", "ironing_pattern"};

// The first pattern option offering the token.
std::string option_offering(const std::string &token)
{
    for (const FrozenOption &option : frozen_options)
        if (std::find(option.values.begin(), option.values.end(), token) != option.values.end())
            return option.key;
    return {};
}

} // namespace

TEST_CASE("Infill pattern rows describe the built-in patterns in InfillPattern order", "[InfillRegistry]")
{
    const auto &builtins = InfillPatternRegistry::builtins();
    REQUIRE(builtins.size() == size_t(ipCount));
    for (size_t i = 0; i < builtins.size(); ++i) {
        CAPTURE(i);
        const InfillPatternDescriptor &pattern = builtins[i];
        CHECK(size_t(pattern.id) == i);
        CHECK(InfillPatternRegistry::find(InfillPattern(i)) == &pattern);
        CHECK(&infill_pattern(InfillPattern(i)) == &pattern);
        if (i < frozen_tokens.size()) {
            REQUIRE_FALSE(pattern.internal());
            CHECK(pattern.key == frozen_tokens[i]);
            CHECK(pattern.label == frozen_labels[i]);
            CHECK(std::strchr(pattern.key, '.') == nullptr);
        } else {
            CHECK(pattern.internal());
            CHECK(pattern.label == nullptr);
            CHECK(pattern.slots == 0);
        }
    }
    CHECK(builtins[ipSupportBase].internal());
    CHECK(builtins[ipConcentricInternal].internal());
    // The first value past the patterns of this build names none.
    const InfillPattern none = InfillPattern(InfillPatternRegistry::patterns().size());
    CHECK(InfillPatternRegistry::find(none) == nullptr);
    CHECK(infill_pattern(none).internal());
    CHECK(infill_pattern(none).flags == 0);
    CHECK_FALSE(infill_pattern(none).available());
}

TEST_CASE("Infill pattern options list the built-in patterns in InfillPattern order", "[InfillRegistry]")
{
    for (const FrozenOption &frozen : frozen_options) {
        CAPTURE(frozen.key);
        const ConfigOptionDef *def = print_config_def.get(frozen.key);
        REQUIRE(def != nullptr);
        CHECK(def->enum_keys_map == &ConfigOptionEnum<InfillPattern>::get_enum_values());
        // The patterns with an enum value, in list order; the patterns of appended modules are listed among them.
        REQUIRE(def->enum_labels.size() == def->enum_values.size());
        std::vector<std::string> values, labels;
        for (size_t i = 0; i < def->enum_values.size(); ++i)
            if (! InfillPatternRegistry::find(def->enum_values[i])->appended()) {
                values.emplace_back(def->enum_values[i]);
                labels.emplace_back(def->enum_labels[i]);
            }
        CHECK(values == frozen.values);
        CHECK(labels == frozen.labels);
        REQUIRE(def->default_value.get() != nullptr);
        CHECK(def->default_value->getInt() == int(frozen.default_value));
    }

    t_config_enum_values builtin_keys;
    for (const auto &[key, value] : ConfigOptionEnum<InfillPattern>::get_enum_values())
        if (value < int(ipCount))
            builtin_keys.emplace(key, value);
    t_config_enum_values frozen_keys;
    for (size_t i = 0; i < frozen_tokens.size(); ++i)
        frozen_keys.emplace(frozen_tokens[i], int(i));
    CHECK(builtin_keys == frozen_keys);

    const t_config_enum_names &names = ConfigOptionEnum<InfillPattern>::get_enum_names();
    REQUIRE(names.size() >= frozen_tokens.size());
    CHECK(std::vector<std::string>(names.begin(), names.begin() + frozen_tokens.size()) == frozen_tokens);

    for (const auto &[key, slot] : InfillPatternRegistry::options()) {
        CAPTURE(key);
        const auto frozen = std::find_if(frozen_options.begin(), frozen_options.end(),
                                         [key = key](const FrozenOption &option) { return std::strcmp(option.key, key) == 0; });
        REQUIRE(frozen != frozen_options.end());
        std::vector<std::string> in_slot;
        for (const InfillPatternDescriptor &pattern : InfillPatternRegistry::builtins())
            if (pattern.slots & slot)
                in_slot.emplace_back(pattern.key);
        CHECK(in_slot == frozen->values);
    }
}

TEST_CASE("Infill pattern traits of the built-in patterns", "[InfillRegistry]")
{
    auto in = [](const Patterns &patterns, InfillPattern p) { return patterns.count(p) != 0; };
    std::set<std::string> owned_option_keys;
    for (const auto &[pattern, keys] : frozen_owned_options)
        owned_option_keys.insert(keys.begin(), keys.end());

    for (int i = 0; i < int(ipCount); ++i) {
        const InfillPattern            p       = InfillPattern(i);
        const InfillPatternDescriptor &pattern = infill_pattern(p);
        CAPTURE(i);
        CHECK(pattern.has(ipfSeparable) == in(frozen_separable, p));
        CHECK(pattern.has(ipfOctree) == in(frozen_octree, p));
        CHECK(pattern.has(ipfAdaptiveDensity) == in(frozen_adaptive, p));
        CHECK(pattern.smoothable(1) == in(frozen_smoothable, p));
        CHECK(pattern.smoothable(2) == (in(frozen_smoothable, p) || in(frozen_smoothable_multiline, p)));
        CHECK(pattern.has(ipfMultiline) == in(frozen_multiline, p));
        CHECK(pattern.has(ipfNoAnchors) == in(frozen_no_anchors, p));
        CHECK(pattern.has(ipfNoAnchorMax) == in(frozen_no_anchor_max, p));
        CHECK(pattern.has(ipfFillOrder) == in(frozen_fill_order, p));
        CHECK(pattern.has(ipfCenteredSurface) == in(frozen_centered_surface, p));
        CHECK(pattern.has(ipfLineBasedSolid) == in(frozen_line_based_solid, p));
        CHECK(pattern.has(ipfArachne) == in(frozen_arachne, p));
        CHECK(pattern.has(ipfNoReverse) == in(frozen_no_reverse, p));
        CHECK(pattern.has(ipfRotateTemplateSafe) == in(frozen_rotate_template_safe, p));
        CHECK(pattern.has(ipfSmallAreaFlowComp) == in(frozen_small_area_flow_comp, p));
        CHECK(pattern.has(ipfNoSurfaceRetract) == in(frozen_no_surface_retract, p));
        CHECK(pattern.has(ipfCurvedBridgeAnchors) == in(frozen_curved_bridge_anchors, p));
        CHECK(pattern.void_area_factor == (in(frozen_void_area_4, p) ? 4. : in(frozen_void_area_4_5, p) ? 4.5 : 1.));
        CHECK(pattern.combine_clearance_factor == (in(frozen_clearance_1_5, p) ? 1.5f : 0.5f));
        const auto owned = frozen_owned_options.find(p);
        for (const std::string &key : owned_option_keys) {
            CAPTURE(key);
            CHECK(pattern.has_option(key.c_str()) == (owned != frozen_owned_options.end() && owned->second.count(key) != 0));
        }
        if (pattern.available()) {
            CHECK(is_separable_infill_pattern(p) == in(frozen_separable, p));
            CHECK(is_octree_infill_pattern(p) == in(frozen_octree, p));
            CHECK(is_adaptive_infill_pattern(p) == in(frozen_adaptive, p));
            CHECK(is_smoothable_infill_pattern(p) == in(frozen_smoothable, p));
            CHECK(is_smoothable_infill_pattern(p, 2) == (in(frozen_smoothable, p) || in(frozen_smoothable_multiline, p)));
        }
    }
}

TEST_CASE("Infill patterns a build may leave out fall back to a pattern every build has", "[InfillRegistry]")
{
    for (const InfillPatternDescriptor &pattern : InfillPatternRegistry::builtins()) {
        CAPTURE(int(pattern.id));
        const FrozenOptional *frozen = find_optional(pattern.id);
        CHECK(pattern.optional == (frozen != nullptr));
        if (! pattern.optional) {
            CHECK(pattern.fallback == pattern.id);
            CHECK(pattern.available());
            continue;
        }
        CHECK(pattern.fallback == frozen->fallback);
        const InfillPatternDescriptor *fallback = InfillPatternRegistry::find(pattern.fallback);
        REQUIRE(fallback != nullptr);
        CHECK_FALSE(fallback->optional);
        CHECK(fallback->available());
        CHECK((pattern.slots & ~fallback->slots) == 0);
        CHECK(&sliced_infill_pattern(pattern.id) == (pattern.available() ? &pattern : fallback));
    }
    for (const FrozenOption &option : frozen_options) {
        CAPTURE(option.key);
        CHECK_FALSE(infill_pattern(option.default_value).optional);
    }
}

TEST_CASE("Infill pattern availability follows the CMake options", "[InfillRegistry]")
{
    bool all_built = true;
    for (const InfillPatternDescriptor &pattern : InfillPatternRegistry::builtins()) {
        CAPTURE(int(pattern.id));
        const FrozenOptional *frozen = find_optional(pattern.id);
        const bool built = frozen == nullptr || frozen->built;
        CHECK(InfillPatternRegistry::find(pattern.id)->available() == built);
        all_built = all_built && built;
    }
    // Patterns without an enum value follow the CMake options of their modules.
    for (const InfillPatternDescriptor &pattern : InfillPatternRegistry::patterns())
        all_built = all_built && (! pattern.appended() || pattern.available());
    CHECK(InfillPatternRegistry::all_available() == all_built);
}

TEST_CASE("Infill pattern tokens load, validate and save unchanged", "[InfillRegistry]")
{
    for (const std::string &token : frozen_tokens) {
        CAPTURE(token);
        const std::string opt_key = option_offering(token);
        REQUIRE_FALSE(opt_key.empty());
        CAPTURE(opt_key);

        DynamicPrintConfig dynamic = DynamicPrintConfig::full_print_config();
        dynamic.set_deserialize_strict(opt_key, token);
        CHECK(dynamic.opt_serialize(opt_key) == token);
        CHECK(dynamic.validate().empty());

        FullPrintConfig full;
        full.set_deserialize_strict(opt_key, token);
        CHECK(full.opt_serialize(opt_key) == token);
        CHECK(full.option(opt_key)->getInt() == int(InfillPatternRegistry::find(token)->id));
    }

    DynamicPrintConfig legacy;
    for (const FrozenOption &option : frozen_options) {
        CAPTURE(option.key);
        legacy.set_deserialize_strict(option.key, "zig-zag");
        CHECK(legacy.opt_serialize(option.key) == "rectilinear");
    }
}

TEST_CASE("Infill patterns this build leaves out are sliced with their fallbacks", "[InfillRegistry]")
{
    for (const std::string &opt_key : region_pattern_options) {
        for (const std::string &token : print_config_def.get(opt_key)->enum_values) {
            CAPTURE(opt_key, token);
            const InfillPatternDescriptor *pattern = InfillPatternRegistry::find(token);
            REQUIRE(pattern != nullptr);
            PrintRegionConfig config;
            config.option(opt_key)->setInt(int(pattern->id));
            InfillPatternRegistry::resolve_unavailable(config);
            CHECK(config.option(opt_key)->getInt() == int(pattern->available() ? pattern->id : pattern->fallback));
        }
    }

    for (const auto &[opt_key, slot] : InfillPatternRegistry::options()) {
        for (const std::string &token : print_config_def.get(opt_key)->enum_values) {
            CAPTURE(opt_key, token);
            const InfillPatternDescriptor *pattern = InfillPatternRegistry::find(token);
            REQUIRE(pattern != nullptr);
            DynamicPrintConfig config;
            config.set_deserialize_strict(opt_key, token);
            const auto unavailable = InfillPatternRegistry::unavailable_in(config);
            if (pattern->available()) {
                CHECK(unavailable.empty());
            } else {
                REQUIRE(unavailable.size() == 1);
                CHECK(unavailable.front().first == opt_key);
                CHECK(unavailable.front().second == pattern);
            }
        }
    }
}

namespace {

// The module describing each InfillPattern enum value.
const std::map<InfillPattern, std::string> frozen_modules{
    {ipMonotonic, "rectilinear"}, {ipMonotonicLine, "rectilinear"}, {ipRectilinear, "rectilinear"},
    {ipAlignedRectilinear, "rectilinear"}, {ipZigZag, "zigzag"}, {ipCrossZag, "crosszag"}, {ipLockedZag, "lockedzag"},
    {ipLine, "line"}, {ipGrid, "rectilinear"}, {ipTriangles, "rectilinear"}, {ipStars, "trihexagon"}, {ipCubic, "rectilinear"},
    {ipAdaptiveCubic, "adaptivecubic"}, {ipQuarterCubic, "quartercubic"}, {ipSupportCubic, "supportcubic"},
    {ipLightning, "lightning"}, {ipHoneycomb, "honeycomb"}, {ip3DHoneycomb, "3dhoneycomb"},
    {ipLateralHoneycomb, "lateralhoneycomb"}, {ipLateralLattice, "laterallattice"}, {ipCrossHatch, "crosshatch"},
    {ipTpmsD, "tpmsd"}, {ipTpmsFK, "tpmsfk"}, {ipGyroid, "gyroid"}, {ipConcentric, "concentric"},
    {ipSpiralInset, "spiralinset"}, {ipHilbertCurve, "hilbertcurve"}, {ipArchimedeanChords, "planepath"},
    {ipOctagramSpiral, "octagramspiral"}, {ipSupportBase, "rectilinear"}, {ipConcentricInternal, "concentric"}};

// Modules of the builder tests. Their rows use a filler class every build has.
constexpr InfillPatternDescriptor test_zz_rows[] = {
    {ipCount, "zz-test", "ZZ test", ipsSparse, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>}};
constexpr InfillPatternDescriptor test_aa_rows[] = {
    {ipCount, "aa-test", "AA test", ipsSparse, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>}};
constexpr InfillPatternDescriptor test_zz_after_line_rows[] = {
    {ipCount, "zz-test", "ZZ test", ipsSparse, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>, "line"}};
constexpr InfillPatternDescriptor test_aa_after_line_rows[] = {
    {ipCount, "aa-test", "AA test", ipsSparse, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>, "line"}};
constexpr InfillPatternDescriptor test_after_appended_rows[] = {
    {ipCount, "bb-test", "BB test", ipsSparse, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>, "aa-test"}};
constexpr InfillPatternDescriptor test_gyroid_rows[] = {
    {ipGyroid, "gyroid-test", "Gyroid test", ipsSparse, ipfSeparable, 1., 0.5f, ipCount, nullptr, &make_fill<FillRectilinear>}};
constexpr InfillPatternDescriptor test_optional_fallback_rows[] = {
    {ipCount, "cc-test", "CC test", ipsSparse, ipfSeparable, 1., 0.5f, ipZigZag, nullptr, &make_fill<FillRectilinear>}};
constexpr InfillPatternDescriptor test_surface_fallback_rows[] = {
    {ipCount, "dd-test", "DD test", ipsSurface, ipfSeparable, 1., 0.5f, ipGrid, nullptr, &make_fill<FillRectilinear>}};
constexpr InfillPatternDescriptor test_no_factory_rows[] = {
    {ipCount, "ee-test", "EE test", ipsSparse, ipfSeparable, 1., 0.5f, ipCount, nullptr, nullptr}};
constexpr InfillRegionOption test_region_options[] = {{"test_option", InfillOptionStep::PrepareInfill, nullptr}};

// Modules ORCA_INFILL_MODULE() refuses at compile time.
constexpr InfillPatternDescriptor test_dotted_token_rows[] = {
    {ipCount, "zz.test", "ZZ test", ipsSparse, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>}};
constexpr InfillPatternDescriptor test_no_label_rows[] = {
    {ipCount, "zz-test", nullptr, ipsSparse, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>}};
constexpr InfillPatternDescriptor test_appended_ironing_rows[] = {
    {ipCount, "zz-test", "ZZ test", ipsIroning, ipfSeparable, 1., 0.5f, ipRectilinear, nullptr, &make_fill<FillRectilinear>}};
static_assert(valid_infill_module(InfillModule{"zztest", test_zz_rows, std::size(test_zz_rows)}));
static_assert(! valid_infill_module(InfillModule{"bad", test_dotted_token_rows, std::size(test_dotted_token_rows)}));
static_assert(! valid_infill_module(InfillModule{"bad", test_no_label_rows, std::size(test_no_label_rows)}));
static_assert(! valid_infill_module(InfillModule{"bad", test_appended_ironing_rows, std::size(test_appended_ironing_rows)}));
// Region options without the function that defines them.
static_assert(! valid_infill_module(InfillModule{"bad", test_zz_rows, std::size(test_zz_rows), test_region_options, std::size(test_region_options)}));

const InfillModule test_zz_module{"zztest", test_zz_rows, std::size(test_zz_rows)};
const InfillModule test_aa_module{"aatest", test_aa_rows, std::size(test_aa_rows)};
const InfillModule test_zz_after_line_module{"zztest", test_zz_after_line_rows, std::size(test_zz_after_line_rows)};
const InfillModule test_aa_after_line_module{"aatest", test_aa_after_line_rows, std::size(test_aa_after_line_rows)};
const InfillModule test_after_appended_module{"bbtest", test_after_appended_rows, std::size(test_after_appended_rows)};
const InfillModule test_gyroid_module{"gyroidtest", test_gyroid_rows, std::size(test_gyroid_rows)};
const InfillModule test_optional_fallback_module{"cctest", test_optional_fallback_rows, std::size(test_optional_fallback_rows)};
const InfillModule test_surface_fallback_module{"ddtest", test_surface_fallback_rows, std::size(test_surface_fallback_rows)};
const InfillModule test_no_factory_module{"eetest", test_no_factory_rows, std::size(test_no_factory_rows)};
const InfillModule test_option_module_1{"optiontest1", test_zz_rows, std::size(test_zz_rows), test_region_options, std::size(test_region_options)};
const InfillModule test_option_module_2{"optiontest2", test_aa_rows, std::size(test_aa_rows), test_region_options, std::size(test_region_options)};

// Builds the table of the modules of this build followed by the given ones; returns the error.
std::string build_with(std::initializer_list<detail::InfillModuleEntry> extra, detail::InfillPatternTable &table)
{
    std::vector<detail::InfillModuleEntry> entries(detail::infill_module_entries, detail::infill_module_entries + detail::infill_module_entry_count);
    entries.insert(entries.end(), extra);
    return detail::build_infill_pattern_table(entries.data(), entries.size(), table);
}

std::vector<std::string> slot_tokens(const detail::InfillPatternTable &table, InfillPatternSlot slot)
{
    std::vector<std::string> tokens;
    for (const InfillPatternDescriptor *pattern : detail::infill_slot_patterns(table, slot))
        tokens.emplace_back(pattern->key);
    return tokens;
}

} // namespace

TEST_CASE("Infill patterns belong to their modules", "[InfillRegistry]")
{
    for (int i = 0; i < int(ipCount); ++i) {
        CAPTURE(i);
        const InfillModule *module = InfillPatternRegistry::module_of(InfillPattern(i));
        REQUIRE(module != nullptr);
        CHECK(module->name == frozen_modules.at(InfillPattern(i)));
    }
    std::set<std::string> expected;
    for (const auto &[pattern, name] : frozen_modules)
        expected.insert(name);
    CHECK(expected.size() == 23);
    // Every built-in module is present; the other modules of this build add patterns without an enum value only.
    std::set<std::string> names;
    for (const InfillModule *module : InfillPatternRegistry::modules()) {
        CAPTURE(module->name);
        CHECK(names.insert(module->name).second);
        if (expected.count(module->name) == 0)
            for (size_t i = 0; i < module->pattern_count; ++i)
                CHECK(module->patterns[i].appended());
    }
    CHECK(std::includes(names.begin(), names.end(), expected.begin(), expected.end()));
    CHECK(InfillPatternRegistry::module_of(InfillPattern(InfillPatternRegistry::patterns().size())) == nullptr);
}

TEST_CASE("The generated infill module list builds a consistent table", "[InfillRegistry]")
{
    detail::InfillPatternTable table;
    REQUIRE(detail::build_infill_pattern_table(detail::infill_module_entries, detail::infill_module_entry_count, table).empty());
    REQUIRE(table.rows.size() == InfillPatternRegistry::patterns().size());
    for (size_t i = 0; i < detail::infill_module_entry_count; ++i) {
        const detail::InfillModuleEntry &entry = detail::infill_module_entries[i];
        CAPTURE(entry.module->name);
        for (size_t j = 0; j < entry.module->pattern_count; ++j) {
            const InfillPatternDescriptor &row = entry.module->patterns[j];
            if (row.appended())
                continue;
            const InfillPatternDescriptor &built = table.rows[size_t(row.id)];
            CHECK(built.optional == ! entry.core);
            CHECK((built.factory != nullptr) == entry.built);
            CHECK(InfillPatternRegistry::find(row.id)->available() == entry.built);
        }
    }
}

TEST_CASE("The infill module table places appended patterns and rejects contradicting modules", "[InfillRegistry]")
{
    detail::InfillPatternTable table;

    SECTION("appended patterns follow the enum values in token order") {
        REQUIRE(build_with({{&test_zz_module, false, true}, {&test_aa_module, false, true}}, table).empty());
        const InfillPattern aa = table.by_key.at("aa-test"), zz = table.by_key.at("zz-test");
        CHECK(int(aa) >= int(ipCount));
        CHECK(int(zz) > int(aa));
        for (size_t i = size_t(ipCount); i + 1 < table.rows.size(); ++i)
            CHECK(std::strcmp(table.rows[i].key, table.rows[i + 1].key) < 0);
        CHECK(table.rows[size_t(aa)].optional);
        CHECK(table.rows[size_t(aa)].fallback == ipRectilinear);
        const std::vector<std::string> sparse = slot_tokens(table, ipsSparse);
        const auto position = [&sparse](const std::string &token) { return std::find(sparse.begin(), sparse.end(), token) - sparse.begin(); };
        for (const std::string &token : frozen_sparse_values) {
            CHECK(position(token) < position("aa-test"));
            CHECK(position(token) < position("zz-test"));
        }
        CHECK(position("aa-test") < position("zz-test"));
    }
    SECTION("appended patterns with a place follow their anchor in token order") {
        REQUIRE(build_with({{&test_zz_after_line_module, false, true}, {&test_aa_after_line_module, false, true}}, table).empty());
        std::vector<std::string> after_line;
        for (const InfillPatternDescriptor &row : table.rows)
            if (row.menu_after != nullptr && std::strcmp(row.menu_after, "line") == 0)
                after_line.emplace_back(row.key);
        std::sort(after_line.begin(), after_line.end());
        const std::vector<std::string> sparse = slot_tokens(table, ipsSparse);
        const auto line = std::find(sparse.begin(), sparse.end(), "line");
        REQUIRE(sparse.end() - line > ptrdiff_t(after_line.size()));
        CHECK(std::vector<std::string>(line + 1, line + 1 + after_line.size()) == after_line);
        CHECK(std::find(after_line.begin(), after_line.end(), "aa-test") < std::find(after_line.begin(), after_line.end(), "zz-test"));
    }
    SECTION("an anchor must have an enum value") {
        const std::string error = build_with({{&test_aa_module, false, true}, {&test_after_appended_module, false, true}}, table);
        CHECK(error.find("menu_after of bb-test") != std::string::npos);
    }
    SECTION("an enum value belongs to one module") {
        const std::string error = build_with({{&test_gyroid_module, true, true}}, table);
        CHECK(error.find("gyroid") != std::string::npos);
        CHECK(error.find("gyroidtest") != std::string::npos);
    }
    SECTION("a token is used once") {
        const std::string error = build_with({{&test_zz_module, false, true}, {&test_zz_after_line_module, false, true}}, table);
        CHECK(error.find("the token zz-test is used twice") != std::string::npos);
    }
    SECTION("a fallback is always built") {
        const std::string error = build_with({{&test_optional_fallback_module, false, true}}, table);
        CHECK(error.find("the fallback of cc-test") != std::string::npos);
    }
    SECTION("a fallback is offered in every slot of its pattern") {
        const std::string error = build_with({{&test_surface_fallback_module, false, true}}, table);
        CHECK(error.find("the fallback of dd-test") != std::string::npos);
    }
    SECTION("a built module has factories") {
        const std::string error = build_with({{&test_no_factory_module, true, true}}, table);
        CHECK(error.find("the module eetest has no factory") != std::string::npos);
    }
    SECTION("a left-out module needs no factories") {
        CHECK(build_with({{&test_zz_module, false, false}}, table).empty());
        CHECK_FALSE(table.rows[size_t(table.by_key.at("zz-test"))].available());
        CHECK_FALSE(table.all_available);
    }
    SECTION("a region option is added once") {
        const std::string error = build_with({{&test_option_module_1, false, true}, {&test_option_module_2, false, true}}, table);
        CHECK(error.find("the option test_option is added twice") != std::string::npos);
    }
}

TEST_CASE("Options added by infill modules are complete region options", "[InfillRegistry]")
{
    const PrintRegionConfig        region;
    const std::vector<std::string> &preset_keys = Preset::print_options();
    // Each member REGION_OPTIONS adds in a module.cmake is a region option of the module source.
    for (const char *key : std::initializer_list<const char *>{ORCA_INFILL_MODULE_REGION_OPTION_KEYS}) {
        CAPTURE(key);
        CHECK(InfillPatternRegistry::region_option(key) != nullptr);
    }
    for (const auto &[option, module] : InfillPatternRegistry::region_options()) {
        CAPTURE(option->key, module->name);
        CHECK(std::string(option->key).rfind(std::string(module->name) + "_", 0) == 0);
        const ConfigOption *member = region.option(option->key);
        REQUIRE(member != nullptr);
        const ConfigOptionDef *def = print_config_def.get(option->key);
        REQUIRE(def != nullptr);
        CHECK(def->type == member->type());
        // Defined by the module alone: no print option of the slicer and no SLA option has its key.
        CHECK(def->printer_technology == ptFFF);
        CHECK(std::count(preset_keys.begin(), preset_keys.end(), option->key) == 1);
        bool owned = false;
        for (size_t i = 0; i < module->pattern_count; ++i)
            owned = owned || module->patterns[i].has_option(option->key);
        CHECK(owned);
    }
}

TEST_CASE("Infill pattern enum values add only module options to the fill grouping key", "[InfillRegistry]")
{
    const PrintRegionConfig region;
    for (int i = 0; i < int(ipCount); ++i) {
        CAPTURE(i);
        const InfillPatternDescriptor &pattern = *InfillPatternRegistry::find(InfillPattern(i));
        std::string expected;
        for (const char *const *key = pattern.options; key != nullptr && *key != nullptr; ++key)
            if (InfillPatternRegistry::region_option(*key) != nullptr) {
                REQUIRE(region.option(*key) != nullptr);
                expected += region.option(*key)->serialize() + "\n";
            }
        std::string params = "stale";
        InfillPatternRegistry::module_params(InfillPattern(i), region, params);
        CHECK(params == expected);
    }
}
