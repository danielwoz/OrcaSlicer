#pragma once

#include <memory>
#include <utility>
#include <vector>

struct orca_infill_builtin_library;

namespace Slic3r {

enum InfillPattern : int;
enum class AdaptiveInfillMode;

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

// The object states of a print object, for the rows served by pattern libraries.
struct InfillObjectStates
{
    // The adaptive infill field of each row and adaptive_infill_mode the regions slice; a field that came out empty is
    // left out, so that the row lays its regular pattern.
    std::vector<std::pair<std::pair<InfillPattern, AdaptiveInfillMode>, InfillObjectStatePtr>> fields;

    // The field of row in mode, nullptr when there is none.
    const InfillObjectState* field(InfillPattern row, AdaptiveInfillMode mode) const
    {
        for (const auto &[key, state] : fields)
            if (key.first == row && key.second == mode)
                return state.get();
        return nullptr;
    }
};

} // namespace Slic3r
