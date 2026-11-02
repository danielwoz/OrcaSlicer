# Builds an OrcaSlicer module: an infill plugin target (OrcaInfillPlugin.cmake) that also sees the module headers.
#   include(<OrcaSlicer>/sdk/modules/OrcaModule.cmake)
#   orca_add_module(my_module my_module.cpp)
include(${CMAKE_CURRENT_LIST_DIR}/../infill/OrcaInfillPlugin.cmake)
set(ORCA_MODULE_SDK_DIR ${CMAKE_CURRENT_LIST_DIR})

function(orca_add_module target)
    orca_add_infill_plugin(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${ORCA_MODULE_SDK_DIR}/include)
endfunction()
