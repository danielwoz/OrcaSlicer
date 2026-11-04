// The inputs of the lightning generators (Fill/Lightning/Generator.hpp) that come from a print object: the parameters,
// read from its configuration, and the infill outlines of its layers, which the pattern code does not read.
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"
#include "FillLightning.hpp"
#include "Lightning/Generator.hpp"

namespace Slic3r::FillLightning {

Generator::Params lightning_infill_params(const PrintObject &print_object)
{
    Generator::Params          params;
    const PrintConfig         &print_config         = print_object.print()->config();
    const PrintObjectConfig   &object_config        = print_object.config();
    const PrintRegionConfig   &region_config        = print_object.shared_regions()->all_regions.front()->config();
    const std::vector<double> &nozzle_diameters     = print_config.nozzle_diameter.values;
    double                     max_nozzle_diameter  = *std::max_element(nozzle_diameters.begin(), nozzle_diameters.end());
    const int                  n_multiline          = region_config.fill_multiline.value;
//    const int                  infill_extruder      = region_config.infill_extruder.value;
    const double               default_infill_extrusion_width = Flow::auto_extrusion_width(FlowRole::frInfill, float(max_nozzle_diameter));
    // Note: There's not going to be a layer below the first one, so the 'initial layer height' doesn't have to be taken into account.
    const double               layer_thickness      = scaled<double>(object_config.layer_height.value);

    params.infill_extrusion_width = scaled<float>(region_config.sparse_infill_line_width.get_abs_value(max_nozzle_diameter));
    // Orca: fix lightning infill divide by zero when infill line width is set to 0.
    // firstly attempt to set it to the default line width. If that is not provided either, set it to a sane default
    // based on the nozzle diameter.
    if (params.infill_extrusion_width < EPSILON)
        params.infill_extrusion_width = scaled<float>(
            object_config.line_width.get_abs_value(max_nozzle_diameter) < EPSILON ?
            default_infill_extrusion_width :
            object_config.line_width.get_abs_value(max_nozzle_diameter)
        );

    params.supporting_radius = coord_t(params.infill_extrusion_width) * 100 * n_multiline / region_config.sparse_infill_density;

    const double lightning_infill_overhang_angle      = region_config.lightning_overhang_angle.value * M_PI / 180.0;
    const double lightning_infill_prune_angle         = region_config.lightning_prune_angle.value * M_PI / 180.0;
    const double lightning_infill_straightening_angle = region_config.lightning_straightening_angle.value * M_PI / 180.0;
    params.wall_supporting_radius                     = coord_t(layer_thickness * std::tan(lightning_infill_overhang_angle));
    params.prune_length                               = coord_t(layer_thickness * std::tan(lightning_infill_prune_angle));
    params.straightening_max_distance                 = coord_t(layer_thickness * std::tan(lightning_infill_straightening_angle));
    return params;
}

std::vector<Polygons> lightning_infill_outlines(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    std::vector<Polygons> infill_outlines(print_object.layers().size(), Polygons());

    // For-each layer from top to bottom:
    for (int layer_id = int(print_object.layers().size()) - 1; layer_id >= 0; layer_id--) {
        throw_on_cancel_callback();
        for (const LayerRegion *layerm : print_object.get_layer(layer_id)->regions())
            for (const Surface &surface : layerm->fill_surfaces.surfaces)
                if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid)
                    append(infill_outlines[layer_id], to_polygons(surface.expolygon));
    }
    return infill_outlines;
}

Generator::Params lightning_support_params(const PrintObject &print_object, float density)
{
    Generator::Params          params;
    const PrintConfig         &print_config         = print_object.print()->config();
    const PrintObjectConfig   &object_config        = print_object.config();
    const PrintRegionConfig   &region_config        = print_object.shared_regions()->all_regions.front()->config();
    const std::vector<double> &nozzle_diameters     = print_config.nozzle_diameter.values;
    double                     max_nozzle_diameter  = *std::max_element(nozzle_diameters.begin(), nozzle_diameters.end());
//    const int                  infill_extruder      = region_config.infill_extruder.value;
    const double               default_infill_extrusion_width = Flow::auto_extrusion_width(FlowRole::frInfill, float(max_nozzle_diameter));
    // Note: There's not going to be a layer below the first one, so the 'initial layer height' doesn't have to be taken into account.
    const double               layer_thickness      = scaled<double>(object_config.layer_height.value);

    params.infill_extrusion_width = scaled<float>(region_config.sparse_infill_line_width.get_abs_value(max_nozzle_diameter));
    // Orca: fix lightning infill divide by zero when infill line width is set to 0.
    // firstly attempt to set it to the default line width. If that is not provided either, set it to a sane default
    // based on the nozzle diameter.
    if (params.infill_extrusion_width < EPSILON)
        params.infill_extrusion_width = scaled<float>(
            object_config.line_width.get_abs_value(max_nozzle_diameter) < EPSILON ?
            default_infill_extrusion_width :
            object_config.line_width.get_abs_value(max_nozzle_diameter)
        );
    //m_supporting_radius: against to the density of lightning, failures may happen if set to high density
    //higher density lightning makes support harder, more time-consuming on computing and printing, but more reliable on supporting overhangs
    //lower density lightning performs opposite
    //TODO: decide whether enable density controller in advanced options or not
    density = std::max(0.15f, density);
    params.supporting_radius = coord_t(params.infill_extrusion_width) / density;
    // Keep support-lightning behavior fixed and independent of user print-region angles.
    const double lightning_infill_overhang_angle = M_PI / 4; // 45 degrees
    const double lightning_infill_prune_angle = M_PI / 4; // 45 degrees
    const double lightning_infill_straightening_angle = M_PI / 4; // 45 degrees
    params.wall_supporting_radius = layer_thickness * std::tan(lightning_infill_overhang_angle);
    params.prune_length = layer_thickness * std::tan(lightning_infill_prune_angle);
    params.straightening_max_distance = layer_thickness * std::tan(lightning_infill_straightening_angle);
    return params;
}

GeneratorPtr build_generator(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    return GeneratorPtr(new Generator(lightning_infill_params(print_object), lightning_infill_outlines(print_object, throw_on_cancel_callback),
                                      throw_on_cancel_callback));
}

} // namespace Slic3r::FillLightning
