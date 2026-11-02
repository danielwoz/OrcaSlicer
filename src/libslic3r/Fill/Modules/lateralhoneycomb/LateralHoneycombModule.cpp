#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *lateral_honeycomb_options[] = { "infill_overhang_angle", nullptr };

// Drawn by FillLateralHoneycomb on the rectilinear engine (Fill/FillRectilinear.cpp); leaving the module out removes
// the pattern, not engine code.
constexpr InfillPatternDescriptor lateralhoneycomb_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipLateralHoneycomb, "lateral-honeycomb", L("Lateral Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline,
      1., 1.5f, ipHoneycomb, lateral_honeycomb_options, &make_fill<FillLateralHoneycomb> },
};

} // namespace

ORCA_INFILL_MODULE(lateralhoneycomb, lateralhoneycomb_patterns, std::size(lateralhoneycomb_patterns));

} // namespace Slic3r
