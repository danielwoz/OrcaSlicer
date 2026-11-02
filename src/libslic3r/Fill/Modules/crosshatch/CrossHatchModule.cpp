#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillCrossHatch.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillCrossHatch (Fill/FillCrossHatch.cpp); the default sparse infill pattern.
constexpr InfillPatternDescriptor crosshatch_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipCrossHatch, "crosshatch", L("Cross Hatch"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      1., 0.5f, ipCount, nullptr, &make_fill<FillCrossHatch> },
};

} // namespace

ORCA_INFILL_MODULE(crosshatch, crosshatch_patterns, std::size(crosshatch_patterns));

} // namespace Slic3r
