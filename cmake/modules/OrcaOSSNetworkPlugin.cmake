# OrcaOSSNetworkPlugin.cmake
#
# Builds the open-source network plugin (bambu_network_oss submodule) against
# OrcaSlicer's legacy ABI and installs it as
#   resources/plugins/libbambu_networking_<legacy-version>.so
# so the app can load it instead of downloading the proprietary plugin.
#
# Gated by the ORCA_OSS_NETWORK_PLUGIN option (see top-level CMakeLists.txt).
# Defensive: if prerequisites (submodule, OpenSSL>=3, paho) are missing it warns
# and skips rather than breaking the main OrcaSlicer build.

include(ExternalProject)

set(_oss_src      "${CMAKE_SOURCE_DIR}/bambu_network_oss")
set(_oss_compat   "${_oss_src}/compat/orca_legacy")
# Legacy agent version OrcaSlicer expects (src/libslic3r/AppConfig.hpp).
set(ORCA_OSS_NETWORK_PLUGIN_VERSION "01.10.01.09" CACHE STRING
    "Version the OSS network plugin reports + filename suffix (must match BAMBU_NETWORK_AGENT_VERSION_LEGACY)")

# paho-mqtt-c (async ssl) built against OpenSSL 3 — not part of OrcaSlicer's deps;
# point these at a suitable build. Defaults to the BambuBridge deps tree on this host.
set(ORCA_OSS_PAHO_INCLUDE_DIR "/mnt/cephfs/ssd/BambuBridge/BambuStudio/deps/build/destdir/usr/local/include"
    CACHE PATH "paho-mqtt-c include dir for the OSS plugin")
set(ORCA_OSS_PAHO_LIBRARY "/mnt/cephfs/ssd/BambuBridge/BambuStudio/deps/build/destdir/usr/local/lib/libpaho-mqtt3as.a"
    CACHE FILEPATH "paho-mqtt3as static library for the OSS plugin")

set(_skip "")
if (NOT EXISTS "${_oss_compat}/slic3r/Utils/bambu_networking.hpp")
    set(_skip "submodule not initialized or compat shim missing at ${_oss_compat} (run: git submodule update --init)")
elseif (NOT EXISTS "${ORCA_OSS_PAHO_LIBRARY}")
    set(_skip "paho library not found at ${ORCA_OSS_PAHO_LIBRARY} (set -DORCA_OSS_PAHO_LIBRARY=...)")
endif()

if (_skip)
    message(WARNING "ORCA_OSS_NETWORK_PLUGIN: skipping OSS plugin build — ${_skip}")
    return()
endif()

# The OSS plugin links Boost statically to stay ABI-isolated from the host, and
# must use the SAME Boost as OrcaSlicer (its bundled 1.84, PIC). But OrcaSlicer's
# deps prefix also contains OpenSSL 1.1.1w, while the plugin requires OpenSSL >=3.
# Build a Boost-only prefix (symlinks) so only Boost is taken from the deps tree;
# OpenSSL/CURL then resolve from the system (3.x) toolchain.
set(_deps_prefix "${CMAKE_PREFIX_PATH}")
set(_boost_prefix "${CMAKE_BINARY_DIR}/oss_boost_prefix")
file(MAKE_DIRECTORY "${_boost_prefix}/lib/cmake" "${_boost_prefix}/lib" "${_boost_prefix}/include")
if (EXISTS "${_deps_prefix}/include/boost")
    file(CREATE_LINK "${_deps_prefix}/include/boost" "${_boost_prefix}/include/boost" SYMBOLIC)
    file(GLOB _boost_cmake_dirs LIST_DIRECTORIES true "${_deps_prefix}/lib/cmake/Boost*" "${_deps_prefix}/lib/cmake/boost_*")
    foreach(_d ${_boost_cmake_dirs})
        get_filename_component(_n "${_d}" NAME)
        file(CREATE_LINK "${_d}" "${_boost_prefix}/lib/cmake/${_n}" SYMBOLIC)
    endforeach()
    file(GLOB _boost_libs "${_deps_prefix}/lib/libboost_*.a")
    foreach(_l ${_boost_libs})
        get_filename_component(_n "${_l}" NAME)
        file(CREATE_LINK "${_l}" "${_boost_prefix}/lib/${_n}" SYMBOLIC)
    endforeach()
else()
    message(WARNING "ORCA_OSS_NETWORK_PLUGIN: Boost not found under ${_deps_prefix}; the OSS plugin build may pick up system Boost.")
endif()

set(_oss_binary "${CMAKE_BINARY_DIR}/bambu_network_oss-build")
set(_oss_so "${_oss_binary}/plugin/libbambu_networking.so.0.1.0")
set(_oss_installed "${CMAKE_SOURCE_DIR}/resources/plugins/libbambu_networking_${ORCA_OSS_NETWORK_PLUGIN_VERSION}.so")

ExternalProject_Add(bambu_network_oss_plugin
    SOURCE_DIR        "${_oss_src}"
    BINARY_DIR        "${_oss_binary}"
    CMAKE_GENERATOR   "Ninja"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DBAMBU_NET_OSS_BUILD_PLUGIN=ON
        -DBAMBU_NET_OSS_BUILD_STANDALONE=OFF
        -DBAMBU_NET_OSS_BUILD_TESTS=OFF
        -DBAMBU_STUDIO_SRC=${_oss_compat}
        -DBAMBU_NET_OSS_AGENT_VERSION=${ORCA_OSS_NETWORK_PLUGIN_VERSION}
        -DCMAKE_PREFIX_PATH=${_boost_prefix}
        -DPAHO_MQTT_C_INCLUDE_DIR=${ORCA_OSS_PAHO_INCLUDE_DIR}
        -DPAHO_MQTT_C_LIBRARY=${ORCA_OSS_PAHO_LIBRARY}
    BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR> --target bambu_networking
    BUILD_BYPRODUCTS  "${_oss_so}"
    # Install the versioned .so into resources/plugins so the app bundles it.
    INSTALL_COMMAND   ${CMAKE_COMMAND} -E copy_if_different "${_oss_so}" "${_oss_installed}"
    BUILD_ALWAYS      OFF
)

message(STATUS "ORCA_OSS_NETWORK_PLUGIN: will build OSS plugin v${ORCA_OSS_NETWORK_PLUGIN_VERSION} → ${_oss_installed}")
