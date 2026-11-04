#include "BuiltinMirror.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <tuple>
#include <type_traits>
#include <utility>

#include <boost/pfr/core.hpp>

namespace Slic3r {
namespace BuiltinMirror {
namespace {

// ---- Drift guards: a failing assertion names the C struct or constants to update with the C++ type ----

// Points and widths are viewed in place.
static_assert(std::is_same_v<coord_t, int64_t> && sizeof(Point) == sizeof(orca_infill_point) && alignof(Point) == alignof(orca_infill_point),
              "a Point is no longer laid out as an orca_infill_point");
static_assert(std::is_same_v<decltype(ThickPolyline::width), std::vector<double>>, "ThickPolyline::width is no longer viewed as doubles");
// The vertices, triangles and overhangs of an octree are viewed in place.
static_assert(std::is_same_v<stl_vertex::Scalar, float> && sizeof(stl_vertex) == 3 * sizeof(float) &&
              std::is_same_v<stl_triangle_vertex_indices::Scalar, int32_t> && sizeof(stl_triangle_vertex_indices) == 3 * sizeof(int32_t) &&
              std::is_same_v<Vec3d::Scalar, double> && sizeof(Vec3d) == 3 * sizeof(double),
              "a mesh or an overhang is no longer laid out as the arrays of orca_infill_octree_input");

// FillParams: every field is carried but config.
static_assert(boost::pfr::tuple_size_v<FillParams> == 35, "FillParams gained or lost a field: update orca_infill_fill_params");
static_assert(std::is_same_v<decltype(boost::pfr::structure_to_tuple(std::declval<const FillParams&>())),
                             std::tuple<float, int, float, float, double, bool, bool, bool, bool, double, bool, AdaptiveInfillMode, float,
                                        AdaptiveInfillGradient, double, double, double, InfillPattern, float, Flow, ExtrusionRole, bool,
                                        float, const PrintRegionConfig*, bool, bool, SurfaceFillOrder, float, bool, bool, int64_t, bool,
                                        float, float, CenterOfSurfacePattern>>,
              "a field of FillParams changed its type: update orca_infill_fill_params");

// FillLightning::Generator::Params: every field is carried.
static_assert(std::is_same_v<decltype(boost::pfr::structure_to_tuple(std::declval<const FillLightning::Generator::Params&>())),
                             std::tuple<float, int64_t, int64_t, int64_t, int64_t>>,
              "FillLightning::Generator::Params changed: update orca_infill_lightning_input");

// Fill and Surface members, mirrored by hand.
static_assert(std::is_same_v<decltype(Fill::layer_id), size_t> && std::is_same_v<decltype(Fill::z), double> &&
              std::is_same_v<decltype(Fill::spacing), double> && std::is_same_v<decltype(Fill::overlap), double> &&
              std::is_same_v<decltype(Fill::angle), float> && std::is_same_v<decltype(Fill::fixed_angle), bool> &&
              std::is_same_v<decltype(Fill::link_max_length), int64_t> && std::is_same_v<decltype(Fill::loop_clipping), int64_t> &&
              std::is_same_v<decltype(Fill::bounding_box), BoundingBox> && std::is_same_v<decltype(Fill::no_overlap_expolygons), ExPolygons> &&
              std::is_same_v<decltype(Fill::dont_alternate_fill_direction), bool> &&
              std::is_same_v<decltype(Fill::min_nozzle_diameter), double> &&
              std::is_same_v<decltype(Fill::calib_flowrate_topinfill_special_order), bool> &&
              std::is_same_v<decltype(Fill::object_state), const InfillObjectState*>,
              "a member of Fill changed its type: update orca_infill_fill_state");
static_assert(std::is_same_v<decltype(Surface::thickness), double> && std::is_same_v<decltype(Surface::bridge_angle), double> &&
              std::is_same_v<decltype(Surface::thickness_layers), uint16_t> && std::is_same_v<decltype(Surface::extra_perimeters), uint16_t>,
              "a member of Surface changed its type: update orca_infill_surface");

// Fill, Surface, LockRegionParam and Flow have the members below and no other: each is the size of a struct that
// declares the same members in the same order. A new member that only fills padding keeps the size and is not seen.
struct FillLayout
{
    virtual ~FillLayout() = default;
    size_t                     layer_id;
    coordf_t                   z;
    coordf_t                   spacing;
    coordf_t                   overlap;
    float                      angle;
    bool                       fixed_angle;
    coord_t                    link_max_length;
    coord_t                    loop_clipping;
    BoundingBox                bounding_box;
    // Not mirrored: the object state, the configurations and the values taken from them come in their own fields.
    FillAdaptive::Octree      *adapt_fill_octree;
    const AdaptiveInfillField *adaptive_infill_field;
    const InfillObjectState   *object_state;
    const PrintConfig         *print_config;
    const PrintObjectConfig   *print_object_config;
    double                     min_nozzle_diameter;
    bool                       calib_flowrate_topinfill_special_order;
    ExPolygons                 no_overlap_expolygons;
    bool                       dont_alternate_fill_direction;
};
static_assert(sizeof(Fill) == sizeof(FillLayout), "Fill gained or lost a member: update orca_infill_fill_state");
struct SurfaceLayout
{
    SurfaceType    surface_type;
    ExPolygon      expolygon;
    double         thickness;
    unsigned short thickness_layers;
    double         bridge_angle;
    unsigned short extra_perimeters;
};
static_assert(sizeof(Surface) == sizeof(SurfaceLayout), "Surface gained or lost a member: update orca_infill_surface");
struct LockRegionParamLayout
{
    std::map<float, ExPolygons> skin_density_params;
    std::map<float, ExPolygons> skeleton_density_params;
    std::map<Flow, ExPolygons>  skin_flow_params;
    std::map<Flow, ExPolygons>  skeleton_flow_params;
};
static_assert(sizeof(LockRegionParam) == sizeof(LockRegionParamLayout), "LockRegionParam gained or lost a member: update orca_infill_lock");
struct FlowLayout
{
    float width;
    float height;
    float spacing;
    float nozzle_diameter;
    bool  bridge;
};
static_assert(sizeof(Flow) == sizeof(FlowLayout), "Flow gained or lost a member: update orca_infill_flow");

// Enumerations travel as their Slic3r values.
static_assert(stTop == ORCA_INFILL_ST_TOP && stBottom == ORCA_INFILL_ST_BOTTOM && stBottomBridge == ORCA_INFILL_ST_BOTTOM_BRIDGE &&
              stInternalAfterExternalBridge == ORCA_INFILL_ST_INTERNAL_AFTER_EXTERNAL_BRIDGE && stInternal == ORCA_INFILL_ST_INTERNAL &&
              stInternalSolid == ORCA_INFILL_ST_INTERNAL_SOLID && stInternalBridge == ORCA_INFILL_ST_INTERNAL_BRIDGE &&
              stSecondInternalBridge == ORCA_INFILL_ST_SECOND_INTERNAL_BRIDGE && stInternalVoid == ORCA_INFILL_ST_INTERNAL_VOID &&
              stPerimeter == ORCA_INFILL_ST_PERIMETER && stCount == ORCA_INFILL_ST_PERIMETER + 1,
              "SurfaceType changed: update ORCA_INFILL_ST_*");
static_assert(erNone == ORCA_INFILL_ER_NONE && erPerimeter == ORCA_INFILL_ER_PERIMETER &&
              erExternalPerimeter == ORCA_INFILL_ER_EXTERNAL_PERIMETER && erOverhangPerimeter == ORCA_INFILL_ER_OVERHANG_PERIMETER &&
              erInternalInfill == ORCA_INFILL_ER_INTERNAL_INFILL && erSolidInfill == ORCA_INFILL_ER_SOLID_INFILL &&
              erTopSolidInfill == ORCA_INFILL_ER_TOP_SOLID_INFILL && erBottomSurface == ORCA_INFILL_ER_BOTTOM_SURFACE &&
              erIroning == ORCA_INFILL_ER_IRONING && erBridgeInfill == ORCA_INFILL_ER_BRIDGE_INFILL &&
              erInternalBridgeInfill == ORCA_INFILL_ER_INTERNAL_BRIDGE_INFILL && erGapFill == ORCA_INFILL_ER_GAP_FILL &&
              erSkirt == ORCA_INFILL_ER_SKIRT && erBrim == ORCA_INFILL_ER_BRIM && erSupportMaterial == ORCA_INFILL_ER_SUPPORT_MATERIAL &&
              erSupportMaterialInterface == ORCA_INFILL_ER_SUPPORT_MATERIAL_INTERFACE &&
              erSupportTransition == ORCA_INFILL_ER_SUPPORT_TRANSITION && erWipeTower == ORCA_INFILL_ER_WIPE_TOWER &&
              erCustom == ORCA_INFILL_ER_CUSTOM && erMixed == ORCA_INFILL_ER_MIXED && erCount == ORCA_INFILL_ER_MIXED + 1,
              "ExtrusionRole changed: update ORCA_INFILL_ER_*");
static_assert(int(SurfaceFillOrder::Default) == ORCA_INFILL_ORDER_DEFAULT && int(SurfaceFillOrder::Outward) == ORCA_INFILL_ORDER_OUTWARD &&
              int(SurfaceFillOrder::Inward) == ORCA_INFILL_ORDER_INWARD && int(SurfaceFillOrder::Count) == ORCA_INFILL_ORDER_INWARD + 1,
              "SurfaceFillOrder changed: update ORCA_INFILL_ORDER_*");
static_assert(int(CenterOfSurfacePattern::Each_Surface) == ORCA_INFILL_CENTER_EACH_SURFACE &&
              int(CenterOfSurfacePattern::Each_Model) == ORCA_INFILL_CENTER_EACH_MODEL &&
              int(CenterOfSurfacePattern::Each_Assembly) == ORCA_INFILL_CENTER_EACH_ASSEMBLY,
              "CenterOfSurfacePattern changed: update ORCA_INFILL_CENTER_*");
static_assert(int(AdaptiveInfillMode::Disabled) == ORCA_INFILL_ADAPTIVE_DISABLED &&
              int(AdaptiveInfillMode::DistanceWarp) == ORCA_INFILL_ADAPTIVE_DISTANCE_WARP &&
              int(AdaptiveInfillMode::SmoothBlend) == ORCA_INFILL_ADAPTIVE_SMOOTH_BLEND &&
              int(AdaptiveInfillMode::SteppedShells) == ORCA_INFILL_ADAPTIVE_STEPPED_SHELLS &&
              int(AdaptiveInfillMode::Lobes) == ORCA_INFILL_ADAPTIVE_LOBES && int(AdaptiveInfillMode::NormalZ) == ORCA_INFILL_ADAPTIVE_NORMAL_Z &&
              int(AdaptiveInfillMode::NormalY) == ORCA_INFILL_ADAPTIVE_NORMAL_Y && int(AdaptiveInfillMode::NormalX) == ORCA_INFILL_ADAPTIVE_NORMAL_X &&
              int(AdaptiveInfillMode::Count) == ORCA_INFILL_ADAPTIVE_NORMAL_X + 1,
              "AdaptiveInfillMode changed: update ORCA_INFILL_ADAPTIVE_*");
static_assert(int(AdaptiveInfillGradient::Linear) == ORCA_INFILL_GRADIENT_LINEAR &&
              int(AdaptiveInfillGradient::Quadratic) == ORCA_INFILL_GRADIENT_QUADRATIC &&
              int(AdaptiveInfillGradient::Exponential) == ORCA_INFILL_GRADIENT_EXPONENTIAL,
              "AdaptiveInfillGradient changed: update ORCA_INFILL_GRADIENT_*");

// A struct that starts with struct_size, up to its struct_size; the fields beyond it are zero.
template<class T> T read(const T &source)
{
    static_assert(std::is_trivially_copyable_v<T>);
    T out;
    std::memset(&out, 0, sizeof(T));
    std::memcpy(&out, &source, std::min<size_t>(source.struct_size, sizeof(T)));
    return out;
}

orca_infill_path path_view(const Points &points) { return { view(points), points.size() }; }

// Views of expolygons whose holes are appended to holes, and the expolygons to regions; both have the capacity.
orca_infill_region region_view(const ExPolygon &expolygon, std::vector<orca_infill_path> &holes)
{
    assert(holes.size() + expolygon.holes.size() <= holes.capacity());
    const size_t first = holes.size();
    for (const Polygon &hole : expolygon.holes)
        holes.push_back(path_view(hole.points));
    return { path_view(expolygon.contour.points), expolygon.holes.empty() ? nullptr : holes.data() + first, expolygon.holes.size() };
}

orca_infill_regions regions_view(const ExPolygons &expolygons, std::vector<orca_infill_path> &holes, std::vector<orca_infill_region> &regions)
{
    assert(regions.size() + expolygons.size() <= regions.capacity());
    const size_t first = regions.size();
    for (const ExPolygon &expolygon : expolygons)
        regions.push_back(region_view(expolygon, holes));
    return { expolygons.empty() ? nullptr : regions.data() + first, expolygons.size() };
}

// Views of polygons appended to paths, which has the capacity.
orca_infill_polygons polygons_view(const Polygons &polygons, std::vector<orca_infill_path> &paths)
{
    assert(paths.size() + polygons.size() <= paths.capacity());
    const size_t first = paths.size();
    for (const Polygon &polygon : polygons)
        paths.push_back(path_view(polygon.points));
    return { polygons.empty() ? nullptr : paths.data() + first, polygons.size() };
}

// The polygons of each of count layers, none when layers is null.
std::vector<Polygons> to_layers(const orca_infill_polygons *layers, size_t count)
{
    std::vector<Polygons> out;
    if (layers == nullptr)
        return out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++ i) {
        Polygons &polygons = out.emplace_back();
        polygons.reserve(layers[i].count);
        for (size_t j = 0; j < layers[i].count; ++ j)
            polygons.emplace_back(to_points(layers[i].items[j].points, layers[i].items[j].count));
    }
    return out;
}

orca_infill_bbox to_c(const BoundingBox &bbox) { return { { bbox.min.x(), bbox.min.y() }, { bbox.max.x(), bbox.max.y() } }; }

float key(const orca_infill_density_regions &entry) { return entry.density; }
Flow  key(const orca_infill_flow_regions &entry) { return to_flow(entry.flow); }

// Rebuilds a map of LockRegionParam from its entries in iteration order.
template<class Map, class Entry> void rebuild(Map &map, const Entry *entries, size_t count)
{
    for (size_t i = 0; i < count; ++ i)
        map.emplace_hint(map.end(), key(entries[i]), to_expolygons(entries[i].regions));
}

} // namespace

// ---- Host side ----

const orca_infill_point* view(const Points &points) { return reinterpret_cast<const orca_infill_point*>(points.data()); }

orca_infill_thick_path view(const ThickPolyline &polyline)
{
    return { view(polyline.points), polyline.points.size(), polyline.width.data(), polyline.width.size(),
             uint8_t(polyline.endpoints.first), uint8_t(polyline.endpoints.second) };
}

orca_infill_flow to_c(const Flow &flow)
{
    return { flow.width(), flow.height(), flow.spacing(), flow.nozzle_diameter(), uint8_t(flow.bridge()) };
}

Request::Request(int32_t call, const Surface &surface, const FillParams &params, const Fill &fill, const LockRegionParam *lock, void *state)
{
    const ExPolygons *no_overlap = call == ORCA_INFILL_CALL_CONCENTRIC_INTERNAL ? &fill.no_overlap_expolygons : nullptr;
    size_t holes = surface.expolygon.holes.size(), regions = 0;
    auto count = [&holes, &regions](const ExPolygons &expolygons) {
        regions += expolygons.size();
        for (const ExPolygon &expolygon : expolygons)
            holes += expolygon.holes.size();
    };
    auto count_map = [&count](const auto &map) {
        for (const auto &entry : map)
            count(entry.second);
    };
    if (no_overlap != nullptr)
        count(*no_overlap);
    if (lock != nullptr) {
        count_map(lock->skin_density_params);
        count_map(lock->skeleton_density_params);
        count_map(lock->skeleton_flow_params);
        count_map(lock->skin_flow_params);
    }
    m_holes.reserve(holes);
    m_regions.reserve(regions);

    m_surface.struct_size      = sizeof(m_surface);
    m_surface.surface_type     = int32_t(surface.surface_type);
    m_surface.expolygon        = this->view(surface.expolygon);
    m_surface.thickness        = surface.thickness;
    m_surface.bridge_angle     = surface.bridge_angle;
    m_surface.thickness_layers = surface.thickness_layers;
    m_surface.extra_perimeters = surface.extra_perimeters;

    orca_infill_fill_params &p = m_params;
    p.struct_size                      = sizeof(p);
    p.density                          = params.density;
    p.multiline                        = params.multiline;
    p.anchor_length                    = params.anchor_length;
    p.anchor_length_max                = params.anchor_length_max;
    p.resolution                       = params.resolution;
    p.dont_adjust                      = params.dont_adjust;
    p.monotonic                        = params.monotonic;
    p.complete                         = params.complete;
    p.use_arachne                      = params.use_arachne;
    p.layer_height                     = params.layer_height;
    p.gyroid_optimized                 = params.gyroid_optimized;
    p.adaptive_infill_mode             = int32_t(params.adaptive_infill_mode);
    p.adaptive_infill_interior_density = params.adaptive_infill_interior_density;
    p.adaptive_infill_gradient         = int32_t(params.adaptive_infill_gradient);
    p.smooth_factor                    = params.smooth_factor;
    p.lateral_lattice_angle_1          = params.lateral_lattice_angle_1;
    p.lateral_lattice_angle_2          = params.lateral_lattice_angle_2;
    p.pattern                          = int32_t(params.pattern);
    p.infill_overhang_angle            = params.infill_overhang_angle;
    p.flow                             = to_c(params.flow);
    p.extrusion_role                   = int32_t(params.extrusion_role);
    p.using_internal_flow              = params.using_internal_flow;
    p.no_extrusion_overlap             = params.no_extrusion_overlap;
    p.dont_sort                        = params.dont_sort;
    p.can_reverse                      = params.can_reverse;
    p.fill_order                       = int32_t(params.fill_order);
    p.horiz_move                       = params.horiz_move;
    p.symmetric_infill_y_axis          = params.symmetric_infill_y_axis;
    p.infill_complete_top              = params.infill_complete_top;
    p.symmetric_y_axis                 = params.symmetric_y_axis;
    p.locked_zag                       = params.locked_zag;
    p.infill_lock_depth                = params.infill_lock_depth;
    p.skin_infill_depth                = params.skin_infill_depth;
    p.center_of_surface_pattern        = int32_t(params.center_of_surface_pattern);

    orca_infill_fill_state &f = m_fill;
    f.struct_size                            = sizeof(f);
    f.layer_id                               = fill.layer_id == size_t(-1) ? UINT64_MAX : uint64_t(fill.layer_id);
    f.z                                      = fill.z;
    f.spacing                                = fill.spacing;
    f.overlap                                = fill.overlap;
    f.angle                                  = fill.angle;
    f.fixed_angle                            = fill.fixed_angle;
    f.dont_alternate_fill_direction          = fill.dont_alternate_fill_direction;
    f.bounding_box_defined                   = fill.bounding_box.defined;
    f.calib_flowrate_topinfill_special_order = fill.calib_flowrate_topinfill_special_order;
    f.link_max_length                        = fill.link_max_length;
    f.loop_clipping                          = fill.loop_clipping;
    f.bounding_box                           = to_c(fill.bounding_box);
    f.min_nozzle_diameter                    = fill.min_nozzle_diameter;
    f.state                                  = state;
    if (no_overlap != nullptr)
        f.no_overlap = this->view(*no_overlap);

    if (lock != nullptr) {
        m_densities.reserve(lock->skin_density_params.size() + lock->skeleton_density_params.size());
        m_flows.reserve(lock->skeleton_flow_params.size() + lock->skin_flow_params.size());
        for (const auto *map : { &lock->skin_density_params, &lock->skeleton_density_params })
            for (const auto &[density, expolygons] : *map)
                m_densities.push_back({ density, this->view(expolygons) });
        for (const auto *map : { &lock->skeleton_flow_params, &lock->skin_flow_params })
            for (const auto &[flow, expolygons] : *map)
                m_flows.push_back({ to_c(flow), this->view(expolygons) });
        const size_t skin_density = lock->skin_density_params.size(), skeleton_flow = lock->skeleton_flow_params.size();
        m_lock = { sizeof(m_lock), m_densities.data(), skin_density, m_densities.data() + skin_density,
                   lock->skeleton_density_params.size(), m_flows.data(), skeleton_flow, m_flows.data() + skeleton_flow,
                   lock->skin_flow_params.size() };
    }

    m_request = { sizeof(m_request), call, SCALING_FACTOR, &m_surface, &m_params, &m_fill, lock != nullptr ? &m_lock : nullptr };
}

orca_infill_region Request::view(const ExPolygon &expolygon) { return region_view(expolygon, m_holes); }

orca_infill_regions Request::view(const ExPolygons &expolygons) { return regions_view(expolygons, m_holes, m_regions); }

FieldInput::FieldInput(const std::vector<AdaptiveInfillField::Slice> &slices, const BoundingBox &bbox)
{
    size_t holes = 0, regions = 0;
    for (const AdaptiveInfillField::Slice &slice : slices) {
        regions += slice.expolygons->size();
        for (const ExPolygon &expolygon : *slice.expolygons)
            holes += expolygon.holes.size();
    }
    m_holes.reserve(holes);
    m_regions.reserve(regions);
    m_slices.reserve(slices.size());
    for (const AdaptiveInfillField::Slice &slice : slices)
        m_slices.push_back({ slice.bottom_z, slice.top_z, regions_view(*slice.expolygons, m_holes, m_regions) });
    m_input = { sizeof(m_input), m_slices.empty() ? nullptr : m_slices.data(), m_slices.size(), to_c(bbox), uint8_t(bbox.defined),
                ORCA_INFILL_ADAPTIVE_DISABLED };
}

orca_infill_field_input FieldInput::get(AdaptiveInfillMode mode) const
{
    orca_infill_field_input out = m_input;
    out.mode                    = int32_t(mode);
    return out;
}

orca_infill_octree_input octree_input(const indexed_triangle_set &mesh, const std::vector<Vec3d> &overhangs, double line_spacing,
                                      bool support_overhangs_only)
{
    return { sizeof(orca_infill_octree_input),
             mesh.vertices.empty() ? nullptr : reinterpret_cast<const float*>(mesh.vertices.data()), mesh.vertices.size(),
             mesh.indices.empty() ? nullptr : reinterpret_cast<const int32_t*>(mesh.indices.data()), mesh.indices.size(),
             overhangs.empty() ? nullptr : reinterpret_cast<const double*>(overhangs.data()), overhangs.size(),
             line_spacing, uint8_t(support_overhangs_only) };
}

LightningInput::LightningInput(const FillLightning::Generator::Params &params, const std::vector<Polygons> &outlines,
                               const std::vector<Polygons> *overhangs)
{
    assert(overhangs == nullptr || overhangs->size() == outlines.size());
    size_t paths = 0;
    for (const std::vector<Polygons> *layers : { &outlines, overhangs })
        if (layers != nullptr)
            for (const Polygons &polygons : *layers)
                paths += polygons.size();
    m_paths.reserve(paths);
    m_layers.reserve(outlines.size() * (overhangs != nullptr ? 2 : 1));
    for (const Polygons &polygons : outlines)
        m_layers.push_back(polygons_view(polygons, m_paths));
    if (overhangs != nullptr)
        for (const Polygons &polygons : *overhangs)
            m_layers.push_back(polygons_view(polygons, m_paths));
    const bool layers = ! outlines.empty();
    m_input = { sizeof(m_input), outlines.size(), layers ? m_layers.data() : nullptr,
                layers && overhangs != nullptr ? m_layers.data() + outlines.size() : nullptr, params.infill_extrusion_width,
                params.supporting_radius, params.wall_supporting_radius, params.prune_length, params.straightening_max_distance };
}

// ---- Library side ----

Points to_points(const orca_infill_point *points, size_t count)
{
    Points out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++ i)
        out.emplace_back(points[i].x, points[i].y);
    return out;
}

ExPolygon to_expolygon(const orca_infill_region &region)
{
    ExPolygon out;
    out.contour.points = to_points(region.contour.points, region.contour.count);
    out.holes.resize(region.hole_count);
    for (size_t i = 0; i < region.hole_count; ++ i)
        out.holes[i].points = to_points(region.holes[i].points, region.holes[i].count);
    return out;
}

ExPolygons to_expolygons(const orca_infill_regions &regions)
{
    ExPolygons out;
    out.reserve(regions.count);
    for (size_t i = 0; i < regions.count; ++ i)
        out.emplace_back(to_expolygon(regions.items[i]));
    return out;
}

ThickPolyline to_thick_polyline(const orca_infill_thick_path &path)
{
    ThickPolyline out;
    out.points = to_points(path.points, path.count);
    out.width.assign(path.widths, path.widths + path.width_count);
    out.endpoints = { path.first_endpoint != 0, path.last_endpoint != 0 };
    return out;
}

Flow to_flow(const orca_infill_flow &flow) { return Flow::raw(flow.width, flow.height, flow.spacing, flow.nozzle_diameter, flow.bridge != 0); }

Surface to_surface(const orca_infill_surface &source)
{
    const orca_infill_surface s = read(source);
    Surface out(SurfaceType(s.surface_type));
    out.expolygon        = to_expolygon(s.expolygon);
    out.thickness        = s.thickness;
    out.thickness_layers = s.thickness_layers;
    out.bridge_angle     = s.bridge_angle;
    out.extra_perimeters = s.extra_perimeters;
    return out;
}

FillParams to_fill_params(const orca_infill_fill_params &source)
{
    const orca_infill_fill_params p = read(source);
    FillParams out;
    out.density                          = p.density;
    out.multiline                        = p.multiline;
    out.anchor_length                    = p.anchor_length;
    out.anchor_length_max                = p.anchor_length_max;
    out.resolution                       = p.resolution;
    out.dont_adjust                      = p.dont_adjust != 0;
    out.monotonic                        = p.monotonic != 0;
    out.complete                         = p.complete != 0;
    out.use_arachne                      = p.use_arachne != 0;
    out.layer_height                     = p.layer_height;
    out.gyroid_optimized                 = p.gyroid_optimized != 0;
    out.adaptive_infill_mode             = AdaptiveInfillMode(p.adaptive_infill_mode);
    out.adaptive_infill_interior_density = p.adaptive_infill_interior_density;
    out.adaptive_infill_gradient         = AdaptiveInfillGradient(p.adaptive_infill_gradient);
    out.smooth_factor                    = p.smooth_factor;
    out.lateral_lattice_angle_1          = p.lateral_lattice_angle_1;
    out.lateral_lattice_angle_2          = p.lateral_lattice_angle_2;
    out.pattern                          = InfillPattern(p.pattern);
    out.infill_overhang_angle            = p.infill_overhang_angle;
    out.flow                             = to_flow(p.flow);
    out.extrusion_role                   = ExtrusionRole(p.extrusion_role);
    out.using_internal_flow              = p.using_internal_flow != 0;
    out.no_extrusion_overlap             = p.no_extrusion_overlap;
    out.config                           = nullptr;
    out.dont_sort                        = p.dont_sort != 0;
    out.can_reverse                      = p.can_reverse != 0;
    out.fill_order                       = SurfaceFillOrder(p.fill_order);
    out.horiz_move                       = p.horiz_move;
    out.symmetric_infill_y_axis          = p.symmetric_infill_y_axis != 0;
    out.infill_complete_top              = p.infill_complete_top != 0;
    out.symmetric_y_axis                 = p.symmetric_y_axis;
    out.locked_zag                       = p.locked_zag != 0;
    out.infill_lock_depth                = p.infill_lock_depth;
    out.skin_infill_depth                = p.skin_infill_depth;
    out.center_of_surface_pattern        = CenterOfSurfacePattern(p.center_of_surface_pattern);
    return out;
}

void apply_fill_state(const orca_infill_fill_state &source, Fill &fill)
{
    const orca_infill_fill_state f = read(source);
    fill.layer_id                               = f.layer_id == UINT64_MAX ? size_t(-1) : size_t(f.layer_id);
    fill.z                                      = f.z;
    fill.spacing                                = f.spacing;
    fill.overlap                                = f.overlap;
    fill.angle                                  = f.angle;
    fill.fixed_angle                            = f.fixed_angle != 0;
    fill.link_max_length                        = f.link_max_length;
    fill.loop_clipping                          = f.loop_clipping;
    fill.bounding_box.min                       = Point(f.bounding_box.min.x, f.bounding_box.min.y);
    fill.bounding_box.max                       = Point(f.bounding_box.max.x, f.bounding_box.max.y);
    fill.bounding_box.defined                   = f.bounding_box_defined != 0;
    fill.adapt_fill_octree                      = nullptr;
    fill.adaptive_infill_field                  = nullptr;
    fill.object_state                           = nullptr;
    fill.print_config                           = nullptr;
    fill.print_object_config                    = nullptr;
    fill.min_nozzle_diameter                    = f.min_nozzle_diameter;
    fill.calib_flowrate_topinfill_special_order = f.calib_flowrate_topinfill_special_order != 0;
    fill.no_overlap_expolygons                  = to_expolygons(f.no_overlap);
    fill.dont_alternate_fill_direction          = f.dont_alternate_fill_direction != 0;
}

void* to_state(const orca_infill_fill_state &state) { return read(state).state; }

FieldSlices to_field_slices(const orca_infill_field_input &source)
{
    const orca_infill_field_input input = read(source);
    FieldSlices out;
    out.expolygons.reserve(input.slice_count);
    for (size_t i = 0; i < input.slice_count; ++ i)
        out.expolygons.emplace_back(to_expolygons(input.slices[i].lslices));
    out.slices.reserve(input.slice_count);
    for (size_t i = 0; i < input.slice_count; ++ i)
        out.slices.push_back({ input.slices[i].bottom_z, input.slices[i].print_z, &out.expolygons[i] });
    out.bbox.min     = Point(input.bbox.min.x, input.bbox.min.y);
    out.bbox.max     = Point(input.bbox.max.x, input.bbox.max.y);
    out.bbox.defined = input.bbox_defined != 0;
    out.mode         = AdaptiveInfillMode(input.mode);
    return out;
}

OctreeMesh to_octree_mesh(const orca_infill_octree_input &source)
{
    const orca_infill_octree_input input = read(source);
    OctreeMesh out;
    out.mesh.vertices.reserve(input.vertex_count);
    for (size_t i = 0; i < input.vertex_count; ++ i)
        out.mesh.vertices.emplace_back(input.vertices[3 * i], input.vertices[3 * i + 1], input.vertices[3 * i + 2]);
    out.mesh.indices.reserve(input.triangle_count);
    for (size_t i = 0; i < input.triangle_count; ++ i)
        out.mesh.indices.emplace_back(input.indices[3 * i], input.indices[3 * i + 1], input.indices[3 * i + 2]);
    out.overhangs.reserve(input.overhang_vertex_count);
    for (size_t i = 0; i < input.overhang_vertex_count; ++ i)
        out.overhangs.emplace_back(input.overhangs[3 * i], input.overhangs[3 * i + 1], input.overhangs[3 * i + 2]);
    out.line_spacing           = input.line_spacing;
    out.support_overhangs_only = input.support_overhangs_only != 0;
    return out;
}

LightningLayers to_lightning_layers(const orca_infill_lightning_input &source)
{
    const orca_infill_lightning_input input = read(source);
    LightningLayers out;
    out.params    = { input.infill_extrusion_width, input.supporting_radius, input.wall_supporting_radius, input.prune_length,
                      input.straightening_max_distance };
    out.outlines  = to_layers(input.outlines, input.layer_count);
    out.overhangs = to_layers(input.overhangs, input.layer_count);
    return out;
}

LockRegionParam to_lock_region_param(const orca_infill_lock &source)
{
    const orca_infill_lock lock = read(source);
    LockRegionParam out;
    rebuild(out.skin_density_params, lock.skin_density, lock.skin_density_count);
    rebuild(out.skeleton_density_params, lock.skeleton_density, lock.skeleton_density_count);
    rebuild(out.skeleton_flow_params, lock.skeleton_flow, lock.skeleton_flow_count);
    rebuild(out.skin_flow_params, lock.skin_flow, lock.skin_flow_count);
    return out;
}

} // namespace BuiltinMirror
} // namespace Slic3r
