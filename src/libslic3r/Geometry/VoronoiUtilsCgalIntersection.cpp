// The exact planarity check of VoronoiUtilsCgal. It is the only one that uses exact constructions, and with them GMP
// and MPFR, so it has an object of its own: a binary links that code only when it calls the check, as the assertions
// of Arachne do.
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Arr_segment_traits_2.h>
#include <CGAL/Surface_sweep_2_algorithms.h>
#include <algorithm>
#include <cassert>
#include <iterator>
#include <vector>

#include "libslic3r/Geometry/Voronoi.hpp"
#include "libslic3r/Geometry/VoronoiUtils.hpp"
#include "VoronoiUtilsCgal.hpp"

using VD = Slic3r::Geometry::VoronoiDiagram;

namespace Slic3r::Geometry {

// FIXME Lukas H.: Also includes parabolic segments.
bool VoronoiUtilsCgal::is_voronoi_diagram_planar_intersection(const VD &voronoi_diagram)
{
    using CGAL_E_Point   = CGAL::Exact_predicates_exact_constructions_kernel::Point_2;
    using CGAL_E_Segment = CGAL::Arr_segment_traits_2<CGAL::Exact_predicates_exact_constructions_kernel>::Curve_2;
    auto to_cgal_point   = [](const VD::vertex_type &pt) -> CGAL_E_Point { return {pt.x(), pt.y()}; };

    assert(std::all_of(voronoi_diagram.edges().cbegin(), voronoi_diagram.edges().cend(),
                       [](const VD::edge_type &edge) { return edge.color() == 0; }));

    std::vector<CGAL_E_Segment> segments;
    segments.reserve(voronoi_diagram.num_edges());

    for (const VD::edge_type &edge : voronoi_diagram.edges()) {
        if (edge.color() != 0)
            continue;

        if (edge.is_finite() && edge.is_linear() && edge.vertex0() != nullptr && edge.vertex1() != nullptr &&
            VoronoiUtils::is_finite(*edge.vertex0()) && VoronoiUtils::is_finite(*edge.vertex1())) {
            segments.emplace_back(to_cgal_point(*edge.vertex0()), to_cgal_point(*edge.vertex1()));
            edge.color(1);
            assert(edge.twin() != nullptr);
            edge.twin()->color(1);
        }
    }

    for (const VD::edge_type &edge : voronoi_diagram.edges())
        edge.color(0);

    std::vector<CGAL_E_Point> intersections_pt;
    CGAL::compute_intersection_points(segments.begin(), segments.end(), std::back_inserter(intersections_pt));
    return intersections_pt.empty();
}

} // namespace Slic3r::Geometry
