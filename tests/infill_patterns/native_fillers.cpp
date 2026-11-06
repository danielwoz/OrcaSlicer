#include "native_fillers.hpp"

#include "libslic3r/Fill/Fill3DHoneycomb.hpp"
#include "libslic3r/Fill/FillAdaptive.hpp"
#include "libslic3r/Fill/FillConcentric.hpp"
#include "libslic3r/Fill/FillConcentricInternal.hpp"
#include "libslic3r/Fill/FillCrossHatch.hpp"
#include "libslic3r/Fill/FillGyroid.hpp"
#include "libslic3r/Fill/FillHoneycomb.hpp"
#include "libslic3r/Fill/FillLightning.hpp"
#include "libslic3r/Fill/FillLine.hpp"
#include "libslic3r/Fill/FillPlanePath.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"
#include "libslic3r/Fill/FillSpiralInset.hpp"
#include "libslic3r/Fill/FillTpmsD.hpp"
#include "libslic3r/Fill/FillTpmsFK.hpp"
#include "libslic3r_infill_patterns.h"

namespace Slic3r::InfillCorpus {

std::unique_ptr<Fill> native_filler(InfillPattern pattern)
{
    switch (pattern) {
    case ipMonotonic: return std::make_unique<FillMonotonic>();
    case ipMonotonicLine: return std::make_unique<FillMonotonicLines>();
    case ipRectilinear: return std::make_unique<FillRectilinear>();
    case ipAlignedRectilinear: return std::make_unique<FillAlignedRectilinear>();
    case ipZigZag: return std::make_unique<FillZigZag>();
    case ipCrossZag: return std::make_unique<FillCrossZag>();
    case ipLockedZag: return std::make_unique<FillLockedZag>();
    // The vtable of these classes is in a file a build may leave out.
#if ORCA_INFILL_LINE
    case ipLine: return std::make_unique<FillLine>();
#endif
    case ipGrid: return std::make_unique<FillGrid>();
    case ipTriangles: return std::make_unique<FillTriangles>();
    case ipStars: return std::make_unique<FillStars>();
    case ipCubic: return std::make_unique<FillCubic>();
    case ipAdaptiveCubic: return std::make_unique<FillAdaptive::Filler>();
    case ipQuarterCubic: return std::make_unique<FillQuarterCubic>();
    case ipSupportCubic: return std::make_unique<FillAdaptive::Filler>();
    case ipLightning: return std::make_unique<FillLightning::Filler>();
    case ipHoneycomb: return std::make_unique<FillHoneycomb>();
    case ip3DHoneycomb: return std::make_unique<Fill3DHoneycomb>();
    case ipLateralHoneycomb: return std::make_unique<FillLateralHoneycomb>();
    case ipLateralLattice: return std::make_unique<FillLateralLattice>();
    case ipCrossHatch: return std::make_unique<FillCrossHatch>();
#if ORCA_INFILL_TPMSD
    case ipTpmsD: return std::make_unique<FillTpmsD>();
#endif
#if ORCA_INFILL_TPMSFK
    case ipTpmsFK: return std::make_unique<FillTpmsFK>();
#endif
    case ipGyroid: return std::make_unique<FillGyroid>();
    case ipConcentric: return std::make_unique<FillConcentric>();
    case ipSpiralInset: return std::make_unique<FillSpiralInset>();
    case ipHilbertCurve: return std::make_unique<FillHilbertCurve>();
    case ipArchimedeanChords: return std::make_unique<FillArchimedeanChords>();
    case ipOctagramSpiral: return std::make_unique<FillOctagramSpiral>();
    case ipSupportBase: return std::make_unique<FillSupportBase>();
    case ipConcentricInternal: return std::make_unique<FillConcentricInternal>();
    default: return nullptr;
    }
}

} // namespace Slic3r::InfillCorpus
