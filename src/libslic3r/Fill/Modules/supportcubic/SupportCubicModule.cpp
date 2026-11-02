#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillAdaptive.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillAdaptive::Filler (Fill/FillAdaptive.cpp) from the support octree PrintObject builds; leaving the
// module out removes the pattern, not engine code.
constexpr InfillPatternDescriptor supportcubic_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipSupportCubic, "supportcubic", L("Support Cubic"), ipsSparse,
      ipfOctree | ipfMultiline,
      4.5, 0.5f, ipAdaptiveCubic, nullptr, &make_fill<FillAdaptive::Filler> },
};

} // namespace

ORCA_INFILL_MODULE(supportcubic, supportcubic_patterns, std::size(supportcubic_patterns));

} // namespace Slic3r
