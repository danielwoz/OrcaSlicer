#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *crosszag_options[] = { "infill_shift_step", "symmetric_infill_y_axis", nullptr };

// Drawn by FillCrossZag on the rectilinear engine (Fill/FillRectilinear.cpp); leaving the module out removes the
// pattern, not engine code.
constexpr InfillPatternDescriptor crosszag_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipCrossZag, "crosszag", L("Cross Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, ipRectilinear, crosszag_options, &make_fill<FillCrossZag> },
};

} // namespace

ORCA_INFILL_MODULE(crosszag, crosszag_patterns, std::size(crosszag_patterns));

} // namespace Slic3r
