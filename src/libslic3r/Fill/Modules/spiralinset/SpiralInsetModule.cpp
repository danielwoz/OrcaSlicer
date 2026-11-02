#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillSpiralInset.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillSpiralInset (Fill/FillSpiralInset.cpp).
constexpr InfillPatternDescriptor spiralinset_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipSpiralInset, "spiralinset", L("Spiral Inset"), ipsSurface,
      ipfNoAnchors | ipfFillOrder | ipfArachne,
      1., 0.5f, ipCount, nullptr, &make_fill<FillSpiralInset> },
};

} // namespace

ORCA_INFILL_MODULE(spiralinset, spiralinset_patterns, std::size(spiralinset_patterns));

} // namespace Slic3r
