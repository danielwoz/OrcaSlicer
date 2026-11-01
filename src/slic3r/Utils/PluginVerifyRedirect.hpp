#ifndef slic3r_PluginVerifyRedirect_hpp_
#define slic3r_PluginVerifyRedirect_hpp_

namespace Slic3r {

// Windows-only. When running a forked/unsigned BambuStudio under a GENUINE,
// version-matched bambu-studio.exe launcher, the network plugin's "signed
// studio" gate still verifies *this* BambuStudio.dll via WinVerifyTrust and
// rejects it (unsigned). This installs in-process inline hooks (MinHook) so the
// plugin's Authenticode verification of our DLL resolves to the genuine official
// BambuStudio.dll instead, satisfying the gate so control commands get signed.
//
// Installed only in MSVC x64 builds with ORCA_PLUGIN_VERIFY_REDIRECT; a no-op
// otherwise (other platforms, ARM64, or the option off). The genuine DLL is
// BambuStudioOriginal.dll next to our BambuStudio.dll when present, else the
// one named by env BAMBU_BRIDGE_GENUINE_DLL or found at the standard install
// path. Nothing is hooked if no genuine file is found.
//
// MUST be called before the network plugin is loaded.
void install_plugin_verify_redirect();

} // namespace Slic3r

#endif
