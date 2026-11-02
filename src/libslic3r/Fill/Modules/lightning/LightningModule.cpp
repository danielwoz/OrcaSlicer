#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillLightning.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *lightning_options[] = { "lightning_overhang_angle", "lightning_prune_angle",
                                              "lightning_straightening_angle", nullptr };

// Drawn by FillLightning::Filler (Fill/FillLightning.cpp) from the generator PrintObject builds. Fill/Lightning/ is
// shared with tree support and stays in the central source list.
constexpr InfillPatternDescriptor lightning_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipLightning, "lightning", L("Lightning"), ipsSparse,
      ipfMultiline | ipfSmoothable,
      4., 0.5f, ipCount, lightning_options, &make_fill<FillLightning::Filler> },
};

} // namespace

ORCA_INFILL_MODULE(lightning, lightning_patterns, std::size(lightning_patterns));

} // namespace Slic3r
