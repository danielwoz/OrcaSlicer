#pragma once

#include <cstddef>
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

// Orca: One octree per body (see Layer::lslices_separated_component_ids), and one of the whole object
// for objects of a single body or with a body that has none of its own. The library of row builds them.
struct Octrees
{
    InfillPattern                     row;
    InfillObjectStatePtr              object;
    std::vector<InfillObjectStatePtr> bodies;

    // A body without an octree, or body -1, uses the object's, or any body's when the object has none.
    const InfillObjectState* get(int body) const
    {
        if (body >= 0 && size_t(body) < bodies.size() && bodies[body])
            return bodies[body].get();
        if (object)
            return object.get();
        for (const InfillObjectStatePtr &octree : bodies)
            if (octree)
                return octree.get();
        return nullptr;
    }
};

// Orca: The octrees of each line spacing the regions of an object fill with.
struct RegionOctrees
{
    std::vector<Octrees> sets;
    // Index into sets for each region, -1 for a region without adaptive or support cubic infill.
    std::vector<int>     region_set;

    // The octrees a filler of row fills region_id with, nullptr when the region has none of that row.
    const Octrees *region(size_t region_id, InfillPattern row) const
    {
        if (region_id >= region_set.size() || region_set[region_id] < 0)
            return nullptr;
        const Octrees &set = sets[region_set[region_id]];
        return set.row == row ? &set : nullptr;
    }
};

// The object states of a print object, for the rows served by pattern libraries.
struct InfillObjectStates
{
    // The octrees of adaptive and support cubic infill.
    RegionOctrees octrees;
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
