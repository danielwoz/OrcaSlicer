#include "InfillPatternRegistry.hpp"

#include <cstring>

#include "libslic3r_infill_patterns.h"      // generated: ORCA_INFILL_<NAME> 0/1
#include "../Exception.hpp"
#include "Fill3DHoneycomb.hpp"
#include "FillAdaptive.hpp"
#include "FillConcentric.hpp"
#include "FillConcentricInternal.hpp"
#include "FillCrossHatch.hpp"
#include "FillGyroid.hpp"
#include "FillHoneycomb.hpp"
#include "FillLightning.hpp"
#include "FillLine.hpp"
#include "FillPlanePath.hpp"
#include "FillRectilinear.hpp"
#include "FillSpiralInset.hpp"
#include "FillTpmsD.hpp"
#include "FillTpmsFK.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

template<class T> Fill* make_fill(const InfillPatternDescriptor &) { return new T(); }

// nullptr when the build leaves the pattern out. The discarded branch is never instantiated, so the class
// only needs to be declared and its implementation file may be left out of the build.
template<bool Built, class T> constexpr InfillFactory optional_fill()
{
    if constexpr (Built)
        return &make_fill<T>;
    else
        return nullptr;
}

constexpr const char *zigzag_options[]            = { "symmetric_infill_y_axis", nullptr };
constexpr const char *crosszag_options[]          = { "infill_shift_step", "symmetric_infill_y_axis", nullptr };
constexpr const char *lockedzag_options[]         = { "infill_shift_step", "symmetric_infill_y_axis", "skeleton_infill_density",
                                                      "skin_infill_density", "infill_lock_depth", "skin_infill_depth",
                                                      "skin_infill_line_width", "skeleton_infill_line_width", nullptr };
constexpr const char *lightning_options[]         = { "lightning_overhang_angle", "lightning_prune_angle",
                                                      "lightning_straightening_angle", nullptr };
constexpr const char *honeycomb_3d_options[]      = { "infill_complete_top", nullptr };
constexpr const char *lateral_honeycomb_options[] = { "infill_overhang_angle", nullptr };
constexpr const char *lateral_lattice_options[]   = { "lateral_lattice_angle_1", "lateral_lattice_angle_2", nullptr };
constexpr const char *gyroid_options[]            = { "gyroid_optimized", nullptr };

// Indexed by InfillPattern. The ordinals decide in which order fills are trimmed against each other and printed,
// and the slot lists keep this order, so rows are never reordered.
constexpr std::array<InfillPatternDescriptor, size_t(ipCount)> s_builtins {{
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, optional, fallback, options, factory
    { ipMonotonic, "monotonic", L("Monotonic"), ipsSurface,
      ipfSeparable | ipfLineBasedSolid | ipfSmallAreaFlowComp,
      1., 1.5f, false, ipMonotonic, nullptr, &make_fill<FillMonotonic> },
    { ipMonotonicLine, "monotonicline", L("Monotonic line"), ipsSurface,
      ipfSeparable | ipfLineBasedSolid | ipfSmallAreaFlowComp,
      1., 0.5f, false, ipMonotonicLine, nullptr, &make_fill<FillMonotonicLines> },
    { ipRectilinear, "rectilinear", L("Rectilinear"), ipsSparse | ipsSurface | ipsIroning,
      ipfSeparable | ipfMultiline | ipfLineBasedSolid | ipfRotateTemplateSafe | ipfSmallAreaFlowComp,
      1., 1.5f, false, ipRectilinear, nullptr, &make_fill<FillRectilinear> },
    { ipAlignedRectilinear, "alignedrectilinear", L("Aligned Rectilinear"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfLineBasedSolid | ipfSmallAreaFlowComp,
      1., 0.5f, false, ipAlignedRectilinear, nullptr, &make_fill<FillAlignedRectilinear> },
    { ipZigZag, "zigzag", L("Zig Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, true, ipRectilinear, zigzag_options, optional_fill<ORCA_INFILL_ZIGZAG != 0, FillZigZag>() },
    { ipCrossZag, "crosszag", L("Cross Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, true, ipRectilinear, crosszag_options, optional_fill<ORCA_INFILL_CROSSZAG != 0, FillCrossZag>() },
    { ipLockedZag, "lockedzag", L("Locked Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, true, ipRectilinear, lockedzag_options, optional_fill<ORCA_INFILL_LOCKEDZAG != 0, FillLockedZag>() },
    { ipLine, "line", L("Line"), ipsSparse,
      ipfSeparable | ipfNoAnchorMax | ipfRotateTemplateSafe,
      1., 1.5f, true, ipRectilinear, nullptr, optional_fill<ORCA_INFILL_LINE != 0, FillLine>() },
    { ipGrid, "grid", L("Grid"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline | ipfNoReverse,
      4., 1.5f, false, ipGrid, nullptr, &make_fill<FillGrid> },
    { ipTriangles, "triangles", L("Triangles"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, false, ipTriangles, nullptr, &make_fill<FillTriangles> },
    { ipStars, "tri-hexagon", L("Tri-hexagon"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, true, ipTriangles, nullptr, optional_fill<ORCA_INFILL_TRIHEXAGON != 0, FillStars>() },
    { ipCubic, "cubic", L("Cubic"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, false, ipCubic, nullptr, &make_fill<FillCubic> },
    { ipAdaptiveCubic, "adaptivecubic", L("Adaptive Cubic"), ipsSparse,
      ipfOctree | ipfMultiline,
      4.5, 0.5f, false, ipAdaptiveCubic, nullptr, &make_fill<FillAdaptive::Filler> },
    { ipQuarterCubic, "quartercubic", L("Quarter Cubic"), ipsSparse,
      ipfSeparable | ipfMultiline,
      1., 0.5f, true, ipCubic, nullptr, optional_fill<ORCA_INFILL_QUARTERCUBIC != 0, FillQuarterCubic>() },
    { ipSupportCubic, "supportcubic", L("Support Cubic"), ipsSparse,
      ipfOctree | ipfMultiline,
      4.5, 0.5f, true, ipAdaptiveCubic, nullptr, optional_fill<ORCA_INFILL_SUPPORTCUBIC != 0, FillAdaptive::Filler>() },
    { ipLightning, "lightning", L("Lightning"), ipsSparse,
      ipfMultiline | ipfSmoothable,
      4., 0.5f, false, ipLightning, lightning_options, &make_fill<FillLightning::Filler> },
    { ipHoneycomb, "honeycomb", L("Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      4., 1.5f, false, ipHoneycomb, nullptr, &make_fill<FillHoneycomb> },
    { ip3DHoneycomb, "3dhoneycomb", L("3D Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      1., 0.5f, false, ip3DHoneycomb, honeycomb_3d_options, &make_fill<Fill3DHoneycomb> },
    { ipLateralHoneycomb, "lateral-honeycomb", L("Lateral Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline,
      1., 1.5f, true, ipHoneycomb, lateral_honeycomb_options,
      optional_fill<ORCA_INFILL_LATERALHONEYCOMB != 0, FillLateralHoneycomb>() },
    { ipLateralLattice, "lateral-lattice", L("Lateral Lattice"), ipsSparse,
      ipfSeparable | ipfMultiline,
      4., 1.5f, true, ipGrid, lateral_lattice_options,
      optional_fill<ORCA_INFILL_LATERALLATTICE != 0, FillLateralLattice>() },
    { ipCrossHatch, "crosshatch", L("Cross Hatch"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      1., 0.5f, false, ipCrossHatch, nullptr, &make_fill<FillCrossHatch> },
    { ipTpmsD, "tpmsd", L("TPMS-D"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, true, ipGyroid, nullptr, optional_fill<ORCA_INFILL_TPMSD != 0, FillTpmsD>() },
    { ipTpmsFK, "tpmsfk", L("TPMS-FK"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, true, ipGyroid, nullptr, optional_fill<ORCA_INFILL_TPMSFK != 0, FillTpmsFK>() },
    { ipGyroid, "gyroid", L("Gyroid"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, false, ipGyroid, gyroid_options, &make_fill<FillGyroid> },
    { ipConcentric, "concentric", L("Concentric"), ipsSparse | ipsSurface | ipsIroning,
      ipfMultiline | ipfSmoothable | ipfNoAnchors | ipfFillOrder | ipfArachne,
      1., 0.5f, false, ipConcentric, nullptr, &make_fill<FillConcentric> },
    { ipSpiralInset, "spiralinset", L("Spiral Inset"), ipsSurface,
      ipfNoAnchors | ipfFillOrder | ipfArachne,
      1., 0.5f, false, ipSpiralInset, nullptr, &make_fill<FillSpiralInset> },
    { ipHilbertCurve, "hilbertcurve", L("Hilbert Curve"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfSmoothable | ipfNoSurfaceRetract | ipfCurvedBridgeAnchors,
      1., 0.5f, true, ipRectilinear, nullptr, optional_fill<ORCA_INFILL_HILBERTCURVE != 0, FillHilbertCurve>() },
    { ipArchimedeanChords, "archimedeanchords", L("Archimedean Chords"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfFillOrder | ipfCenteredSurface,
      1., 0.5f, false, ipArchimedeanChords, nullptr, &make_fill<FillArchimedeanChords> },
    { ipOctagramSpiral, "octagramspiral", L("Octagram Spiral"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfSmoothable | ipfFillOrder | ipfCenteredSurface | ipfCurvedBridgeAnchors,
      1., 0.5f, true, ipArchimedeanChords, nullptr, optional_fill<ORCA_INFILL_OCTAGRAMSPIRAL != 0, FillOctagramSpiral>() },
    // Used by the slicer only: support base, and narrow internal solid areas.
    { ipSupportBase, nullptr, nullptr, 0, 0,
      1., 0.5f, false, ipSupportBase, nullptr, &make_fill<FillSupportBase> },
    { ipConcentricInternal, nullptr, nullptr, 0, ipfArachne,
      1., 0.5f, false, ipConcentricInternal, nullptr, &make_fill<FillConcentricInternal> },
}};

// The descriptor of a value that names no pattern: void area factor 1, combine clearance factor 0.5.
constexpr InfillPatternDescriptor s_none { ipCount, nullptr, nullptr, 0, 0, 1., 0.5f, false, ipCount, nullptr, nullptr };

constexpr bool has_dot(const char *s)
{
    for (; s != nullptr && *s != 0; ++ s)
        if (*s == '.')
            return true;
    return false;
}

constexpr bool builtins_are_consistent()
{
    for (size_t i = 0; i < s_builtins.size(); ++ i) {
        const InfillPatternDescriptor &p = s_builtins[i];
        // Row i describes value i; only the slicer's own patterns have no token, label or slot; tokens of built-in
        // patterns contain no '.', which keeps them apart from the tokens of plugin patterns.
        if (size_t(p.id) != i || (p.key == nullptr) != (p.label == nullptr) || (p.key == nullptr) != (p.slots == 0) || has_dot(p.key))
            return false;
        if (! p.optional) {
            if (p.factory == nullptr || p.fallback != p.id)
                return false;
        } else {
            if (size_t(p.fallback) >= s_builtins.size())
                return false;
            const InfillPatternDescriptor &fallback = s_builtins[size_t(p.fallback)];
            if (fallback.optional || (p.slots & ~fallback.slots) != 0)
                return false;
        }
    }
    return true;
}
static_assert(builtins_are_consistent(), "infill pattern table: ids, tokens, factories and fallbacks");

constexpr bool builtins_all_available()
{
    for (const InfillPatternDescriptor &p : s_builtins)
        if (! p.available())
            return false;
    return true;
}

constexpr std::array<std::pair<const char*, InfillPatternSlot>, 6> s_options {{
    { "sparse_infill_pattern", ipsSparse }, { "top_surface_pattern", ipsSurface }, { "bottom_surface_pattern", ipsSurface },
    { "internal_solid_infill_pattern", ipsSurface }, { "ironing_pattern", ipsIroning }, { "support_ironing_pattern", ipsIroning } }};

} // namespace

bool InfillPatternDescriptor::has_option(const char *opt_key) const
{
    for (const char *const *it = options; it != nullptr && *it != nullptr; ++ it)
        if (std::strcmp(*it, opt_key) == 0)
            return true;
    return false;
}

const std::array<std::pair<const char*, InfillPatternSlot>, 6>& InfillPatternRegistry::options() { return s_options; }
const std::array<InfillPatternDescriptor, size_t(ipCount)>& InfillPatternRegistry::builtins() { return s_builtins; }

const InfillPatternDescriptor* InfillPatternRegistry::find(InfillPattern id)
{
    return unsigned(id) < unsigned(ipCount) ? &s_builtins[size_t(id)] : nullptr;
}

const InfillPatternDescriptor* InfillPatternRegistry::find(const std::string &key)
{
    const t_config_enum_values &keys = ConfigOptionEnum<InfillPattern>::get_enum_values();
    const auto it = keys.find(key);
    return it == keys.end() ? nullptr : find(InfillPattern(it->second));
}

t_config_enum_values InfillPatternRegistry::keys_map()
{
    t_config_enum_values keys;
    for (const InfillPatternDescriptor &pattern : s_builtins)
        if (! pattern.internal())
            keys.emplace(pattern.key, int(pattern.id));
    return keys;
}

void InfillPatternRegistry::define_enum_values(ConfigOptionDef &def, InfillPatternSlot slot)
{
    def.enum_values.clear();
    def.enum_labels.clear();
    for (const InfillPatternDescriptor &pattern : s_builtins)
        if (pattern.slots & slot) {
            def.enum_values.emplace_back(pattern.key);
            def.enum_labels.emplace_back(pattern.label);
        }
}

Fill* InfillPatternRegistry::create(InfillPattern id)
{
    const InfillPatternDescriptor &pattern = sliced_infill_pattern(id);
    if (pattern.factory == nullptr)
        throw Slic3r::InvalidArgument("unknown type");
    return pattern.factory(pattern);
}

bool InfillPatternRegistry::all_available() { return builtins_all_available(); }

void InfillPatternRegistry::resolve_unavailable(PrintRegionConfig &config)
{
    if (all_available())
        return;
    for (ConfigOptionEnum<InfillPattern> *opt : { &config.sparse_infill_pattern, &config.top_surface_pattern, &config.bottom_surface_pattern,
                                                  &config.internal_solid_infill_pattern, &config.ironing_pattern })
        if (const InfillPatternDescriptor *pattern = find(opt->value); pattern != nullptr && ! pattern->available())
            opt->value = pattern->fallback;
}

std::vector<std::pair<std::string, const InfillPatternDescriptor*>> InfillPatternRegistry::unavailable_in(const ConfigBase &config)
{
    std::vector<std::pair<std::string, const InfillPatternDescriptor*>> out;
    for (const auto &[opt_key, slot] : s_options)
        if (const ConfigOption *opt = config.option(opt_key); opt != nullptr)
            if (const InfillPatternDescriptor *pattern = find(InfillPattern(opt->getInt())); pattern != nullptr && ! pattern->available())
                out.emplace_back(opt_key, pattern);
    return out;
}

const InfillPatternDescriptor& infill_pattern(InfillPattern pattern)
{
    const InfillPatternDescriptor *found = InfillPatternRegistry::find(pattern);
    return found == nullptr ? s_none : *found;
}

const InfillPatternDescriptor& sliced_infill_pattern(InfillPattern pattern)
{
    const InfillPatternDescriptor *found = InfillPatternRegistry::find(pattern);
    return found == nullptr ? s_none : found->available() ? *found : *InfillPatternRegistry::find(found->fallback);
}

// The infill pattern traits declared in PrintConfig.hpp; they describe the pattern that is sliced.
bool is_separable_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfSeparable); }
bool is_octree_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfOctree); }
bool is_adaptive_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfAdaptiveDensity); }
bool is_smoothable_infill_pattern(InfillPattern pattern, int multiline) { return sliced_infill_pattern(pattern).smoothable(multiline); }

} // namespace Slic3r
