#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "ShortestPath.hpp"
#include "Clipper2Utils.hpp"

// #define CLIPPER_UTILS_DEBUG

#ifdef CLIPPER_UTILS_DEBUG
#include "SVG.hpp"
#endif /* CLIPPER_UTILS_DEBUG */

// Profiling support using the Shiny intrusive profiler
//#define CLIPPER_UTILS_PROFILE
#if defined(SLIC3R_PROFILE) && defined(CLIPPER_UTILS_PROFILE)
	#include <Shiny/Shiny.h>
	#define CLIPPERUTILS_PROFILE_FUNC() PROFILE_FUNC()
	#define CLIPPERUTILS_PROFILE_BLOCK(name) PROFILE_BLOCK(name)
#else
	#define CLIPPERUTILS_PROFILE_FUNC()
	#define CLIPPERUTILS_PROFILE_BLOCK(name)
#endif

namespace Slic3r {

#ifdef CLIPPER_UTILS_DEBUG
// For debugging the Clipper library, for providing bug reports to the Clipper author.
bool export_clipper_input_polygons_bin(const char *path, const ClipperLib::Paths &input_subject, const ClipperLib::Paths &input_clip)
{
    FILE *pfile = fopen(path, "wb");
    if (pfile == NULL)
        return false;

    uint32_t sz = uint32_t(input_subject.size());
    fwrite(&sz, 1, sizeof(sz), pfile);
    for (size_t i = 0; i < input_subject.size(); ++i) {
        const ClipperLib::Path &path = input_subject[i];
        sz = uint32_t(path.size());
        ::fwrite(&sz, 1, sizeof(sz), pfile);
        ::fwrite(path.data(), sizeof(ClipperLib::IntPoint), sz, pfile);
    }
    sz = uint32_t(input_clip.size());
    ::fwrite(&sz, 1, sizeof(sz), pfile);
    for (size_t i = 0; i < input_clip.size(); ++i) {
        const ClipperLib::Path &path = input_clip[i];
        sz = uint32_t(path.size());
        ::fwrite(&sz, 1, sizeof(sz), pfile);
        ::fwrite(path.data(), sizeof(ClipperLib::IntPoint), sz, pfile);
    }
    ::fclose(pfile);
    return true;

err:
    ::fclose(pfile);
    return false;
}
#endif /* CLIPPER_UTILS_DEBUG */

namespace ClipperUtils {
Points EmptyPathsProvider::s_empty_points;
Points SinglePathProvider::s_end;

// Clip source polygon to be used as a clipping polygon with a bouding box around the source (to be clipped) polygon.
// Useful as an optimization for expensive ClipperLib operations, for example when clipping source polygons one by one
// with a set of polygons covering the whole layer below.
template<typename PointsType> inline void clip_clipper_polygon_with_subject_bbox_templ(const PointsType &src, const BoundingBox &bbox, PointsType &out, const bool get_entire_polygons=false)
{
    using PointType = typename PointsType::value_type;

    out.clear();
    const size_t cnt = src.size();
    if (cnt < 3) return;

    enum class Side { Left = 1, Right = 2, Top = 4, Bottom = 8 };

    auto sides = [bbox](const PointType &p) {
        return int(p.x() < bbox.min.x()) * int(Side::Left) + int(p.x() > bbox.max.x()) * int(Side::Right) + int(p.y() < bbox.min.y()) * int(Side::Bottom) +
               int(p.y() > bbox.max.y()) * int(Side::Top);
    };

    int          sides_prev = sides(src.back());
    int          sides_this = sides(src.front());
    const size_t last       = cnt - 1;
    for (size_t i = 0; i < last; ++i) {
        int sides_next = sides(src[i + 1]);
        if ( // This point is inside. Take it.
            sides_this == 0 ||
            // Either this point is outside and previous or next is inside, or
            // the edge possibly cuts corner of the bounding box.
            (sides_prev & sides_this & sides_next) == 0) {
            out.emplace_back(src[i]);
            sides_prev = sides_this;
        } else {
            // All the three points (this, prev, next) are outside at the same side.
            // Ignore this point.
        }
        sides_this = sides_next;
    }

    // Never produce just a single point output polygon.
    if (!out.empty()) {
        if (get_entire_polygons) {
            out=src;
        } else {
            if (int sides_next = sides(out.front());
            // The last point is inside. Take it.
            sides_this == 0 ||
            // Either this point is outside and previous or next is inside, or
            // the edge possibly cuts corner of the bounding box.
            (sides_prev & sides_this & sides_next) == 0)
            out.emplace_back(src.back());
        }
    }
}

void clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox, Points &out, const bool get_entire_polygons) { clip_clipper_polygon_with_subject_bbox_templ(src, bbox, out, get_entire_polygons); }
void clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox, ZPoints &out) { clip_clipper_polygon_with_subject_bbox_templ(src, bbox, out); }

template<typename PointsType> [[nodiscard]] PointsType clip_clipper_polygon_with_subject_bbox_templ(const PointsType &src, const BoundingBox &bbox)
{
    PointsType out;
    clip_clipper_polygon_with_subject_bbox(src, bbox, out);
    return out;
}

[[nodiscard]] Points  clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox) { return clip_clipper_polygon_with_subject_bbox_templ(src, bbox); }
[[nodiscard]] ZPoints clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox) { return clip_clipper_polygon_with_subject_bbox_templ(src, bbox); }

void clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, Polygon &out) { 
    clip_clipper_polygon_with_subject_bbox(src.points, bbox, out.points);
}

[[nodiscard]] Polygon clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygon out;
    clip_clipper_polygon_with_subject_bbox(src.points, bbox, out.points, get_entire_polygons);
    return out;
}

[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const Polygons &src, const BoundingBox &bbox)
{
    Polygons out;
    out.reserve(src.size());
    for (const Polygon &p : src) out.emplace_back(clip_clipper_polygon_with_subject_bbox(p, bbox));
    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) { return polygon.empty(); }), out.end());
    return out;
}
[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygon &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygons out;
    out.reserve(src.num_contours());
    out.emplace_back(clip_clipper_polygon_with_subject_bbox(src.contour, bbox, get_entire_polygons));
    for (const Polygon &p : src.holes) out.emplace_back(clip_clipper_polygon_with_subject_bbox(p, bbox, get_entire_polygons));
    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) { return polygon.empty(); }), out.end());
    return out;
}
[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygons &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygons out;
    out.reserve(number_polygons(src));
    for (const ExPolygon &p : src) {
        Polygons temp = clip_clipper_polygons_with_subject_bbox(p, bbox, get_entire_polygons);
        out.insert(out.end(), temp.begin(), temp.end());
    }

    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) {return polygon.empty(); }), out.end());
    return out;
}
}

static ExPolygons PolyTreeToExPolygons(ClipperLib::PolyTree &&polytree)
{
    struct Inner {
        static void PolyTreeToExPolygonsRecursive(ClipperLib::PolyNode &&polynode, ExPolygons *expolygons)
        {  
            size_t cnt = expolygons->size();
            expolygons->resize(cnt + 1);
            (*expolygons)[cnt].contour.points = std::move(polynode.Contour);
            (*expolygons)[cnt].holes.resize(polynode.ChildCount());
            for (int i = 0; i < polynode.ChildCount(); ++ i) {
                (*expolygons)[cnt].holes[i].points = std::move(polynode.Childs[i]->Contour);
                // Add outer polygons contained by (nested within) holes.
                for (int j = 0; j < polynode.Childs[i]->ChildCount(); ++ j)
                    PolyTreeToExPolygonsRecursive(std::move(*polynode.Childs[i]->Childs[j]), expolygons);
            }
        }

        static size_t PolyTreeCountExPolygons(const ClipperLib::PolyNode &polynode)
        {
            size_t cnt = 1;
            for (int i = 0; i < polynode.ChildCount(); ++ i) {
                for (int j = 0; j < polynode.Childs[i]->ChildCount(); ++ j)
                cnt += PolyTreeCountExPolygons(*polynode.Childs[i]->Childs[j]);
            }
            return cnt;
        }
    };

    ExPolygons retval;
    size_t cnt = 0;
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        cnt += Inner::PolyTreeCountExPolygons(*polytree.Childs[i]);
    retval.reserve(cnt);
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        Inner::PolyTreeToExPolygonsRecursive(std::move(*polytree.Childs[i]), &retval);
    return retval;
}

Polylines PolyTreeToPolylines(ClipperLib::PolyTree &&polytree)
{
    struct Inner {
        static void AddPolyNodeToPaths(ClipperLib::PolyNode &polynode, Polylines &out)
        {
            if (! polynode.Contour.empty())
                out.emplace_back(std::move(polynode.Contour));
            for (ClipperLib::PolyNode *child : polynode.Childs)
                AddPolyNodeToPaths(*child, out);
        }
    };

    Polylines out;
    out.reserve(polytree.Total());
    Inner::AddPolyNodeToPaths(polytree, out);
    return out;
}

#if 0
// Global test.
bool has_duplicate_points(const ClipperLib::PolyTree &polytree)
{
    struct Helper {
        static void collect_points_recursive(const ClipperLib::PolyNode &polynode, ClipperLib::Path &out) {
            // For each hole of the current expolygon:
            out.insert(out.end(), polynode.Contour.begin(), polynode.Contour.end());
            for (int i = 0; i < polynode.ChildCount(); ++ i)
                collect_points_recursive(*polynode.Childs[i], out);
        }
    };
    ClipperLib::Path pts;
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        Helper::collect_points_recursive(*polytree.Childs[i], pts);
    return has_duplicate_points(std::move(pts));
}
#else
// Local test inside each of the contours.
bool has_duplicate_points(const ClipperLib::PolyTree &polytree)
{
    struct Helper {
        static bool has_duplicate_points_recursive(const ClipperLib::PolyNode &polynode) {
            if (has_duplicate_points(polynode.Contour))
                return true;
            for (int i = 0; i < polynode.ChildCount(); ++ i)
                if (has_duplicate_points_recursive(*polynode.Childs[i]))
                    return true;
            return false;
        }
    };
    ClipperLib::Path pts;
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        if (Helper::has_duplicate_points_recursive(*polytree.Childs[i]))
            return true;
    return false;
}
#endif

// ---- Clipper2 boolean / offset engine ------------------------------------------
// Coords are identical int64, so the provider->Paths64 conversion is a straight copy.
// The boolean family (_clipper/_clipper_ex/union_) and the raw_offset funnel run on
// Clipper2. Safety-offset cases still go through the ClipperLib path below (the safety
// offset is itself a ClipperLib op), as do the offset helpers that drive ClipperLib's
// ClipperOffset / Clipper directly (shrink_paths, offset_expolygon_inner, etc.).
template<typename PathsProvider>
static Clipper2Lib::Paths64 provider_to_paths64(PathsProvider &&provider)
{
    Clipper2Lib::Paths64 out;
    for (const Slic3r::Points &path : provider) {
        Clipper2Lib::Path64 p;
        p.reserve(path.size());
        for (const Slic3r::Point &pt : path)
            p.emplace_back(pt.x(), pt.y());
        out.emplace_back(std::move(p));
    }
    return out;
}

static inline Clipper2Lib::FillRule to_clipper2_fillrule(ClipperLib::PolyFillType ft)
{
    switch (ft) {
    case ClipperLib::pftEvenOdd:  return Clipper2Lib::FillRule::EvenOdd;
    case ClipperLib::pftPositive: return Clipper2Lib::FillRule::Positive;
    case ClipperLib::pftNegative: return Clipper2Lib::FillRule::Negative;
    default:                      return Clipper2Lib::FillRule::NonZero;
    }
}

static inline Clipper2Lib::ClipType to_clipper2_cliptype(ClipperLib::ClipType ct)
{
    switch (ct) {
    case ClipperLib::ctIntersection: return Clipper2Lib::ClipType::Intersection;
    case ClipperLib::ctUnion:        return Clipper2Lib::ClipType::Union;
    case ClipperLib::ctDifference:   return Clipper2Lib::ClipType::Difference;
    case ClipperLib::ctXor:          return Clipper2Lib::ClipType::Xor;
    default:                         return Clipper2Lib::ClipType::Union;
    }
}

static inline Clipper2Lib::JoinType to_clipper2_jointype(ClipperLib::JoinType jt)
{
    switch (jt) {
    case ClipperLib::jtSquare: return Clipper2Lib::JoinType::Square;
    case ClipperLib::jtMiter:  return Clipper2Lib::JoinType::Miter;
    case ClipperLib::jtRound:  return Clipper2Lib::JoinType::Round;
    default:                   return Clipper2Lib::JoinType::Round;
    }
}

static inline Clipper2Lib::EndType to_clipper2_endtype(ClipperLib::EndType et)
{
    switch (et) {
    case ClipperLib::etClosedPolygon: return Clipper2Lib::EndType::Polygon;
    case ClipperLib::etClosedLine:    return Clipper2Lib::EndType::Joined;
    case ClipperLib::etOpenButt:      return Clipper2Lib::EndType::Butt;
    case ClipperLib::etOpenSquare:    return Clipper2Lib::EndType::Square;
    case ClipperLib::etOpenRound:     return Clipper2Lib::EndType::Round;
    default:                          return Clipper2Lib::EndType::Polygon;
    }
}

// Convert a single Clipper2 Path64 to a ClipperLib::Path (identical int64 coords).
static inline ClipperLib::Path path64_to_clipper1(const Clipper2Lib::Path64 &in)
{
    ClipperLib::Path out;
    out.reserve(in.size());
    for (const Clipper2Lib::Point64 &p : in)
        out.emplace_back(ClipperLib::cInt(p.x), ClipperLib::cInt(p.y));
    return out;
}

// Clipper2 mirror of raw_offset (below). Reproduces Clipper1's exact per-path
// CCW-orientation semantics. Clipper1 reorients each contour so the outermost has
// positive area (CCW), applies the offset to that CCW form (hence the sign is
// reversed for CW input), and reverses the result back for CW inputs -- so CCW
// contours grow/shrink with the offset sign while CW (hole) contours offset in the
// opposite sense, all output staying CCW for original-CCW and CW for original-CW.
//
// Clipper2's ClipperOffset does its own orientation handling (is_reversed =
// Area < 0), which would double-handle and invert holes. To get an exact mirror we
// neutralize it: feed Clipper2 a forced-CCW copy of the path (so is_reversed is
// always false / a no-op), apply the same signed offset Clipper1 uses
// (ccw ? offset : -offset), then reverse the result for CW inputs. Verified to
// match Clipper1 area + orientation on CCW/CW squares for +/- offset.
//
// There is no ShortestEdgeLength analog in Clipper2 (its offsetter merges short
// edges internally); ArcTolerance/MiterLimit map directly. Returns ClipperLib::Paths
// so the (many) raw_offset callers are unaffected. Open end types (etOpen*/
// etClosedLine) are never reoriented (matches Clipper1's ccw=true for non-Polygon).
template<typename PathsProvider>
static ClipperLib::Paths raw_offset_clipper2(PathsProvider &&paths, float offset, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType endType)
{
    const Clipper2Lib::JoinType jt = to_clipper2_jointype(joinType);
    const Clipper2Lib::EndType  et = to_clipper2_endtype(endType);
    const bool is_closed_polygon = (endType == ClipperLib::etClosedPolygon);

    ClipperLib::Paths out;
    out.reserve(paths.size());
    for (const auto &path : paths) {
        Clipper2Lib::Path64 p;
        p.reserve(path.size());
        for (const auto &pt : path)
            p.emplace_back(pt.x(), pt.y());

        // Mirror Clipper1: ccw orientation only considered for closed polygons.
        const bool ccw = is_closed_polygon ? Clipper2Lib::IsPositive(p) : true;
        if (!ccw)
            // Force CCW input so Clipper2's internal orientation handling is a no-op.
            std::reverse(p.begin(), p.end());

        Clipper2Lib::ClipperOffset co;
        if (joinType == jtRound)
            co.ArcTolerance(miterLimit);
        else
            co.MiterLimit(miterLimit);
        co.AddPath(p, jt, et);
        Clipper2Lib::Paths64 sol;
        co.Execute(ccw ? double(offset) : -double(offset), sol);
        // Clipper2's ClipperOffset has no ShortestEdgeLength, so the round-join arc
        // approximation leaves many micro-edges that Clipper1 merged away. Restore
        // that by simplifying the offset result (epsilon = |offset| * the same
        // ShortestEdgeFactor) -- thins vertex density back toward Clipper1's, which
        // cuts the downstream per-point work (perimeters/fill/gcode) that profiling
        // showed dominates the Clipper2 regression. Offset results are closed polygons.
        if (const double eps = std::abs(double(offset)) * ClipperOffsetShortestEdgeFactor; eps > 0.0)
            sol = Clipper2Lib::SimplifyPaths(sol, eps);
        for (Clipper2Lib::Path64 &c2 : sol) {
            if (!ccw)
                std::reverse(c2.begin(), c2.end());
            out.emplace_back(path64_to_clipper1(c2));
        }
    }
    return out;
}

// ---- Clipper2 boolean helpers operating on ClipperLib::Paths -------------------
// These mirror the old Clipper1 boolean cores but run on Clipper2Lib::Clipper64.
// Coordinates are identical int64, so the conversion is a straight copy. They return
// ClipperLib::Paths so the (many) callers that consume Paths are unaffected.

// Convert any path container (ClipperLib::Paths, or a PathsProvider yielding Slic3r::Points)
// into Clipper2Lib::Paths64.
template<typename PathContainer>
static inline Clipper2Lib::Paths64 to_paths64(const PathContainer &in)
{
    Clipper2Lib::Paths64 out;
    out.reserve(in.size());
    for (const auto &path : in) {
        Clipper2Lib::Path64 p;
        p.reserve(path.size());
        for (const auto &pt : path)
            p.emplace_back(pt.x(), pt.y());
        out.emplace_back(std::move(p));
    }
    return out;
}

static inline ClipperLib::Paths paths64_to_clipper1(const Clipper2Lib::Paths64 &in)
{
    ClipperLib::Paths out;
    out.reserve(in.size());
    for (const Clipper2Lib::Path64 &p : in)
        out.emplace_back(path64_to_clipper1(p));
    return out;
}

template<typename PointContainer>
static inline Clipper2Lib::Path64 path_to_path64(const PointContainer &in)
{
    Clipper2Lib::Path64 out;
    out.reserve(in.size());
    for (const auto &pt : in)
        out.emplace_back(pt.x(), pt.y());
    return out;
}

// Boolean on Clipper2, both inputs treated as closed subject/clip, Paths output.
template<typename TSubj, typename TClip>
static ClipperLib::Paths clipper2_do_paths(
    const ClipperLib::ClipType     clipType,
    const TSubj                   &subject,
    const TClip                   &clip,
    const ClipperLib::PolyFillType fillType)
{
    Clipper2Lib::Clipper64 c;
    Clipper2Lib::Paths64 subj = to_paths64(subject);
    if (!subj.empty())
        c.AddSubject(subj);
    Clipper2Lib::Paths64 cl = to_paths64(clip);
    if (!cl.empty())
        c.AddClip(cl);
    Clipper2Lib::Paths64 sol;
    c.Execute(to_clipper2_cliptype(clipType), to_clipper2_fillrule(fillType), sol);
    return paths64_to_clipper1(sol);
}

// Union on Clipper2, single (already-prepared) subject, Paths output.
template<typename TSubj>
static ClipperLib::Paths clipper2_union_paths(
    const TSubj                   &subject,
    const ClipperLib::PolyFillType fillType = ClipperLib::pftNonZero)
{
    Clipper2Lib::Clipper64 c;
    Clipper2Lib::Paths64 subj = to_paths64(subject);
    if (!subj.empty())
        c.AddSubject(subj);
    Clipper2Lib::Paths64 sol;
    c.Execute(Clipper2Lib::ClipType::Union, to_clipper2_fillrule(fillType), sol);
    return paths64_to_clipper1(sol);
}

// Apply the safety offset (10um outward, one path at a time) on Clipper2. The safety
// offset is a small ClipperOffset on the clip before an intersection/difference.
static inline ClipperLib::Paths clipper2_safety_offset(const ClipperLib::Paths &clip)
{
    return raw_offset_clipper2(clip, ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, ClipperLib::etClosedPolygon);
}

// Union a set of ClipperLib::Paths on Clipper2 into nested ExPolygons.
static inline ExPolygons clipper2_union_ex(const ClipperLib::Paths &paths, ClipperLib::PolyFillType fillType = ClipperLib::pftNonZero)
{
    return clipper2_clip_ex(Clipper2Lib::ClipType::Union, to_clipper2_fillrule(fillType), to_paths64(paths), Clipper2Lib::Paths64{});
}

// Offset CCW contours outside, CW contours (holes) inside.
// Don't calculate union of the output paths.
template<typename PathsProvider>
static ClipperLib::Paths raw_offset(PathsProvider &&paths, float offset, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType endType = ClipperLib::etClosedPolygon)
{
    return raw_offset_clipper2(std::forward<PathsProvider>(paths), offset, joinType, miterLimit, endType);
}

// Offset outside by 10um, one by one. Used as a clip-side safety offset before a
// boolean. Runs on Clipper2 via raw_offset (raw_offset_clipper2). Materializes the
// provider into ClipperLib::Paths so the result is reusable by the Clipper2 boolean.
template<typename PathsProvider>
static ClipperLib::Paths safety_offset(PathsProvider &&paths)
{
    return raw_offset(std::forward<PathsProvider>(paths), ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit);
}

// Boolean on Clipper2 producing nested ExPolygons. Clipper2 handles overlapping
// edges efficiently, so the Clipper1 two-pass (Paths then re-Union to PolyTree)
// workaround is unnecessary -- a single Execute to a PolyTree64 suffices. When the
// safety offset is requested it is applied (on Clipper2) to the clip beforehand.
template<typename TSubj, typename TClip>
static ExPolygons clipper2_do_ex(
    const ClipperLib::ClipType     clipType,
    TSubj                        &&subject,
    TClip                        &&clip,
    const ClipperLib::PolyFillType fillType,
    const ApplySafetyOffset        do_safety_offset = ApplySafetyOffset::No)
{
    assert(do_safety_offset == ApplySafetyOffset::No || clipType != ClipperLib::ctUnion);
    Clipper2Lib::Paths64 subj = provider_to_paths64(std::forward<TSubj>(subject));
    Clipper2Lib::Paths64 cl   = do_safety_offset == ApplySafetyOffset::Yes ?
        to_paths64(safety_offset(std::forward<TClip>(clip))) :
        provider_to_paths64(std::forward<TClip>(clip));
    return clipper2_clip_ex(to_clipper2_cliptype(clipType), to_clipper2_fillrule(fillType), subj, cl);
}

// Perform union of input polygons using the positive rule, convert to ExPolygons.
//FIXME is there any benefit of not doing the boolean / using pftEvenOdd?
inline ExPolygons ClipperPaths_to_Slic3rExPolygons(const ClipperLib::Paths &input, bool do_union)
{
    return clipper2_union_ex(input, do_union ? ClipperLib::pftNonZero : ClipperLib::pftEvenOdd);
}

template<typename PathsProvider>
static ClipperLib::Paths raw_offset_polyline(PathsProvider &&paths, float offset, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType end_type = ClipperLib::etOpenButt)
{
    assert(offset > 0);
    return raw_offset<PathsProvider>(std::forward<PathsProvider>(paths), offset, joinType, miterLimit, end_type);
}

// Expand each contour outward then union the (possibly overlapping) offset paths.
// raw_offset (Clipper2) already produces correctly oriented contours/holes; the
// union below collapses overlaps. Returns ClipperLib::Paths.
template<typename PathsProvider>
static ClipperLib::Paths expand_paths(PathsProvider &&paths, float offset, ClipperLib::JoinType joinType, double miterLimit)
{
    // BBS
    //assert(offset > 0);
    return clipper2_union_paths(raw_offset(std::forward<PathsProvider>(paths), offset, joinType, miterLimit));
}

// Inner (negative) offset. raw_offset already offsets contours/holes in the proper
// inward sense and keeps Clipper1's per-path orientation (CCW contours, CW holes);
// a Clipper2 NonZero union then cleans up the result. This replaces the Clipper1
// "reversed bounding box + Negative fill + remove outermost" idiom, which existed
// only to coax Clipper1 into the same union for already-correctly-oriented paths.
template<typename PathsProvider>
static ClipperLib::Paths shrink_paths(PathsProvider &&paths, float offset, ClipperLib::JoinType joinType, double miterLimit)
{
    // BBS
    //assert(offset > 0);
    ClipperLib::Paths raw = raw_offset(std::forward<PathsProvider>(paths), - offset, joinType, miterLimit);
    if (raw.empty())
        return {};
    return clipper2_union_paths(raw, ClipperLib::pftNonZero);
}

template<typename PathsProvider>
static ClipperLib::Paths offset_paths(PathsProvider &&paths, float offset, ClipperLib::JoinType joinType, double miterLimit)
{
    // BBS
    //assert(offset != 0);

    return offset > 0 ?
        expand_paths(std::forward<PathsProvider>(paths),   offset, joinType, miterLimit) :
        shrink_paths(std::forward<PathsProvider>(paths), - offset, joinType, miterLimit);
}

template<typename PathsProvider>
static ExPolygons offset_paths_ex(PathsProvider &&paths, float offset, ClipperLib::JoinType joinType, double miterLimit)
{
    return clipper2_union_ex(offset_paths(std::forward<PathsProvider>(paths), offset, joinType, miterLimit));
}

Slic3r::Polygons offset(const Slic3r::Polygon &polygon, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return to_polygons(raw_offset(ClipperUtils::SinglePathProvider(polygon.points), delta, joinType, miterLimit)); }

Slic3r::Polygons offset(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return to_polygons(offset_paths(ClipperUtils::PolygonsProvider(polygons), delta, joinType, miterLimit)); }
Slic3r::ExPolygons offset_ex(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return offset_paths_ex(ClipperUtils::PolygonsProvider(polygons), delta, joinType, miterLimit); }

Slic3r::Polygons offset(const Slic3r::Polyline &polyline, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType end_type)
    { assert(delta > 0); return to_polygons(clipper2_union_paths(raw_offset_polyline(ClipperUtils::SinglePathProvider(polyline.points), delta, joinType, miterLimit, end_type))); }

Slic3r::Polygons offset(const Slic3r::Polyline3 &polyline, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType end_type)
{
    assert(delta > 0);
    return to_polygons(
        clipper2_union_paths(
            raw_offset_polyline(
                ClipperUtils::SinglePathProvider(polyline.to_polyline().points),
                delta,
                joinType,
                miterLimit,
                end_type)));
}
Slic3r::Polygons offset(const Slic3r::Polylines &polylines, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType end_type)
    { assert(delta > 0); return to_polygons(clipper2_union_paths(raw_offset_polyline(ClipperUtils::PolylinesProvider(polylines), delta, joinType, miterLimit, end_type))); }

Polygons contour_to_polygons(const Polygon &polygon, const float line_width, ClipperLib::JoinType join_type, double miter_limit){
    assert(line_width > 1.f); return to_polygons(clipper2_union_paths(
        raw_offset(ClipperUtils::SinglePathProvider(polygon.points), line_width/2, join_type, miter_limit, ClipperLib::etClosedLine)));}
Polygons contour_to_polygons(const Polygons &polygons, const float line_width, ClipperLib::JoinType join_type, double miter_limit){
    assert(line_width > 1.f); return to_polygons(clipper2_union_paths(
        raw_offset(ClipperUtils::PolygonsProvider(polygons), line_width/2, join_type, miter_limit, ClipperLib::etClosedLine)));}

// returns number of expolygons collected (0 or 1).
static int offset_expolygon_inner(const Slic3r::ExPolygon &expoly, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::Paths &out)
{
    // 1) Offset the outer contour.
    // A single-path Clipper2 ClipperOffset with the engine's NATIVE orientation
    // handling (is_reversed = Area<0) reproduces Clipper1's auto-reorientation that
    // this routine relies on: Clipper1 reorients the path so the outermost contour
    // has positive area, applies the offset to that CCW form, and so the offset sign
    // is reversed for CW (hole) input. raw_offset_clipper2 deliberately neutralizes
    // that reorientation, so it must NOT be used here. There is no ShortestEdgeLength
    // analog in Clipper2 (it merges short edges internally).
    auto offset_path_native = [&](const Points &path, double off) -> ClipperLib::Paths {
        Clipper2Lib::Path64 p;
        p.reserve(path.size());
        for (const Slic3r::Point &pt : path)
            p.emplace_back(pt.x(), pt.y());
        Clipper2Lib::ClipperOffset co;
        if (joinType == jtRound)
            co.ArcTolerance(miterLimit);
        else
            co.MiterLimit(miterLimit);
        co.AddPath(p, to_clipper2_jointype(joinType), Clipper2Lib::EndType::Polygon);
        Clipper2Lib::Paths64 sol;
        co.Execute(off, sol);
        // Clipper1's ClipperOffset reorients so the outermost contour has positive area;
        // the output is therefore CCW (positive area) for both contour and hole inputs.
        // Clipper2 instead preserves the input orientation (it offsets CW holes in place),
        // so force each output contour CCW here to reproduce Clipper1's invariant. The
        // downstream NonZero difference depends on this orientation.
        ClipperLib::Paths out_paths;
        out_paths.reserve(sol.size());
        for (Clipper2Lib::Path64 &c2 : sol) {
            if (!Clipper2Lib::IsPositive(c2))
                std::reverse(c2.begin(), c2.end());
            out_paths.emplace_back(path64_to_clipper1(c2));
        }
        return out_paths;
    };

    ClipperLib::Paths contours = offset_path_native(expoly.contour.points, delta);
    if (contours.empty())
        // No need to try to offset the holes.
        return 0;

    if (expoly.holes.empty()) {
        // No need to subtract holes from the offsetted expolygon, we are done.
        append(out, std::move(contours));
    } else {
        // 2) Offset the holes one by one, collect the offsetted holes.
        ClipperLib::Paths holes;
        {
            for (const Polygon &hole : expoly.holes) {
                // Execute reorients the contours so that the outer most contour has a positive area. Thus the output
                // contours will be CCW oriented even though the input paths are CW oriented.
                // Offset is applied after contour reorientation, thus the signum of the offset value is reversed.
                ClipperLib::Paths out2 = offset_path_native(hole.points, - delta);
                append(holes, std::move(out2));
            }
        }

        // 3) Subtract holes from the contours.
        if (holes.empty()) {
            // No hole remaining after an offset. Just copy the outer contour.
            append(out, std::move(contours));
        } else if (delta < 0) {
            // Negative offset. There is a chance, that the offsetted hole intersects the outer contour.
            // Subtract the offsetted holes from the offsetted contours.
            if (auto output = clipper2_do_paths(ClipperLib::ctDifference, contours, holes, ClipperLib::pftNonZero); ! output.empty()) {
                append(out, std::move(output));
            } else {
                // The offsetted holes have eaten up the offsetted outer contour.
                return 0;
            }
        } else {
            // Positive offset. As long as the Clipper offset does what one expects it to do, the offsetted hole will have a smaller
            // area than the original hole or even disappear, therefore there will be no new intersections.
            // Just collect the reversed holes.
            out.reserve(contours.size() + holes.size());
            append(out, std::move(contours));
            // Reverse the holes in place.
            for (size_t i = 0; i < holes.size(); ++ i)
                std::reverse(holes[i].begin(), holes[i].end());
            append(out, std::move(holes));
        }
    }

    return 1;
}

static int offset_expolygon_inner(const Slic3r::Surface &surface, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::Paths &out)
    { return offset_expolygon_inner(surface.expolygon, delta, joinType, miterLimit, out); }
static int offset_expolygon_inner(const Slic3r::Surface *surface, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::Paths &out)
    { return offset_expolygon_inner(surface->expolygon, delta, joinType, miterLimit, out); }

ClipperLib::Paths expolygon_offset(const Slic3r::ExPolygon &expolygon, const float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    ClipperLib::Paths out;
    offset_expolygon_inner(expolygon, delta, joinType, miterLimit, out);
    return out;
}

// This is a safe variant of the polygons offset, tailored for multiple ExPolygons.
// It is required, that the input expolygons do not overlap and that the holes of each ExPolygon don't intersect with their respective outer contours.
// Each ExPolygon is offsetted separately. For outer offset, the the offsetted ExPolygons shall be united outside of this function.
template<typename ExPolygonVector>
static std::pair<ClipperLib::Paths, size_t> expolygons_offset_raw(const ExPolygonVector &expolygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    // Offsetted ExPolygons before they are united.
    ClipperLib::Paths output;
    output.reserve(expolygons.size());
    // How many non-empty offsetted expolygons were actually collected into output?
    // If only one, then there is no need to do a final union.
    size_t expolygons_collected = 0;
    for (const auto &expoly : expolygons)
        expolygons_collected += offset_expolygon_inner(expoly, delta, joinType, miterLimit, output);
    return std::make_pair(std::move(output), expolygons_collected);
}

// See comment on expolygon_offsets_raw. In addition, for positive offset the contours are united.
template<typename ExPolygonVector>
static ClipperLib::Paths expolygons_offset(const ExPolygonVector &expolygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    auto [output, expolygons_collected] = expolygons_offset_raw(expolygons, delta, joinType, miterLimit);
    // Unite the offsetted expolygons.
    return expolygons_collected > 1 && delta > 0 ?
        // There is a chance that the outwards offsetted expolygons may intersect. Perform a union.
        clipper2_union_paths(output) :
        // Negative offset. The shrunk expolygons shall not mutually intersect. Just copy the output.
        output;
}

// See comment on expolygons_offset_raw. In addition, the polygons are always united into ExPolygons.
template<typename ExPolygonVector>
static ExPolygons expolygons_offset_ex(const ExPolygonVector &expolygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    auto [output, expolygons_collected] = expolygons_offset_raw(expolygons, delta, joinType, miterLimit);
    // Unite the offsetted expolygons.
    return clipper2_union_ex(output);
}

Slic3r::Polygons offset(const Slic3r::ExPolygon &expolygon, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return to_polygons(expolygon_offset(expolygon, delta, joinType, miterLimit)); }
Slic3r::Polygons offset(const Slic3r::ExPolygons &expolygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return to_polygons(expolygons_offset(expolygons, delta, joinType, miterLimit)); }
Slic3r::Polygons offset(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return to_polygons(expolygons_offset(surfaces, delta, joinType, miterLimit)); }
Slic3r::Polygons offset(const Slic3r::SurfacesPtr &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return to_polygons(expolygons_offset(surfaces, delta, joinType, miterLimit)); }
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygon &expolygon, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    //FIXME one may spare one Clipper Union call.
    { return ClipperPaths_to_Slic3rExPolygons(expolygon_offset(expolygon, delta, joinType, miterLimit)); }
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygons &expolygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return expolygons_offset_ex(expolygons, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return expolygons_offset_ex(surfaces, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::SurfacesPtr &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return expolygons_offset_ex(surfaces, delta, joinType, miterLimit); }

Polygons offset2(const ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    return to_polygons(offset_paths(expolygons_offset(expolygons, delta1, joinType, miterLimit), delta2, joinType, miterLimit));
}
ExPolygons offset2_ex(const ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    return offset_paths_ex(expolygons_offset(expolygons, delta1, joinType, miterLimit), delta2, joinType, miterLimit);
}
ExPolygons offset2_ex(const Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    //FIXME it may be more efficient to offset to_expolygons(surfaces) instead of to_polygons(surfaces).
    return offset_paths_ex(expolygons_offset(surfaces, delta1, joinType, miterLimit), delta2, joinType, miterLimit);
}

// Offset outside, then inside produces morphological closing. All deltas should be positive.
Slic3r::Polygons closing(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    return to_polygons(shrink_paths(expand_paths(ClipperUtils::PolygonsProvider(polygons), delta1, joinType, miterLimit), delta2, joinType, miterLimit));
}
Slic3r::ExPolygons closing_ex(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    return clipper2_union_ex(shrink_paths(expand_paths(ClipperUtils::PolygonsProvider(polygons), delta1, joinType, miterLimit), delta2, joinType, miterLimit));
}
Slic3r::ExPolygons closing_ex(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    //FIXME it may be more efficient to offset to_expolygons(surfaces) instead of to_polygons(surfaces).
    return clipper2_union_ex(shrink_paths(expand_paths(ClipperUtils::SurfacesProvider(surfaces), delta1, joinType, miterLimit), delta2, joinType, miterLimit));
}

// Offset inside, then outside produces morphological opening. All deltas should be positive.
Slic3r::Polygons opening(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    return to_polygons(expand_paths(shrink_paths(ClipperUtils::PolygonsProvider(polygons), delta1, joinType, miterLimit), delta2, joinType, miterLimit));
}
Slic3r::Polygons opening(const Slic3r::ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    return to_polygons(expand_paths(shrink_paths(ClipperUtils::ExPolygonsProvider(expolygons), delta1, joinType, miterLimit), delta2, joinType, miterLimit));
}
Slic3r::Polygons opening(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    //FIXME it may be more efficient to offset to_expolygons(surfaces) instead of to_polygons(surfaces).
    return to_polygons(expand_paths(shrink_paths(ClipperUtils::SurfacesProvider(surfaces), delta1, joinType, miterLimit), delta2, joinType, miterLimit));
}

template<class TSubj, class TClip>
static inline Polygons _clipper(ClipperLib::ClipType clipType, TSubj &&subject, TClip &&clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return clipper2_clip_polygons(to_clipper2_cliptype(clipType), Clipper2Lib::FillRule::NonZero,
                                      provider_to_paths64(std::forward<TSubj>(subject)),
                                      provider_to_paths64(std::forward<TClip>(clip)));
    // Safety-offset path: apply the safety offset (Clipper2) to the clip, then the Clipper2 boolean.
    return clipper2_clip_polygons(to_clipper2_cliptype(clipType), Clipper2Lib::FillRule::NonZero,
                                  provider_to_paths64(std::forward<TSubj>(subject)),
                                  to_paths64(safety_offset(std::forward<TClip>(clip))));
}

Slic3r::Polygons diff(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff_clipped(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset) 
    { return diff(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return diff_ex(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons & subject, const Slic3r::ExPolygons & clip, ApplySafetyOffset do_safety_offset)
{
    return diff_ex(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset);
}
Slic3r::Polygons diff(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::Polygons intersection_clipped(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset) 
    { return intersection(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygon &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// BBS
Slic3r::Polygons intersection(const Slic3r::Polygons& subject, const Slic3r::Polygon& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons clip_temp;
    clip_temp.push_back(clip);
    return intersection(subject, clip_temp, do_safety_offset);
}

Slic3r::Polygons union_(const Slic3r::Polygons &subject)
    { return _clipper(ClipperLib::ctUnion, ClipperUtils::PolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::Polygons union_(const Slic3r::ExPolygons &subject)
    { return _clipper(ClipperLib::ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::Polygons union_(const Slic3r::Polygons &subject, const ClipperLib::PolyFillType fillType)
    {
        return clipper2_clip_polygons(Clipper2Lib::ClipType::Union, to_clipper2_fillrule(fillType),
                                      provider_to_paths64(ClipperUtils::PolygonsProvider(subject)),
                                      Clipper2Lib::Paths64{});
    }
Slic3r::Polygons union_(const Slic3r::Polygons &subject, const Slic3r::Polygons &subject2)
    {
        // BBS
        Polygons polys = subject;
        for (const Polygon& poly : subject2)
            polys.push_back(poly);
        return union_(polys);
    }

template <typename TSubject, typename TClip>
static ExPolygons _clipper_ex(ClipperLib::ClipType clipType, TSubject &&subject,  TClip &&clip, ApplySafetyOffset do_safety_offset, ClipperLib::PolyFillType fill_type = ClipperLib::pftNonZero)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return clipper2_clip_ex(to_clipper2_cliptype(clipType), to_clipper2_fillrule(fill_type),
                                provider_to_paths64(std::forward<TSubject>(subject)),
                                provider_to_paths64(std::forward<TClip>(clip)));
    // Safety-offset path: apply the safety offset (Clipper2) to the clip, then the Clipper2 boolean.
    return clipper2_do_ex(clipType, std::forward<TSubject>(subject), std::forward<TClip>(clip), fill_type, do_safety_offset);
}

Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygon &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// BBS
inline Slic3r::ExPolygons diff_ex(const Slic3r::Polygon& subject, const Slic3r::Polygons& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons subject_temp;
    subject_temp.push_back(subject);

    return diff_ex(subject_temp, clip, do_safety_offset);
}

inline Slic3r::ExPolygons diff_ex(const Slic3r::Polygon& subject, const Slic3r::Polygon& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons subject_temp;
    Slic3r::Polygons clip_temp;

    subject_temp.push_back(subject);
    clip_temp.push_back(clip);
    return diff_ex(subject_temp, clip_temp, do_safety_offset);
}

Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset);}
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygons& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset);}
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// May be used to "heal" unusual models (3DLabPrints etc.) by providing fill_type (pftEvenOdd, pftNonZero, pftPositive, pftNegative).
Slic3r::ExPolygons union_ex(const Slic3r::Polygons &subject, ClipperLib::PolyFillType fill_type)
    { return _clipper_ex(ClipperLib::ctUnion, ClipperUtils::PolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No, fill_type); }
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject)
    { return clipper2_do_ex(ClipperLib::ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ClipperLib::pftNonZero); }
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &subject2)
    { return clipper2_do_ex(ClipperLib::ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(subject2), ClipperLib::pftNonZero); }
Slic3r::ExPolygons union_ex(const Slic3r::Surfaces &subject)
    { return clipper2_do_ex(ClipperLib::ctUnion, ClipperUtils::SurfacesProvider(subject), ClipperUtils::EmptyPathsProvider(), ClipperLib::pftNonZero); }
// BBS
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons& poly1, const Slic3r::ExPolygons& poly2, bool safety_offset_)
    {
    ExPolygons expolys = poly1;
    for (const ExPolygon& expoly : poly2)
        expolys.push_back(expoly);
    return union_ex(expolys);
}

Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset) {
    return _clipper_ex(ClipperLib::ctXor, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset);
}
Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset) {
    return _clipper_ex(ClipperLib::ctXor, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset);
}

template<typename PathsProvider1, typename PathsProvider2>
Polylines _clipper_pl_open(ClipperLib::ClipType clipType, PathsProvider1 &&subject, PathsProvider2 &&clip)
{
    // Open-path clipping on Clipper2: the subject paths are open, the clip is closed.
    // Clipper2 returns the closed and open portions of the solution separately; collect both.
    Clipper2Lib::Clipper64 c;
    Clipper2Lib::Paths64 subj = provider_to_paths64(std::forward<PathsProvider1>(subject));
    if (!subj.empty())
        c.AddOpenSubject(subj);
    Clipper2Lib::Paths64 cl = provider_to_paths64(std::forward<PathsProvider2>(clip));
    if (!cl.empty())
        c.AddClip(cl);
    Clipper2Lib::Paths64 solution, solution_open;
    c.Execute(to_clipper2_cliptype(clipType), Clipper2Lib::FillRule::NonZero, solution, solution_open);

    Polylines out;
    out.reserve(solution.size() + solution_open.size());
    for (const Clipper2Lib::Path64 &p : solution)
        out.emplace_back(path64_to_clipper1(p));
    for (const Clipper2Lib::Path64 &p : solution_open)
        out.emplace_back(path64_to_clipper1(p));
    return out;
}

// If the split_at_first_point() call above happens to split the polygon inside the clipping area
// we would get two consecutive polylines instead of a single one, so we go through them in order
// to recombine continuous polylines.
static void _clipper_pl_recombine(Polylines &polylines)
{
    for (size_t i = 0; i < polylines.size(); ++i) {
        for (size_t j = i+1; j < polylines.size(); ++j) {
            if (polylines[i].points.back() == polylines[j].points.front()) {
                /* If last point of i coincides with first point of j,
                   append points of j to i and delete j */
                polylines[i].points.insert(polylines[i].points.end(), polylines[j].points.begin()+1, polylines[j].points.end());
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.front() == polylines[j].points.back()) {
                /* If first point of i coincides with last point of j,
                   prepend points of j to i and delete j */
                polylines[i].points.insert(polylines[i].points.begin(), polylines[j].points.begin(), polylines[j].points.end()-1);
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.front() == polylines[j].points.front()) {
                /* Since Clipper does not preserve orientation of polylines, 
                   also check the case when first point of i coincides with first point of j. */
                polylines[j].reverse();
                polylines[i].points.insert(polylines[i].points.begin(), polylines[j].points.begin(), polylines[j].points.end()-1);
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.back() == polylines[j].points.back()) {
                /* Since Clipper does not preserve orientation of polylines, 
                   also check the case when last point of i coincides with last point of j. */
                polylines[j].reverse();
                polylines[i].points.insert(polylines[i].points.end(), polylines[j].points.begin()+1, polylines[j].points.end());
                polylines.erase(polylines.begin() + j);
                --j;
            }
        }
    }
}

template<typename PathProvider1, typename PathProvider2>
Polylines _clipper_pl_closed(ClipperLib::ClipType clipType, PathProvider1 &&subject, PathProvider2 &&clip)
{
    // Transform input polygons into open paths.
    ClipperLib::Paths paths;
    paths.reserve(subject.size());
    for (const Points &poly : subject) {
        // Emplace polygon, duplicate the 1st point.
        paths.push_back({});
        ClipperLib::Path &path = paths.back();
        path.reserve(poly.size() + 1);
        path = poly;
        path.emplace_back(poly.front());
    }
    // perform clipping
    Polylines retval = _clipper_pl_open(clipType, paths, std::forward<PathProvider2>(clip));
    _clipper_pl_recombine(retval);
    return retval;
}

Slic3r::Polylines diff_pl(const Slic3r::Polyline& subject, const Slic3r::Polygons& clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_closed(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygon &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::SinglePathProvider(clip.points)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polyline &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_closed(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip)); }

Lines _clipper_ln(ClipperLib::ClipType clipType, const Lines &subject, const Polygons &clip)
{
    // convert Lines to Polylines
    Polylines polylines;
    polylines.reserve(subject.size());
    for (const Line &line : subject)
        polylines.emplace_back(Polyline(line.a, line.b));
    
    // perform operation
    polylines = _clipper_pl_open(clipType, ClipperUtils::PolylinesProvider(polylines), ClipperUtils::PolygonsProvider(clip));
    
    // convert Polylines to Lines
    Lines retval;
    for (Polylines::const_iterator polyline = polylines.begin(); polyline != polylines.end(); ++polyline)
        if (polyline->size() >= 2)
            //FIXME It may happen, that Clipper produced a polyline with more than 2 collinear points by clipping a single line with polygons. It is a very rare issue, but it happens, see GH #6933.
            retval.push_back({ polyline->front(), polyline->back() });
    return retval;
}

// Run a Clipper2 union (EvenOdd) and assemble the result into a ClipperLib::PolyTree,
// preserving the nesting (and thus IsHole() parity) the downstream PolyNode traversal
// relies on. The PolyTree return type is part of the public API and is consumed as a
// ClipperLib data container (SLA::Pad, union_pt_chained_outside_in), so it is kept; only
// the boolean engine moves to Clipper2.
static ClipperLib::PolyTree clipper2_union_pt(const Clipper2Lib::Paths64 &subject, Clipper2Lib::FillRule fillRule)
{
    Clipper2Lib::Clipper64 c;
    if (!subject.empty())
        c.AddSubject(subject);
    Clipper2Lib::PolyTree64 solution;
    c.Execute(Clipper2Lib::ClipType::Union, fillRule, solution);

    // Flatten the Clipper2 polytree into parallel parents/contours arrays
    // (parents-before-children), then build the ClipperLib::PolyTree.
    std::vector<int>             parents;
    std::vector<ClipperLib::Path> contours;
    struct Flatten {
        static void rec(const Clipper2Lib::PolyPath64 &node, int parent,
                        std::vector<int> &parents, std::vector<ClipperLib::Path> &contours)
        {
            for (size_t i = 0; i < node.Count(); ++i) {
                const Clipper2Lib::PolyPath64 &child = *node.Child(i);
                int idx = int(contours.size());
                parents.emplace_back(parent);
                contours.emplace_back(path64_to_clipper1(child.Polygon()));
                rec(child, idx, parents, contours);
            }
        }
    };
    Flatten::rec(solution, -1, parents, contours);

    ClipperLib::PolyTree out;
    out.BuildFromNesting(parents, std::move(contours));
    return out;
}

// Convert polygons / expolygons into ClipperLib::PolyTree using EvenOdd, thus union will NOT be performed.
// If the contours are not intersecting, their orientation shall not be modified by union_pt().
ClipperLib::PolyTree union_pt(const Polygons &subject)
{
    return clipper2_union_pt(provider_to_paths64(ClipperUtils::PolygonsProvider(subject)), Clipper2Lib::FillRule::EvenOdd);
}
ClipperLib::PolyTree union_pt(const ExPolygons &subject)
{
    return clipper2_union_pt(provider_to_paths64(ClipperUtils::ExPolygonsProvider(subject)), Clipper2Lib::FillRule::EvenOdd);
}

// Simple spatial ordering of Polynodes
ClipperLib::PolyNodes order_nodes(const ClipperLib::PolyNodes &nodes)
{
    // collect ordering points
    Points ordering_points;
    ordering_points.reserve(nodes.size());
    
    for (const ClipperLib::PolyNode *node : nodes)
        ordering_points.emplace_back(
            Point(node->Contour.front().x(), node->Contour.front().y()));

    // perform the ordering
    ClipperLib::PolyNodes ordered_nodes =
        chain_clipper_polynodes(ordering_points, nodes);

    return ordered_nodes;
}

static void traverse_pt_noholes(const ClipperLib::PolyNodes &nodes, Polygons *out)
{
    foreach_node<e_ordering::ON>(nodes, [&out](const ClipperLib::PolyNode *node) 
    {
        traverse_pt_noholes(node->Childs, out);
        out->emplace_back(node->Contour);
        if (node->IsHole()) out->back().reverse(); // ccw
    });
}

static void traverse_pt_outside_in(ClipperLib::PolyNodes &&nodes, Polygons *retval)
{
    // collect ordering points
    Points ordering_points;
    ordering_points.reserve(nodes.size());
    for (const ClipperLib::PolyNode *node : nodes)
        ordering_points.emplace_back(node->Contour.front().x(), node->Contour.front().y());

    // Perform the ordering, push results recursively.
    //FIXME pass the last point to chain_clipper_polynodes?
    for (ClipperLib::PolyNode *node : chain_clipper_polynodes(ordering_points, nodes)) {
        retval->emplace_back(std::move(node->Contour));
        if (node->IsHole()) 
            // Orient a hole, which is clockwise oriented, to CCW.
            retval->back().reverse();
        // traverse the next depth
        traverse_pt_outside_in(std::move(node->Childs), retval);
    }
}

Polygons union_pt_chained_outside_in(const Polygons &subject)
{
    Polygons retval;
    traverse_pt_outside_in(union_pt(subject).Childs, &retval);
    return retval;
}

Polygons simplify_polygons(const Polygons &subject)
{
    // Clipper2 always emits strictly-simple (non self-intersecting) output, so a plain
    // NonZero union is the equivalent of the old StrictlySimple Clipper1 union.
    return clipper2_clip_polygons(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::NonZero,
                                  provider_to_paths64(ClipperUtils::PolygonsProvider(subject)),
                                  Clipper2Lib::Paths64{});
}

ExPolygons simplify_polygons_ex(const Polygons &subject)
{
    return clipper2_clip_ex(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::NonZero,
                            provider_to_paths64(ClipperUtils::PolygonsProvider(subject)),
                            Clipper2Lib::Paths64{});
}

Polygons top_level_islands(const Slic3r::Polygons &polygons)
{
    // Perform an EvenOdd union on Clipper2 producing a polytree, then keep only the
    // top-level islands (outer contours, ignoring their holes/children).
    Clipper2Lib::Clipper64 c;
    Clipper2Lib::Paths64 subj = provider_to_paths64(ClipperUtils::PolygonsProvider(polygons));
    if (!subj.empty())
        c.AddSubject(subj);
    Clipper2Lib::PolyTree64 polytree;
    c.Execute(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::EvenOdd, polytree);
    Polygons out;
    out.reserve(polytree.Count());
    for (size_t i = 0; i < polytree.Count(); ++i)
        out.emplace_back(path64_to_clipper1(polytree[i]->Polygon()));
    return out;
}

// Outer offset shall not split the input contour into multiples. It is expected, that the solution will be non empty and it will contain just a single polygon.
ClipperLib::Paths fix_after_outer_offset(
	const ClipperLib::Path 		&input, 
													// combination of default prameters to correspond to void ClipperOffset::Execute(Paths& solution, double delta)
													// to produce a CCW output contour from CCW input contour for a positive offset.
	ClipperLib::PolyFillType 	 filltype, 			// = ClipperLib::pftPositive
	bool 						 reverse_result)	// = false
{
  	ClipperLib::Paths solution;
  	if (! input.empty()) {
		Clipper2Lib::Path64 in = path_to_path64(input);
		Clipper2Lib::Clipper64 clipper;
		clipper.ReverseSolution(reverse_result);
		clipper.AddSubject(Clipper2Lib::Paths64{ in });
		Clipper2Lib::Paths64 sol;
		clipper.Execute(Clipper2Lib::ClipType::Union, to_clipper2_fillrule(filltype), sol);
		solution = paths64_to_clipper1(sol);
	}
    return solution;
}

// Inner offset may split the source contour into multiple contours, but one resulting contour shall not lie inside the other.
ClipperLib::Paths fix_after_inner_offset(
	const ClipperLib::Path 		&input, 
													// combination of default prameters to correspond to void ClipperOffset::Execute(Paths& solution, double delta)
													// to produce a CCW output contour from CCW input contour for a negative offset.
	ClipperLib::PolyFillType 	 filltype, 			// = ClipperLib::pftNegative
	bool 						 reverse_result) 	// = true
{
  	ClipperLib::Paths solution;
  	if (! input.empty()) {
		Clipper2Lib::Path64 in = path_to_path64(input);
		Clipper2Lib::Rect64 r = Clipper2Lib::GetBounds(in);
		r.left -= 10; r.top -= 10; r.right += 10; r.bottom += 10;
		Clipper2Lib::Path64 bbox = (filltype == ClipperLib::pftPositive) ?
			Clipper2Lib::Path64{ { r.left, r.bottom }, { r.left, r.top }, { r.right, r.top }, { r.right, r.bottom } } :
			Clipper2Lib::Path64{ { r.left, r.bottom }, { r.right, r.bottom }, { r.right, r.top }, { r.left, r.top } };
		Clipper2Lib::Clipper64 clipper;
		clipper.ReverseSolution(reverse_result);
		clipper.AddSubject(Clipper2Lib::Paths64{ in, bbox });
		Clipper2Lib::Paths64 sol;
		clipper.Execute(Clipper2Lib::ClipType::Union, to_clipper2_fillrule(filltype), sol);
		// The added bounding box becomes the outermost contour of the union; drop it.
		// Clipper2 does not guarantee solution ordering, so remove the largest-area
		// contour (the bbox) rather than the first one (which Clipper1 happened to emit).
		if (! sol.empty()) {
			size_t outer = 0;
			double max_abs_area = -1.0;
			for (size_t i = 0; i < sol.size(); ++i) {
				double a = std::abs(Clipper2Lib::Area(sol[i]));
				if (a > max_abs_area) { max_abs_area = a; outer = i; }
			}
			sol.erase(sol.begin() + outer);
		}
		solution = paths64_to_clipper1(sol);
	}
	return solution;
}

ClipperLib::Path mittered_offset_path_scaled(const Points &contour, const std::vector<float> &deltas, double miter_limit)
{
	assert(contour.size() == deltas.size());

#ifndef NDEBUG
	// Verify that the deltas are either all positive, or all negative.
	bool positive = false;
	bool negative = false;
	for (float delta : deltas)
		if (delta < 0.f)
			negative = true;
		else if (delta > 0.f)
			positive = true;
	assert(! (negative && positive));
#endif /* NDEBUG */

	ClipperLib::Path out;

	if (deltas.size() > 2)
	{
		out.reserve(contour.size() * 2);

		// Clamp miter limit to 2.
		miter_limit = (miter_limit > 2.) ? 2. / (miter_limit * miter_limit) : 0.5;
		
		// perpenduclar vector
		auto   perp = [](const Vec2d &v) -> Vec2d { return Vec2d(v.y(), - v.x()); };

		// Add a new point to the output, scale by CLIPPER_OFFSET_SCALE and round to ClipperLib::cInt.
		auto   add_offset_point = [&out](Vec2d pt) {
            pt += Vec2d(0.5 - (pt.x() < 0), 0.5 - (pt.y() < 0));
			out.emplace_back(ClipperLib::cInt(pt.x()), ClipperLib::cInt(pt.y()));
		};

		// Minimum edge length, squared.
		double lmin  = *std::max_element(deltas.begin(), deltas.end()) * ClipperOffsetShortestEdgeFactor;
		double l2min = lmin * lmin;
		// Minimum angle to consider two edges to be parallel.
		// Vojtech's estimate.
//		const double sin_min_parallel = EPSILON + 1. / double(CLIPPER_OFFSET_SCALE);
		// Implementation equal to Clipper.
		const double sin_min_parallel = 1.;

		// Find the last point further from pt by l2min.
		Vec2d  pt     = contour.front().cast<double>();
		size_t iprev  = contour.size() - 1;
		Vec2d  ptprev;
		for (; iprev > 0; -- iprev) {
			ptprev = contour[iprev].cast<double>();
			if ((ptprev - pt).squaredNorm() > l2min)
				break;
		}

		if (iprev != 0) {
			size_t ilast = iprev;
			// Normal to the (pt - ptprev) segment.
			Vec2d nprev = perp(pt - ptprev).normalized();
			for (size_t i = 0; ; ) {
				// Find the next point further from pt by l2min.
				size_t j = i + 1;
				Vec2d ptnext;
				for (; j <= ilast; ++ j) {
					ptnext = contour[j].cast<double>();
					double l2 = (ptnext - pt).squaredNorm();
					if (l2 > l2min)
						break;
				}
				if (j > ilast) {
					assert(i <= ilast);
					// If the last edge is too short, merge it with the previous edge.
					i = ilast;
					ptnext = contour.front().cast<double>();
				}

				// Normal to the (ptnext - pt) segment.
				Vec2d nnext  = perp(ptnext - pt).normalized();

				double delta  = deltas[i];
				double sin_a  = std::clamp(cross2(nprev, nnext), -1., 1.);
				double convex = sin_a * delta;
				if (convex <= - sin_min_parallel) {
					// Concave corner.
					add_offset_point(pt + nprev * delta);
					add_offset_point(pt);
					add_offset_point(pt + nnext * delta);
				} else {
					double dot = nprev.dot(nnext);
					if (convex < sin_min_parallel && dot > 0.) {
						// Nearly parallel.
						add_offset_point((nprev.dot(nnext) > 0.) ? (pt + nprev * delta) : pt);
					} else {
						// Convex corner, possibly extremely sharp if convex < sin_min_parallel.
						double r = 1. + dot;
					  	if (r >= miter_limit)
							add_offset_point(pt + (nprev + nnext) * (delta / r));
					  	else {
							double dx = std::tan(std::atan2(sin_a, dot) / 4.);
							Vec2d  newpt1 = pt + (nprev - perp(nprev) * dx) * delta;
							Vec2d  newpt2 = pt + (nnext + perp(nnext) * dx) * delta;
#ifndef NDEBUG
							Vec2d vedge = 0.5 * (newpt1 + newpt2) - pt;
							double dist_norm = vedge.norm();
							assert(std::abs(dist_norm - std::abs(delta)) < SCALED_EPSILON);
#endif /* NDEBUG */
							add_offset_point(newpt1);
							add_offset_point(newpt2);
					  	}
					}
				}

				if (i == ilast)
					break;

				ptprev = pt;
				nprev  = nnext;
				pt     = ptnext;
				i = j;
			}
		}
	}

#if 0
	{
		ClipperLib::Path polytmp(out);
		unscaleClipperPolygon(polytmp);
		Slic3r::Polygon offsetted(std::move(polytmp));
		BoundingBox bbox = get_extents(contour);
		bbox.merge(get_extents(offsetted));
		static int iRun = 0;
		SVG svg(debug_out_path("mittered_offset_path_scaled-%d.svg", iRun ++).c_str(), bbox);
		svg.draw_outline(Polygon(contour), "blue", scale_(0.01));
		svg.draw_outline(offsetted, "red", scale_(0.01));
		svg.draw(contour, "blue", scale_(0.03));
		svg.draw((Points)offsetted, "blue", scale_(0.03));
	}
#endif

	return out;
}

Polygons variable_offset_inner(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
	for (const std::vector<float> &ds : deltas)
		for (float delta : ds)
			assert(delta <= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_inner_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftNegative, true);
#ifndef NDEBUG	
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_outer_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftNegative, false));
#ifndef NDEBUG	
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ClipperLib::Paths output;
	if (holes.empty())
		output = std::move(contours);
	else {
		output = clipper2_do_paths(ClipperLib::ctDifference, contours, holes, ClipperLib::pftNonZero);
	}

	return to_polygons(std::move(output));
}

Polygons variable_offset_outer(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta >= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_outer_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftPositive, false);
#ifndef NDEBUG
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_inner_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftPositive, true));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ClipperLib::Paths output;
	if (holes.empty())
		output = std::move(contours);
	else {
		output = clipper2_do_paths(ClipperLib::ctDifference, contours, holes, ClipperLib::pftNonZero);
	}

	return to_polygons(std::move(output));
}

ExPolygons variable_offset_outer_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta >= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_outer_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftPositive, false);
#ifndef NDEBUG
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_inner_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftPositive, true));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ExPolygons output;
	if (holes.empty()) {
		output.reserve(contours.size());
		for (ClipperLib::Path &path : contours) 
			output.emplace_back(std::move(path));
	} else {
		output = clipper2_clip_ex(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero,
		                          to_paths64(contours), to_paths64(holes));
	}

	return output;
}


ExPolygons variable_offset_inner_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
	for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta <= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_inner_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftNegative, true);
#ifndef NDEBUG
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_outer_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftNegative, false));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ExPolygons output;
	if (holes.empty()) {
		output.reserve(contours.size());
		for (ClipperLib::Path &path : contours) 
			output.emplace_back(std::move(path));
	} else {
		output = clipper2_clip_ex(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero,
		                          to_paths64(contours), to_paths64(holes));
	}

	return output;
}

Pointfs make_counter_clockwise(const Pointfs& pointfs)
{
    Pointfs ps = pointfs;
    if (Polygon::new_scale(pointfs).is_clockwise()) {
        std::reverse(ps.begin(), ps.end());
    }

    return ps;
}

}
