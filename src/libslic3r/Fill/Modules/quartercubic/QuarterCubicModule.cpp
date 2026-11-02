#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillQuarterCubic on the rectilinear engine (Fill/FillRectilinear.cpp); leaving the module out removes the
// pattern, not engine code.
constexpr InfillPatternDescriptor quartercubic_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipQuarterCubic, "quartercubic", L("Quarter Cubic"), ipsSparse,
      ipfSeparable | ipfMultiline,
      1., 0.5f, ipCubic, nullptr, &make_fill<FillQuarterCubic> },
};

} // namespace

ORCA_INFILL_MODULE(quartercubic, quartercubic_patterns, std::size(quartercubic_patterns));

} // namespace Slic3r
