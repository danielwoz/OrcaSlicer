#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillAdaptive.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillAdaptive::Filler (Fill/FillAdaptive.cpp) from the octree PrintObject builds; the fallback of Support Cubic.
constexpr InfillPatternDescriptor adaptivecubic_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipAdaptiveCubic, "adaptivecubic", L("Adaptive Cubic"), ipsSparse,
      ipfOctree | ipfMultiline,
      4.5, 0.5f, ipCount, nullptr, &make_fill<FillAdaptive::Filler> },
};

} // namespace

ORCA_INFILL_MODULE(adaptivecubic, adaptivecubic_patterns, std::size(adaptivecubic_patterns));

} // namespace Slic3r
