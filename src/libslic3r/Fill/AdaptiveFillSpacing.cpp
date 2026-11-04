// FillAdaptive::adaptive_fill_line_spacing(): the line spacing the octrees of adaptive and support cubic infill are
// built with, from the print object and its region configurations, which the pattern code does not read.
#include <algorithm>
#include <cstddef>
#include <vector>

#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "FillAdaptive.hpp"

namespace Slic3r {
namespace FillAdaptive {

std::vector<double> adaptive_fill_line_spacing(const PrintObject &print_object)
{
    std::vector<double>        line_spacing(print_object.num_printing_regions(), 0.);
    const std::vector<double> &nozzle_diameters               = print_object.print()->config().nozzle_diameter.values;
    double                     max_nozzle_diameter            = *std::max_element(nozzle_diameters.begin(), nozzle_diameters.end());
    double                     default_infill_extrusion_width = Flow::auto_extrusion_width(FlowRole::frInfill, float(max_nozzle_diameter));
    for (size_t region_id = 0; region_id < line_spacing.size(); ++ region_id) {
        const PrintRegionConfig &config = print_object.printing_region(region_id).config();
        if (config.sparse_infill_density <= 0 || ! is_octree_infill_pattern(config.sparse_infill_pattern) ||
            std::none_of(print_object.layers().begin(), print_object.layers().end(), [region_id](const Layer *layer) {
                return region_id < layer->regions().size() && ! layer->regions()[region_id]->fill_surfaces.empty();
            }))
            continue;
        double extrusion_width = config.sparse_infill_line_width.get_abs_value(max_nozzle_diameter);
        if (extrusion_width == 0.)
            extrusion_width = default_infill_extrusion_width;
        line_spacing[region_id] = extrusion_width / ((config.sparse_infill_density / 100.0f) * 0.333333333f) * config.fill_multiline.value;
    }
    return line_spacing;
}

} // namespace FillAdaptive
} // namespace Slic3r
