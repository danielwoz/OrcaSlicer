#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../PrintConfig.hpp"

struct orca_infill_builtin_library;

namespace Slic3r {

class Fill;
class InfillPatternLibraryLoader;
struct InfillPatternDescriptor;

// The pattern options a pattern is offered in.
enum InfillPatternSlot : uint8_t {
    ipsSparse  = 1 << 0, // sparse_infill_pattern
    ipsSurface = 1 << 1, // top_surface_pattern, bottom_surface_pattern, internal_solid_infill_pattern
    ipsIroning = 1 << 2, // ironing_pattern, support_ironing_pattern
};

// What code outside a pattern's Fill class needs to know about the pattern.
enum InfillPatternFlag : uint32_t {
    ipfSeparable           = 1u << 0,  // separated_infills can center it on each connected body
    ipfOctree              = 1u << 1,  // laid out by the object's octree per connected body; infill_direction and
                                       // sparse_infill_rotate_template do not apply
    ipfAdaptiveDensity     = 1u << 2,  // graded by adaptive_infill_mode
    ipfSmoothable          = 1u << 3,  // rounds its corners by sparse_infill_smooth_factor
    ipfSmoothableMultiline = 1u << 4,  // rounds its corners only with more than one line per infill wall
    ipfMultiline           = 1u << 5,  // honours fill_multiline
    ipfNoAnchors           = 1u << 6,  // follows the outline: infill_anchor and infill_anchor_max do not apply
    ipfNoAnchorMax         = 1u << 7,  // never connects its lines: infill_anchor_max does not apply
    ipfFillOrder           = 1u << 8,  // honours top_surface_fill_order and bottom_surface_fill_order
    ipfCenteredSurface     = 1u << 9,  // honours center_of_surface_pattern
    ipfLineBasedSolid      = 1u << 10, // narrow internal solid areas are split along its lines
    ipfArachne             = 1u << 11, // generated as variable width extrusions
    ipfNoReverse           = 1u << 12, // its paths keep the direction they were generated in
    ipfRotateTemplateSafe  = 1u << 13, // keeps its strength under sparse_infill_rotate_template
    ipfSmallAreaFlowComp   = 1u << 14, // small_area_infill_flow_compensation applies to its surfaces
    ipfNoSurfaceRetract    = 1u << 15, // no retraction within a top surface or a first layer bottom surface
    ipfCurvedBridgeAnchors = 1u << 16, // bridges over it follow the infill rotation and scan anchors densely
    ipfNoSort              = 1u << 17, // its paths print in the order generated, see Fill::no_sort()
};

using InfillFactory = Fill *(*)(const InfillPatternDescriptor &pattern);

struct InfillPatternDescriptor
{
    InfillPattern      id;
    // Token stored in configurations; nullptr for the patterns only the slicer uses.
    const char        *key;
    // Untranslated label in the pattern lists; nullptr for the patterns only the slicer uses.
    const char        *label;
    // InfillPatternSlot bits.
    uint8_t            slots;
    // InfillPatternFlag bits.
    uint32_t           flags;
    // Largest void between infill lines in squared line distances, see Layer::get_sparse_infill_max_void_area().
    double             void_area_factor;
    // Clearance of combined infill in solid infill line widths, see PrintObject::combine_infill().
    float              combine_clearance_factor;
    // A build may leave the pattern out (CMake option ORCA_INFILL_<NAME>).
    bool               optional;
    // Sliced instead when this build leaves the pattern out. Always built, and offered in every slot of this pattern.
    InfillPattern      fallback;
    // Sparse infill options shown only while this is the sparse infill pattern; nullptr-terminated, may be nullptr.
    const char *const *options;
    // Creates the filler; nullptr when this build leaves the pattern out.
    InfillFactory      factory;
    // Data of the factory; set for the patterns registered at run time.
    const void        *context = nullptr;

    constexpr bool available() const { return factory != nullptr; }
    // Registered at run time by an infill plugin. The descriptor of a value that names no pattern has no factory.
    constexpr bool plugin() const { return int(id) >= int(ipCount) && factory != nullptr; }
    constexpr bool internal() const { return key == nullptr; }
    constexpr bool has(InfillPatternFlag flag) const { return (flags & flag) != 0; }
    constexpr bool smoothable(int multiline) const { return has(ipfSmoothable) || (multiline > 1 && has(ipfSmoothableMultiline)); }
    bool           has_option(const char *opt_key) const;
};

// A pattern library of this build (orca_infill_manifest.h).
struct InfillPatternLibraryRow
{
    InfillPattern pattern;
    // The token of the row, nullptr for an internal row.
    const char   *key;
    // orca_infill_<token>.
    const char   *name;
    // The file name; empty when libslic3r links the entries (ORCA_INFILL_PATTERN_LINKAGE=STATIC).
    const char   *file;

    // The file the loader opens, or the name of the library when there is no file.
    constexpr const char* file_or_name() const { return *file != 0 ? file : name; }
};

// The pattern library that serves a built-in row (Fill/InfillPatternLibraries.hpp).
struct InfillPatternBinding
{
    // The file name of the library, or its name when it is linked into the executable.
    std::string                        file;
    // The library once it passed every check; nullptr while the row is unbound.
    const orca_infill_builtin_library *library { nullptr };
    // Why the row is unbound.
    std::string                        error;
};

class InfillPatternRegistry
{
public:
    // The pattern options, with the slot their values come from.
    static const std::array<std::pair<const char*, InfillPatternSlot>, 6>& options();
    // The built-in patterns indexed by InfillPattern, including those this build leaves out.
    static const std::array<InfillPatternDescriptor, size_t(ipCount)>& builtins();
    // nullptr for a value that names no pattern.
    static const InfillPatternDescriptor* find(InfillPattern id);
    // Looks the token up in the map configurations are parsed with; nullptr for an unknown token.
    static const InfillPatternDescriptor* find(const std::string &key);
    // Token to value map of ConfigOptionEnum<InfillPattern>: every pattern that has a token, built or not, and the
    // runtime patterns.
    static t_config_enum_values keys_map();
    // Fills the value and label lists of a pattern option with the patterns offered in slot: the built-in patterns in
    // InfillPattern order, then the runtime patterns by token.
    static void define_enum_values(ConfigOptionDef &def, InfillPatternSlot slot);
    // Creates the filler slicing uses for id; throws InvalidArgument for a value that names no pattern.
    static Fill* create(InfillPattern id);
    // Whether this build includes every built-in pattern.
    static bool all_available();
    // Replaces the patterns this build leaves out by their fallbacks.
    static void resolve_unavailable(PrintRegionConfig &config);
    // The pattern options of config that name a pattern this build leaves out, with that pattern.
    static std::vector<std::pair<std::string, const InfillPatternDescriptor*>> unavailable_in(const ConfigBase &config);

    // Adds a pattern provided at run time under the next value after the built-in patterns and offers it in the
    // pattern options of its slots. key and label must stay valid until the process exits. Returns nullptr and sets
    // error when the token is malformed or taken, or registration is closed.
    static const InfillPatternDescriptor* register_runtime(InfillPatternDescriptor pattern, std::string &error);
    // Closes registration. Configurations may be read on other threads afterwards.
    static void seal();
    static bool sealed();
    static bool has_runtime_patterns();
    // Sort rank of fill groups: the value itself for a built-in pattern, then the runtime patterns by token,
    // whatever order they were registered in.
    static unsigned order(InfillPattern id);

    // The binding of a row that a pattern library of this build serves; nullptr for every other value. Which rows a
    // library serves is fixed when OrcaSlicer is built, like available().
    static const InfillPatternBinding* binding(InfillPattern id);
    // Whether a library is bound to the row.
    static bool bound(InfillPattern id);
    // Whether a library is bound to every row a pattern library serves.
    static bool all_bound();
    // The rows a pattern library serves that no library is bound to, in InfillPattern order.
    static std::vector<InfillPattern> unbound();
    // The pattern libraries of this build, in InfillPattern order.
    static const std::vector<InfillPatternLibraryRow>& library_rows();
    // Whether two tokens are equal; nullptr, the token of an internal row, equals only itself.
    static constexpr bool same_key(const char *a, const char *b)
    {
        if (a == nullptr || b == nullptr)
            return a == b;
        for (; *a != 0 && *a == *b; ++ a, ++ b) ;
        return *a == *b;
    }

private:
    friend class InfillPatternLibraryLoader;
    // Binds library, loaded from file, to a row a pattern library serves; fails when the row is bound already or
    // registration is closed.
    static bool bind_library(InfillPattern id, const orca_infill_builtin_library &library, const std::string &file);
    // Records why a row a pattern library serves stays unbound, with the file it was looked for in.
    static void set_unbound(InfillPattern id, const std::string &file, const std::string &error);
};

// The pattern a value names. A value that names none gets a descriptor without slots, flags or factory.
// The GUI uses this: it shows what the user selected.
const InfillPatternDescriptor& infill_pattern(InfillPattern pattern);
// The pattern slicing uses for a value: the pattern itself, or its fallback when this build leaves it out.
// Slicing code uses this: it describes what is printed.
const InfillPatternDescriptor& sliced_infill_pattern(InfillPattern pattern);

} // namespace Slic3r
