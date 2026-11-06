// The table of the pattern library entries linked into an executable (orca_infill_patterns_linked): one entry per
// library of the build, under the name orca_add_builtin_infill_library() gave its linked copy.
#include "libslic3r/Fill/InfillPatternLibraries.hpp"
#include "libslic3r/Fill/Library/orca_infill_builtin.h"

#include "orca_infill_manifest.h"

#define ORCA_INFILL_LINKED_ENTRY(row, token, name, file, linked) \
    extern "C" const orca_infill_builtin_library *ORCA_INFILL_CALL linked(const orca_infill_builtin_host *host);
ORCA_INFILL_MANIFEST(ORCA_INFILL_LINKED_ENTRY)
#undef ORCA_INFILL_LINKED_ENTRY

namespace Slic3r {

const std::vector<LinkedBuiltinInfillLibrary>& linked_builtin_infill_patterns()
{
#define ORCA_INFILL_LINKED_ENTRY(row, token, name, file, linked) { row, name, &linked },
    static const std::vector<LinkedBuiltinInfillLibrary> libraries { ORCA_INFILL_MANIFEST(ORCA_INFILL_LINKED_ENTRY) };
#undef ORCA_INFILL_LINKED_ENTRY
    return libraries;
}

} // namespace Slic3r
