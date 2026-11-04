#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "Arachne/WallToolPaths.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"

#include "FillConcentricInternal.hpp"
#include <cassert>
#include <cstddef>
#include <algorithm>
#include <libslic3r/ShortestPath.hpp>
#include <vector>
#include <utility>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Flow.hpp"

namespace Slic3r {

ThickPolylines FillConcentricInternal::concentric_internal_lines(const FillParams& params)
{
    ThickPolylines thick_polylines_out;

    for (size_t i = 0; i < this->no_overlap_expolygons.size(); ++i) {
        ExPolygon &expolygon = this->no_overlap_expolygons[i];

        // no rotation is supported for this infill pattern
        Point   bbox_size = expolygon.contour.bounding_box().size();
        coord_t min_spacing = params.flow.scaled_spacing();

        coord_t                loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
        Polygons               polygons = to_polygons(expolygon);

        Arachne::WallToolPathsParams input_params;
        input_params.min_bead_width = 0.85 * min_nozzle_diameter;
        input_params.min_feature_size = 0.25 * min_nozzle_diameter;
        input_params.wall_transition_length = 0.4;
        input_params.wall_transition_angle = 10;
        input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
        input_params.wall_distribution_count = 1;

        Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);

        std::vector<Arachne::VariableWidthLines>    loops = wallToolPaths.getToolPaths();
        std::vector<const Arachne::ExtrusionLine*> all_extrusions;
        for (Arachne::VariableWidthLines& loop : loops) {
            if (loop.empty())
                continue;
            for (const Arachne::ExtrusionLine& wall : loop)
                all_extrusions.emplace_back(&wall);
        }

        // Split paths using a nearest neighbor search.
        size_t firts_poly_idx = thick_polylines_out.size();
        Point  last_pos(0, 0);
        for (const Arachne::ExtrusionLine* extrusion : all_extrusions) {
            if (extrusion->empty())
                continue;

            ThickPolyline thick_polyline = Arachne::to_thick_polyline(*extrusion);
            if (extrusion->is_closed && thick_polyline.points.front() == thick_polyline.points.back() && thick_polyline.width.front() == thick_polyline.width.back()) {
                thick_polyline.points.pop_back();
                assert(thick_polyline.points.size() * 2 == thick_polyline.width.size());
                int nearest_idx = last_pos.nearest_point_index(thick_polyline.points);
                std::rotate(thick_polyline.points.begin(), thick_polyline.points.begin() + nearest_idx, thick_polyline.points.end());
                std::rotate(thick_polyline.width.begin(), thick_polyline.width.begin() + 2 * nearest_idx, thick_polyline.width.end());
                thick_polyline.points.emplace_back(thick_polyline.points.front());
            }
            thick_polylines_out.emplace_back(std::move(thick_polyline));
        }

        // clip the paths to prevent the extruder from getting exactly on the first point of the loop
        // Keep valid paths only.
        size_t j = firts_poly_idx;
        for (size_t i = firts_poly_idx; i < thick_polylines_out.size(); ++i) {
            thick_polylines_out[i].clip_end(this->loop_clipping);
            if (thick_polylines_out[i].is_valid()) {
                if (j < i)
                    thick_polylines_out[j] = std::move(thick_polylines_out[i]);
                ++j;
            }
        }
        if (j < thick_polylines_out.size())
            thick_polylines_out.erase(thick_polylines_out.begin() + int(j), thick_polylines_out.end());

        reorder_by_shortest_traverse(thick_polylines_out);
    }

    return thick_polylines_out;
}


} // namespace Slic3r
