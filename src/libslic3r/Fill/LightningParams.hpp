#pragma once

// The inputs of the lightning generators (Fill/Lightning/Generator.hpp) that come from a print object: the parameters,
// read from its configuration, and the infill outlines of its layers (Fill/LightningParams.cpp).

#include <functional>
#include <vector>

#include "../Polygon.hpp"
#include "../libslic3r.h"

namespace Slic3r {
class PrintObject;

namespace FillLightning {

// The widths and distances a generator grows its trees by, scaled.
struct GeneratorParams
{
    float   infill_extrusion_width;
    coord_t supporting_radius;
    coord_t wall_supporting_radius;
    coord_t prune_length;
    coord_t straightening_max_distance;
};

// The parameters, from the configuration of print_object, and the outlines of each layer of the lightning infill of
// print_object.
GeneratorParams       lightning_infill_params(const PrintObject &print_object);
std::vector<Polygons> lightning_infill_outlines(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback);
// The parameters of the lightning support base of print_object, of the given density.
GeneratorParams       lightning_support_params(const PrintObject &print_object, float density = 0.15);

} // namespace FillLightning
} // namespace Slic3r
