#include "libslic3r_infill_patterns.h"        // generated: ORCA_INFILL_<NAME> 0/1
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillTpmsFK.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillTpmsFK (Fill/FillTpmsFK.cpp), which is compiled only when the module is built.
constexpr InfillPatternDescriptor tpmsfk_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipTpmsFK, "tpmsfk", L("TPMS-FK"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, ipGyroid, nullptr, optional_fill<ORCA_INFILL_TPMSFK != 0, FillTpmsFK>() },
};

} // namespace

ORCA_INFILL_MODULE(tpmsfk, tpmsfk_patterns, std::size(tpmsfk_patterns));

} // namespace Slic3r
