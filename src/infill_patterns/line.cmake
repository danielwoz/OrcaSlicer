# Rows of the line family: Fill/FillLine.cpp. A build that leaves the row out (ORCA_INFILL_LINE=OFF) has no library.
if (ORCA_INFILL_LINE)
    orca_add_builtin_infill_library(line ROW ipLine CLASS FillLine HEADER libslic3r/Fill/FillLine.hpp OBJECTS orca_fill_line_obj)
endif ()
