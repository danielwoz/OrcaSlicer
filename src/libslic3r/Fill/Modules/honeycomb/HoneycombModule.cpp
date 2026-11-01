#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillHoneycomb.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillHoneycomb (Fill/FillHoneycomb.cpp); the fallback of Lateral Honeycomb.
constexpr InfillPatternDescriptor honeycomb_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipHoneycomb, "honeycomb", L("Honeycomb"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfSmoothable,
      4., 1.5f, ipCount, nullptr, &make_fill<FillHoneycomb> },
};

} // namespace

ORCA_INFILL_MODULE(honeycomb, honeycomb_patterns, std::size(honeycomb_patterns));

} // namespace Slic3r
