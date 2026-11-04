# Rows of the TPMS family: Fill/FillGyroid.cpp, Fill/FillTpmsD.cpp and Fill/FillTpmsFK.cpp, with Fill/AdaptiveInfillField.cpp.
# Each library builds the adaptive infill field its row is graded by.
orca_add_builtin_infill_library(gyroid ROW ipGyroid CLASS FillGyroid HEADER libslic3r/Fill/FillGyroid.hpp
                                OBJECTS orca_fill_gyroid_obj orca_fill_adaptive_field_obj STATE ADAPTIVE_FIELD)
if (ORCA_INFILL_TPMSD)
    orca_add_builtin_infill_library(tpmsd ROW ipTpmsD CLASS FillTpmsD HEADER libslic3r/Fill/FillTpmsD.hpp
                                    OBJECTS orca_fill_tpmsd_obj orca_fill_adaptive_field_obj STATE ADAPTIVE_FIELD)
endif ()
if (ORCA_INFILL_TPMSFK)
    orca_add_builtin_infill_library(tpmsfk ROW ipTpmsFK CLASS FillTpmsFK HEADER libslic3r/Fill/FillTpmsFK.hpp
                                    OBJECTS orca_fill_tpmsfk_obj orca_fill_adaptive_field_obj STATE ADAPTIVE_FIELD)
endif ()
