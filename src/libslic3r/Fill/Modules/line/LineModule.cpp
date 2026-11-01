#include "libslic3r_infill_patterns.h"        // generated: ORCA_INFILL_<NAME> 0/1
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillLine.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillLine (Fill/FillLine.cpp), which is compiled only when the module is built.
constexpr InfillPatternDescriptor line_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipLine, "line", L("Line"), ipsSparse,
      ipfSeparable | ipfNoAnchorMax | ipfRotateTemplateSafe,
      1., 1.5f, ipRectilinear, nullptr, optional_fill<ORCA_INFILL_LINE != 0, FillLine>() },
};

} // namespace

ORCA_INFILL_MODULE(line, line_patterns, std::size(line_patterns));

} // namespace Slic3r
