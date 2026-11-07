#pragma once

#include <string>
#include <vector>

#include <orca_infill_plugin.h>

#include "FillBase.hpp"

namespace Slic3r {

struct InfillPatternDescriptor;

struct InfillPluginParam
{
    std::string              name, label, tooltip, unit;
    int32_t                  type { ORCA_INFILL_PARAM_FLOAT };
    double                   default_value { 0. }, min_value { 0. }, max_value { 0. };
    std::vector<std::string> enum_values, enum_labels;
};

// The host's copy of a pattern of an infill plugin; it lives until the process exits.
struct InfillPluginPattern
{
    std::string                    key, label;
    std::string                    plugin;      // "<name> <version>"
    uint32_t                       slots { 0 }, caps { 0 }, post { 0 }; // ORCA_INFILL_SLOT_*, CAP_*, POST_*
    std::vector<InfillPluginParam> params;
    void                          *user { nullptr };
    orca_infill_generate_fn        generate { nullptr };
    InfillPattern                  id { ipCount };
};

// An ExPolygon as an orca_infill_region, whose paths point into this object.
class InfillPluginRegion
{
public:
    explicit InfillPluginRegion(const ExPolygon &expolygon);
    InfillPluginRegion(const InfillPluginRegion &) = delete;
    InfillPluginRegion& operator=(const InfillPluginRegion &) = delete;
    const orca_infill_region& view() const { return m_view; }

private:
    std::vector<orca_infill_point> m_points;
    std::vector<orca_infill_path>  m_holes;
    orca_infill_region             m_view;
};

// Fills regions with the pattern of an infill plugin. The plugin draws lines in the pattern frame; this class rotates
// the region into that frame and back, and draws the multiline copies, clips, drops short pieces and connects the
// lines as the pattern asks.
class FillPlugin : public Fill
{
public:
    explicit FillPlugin(const InfillPluginPattern &pattern) : m_pattern(&pattern) {}
    Fill *clone() const override { return new FillPlugin(*this); }
    bool  is_self_crossing() override { return false; }
    bool  no_sort() const override { return (m_pattern->caps & ORCA_INFILL_CAP_KEEP_ORDER) != 0; }

protected:
    float _layer_angle(size_t idx) const override
    { return (m_pattern->caps & ORCA_INFILL_CAP_FIXED_ANGLE) ? 0.f : Fill::_layer_angle(idx); }
    using Fill::_fill_surface_single;
    void  _fill_surface_single(const FillParams &params, unsigned int thickness_layers, const std::pair<float, Point> &direction,
                               ExPolygon expolygon, Polylines &polylines_out) override;

private:
    const InfillPluginPattern *m_pattern;
    // Parameter values, read from params.config the first time it is seen.
    const PrintRegionConfig   *m_params_config { nullptr };
    bool                       m_params_read { false };
    std::vector<double>        m_params;
};

// Factory of the plugin rows of InfillPatternRegistry.
Fill* make_plugin_fill(const InfillPatternDescriptor &pattern);
// Calls the pattern's generate(), appending what it emits to out. A status reserved for the built-in pattern libraries
// is returned as ORCA_INFILL_ERR_EXCEPTION.
orca_infill_status call_infill_plugin(const InfillPluginPattern &pattern, const orca_infill_request &request, Polylines &out);

} // namespace Slic3r
