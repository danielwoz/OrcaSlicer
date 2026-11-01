#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"

// Marks the labels for translation; they are translated where they are shown.
#define L(s) (s)

namespace Slic3r {
namespace {

constexpr const char *lateral_lattice_options[] = { "lateral_lattice_angle_1", "lateral_lattice_angle_2", nullptr };

// Drawn by FillLateralLattice on the rectilinear engine (Fill/FillRectilinear.cpp); leaving the module out removes
// the pattern, not engine code.
constexpr InfillPatternDescriptor laterallattice_patterns[] = {
    // id, key, label, slots,
    //     flags,
    //     void area factor, combine clearance factor, fallback, options, factory
    { ipLateralLattice, "lateral-lattice", L("Lateral Lattice"), ipsSparse,
      ipfSeparable | ipfMultiline,
      4., 1.5f, ipGrid, lateral_lattice_options, &make_fill<FillLateralLattice> },
};

} // namespace

ORCA_INFILL_MODULE(laterallattice, laterallattice_patterns, std::size(laterallattice_patterns));

} // namespace Slic3r
