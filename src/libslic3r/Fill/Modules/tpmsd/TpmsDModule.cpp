#include "libslic3r_infill_patterns.h"        // generated: ORCA_INFILL_<NAME> 0/1
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillTpmsD.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

// Drawn by FillTpmsD (Fill/FillTpmsD.cpp), which is compiled only when the module is built.
constexpr InfillPatternDescriptor tpmsd_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipTpmsD, "tpmsd", L("TPMS-D"), ipsSparse,
      ipfSeparable | ipfMultiline | ipfAdaptiveDensity,
      1., 0.5f, ipGyroid, nullptr, optional_fill<ORCA_INFILL_TPMSD != 0, FillTpmsD>() },
};

} // namespace

ORCA_INFILL_MODULE(tpmsd, tpmsd_patterns, std::size(tpmsd_patterns));

} // namespace Slic3r
