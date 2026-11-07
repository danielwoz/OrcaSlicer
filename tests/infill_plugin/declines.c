/* A plugin written in C, so the ABI header is compiled as C; it declines every host. */
#include "orca_infill_plugin.h"

ORCA_INFILL_EXPORT const orca_infill_plugin *ORCA_INFILL_CALL orca_infill_plugin_entry_v1(const orca_infill_host *host)
{
    (void) host;
    return NULL;
}
