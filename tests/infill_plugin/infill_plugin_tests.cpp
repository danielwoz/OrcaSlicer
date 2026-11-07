// libslic3r leaves the nanosvg implementation to the executable.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "libslic3r/Fill/InfillPatternLibraries.hpp"
#include "libslic3r/Fill/InfillPlugins.hpp"

#include "infill_pattern_libraries.h"
#include "infill_plugin_paths.h"

// What loading the sample plugin before the first test reported.
Slic3r::InfillPluginLoad &sample_plugin_load()
{
    static Slic3r::InfillPluginLoad load;
    return load;
}

// Loads the sample plugin before the first test, like the application at start-up: after the built-in pattern
// libraries, whichever listener runs first.
class InfillPluginListener : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo &) override
    {
        Slic3r::load_builtin_infill_patterns({ ORCA_TEST_INFILL_PATTERN_DIR });
        sample_plugin_load() = Slic3r::load_infill_plugin(SAMPLE_INFILL_PLUGIN);
    }
};
CATCH_REGISTER_LISTENER(InfillPluginListener)
