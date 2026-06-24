// Provides the nanosvg implementation symbols (nsvgParseFromFile / nsvgDelete /
// nsvgParse / nsvgRasterize ...) for the standalone sla_raster_bench executable.
//
// libslic3r references these symbols (svg.cpp / NSVGUtils.cpp) but the
// implementation is normally compiled into the GUI library (BitmapCache.cpp),
// which the benchmark deliberately does NOT link. nanosvg is header-only, so we
// instantiate it here exactly as the GUI does. Self-contained; no GUI needed.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
