#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillStars on the rectilinear engine (Fill/FillRectilinear.cpp); leaving the module out removes the
// pattern, not engine code.
constexpr InfillPatternDescriptor trihexagon_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipStars, "tri-hexagon", L("Tri-hexagon"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, ipTriangles, nullptr, &make_fill<FillStars> },
};

} // namespace

ORCA_INFILL_MODULE(trihexagon, trihexagon_patterns, std::size(trihexagon_patterns));

} // namespace Slic3r
