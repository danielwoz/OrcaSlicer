# Rows of the concentric family: Fill/FillConcentric.cpp, Fill/FillSpiralInset.cpp and Fill/FillConcentricInternal.cpp.
# Concentric and spiral inset also generate the variable width loops of Arachne; concentric internal generates only
# those, in the narrow parts make_fills() finds in internal solid surfaces.
orca_add_builtin_infill_library(concentric ROW ipConcentric CLASS FillConcentric HEADER libslic3r/Fill/FillConcentric.hpp
                                OBJECTS orca_fill_concentric_obj CALLS FILL_SURFACE FILL_ARACHNE NO_SORT)
orca_add_builtin_infill_library(spiralinset ROW ipSpiralInset CLASS FillSpiralInset HEADER libslic3r/Fill/FillSpiralInset.hpp
                                OBJECTS orca_fill_spiralinset_obj CALLS FILL_SURFACE FILL_ARACHNE NO_SORT)
orca_add_builtin_infill_library(concentricinternal ROW ipConcentricInternal CLASS FillConcentricInternal
                                HEADER libslic3r/Fill/FillConcentricInternal.hpp OBJECTS orca_fill_concentric_internal_obj
                                CALLS CONCENTRIC_INTERNAL NO_SORT INTERNAL)
