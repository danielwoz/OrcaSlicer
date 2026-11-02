#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/Fill3DHoneycomb.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *honeycomb_3d_options[] = { "infill_complete_top", nullptr };

// Drawn by Fill3DHoneycomb (Fill/Fill3DHoneycomb.cpp).
constexpr InfillPatternDescriptor honeycomb_3d_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ip3DHoneycomb, "3dhoneycomb", L("3D Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      1., 0.5f, ipCount, honeycomb_3d_options, &make_fill<Fill3DHoneycomb> },
};

} // namespace

ORCA_INFILL_MODULE(3dhoneycomb, honeycomb_3d_patterns, std::size(honeycomb_3d_patterns));

} // namespace Slic3r
