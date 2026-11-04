#pragma once

#include <memory>

struct orca_infill_builtin_library;

namespace Slic3r {

// An object-level state of a built-in pattern, such as an octree or a lightning generator, built by the pattern
// library of its row with prepare_state() from inputs the print object gathers. The print object owns it; a filler of
// the row reads it through Fill::object_state.
struct InfillObjectState
{
    InfillObjectState(const orca_infill_builtin_library &library, void *raw) : library(&library), raw(raw) {}
    // Releases the state in its library, unless shutdown_builtin_infill_patterns() ran (Fill/InfillPatternLibraries.hpp):
    // after it the state is left to the process.
    ~InfillObjectState();
    InfillObjectState(const InfillObjectState &) = delete;
    InfillObjectState& operator=(const InfillObjectState &) = delete;

    const orca_infill_builtin_library *library;
    void                              *raw;
};

using InfillObjectStatePtr = std::shared_ptr<const InfillObjectState>;

} // namespace Slic3r
