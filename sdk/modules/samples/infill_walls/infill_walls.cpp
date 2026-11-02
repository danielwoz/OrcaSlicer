#include "orca_module_sdk.hpp"

#include <vector>

namespace {

// Loops around sparse infill, inside the inner wall. The sparse lines are trimmed back so they end on the loops.
struct InfillWalls
{
    static constexpr const char *key     = "infill_walls";
    static constexpr const char *label   = "Infill walls";
    static constexpr const char *version = "1.0.0";

    static constexpr orca_infill_param loops = orca_infill::int_param("loops", "Loops", 0, 0, 5, nullptr,
        "Loops printed around the sparse infill, inside the inner wall; the sparse infill lines end on them. 0 turns this off.");
    static constexpr orca_infill_param min_width = orca_infill::float_param("min_width", "Minimum area width", 5., 0., 50., "mm",
        "Sparse infill areas whose part inside the loops is narrower than this keep their plain infill.");
    static constexpr orca_module_option options[] = {
        orca_mod::region_option(loops, ORCA_STEP_INFILL, "sparse_infill_density > 0"),
        orca_mod::region_option(min_width, ORCA_STEP_INFILL, "sparse_infill_density > 0 and infill_walls__loops > 0",
                                ORCA_OPTION_ADVANCED),
    };
    enum { Loops, MinWidth };

    static void fill_layer(const orca_mod::FillLayer &layer, orca_mod::FillEdit &edit)
    {
        // The loops are added once every area is trimmed, so the trimming of one area never cuts the loops of another.
        struct Loop { const orca_mod::FillRegion *region; orca_infill::Points points; double height; };
        std::vector<Loop> added;
        for (const orca_mod::FillRegion &region : layer.regions()) {
            const int loops = int(region.option(Loops));
            if (loops <= 0 || region.sparse_density() <= 0.)
                continue;
            const double spacing = region.sparse_spacing();
            for (const orca_mod::Area &area : layer.areas(region, ORCA_FILL_SPARSE)) {
                // A narrow area keeps its plain infill.
                if (edit.offset(area.region, -(loops * spacing + 0.5 * region.option(MinWidth))).empty())
                    continue;
                // The sparse lines reach a little past the area, into the wall overlap, so the band starts outside it.
                edit.erase(ORCA_FILL_MASK(ORCA_FILL_SPARSE), edit.offset(area.region, spacing), edit.offset(area.region, -loops * spacing));
                for (int i = 0; i < loops; ++ i)
                    for (orca_infill::Region &ring : edit.offset(area.region, -(i + 0.5) * spacing)) {
                        added.push_back({ &region, std::move(ring.contour), area.height });
                        for (orca_infill::Points &hole : ring.holes)
                            added.push_back({ &region, std::move(hole), area.height });
                    }
            }
        }
        for (const Loop &loop : added)
            edit.add(*loop.region, ORCA_FILL_SPARSE, loop.points, true, loop.height);
    }
};

} // namespace

ORCA_MODULE(InfillWalls)
