<#
.SYNOPSIS
  Launch OrcaSlicer's code under a GENUINE, Bambu-signed bambu-studio.exe so the
  network plugin's "signed studio" gate (enc_msg verify_image) is satisfied and
  control commands get signed.

.DESCRIPTION
  The packed network plugin validates the HOST process's in-memory PE Authenticode
  signature against the real loaded bytes -- an unsigned host (orca-slicer.exe) can
  never pass, and API-redirect hooks cannot forge it (proven). The only working path
  is to run the genuine signed bambu-studio.exe (genuine in memory) and have it load
  our forked code as BambuStudio.dll.

  The genuine bambu-studio.exe is a thin launcher: LoadLibrary("BambuStudio.dll") +
  GetProcAddress("bambustu_main") + call it. OrcaSlicer.dll now exports bambustu_main
  (alias of orcaslicer_main), so the genuine exe runs OrcaSlicer. The baked-in
  PluginVerifyRedirect hook redirects the plugin's Authenticode check of OUR
  BambuStudio.dll to the genuine official BambuStudio.dll.

  REQUIREMENT: a genuine, version-matched Bambu Studio installed (default
  C:\Program Files\Bambu Studio): provides the signed launcher exe AND the genuine
  BambuStudio.dll redirect target. Keep it matched to the staged plugin
  (%APPDATA%\OrcaSlicer\plugins\bambu_networking_<ver>.dll).

.PARAMETER OfficialDir
  Install dir of the genuine Bambu Studio. Default: C:\Program Files\Bambu Studio
#>
param(
  [string]$OfficialDir = 'C:\Program Files\Bambu Studio'
)

$ErrorActionPreference = 'Stop'

$ReleaseDir   = Join-Path $PSScriptRoot '..\build\OrcaSlicer' | Resolve-Path | Select-Object -ExpandProperty Path
$ourDll       = Join-Path $ReleaseDir 'OrcaSlicer.dll'
$stagedDll    = Join-Path $ReleaseDir 'BambuStudio.dll'
$officialExe  = Join-Path $OfficialDir 'bambu-studio.exe'
$officialDll  = Join-Path $OfficialDir 'BambuStudio.dll'
$genuineLnch  = Join-Path $ReleaseDir 'bambu-studio-genuine.exe'

foreach ($p in @($ourDll, $officialExe, $officialDll)) {
  if (-not (Test-Path $p)) { throw "missing required file: $p" }
}

# A folder literally named 'data_dir' next to the exe would override the data dir
# (GUI_App.cpp). Abort if present so we keep using %APPDATA%\OrcaSlicer + its plugin.
if (Test-Path (Join-Path $ReleaseDir 'data_dir') -PathType Container) {
  throw "a 'data_dir' folder exists in $ReleaseDir -- remove it (it would override the OrcaSlicer data dir)."
}

# the genuine launcher must be validly signed
$sig = Get-AuthenticodeSignature $officialExe
if ($sig.Status -ne 'Valid') { throw "official exe is not validly signed (Status=$($sig.Status)): $officialExe" }
$exeVer = (Get-Item $officialExe).VersionInfo.FileVersion
Write-Host "genuine launcher : $officialExe  ($exeVer, $($sig.Status))"
Write-Host "redirect target  : $officialDll"
Write-Host "our DLL          : $ourDll"

# stage our DLL under the name the genuine exe loads, and a local copy of the
# signed launcher (its own-dir DLL search resolves OUR BambuStudio.dll first).
Copy-Item $ourDll      $stagedDll   -Force
Copy-Item $officialExe $genuineLnch -Force
Write-Host "staged           : $stagedDll  +  $genuineLnch"

# Launch via ProcessStartInfo so env + working dir reliably reach the child.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName         = $genuineLnch
$psi.UseShellExecute  = $false
$psi.WorkingDirectory = $ReleaseDir
$psi.EnvironmentVariables['PJARCZAK_LINUX_BRIDGE_ENABLED'] = '0'   # in-process Windows plugin, not the WSL bridge
$psi.EnvironmentVariables['BAMBU_BRIDGE_GENUINE_DLL']      = $officialDll

$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host "launched PID=$($proc.Id)  (genuine host: bambu-studio-genuine.exe, WD=$ReleaseDir)"
