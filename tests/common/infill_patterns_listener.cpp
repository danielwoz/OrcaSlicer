// Loads the built-in infill pattern libraries before the first test, as the application does at start-up
// (libslic3r/Fill/InfillPatternLibraries.hpp). Compiled into every test executable through test_common; an executable
// that checks how slicing behaves without the libraries defines ORCA_TEST_INFILL_PATTERNS_UNLOADED.
#include <iostream>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "libslic3r/Fill/InfillPatternLibraries.hpp"

#include "infill_pattern_libraries.h"

#ifndef ORCA_TEST_INFILL_PATTERNS_UNLOADED
namespace {

class InfillPatternLibrariesListener : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo &) override
    {
        for (const Slic3r::BuiltinInfillLoad &load : Slic3r::load_builtin_infill_patterns({ ORCA_TEST_INFILL_PATTERN_DIR }))
            if (! load.error.empty())
                std::cerr << "infill pattern library " << load.file << ": " << load.error << std::endl;
    }
};

} // namespace
CATCH_REGISTER_LISTENER(InfillPatternLibrariesListener)
#endif
