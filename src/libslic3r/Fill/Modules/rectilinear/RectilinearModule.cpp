#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Patterns the rectilinear engine (Fill/FillRectilinear.cpp) draws that every build needs: surfaces, ironing, support,
// calibration, interlocking and the wipe tower use them.
constexpr InfillPatternDescriptor rectilinear_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipMonotonic, "monotonic", L("Monotonic"), ipsSurface,
      ipfSeparable | ipfLineBasedSolid | ipfSmallAreaFlowComp,
      1., 1.5f, ipCount, nullptr, &make_fill<FillMonotonic> },
    { ipMonotonicLine, "monotonicline", L("Monotonic line"), ipsSurface,
      ipfSeparable | ipfLineBasedSolid | ipfSmallAreaFlowComp,
      1., 0.5f, ipCount, nullptr, &make_fill<FillMonotonicLines> },
    { ipRectilinear, "rectilinear", L("Rectilinear"), ipsSparse | ipsSurface | ipsIroning,
      ipfSeparable | ipfMultiline | ipfLineBasedSolid | ipfRotateTemplateSafe | ipfSmallAreaFlowComp,
      1., 1.5f, ipCount, nullptr, &make_fill<FillRectilinear> },
    { ipAlignedRectilinear, "alignedrectilinear", L("Aligned Rectilinear"), ipsSparse | ipsSurface,
      ipfSeparable | ipfMultiline | ipfLineBasedSolid | ipfSmallAreaFlowComp,
      1., 0.5f, ipCount, nullptr, &make_fill<FillAlignedRectilinear> },
    { ipGrid, "grid", L("Grid"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline | ipfNoReverse,
      4., 1.5f, ipCount, nullptr, &make_fill<FillGrid> },
    { ipTriangles, "triangles", L("Triangles"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, ipCount, nullptr, &make_fill<FillTriangles> },
    { ipCubic, "cubic", L("Cubic"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothableMultiline,
      4.5, 0.5f, ipCount, nullptr, &make_fill<FillCubic> },
    // Support base, and support interfaces below 95 % density; used by the slicer only.
    { ipSupportBase, nullptr, nullptr, 0,
      0,
      1., 0.5f, ipCount, nullptr, &make_fill<FillSupportBase> },
};

} // namespace

ORCA_INFILL_MODULE(rectilinear, rectilinear_patterns, std::size(rectilinear_patterns));

} // namespace Slic3r
