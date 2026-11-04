#pragma once

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

class AdaptiveInfillField;
namespace FillAdaptive { struct Octree; }
namespace FillLightning { class Generator; }

// Filler calls of every built-in pattern that has a filler: the shapes, surface types, parameters and object states
// that slicing, supports, ironing, the wipe tower and interlocking pass to it. The cases hold plain Slic3r values only,
// so that the same corpus runs against any implementation of a pattern.
namespace InfillCorpus {

// The filler method a case calls.
enum class Call : uint8_t {
    FillSurface, // Fill::fill_surface()
    FillArachne, // Fill::fill_surface_arachne()
    LockedZag,   // FillLockedZag::fill_surface_locked_zag()
    Extrusion,   // Fill::fill_surface_extrusion()
};

// The object state a case fills with. It is built from fixed inputs once per scaling factor.
enum class State : uint8_t {
    None,
    AdaptiveOctree, // FillAdaptive::Octree of a 20 mm cube, as adaptive cubic builds it
    SupportOctree,  // FillAdaptive::Octree of the cube densified below an internal overhang, as support cubic builds it
    Lightning,      // FillLightning::Generator of a sliced 20 mm square slab
    Field,          // AdaptiveInfillField of a sphere joined by a column, in the case's adaptive_infill_mode
};

// The members of Fill a caller sets, in the order Fill declares them.
struct FillMembers
{
    size_t      layer_id        = size_t(-1);
    coordf_t    z               = 0.;
    coordf_t    spacing         = 0.;
    coordf_t    overlap         = 0.;
    float       angle           = FLT_MAX;
    bool        fixed_angle     = false;
    coord_t     link_max_length = 0;
    coord_t     loop_clipping   = 0;
    BoundingBox bounding_box { Point(0, 0), Point(-1, -1) };
    ExPolygons  no_overlap_expolygons;
    bool        dont_alternate_fill_direction = false;
    // Fill::print_config and Fill::print_object_config point at print_config() and object_config(); null otherwise.
    bool        configs = true;
    // Selects the object configuration with calib_flowrate_topinfill_special_order set.
    bool        calib_flowrate_topinfill_special_order = false;
};

struct Case
{
    // Unique and stable: the key of the golden digests.
    std::string     name;
    InfillPattern   pattern { ipRectilinear };
    Call            call { Call::FillSurface };
    // SCALING_FACTOR the case was built under and runs under.
    double          scaling_factor { SCALING_FACTOR_INTERNAL };
    Surface         surface;
    FillParams      params;
    FillMembers     fill;
    State           state { State::None };
    // Passed to Fill::set_lock_region_param() before the call.
    bool            has_lock { false };
    LockRegionParam lock;
};

// Every case, grouped by pattern.
const std::vector<Case>& cases();

// The configurations the cases point Fill::print_config, Fill::print_object_config and FillParams::config at.
const PrintConfig&       print_config();
const PrintObjectConfig& object_config(bool calib_flowrate_topinfill_special_order);
const PrintRegionConfig& region_config();

// The native object states of the cases, built on first use with the slicer's own builders under the current
// SCALING_FACTOR, and kept for the lifetime of the object.
class NativeStates
{
public:
    NativeStates();
    ~NativeStates();

    FillAdaptive::Octree*      octree(State state);
    const AdaptiveInfillField* field(AdaptiveInfillMode mode);
    FillLightning::Generator*  lightning();

    // Points the state members of a native filler at the state of the case.
    void attach(const Case &c, Fill &filler);

private:
    struct Set;
    Set& current();
    std::map<double, std::unique_ptr<Set>> m_sets;
};

// Sets the members of filler from the case, except the object state.
void apply_members(const Case &c, Fill &filler);

// What a filler call returned.
struct Output
{
    enum class Error : uint8_t { None, InfillFailed, BadAlloc, Exception, Unknown };

    Error                                   error { Error::None };
    Polylines                               polylines;
    ThickPolylines                          thick_polylines;
    std::vector<std::pair<Polylines, Flow>> flow_groups;
    ExtrusionEntityCollection               entities;
    // Fill::spacing after the call.
    double                                  spacing { 0. };
};

// Runs the call of the case on a filler whose members and state are already set.
Output run(const Case &c, Fill &filler);

// A digest of every value of the output, with the number of paths and points it holds.
struct Digest
{
    uint64_t hash   { 0 };
    size_t   paths  { 0 };
    size_t   points { 0 };
};
Digest digest(const Case &c, const Output &output);

} // namespace InfillCorpus
} // namespace Slic3r
