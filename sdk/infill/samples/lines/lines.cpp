#include "orca_infill_sdk.hpp"

namespace {

constexpr const char *phase_values[] = { "aligned", "staggered" };
constexpr const char *phase_labels[] = { "Aligned", "Staggered" };

// Parallel lines along the x axis of the pattern frame, laid out from the origin so they continue across regions
// and layers; optionally crossed by lines along the y axis.
struct Lines
{
    static constexpr const char *key   = "org.orcaslicer.sample.lines";
    static constexpr const char *label = "Lines (sample)";
    static constexpr uint32_t    slots = ORCA_INFILL_SLOT_SPARSE | ORCA_INFILL_SLOT_SURFACE;
    static constexpr uint32_t    caps  = ORCA_INFILL_CAP_MULTILINE | ORCA_INFILL_CAP_SEPARABLE | ORCA_INFILL_CAP_FIXED_ANGLE |
                                         ORCA_INFILL_CAP_ROTATE_TEMPLATE_SAFE;
    static constexpr uint32_t    post  = ORCA_INFILL_POST_DEFAULT;
    enum { SpacingScale, Cross, Phase };
    static constexpr orca_infill_param params[] = {
        orca_infill::float_param("spacing_scale", "Spacing scale", 1., 0.5, 4., nullptr,
                                 "Distance of the lines as a multiple of the distance for the infill density."),
        orca_infill::bool_param("cross", "Cross lines", false, "Also draw lines at right angles, at twice the distance."),
        orca_infill::enum_param("phase", "Phase", phase_values, phase_labels, 0,
                                "Staggered shifts the lines by half their distance on every other layer."),
    };

    static void generate(const orca_infill::Request &req, orca_infill::Output &out)
    {
        const bool    cross    = req.param(Cross) != 0.;
        const int64_t distance = req.units(req.line_distance() * req.param(SpacingScale) * (cross ? 2. : 1.));
        if (distance <= 0)
            throw orca_infill::Failed();
        const int64_t           shift  = req.param(Phase) == 1. && (req.layer_index() & 1) ? distance / 2 : 0;
        const orca_infill_bbox &box    = req.bbox();
        const orca_infill_point origin = req.origin();
        for (int64_t y = orca_infill::Request::align_up(box.min.y, origin.y + shift, distance); y <= box.max.y; y += distance)
            out.line({ box.min.x, y }, { box.max.x, y });
        if (cross)
            for (int64_t x = orca_infill::Request::align_up(box.min.x, origin.x + shift, distance); x <= box.max.x; x += distance)
                out.line({ x, box.min.y }, { x, box.max.y });
    }
};

} // namespace

#ifdef ORCA_SAMPLE_LINES_MODULE
#include "orca_module_sdk.hpp"
namespace {
// The same pattern as a module: its parameters are the settings sample_lines__spacing_scale, __cross and __phase.
struct SampleLines
{
    static constexpr const char *key     = "sample_lines";
    static constexpr const char *label   = "Lines (sample)";
    static constexpr const char *version = "1.0.0";
    using patterns = std::tuple<Lines>;
};
} // namespace
ORCA_MODULE(SampleLines)
#else
ORCA_INFILL_PLUGIN("OrcaSlicer sample infill patterns", "1.0.0", Lines)
#endif
