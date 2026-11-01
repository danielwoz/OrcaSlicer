#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *lockedzag_options[] = { "infill_shift_step", "symmetric_infill_y_axis", "skeleton_infill_density",
                                              "skin_infill_density", "infill_lock_depth", "skin_infill_depth",
                                              "skin_infill_line_width", "skeleton_infill_line_width", nullptr };

// Drawn by FillLockedZag on the rectilinear engine (Fill/FillRectilinear.cpp); leaving the module out removes the
// pattern, not engine code.
constexpr InfillPatternDescriptor lockedzag_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipLockedZag, "lockedzag", L("Locked Zag"), ipsSparse,
      ipfSeparable | ipfRotateTemplateSafe,
      1., 0.5f, ipRectilinear, lockedzag_options, &make_fill<FillLockedZag> },
};

} // namespace

ORCA_INFILL_MODULE(lockedzag, lockedzag_patterns, std::size(lockedzag_patterns));

} // namespace Slic3r
