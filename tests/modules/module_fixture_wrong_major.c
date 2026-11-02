/* A module of a future ABI that does not check the host's version. Written in C, so the module ABI header is compiled
   as C. */
#include "orca_module.h"

ORCA_INFILL_EXPORT const orca_module *ORCA_INFILL_CALL orca_module_entry_v1(const orca_module_host *host)
{
    static const orca_module module = { sizeof(orca_module), 2, 0, "fixture_major", "Fixture", "1.0.0", NULL, 0, NULL, 0,
                                        NULL, NULL, NULL };
    (void) host;
    return &module;
}
