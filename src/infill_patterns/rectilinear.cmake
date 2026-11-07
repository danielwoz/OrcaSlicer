# Rows of the rectilinear family: the Fill classes of Fill/FillRectilinear.cpp. A row the build leaves out
# (ORCA_INFILL_<NAME>=OFF) has no library.
orca_add_builtin_infill_library(monotonic ROW ipMonotonic CLASS FillMonotonic HEADER libslic3r/Fill/FillRectilinear.hpp
                                OBJECTS orca_fill_rectilinear_obj NO_SORT)
orca_add_builtin_infill_library(monotonicline ROW ipMonotonicLine CLASS FillMonotonicLines HEADER libslic3r/Fill/FillRectilinear.hpp
                                OBJECTS orca_fill_rectilinear_obj NO_SORT)
orca_add_builtin_infill_library(rectilinear ROW ipRectilinear CLASS FillRectilinear HEADER libslic3r/Fill/FillRectilinear.hpp
                                OBJECTS orca_fill_rectilinear_obj)
orca_add_builtin_infill_library(alignedrectilinear ROW ipAlignedRectilinear CLASS FillAlignedRectilinear
                                HEADER libslic3r/Fill/FillRectilinear.hpp OBJECTS orca_fill_rectilinear_obj)
if (ORCA_INFILL_ZIGZAG)
    orca_add_builtin_infill_library(zigzag ROW ipZigZag CLASS FillZigZag HEADER libslic3r/Fill/FillRectilinear.hpp
                                    OBJECTS orca_fill_rectilinear_obj)
endif ()
if (ORCA_INFILL_CROSSZAG)
    orca_add_builtin_infill_library(crosszag ROW ipCrossZag CLASS FillCrossZag HEADER libslic3r/Fill/FillRectilinear.hpp
                                    OBJECTS orca_fill_rectilinear_obj)
endif ()
if (ORCA_INFILL_LOCKEDZAG)
    orca_add_builtin_infill_library(lockedzag ROW ipLockedZag CLASS FillLockedZag HEADER libslic3r/Fill/FillRectilinear.hpp
                                    OBJECTS orca_fill_rectilinear_obj CALLS FILL_SURFACE LOCKED_ZAG)
endif ()
orca_add_builtin_infill_library(grid ROW ipGrid CLASS FillGrid HEADER libslic3r/Fill/FillRectilinear.hpp
                                OBJECTS orca_fill_rectilinear_obj)
orca_add_builtin_infill_library(triangles ROW ipTriangles CLASS FillTriangles HEADER libslic3r/Fill/FillRectilinear.hpp
                                OBJECTS orca_fill_rectilinear_obj)
if (ORCA_INFILL_TRIHEXAGON)
    orca_add_builtin_infill_library(tri-hexagon ROW ipStars CLASS FillStars HEADER libslic3r/Fill/FillRectilinear.hpp
                                    OBJECTS orca_fill_rectilinear_obj)
endif ()
orca_add_builtin_infill_library(cubic ROW ipCubic CLASS FillCubic HEADER libslic3r/Fill/FillRectilinear.hpp
                                OBJECTS orca_fill_rectilinear_obj)
if (ORCA_INFILL_QUARTERCUBIC)
    orca_add_builtin_infill_library(quartercubic ROW ipQuarterCubic CLASS FillQuarterCubic HEADER libslic3r/Fill/FillRectilinear.hpp
                                    OBJECTS orca_fill_rectilinear_obj)
endif ()
if (ORCA_INFILL_LATERALHONEYCOMB)
    orca_add_builtin_infill_library(lateral-honeycomb ROW ipLateralHoneycomb CLASS FillLateralHoneycomb
                                    HEADER libslic3r/Fill/FillRectilinear.hpp OBJECTS orca_fill_rectilinear_obj)
endif ()
if (ORCA_INFILL_LATERALLATTICE)
    orca_add_builtin_infill_library(lateral-lattice ROW ipLateralLattice CLASS FillLateralLattice
                                    HEADER libslic3r/Fill/FillRectilinear.hpp OBJECTS orca_fill_rectilinear_obj)
endif ()
orca_add_builtin_infill_library(supportbase ROW ipSupportBase CLASS FillSupportBase HEADER libslic3r/Fill/FillRectilinear.hpp
                                OBJECTS orca_fill_rectilinear_obj INTERNAL)
