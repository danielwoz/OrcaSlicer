#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillConcentric.hpp"
#include "libslic3r/Fill/FillConcentricInternal.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillConcentric (Fill/FillConcentric.cpp) and FillConcentricInternal (Fill/FillConcentricInternal.cpp).
constexpr InfillPatternDescriptor concentric_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipConcentric, "concentric", L("Concentric"), ipsSparse | ipsSurface | ipsIroning,
      ipfMultiline | ipfSmoothable | ipfNoAnchors | ipfFillOrder | ipfArachne,
      1., 0.5f, ipCount, nullptr, &make_fill<FillConcentric> },
    // Narrow internal solid areas; used by the slicer only.
    { ipConcentricInternal, nullptr, nullptr, 0,
      ipfArachne,
      1., 0.5f, ipCount, nullptr, &make_fill<FillConcentricInternal> },
};

} // namespace

ORCA_INFILL_MODULE(concentric, concentric_patterns, std::size(concentric_patterns));

} // namespace Slic3r
