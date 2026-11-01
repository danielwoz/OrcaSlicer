#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillPlanePath.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillOctagramSpiral on the plane-path engine (Fill/FillPlanePath.cpp); leaving the module out removes the
// pattern, not engine code.
constexpr InfillPatternDescriptor octagramspiral_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipOctagramSpiral, "octagramspiral", L("Octagram Spiral"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfSmoothable | ipfFillOrder | ipfCenteredSurface | ipfCurvedBridgeAnchors,
      1., 0.5f, ipArchimedeanChords, nullptr, &make_fill<FillOctagramSpiral> },
};

} // namespace

ORCA_INFILL_MODULE(octagramspiral, octagramspiral_patterns, std::size(octagramspiral_patterns));

} // namespace Slic3r
