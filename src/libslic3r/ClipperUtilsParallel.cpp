// diff_ex_by_piece() and intersection_ex_by_piece(): Clipper operations on a subject split into tiles, the tiles in parallel.
#include <cstddef>
#include <utility>
#include <vector>

#include <tbb/parallel_for.h>

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "libslic3r.h"

namespace Slic3r {

static Slic3r::ExPolygons clipper_ex_by_piece(ClipType clipType, const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
{
    // A few dozen subject ExPolygons to a tile, each tile one Clipper call with the clip cut to the tile's box.
    const std::vector<ClipperUtils::ExPolygonsTile> tiles = ClipperUtils::tile_expolygons(subject, 32);
    // One tile is the plain call: cutting the clip would only cost time.
    if (tiles.size() <= 1)
        return ClipperUtils::clip_ex(clipType, subject, clip, do_safety_offset);
    std::vector<BoundingBox> clip_bboxes;
    clip_bboxes.reserve(clip.size());
    for (const Polygon &polygon : clip)
        clip_bboxes.emplace_back(get_extents(polygon));

    std::vector<Slic3r::ExPolygons> out_tiles(tiles.size());
    tbb::parallel_for(size_t(0), tiles.size(), [&](size_t tile_idx) {
        const ClipperUtils::ExPolygonsTile &tile = tiles[tile_idx];
        Slic3r::ExPolygons local_subject;
        local_subject.reserve(tile.members.size());
        for (size_t i : tile.members)
            local_subject.emplace_back(subject[i]);
        // Grown so that the cut edges of the clip stay clear of the subject, also after the safety offset.
        const BoundingBox bbox = tile.bbox.inflated(SCALED_EPSILON);
        Polygons local_clip;
        for (size_t i = 0; i < clip.size(); ++i)
            if (clip_bboxes[i].overlap(bbox))
                if (Polygon clipped = ClipperUtils::clip_clipper_polygon_with_subject_bbox(clip[i], bbox); ! clipped.empty())
                    local_clip.emplace_back(std::move(clipped));
        out_tiles[tile_idx] = ClipperUtils::clip_ex(clipType, local_subject, local_clip, do_safety_offset);
    });
    Slic3r::ExPolygons out;
    for (Slic3r::ExPolygons &out_tile : out_tiles)
        append(out, std::move(out_tile));
    return out;
}
Slic3r::ExPolygons diff_ex_by_piece(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return clipper_ex_by_piece(ctDifference, subject, clip, do_safety_offset); }
Slic3r::ExPolygons intersection_ex_by_piece(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return clipper_ex_by_piece(ctIntersection, subject, clip, do_safety_offset); }

} // namespace Slic3r
