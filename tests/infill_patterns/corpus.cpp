#include "corpus.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <new>
#include <stdexcept>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Fill/AdaptiveInfillField.hpp"
#include "libslic3r/Fill/FillAdaptive.hpp"
#include "libslic3r/Fill/FillLightning.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r::InfillCorpus {
namespace {

// ---- Shapes, in millimetres around the centre of the 20 mm test objects -------------------------------------------

Polygon polygon_mm(const std::vector<Vec2d> &points)
{
    Polygon out;
    for (const Vec2d &p : points)
        out.points.emplace_back(Point::new_scale(p.x(), p.y()));
    return out;
}

Polygon regular_mm(size_t sides, double radius, double rotation, const Vec2d &center)
{
    std::vector<Vec2d> points;
    for (size_t i = 0; i < sides; ++ i) {
        const double a = rotation + 2. * M_PI * double(i) / double(sides);
        points.emplace_back(center + radius * Vec2d(std::cos(a), std::sin(a)));
    }
    return polygon_mm(points);
}

Polygon rectangle_mm(double x0, double y0, double x1, double y1) { return polygon_mm({ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }); }

Polygon hole(Polygon polygon)
{
    polygon.make_clockwise();
    return polygon;
}

ExPolygon square() { return ExPolygon(rectangle_mm(-8., -8., 8., 8.)); }

ExPolygon large() { return ExPolygon(rectangle_mm(-100., -100., 100., 100.)); }

// A rotated heptagon with an off-centre rotated square hole.
ExPolygon heptagon()
{
    ExPolygon out(regular_mm(7, 8.6, 0.3, Vec2d(0., 0.)));
    out.holes.emplace_back(hole(regular_mm(4, 3.1, 0.5, Vec2d(1.3, -0.9))));
    return out;
}

// An 18 mm square with a 4 x 4 grid of square holes.
ExPolygon perforated()
{
    ExPolygon out(rectangle_mm(-9., -9., 9., 9.));
    for (int i = 0; i < 4; ++ i)
        for (int j = 0; j < 4; ++ j) {
            const double x = -6.75 + 4.5 * i, y = -6.75 + 4.5 * j;
            out.holes.emplace_back(hole(rectangle_mm(x - 0.8, y - 0.8, x + 0.8, y + 0.8)));
        }
    return out;
}

// Five beads joined by necks narrower than one line, so that the inset falls apart into islands.
ExPolygon necklace()
{
    constexpr int    beads = 5;
    constexpr double bead = 2.4, pitch = 3.6, x0 = -8.4, half = 1.2, neck = 0.075;
    std::vector<Vec2d> points;
    for (int i = 0; i < beads; ++ i) {
        const double a = x0 + pitch * i, b = a + bead;
        if (i > 0)
            points.emplace_back(a, -neck);
        points.emplace_back(a, -half);
        points.emplace_back(b, -half);
        if (i + 1 < beads)
            points.emplace_back(b, -neck);
    }
    for (int i = beads - 1; i >= 0; -- i) {
        const double a = x0 + pitch * i, b = a + bead;
        if (i + 1 < beads)
            points.emplace_back(b, neck);
        points.emplace_back(b, half);
        points.emplace_back(a, half);
        if (i > 0)
            points.emplace_back(a, neck);
    }
    return ExPolygon(polygon_mm(points));
}

// A rotated strip less than two lines wide.
ExPolygon sliver()
{
    Polygon p = rectangle_mm(-7., -0.35, 7., 0.35);
    p.rotate(0.4);
    p.translate(Point::new_scale(0.5, 0.3));
    return ExPolygon(p);
}

ExPolygon l_shape() { return ExPolygon(polygon_mm({ { -8., -8. }, { 8., -8. }, { 8., -2. }, { -2., -2. }, { -2., 8. }, { -8., 8. } })); }

BoundingBox object_bbox() { return BoundingBox(Point::new_scale(-10., -10.), Point::new_scale(10., 10.)); }

// ---- Rows ---------------------------------------------------------------------------------------------------------

struct Row
{
    InfillPattern id;
    const char   *name;
    // The object state every case of the row fills with.
    State         state      { State::None };
    // make_fills() generates the row with use_arachne.
    bool          arachne    { false };
    // make_fills() clears can_reverse for the row.
    bool          no_reverse { false };
};

constexpr std::array<Row, 31> rows {{
    { ipMonotonic, "monotonic" },
    { ipMonotonicLine, "monotonicline" },
    { ipRectilinear, "rectilinear" },
    { ipAlignedRectilinear, "alignedrectilinear" },
    { ipZigZag, "zigzag" },
    { ipCrossZag, "crosszag" },
    { ipLockedZag, "lockedzag" },
    { ipLine, "line" },
    { ipGrid, "grid", State::None, false, true },
    { ipTriangles, "triangles" },
    { ipStars, "tri-hexagon" },
    { ipCubic, "cubic" },
    { ipAdaptiveCubic, "adaptivecubic", State::AdaptiveOctree },
    { ipQuarterCubic, "quartercubic" },
    { ipSupportCubic, "supportcubic", State::SupportOctree },
    { ipLightning, "lightning", State::Lightning },
    { ipHoneycomb, "honeycomb" },
    { ip3DHoneycomb, "3dhoneycomb" },
    { ipLateralHoneycomb, "lateral-honeycomb" },
    { ipLateralLattice, "lateral-lattice" },
    { ipCrossHatch, "crosshatch" },
    { ipTpmsD, "tpmsd" },
    { ipTpmsFK, "tpmsfk" },
    { ipGyroid, "gyroid" },
    { ipConcentric, "concentric", State::None, true },
    { ipSpiralInset, "spiralinset", State::None, true },
    { ipHilbertCurve, "hilbertcurve" },
    { ipArchimedeanChords, "archimedeanchords" },
    { ipOctagramSpiral, "octagramspiral" },
    { ipSupportBase, "supportbase" },
    { ipConcentricInternal, "concentricinternal", State::None, true },
}};

constexpr bool rows_follow_pattern_values()
{
    for (size_t i = 0; i < rows.size(); ++ i)
        if (size_t(rows[i].id) != i)
            return false;
    return rows.size() == size_t(ipCount);
}
static_assert(rows_follow_pattern_values(), "one row per pattern value, in value order");

const Flow &sparse_flow()
{
    static const Flow flow(0.45f, 0.2f, 0.4f);
    return flow;
}

// ---- Case builders ------------------------------------------------------------------------------------------------

// The sparse infill of layer 5 of a 20 mm object, as make_fills() sets it up.
Case base_case(const Row &row, double scaling_factor)
{
    Case c;
    c.pattern        = row.id;
    c.scaling_factor = scaling_factor;
    c.surface        = Surface(stInternal, heptagon());
    c.state          = row.state;

    FillParams &p = c.params;
    p.density             = 0.2f;
    p.multiline           = 1;
    p.anchor_length       = 1.6f;
    p.anchor_length_max   = 20.f;
    p.resolution          = 0.0125;
    p.dont_adjust         = false;
    p.layer_height        = 0.2;
    p.pattern             = row.id;
    p.flow                = sparse_flow();
    p.extrusion_role      = erInternalInfill;
    p.using_internal_flow = true;
    p.config              = &region_config();
    p.can_reverse         = ! row.no_reverse;

    FillMembers &f = c.fill;
    f.layer_id      = 5;
    f.z             = 1.2;
    f.spacing       = p.flow.spacing();
    f.angle         = float(M_PI / 4.);
    f.bounding_box  = object_bbox();
    f.loop_clipping = scaled<coord_t>(0.06);
    return c;
}

void solid(Case &c, SurfaceType type, ExtrusionRole role)
{
    c.surface.surface_type       = type;
    c.params.density             = 1.f;
    c.params.extrusion_role      = role;
    c.params.using_internal_flow = false;
}

void bridge(Case &c, SurfaceType type, ExtrusionRole role, double angle)
{
    solid(c, type, role);
    c.surface.bridge_angle = angle;
    c.params.dont_adjust   = true;
    c.params.flow          = Flow::bridging_flow(0.4f, 0.4f);
    c.fill.spacing         = c.params.flow.spacing();
}

void at_layer(Case &c, size_t layer_id)
{
    c.fill.layer_id = layer_id;
    c.fill.z        = 0.2 * double(layer_id + 1);
}

class Builder
{
public:
    Builder(std::vector<Case> &out, const Row &row, double scaling_factor) : m_out(out), m_row(row), m_scaling_factor(scaling_factor) {}

    const Row& row() const { return m_row; }

    void add(const char *name, Call call, const std::function<void(Case &)> &setup)
    {
        Case c = base_case(m_row, m_scaling_factor);
        c.name = std::string(m_row.name) + "/" + name;
        c.call = call;
        // make_fills() generates the variable width rows with Arachne.
        c.params.use_arachne = call == Call::FillArachne || (call == Call::Extrusion && m_row.arachne);
        setup(c);
        // make_fills() limits gap fill to the part of the layer's no-overlap area under the surface.
        if (call == Call::Extrusion && c.fill.no_overlap_expolygons.empty() && ! c.surface.expolygon.empty())
            c.fill.no_overlap_expolygons = { c.surface.expolygon };
        m_out.emplace_back(std::move(c));
    }
    void add(const char *name, const std::function<void(Case &)> &setup) { this->add(name, Call::FillSurface, setup); }

private:
    std::vector<Case> &m_out;
    const Row         &m_row;
    double             m_scaling_factor;
};

// Cases every row with a fill_surface() of its own runs.
void add_common(Builder &b)
{
    const State state = b.row().state;
    b.add("base", [](Case &) {});
    b.add("islands", [](Case &c) {
        c.surface.expolygon        = necklace();
        c.params.density           = 0.35f;
        c.params.anchor_length     = 0.05f;
        c.params.anchor_length_max = 0.05f;
        c.fill.overlap             = 0.12;
        at_layer(c, 6);
    });
    b.add("holes", [](Case &c) {
        c.surface.expolygon = perforated();
        at_layer(c, 4);
        c.params.anchor_length     = 0.f;
        c.params.anchor_length_max = 0.f;
        c.params.smooth_factor     = 0.5;
    });
    b.add("sliver", [](Case &c) {
        c.surface.expolygon = sliver();
        c.params.density    = 0.35f;
        at_layer(c, 2);
        c.fill.angle = FLT_MAX;
    });
    b.add("empty", [](Case &c) { c.surface.expolygon = ExPolygon(); });
    b.add("solid", [](Case &c) {
        solid(c, stInternalAfterExternalBridge, erSolidInfill);
        c.surface.expolygon = l_shape();
        at_layer(c, 9);
        c.fill.link_max_length = scaled<coord_t>(3. * c.fill.spacing);
    });
    b.add("top", [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon                = square();
        c.params.dont_adjust               = true;
        c.params.fill_order                = SurfaceFillOrder::Outward;
        c.params.center_of_surface_pattern = CenterOfSurfacePattern::Each_Model;
        at_layer(c, 10);
    });
    b.add("bottom", [](Case &c) {
        solid(c, stBottom, erBottomSurface);
        c.params.density                   = 0.9999f;
        c.params.fill_order                = SurfaceFillOrder::Inward;
        c.params.center_of_surface_pattern = CenterOfSurfacePattern::Each_Assembly;
        c.params.anchor_length             = 1000.f;
        c.params.anchor_length_max         = 1000.f;
        at_layer(c, 0);
    });
    b.add("bridge", [](Case &c) {
        bridge(c, stBottomBridge, erBridgeInfill, 0.3);
        c.surface.expolygon = square();
        at_layer(c, 3);
    });
    b.add("internal-bridge", [](Case &c) {
        bridge(c, stInternalBridge, erInternalBridgeInfill, 1.2);
        c.surface.expolygon = l_shape();
        at_layer(c, 8);
    });
    b.add("multiline2", [](Case &c) {
        c.params.density   = 0.25f;
        c.params.multiline = 2;
    });
    b.add("multiline3", [](Case &c) {
        c.surface.expolygon    = square();
        c.params.density       = 0.4f;
        c.params.multiline     = 3;
        c.params.smooth_factor = 0.5;
        at_layer(c, 6);
    });
    // A lightning generator has trees for its own layers only.
    if (state != State::Lightning)
        b.add("layer-undefined", [](Case &c) {
            c.params.density    = 0.35f;
            c.fill.layer_id     = size_t(-1);
            c.fill.angle        = FLT_MAX;
            c.fill.bounding_box = BoundingBox(Point(0, 0), Point(-1, -1));
        });
    b.add("thick-layers", [](Case &c) {
        c.surface.surface_type     = stInternalVoid;
        c.surface.thickness_layers = 2;
        at_layer(c, 7);
        c.fill.fixed_angle         = true;
        c.fill.overlap             = -0.05;
    });
    b.add("bbox-shifted", [](Case &c) {
        c.fill.bounding_box.translate(Point::new_scale(1.3, -0.7));
        c.fill.dont_alternate_fill_direction = true;
    });
    // The sparse infill polylines bridges over it anchor to: the pattern is left at its default.
    b.add("anchoring", [](Case &c) {
        c.params.pattern = FillParams().pattern;
        at_layer(c, 4);
    });
    if (state == State::None)
        b.add("large", [](Case &c) {
            c.surface.expolygon = large();
            c.params.density    = 0.08f;
            c.fill.bounding_box = BoundingBox(Point::new_scale(-100., -100.), Point::new_scale(100., 100.));
        });
    // make_fills() always passes Locked Zag its lock maps, see add_locked_zag().
    if (b.row().id == ipLockedZag)
        return;
    b.add("extrusion-sparse", Call::Extrusion, [](Case &) {});
    b.add("extrusion-solid", Call::Extrusion, [](Case &c) {
        solid(c, stInternalSolid, erSolidInfill);
        at_layer(c, 9);
        c.fill.link_max_length = scaled<coord_t>(3. * c.fill.spacing);
    });
    b.add("extrusion-top", Call::Extrusion, [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon  = l_shape();
        c.params.fill_order  = SurfaceFillOrder::Inward;
        c.params.can_reverse = false;
        at_layer(c, 10);
    });
    b.add("extrusion-second-bridge", Call::Extrusion, [](Case &c) {
        bridge(c, stSecondInternalBridge, erInternalBridgeInfill, 0.7);
        c.surface.expolygon = perforated();
        at_layer(c, 9);
    });
}

// A line width the adjusted spacing cannot reach, so that Flow::with_spacing() throws after the fill.
void add_bad_flow(Builder &b)
{
    b.add("extrusion-bad-flow", Call::Extrusion, [](Case &c) {
        solid(c, stInternalSolid, erSolidInfill);
        c.params.flow  = Flow(0.3f, 0.25f, 0.4f);
        c.fill.spacing = 0.15;
    });
}

void add_zag(Builder &b)
{
    const bool shifts = b.row().id != ipZigZag;
    if (shifts) {
        // infill_shift_step = 0.4 mm on layers 6 and 7.
        b.add("shift-even", [](Case &c) {
            at_layer(c, 6);
            c.params.horiz_move -= float(scale_(0.4) * 3);
        });
        b.add("shift-odd", [](Case &c) {
            at_layer(c, 7);
            c.params.horiz_move += float(scale_(0.4) * 3);
        });
    }
    b.add("symmetric", [](Case &c) {
        c.params.symmetric_infill_y_axis = true;
        c.params.symmetric_y_axis        = scaled<coord_t>(1.5);
        c.surface.expolygon.symmetric_y(c.params.symmetric_y_axis);
    });
}

// The maps group_fills() builds for a region whose skin and skeleton have their own density and width.
LockRegionParam lock_param(bool two_regions)
{
    LockRegionParam lock;
    const ExPolygons whole { heptagon() };
    if (! two_regions) {
        lock.skin_density_params[0.6f]               = whole;
        lock.skeleton_density_params[0.25f]          = whole;
        lock.skin_flow_params[Flow(0.62f, 0.2f, 0.4f)] = whole;
        lock.skeleton_flow_params[sparse_flow()]     = whole;
        return lock;
    }
    const ExPolygons left { ExPolygon(rectangle_mm(-10., -10., 0.5, 10.)) }, right { ExPolygon(rectangle_mm(-0.5, -10., 10., 10.)) };
    lock.skin_density_params[0.55f]                 = left;
    lock.skin_density_params[0.7f]                  = right;
    lock.skeleton_density_params[0.2f]              = left;
    lock.skeleton_density_params[0.3f]              = right;
    lock.skin_flow_params[Flow(0.55f, 0.2f, 0.4f)]  = left;
    lock.skin_flow_params[Flow(0.65f, 0.2f, 0.4f)]  = right;
    lock.skeleton_flow_params[sparse_flow()]        = left;
    lock.skeleton_flow_params[Flow(0.5f, 0.2f, 0.4f)] = right;
    return lock;
}

void locked(Case &c, bool two_regions)
{
    c.params.locked_zag        = true;
    c.params.infill_lock_depth = float(scale_(1.));
    c.params.skin_infill_depth = float(scale_(2.));
    c.has_lock                 = true;
    c.lock                     = lock_param(two_regions);
}

void add_locked_zag(Builder &b)
{
    b.add("locked", [](Case &c) { locked(c, false); });
    b.add("lock-one", Call::LockedZag, [](Case &c) { locked(c, false); });
    b.add("lock-two", Call::LockedZag, [](Case &c) {
        locked(c, true);
        at_layer(c, 7);
        c.params.horiz_move += float(scale_(0.4) * 3);
    });
    b.add("lock-empty", Call::LockedZag, [](Case &c) {
        locked(c, false);
        c.lock = LockRegionParam();
    });
    b.add("lock-holes", Call::LockedZag, [](Case &c) {
        locked(c, true);
        c.surface.expolygon = perforated();
        c.params.skin_infill_depth = float(scale_(0.6));
    });
    b.add("extrusion-lock", Call::Extrusion, [](Case &c) { locked(c, false); });
    b.add("extrusion-lock-two", Call::Extrusion, [](Case &c) {
        locked(c, true);
        c.params.using_internal_flow = false;
        c.params.can_reverse         = false;
    });
}

void add_scaled(Builder &b)
{
    if (b.row().id == ipConcentricInternal) {
        b.add("scale-1e5", Call::Extrusion, [](Case &c) { solid(c, stInternalSolid, erSolidInfill); });
        return;
    }
    b.add("scale-1e5", [](Case &) {});
    b.add("scale-1e5-solid", [](Case &c) {
        solid(c, stInternalSolid, erSolidInfill);
        c.surface.expolygon = l_shape();
        at_layer(c, 9);
    });
    b.add("scale-1e5-extrusion", Call::Extrusion, [&b](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        at_layer(c, 10);
        if (b.row().id == ipLockedZag)
            locked(c, true);
    });
}

void add_rectilinear(Builder &b)
{
    b.add("monotonic-flag", [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.params.monotonic = true;
    });
    b.add("support", [](Case &c) {
        c.params.density        = 0.3f;
        c.params.dont_adjust    = true;
        c.params.extrusion_role = erSupportMaterial;
        c.fill.layer_id         = size_t(-1);
        c.fill.angle            = 0.f;
        c.fill.link_max_length  = coord_t(scale_(c.fill.spacing * 1.5 / 0.3));
        c.fill.configs          = false;
    });
    b.add("ironing", [](Case &c) {
        solid(c, stTop, erIroning);
        c.params.dont_adjust = true;
        c.fill.spacing       = 0.1;
        c.fill.angle         = float(M_PI / 4.);
        c.fill.fixed_angle   = true;
        at_layer(c, 10);
    });
}

// A caller that sets only the members it needs, as the wipe tower and interlocking do.
void minimal(Case &c, double angle_deg, double spacing)
{
    c.params                = FillParams();
    c.params.density        = 1.f;
    c.fill                  = FillMembers();
    c.fill.angle            = float(angle_deg * M_PI / 180.);
    c.fill.spacing          = spacing;
    c.fill.bounding_box     = get_extents(c.surface.expolygon);
    c.fill.configs          = false;
}

void add_plane_path(Builder &b)
{
    b.add("top-each-surface", [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon = l_shape();
        at_layer(c, 10);
    });
    b.add("top-each-assembly-sparse", [](Case &c) {
        c.params.density                   = 0.45f;
        c.params.extrusion_role            = erTopSolidInfill;
        c.params.center_of_surface_pattern = CenterOfSurfacePattern::Each_Assembly;
        c.surface.surface_type             = stTop;
    });
    if (b.row().id == ipArchimedeanChords) {
        b.add("calib-flow", [](Case &c) {
            solid(c, stTop, erTopSolidInfill);
            c.fill.calib_flowrate_topinfill_special_order = true;
        });
        b.add("extrusion-calib-flow", Call::Extrusion, [](Case &c) {
            solid(c, stTop, erTopSolidInfill);
            c.fill.calib_flowrate_topinfill_special_order = true;
        });
    }
}

void add_tpms(Builder &b, const std::vector<AdaptiveInfillMode> &modes)
{
    static const char *const names[] = { "disabled", "distance-warp", "smooth-blend", "stepped-shells", "lobes", "normal-z", "normal-y", "normal-x" };
    for (AdaptiveInfillMode mode : modes) {
        const std::string name = std::string("adaptive-") + names[size_t(mode)];
        b.add(name.c_str(), [mode](Case &c) {
            c.state                                   = State::Field;
            c.params.adaptive_infill_mode             = mode;
            c.params.adaptive_infill_interior_density = 0.05f;
            c.params.density                          = 0.25f;
            at_layer(c, 29);
        });
    }
    b.add("adaptive-lobes-quadratic", [](Case &c) {
        c.state                                   = State::Field;
        c.params.adaptive_infill_mode             = AdaptiveInfillMode::Lobes;
        c.params.adaptive_infill_interior_density = 0.08f;
        c.params.adaptive_infill_gradient         = AdaptiveInfillGradient::Quadratic;
        c.params.multiline                        = 2;
        c.surface.expolygon                       = square();
        at_layer(c, 59);
    });
    b.add("adaptive-shells-exponential", [](Case &c) {
        c.state                                   = State::Field;
        c.params.adaptive_infill_mode             = AdaptiveInfillMode::SteppedShells;
        c.params.adaptive_infill_interior_density = 0.1f;
        c.params.adaptive_infill_gradient         = AdaptiveInfillGradient::Exponential;
        c.surface.expolygon                       = perforated();
        at_layer(c, 44);
    });
    b.add("adaptive-extrusion", Call::Extrusion, [](Case &c) {
        c.state                                   = State::Field;
        c.params.adaptive_infill_mode             = AdaptiveInfillMode::DistanceWarp;
        c.params.adaptive_infill_interior_density = 0.05f;
        at_layer(c, 49);
    });
}

void add_arachne(Builder &b)
{
    b.add("arachne-sparse", Call::FillArachne, [](Case &) {});
    b.add("arachne-solid", Call::FillArachne, [](Case &c) {
        solid(c, stInternalSolid, erSolidInfill);
        at_layer(c, 9);
    });
    b.add("arachne-top-outward", Call::FillArachne, [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon = perforated();
        c.params.fill_order = SurfaceFillOrder::Outward;
        at_layer(c, 10);
    });
    b.add("arachne-bottom-inward", Call::FillArachne, [](Case &c) {
        solid(c, stBottom, erBottomSurface);
        c.surface.expolygon = l_shape();
        c.params.fill_order = SurfaceFillOrder::Inward;
        at_layer(c, 0);
    });
    b.add("arachne-dont-adjust", Call::FillArachne, [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.params.dont_adjust = true;
        c.surface.expolygon  = necklace();
    });
    b.add("arachne-clipping", Call::FillArachne, [](Case &c) {
        solid(c, stInternalSolid, erSolidInfill);
        c.fill.loop_clipping = scaled<coord_t>(0.4);
        c.params.layer_height = 0.3;
        c.surface.expolygon   = square();
    });
    b.add("arachne-sliver", Call::FillArachne, [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon = sliver();
    });
    b.add("arachne-empty", Call::FillArachne, [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon = ExPolygon();
    });
    b.add("extrusion-arachne-top", Call::Extrusion, [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon = perforated();
        c.params.fill_order = SurfaceFillOrder::Outward;
    });
}

// FillConcentricInternal fills Fill::no_overlap_expolygons from fill_surface_extrusion() only.
void add_concentric_internal(Builder &b)
{
    auto internal = [](Case &c) { solid(c, stInternalSolid, erSolidInfill); };
    b.add("base", Call::Extrusion, internal);
    b.add("islands", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.fill.no_overlap_expolygons = offset_ex(necklace(), -float(scale_(0.1)));
    });
    b.add("holes", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.surface.expolygon = perforated();
    });
    b.add("sliver", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.surface.expolygon = sliver();
    });
    b.add("empty", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.surface.expolygon = ExPolygon();
    });
    b.add("several", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.fill.no_overlap_expolygons = { l_shape(), ExPolygon(rectangle_mm(1., 1., 7., 4.)) };
    });
    b.add("clipping", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.fill.loop_clipping = scaled<coord_t>(0.5);
    });
    b.add("layer-height", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.params.layer_height = 0.32;
        at_layer(c, 3);
    });
    b.add("top", Call::Extrusion, [](Case &c) {
        solid(c, stTop, erTopSolidInfill);
        c.surface.expolygon = l_shape();
    });
    b.add("wide-flow", Call::Extrusion, [&](Case &c) {
        internal(c);
        c.params.flow  = Flow(0.62f, 0.2f, 0.6f);
        c.fill.spacing = c.params.flow.spacing();
    });
    add_bad_flow(b);
    b.add("sparse-role", Call::Extrusion, [](Case &c) {
        c.surface.expolygon = square();
        c.params.using_internal_flow = false;
    });
}

void add_row(std::vector<Case> &out, const Row &row)
{
    Builder b(out, row, SCALING_FACTOR_INTERNAL);
    if (row.id == ipConcentricInternal) {
        add_concentric_internal(b);
        return;
    }
    add_common(b);
    switch (row.id) {
    case ipMonotonicLine:
        b.add("wipe-tower", [](Case &c) {
            c.surface = Surface(stBottom, l_shape());
            minimal(c, 45., 0.42);
        });
        break;
    case ipAlignedRectilinear:
        b.add("interlocking", [](Case &c) {
            minimal(c, 90., 0.8);
            c.params.anchor_length_max = 0.f;
        });
        break;
    case ipRectilinear:
        add_rectilinear(b);
        add_bad_flow(b);
        break;
    case ipHoneycomb: add_bad_flow(b); break;
    case ipZigZag:
    case ipCrossZag: add_zag(b); break;
    case ipLockedZag:
        add_zag(b);
        add_locked_zag(b);
        break;
    case ipLateralHoneycomb:
        b.add("overhang-45", [](Case &c) { c.params.infill_overhang_angle = 45.f; });
        b.add("overhang-70-multiline2", [](Case &c) {
            c.params.infill_overhang_angle = 70.f;
            c.params.multiline             = 2;
            at_layer(c, 23);
        });
        break;
    case ipLateralLattice:
        b.add("angles", [](Case &c) {
            c.params.lateral_lattice_angle_1 = -30.;
            c.params.lateral_lattice_angle_2 = 45.;
            at_layer(c, 16);
        });
        b.add("angles-multiline2", [](Case &c) {
            c.params.lateral_lattice_angle_1 = -45.;
            c.params.lateral_lattice_angle_2 = 10.;
            c.params.multiline               = 2;
            at_layer(c, 31);
        });
        break;
    case ip3DHoneycomb:
        b.add("complete-top", [](Case &c) {
            c.params.infill_complete_top = true;
            at_layer(c, 24);
        });
        b.add("high", [](Case &c) { at_layer(c, 56); });
        break;
    case ipCrossHatch:
        b.add("transition-15", [](Case &c) { at_layer(c, 15); });
        b.add("transition-33", [](Case &c) {
            at_layer(c, 33);
            c.params.density = 0.12f;
        });
        b.add("repeat-dense", [](Case &c) {
            at_layer(c, 21);
            c.params.density = 0.6f;
        });
        break;
    case ipGyroid:
        b.add("optimized", [](Case &c) { c.params.gyroid_optimized = true; });
        b.add("optimized-multiline2", [](Case &c) {
            c.params.gyroid_optimized = true;
            c.params.multiline        = 2;
            at_layer(c, 13);
        });
        add_tpms(b, { AdaptiveInfillMode::DistanceWarp, AdaptiveInfillMode::SmoothBlend, AdaptiveInfillMode::SteppedShells,
                      AdaptiveInfillMode::Lobes, AdaptiveInfillMode::NormalZ, AdaptiveInfillMode::NormalY, AdaptiveInfillMode::NormalX });
        add_bad_flow(b);
        break;
    case ipTpmsD:
    case ipTpmsFK:
        add_tpms(b, { AdaptiveInfillMode::DistanceWarp, AdaptiveInfillMode::SteppedShells, AdaptiveInfillMode::Lobes,
                      AdaptiveInfillMode::NormalY });
        break;
    case ipConcentric:
        add_arachne(b);
        add_bad_flow(b);
        break;
    case ipSpiralInset: add_arachne(b); break;
    case ipArchimedeanChords:
        add_plane_path(b);
        add_bad_flow(b);
        break;
    case ipHilbertCurve:
    case ipOctagramSpiral: add_plane_path(b); break;
    case ipSupportBase:
        b.add("support", [](Case &c) {
            c.params.density        = 0.25f;
            c.params.dont_adjust    = true;
            c.params.extrusion_role = erSupportMaterial;
            c.fill.layer_id         = size_t(-1);
            c.fill.angle            = 0.f;
            c.fill.link_max_length  = coord_t(scale_(c.fill.spacing * 1.5 / 0.25));
            c.fill.configs          = false;
        });
        b.add("support-holes", [](Case &c) {
            c.surface.expolygon     = perforated();
            c.params.density        = 0.4f;
            c.params.dont_adjust    = true;
            c.params.extrusion_role = erSupportMaterial;
            c.fill.angle            = float(M_PI / 2.);
            c.fill.configs          = false;
        });
        break;
    case ipAdaptiveCubic:
    case ipSupportCubic:
        b.add("mid", [](Case &c) { at_layer(c, 49); });
        b.add("top-layers", [](Case &c) {
            at_layer(c, 96);
            c.params.multiline = 2;
        });
        break;
    case ipLightning:
        b.add("layer-3", [](Case &c) { at_layer(c, 3); });
        b.add("layer-15", [](Case &c) { at_layer(c, 15); });
        b.add("layer-12-multiline2", [](Case &c) {
            at_layer(c, 12);
            c.params.multiline     = 2;
            c.params.smooth_factor = 0.5;
        });
        break;
    default: break;
    }
}

std::vector<Case> build_cases()
{
    std::vector<Case> out;
    const double saved = SCALING_FACTOR;
    set_scaling_factor(SCALING_FACTOR_INTERNAL);
    for (const Row &row : rows)
        add_row(out, row);
    // The scaling factor of large beds.
    set_scaling_factor(SCALING_FACTOR_INTERNAL_LARGE_PRINTER);
    for (const Row &row : rows) {
        Builder b(out, row, SCALING_FACTOR_INTERNAL_LARGE_PRINTER);
        add_scaled(b);
    }
    {
        Builder b(out, rows[size_t(ipGyroid)], SCALING_FACTOR_INTERNAL_LARGE_PRINTER);
        b.add("scale-1e5-lobes", [](Case &c) {
            c.state                                   = State::Field;
            c.params.adaptive_infill_mode             = AdaptiveInfillMode::Lobes;
            c.params.adaptive_infill_interior_density = 0.05f;
            at_layer(c, 29);
        });
    }
    set_scaling_factor(saved);
    std::stable_sort(out.begin(), out.end(), [](const Case &a, const Case &b) { return a.pattern < b.pattern; });
    return out;
}

// ---- Digest -------------------------------------------------------------------------------------------------------

// FNV-1a over the bytes of every value.
class Hash
{
public:
    void bytes(const void *data, size_t size)
    {
        for (const unsigned char *p = static_cast<const unsigned char *>(data), *end = p + size; p != end; ++ p) {
            m_hash ^= *p;
            m_hash *= 0x100000001b3ull;
        }
    }
    void u(uint64_t v) { this->bytes(&v, sizeof(v)); }
    void i(int64_t v) { this->bytes(&v, sizeof(v)); }
    void d(double v) { this->bytes(&v, sizeof(v)); }
    void f(float v) { this->bytes(&v, sizeof(v)); }
    uint64_t value() const { return m_hash; }

private:
    uint64_t m_hash { 0xcbf29ce484222325ull };
};

struct Digester
{
    Hash   hash;
    size_t paths  = 0;
    size_t points = 0;

    void polyline(const Polyline &pl)
    {
        ++ paths;
        points += pl.points.size();
        hash.u(pl.points.size());
        for (const Point &p : pl.points) {
            hash.i(p.x());
            hash.i(p.y());
        }
        hash.u(pl.fitting_result.size());
    }
    void polylines(const Polylines &pls)
    {
        hash.u(pls.size());
        for (const Polyline &pl : pls)
            this->polyline(pl);
    }
    void thick_polyline(const ThickPolyline &pl)
    {
        this->polyline(pl);
        hash.u(pl.width.size());
        for (double w : pl.width)
            hash.d(w);
        hash.u(pl.endpoints.first);
        hash.u(pl.endpoints.second);
    }
    void flow(const Flow &flow)
    {
        hash.f(flow.width());
        hash.f(flow.height());
        hash.f(flow.spacing());
        hash.f(flow.nozzle_diameter());
        hash.u(flow.bridge());
    }
    void path(const ExtrusionPath &path)
    {
        ++ paths;
        points += path.polyline.points.size();
        hash.u(uint64_t(path.role()));
        hash.u(path.can_reverse());
        hash.u(path.is_force_no_extrusion());
        hash.d(path.mm3_per_mm);
        hash.f(path.width);
        hash.f(path.height);
        hash.d(path.overhang_degree);
        hash.i(path.curve_degree);
        hash.d(path.smooth_speed);
        hash.u(path.z_contoured);
        hash.u(path.polyline.points.size());
        for (const Point3 &p : path.polyline.points) {
            hash.i(p.x());
            hash.i(p.y());
            hash.i(p.z());
        }
        hash.u(path.polyline.fitting_result.size());
    }
    void entity(const ExtrusionEntity &e)
    {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&e)) {
            hash.u(1);
            hash.u(collection->no_sort);
            hash.u(collection->can_reverse());
            hash.u(collection->entities.size());
            for (const ExtrusionEntity *child : collection->entities)
                this->entity(*child);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&e)) {
            hash.u(2);
            hash.u(uint64_t(loop->loop_role()));
            hash.u(loop->paths.size());
            for (const ExtrusionPath &p : loop->paths)
                this->path(p);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&e)) {
            hash.u(3);
            hash.u(multi->can_reverse());
            hash.u(multi->paths.size());
            for (const ExtrusionPath &p : multi->paths)
                this->path(p);
        } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(&e)) {
            hash.u(4);
            this->path(*p);
        } else
            hash.u(0);
    }
};

} // namespace

const std::vector<Case>& cases()
{
    static const std::vector<Case> all = build_cases();
    return all;
}

const PrintConfig& print_config()
{
    static const PrintConfig config = [] {
        PrintConfig c;
        c.nozzle_diameter.values = { 0.6, 0.4 };
        return c;
    }();
    return config;
}

const PrintObjectConfig& object_config(bool calib_flowrate_topinfill_special_order)
{
    auto make = [](bool calib) {
        PrintObjectConfig c;
        c.gap_fill_target.value                        = gftEverywhere;
        c.calib_flowrate_topinfill_special_order.value = calib;
        return c;
    };
    static const PrintObjectConfig regular = make(false), calib = make(true);
    return calib_flowrate_topinfill_special_order ? calib : regular;
}

const PrintRegionConfig& region_config()
{
    static const PrintRegionConfig config = [] {
        PrintRegionConfig c;
        c.filter_out_gap_fill.value = 0.3;
        return c;
    }();
    return config;
}

// ---- Native states ------------------------------------------------------------------------------------------------

struct NativeStates::Set
{
    FillAdaptive::OctreePtr                                      adaptive_octree, support_octree;
    bool                                                         octrees_built { false };
    std::vector<ExPolygons>                                      field_layers;
    std::vector<AdaptiveInfillField::Slice>                      field_slices;
    std::map<AdaptiveInfillMode, std::unique_ptr<AdaptiveInfillField>> fields;
    std::unique_ptr<Model>                                       model;
    std::unique_ptr<Print>                                       print;
    FillLightning::GeneratorPtr                                  generator;
};

NativeStates::NativeStates()  = default;
NativeStates::~NativeStates() = default;

NativeStates::Set& NativeStates::current()
{
    std::unique_ptr<Set> &set = m_sets[SCALING_FACTOR];
    if (! set)
        set = std::make_unique<Set>();
    return *set;
}

FillAdaptive::Octree* NativeStates::octree(State state)
{
    Set &set = this->current();
    if (! set.octrees_built) {
        // A 20 mm cube standing on the bed with its centre over the origin, in the octree frame.
        indexed_triangle_set mesh = its_make_cube(20., 20., 20.);
        for (stl_vertex &v : mesh.vertices)
            v -= stl_vertex(10.f, 10.f, 0.f);
        const Eigen::Matrix3d to_octree = FillAdaptive::transform_to_octree().toRotationMatrix();
        its_transform(mesh, Transform3d(to_octree), true);
        // An internal bridge 12 mm above the bed.
        std::vector<Vec3d> overhangs = triangulate_expolygon_3d(ExPolygon(rectangle_mm(-4., -5., 6., 3.)), 12.);
        for (Vec3d &p : overhangs)
            p = (to_octree * p).eval();
        set.adaptive_octree = FillAdaptive::build_octree(mesh, {}, 2.2, false);
        set.support_octree  = FillAdaptive::build_octree(mesh, overhangs, 2.2, true);
        set.octrees_built   = true;
    }
    return (state == State::SupportOctree ? set.support_octree : set.adaptive_octree).get();
}

const AdaptiveInfillField* NativeStates::field(AdaptiveInfillMode mode)
{
    if (mode == AdaptiveInfillMode::Disabled)
        return nullptr;
    Set &set = this->current();
    if (set.field_slices.empty()) {
        // A sphere of 9.5 mm radius joined by a column that ends below its equator, in 0.2 mm layers.
        constexpr size_t layers = 100;
        set.field_layers.reserve(layers);
        for (size_t i = 0; i < layers; ++ i) {
            const double z = 0.2 * double(i) + 0.1, r2 = 9.5 * 9.5 - (z - 10.) * (z - 10.);
            Polygons     polygons;
            if (r2 > 0.04)
                polygons.emplace_back(regular_mm(64, std::sqrt(r2), 0., Vec2d(0., 0.)));
            if (z < 8.)
                polygons.emplace_back(rectangle_mm(5., -3., 12., 3.));
            set.field_layers.emplace_back(union_ex(polygons));
        }
        BoundingBox bbox;
        for (size_t i = 0; i < layers; ++ i) {
            set.field_slices.push_back({ 0.2 * double(i), 0.2 * double(i + 1), &set.field_layers[i] });
            bbox.merge(get_extents(set.field_layers[i]));
        }
        for (int m = int(AdaptiveInfillMode::DistanceWarp); m < int(AdaptiveInfillMode::Count); ++ m) {
            auto field = std::make_unique<AdaptiveInfillField>(set.field_slices, bbox, AdaptiveInfillMode(m), [] {});
            if (! field->empty())
                set.fields[AdaptiveInfillMode(m)] = std::move(field);
        }
    }
    auto it = set.fields.find(mode);
    return it == set.fields.end() ? nullptr : it->second.get();
}

FillLightning::Generator* NativeStates::lightning()
{
    Set &set = this->current();
    if (! set.generator) {
        // A 20 mm square slab 4 mm high with 25 % lightning infill in 0.2 mm layers: trees on layers 3 to 15.
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "sparse_infill_pattern", "lightning" }, { "sparse_infill_density", "25%" },
                                        { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 } });
        set.model = std::make_unique<Model>();
        set.print = std::make_unique<Print>();
        ModelObject *object = set.model->add_object();
        object->add_volume(make_cube(20., 20., 4.));
        object->add_instance();
        object->ensure_on_bed();
        set.print->auto_assign_extruders(object);
        set.print->apply(*set.model, config);
        set.print->validate();
        set.print->set_status_silent();
        set.print->process();
        set.generator = FillLightning::build_generator(*set.print->objects().front(), [] {});
    }
    return set.generator.get();
}

void NativeStates::attach(const Case &c, Fill &filler)
{
    switch (c.state) {
    case State::None: break;
    case State::AdaptiveOctree:
    case State::SupportOctree: filler.adapt_fill_octree = this->octree(c.state); break;
    case State::Field: filler.adaptive_infill_field = this->field(c.params.adaptive_infill_mode); break;
    case State::Lightning: dynamic_cast<FillLightning::Filler &>(filler).generator = this->lightning(); break;
    }
}

void apply_members(const Case &c, Fill &filler)
{
    const FillMembers &f = c.fill;
    filler.layer_id                      = f.layer_id;
    filler.z                             = f.z;
    filler.spacing                       = f.spacing;
    filler.overlap                       = f.overlap;
    filler.angle                         = f.angle;
    filler.fixed_angle                   = f.fixed_angle;
    filler.link_max_length               = f.link_max_length;
    filler.loop_clipping                 = f.loop_clipping;
    filler.bounding_box                  = f.bounding_box;
    filler.print_config                  = f.configs ? &print_config() : nullptr;
    filler.print_object_config           = f.configs ? &object_config(f.calib_flowrate_topinfill_special_order) : nullptr;
    filler.no_overlap_expolygons         = f.no_overlap_expolygons;
    filler.dont_alternate_fill_direction = f.dont_alternate_fill_direction;
    if (c.has_lock)
        filler.set_lock_region_param(c.lock);
}

Output run(const Case &c, Fill &filler)
{
    Output out;
    try {
        switch (c.call) {
        case Call::FillSurface: out.polylines = filler.fill_surface(&c.surface, c.params); break;
        case Call::FillArachne: out.thick_polylines = filler.fill_surface_arachne(&c.surface, c.params); break;
        case Call::LockedZag:
            dynamic_cast<FillLockedZag &>(filler).fill_surface_locked_zag(&c.surface, c.params, out.flow_groups);
            break;
        case Call::Extrusion: filler.fill_surface_extrusion(&c.surface, c.params, out.entities.entities); break;
        }
    } catch (const InfillFailedException &) {
        out.error = Output::Error::InfillFailed;
    } catch (const std::bad_alloc &) {
        out.error = Output::Error::BadAlloc;
    } catch (const std::exception &) {
        out.error = Output::Error::Exception;
    } catch (...) {
        out.error = Output::Error::Unknown;
    }
    out.spacing = filler.spacing;
    return out;
}

Digest digest(const Case &c, const Output &output)
{
    Digester d;
    d.hash.u(uint64_t(c.call));
    d.hash.u(uint64_t(output.error));
    d.hash.d(output.spacing);
    d.polylines(output.polylines);
    d.hash.u(output.thick_polylines.size());
    for (const ThickPolyline &pl : output.thick_polylines)
        d.thick_polyline(pl);
    d.hash.u(output.flow_groups.size());
    for (const std::pair<Polylines, Flow> &group : output.flow_groups) {
        d.flow(group.second);
        d.polylines(group.first);
    }
    d.entity(output.entities);
    return { d.hash.value(), d.paths, d.points };
}

} // namespace Slic3r::InfillCorpus
