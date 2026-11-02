#include "FillPlugin.hpp"

#include <algorithm>

#include "../ClipperUtils.hpp"
#include "../I18N.hpp"
#include "../format.hpp"
#include "InfillPlugins.hpp"

namespace Slic3r {

InfillPluginRegion::InfillPluginRegion(const ExPolygon &expolygon)
{
    size_t count = expolygon.contour.size();
    for (const Polygon &hole : expolygon.holes)
        count += hole.size();
    m_points.reserve(count);
    auto add = [this](const Polygon &polygon) {
        for (const Point &p : polygon.points)
            m_points.push_back({ p.x(), p.y() });
    };
    add(expolygon.contour);
    for (const Polygon &hole : expolygon.holes)
        add(hole);
    // The points are in place, so the paths can point into them.
    const orca_infill_point *next = m_points.data();
    m_view.contour = { next, expolygon.contour.size() };
    next += expolygon.contour.size();
    for (const Polygon &hole : expolygon.holes) {
        m_holes.push_back({ next, hole.size() });
        next += hole.size();
    }
    m_view.holes      = m_holes.data();
    m_view.hole_count = m_holes.size();
}

void FillPlugin::_fill_surface_single(const FillParams &params, unsigned int thickness_layers, const std::pair<float, Point> &direction,
                                      ExPolygon expolygon, Polylines &polylines_out)
{
    // The plugin works in the pattern frame, whose x axis is the infill direction.
    const float angle = direction.first;
    expolygon.rotate(-angle);
    Point origin = direction.second;
    origin.rotate(-angle);
    // fill_multiline applies only to a pattern that lets the host draw the copies.
    const int multiline = (m_pattern->caps & ORCA_INFILL_CAP_MULTILINE) ? std::max(1, params.multiline) : 1;
    BoundingBox bbox = get_extents(expolygon);
    bbox.offset(scale_(this->spacing * multiline));

    // A Fill serves one fill group, and the parameters are part of the group key, so one reading per config holds.
    if (! m_params_read || m_params_config != params.config) {
        m_params        = infill_plugin_param_values(m_pattern->id, params.config);
        m_params_config = params.config;
        m_params_read   = true;
    }

    const InfillPluginRegion region(expolygon);
    orca_infill_request request {};
    request.struct_size      = sizeof(request);
    request.region           = &region.view();
    request.bbox             = { { bbox.min.x(), bbox.min.y() }, { bbox.max.x(), bbox.max.y() } };
    request.origin           = { origin.x(), origin.y() };
    request.units_per_mm     = 1. / SCALING_FACTOR;
    request.angle            = angle;
    request.spacing          = this->spacing;
    request.density          = params.density;
    request.z                = this->z;
    request.layer_height     = params.layer_height;
    request.layer_index      = this->layer_id == size_t(-1) ? -1 : int64_t(this->layer_id);
    request.thickness_layers = std::max(1u, thickness_layers);
    request.multiline        = multiline;
    request.role             = params.extrusion_role == erTopSolidInfill ? ORCA_INFILL_ROLE_TOP :
                               params.extrusion_role == erBottomSurface  ? ORCA_INFILL_ROLE_BOTTOM :
                               params.extrusion_role == erSolidInfill    ? ORCA_INFILL_ROLE_INTERNAL_SOLID : ORCA_INFILL_ROLE_SPARSE;
    request.params           = m_params.data();
    request.param_count      = m_params.size();

    Polylines lines;
    if (const orca_infill_status status = call_infill_plugin(*m_pattern, request, lines); status == ORCA_INFILL_ERR_FAILED)
        // The pattern cannot fill this region: it stays empty, as with a built-in pattern.
        throw InfillFailedException();
    else if (status != ORCA_INFILL_OK)
        throw Slic3r::SlicingError(Slic3r::format(_u8L("The infill pattern \"%1%\" of \"%2%\" failed (error %3%)."),
                                                  m_pattern->key, m_pattern->plugin, status));

    const uint32_t post = m_pattern->post;
    if ((post & ORCA_INFILL_POST_SMOOTH) && params.smooth_factor > 0.)
        smooth_polylines_corners(lines, params.corner_smoothing());
    if (m_pattern->caps & ORCA_INFILL_CAP_MULTILINE)
        multiline_fill(lines, params, float(this->spacing));
    if (post & ORCA_INFILL_POST_CLIP)
        lines = intersection_pl(std::move(lines), expolygon);
    if (post & ORCA_INFILL_POST_DROP_SHORT) {
        const double min_length = scale_(0.8 * this->spacing);
        lines.erase(std::remove_if(lines.begin(), lines.end(), [min_length](const Polyline &pl) { return pl.length() < min_length; }), lines.end());
    }
    if (lines.empty())
        return;
    const size_t first = polylines_out.size();
    if (post & ORCA_INFILL_POST_CONNECT)
        chain_or_connect_infill(std::move(lines), expolygon, polylines_out, this->spacing, params);
    else
        append(polylines_out, std::move(lines));
    for (auto it = polylines_out.begin() + first; it != polylines_out.end(); ++ it)
        it->rotate(angle);
}

} // namespace Slic3r
