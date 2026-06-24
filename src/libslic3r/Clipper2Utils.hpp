#ifndef slic3r_Clipper2Utils_hpp_
#define slic3r_Clipper2Utils_hpp_

#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "clipper2/clipper.h"

namespace Slic3r {

Clipper2Lib::Paths64 Slic3rPolylines_to_Paths64(const Slic3r::Polylines& in);
Slic3r::Polylines  Paths64_to_polylines(const Clipper2Lib::Paths64& in);
Slic3r::Polylines  intersection_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip);
Slic3r::Polylines  diff_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip);
ExPolygons         union_ex_2(const Polygons &expolygons);
ExPolygons         union_ex_2(const ExPolygons &expolygons);
ExPolygons         offset_ex_2(const ExPolygons &expolygons, double delta);
ExPolygons         offset2_ex_2(const ExPolygons &expolygons, double delta1, double delta2);

// Generic boolean on pre-converted int64 paths, returning ExPolygons. Used by
// ClipperUtils' _clipper_ex() to run the diff_ex/intersection_ex/union_ex family
// through Clipper2's sweepline. subject/clip are already in Clipper2 Paths64 (same
// int64 coords as the ClipperLib path types).
ExPolygons         clipper2_clip_ex(Clipper2Lib::ClipType clipType, Clipper2Lib::FillRule fillRule,
                                    const Clipper2Lib::Paths64 &subject, const Clipper2Lib::Paths64 &clip);

// Generic boolean on pre-converted int64 paths, returning Polygons (flat list,
// no contour/hole nesting). Used by ClipperUtils' _clipper()/union_ funnels to run
// the Polygons-returning boolean family through Clipper2. Mirrors clipper2_clip_ex
// but emits a flat Paths64 solution converted via to_polygons.
Polygons           clipper2_clip_polygons(Clipper2Lib::ClipType clipType, Clipper2Lib::FillRule fillRule,
                                          const Clipper2Lib::Paths64 &subject, const Clipper2Lib::Paths64 &clip);
}

#endif