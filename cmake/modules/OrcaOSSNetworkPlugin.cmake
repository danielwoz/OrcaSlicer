# OrcaOSSNetworkPlugin.cmake
#
# The open-source network plugin (open-bamboo-networking, the bambu_network_oss
# submodule) is BUILT IN CI per platform and STAGED into resources/plugins/
# BEFORE OrcaSlicer is configured (see .github/workflows/build_orca.yml).
# OrcaSlicer's top-level install(DIRECTORY resources/ ...) then bundles whatever
# is in resources/plugins/ into the AppImage (Linux), the NSIS installer
# (Windows) and the app bundle (macOS).
#
# This module therefore does NOT build the plugin. It only:
#   1. Sets ORCA_OSS_NETWORK_PLUGIN_VERSION (the version the plugin reports
#      and the suffix in its staged filename). This MUST stay in lockstep with:
#         - the --with-version / -WithVersion passed to the plugin's configure
#           in CI (the plugin derives its ABI from the first three components),
#           and
#         - a NetworkAbi::Current row of AVAILABLE_NETWORK_VERSIONS
#           (src/slic3r/Utils/bambu_networking.hpp) for its AA.BB.CC series;
#           tests/slic3rutils/test_network_versions.cpp checks this.
#   2. Checks whether a matching plugin is staged in resources/plugins/. Only
#      then are the ORCA_OSS_NETWORK_PLUGIN and ORCA_OSS_NETWORK_PLUGIN_VERSION
#      compile definitions set, which compile in the bundled-plugin code and
#      leave out the proprietary-plugin download. Without a staged plugin the
#      module warns, and the build keeps the proprietary-plugin download, so a
#      build that bundles no plugin still has a network plugin.
#
# vcpkg note: the plugin's Windows build needs vcpkg; OrcaSlicer's Windows
# build does NOT use vcpkg (it uses the deps/ ExternalProject tree). The two
# never meet because the plugin is built in its own CI step and only the
# finished DLLs are copied here. Do not ExternalProject-build the plugin inside
# this tree on Windows.

# Version the OSS network plugin reports + staged filename suffix (network ABI series 02.08.04; the .99 build marks
# the open-source plugin). A plain variable, so a reconfigured build tree always takes the current lockstep version.
set(ORCA_OSS_NETWORK_PLUGIN_VERSION "02.08.04.99")

# Expected staged main-plugin filename per platform. The plugin's orca_slicer
# install names the shared object after the full version; at startup
# ensure_oss_network_plugin() installs it under that version with an "-oss"
# suffix. BambuSource / live555 keep their fixed names.
if (WIN32)
    set(_oss_staged_main "bambu_networking_${ORCA_OSS_NETWORK_PLUGIN_VERSION}.dll")
elseif (APPLE)
    set(_oss_staged_main "libbambu_networking_${ORCA_OSS_NETWORK_PLUGIN_VERSION}.dylib")
else()
    set(_oss_staged_main "libbambu_networking_${ORCA_OSS_NETWORK_PLUGIN_VERSION}.so")
endif()

set(_oss_staged_path "${CMAKE_SOURCE_DIR}/resources/plugins/${_oss_staged_main}")

if (EXISTS "${_oss_staged_path}")
    message(STATUS "ORCA_OSS_NETWORK_PLUGIN: found pre-staged plugin v${ORCA_OSS_NETWORK_PLUGIN_VERSION} at ${_oss_staged_path} (will be bundled via resources/).")
    # Used by GUI_App (provisioning + load) and PresetUpdater (download bypass).
    add_compile_definitions(ORCA_OSS_NETWORK_PLUGIN ORCA_OSS_NETWORK_PLUGIN_VERSION="${ORCA_OSS_NETWORK_PLUGIN_VERSION}")
else()
    message(WARNING "ORCA_OSS_NETWORK_PLUGIN: no pre-staged plugin at ${_oss_staged_path}. The OrcaSlicer package will NOT bundle the OSS network plugin and keeps the proprietary network plugin download. CI stages it before configuring; for a local build, build open-bamboo-networking (--client-type=orca_slicer --with-version=${ORCA_OSS_NETWORK_PLUGIN_VERSION}), copy plugins/* into resources/plugins/ and configure again.")
endif()
