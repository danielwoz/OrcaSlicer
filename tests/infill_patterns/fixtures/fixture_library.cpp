// Pattern libraries that each break one rule of the loader, built from the entry template (runtime/PatternEntry.hpp) and
// linked as the libraries of src/infill_patterns are. FIXTURE_<NAME> selects the rule (test_builtin_binding.cpp). They
// describe the rectilinear row with a filler that returns the contour of the surface, or throws.
#include <cstdint>
#include <stdexcept>

#include "PatternEntry.hpp"
#include "orca_infill_build_id.h"

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Surface.hpp"

namespace {

class FixtureFill : public Slic3r::Fill
{
public:
    Fill* clone() const override { return new FixtureFill(*this); }
    bool  is_self_crossing() override { return false; }

    Slic3r::Polylines fill_surface(const Slic3r::Surface *surface, const Slic3r::FillParams &) override
    {
#ifdef FIXTURE_THROWS_IN_RUN
        (void) surface;
        throw std::runtime_error("fixture failure");
#else
        Slic3r::Polyline line;
        line.points = surface->expolygon.contour.points;
        return { line };
#endif
    }
};

struct Row
{
    using Filler = FixtureFill;
#if defined(FIXTURE_WRONG_ROW)
    static constexpr int32_t     pattern = int32_t(Slic3r::ipCrossHatch);
    static constexpr const char *key     = "crosshatch";
#elif defined(FIXTURE_WRONG_TOKEN)
    static constexpr int32_t     pattern = int32_t(Slic3r::ipRectilinear);
    static constexpr const char *key     = "rectilinear-fixture";
#else
    static constexpr int32_t     pattern = int32_t(Slic3r::ipRectilinear);
    static constexpr const char *key     = "rectilinear";
#endif
    static constexpr const char *name = "orca_infill_rectilinear";
#ifdef FIXTURE_BAD_BUILD_ID
    static constexpr const char *build_id = "another build";
#else
    static constexpr const char *build_id = ORCA_INFILL_BUILD_ID;
#endif
#ifdef FIXTURE_CALLS_MISMATCH
    static constexpr uint32_t calls = (1u << ORCA_INFILL_CALL_FILL_SURFACE) | (1u << ORCA_INFILL_CALL_FILL_ARACHNE);
#else
    static constexpr uint32_t calls = 1u << ORCA_INFILL_CALL_FILL_SURFACE;
#endif
#ifdef FIXTURE_TRAIT_MISMATCH
    static constexpr uint32_t traits = ORCA_INFILL_TRAIT_NO_SORT;
#else
    static constexpr uint32_t traits = 0;
#endif
    static constexpr uint32_t state_kinds = 0;
};

} // namespace

#ifdef FIXTURE_NO_ENTRY
#define FIXTURE_ENTRY orca_infill_fixture_entry
#else
#define FIXTURE_ENTRY orca_infill_builtin_entry_v1
#endif

extern "C" ORCA_INFILL_EXPORT const orca_infill_builtin_library *ORCA_INFILL_CALL FIXTURE_ENTRY(const orca_infill_builtin_host *host)
{
    return Slic3r::PatternLibrary::PatternEntry<Row>::bind(host);
}

#ifdef FIXTURE_ALSO_PLUGIN_ENTRY
extern "C" ORCA_INFILL_EXPORT const void *ORCA_INFILL_CALL orca_infill_plugin_entry_v1(const void *) { return nullptr; }
#endif
