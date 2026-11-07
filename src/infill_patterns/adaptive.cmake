# Rows of the adaptive cubic family: Fill/FillAdaptive.cpp. Both rows run FillAdaptive::Filler, and each library builds
# the octrees its row fills with.
orca_add_builtin_infill_library(adaptivecubic ROW ipAdaptiveCubic CLASS FillAdaptive::Filler HEADER libslic3r/Fill/FillAdaptive.hpp
                                OBJECTS orca_fill_adaptive_obj STATE OCTREE)
if (ORCA_INFILL_SUPPORTCUBIC)
    orca_add_builtin_infill_library(supportcubic ROW ipSupportCubic CLASS FillAdaptive::Filler HEADER libslic3r/Fill/FillAdaptive.hpp
                                    OBJECTS orca_fill_adaptive_obj STATE OCTREE)
endif ()
