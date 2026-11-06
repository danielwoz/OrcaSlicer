# Rows of the lightning family: Fill/FillLightning.cpp and Fill/Lightning/, whose distance fields sample the overhangs
# with sample_grid_pattern() of Fill/FillRectilinear.cpp. The library builds the generator its row draws the trees of,
# and the generator of the lightning support base of tree support, whose trees it answers as lines.
orca_add_builtin_infill_library(lightning ROW ipLightning CLASS FillLightning::Filler HEADER libslic3r/Fill/FillLightning.hpp
                                OBJECTS orca_fill_lightning_obj orca_fill_rectilinear_obj STATE LIGHTNING LIGHTNING_SUPPORT)
