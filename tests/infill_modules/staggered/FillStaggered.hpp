#pragma once

#include "libslic3r/Fill/FillBase.hpp"

namespace Slic3r {

// Parallel lines along the infill direction that move by half their distance on every other layer, optionally crossed
// at right angles at twice their distance (staggered_spacing_scale, staggered_cross).
class FillStaggered : public Fill
{
public:
    Fill *clone() const override { return new FillStaggered(*this); }
    bool  is_self_crossing() override { return false; }

protected:
    // The lines keep the infill direction on every layer; the stagger takes the place of the 90 degree turn.
    float _layer_angle(size_t /* idx */) const override { return 0.f; }
    using Fill::_fill_surface_single;
    void  _fill_surface_single(const FillParams &params, unsigned int thickness_layers, const std::pair<float, Point> &direction,
                               ExPolygon expolygon, Polylines &polylines_out) override;
};

} // namespace Slic3r
