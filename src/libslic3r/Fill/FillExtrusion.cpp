// Fill::fill_surface_extrusion(): the extrusions of the lines a filler generates for a surface, and their gap fill.
// It reads the print object and region configurations, which the pattern code does not.
#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"
#include "../PrintConfig.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "../libslic3r.h"

#include "FillBase.hpp"

namespace Slic3r {

// BBS: this method is used to fill the ExtrusionEntityCollection. It call fill_surface by default
void Fill::fill_surface_extrusion(const Surface* surface, const FillParams& params, ExtrusionEntitiesPtr& out)
{
    Polylines polylines;
    ThickPolylines thick_polylines;
    try {
        if (params.use_arachne)
            thick_polylines = this->fill_surface_arachne(surface, params);
        else
            polylines = this->fill_surface(surface, params);
    }
    catch (InfillFailedException&) {}

    if (!polylines.empty() || !thick_polylines.empty()) {
        // calculate actual flow from spacing (which might have been adjusted by the infill
        // pattern generator)
        double flow_mm3_per_mm = params.flow.mm3_per_mm();
        double flow_width = params.flow.width();
        if (params.using_internal_flow) {
            // if we used the internal flow we're not doing a solid infill
            // so we can safely ignore the slight variation that might have
            // been applied to f->spacing
        }
        else {
            Flow new_flow = params.flow.with_spacing(this->spacing);
            flow_mm3_per_mm = new_flow.mm3_per_mm();
            flow_width = new_flow.width();
        }
        // Save into layer.
        ExtrusionEntityCollection* eec = nullptr;
        out.push_back(eec = new ExtrusionEntityCollection());
        // Only concentric fills are not sorted.
        eec->no_sort = this->no_sort();
        // ORCA: special flag for flow rate calibration
        auto is_flow_calib = params.extrusion_role == erTopSolidInfill && this->print_object_config->has("calib_flowrate_topinfill_special_order") &&
                             this->print_object_config->option("calib_flowrate_topinfill_special_order")->getBool();
        // Orca: a forced surface fill order must survive the G-code path planner, which would
        // otherwise re-chain and possibly reverse the paths. The same applies to the flow rate
        // calibration's special toolpath order.
        const bool keep_fill_order = params.fill_order != SurfaceFillOrder::Default;
        if (is_flow_calib || keep_fill_order) {
            eec->no_sort = true;
        }
        size_t idx   = eec->entities.size();
        if (params.use_arachne) {
            Flow new_flow = params.flow.with_spacing(float(this->spacing));
            variable_width(thick_polylines, params.extrusion_role, new_flow, eec->entities);
            thick_polylines.clear();
        }
        else {
            // Orca: sparse infill lines whose ends are next to each other print as one path, without a travel between them.
            if (params.extrusion_role == erInternalInfill && ! eec->no_sort)
                join_close_ends(polylines, scale_(2. * this->spacing), params.can_reverse);
            extrusion_entities_append_paths(
                eec->entities, std::move(polylines),
                params.extrusion_role,
                flow_mm3_per_mm, float(flow_width), params.flow.height());
        }
        if (!params.can_reverse || is_flow_calib || keep_fill_order) {
            for (size_t i = idx; i < eec->entities.size(); i++)
                eec->entities[i]->set_reverse();
        }

        // Orca: run gap fill
        this->_create_gap_fill(surface, params, eec);
    }
}

// Orca: port of Bambu Studio's gap fill interleaving for fills printed in a fixed order, see docs/HLSD/travel-optimization.md.
static void insert_gap_fill(const ThickPolylines &gaps, const Flow &flow, ExtrusionEntitiesPtr &entities)
{
    if (entities.empty()) {
        variable_width(gaps, erGapFill, flow, entities);
        return;
    }
    static constexpr size_t npos = size_t(-1);
    // An extrusion of the fill, or gaps[gap], with the points where the nozzle enters and leaves it.
    struct Item { Point in, out; size_t gap; bool reversed; };
    auto distance = [](const Point &a, const Point &b) { return (b - a).cast<double>().norm(); };
    auto closed   = [&gaps](size_t gap) { return gaps[gap].first_point() == gaps[gap].last_point(); };
    // GCode::extrude_loop starts a loop at its point nearest the nozzle.
    auto travel = [&](const std::vector<Item> &seq) {
        double sum = 0.;
        Point  at  = seq.front().out;
        for (auto it = seq.begin() + 1; it != seq.end(); ++ it) {
            const Point in = it->gap != npos && closed(it->gap) ? *gaps[it->gap].closest_point(at) : it->in;
            sum += distance(at, in);
            at = it->gap != npos && closed(it->gap) ? in : it->out;
        }
        return sum;
    };

    std::vector<Item> items, appended;
    items.reserve(entities.size() + gaps.size());
    for (const ExtrusionEntity *ee : entities)
        items.push_back({ ee->first_point(), ee->last_point(), npos, false });
    appended = items;
    for (size_t gap = 0; gap < gaps.size(); ++ gap) {
        appended.push_back({ gaps[gap].first_point(), gaps[gap].last_point(), gap, false });
        double best      = std::numeric_limits<double>::max();
        size_t best_slot = 0;
        Item   best_item{};
        for (size_t slot = 0; slot <= items.size(); ++ slot) {
            const Point *prev = slot > 0 ? &items[slot - 1].out : nullptr;
            const Point *next = slot < items.size() ? &items[slot].in : nullptr;
            auto consider = [&](const Point &in, const Point &out, bool reversed) {
                const double added = (prev ? distance(*prev, in) : 0.) + (next ? distance(out, *next) : 0.) - (prev && next ? distance(*prev, *next) : 0.);
                if (added < best) {
                    best      = added;
                    best_slot = slot;
                    best_item = { in, out, gap, reversed };
                }
            };
            if (closed(gap)) {
                const Point &entry = *gaps[gap].closest_point(prev ? *prev : *next);
                consider(entry, entry, false);
            } else {
                consider(gaps[gap].first_point(), gaps[gap].last_point(), false);
                consider(gaps[gap].last_point(), gaps[gap].first_point(), true);
            }
        }
        items.insert(items.begin() + best_slot, best_item);
    }
    if (travel(appended) <= travel(items))
        items = std::move(appended);

    ExtrusionEntitiesPtr out;
    out.reserve(entities.size() + gaps.size());
    size_t next_entity = 0;
    for (const Item &item : items)
        if (item.gap == npos)
            out.push_back(entities[next_entity ++]);
        else {
            ThickPolylines gap { gaps[item.gap] };
            if (item.reversed)
                gap.front().reverse();
            variable_width(gap, erGapFill, flow, out);
        }
    entities = std::move(out);
}

// Orca: Dedicated function to calculate gap fill lines for the provided surface, according to the print object parameters
// and append them to the out ExtrusionEntityCollection.
void Fill::_create_gap_fill(const Surface* surface, const FillParams& params, ExtrusionEntityCollection* out){

    //Orca: just to be safe, check against null pointer for the print object config and if NULL return.
    if (this->print_object_config == nullptr) return;

    // Orca: Enable gap fill as per the user preference. Return early if gap fill is to not be applied.
    if ((this->print_object_config->gap_fill_target.value == gftNowhere) ||
        (surface->surface_type == stInternalSolid && this->print_object_config->gap_fill_target.value != gftEverywhere))
        return;

    Flow new_flow = params.flow;
    ExPolygons unextruded_areas;
    unextruded_areas = diff_ex(this->no_overlap_expolygons, union_ex(out->polygons_covered_by_spacing(10)));
    ExPolygons gapfill_areas = union_ex(unextruded_areas);
    if (!this->no_overlap_expolygons.empty())
        gapfill_areas = intersection_ex(gapfill_areas, this->no_overlap_expolygons);

    if (gapfill_areas.size() > 0 && params.density >= 1) {
        double min = 0.2 * new_flow.scaled_spacing() * (1 - INSET_OVERLAP_TOLERANCE);
        double max = 2. * new_flow.scaled_spacing();
        ExPolygons gaps_ex = diff_ex(
                                     opening_ex(gapfill_areas, float(min / 2.)),
                                     offset2_ex(gapfill_areas, -float(max / 2.), float(max / 2. + ClipperSafetyOffset)));
        //BBS: sort the gap_ex to avoid mess travel
        Points ordering_points;
        ordering_points.reserve(gaps_ex.size());
        ExPolygons gaps_ex_sorted;
        gaps_ex_sorted.reserve(gaps_ex.size());
        for (const ExPolygon &ex : gaps_ex)
            ordering_points.push_back(ex.contour.first_point());
        std::vector<Points::size_type> order2 = chain_points(ordering_points);
        for (size_t i : order2)
            gaps_ex_sorted.emplace_back(std::move(gaps_ex[i]));

        ThickPolylines polylines;
        for (ExPolygon& ex : gaps_ex_sorted) {
            //BBS: Use DP simplify to avoid duplicated points and accelerate medial-axis calculation as well.
            ex.douglas_peucker(SCALED_RESOLUTION * 0.1);
            ex.medial_axis(min, max, &polylines);
        }

        if (!polylines.empty() && !is_bridge(params.extrusion_role)) {
            polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                                           [&](const ThickPolyline& p) {
                return p.length() < scale_(params.config->filter_out_gap_fill.value);
            }), polylines.end());

            if (out->no_sort)
                insert_gap_fill(polylines, params.flow, out->entities);
            else
                variable_width(polylines, erGapFill, params.flow, out->entities);
        }
    }
}

} // namespace Slic3r
