#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../PrintConfig.hpp"

namespace Slic3r {

class Fill;
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
};

using InfillFactory = Fill *(*)(const InfillPatternDescriptor &pattern);

// One pattern. Each module directory Fill/Modules/<name>/ describes its patterns in a constexpr array of these.
struct InfillPatternDescriptor
{
    // The InfillPattern value. A module writes ipCount for a pattern without an enum value; the registry numbers those
    // patterns from ipCount on, in token order.
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
    // Sliced instead when this build leaves the module out: an enum value of an always built module, offered in every
    // slot of this pattern. An always built module writes ipCount; the registry then sets the pattern itself.
    InfillPattern      fallback;
    // Options shown only while this pattern is selected; nullptr-terminated, may be nullptr.
    const char *const *options;
    // Creates the filler; nullptr when this build leaves the module out.
    InfillFactory      factory;
    // Patterns without an enum value only: listed right after this token in each pattern option offering both;
    // nullptr lists the pattern after all patterns with an enum value.
    const char        *menu_after = nullptr;
    // A build may leave the module out (CORE OFF in its module.cmake); set by the registry.
    bool               optional   = false;

    constexpr bool available() const { return factory != nullptr; }
    constexpr bool internal() const { return key == nullptr; }
    // A pattern without an InfillPattern enum value.
    constexpr bool appended() const { return int(id) >= int(ipCount); }
    constexpr bool has(InfillPatternFlag flag) const { return (flags & flag) != 0; }
    constexpr bool smoothable(int multiline) const { return has(ipfSmoothable) || (multiline > 1 && has(ipfSmoothableMultiline)); }
    bool           has_option(const char *opt_key) const;
};

// The slicing step from which a change of a module option is sliced again.
enum class InfillOptionStep : uint8_t { PrepareInfill, Infill };

// An option a module adds to PrintRegionConfig through REGION_OPTIONS in its module.cmake.
struct InfillRegionOption
{
    const char       *key;
    InfillOptionStep  invalidates;
    // Help page of the settings line, as passed to append_single_option_line(); nullptr for none.
    const char       *wiki;
};

// Creates an option in print_config_def, preset to category Strength and mode Advanced.
using InfillOptionAdder = std::function<ConfigOptionDef*(const char *opt_key, ConfigOptionType type)>;

// The patterns of one module directory, defined in the module's MODULE source with ORCA_INFILL_MODULE().
// Every member is a constant expression, so the object is ready before any dynamic initialiser runs; the registry
// reads it while PrintConfig.cpp initialises its statics.
struct InfillModule
{
    const char                    *name;
    const InfillPatternDescriptor *patterns;
    size_t                         pattern_count;
    const InfillRegionOption      *region_options      = nullptr;
    size_t                         region_option_count = 0;
    // Defines region_options in print_config_def; nullptr when the module adds none.
    void                         (*define_region_options)(const InfillOptionAdder &add) = nullptr;
};

// Defines infill_module_<NAME>, which the generated module list names; the list keeps the module in the link.
// The arguments after NAME are the remaining members of InfillModule, starting with the pattern array and its size.
// Used at namespace scope inside namespace Slic3r, where the generated list declares the object. The static_assert
// checks the module's rows at compile time and requires constant arguments, so the object is constant-initialised.
#define ORCA_INFILL_MODULE(NAME, ...)                                                                                   \
    static_assert(::Slic3r::valid_infill_module(::Slic3r::InfillModule{ #NAME, __VA_ARGS__ }), "infill module " #NAME); \
    extern const ::Slic3r::InfillModule infill_module_##NAME;                                                           \
    const ::Slic3r::InfillModule infill_module_##NAME { #NAME, __VA_ARGS__ }

template<class T> Fill* make_fill(const InfillPatternDescriptor &) { return new T(); }

// nullptr when the build leaves the module out. The discarded branch is never instantiated, so the class only needs
// to be declared, and the module's own sources may be left out of the build. A module that may be left out (CORE OFF)
// and compiles its Fill class from its own SOURCES must create it through optional_fill<ORCA_INFILL_<NAME> != 0, T>():
// &make_fill<T> needs the class's code even in a build that leaves those sources out, and the link fails.
template<bool Built, class T> constexpr InfillFactory optional_fill()
{
    if constexpr (Built)
        return &make_fill<T>;
    else
        return nullptr;
}

namespace detail {
// Tokens are lower case letters, digits and '-'. They never contain '.', which keeps them apart from plugin tokens.
constexpr bool infill_token_ok(const char *s)
{
    if (s == nullptr || *s == 0)
        return false;
    for (; *s != 0; ++ s)
        if (! ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-'))
            return false;
    return true;
}
} // namespace detail

// The checks of one module that need no other module; ORCA_INFILL_MODULE() makes them at compile time.
constexpr bool valid_infill_module(const InfillModule &module)
{
    if (module.patterns == nullptr || module.pattern_count == 0)
        return false;
    for (size_t i = 0; i < module.pattern_count; ++ i) {
        const InfillPatternDescriptor &p = module.patterns[i];
        const bool appended = p.id == ipCount;
        if (int(p.id) < 0 || int(p.id) > int(ipCount))
            return false;
        // Only the slicer's own patterns have no token, label or slot, and they all have enum values.
        if (p.key == nullptr ? (appended || p.label != nullptr || p.slots != 0)
                             : (! detail::infill_token_ok(p.key) || p.label == nullptr || p.slots == 0))
            return false;
        // Appended patterns are offered for sparse infill and surfaces only, and only they choose their place.
        if (appended ? (p.slots & ~(ipsSparse | ipsSurface)) != 0 : p.menu_after != nullptr)
            return false;
        if (! (p.void_area_factor > 0.) || ! (p.combine_clearance_factor > 0.f))
            return false;
    }
    // Region options come with the function that defines them.
    for (size_t i = 0; i < module.region_option_count; ++ i)
        if (module.region_options == nullptr || module.region_options[i].key == nullptr || module.define_region_options == nullptr)
            return false;
    return true;
}

// A run of consecutive patterns.
struct InfillPatternRange
{
    const InfillPatternDescriptor *first;
    size_t                         count;

    const InfillPatternDescriptor* begin() const { return first; }
    const InfillPatternDescriptor* end() const { return first + count; }
    size_t                         size() const { return count; }
    const InfillPatternDescriptor& operator[](size_t idx) const { return first[idx]; }
};

class InfillPatternRegistry
{
public:
    // The pattern options, with the slot their values come from.
    static const std::array<std::pair<const char*, InfillPatternSlot>, 6>& options();
    // Every pattern indexed by InfillPattern: the enum values, then the appended patterns; left-out ones included.
    static const std::vector<InfillPatternDescriptor>& patterns();
    // The patterns with an InfillPattern enum value: the first ipCount entries of patterns().
    static InfillPatternRange builtins();
    // nullptr for a value that names no pattern.
    static const InfillPatternDescriptor* find(InfillPattern id);
    // nullptr for an unknown token. Usable during static initialisation.
    static const InfillPatternDescriptor* find(const std::string &key);
    // Token to value map of ConfigOptionEnum<InfillPattern>: every pattern that has a token, built or not.
    static t_config_enum_values keys_map();
    // Fills the value and label lists of a pattern option with the patterns offered in slot: the enum values in
    // InfillPattern order, each appended pattern after the token it names in menu_after or else at the end.
    static void define_enum_values(ConfigOptionDef &def, InfillPatternSlot slot);
    // Creates the filler slicing uses for id; throws InvalidArgument for a value that names no pattern.
    static Fill* create(InfillPattern id);
    // Whether this build includes every pattern.
    static bool all_available();
    // Replaces the patterns this build leaves out by their fallbacks.
    static void resolve_unavailable(PrintRegionConfig &config);
    // The pattern options of config that name a pattern this build leaves out, with that pattern.
    static std::vector<std::pair<std::string, const InfillPatternDescriptor*>> unavailable_in(const ConfigBase &config);

    // The modules of this build, built or left out.
    static const std::vector<const InfillModule*>& modules();
    // The module describing a pattern; nullptr for a value that names none.
    static const InfillModule* module_of(InfillPattern id);
    // The options modules add, with their module, and one by key (nullptr for any other key).
    static const std::vector<std::pair<const InfillRegionOption*, const InfillModule*>>& region_options();
    static const InfillRegionOption* region_option(const std::string &opt_key);
    // Called once by PrintConfigDef: defines the options modules add.
    static void define_region_options(const InfillOptionAdder &add);
    // The values compared when fill regions are grouped: every option an appended pattern owns, and the module
    // options (region_options()) a pattern with an enum value owns; its other settings have their own
    // SurfaceFillParams fields.
    static void module_params(InfillPattern id, const PrintRegionConfig &config, std::string &out);
};

// The pattern a value names. A value that names none gets a descriptor without slots, flags or factory.
// The GUI uses this: it shows what the user selected.
const InfillPatternDescriptor& infill_pattern(InfillPattern pattern);
// The pattern slicing uses for a value: the pattern itself, or its fallback when this build leaves it out.
// Slicing code uses this: it describes what is printed.
const InfillPatternDescriptor& sliced_infill_pattern(InfillPattern pattern);

namespace detail {
struct InfillModuleEntry
{
    const InfillModule *module;
    // CORE ON in module.cmake.
    bool                core;
    // The module's sources are part of this build.
    bool                built;
};
// Defined by the generated infill_modules.cpp: every module of this build.
extern const InfillModuleEntry infill_module_entries[];
extern const size_t            infill_module_entry_count;

struct InfillPatternTable
{
    std::vector<InfillPatternDescriptor>                                   rows;
    std::vector<const InfillModule*>                                       row_module;
    std::vector<const InfillModule*>                                       modules;
    std::vector<std::pair<const InfillRegionOption*, const InfillModule*>> options;
    std::map<std::string, InfillPattern>                                   by_key;
    bool                                                                   all_available { true };
};
// Builds the table; returns why the modules contradict each other, or an empty string.
std::string build_infill_pattern_table(const InfillModuleEntry *entries, size_t count, InfillPatternTable &table);
// The patterns a pattern option offering slot lists, in list order.
std::vector<const InfillPatternDescriptor*> infill_slot_patterns(const InfillPatternTable &table, InfillPatternSlot slot);
} // namespace detail

} // namespace Slic3r
