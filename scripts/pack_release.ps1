# Build a drag-and-drop zip: extract / copy into the Black Mesa folder (next to bms.exe).
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dll = Join-Path $Root "build\Release\d3d9.dll"
if (-not (Test-Path $Dll)) { throw "Build d3d9.dll first (scripts\build.ps1)" }

$OpenVrSrc = Join-Path $Root "third_party\openvr\lib\win32\openvr_api.dll"
$SteamVrOvr = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win32\openvr_api.dll"
# Prefer SteamVR's 32-bit OpenVR next to the DLL (AGENTS.md). Repo copy is fallback.
if (Test-Path $SteamVrOvr) { $OpenVrSrc = $SteamVrOvr }
if (-not (Test-Path $OpenVrSrc)) { throw "openvr_api.dll not found (SteamVR win32 or third_party/openvr)" }

$VrSrc = Join-Path $Root "VR"
$ReadmeSrc = Join-Path $Root "packaging\BMVR_README.txt"
if (-not (Test-Path $VrSrc)) { throw "Missing $VrSrc" }
if (-not (Test-Path $ReadmeSrc)) { throw "Missing $ReadmeSrc" }

$Dist = Join-Path $Root "dist"
$Stage = Join-Path $Dist "drop-into-Black-Mesa-folder"
$TestNotes = Join-Path $Root "packaging\TEST_RELEASE.txt"
$ZipName = "Black-Mesa-VR-drop-in.zip"
$Zip = Join-Path $Dist $ZipName
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
if (Test-Path $Zip) { Remove-Item -Force $Zip }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

Copy-Item -Force $ReadmeSrc (Join-Path $Stage "BMVR_README.txt")
if (Test-Path $TestNotes) {
  Copy-Item -Force $TestNotes (Join-Path $Stage "TEST_RELEASE.txt")
}
$DisableSrc = Join-Path $Root "packaging\DISABLE-SOURCEVR.bat"
if (Test-Path $DisableSrc) {
  Copy-Item -Force $DisableSrc (Join-Path $Stage "DISABLE-SOURCEVR.bat")
}
foreach ($unName in @("UNINSTALL-BMVR.bat", "UNINSTALL-BMVR.ps1")) {
  $unSrc = Join-Path $Root "scripts\$unName"
  if (-not (Test-Path $unSrc)) { throw "Missing $unSrc" }
  Copy-Item -Force $unSrc (Join-Path $Stage $unName)
}
Copy-Item -Force $Dll (Join-Path $Stage "d3d9.dll")
Copy-Item -Force $OpenVrSrc (Join-Path $Stage "openvr_api.dll")

$Bin = Join-Path $Stage "bin"
$Dxvk = Join-Path $Bin "thirdparty\dxvk-windows-x86"
New-Item -ItemType Directory -Force -Path $Dxvk | Out-Null
Copy-Item -Force $Dll (Join-Path $Bin "d3d9.dll")
Copy-Item -Force $OpenVrSrc (Join-Path $Bin "openvr_api.dll")
Copy-Item -Force $Dll (Join-Path $Dxvk "d3d9.dll")
Copy-Item -Force $OpenVrSrc (Join-Path $Dxvk "openvr_api.dll")
@"
d3d9.deferSurfaceCreation = True
d3d9.deviceLossOnFocusLoss = False
"@ | Set-Content -LiteralPath (Join-Path $Dxvk "dxvk.conf") -Encoding ASCII

$VrDst = Join-Path $Stage "VR"
New-Item -ItemType Directory -Force -Path $VrDst | Out-Null
Copy-Item -Force (Join-Path $VrSrc "config.txt") (Join-Path $VrDst "config.txt")
$OffSrc = Join-Path $VrSrc "viewmodel_offsets.txt"
if (Test-Path $OffSrc) {
  Copy-Item -Force $OffSrc (Join-Path $VrDst "viewmodel_offsets.txt")
}
Copy-Item -Recurse -Force (Join-Path $VrSrc "SteamVRActionManifest") (Join-Path $VrDst "SteamVRActionManifest")
$HandsSrc = Join-Path $VrSrc "hands"
if (Test-Path $HandsSrc) {
  $HandsDst = Join-Path $VrDst "hands"
  New-Item -ItemType Directory -Force -Path $HandsDst | Out-Null
  Copy-Item -Force (Join-Path $HandsSrc "*.glb") $HandsDst
}
$WheelSrc = Join-Path $VrSrc "weapon_wheel"
if (Test-Path $WheelSrc) {
  $WheelDst = Join-Path $VrDst "weapon_wheel"
  New-Item -ItemType Directory -Force -Path $WheelDst | Out-Null
  Copy-Item -Force (Join-Path $WheelSrc "*.vtf") $WheelDst
}
$HelperSrc = Join-Path $Root "build\Release\openxr_helper64"
if (Test-Path (Join-Path $HelperSrc "OpenXRHelper64.exe")) {
  $HelperDst = Join-Path $VrDst "openxr_helper64"
  New-Item -ItemType Directory -Force -Path $HelperDst | Out-Null
  Copy-Item -Force (Join-Path $HelperSrc "OpenXRHelper64.exe") $HelperDst
  $Loader = Join-Path $HelperSrc "openxr_loader.dll"
  if (Test-Path $Loader) {
    Copy-Item -Force $Loader $HelperDst
  }
}
$BmvrCfgSrc = Join-Path $VrSrc "bmvr.cfg"
if (Test-Path $BmvrCfgSrc) {
  $CfgGame = Join-Path $Stage "bms\cfg"
  New-Item -ItemType Directory -Force -Path $CfgGame | Out-Null
  Copy-Item -Force $BmvrCfgSrc (Join-Path $CfgGame "bmvr.cfg")
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($Stage, $Zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$dllBytes = (Get-Item $Dll).Length
$zipBytes = (Get-Item $Zip).Length
Write-Host ("Packed {0} ({1} bytes, d3d9.dll {2} bytes)" -f $Zip, $zipBytes, $dllBytes)
Write-Host "Copy the contents of drop-into-Black-Mesa-folder into Steam\\steamapps\\common\\Black Mesa"
Write-Host "Or extract the zip and copy that same folder's contents."
