// Infill plugins for the tests: broken, misbehaving or using the host services; the definition FIXTURE_<NAME> selects one.
#include "orca_infill_sdk.hpp"

#include <stdexcept>

#if defined(FIXTURE_WRONG_ABI)

// A plugin of a future ABI that does not check the host's version.
extern "C" ORCA_INFILL_EXPORT const orca_infill_plugin *ORCA_INFILL_CALL orca_infill_plugin_entry_v1(const orca_infill_host *)
{
    static const orca_infill_plugin plugin { sizeof(orca_infill_plugin), 2, 0, "OrcaSlicer test fixture", "1.0.0", nullptr, 0 };
    return &plugin;
}

#elif defined(FIXTURE_NO_ENTRY)

extern "C" ORCA_INFILL_EXPORT int orca_not_an_entry() { return 0; }

#else

#if defined(FIXTURE_BAD_KEY)
#define FIXTURE_KEY "nodot"
#elif defined(FIXTURE_DUPLICATE_KEY)
#define FIXTURE_KEY "org.orcaslicer.sample.lines"
#elif defined(FIXTURE_CONNECT_WITHOUT_CLIP)
#define FIXTURE_KEY  "org.orcaslicer.test.connect"
#define FIXTURE_POST ORCA_INFILL_POST_CONNECT
#elif defined(FIXTURE_FAILS_ODD_LAYERS)
#define FIXTURE_KEY "org.orcaslicer.test.fails"
#elif defined(FIXTURE_THROWS)
#define FIXTURE_KEY "org.orcaslicer.test.throws"
#elif defined(FIXTURE_TOP_ONLY)
#define FIXTURE_KEY   "org.orcaslicer.test.toponly"
#define FIXTURE_SLOTS ORCA_INFILL_SLOT_SURFACE
#elif defined(FIXTURE_INSET)
#define FIXTURE_KEY  "org.orcaslicer.test.inset"
#define FIXTURE_POST 0
#elif defined(FIXTURE_BAD_PARAM)
#define FIXTURE_KEY "org.orcaslicer.test.badparam"
#endif
#ifndef FIXTURE_POST
#define FIXTURE_POST ORCA_INFILL_POST_DEFAULT
#endif
#ifndef FIXTURE_SLOTS
#define FIXTURE_SLOTS (ORCA_INFILL_SLOT_SPARSE | ORCA_INFILL_SLOT_SURFACE)
#endif

namespace {

struct Fixture
{
    static constexpr const char *key   = FIXTURE_KEY;
    static constexpr const char *label = "Test fixture";
    static constexpr uint32_t    slots = FIXTURE_SLOTS;
    static constexpr uint32_t    caps  = 0;
    static constexpr uint32_t    post  = FIXTURE_POST;
#if defined(FIXTURE_BAD_PARAM)
    // An integer parameter whose maximum does not fit an integer.
    static constexpr orca_infill_param params[] = {
        { sizeof(orca_infill_param), "count", "Count", nullptr, nullptr, ORCA_INFILL_PARAM_INT, 1., 0., 1e19, nullptr, nullptr, 0 },
    };
#endif

    static void generate(const orca_infill::Request &req, orca_infill::Output &out)
    {
#if defined(FIXTURE_FAILS_ODD_LAYERS)
        if (req.layer_index() & 1)
            throw orca_infill::Failed();
#elif defined(FIXTURE_THROWS)
        throw std::runtime_error("fixture");
#elif defined(FIXTURE_TOP_ONLY)
        // Fills top surfaces only, so the role of a request shows in the output.
        if (req.role() != ORCA_INFILL_ROLE_TOP)
            throw orca_infill::Failed();
#endif
        // Parallel lines along the x axis at the distance for the density, laid out from the origin.
        const int64_t distance = req.units(req.line_distance());
        if (distance <= 0)
            throw orca_infill::Failed();
        const orca_infill_bbox          &box = req.bbox();
        std::vector<orca_infill::Points> lines;
        for (int64_t y = orca_infill::Request::align_up(box.min.y, req.origin().y, distance); y <= box.max.y; y += distance)
            lines.push_back({ { box.min.x, y }, { box.max.x, y } });
#if defined(FIXTURE_INSET)
        // The lines inside the region shrunk by 2 mm, cut by the host services; the host does not clip them.
        out.log(ORCA_INFILL_LOG_DEBUG, "inset by 2 mm");
        std::vector<orca_infill::Points> pieces;
        for (const orca_infill::Region &inset : out.offset(req.region(), -2.))
            for (orca_infill::Points &piece : out.clip(lines, inset))
                pieces.push_back(std::move(piece));
        lines = std::move(pieces);
#endif
        for (const orca_infill::Points &line : lines)
            out.polyline(line);
    }
};

} // namespace

ORCA_INFILL_PLUGIN("OrcaSlicer test fixture", "1.0.0", Fixture)

#endif
