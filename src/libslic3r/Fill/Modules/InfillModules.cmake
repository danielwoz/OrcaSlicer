# Orca: infill pattern modules. Each directory Fill/Modules/<name>/, and each directory in ORCA_INFILL_MODULE_DIRS,
# holds a module.cmake that calls orca_add_infill_module() once. The generated infill_modules.cpp names every module
# object, so the static library keeps all of them without static constructors. docs/HLSD/infill-modules.md describes
# how to add a pattern; tests/infill_modules/staggered is a complete module.

option(ORCA_INFILL_OPTIONAL_DEFAULT "Initial value of the ORCA_INFILL_<NAME> options of the optional infill modules" ON)
set(ORCA_INFILL_MODULE_DIRS "" CACHE STRING "Further infill module directories holding a module.cmake, ';'-separated")
set(ORCA_INFILL_MODULES_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(ORCA_INFILL_REGION_OPTION_TYPES ConfigOptionFloat ConfigOptionPercent ConfigOptionFloatOrPercent
                                    ConfigOptionInt ConfigOptionBool ConfigOptionString)

# orca_add_infill_module(NAME <name> MODULE <source> [SOURCES <file>...] [CORE ON|OFF] [DEFAULT ON|OFF]
#                        [REGION_OPTIONS <ConfigOption type> <key> ...])
#   NAME            [a-z0-9]+. The module source defines ORCA_INFILL_MODULE(<name>, ...).
#   MODULE          Always compiled, so the patterns of a left-out module stay known.
#   SOURCES         Compiled only when the module is built. Paths are relative to the module directory. With CORE OFF,
#                   a Fill class compiled from them is created through optional_fill<ORCA_INFILL_<NAME> != 0, T>(),
#                   never &make_fill<T>, or a build that leaves the module out fails to link.
#   CORE            ON: always built. OFF (default): built when the option ORCA_INFILL_<NAME> is ON.
#   DEFAULT         Initial value of ORCA_INFILL_<NAME> (default ON), and-ed with ORCA_INFILL_OPTIONAL_DEFAULT.
#   REGION_OPTIONS  Members added to PrintRegionConfig, as type/key pairs; every key starts with "<name>_". The module
#                   source lists each key in its region_options and defines it in its define_region_options.
function(orca_add_infill_module)
    cmake_parse_arguments(PARSE_ARGV 0 M "" "NAME;MODULE;CORE;DEFAULT" "SOURCES;REGION_OPTIONS")
    # The module.cmake calling this function.
    set(_where ${CMAKE_CURRENT_LIST_FILE})
    if (NOT M_NAME MATCHES "^[a-z0-9]+$" OR NOT M_MODULE OR M_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${_where}: orca_add_infill_module() needs NAME [a-z0-9]+ and MODULE, and nothing else")
    endif ()
    get_property(_names GLOBAL PROPERTY ORCA_INFILL_MODULE_NAMES)
    if (M_NAME IN_LIST _names)
        message(FATAL_ERROR "${_where}: the infill module ${M_NAME} is defined twice")
    endif ()
    set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_MODULE_NAMES ${M_NAME})
    string(TOUPPER ${M_NAME} _upper)
    if (M_CORE)
        set(_built ON)
        set(_core true)
    else ()
        if (NOT DEFINED M_DEFAULT)
            set(M_DEFAULT ON)
        endif ()
        if (ORCA_INFILL_OPTIONAL_DEFAULT AND M_DEFAULT)
            set(_initial ON)
        else ()
            set(_initial OFF)
        endif ()
        option(ORCA_INFILL_${_upper} "Build the ${M_NAME} infill module" ${_initial})
        set(_built ${ORCA_INFILL_${_upper}})
        set(_core false)
        if (_built)
            set_property(GLOBAL APPEND_STRING PROPERTY ORCA_INFILL_PATTERN_DEFINES "#define ORCA_INFILL_${_upper} 1\n")
        else ()
            set_property(GLOBAL APPEND_STRING PROPERTY ORCA_INFILL_PATTERN_DEFINES "#define ORCA_INFILL_${_upper} 0\n")
            set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_LEFT_OUT ${_upper})
        endif ()
    endif ()
    get_filename_component(_module ${M_MODULE} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_LIST_DIR})
    set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_MODULE_SOURCES ${_module})
    if (_built)
        foreach (_source IN LISTS M_SOURCES)
            get_filename_component(_source ${_source} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_LIST_DIR})
            set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_MODULE_SOURCES ${_source})
        endforeach ()
        set(_built_cxx true)
    else ()
        set(_built_cxx false)
    endif ()
    set_property(GLOBAL APPEND_STRING PROPERTY ORCA_INFILL_MODULE_DECLARATIONS "extern const InfillModule infill_module_${M_NAME};\n")
    set_property(GLOBAL APPEND_STRING PROPERTY ORCA_INFILL_MODULE_ENTRIES "    { &infill_module_${M_NAME}, ${_core}, ${_built_cxx} },\n")
    # Region options exist whether or not the module is built, so configurations keep their values.
    list(LENGTH M_REGION_OPTIONS _count)
    math(EXPR _odd "${_count} % 2")
    if (_odd)
        message(FATAL_ERROR "${_where}: REGION_OPTIONS takes pairs of a type and a key")
    endif ()
    while (M_REGION_OPTIONS)
        list(GET M_REGION_OPTIONS 0 _type)
        list(GET M_REGION_OPTIONS 1 _key)
        list(REMOVE_AT M_REGION_OPTIONS 0 1)
        if (NOT _type IN_LIST ORCA_INFILL_REGION_OPTION_TYPES OR NOT _key MATCHES "^${M_NAME}_[a-z0-9_]+$")
            message(FATAL_ERROR "${_where}: ${_type} ${_key}: the type must be one of ${ORCA_INFILL_REGION_OPTION_TYPES}"
                                " and the key must start with ${M_NAME}_")
        endif ()
        set_property(GLOBAL APPEND_STRING PROPERTY ORCA_INFILL_MODULE_REGION_OPTIONS " ((${_type}, ${_key}))")
        set_property(GLOBAL APPEND_STRING PROPERTY ORCA_INFILL_MODULE_REGION_OPTION_KEYS " \"${_key}\",")
    endwhile ()
endfunction()

# Includes every module.cmake and writes the generated files. Appends the module sources below the libslic3r directory to
# <in_tree_var>, and sets <other_var> to the others plus the generated module list.
function(orca_add_infill_modules in_tree_var other_var)
    set(_properties ORCA_INFILL_MODULE_NAMES ORCA_INFILL_PATTERN_DEFINES ORCA_INFILL_LEFT_OUT ORCA_INFILL_MODULE_SOURCES
                    ORCA_INFILL_MODULE_DECLARATIONS ORCA_INFILL_MODULE_ENTRIES ORCA_INFILL_MODULE_REGION_OPTIONS
                    ORCA_INFILL_MODULE_REGION_OPTION_KEYS)
    # An empty property still sets its variable below, so a list that stays empty does not read a stale cache entry.
    foreach (_property IN LISTS _properties)
        set_property(GLOBAL PROPERTY ${_property} "")
    endforeach ()
    file(GLOB _module_files CONFIGURE_DEPENDS ${ORCA_INFILL_MODULES_CMAKE_DIR}/*/module.cmake)
    foreach (_dir IN LISTS ORCA_INFILL_MODULE_DIRS)
        get_filename_component(_dir ${_dir} ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
        if (NOT EXISTS ${_dir}/module.cmake)
            message(FATAL_ERROR "ORCA_INFILL_MODULE_DIRS: ${_dir} has no module.cmake")
        endif ()
        list(APPEND _module_files ${_dir}/module.cmake)
    endforeach ()
    foreach (_file IN LISTS _module_files)
        include(${_file})
    endforeach ()
    foreach (_property IN LISTS _properties)
        get_property(${_property} GLOBAL PROPERTY ${_property})
    endforeach ()
    configure_file(${ORCA_INFILL_MODULES_CMAKE_DIR}/infill_modules.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/infill_modules.cpp @ONLY)
    configure_file(${ORCA_INFILL_MODULES_CMAKE_DIR}/libslic3r_infill_patterns.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/libslic3r_infill_patterns.h @ONLY)
    configure_file(${ORCA_INFILL_MODULES_CMAKE_DIR}/libslic3r_infill_module_options.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/libslic3r_infill_module_options.h @ONLY)
    set(_in_tree "")
    set(_other ${CMAKE_CURRENT_BINARY_DIR}/infill_modules.cpp)
    foreach (_source IN LISTS ORCA_INFILL_MODULE_SOURCES)
        string(FIND "${_source}" "${CMAKE_CURRENT_SOURCE_DIR}/" _pos)
        if (_pos EQUAL 0)
            list(APPEND _in_tree ${_source})
        else ()
            list(APPEND _other ${_source})
        endif ()
    endforeach ()
    if (ORCA_INFILL_LEFT_OUT)
        message(STATUS "Infill modules left out of this build: ${ORCA_INFILL_LEFT_OUT}")
    endif ()
    # The CLI and test CMake files read these.
    set(ORCA_INFILL_LEFT_OUT "${ORCA_INFILL_LEFT_OUT}" CACHE INTERNAL "")
    set(ORCA_INFILL_MODULE_NAMES "${ORCA_INFILL_MODULE_NAMES}" CACHE INTERNAL "")
    set(${in_tree_var} ${${in_tree_var}} ${_in_tree} PARENT_SCOPE)
    set(${other_var} ${_other} PARENT_SCOPE)
endfunction()
