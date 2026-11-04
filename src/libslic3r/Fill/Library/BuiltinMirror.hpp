#pragma once

// The state of one built-in filler call as the C structs of orca_infill_builtin.h, in both directions. The host views
// its Surface, FillParams, Fill members and LockRegionParam without copying a point; a pattern library rebuilds the
// same C++ values from the views.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <admesh/stl.h>

#include "orca_infill_builtin.h"

#include "../AdaptiveInfillField.hpp"
#include "../FillBase.hpp"
#include "../../Point.hpp"
#include "../../Surface.hpp"

namespace Slic3r {
namespace BuiltinMirror {

// ---- Host side ----

// The C request of one filler call. It points into the objects it was made from, which must outlive it unchanged.
class Request
{
public:
    // call is ORCA_INFILL_CALL_*. Fill::no_overlap_expolygons is passed only with ORCA_INFILL_CALL_CONCENTRIC_INTERNAL,
    // the one call that reads it. lock is the LockRegionParam set on the filler, or null; state is its object state.
    Request(int32_t call, const Surface &surface, const FillParams &params, const Fill &fill,
            const LockRegionParam *lock = nullptr, void *state = nullptr);
    Request(const Request &) = delete;
    Request& operator=(const Request &) = delete;

    const orca_infill_builtin_request& get() const { return m_request; }

private:
    orca_infill_region  view(const ExPolygon &expolygon);
    orca_infill_regions view(const ExPolygons &expolygons);

    orca_infill_builtin_request              m_request {};
    orca_infill_surface                      m_surface {};
    orca_infill_fill_params                  m_params {};
    orca_infill_fill_state                   m_fill {};
    orca_infill_lock                         m_lock {};
    // Reserved to their final size before the first view is taken, so that no view moves.
    std::vector<orca_infill_path>            m_holes;
    std::vector<orca_infill_region>          m_regions;
    std::vector<orca_infill_density_regions> m_densities; // skin, then skeleton
    std::vector<orca_infill_flow_regions>    m_flows;     // skeleton, then skin
};

// The C input of the adaptive infill fields over slices (ORCA_INFILL_STATE_ADAPTIVE_FIELD). It points into the slices,
// which must outlive it unchanged.
class FieldInput
{
public:
    FieldInput(const std::vector<AdaptiveInfillField::Slice> &slices, const BoundingBox &bbox);
    FieldInput(const FieldInput &) = delete;
    FieldInput& operator=(const FieldInput &) = delete;

    // The input of the field of mode.
    orca_infill_field_input get(AdaptiveInfillMode mode) const;

private:
    // Reserved to their final size before the first view is taken, so that no view moves.
    std::vector<orca_infill_path>        m_holes;
    std::vector<orca_infill_region>      m_regions;
    std::vector<orca_infill_field_slice> m_slices;
    orca_infill_field_input              m_input {};
};

// The C input of an octree (ORCA_INFILL_STATE_OCTREE) of mesh and overhangs, three vertices per overhang triangle, both
// in the octree frame. It points into them, which must outlive it unchanged.
orca_infill_octree_input octree_input(const indexed_triangle_set &mesh, const std::vector<Vec3d> &overhangs, double line_spacing,
                                      bool support_overhangs_only);

// The points as a C array; nothing is copied.
const orca_infill_point* view(const Points &points);
// The thick polyline as a C path pointing into it.
orca_infill_thick_path   view(const ThickPolyline &polyline);
orca_infill_flow         to_c(const Flow &flow);

// ---- Library side ----
// Each struct that starts with struct_size is read up to its struct_size, and the fields beyond it are zero.

Points          to_points(const orca_infill_point *points, size_t count);
ExPolygon       to_expolygon(const orca_infill_region &region);
ExPolygons      to_expolygons(const orca_infill_regions &regions);
ThickPolyline   to_thick_polyline(const orca_infill_thick_path &path);
Flow            to_flow(const orca_infill_flow &flow);
Surface         to_surface(const orca_infill_surface &surface);
// Every field but config, which stays null.
FillParams      to_fill_params(const orca_infill_fill_params &params);
// Sets every member of fill that orca_infill_fill_state mirrors. print_config, print_object_config and the object state
// members, object_state included, become null; the library attaches the state of state.state to the member its pattern
// reads.
void            apply_fill_state(const orca_infill_fill_state &state, Fill &fill);
// The object state of the call: from prepare_state() of the library, or null.
void*           to_state(const orca_infill_fill_state &state);
LockRegionParam to_lock_region_param(const orca_infill_lock &lock);

// The slices, box and mode of an adaptive infill field. The slices point into expolygons, so it is moved, never copied.
struct FieldSlices
{
    std::vector<ExPolygons>                 expolygons;
    std::vector<AdaptiveInfillField::Slice> slices;
    BoundingBox                             bbox;
    AdaptiveInfillMode                      mode;
};
FieldSlices     to_field_slices(const orca_infill_field_input &input);

// The arguments of FillAdaptive::build_octree().
struct OctreeMesh
{
    indexed_triangle_set mesh;
    std::vector<Vec3d>   overhangs;
    double               line_spacing;
    bool                 support_overhangs_only;
};
OctreeMesh      to_octree_mesh(const orca_infill_octree_input &input);

} // namespace BuiltinMirror
} // namespace Slic3r
