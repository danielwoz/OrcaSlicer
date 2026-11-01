#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillPlanePath.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillHilbertCurve on the plane-path engine (Fill/FillPlanePath.cpp); leaving the module out removes the
// pattern, not engine code.
constexpr InfillPatternDescriptor hilbertcurve_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipHilbertCurve, "hilbertcurve", L("Hilbert Curve"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfSmoothable | ipfNoSurfaceRetract | ipfCurvedBridgeAnchors,
      1., 0.5f, ipRectilinear, nullptr, &make_fill<FillHilbertCurve> },
};

} // namespace

ORCA_INFILL_MODULE(hilbertcurve, hilbertcurve_patterns, std::size(hilbertcurve_patterns));

} // namespace Slic3r
