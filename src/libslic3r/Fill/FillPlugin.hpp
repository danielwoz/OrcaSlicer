#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

#include <orca_infill_plugin.h>

#include "FillBase.hpp"

namespace Slic3r {

struct InfillPatternDescriptor;
struct ModuleOption;
struct ModuleRecord;

// Size of the 1.0 layout of an ABI struct: up to the end of its last 1.0 field.
#define ORCA_ABI_V1_SIZE(TYPE, LAST) (offsetof(TYPE, LAST) + sizeof(TYPE::LAST))

// An ABI struct read into the host's layout: fields beyond the library's struct_size are zero, fields the host does
// not know are ignored.
template<class T> T read_abi_struct(const T *source)
{
    T out {};
    uint32_t size = 0;
    std::memcpy(&size, source, sizeof(size));
    std::memcpy(&out, source, std::min<size_t>(size, sizeof(T)));
    out.struct_size = size;
    return out;
}

struct InfillPluginParam
{
    std::string              name, label, tooltip, unit;
    int32_t                  type { ORCA_INFILL_PARAM_FLOAT };
    double                   default_value { 0. }, min_value { 0. }, max_value { 0. };
    std::vector<std::string> enum_values, enum_labels;
};

// The host's copy of a pattern of an infill plugin or a module; it lives until the process exits.
struct InfillPluginPattern
{
    std::string                    key, label;
    std::string                    plugin;      // "<name> <version>"
    std::string                    log_prefix;  // starts the log lines of its generate()
    uint32_t                       slots { 0 }, caps { 0 }, post { 0 }; // ORCA_INFILL_SLOT_*, CAP_*, POST_*
    std::vector<InfillPluginParam> params;
    void                          *user { nullptr };
    orca_infill_generate_fn        generate { nullptr };
    InfillPattern                  id { ipCount };
    // The module of the pattern, whose settings hold the parameter values, one per parameter; nullptr for a pattern
    // of an infill plugin, whose parameters are read from infill_plugin_params.
    const ModuleRecord            *module { nullptr };
    std::vector<const ModuleOption*> param_options;
};

// The infill host services; the table lives until the process exits.
const orca_infill_host& infill_plugin_host();
// A region the host services were given, as an ExPolygon.
ExPolygon infill_region_expolygon(const orca_infill_region &region);
// Whether the paths of a region the host services were given are readable.
bool valid_infill_region(const orca_infill_region *region);

// Names the source of the host's log lines on this thread while it lives. prefix starts each line, such as
// "Module infill_walls: ", and must outlive this object.
class InfillLogSource
{
public:
    explicit InfillLogSource(const std::string &prefix);
    ~InfillLogSource();
    InfillLogSource(const InfillLogSource &) = delete;
    InfillLogSource& operator=(const InfillLogSource &) = delete;

private:
    const std::string *m_previous;
};

// Runs the body of a host service, mapping its exceptions to status codes.
template<class F> orca_infill_status host_service(F &&body) noexcept
{
    try {
        body();
        return ORCA_INFILL_OK;
    } catch (const std::bad_alloc &) {
        return ORCA_INFILL_ERR_NOMEM;
    } catch (...) {
        return ORCA_INFILL_ERR_FAILED;
    }
}

// Calls into a plugin or a module with prefix starting the host's log lines. An exception from a library that breaks
// the ABI rules is logged and fails the call.
template<class F> orca_infill_status call_guarded(const std::string &prefix, F &&call)
{
    const InfillLogSource source(prefix);
    try {
        return call();
    } catch (const std::exception &e) {
        infill_plugin_host().log(ORCA_INFILL_LOG_ERROR, (std::string("threw: ") + e.what()).c_str());
    } catch (...) {
    }
    return ORCA_INFILL_ERR_EXCEPTION;
}

// The reason a parameter is rejected; empty when it is valid. previous holds the parameters declared before it.
std::string check_infill_param(const orca_infill_param &param, const std::vector<InfillPluginParam> &previous);
InfillPluginParam copy_infill_param(const orca_infill_param &param);
// Copies a pattern into a host record of plugin ("<name> <version>"); returns the reason it is rejected, or empty.
// warning receives the parts of the description that are ignored.
std::string copy_infill_pattern(const orca_infill_pattern *source, const std::string &plugin, InfillPluginPattern &out, std::string &warning);
// Registers copied patterns in token order, so their values do not depend on the order they are listed in. Returns
// the records of the registered ones, which live until the process exits; error receives why the others are rejected.
// Main thread only, before InfillPatternRegistry::seal().
std::vector<InfillPluginPattern*> register_infill_patterns(std::vector<InfillPluginPattern> patterns, std::string &error);
// Appends message to a list of errors separated by "; ".
void append_error(std::string &error, const std::string &message);

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
// Calls the pattern's generate(), appending what it emits to out.
orca_infill_status call_infill_plugin(const InfillPluginPattern &pattern, const orca_infill_request &request, Polylines &out);

} // namespace Slic3r
