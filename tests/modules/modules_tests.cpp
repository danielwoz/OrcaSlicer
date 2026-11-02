// libslic3r leaves the nanosvg implementation to the executable.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "libslic3r/Module/ModuleLoader.hpp"

#include "module_paths.h"

// What loading the modules before the first test reported, in load order.
std::vector<Slic3r::ModuleLoad> &startup_loads()
{
    static std::vector<Slic3r::ModuleLoad> loads;
    return loads;
}

// Loads the modules every test may use before the first test, like the application at start-up.
class ModuleListener : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo &) override
    {
        for (const char *path : { SAMPLE_LINES_MODULE, SAMPLE_INFILL_WALLS, SAMPLE_CHANGE_AT_Z, FIXTURE_OPTIONS })
            startup_loads().emplace_back(Slic3r::load_module(path));
    }
};
CATCH_REGISTER_LISTENER(ModuleListener)
