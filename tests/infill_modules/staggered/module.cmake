# Staggered Lines: parallel lines that move by half their distance on every other layer.
orca_add_infill_module(
    NAME           staggered
    MODULE         StaggeredModule.cpp
    SOURCES        FillStaggered.cpp FillStaggered.hpp
    CORE           OFF
    DEFAULT        ON
    REGION_OPTIONS ConfigOptionFloat staggered_spacing_scale
                   ConfigOptionBool  staggered_cross)
