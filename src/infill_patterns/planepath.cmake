# Rows of the plane path family: the Fill classes of Fill/FillPlanePath.cpp. A row the build leaves out
# (ORCA_INFILL_<NAME>=OFF) has no library.
if (ORCA_INFILL_HILBERTCURVE)
    orca_add_builtin_infill_library(hilbertcurve ROW ipHilbertCurve CLASS FillHilbertCurve HEADER libslic3r/Fill/FillPlanePath.hpp
                                    OBJECTS orca_fill_planepath_obj)
endif ()
orca_add_builtin_infill_library(archimedeanchords ROW ipArchimedeanChords CLASS FillArchimedeanChords
                                HEADER libslic3r/Fill/FillPlanePath.hpp OBJECTS orca_fill_planepath_obj)
if (ORCA_INFILL_OCTAGRAMSPIRAL)
    orca_add_builtin_infill_library(octagramspiral ROW ipOctagramSpiral CLASS FillOctagramSpiral
                                    HEADER libslic3r/Fill/FillPlanePath.hpp OBJECTS orca_fill_planepath_obj)
endif ()
