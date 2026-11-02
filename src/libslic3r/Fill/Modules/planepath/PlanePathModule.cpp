#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillPlanePath.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillArchimedeanChords on the plane-path engine (Fill/FillPlanePath.cpp), which also draws the Hilbert
// Curve and Octagram Spiral modules; the fallback of Octagram Spiral.
constexpr InfillPatternDescriptor planepath_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipArchimedeanChords, "archimedeanchords", L("Archimedean Chords"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfFillOrder | ipfCenteredSurface,
      1., 0.5f, ipCount, nullptr, &make_fill<FillArchimedeanChords> },
};

} // namespace

ORCA_INFILL_MODULE(planepath, planepath_patterns, std::size(planepath_patterns));

} // namespace Slic3r
