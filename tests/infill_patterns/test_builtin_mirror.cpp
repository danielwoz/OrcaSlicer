#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/pfr/core.hpp>

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/Library/BuiltinMirror.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include "native_fillers.hpp"

using namespace Slic3r;
namespace Mirror = Slic3r::BuiltinMirror;

namespace {

// A region with two holes, whose contour runs clockwise and repeats a point, so that a reoriented or cleaned copy shows.
ExPolygon odd_region(int shift)
{
    ExPolygon out;
    out.contour = Polygon{ { shift, 0 }, { shift, 1000 }, { shift, 1000 }, { shift + 2000, 1000 }, { shift + 2000, 0 } };
    out.holes   = { Polygon{ { shift + 100, 100 }, { shift + 300, 100 }, { shift + 300, 300 } },
                    Polygon{ { shift + 500, 500 }, { shift + 600, 700 }, { shift + 700, 500 }, { shift + 650, 400 } } };
    return out;
}

bool same_flow(const Flow &a, const Flow &b)
{
    return a.width() == b.width() && a.height() == b.height() && a.spacing() == b.spacing() && a.nozzle_diameter() == b.nozzle_diameter() &&
           a.bridge() == b.bridge();
}

// Stands for the objects a pointer member points at; it is never dereferenced.
int marker = 0;
template<class T> T* fake() { return reinterpret_cast<T*>(&marker); }

// A value other than the default for every field of FillParams.
FillParams distinct_params()
{
    FillParams p;
    p.density                          = 0.37f;
    p.multiline                        = 3;
    p.anchor_length                    = 2.5f;
    p.anchor_length_max                = 11.f;
    p.resolution                       = 0.021;
    p.dont_adjust                      = false;
    p.monotonic                        = true;
    p.complete                         = true;
    p.use_arachne                      = true;
    p.layer_height                     = 0.28;
    p.gyroid_optimized                 = true;
    p.adaptive_infill_mode             = AdaptiveInfillMode::Lobes;
    p.adaptive_infill_interior_density = 0.12f;
    p.adaptive_infill_gradient         = AdaptiveInfillGradient::Exponential;
    p.smooth_factor                    = 0.45;
    p.lateral_lattice_angle_1          = -12.5;
    p.lateral_lattice_angle_2          = 33.25;
    p.pattern                          = ipGyroid;
    p.infill_overhang_angle            = 47.f;
    p.flow                             = Flow::raw(0.47f, 0.21f, 0.43f, 0.6f, true);
    p.extrusion_role                   = erInternalBridgeInfill;
    p.using_internal_flow              = true;
    p.no_extrusion_overlap             = 0.03f;
    p.config                           = fake<const PrintRegionConfig>();
    p.dont_sort                        = true;
    p.can_reverse                      = false;
    p.fill_order                       = SurfaceFillOrder::Inward;
    p.horiz_move                       = 1.75f;
    p.symmetric_infill_y_axis          = true;
    p.infill_complete_top              = true;
    p.symmetric_y_axis                 = 123456789012;
    p.locked_zag                       = true;
    p.infill_lock_depth                = 1.25f;
    p.skin_infill_depth                = 2.5f;
    p.center_of_surface_pattern        = CenterOfSurfacePattern::Each_Assembly;
    return p;
}

// One field of FillParams: the input differs from the default and the output equals the input.
template<class T> void check_field(size_t index, const T &in, const T &out, const T &defaults)
{
    CAPTURE(index);
    CHECK(in != defaults);
    CHECK(out == in);
}
void check_field(size_t index, const Flow &in, const Flow &out, const Flow &defaults)
{
    CAPTURE(index);
    CHECK(in != defaults);
    CHECK(in.spacing() != defaults.spacing());
    CHECK(same_flow(out, in));
}
// FillParams::config does not cross.
void check_field(size_t index, const PrintRegionConfig *const &in, const PrintRegionConfig *const &out, const PrintRegionConfig *const &)
{
    CAPTURE(index);
    CHECK(in != nullptr);
    CHECK(out == nullptr);
}
template<size_t... I> void check_fill_params(const FillParams &in, const FillParams &out, std::index_sequence<I...>)
{
    const FillParams defaults;
    (check_field(I, boost::pfr::get<I>(in), boost::pfr::get<I>(out), boost::pfr::get<I>(defaults)), ...);
}

// The members orca_infill_fill_state mirrors; the configuration and object state pointers are left out.
void check_members(const Fill &out, const Fill &in, bool with_no_overlap)
{
    CHECK(out.layer_id == in.layer_id);
    CHECK(out.z == in.z);
    CHECK(out.spacing == in.spacing);
    CHECK(out.overlap == in.overlap);
    CHECK(out.angle == in.angle);
    CHECK(out.fixed_angle == in.fixed_angle);
    CHECK(out.link_max_length == in.link_max_length);
    CHECK(out.loop_clipping == in.loop_clipping);
    CHECK(out.bounding_box.min == in.bounding_box.min);
    CHECK(out.bounding_box.max == in.bounding_box.max);
    CHECK(out.bounding_box.defined == in.bounding_box.defined);
    CHECK(out.min_nozzle_diameter == in.min_nozzle_diameter);
    CHECK(out.calib_flowrate_topinfill_special_order == in.calib_flowrate_topinfill_special_order);
    CHECK(out.no_overlap_expolygons == (with_no_overlap ? in.no_overlap_expolygons : ExPolygons()));
    CHECK(out.dont_alternate_fill_direction == in.dont_alternate_fill_direction);
}

template<class Map> void check_density_map(const Map &out, const Map &in)
{
    REQUIRE(out.size() == in.size());
    for (auto a = out.begin(), b = in.begin(); a != out.end(); ++ a, ++ b) {
        CHECK(a->first == b->first);
        CHECK(a->second == b->second);
    }
}
template<class Map> void check_flow_map(const Map &out, const Map &in)
{
    REQUIRE(out.size() == in.size());
    for (auto a = out.begin(), b = in.begin(); a != out.end(); ++ a, ++ b) {
        CHECK(same_flow(a->first, b->first));
        CHECK(a->second == b->second);
    }
}

std::set<int> enum_values(const char *opt_key)
{
    std::set<int> out;
    for (const auto &[name, value] : *print_config_def.get(opt_key)->enum_keys_map)
        out.insert(value);
    return out;
}
std::set<int> first_values(int count)
{
    std::set<int> out;
    for (int i = 0; i < count; ++ i)
        out.insert(i);
    return out;
}

} // namespace

TEST_CASE("Built-in mirror: every field of FillParams but config round trips", "[InfillPatterns][BuiltinMirror]")
{
    const FillParams            params = distinct_params();
    const std::unique_ptr<Fill> fill = InfillCorpus::native_filler(ipRectilinear);
    const Mirror::Request       request(ORCA_INFILL_CALL_FILL_SURFACE, Surface(), params, *fill);
    REQUIRE(request.get().params != nullptr);
    CHECK(request.get().params->struct_size == sizeof(orca_infill_fill_params));
    check_fill_params(params, Mirror::to_fill_params(*request.get().params),
                      std::make_index_sequence<boost::pfr::tuple_size_v<FillParams>>());
}

TEST_CASE("Built-in mirror: fields beyond struct_size read as zero", "[InfillPatterns][BuiltinMirror]")
{
    const FillParams            params = distinct_params();
    const std::unique_ptr<Fill> fill = InfillCorpus::native_filler(ipRectilinear);
    const Mirror::Request       request(ORCA_INFILL_CALL_FILL_SURFACE, Surface(), params, *fill);
    orca_infill_fill_params     shorter = *request.get().params;
    shorter.struct_size = uint32_t(offsetof(orca_infill_fill_params, flow));
    const FillParams out = Mirror::to_fill_params(shorter);
    CHECK(out.density == params.density);
    CHECK(out.infill_overhang_angle == params.infill_overhang_angle);
    CHECK(same_flow(out.flow, Flow::raw(0.f, 0.f, 0.f, 0.f, false)));
    CHECK(out.extrusion_role == erNone);
    CHECK_FALSE(out.can_reverse);
    CHECK(out.symmetric_y_axis == 0);
    CHECK(out.center_of_surface_pattern == CenterOfSurfacePattern::Each_Surface);
}

TEST_CASE("Built-in mirror: every Fill member round trips", "[InfillPatterns][BuiltinMirror]")
{
    const std::unique_ptr<Fill> defaults = InfillCorpus::native_filler(ipRectilinear);
    const std::unique_ptr<Fill> host = InfillCorpus::native_filler(ipRectilinear);
    const std::unique_ptr<Fill> library = InfillCorpus::native_filler(ipRectilinear);
    int                         state = 0;

    host->layer_id                               = 17;
    host->z                                      = 3.4;
    host->spacing                                = 0.43;
    host->overlap                                = 0.07;
    host->angle                                  = 1.234f;
    host->fixed_angle                            = true;
    host->link_max_length                        = 12345678901;
    host->loop_clipping                          = 678;
    host->bounding_box                           = BoundingBox(Point(-100, -200), Point(3000, 4000));
    host->adapt_fill_octree                      = fake<FillAdaptive::Octree>();
    host->adaptive_infill_field                  = fake<const AdaptiveInfillField>();
    host->object_state                           = fake<const InfillObjectState>();
    host->print_config                           = fake<const PrintConfig>();
    host->print_object_config                    = fake<const PrintObjectConfig>();
    host->min_nozzle_diameter                    = 0.6;
    host->calib_flowrate_topinfill_special_order = true;
    host->no_overlap_expolygons                  = { odd_region(0), odd_region(5000) };
    host->dont_alternate_fill_direction          = true;

    CHECK(host->layer_id != defaults->layer_id);
    CHECK(host->z != defaults->z);
    CHECK(host->spacing != defaults->spacing);
    CHECK(host->overlap != defaults->overlap);
    CHECK(host->angle != defaults->angle);
    CHECK(host->fixed_angle != defaults->fixed_angle);
    CHECK(host->link_max_length != defaults->link_max_length);
    CHECK(host->loop_clipping != defaults->loop_clipping);
    CHECK(host->bounding_box.min != defaults->bounding_box.min);
    CHECK(host->bounding_box.max != defaults->bounding_box.max);
    CHECK(host->bounding_box.defined != defaults->bounding_box.defined);
    CHECK(host->min_nozzle_diameter != defaults->min_nozzle_diameter);
    CHECK(host->calib_flowrate_topinfill_special_order != defaults->calib_flowrate_topinfill_special_order);
    CHECK(host->no_overlap_expolygons != defaults->no_overlap_expolygons);
    CHECK(host->dont_alternate_fill_direction != defaults->dont_alternate_fill_direction);

    // Values left in the library's filler by an earlier call are all replaced.
    library->layer_id              = 3;
    library->bounding_box          = BoundingBox(Point(7, 7), Point(9, 9));
    library->adapt_fill_octree     = host->adapt_fill_octree;
    library->adaptive_infill_field = host->adaptive_infill_field;
    library->object_state          = host->object_state;
    library->print_config          = host->print_config;
    library->print_object_config   = host->print_object_config;
    library->no_overlap_expolygons = { odd_region(-3000) };

    SECTION("a defined box, with and without the no-overlap area") {
        for (const int32_t call : { ORCA_INFILL_CALL_FILL_SURFACE, ORCA_INFILL_CALL_CONCENTRIC_INTERNAL }) {
            CAPTURE(call);
            const Mirror::Request request(call, Surface(), FillParams(), *host, nullptr, &state);
            REQUIRE(request.get().fill != nullptr);
            CHECK(request.get().fill->state == &state);
            CHECK(Mirror::to_state(*request.get().fill) == &state);
            CHECK(request.get().lock == nullptr);
            Mirror::apply_fill_state(*request.get().fill, *library);
            check_members(*library, *host, call == ORCA_INFILL_CALL_CONCENTRIC_INTERNAL);
            CHECK(library->adapt_fill_octree == nullptr);
            CHECK(library->adaptive_infill_field == nullptr);
            CHECK(library->object_state == nullptr);
            CHECK(library->print_config == nullptr);
            CHECK(library->print_object_config == nullptr);
        }
    }
    SECTION("an undefined box and layer size_t(-1)") {
        // Fill's default box is undefined with max < min; the corners pass as they are either way.
        host->bounding_box         = BoundingBox(Point(500, 600), Point(-1, -2));
        host->bounding_box.defined = false;
        host->layer_id             = size_t(-1);
        const Mirror::Request request(ORCA_INFILL_CALL_FILL_SURFACE, Surface(), FillParams(), *host);
        CHECK(request.get().fill->layer_id == UINT64_MAX);
        CHECK(request.get().fill->state == nullptr);
        Mirror::apply_fill_state(*request.get().fill, *library);
        check_members(*library, *host, false);
    }
}

TEST_CASE("Built-in mirror: every Surface field round trips, viewed in place", "[InfillPatterns][BuiltinMirror]")
{
    Surface surface(stInternalBridge, odd_region(0));
    surface.thickness        = 0.37;
    surface.thickness_layers = 3;
    surface.bridge_angle     = 0.7;
    surface.extra_perimeters = 2;
    const Surface defaults;
    CHECK(surface.surface_type != defaults.surface_type);
    CHECK(surface.thickness != defaults.thickness);
    CHECK(surface.thickness_layers != defaults.thickness_layers);
    CHECK(surface.bridge_angle != defaults.bridge_angle);
    CHECK(surface.extra_perimeters != defaults.extra_perimeters);

    const std::unique_ptr<Fill> fill = InfillCorpus::native_filler(ipRectilinear);
    const Mirror::Request       request(ORCA_INFILL_CALL_FILL_SURFACE, surface, FillParams(), *fill);
    const orca_infill_surface  &view = *request.get().surface;
    CHECK(view.struct_size == sizeof(orca_infill_surface));
    CHECK(view.surface_type == ORCA_INFILL_ST_INTERNAL_BRIDGE);
    CHECK(view.expolygon.contour.points == Mirror::view(surface.expolygon.contour.points));
    CHECK(view.expolygon.contour.count == 5);
    REQUIRE(view.expolygon.hole_count == 2);
    CHECK(view.expolygon.holes[1].points == Mirror::view(surface.expolygon.holes[1].points));
    CHECK(view.expolygon.holes[1].points[2].x == 700);
    CHECK(view.expolygon.holes[1].points[2].y == 500);

    const Surface out = Mirror::to_surface(view);
    CHECK(out.surface_type == surface.surface_type);
    CHECK(out.expolygon == surface.expolygon);
    CHECK(out.thickness == surface.thickness);
    CHECK(out.thickness_layers == surface.thickness_layers);
    CHECK(out.bridge_angle == surface.bridge_angle);
    CHECK(out.extra_perimeters == surface.extra_perimeters);

    const Mirror::Request empty(ORCA_INFILL_CALL_FILL_SURFACE, Surface(stTop), FillParams(), *fill);
    CHECK(empty.get().surface->expolygon.contour.count == 0);
    CHECK(empty.get().surface->expolygon.holes == nullptr);
    CHECK(Mirror::to_surface(*empty.get().surface).expolygon == ExPolygon());
}

TEST_CASE("Built-in mirror: the slices of an adaptive infill field round trip, viewed in place", "[InfillPatterns][BuiltinMirror]")
{
    const ExPolygons                              first { odd_region(0), odd_region(5000) }, none, last { odd_region(-3000) };
    const std::vector<AdaptiveInfillField::Slice> slices { { 0.1, 0.3, &first }, { 0.3, 0.55, &none }, { 0.55, 0.75, &last } };
    const BoundingBox                             bbox(Point(-3000, 0), Point(7000, 1000));
    const Mirror::FieldInput                      input(slices, bbox);
    const orca_infill_field_input                 view = input.get(AdaptiveInfillMode::NormalY);
    CHECK(view.struct_size == sizeof(orca_infill_field_input));
    CHECK(view.mode == ORCA_INFILL_ADAPTIVE_NORMAL_Y);
    CHECK(input.get(AdaptiveInfillMode::Lobes).mode == ORCA_INFILL_ADAPTIVE_LOBES);
    CHECK(view.bbox_defined == 1);
    CHECK(view.bbox.min.x == -3000);
    CHECK(view.bbox.max.y == 1000);
    REQUIRE(view.slice_count == 3);
    CHECK(view.slices[0].bottom_z == 0.1);
    CHECK(view.slices[0].print_z == 0.3);
    REQUIRE(view.slices[0].lslices.count == 2);
    CHECK(view.slices[0].lslices.items[1].contour.points == Mirror::view(first[1].contour.points));
    REQUIRE(view.slices[0].lslices.items[1].hole_count == 2);
    CHECK(view.slices[0].lslices.items[1].holes[1].points == Mirror::view(first[1].holes[1].points));
    CHECK(view.slices[1].lslices.count == 0);
    CHECK(view.slices[1].lslices.items == nullptr);
    REQUIRE(view.slices[2].lslices.count == 1);
    CHECK(view.slices[2].lslices.items[0].holes[0].points == Mirror::view(last[0].holes[0].points));

    const Mirror::FieldSlices out = Mirror::to_field_slices(view);
    CHECK(out.mode == AdaptiveInfillMode::NormalY);
    CHECK(out.bbox.min == bbox.min);
    CHECK(out.bbox.max == bbox.max);
    CHECK(out.bbox.defined);
    REQUIRE(out.slices.size() == slices.size());
    for (size_t i = 0; i < slices.size(); ++ i) {
        CAPTURE(i);
        CHECK(out.slices[i].bottom_z == slices[i].bottom_z);
        CHECK(out.slices[i].top_z == slices[i].top_z);
        CHECK(out.slices[i].expolygons == &out.expolygons[i]);
        CHECK(*out.slices[i].expolygons == *slices[i].expolygons);
    }

    // An undefined box passes its corners as they are; fields beyond struct_size read as zero.
    BoundingBox undefined(Point(500, 600), Point(-1, -2));
    undefined.defined = false;
    const Mirror::FieldInput nothing({}, undefined);
    const Mirror::FieldSlices empty = Mirror::to_field_slices(nothing.get(AdaptiveInfillMode::Lobes));
    CHECK(empty.slices.empty());
    CHECK(empty.bbox.min == undefined.min);
    CHECK(empty.bbox.max == undefined.max);
    CHECK_FALSE(empty.bbox.defined);
    orca_infill_field_input shorter = view;
    shorter.struct_size = uint32_t(offsetof(orca_infill_field_input, bbox));
    const Mirror::FieldSlices partial = Mirror::to_field_slices(shorter);
    CHECK(partial.slices.size() == 3);
    CHECK_FALSE(partial.bbox.defined);
    CHECK(partial.mode == AdaptiveInfillMode::Disabled);
}

TEST_CASE("Built-in mirror: LockRegionParam round trips in map order", "[InfillPatterns][BuiltinMirror]")
{
    // Ordered by mm3_per_mm: thin, middle, wide.
    const Flow thin = Flow::raw(0.4f, 0.2f, 0.37f, 0.4f, false), middle = Flow::raw(0.6f, 0.2f, 0.55f, 0.4f, false),
               wide = Flow::raw(0.8f, 0.2f, 0.75f, 0.4f, false);
    LockRegionParam lock;
    lock.skin_density_params     = { { 0.4f, { odd_region(100), odd_region(9000) } }, { 0.15f, { odd_region(0) } } };
    lock.skeleton_density_params = { { 0.25f, { odd_region(300) } } };
    lock.skeleton_flow_params    = { { wide, { odd_region(0) } }, { thin, { odd_region(500), odd_region(-4000) } } };
    lock.skin_flow_params        = { { middle, { odd_region(700) } } };

    const std::unique_ptr<Fill> fill = InfillCorpus::native_filler(ipRectilinear);
    const Mirror::Request       request(ORCA_INFILL_CALL_LOCKED_ZAG, Surface(stInternal, odd_region(0)), FillParams(), *fill, &lock);
    REQUIRE(request.get().lock != nullptr);
    const orca_infill_lock &view = *request.get().lock;
    CHECK(view.struct_size == sizeof(orca_infill_lock));
    REQUIRE(view.skin_density_count == 2);
    REQUIRE(view.skeleton_density_count == 1);
    REQUIRE(view.skeleton_flow_count == 2);
    REQUIRE(view.skin_flow_count == 1);
    CHECK(view.skin_density[0].density == 0.15f);
    CHECK(view.skin_density[1].regions.count == 2);
    // Flow indices count the skeleton flows first, then the skin flows.
    CHECK(view.skeleton_flow[0].flow.width == thin.width());
    CHECK(view.skeleton_flow[1].flow.width == wide.width());
    CHECK(view.skin_flow[0].flow.width == middle.width());
    CHECK(view.skin_flow == view.skeleton_flow + 2);

    const LockRegionParam out = Mirror::to_lock_region_param(view);
    check_density_map(out.skin_density_params, lock.skin_density_params);
    check_density_map(out.skeleton_density_params, lock.skeleton_density_params);
    check_flow_map(out.skeleton_flow_params, lock.skeleton_flow_params);
    check_flow_map(out.skin_flow_params, lock.skin_flow_params);

    const LockRegionParam empty;
    const Mirror::Request none(ORCA_INFILL_CALL_LOCKED_ZAG, Surface(), FillParams(), *fill, &empty);
    const LockRegionParam empty_out = Mirror::to_lock_region_param(*none.get().lock);
    CHECK(empty_out.skin_density_params.empty());
    CHECK(empty_out.skeleton_density_params.empty());
    CHECK(empty_out.skeleton_flow_params.empty());
    CHECK(empty_out.skin_flow_params.empty());
}

TEST_CASE("Built-in mirror: Flow::raw keeps every value, and flows round trip", "[InfillPatterns][BuiltinMirror]")
{
    const Flow raw = Flow::raw(0.45f, 0.2f, 0.123f, 0.6f, true);
    CHECK(raw.width() == 0.45f);
    CHECK(raw.height() == 0.2f);
    CHECK(raw.spacing() == 0.123f);
    CHECK(raw.nozzle_diameter() == 0.6f);
    CHECK(raw.bridge());
    CHECK(raw.spacing() != Flow(0.45f, 0.2f, 0.6f).spacing());
    for (const Flow &flow : { raw, Flow(0.45f, 0.2f, 0.4f), Flow::bridging_flow(0.4f, 0.4f), Flow() })
        CHECK(same_flow(Mirror::to_flow(Mirror::to_c(flow)), flow));
}

TEST_CASE("Built-in mirror: thick polylines round trip", "[InfillPatterns][BuiltinMirror]")
{
    ThickPolyline polyline;
    polyline.points    = { { 0, 0 }, { 1000, 50 }, { 1000, 50 }, { -300, 2000 } };
    polyline.width     = { 400., 410.5, 410.5, 420., 420., 380.25 };
    polyline.endpoints = { true, false };
    const orca_infill_thick_path view = Mirror::view(polyline);
    CHECK(view.points == Mirror::view(polyline.points));
    CHECK(view.widths == polyline.width.data());
    const ThickPolyline out = Mirror::to_thick_polyline(view);
    CHECK(out.points == polyline.points);
    CHECK(out.width == polyline.width);
    CHECK(out.endpoints == polyline.endpoints);
    CHECK(out.fitting_result.empty());
}

TEST_CASE("Built-in mirror: the configuration offers exactly the enumeration values the ABI names", "[InfillPatterns][BuiltinMirror]")
{
    CHECK(enum_values("top_surface_fill_order") == first_values(ORCA_INFILL_ORDER_INWARD + 1));
    CHECK(enum_values("bottom_surface_fill_order") == first_values(ORCA_INFILL_ORDER_INWARD + 1));
    CHECK(enum_values("center_of_surface_pattern") == first_values(ORCA_INFILL_CENTER_EACH_ASSEMBLY + 1));
    CHECK(enum_values("adaptive_infill_mode") == first_values(ORCA_INFILL_ADAPTIVE_NORMAL_X + 1));
    CHECK(enum_values("adaptive_infill_gradient") == first_values(ORCA_INFILL_GRADIENT_EXPONENTIAL + 1));
}
