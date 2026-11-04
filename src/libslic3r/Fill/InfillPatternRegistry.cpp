#include "InfillPatternRegistry.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <iterator>
#include <numeric>

#include "libslic3r_infill_patterns.h"      // generated: ORCA_INFILL_<NAME> and ORCA_INFILL_LIBRARY_<ROW> 0/1
#include "orca_infill_manifest.h"           // generated: the pattern libraries of this build
#include "../Exception.hpp"
#include "Fill3DHoneycomb.hpp"
#include "FillAdaptive.hpp"
#include "FillConcentric.hpp"
#include "FillConcentricInternal.hpp"
#include "FillCrossHatch.hpp"
#include "FillGyroid.hpp"
#include "FillHoneycomb.hpp"
#include "FillLibrary.hpp"
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

// The filler of a row: the one of the pattern library that serves it (ORCA_INFILL_LIBRARY_<ROW>), or the class itself.
// A discarded branch is never instantiated, so libslic3r does not create the class of a row a library serves.
template<int Library, class T> constexpr InfillFactory builtin_fill()
{
    if constexpr (Library != 0)
        return &FillLibrary::create;
    else
        return &make_fill<T>;
}

// nullptr when the build leaves the pattern out, so the class only needs to be declared and its implementation file
// may be left out of the build.
template<int Built, int Library, class T> constexpr InfillFactory optional_fill()
{
    if constexpr (Built != 0)
        return builtin_fill<Library, T>();
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
      ipfSeparable | ipfLineBasedSolid | ipfSmallAreaFlowComp | ipfNoSort,
      1., 1.5f, false, ipMonotonic, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_MONOTONIC, FillMonotonic>() },
    { ipMonotonicLine, "monotonicline", L("Monotonic line"), ipsSurface,
      ipfSeparable | ipfLineBasedSolid | ipfSmallAreaFlowComp | ipfNoSort,
      1., 0.5f, false, ipMonotonicLine, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_MONOTONICLINE, FillMonotonicLines>() },
    { ipRectilinear, "rectilinear", L("Rectilinear"), ipsSparse | ipsSurface | ipsIroning,
      ipfSeparable | ipfMultiline | ipfLineBasedSolid | ipfRotateTemplateSafe | ipfSmallAreaFlowComp,
      1., 1.5f, false, ipRectilinear, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_RECTILINEAR, FillRectilinear>() },
    { ipAlignedRectilinear, "alignedrectilinear", L("Aligned Rectilinear"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfLineBasedSolid | ipfSmallAreaFlowComp,
      1., 0.5f, false, ipAlignedRectilinear, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_ALIGNEDRECTILINEAR, FillAlignedRectilinear>() },
    { ipZigZag, "zigzag", L("Zig Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, true, ipRectilinear, zigzag_options, optional_fill<ORCA_INFILL_ZIGZAG, ORCA_INFILL_LIBRARY_ZIGZAG, FillZigZag>() },
    { ipCrossZag, "crosszag", L("Cross Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, true, ipRectilinear, crosszag_options, optional_fill<ORCA_INFILL_CROSSZAG, ORCA_INFILL_LIBRARY_CROSSZAG, FillCrossZag>() },
    { ipLockedZag, "lockedzag", L("Locked Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, true, ipRectilinear, lockedzag_options, optional_fill<ORCA_INFILL_LOCKEDZAG, ORCA_INFILL_LIBRARY_LOCKEDZAG, FillLockedZag>() },
    { ipLine, "line", L("Line"), ipsSparse,
      ipfSeparable | ipfNoAnchorMax | ipfRotateTemplateSafe,
      1., 1.5f, true, ipRectilinear, nullptr, optional_fill<ORCA_INFILL_LINE, ORCA_INFILL_LIBRARY_LINE, FillLine>() },
    { ipGrid, "grid", L("Grid"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline | ipfNoReverse,
      4., 1.5f, false, ipGrid, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_GRID, FillGrid>() },
    { ipTriangles, "triangles", L("Triangles"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, false, ipTriangles, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_TRIANGLES, FillTriangles>() },
    { ipStars, "tri-hexagon", L("Tri-hexagon"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, true, ipTriangles, nullptr, optional_fill<ORCA_INFILL_TRIHEXAGON, ORCA_INFILL_LIBRARY_TRIHEXAGON, FillStars>() },
    { ipCubic, "cubic", L("Cubic"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, false, ipCubic, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_CUBIC, FillCubic>() },
    { ipAdaptiveCubic, "adaptivecubic", L("Adaptive Cubic"), ipsSparse,
      ipfOctree | ipfMultiline,
      4.5, 0.5f, false, ipAdaptiveCubic, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_ADAPTIVECUBIC, FillAdaptive::Filler>() },
    { ipQuarterCubic, "quartercubic", L("Quarter Cubic"), ipsSparse,
      ipfSeparable | ipfMultiline,
      1., 0.5f, true, ipCubic, nullptr, optional_fill<ORCA_INFILL_QUARTERCUBIC, ORCA_INFILL_LIBRARY_QUARTERCUBIC, FillQuarterCubic>() },
    { ipSupportCubic, "supportcubic", L("Support Cubic"), ipsSparse,
      ipfOctree | ipfMultiline,
      4.5, 0.5f, true, ipAdaptiveCubic, nullptr, optional_fill<ORCA_INFILL_SUPPORTCUBIC, ORCA_INFILL_LIBRARY_SUPPORTCUBIC, FillAdaptive::Filler>() },
    { ipLightning, "lightning", L("Lightning"), ipsSparse,
      ipfMultiline | ipfSmoothable,
      4., 0.5f, false, ipLightning, lightning_options, builtin_fill<ORCA_INFILL_LIBRARY_LIGHTNING, FillLightning::Filler>() },
    { ipHoneycomb, "honeycomb", L("Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      4., 1.5f, false, ipHoneycomb, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_HONEYCOMB, FillHoneycomb>() },
    { ip3DHoneycomb, "3dhoneycomb", L("3D Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      1., 0.5f, false, ip3DHoneycomb, honeycomb_3d_options, builtin_fill<ORCA_INFILL_LIBRARY_3DHONEYCOMB, Fill3DHoneycomb>() },
    { ipLateralHoneycomb, "lateral-honeycomb", L("Lateral Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline,
      1., 1.5f, true, ipHoneycomb, lateral_honeycomb_options,
      optional_fill<ORCA_INFILL_LATERALHONEYCOMB, ORCA_INFILL_LIBRARY_LATERALHONEYCOMB, FillLateralHoneycomb>() },
    { ipLateralLattice, "lateral-lattice", L("Lateral Lattice"), ipsSparse,
      ipfSeparable | ipfMultiline,
      4., 1.5f, true, ipGrid, lateral_lattice_options,
      optional_fill<ORCA_INFILL_LATERALLATTICE, ORCA_INFILL_LIBRARY_LATERALLATTICE, FillLateralLattice>() },
    { ipCrossHatch, "crosshatch", L("Cross Hatch"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      1., 0.5f, false, ipCrossHatch, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_CROSSHATCH, FillCrossHatch>() },
    { ipTpmsD, "tpmsd", L("TPMS-D"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, true, ipGyroid, nullptr, optional_fill<ORCA_INFILL_TPMSD, ORCA_INFILL_LIBRARY_TPMSD, FillTpmsD>() },
    { ipTpmsFK, "tpmsfk", L("TPMS-FK"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, true, ipGyroid, nullptr, optional_fill<ORCA_INFILL_TPMSFK, ORCA_INFILL_LIBRARY_TPMSFK, FillTpmsFK>() },
    { ipGyroid, "gyroid", L("Gyroid"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, false, ipGyroid, gyroid_options, builtin_fill<ORCA_INFILL_LIBRARY_GYROID, FillGyroid>() },
    { ipConcentric, "concentric", L("Concentric"), ipsSparse | ipsSurface | ipsIroning,
      ipfMultiline | ipfSmoothable | ipfNoAnchors | ipfFillOrder | ipfArachne | ipfNoSort,
      1., 0.5f, false, ipConcentric, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_CONCENTRIC, FillConcentric>() },
    { ipSpiralInset, "spiralinset", L("Spiral Inset"), ipsSurface,
      ipfNoAnchors | ipfFillOrder | ipfArachne | ipfNoSort,
      1., 0.5f, false, ipSpiralInset, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_SPIRALINSET, FillSpiralInset>() },
    { ipHilbertCurve, "hilbertcurve", L("Hilbert Curve"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfSmoothable | ipfNoSurfaceRetract | ipfCurvedBridgeAnchors,
      1., 0.5f, true, ipRectilinear, nullptr, optional_fill<ORCA_INFILL_HILBERTCURVE, ORCA_INFILL_LIBRARY_HILBERTCURVE, FillHilbertCurve>() },
    { ipArchimedeanChords, "archimedeanchords", L("Archimedean Chords"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfFillOrder | ipfCenteredSurface,
      1., 0.5f, false, ipArchimedeanChords, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_ARCHIMEDEANCHORDS, FillArchimedeanChords>() },
    { ipOctagramSpiral, "octagramspiral", L("Octagram Spiral"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfSmoothable | ipfFillOrder | ipfCenteredSurface | ipfCurvedBridgeAnchors,
      1., 0.5f, true, ipArchimedeanChords, nullptr, optional_fill<ORCA_INFILL_OCTAGRAMSPIRAL, ORCA_INFILL_LIBRARY_OCTAGRAMSPIRAL, FillOctagramSpiral>() },
    // Used by the slicer only: support base, and narrow internal solid areas.
    { ipSupportBase, nullptr, nullptr, 0, 0,
      1., 0.5f, false, ipSupportBase, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_SUPPORTBASE, FillSupportBase>() },
    { ipConcentricInternal, nullptr, nullptr, 0, ipfArachne | ipfNoSort,
      1., 0.5f, false, ipConcentricInternal, nullptr, builtin_fill<ORCA_INFILL_LIBRARY_CONCENTRICINTERNAL, FillConcentricInternal>() },
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

// The pattern libraries of this build (orca_infill_manifest.h), ending in ipCount.
constexpr InfillPatternLibraryRow s_library_rows[] = {
#define ORCA_INFILL_LIBRARY_ROW(row, token, name, file, linked) { row, token, name, file },
    ORCA_INFILL_MANIFEST(ORCA_INFILL_LIBRARY_ROW)
#undef ORCA_INFILL_LIBRARY_ROW
    { ipCount, nullptr, nullptr, nullptr } };

// The libraries of the build serve exactly the rows whose factory is a library's, under the tokens of the rows.
constexpr bool libraries_are_consistent()
{
    size_t served = 0;
    for (const InfillPatternDescriptor &p : s_builtins)
        if (p.factory == &FillLibrary::create)
            ++ served;
    size_t listed = 0;
    for (; s_library_rows[listed].pattern != ipCount; ++ listed) {
        const InfillPatternDescriptor &p = s_builtins[size_t(s_library_rows[listed].pattern)];
        if (p.factory != &FillLibrary::create || ! InfillPatternRegistry::same_key(p.key, s_library_rows[listed].key))
            return false;
    }
    return listed == served;
}
static_assert(libraries_are_consistent(), "ORCA_INFILL_LIBRARY_<ROW> and the pattern libraries of the build disagree");

bool served_by_library(InfillPattern id) { return unsigned(id) < unsigned(ipCount) && s_builtins[size_t(id)].factory == &FillLibrary::create; }

// Written on the main thread before seal(), read concurrently afterwards.
std::array<InfillPatternBinding, size_t(ipCount)>& bindings()
{
    static std::array<InfillPatternBinding, size_t(ipCount)> table = [] {
        std::array<InfillPatternBinding, size_t(ipCount)> out;
        for (const InfillPatternLibraryRow &row : InfillPatternRegistry::library_rows())
            out[size_t(row.pattern)] = { row.file_or_name(), nullptr, "the pattern libraries were not loaded" };
        return out;
    }();
    return table;
}

constexpr std::array<std::pair<const char*, InfillPatternSlot>, 6> s_options {{
    { "sparse_infill_pattern", ipsSparse }, { "top_surface_pattern", ipsSurface }, { "bottom_surface_pattern", ipsSurface },
    { "internal_solid_infill_pattern", ipsSurface }, { "ironing_pattern", ipsIroning }, { "support_ironing_pattern", ipsIroning } }};

// Appended to on the main thread before seal(), read concurrently afterwards; a deque keeps the addresses stable.
std::deque<InfillPatternDescriptor>& runtime_patterns() { static std::deque<InfillPatternDescriptor> patterns; return patterns; }
// Rank by token of each runtime pattern, indexed like runtime_patterns().
std::vector<unsigned>& runtime_ranks() { static std::vector<unsigned> ranks; return ranks; }
bool s_sealed = false;

// Runtime patterns sorted by token.
std::vector<const InfillPatternDescriptor*> runtime_patterns_by_token()
{
    const std::deque<InfillPatternDescriptor> &runtime = runtime_patterns();
    std::vector<const InfillPatternDescriptor*> out(runtime.size());
    for (size_t i = 0; i < runtime.size(); ++ i)
        out[runtime_ranks()[i]] = &runtime[i];
    return out;
}

// ^[a-z0-9][a-z0-9_-]*(\.[a-z0-9][a-z0-9_-]*)+$ and at most 64 characters.
bool valid_runtime_key(const char *key)
{
    if (key == nullptr)
        return false;
    const size_t length = std::strlen(key);
    if (length == 0 || length > 64)
        return false;
    bool dot = false, segment_start = true;
    for (const char *c = key; *c != 0; ++ c) {
        const bool alnum = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9');
        if (*c == '.') {
            if (segment_start)
                return false;
            dot = segment_start = true;
        } else if (alnum || (! segment_start && (*c == '_' || *c == '-')))
            segment_start = false;
        else
            return false;
    }
    return dot && ! segment_start;
}

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
    if (unsigned(id) < unsigned(ipCount))
        return &s_builtins[size_t(id)];
    const std::deque<InfillPatternDescriptor> &runtime = runtime_patterns();
    return int(id) >= int(ipCount) && size_t(int(id) - int(ipCount)) < runtime.size() ? &runtime[size_t(int(id) - int(ipCount))] : nullptr;
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
    for (const InfillPatternDescriptor &pattern : runtime_patterns())
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
    for (const InfillPatternDescriptor *pattern : runtime_patterns_by_token())
        if (pattern->slots & slot) {
            def.enum_values.emplace_back(pattern->key);
            def.enum_labels.emplace_back(pattern->label);
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

const InfillPatternDescriptor* InfillPatternRegistry::register_runtime(InfillPatternDescriptor pattern, std::string &error)
{
    assert(! s_sealed);
    if (s_sealed)
        error = "infill pattern registration is closed";
    else if (! valid_runtime_key(pattern.key))
        error = "the token must be two or more parts matching [a-z0-9][a-z0-9_-]* joined by '.', at most 64 characters";
    else if (find(std::string(pattern.key)) != nullptr)
        error = "the token is already in use";
    else if (pattern.label == nullptr || *pattern.label == 0 || pattern.factory == nullptr ||
             pattern.slots == 0 || (pattern.slots & ~(ipsSparse | ipsSurface)) != 0)
        error = "invalid pattern description";
    if (! error.empty())
        return nullptr;
    std::deque<InfillPatternDescriptor> &runtime = runtime_patterns();
    pattern.id       = InfillPattern(int(ipCount) + int(runtime.size()));
    pattern.fallback = pattern.id;
    pattern.optional = false;
    pattern.options  = nullptr;
    runtime.push_back(pattern);
    std::vector<size_t> by_token(runtime.size());
    std::iota(by_token.begin(), by_token.end(), 0);
    std::sort(by_token.begin(), by_token.end(), [&runtime](size_t a, size_t b) { return std::strcmp(runtime[a].key, runtime[b].key) < 0; });
    std::vector<unsigned> &ranks = runtime_ranks();
    ranks.assign(runtime.size(), 0);
    for (size_t rank = 0; rank < by_token.size(); ++ rank)
        ranks[by_token[rank]] = unsigned(rank);
    publish_infill_pattern_values();
    return &runtime.back();
}

void InfillPatternRegistry::seal() { s_sealed = true; }
bool InfillPatternRegistry::sealed() { return s_sealed; }
bool InfillPatternRegistry::has_runtime_patterns() { return ! runtime_patterns().empty(); }

unsigned InfillPatternRegistry::order(InfillPattern id)
{
    const std::vector<unsigned> &ranks = runtime_ranks();
    if (int(id) >= int(ipCount) && size_t(int(id) - int(ipCount)) < ranks.size())
        return unsigned(ipCount) + ranks[size_t(int(id) - int(ipCount))];
    return unsigned(id);
}

const InfillPatternBinding* InfillPatternRegistry::binding(InfillPattern id) { return served_by_library(id) ? &bindings()[size_t(id)] : nullptr; }

bool InfillPatternRegistry::bound(InfillPattern id)
{
    const InfillPatternBinding *b = binding(id);
    return b != nullptr && b->library != nullptr;
}

bool InfillPatternRegistry::all_bound() { return unbound().empty(); }

std::vector<InfillPattern> InfillPatternRegistry::unbound()
{
    std::vector<InfillPattern> out;
    for (const InfillPatternDescriptor &p : s_builtins)
        if (served_by_library(p.id) && ! bound(p.id))
            out.emplace_back(p.id);
    return out;
}

const std::vector<InfillPatternLibraryRow>& InfillPatternRegistry::library_rows()
{
    static const std::vector<InfillPatternLibraryRow> rows(std::begin(s_library_rows), std::end(s_library_rows) - 1);
    return rows;
}

bool InfillPatternRegistry::bind_library(InfillPattern id, const orca_infill_builtin_library &library, const std::string &file)
{
    assert(! s_sealed);
    if (s_sealed || ! served_by_library(id) || bound(id))
        return false;
    bindings()[size_t(id)] = { file, &library, {} };
    return true;
}

void InfillPatternRegistry::set_unbound(InfillPattern id, const std::string &file, const std::string &error)
{
    assert(! s_sealed);
    if (! s_sealed && served_by_library(id) && ! bound(id))
        bindings()[size_t(id)] = { file, nullptr, error };
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

Fill* Fill::new_from_type(const InfillPattern type) { return InfillPatternRegistry::create(type); }

Fill* Fill::new_from_type(const std::string &type)
{
    const InfillPatternDescriptor *pattern = InfillPatternRegistry::find(type);
    return pattern == nullptr ? nullptr : InfillPatternRegistry::create(pattern->id);
}

// The infill pattern traits declared in PrintConfig.hpp; they describe the pattern that is sliced.
bool is_separable_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfSeparable); }
bool is_octree_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfOctree); }
bool is_adaptive_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfAdaptiveDensity); }
bool is_smoothable_infill_pattern(InfillPattern pattern, int multiline) { return sliced_infill_pattern(pattern).smoothable(multiline); }

} // namespace Slic3r
