orca_add_infill_module(NAME concentric MODULE ConcentricModule.cpp CORE ON
                       SOURCES ${ORCA_FILL_DIR}/FillConcentric.cpp ${ORCA_FILL_DIR}/FillConcentric.hpp
                               ${ORCA_FILL_DIR}/FillConcentricInternal.cpp ${ORCA_FILL_DIR}/FillConcentricInternal.hpp)
