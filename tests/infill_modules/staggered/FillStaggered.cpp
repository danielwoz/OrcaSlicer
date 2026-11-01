#include "FillStaggered.hpp"

#include <algorithm>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

void FillStaggered::_fill_surface_single(const FillParams &params, unsigned int /* thickness_layers */,
                                         const std::pair<float, Point> &direction, ExPolygon expolygon, Polylines &polylines_out)
{
    // Without a region configuration (the pattern used as a tool) the option defaults apply.
    const double scale = params.config != nullptr ? params.config->staggered_spacing_scale.value : 1.;
    const bool   cross = params.config != nullptr && params.config->staggered_cross.value;

    // Work in the frame of the infill direction; lines laid out from the infill origin continue across regions.
    const float angle  = direction.first;
    Point       origin = direction.second;
    expolygon.rotate(-angle);
    origin.rotate(-angle);

    const coord_t distance = coord_t(scale_(this->spacing * params.multiline * scale * (cross ? 2. : 1.) /
                                            std::max(double(params.density), 0.001)));
    if (distance <= 0)
        return;
    // Every other layer moves the lines by half their distance.
    const coord_t shift = this->layer_id != size_t(-1) && (this->layer_id & 1) ? distance / 2 : 0;
    BoundingBox bbox = get_extents(expolygon);
    bbox.offset(scale_(this->spacing * params.multiline));

    Polylines lines;
    for (coord_t y = align_to_grid(bbox.min.y(), distance, origin.y() + shift); y <= bbox.max.y(); y += distance)
        lines.emplace_back(Point(bbox.min.x(), y), Point(bbox.max.x(), y));
    if (cross)
        for (coord_t x = align_to_grid(bbox.min.x(), distance, origin.x() + shift); x <= bbox.max.x(); x += distance)
            lines.emplace_back(Point(x, bbox.min.y()), Point(x, bbox.max.y()));

    multiline_fill(lines, params, float(this->spacing));
    lines = intersection_pl(std::move(lines), expolygon);
    const double min_length = scale_(0.8 * this->spacing);
    lines.erase(std::remove_if(lines.begin(), lines.end(), [min_length](const Polyline &pl) { return pl.length() < min_length; }),
                lines.end());
    if (lines.empty())
        return;
    const size_t first = polylines_out.size();
    chain_or_connect_infill(std::move(lines), expolygon, polylines_out, this->spacing, params);
    for (auto it = polylines_out.begin() + first; it != polylines_out.end(); ++ it)
        it->rotate(angle);
}

} // namespace Slic3r
