#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillGyroid.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *gyroid_options[] = { "gyroid_optimized", nullptr };

// Drawn by FillGyroid (Fill/FillGyroid.cpp); the fallback of TPMS-D and TPMS-FK.
constexpr InfillPatternDescriptor gyroid_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipGyroid, "gyroid", L("Gyroid"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, ipCount, gyroid_options, &make_fill<FillGyroid> },
};

} // namespace

ORCA_INFILL_MODULE(gyroid, gyroid_patterns, std::size(gyroid_patterns));

} // namespace Slic3r
