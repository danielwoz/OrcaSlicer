# Rows of the honeycomb family: Fill/FillHoneycomb.cpp and Fill/Fill3DHoneycomb.cpp.
orca_add_builtin_infill_library(honeycomb ROW ipHoneycomb CLASS FillHoneycomb HEADER libslic3r/Fill/FillHoneycomb.hpp
                                OBJECTS orca_fill_honeycomb_obj)
orca_add_builtin_infill_library(3dhoneycomb ROW ip3DHoneycomb CLASS Fill3DHoneycomb HEADER libslic3r/Fill/Fill3DHoneycomb.hpp
                                OBJECTS orca_fill_3dhoneycomb_obj)
