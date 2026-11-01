#include "libslic3r_infill_patterns.h"        // generated: ORCA_INFILL_<NAME> 0/1
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "FillStaggered.hpp"

// Marks the strings for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *staggered_options[] = { "staggered_spacing_scale", "staggered_cross", nullptr };

// Listed after Line; printed as Rectilinear when this build leaves the module out.
constexpr InfillPatternDescriptor staggered_patterns[] = {
    { ipCount, "staggered", L("Staggered Lines"), ipsSparse, ipfSeparable | ipfMultiline | ipfRotateTemplateSafe,
      1., 0.5f, ipRectilinear, staggered_options, optional_fill<ORCA_INFILL_STAGGERED != 0, FillStaggered>(), "line" },
};

constexpr InfillRegionOption staggered_region_options[] = {
    { "staggered_spacing_scale", InfillOptionStep::PrepareInfill, "strength_settings_patterns#staggered-lines" },
    { "staggered_cross",         InfillOptionStep::PrepareInfill, "strength_settings_patterns#staggered-lines" },
};

void define_staggered_options(const InfillOptionAdder &add)
{
    ConfigOptionDef *def = add("staggered_spacing_scale", coFloat);
    def->label   = L("Line distance scale");
    def->tooltip = L("Distance between the Staggered Lines as a multiple of the distance the sparse infill density gives.");
    def->min     = 0.5;
    def->max     = 4.;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = add("staggered_cross", coBool);
    def->label   = L("Cross lines");
    def->tooltip = L("Also print lines at right angles to the Staggered Lines, at twice their distance.");
    def->set_default_value(new ConfigOptionBool(false));
}

} // namespace

ORCA_INFILL_MODULE(staggered, staggered_patterns, std::size(staggered_patterns),
                   staggered_region_options, std::size(staggered_region_options), &define_staggered_options);

} // namespace Slic3r
