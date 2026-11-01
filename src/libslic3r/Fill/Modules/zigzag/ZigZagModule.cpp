#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *zigzag_options[] = { "symmetric_infill_y_axis", nullptr };

// Drawn by FillZigZag on the rectilinear engine (Fill/FillRectilinear.cpp); leaving the module out removes the
// pattern, not engine code.
constexpr InfillPatternDescriptor zigzag_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipZigZag, "zigzag", L("Zig Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, ipRectilinear, zigzag_options, &make_fill<FillZigZag> },
};

} // namespace

ORCA_INFILL_MODULE(zigzag, zigzag_patterns, std::size(zigzag_patterns));

} // namespace Slic3r
