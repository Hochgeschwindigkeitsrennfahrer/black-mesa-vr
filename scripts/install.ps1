# Install the combined d3d9.dll where Black Mesa actually loads it.
# Close the game first — a running bms.exe keeps the old mapping.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dll = Join-Path $Root "build\Release\d3d9.dll"
if (-not (Test-Path $Dll)) { throw "Build d3d9.dll first (scripts\build.ps1)" }

$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
$DxvkDir = Join-Path $GameRoot "bin\thirdparty\dxvk-windows-x86"
$BinDir = Join-Path $GameRoot "bin"
$OpenVrSrc = Join-Path $Root "third_party\openvr\lib\win32\openvr_api.dll"
if (-not (Test-Path $OpenVrSrc)) {
  $OpenVrSrc = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win32\openvr_api.dll"
}

$running = @(Get-Process -Name "bms" -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
  throw "Black Mesa is running (PID $($running.Id -join ', ')). Close it from Steam (do not kill it) so d3d9.dll can be replaced. Force-stopping mid-stereo blacks the HWND and the HMD."
}

$helpers = @(Get-Process -Name "OpenXRHelper64" -ErrorAction SilentlyContinue)
if ($helpers.Count -gt 0) {
  throw "OpenXR helper is still running (PID $($helpers.Id -join ', ')). Close Black Mesa first, then install."
}

# Killing bms.exe mid-stereo leaves bmvr_in_hmd_world.flag next to the DLL.
# The next launch then skipped WorldRenderAtEyeSize (soft world + gun-nod).
$flagDirs = @($GameRoot, $BinDir, $DxvkDir)
$staleFlags = @(
  'bmvr_in_hmd_world.flag',
  'bmvr_in_hmd_offscreen.flag',
  'bmvr_in_hl2vr_ham.flag',
  'bmvr_in_hl2vr_pixelvis.flag',
  'bmvr_in_hl2vr_predraw.flag',
  'bmvr_in_hl2vr_playercull.flag',
  'bmvr_in_hl2vr_proj.flag',
  'bmvr_in_hl2vr_cproj.flag',
  'bmvr_in_hl2vr_neye.flag',
  'bmvr_in_stereo_vis.flag',
  'bmvr_in_ctrl_vm.flag'
)
foreach ($dir in $flagDirs) {
  if (-not (Test-Path $dir)) { continue }
  foreach ($name in $staleFlags) {
    $p = Join-Path $dir $name
    if (Test-Path -LiteralPath $p) {
      Remove-Item -LiteralPath $p -Force
      Write-Host "Removed leftover $p"
    }
  }
}

if (-not (Test-Path $DxvkDir)) { throw "DXVK folder missing: $DxvkDir" }

function Install-One([string]$DestDir) {
  if (-not (Test-Path $DestDir)) { New-Item -ItemType Directory -Force -Path $DestDir | Out-Null }
  $dest = Join-Path $DestDir "d3d9.dll"
  $backup = Join-Path $DestDir "d3d9.dll.stock-dxvk"
  if ($DestDir -eq $DxvkDir -and (Test-Path $dest) -and -not (Test-Path $backup)) {
    Copy-Item -Force $dest $backup
    Write-Host "Backed up stock DXVK to $backup"
  }
  try {
    Copy-Item -Force $Dll $dest
  } catch {
    $locked = Join-Path $DestDir ("d3d9.dll.hung" + (Get-Date -Format "HHmmss"))
    try {
      Rename-Item -LiteralPath $dest -NewName (Split-Path $locked -Leaf)
      Copy-Item -Force $Dll $dest
      Write-Host ("Renamed locked {0} then copied new DLL" -f $dest)
    } catch {
      throw ("Could not overwrite {0}. Close every hung bms.exe and retry. {1}" -f $dest, $_.Exception.Message)
    }
  }
  $want = (Get-Item $Dll).Length
  $got = (Get-Item $dest).Length
  if ($got -ne $want) {
    throw ("Size mismatch after copy: {0} is {1} bytes, build is {2}." -f $dest, $got, $want)
  }
  if (Test-Path $OpenVrSrc) {
    Copy-Item -Force $OpenVrSrc (Join-Path $DestDir "openvr_api.dll")
  }
  Write-Host ("Installed {0} ({1} bytes)" -f $dest, $got)
}

# HL2VR physical crowbar (models + materials). Not vendored (~33MB of VTFs).
function Install-Hl2vrCrowbar {
  $hlvr = "C:\Program Files (x86)\Steam\steamapps\common\Half-Life 2 VR\hlvr"
  $modelSrc = Join-Path $hlvr "models\weapons"
  $matSrc = Join-Path $hlvr "materials\models\weapons\vr_crowbar"
  $mdl = Join-Path $modelSrc "vr_crowbar.mdl"
  if (-not (Test-Path -LiteralPath $mdl)) {
    Write-Host "HL2VR vr_crowbar.mdl not found at $mdl - physical crowbar mesh skipped (swing logic still in the DLL)"
    return
  }
  $bms = Join-Path $GameRoot "bms"
  $modelDst = Join-Path $bms "models\weapons"
  $matDst = Join-Path $bms "materials\models\weapons\vr_crowbar"
  New-Item -ItemType Directory -Force -Path $modelDst | Out-Null
  Copy-Item -Force (Join-Path $modelSrc "vr_crowbar.*") $modelDst
  New-Item -ItemType Directory -Force -Path $matDst | Out-Null
  Get-ChildItem -LiteralPath $matSrc -File | Copy-Item -Force -Destination $matDst
  $legacySrc = Join-Path $matSrc "legacy"
  if (Test-Path -LiteralPath $legacySrc) {
    $legacyDst = Join-Path $matDst "legacy"
    New-Item -ItemType Directory -Force -Path $legacyDst | Out-Null
    Get-ChildItem -LiteralPath $legacySrc -File | Copy-Item -Force -Destination $legacyDst
  }
  $copiedMdl = Join-Path $modelDst "vr_crowbar.mdl"
  if (-not (Test-Path -LiteralPath $copiedMdl)) {
    throw "Failed to copy vr_crowbar.mdl to $modelDst"
  }
  $mdlBytes = (Get-Item -LiteralPath $copiedMdl).Length
  Write-Host "Installed HL2VR vr_crowbar into $copiedMdl ($mdlBytes bytes)"
}

Install-Hl2vrCrowbar

# 1) Launcher DXVK folder (AddDllDirectory / optional full-path LoadLibraryW)
Install-One $DxvkDir
# 2) bin\ — shaderapidx9 LoadLibrary("d3d9.dll") with USER_DIRS; bin is added before the game root
Install-One $BinDir
# 3) Next to bms.exe — L4D2VR-style, and AddDllDirectory(game root)
Install-One $GameRoot

# Quitting GameUI used to leave this armed; the next launch then disabled
# compositor Submit and the HMD stayed black with a working desktop cursor.
foreach ($dir in @($GameRoot, $BinDir, $DxvkDir)) {
  $flag = Join-Path $dir "bmvr_in_menu_vr.flag"
  if (Test-Path -LiteralPath $flag) {
    Remove-Item -LiteralPath $flag -Force
    Write-Host "Removed $flag"
  }
}

# Overlay death false-banned controller tracking (no hands / wrist HUD).
foreach ($skip in @((Join-Path $GameRoot "bmvr_skip.txt"), (Join-Path $BinDir "bmvr_skip.txt"), (Join-Path $DxvkDir "bmvr_skip.txt"))) {
  if (-not (Test-Path -LiteralPath $skip)) { continue }
  $text = Get-Content -LiteralPath $skip -Raw
  $new = [regex]::Replace($text, '(?m)^ctrl_pose\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^ctrl_vm\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^vr_gloves\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hand_overlay\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hl2vr_playercull\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hl2vr_pixelvis\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hl2vr_ham\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hl2vr_proj\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hl2vr_cproj\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hl2vr_neye\r?\n', '')
  $new = [regex]::Replace($new, '(?m)^hmd_world\r?\n', '')
  if ($new -ne $text) {
    Set-Content -LiteralPath $skip -Value $new -Encoding ASCII -NoNewline
    Write-Host "Removed false-ban ctrl_pose/ctrl_vm/vr_gloves/hand_overlay/hl2vr_playercull/hl2vr_pixelvis/hl2vr_ham/hmd_world from $skip"
  }
}

$HelperSrc = Join-Path $Root "build\Release\openxr_helper64"
$HelperDst = Join-Path $GameRoot "VR\openxr_helper64"
if (Test-Path (Join-Path $HelperSrc "OpenXRHelper64.exe")) {
  New-Item -ItemType Directory -Force -Path $HelperDst | Out-Null
  Copy-Item -Force (Join-Path $HelperSrc "OpenXRHelper64.exe") $HelperDst
  $Loader = Join-Path $HelperSrc "openxr_loader.dll"
  if (Test-Path $Loader) {
    Copy-Item -Force $Loader $HelperDst
  }
  Write-Host "Installed OpenXR helper to $HelperDst"
} else {
  Write-Host "OpenXR helper not built (build\Release\openxr_helper64\OpenXRHelper64.exe missing). OpenXR will fall back to OpenVR."
}

$VrSrc = Join-Path $Root "VR"
$VrDst = Join-Path $GameRoot "VR"
if (-not (Test-Path $VrSrc)) { throw "Missing $VrSrc" }
New-Item -ItemType Directory -Force -Path $VrDst | Out-Null
$ManifestSrc = Join-Path $VrSrc "SteamVRActionManifest"
$ManifestDst = Join-Path $VrDst "SteamVRActionManifest"
if (Test-Path $ManifestDst) {
  Remove-Item -Recurse -Force $ManifestDst
}
Copy-Item -Recurse -Force $ManifestSrc $ManifestDst
$HandsSrc = Join-Path $VrSrc "hands"
$HandsDst = Join-Path $VrDst "hands"
if (Test-Path $HandsSrc) {
  New-Item -ItemType Directory -Force -Path $HandsDst | Out-Null
  Copy-Item -Force (Join-Path $HandsSrc "*.glb") $HandsDst
  Write-Host "Installed hand GLBs to $HandsDst"
}
$WheelSrc = Join-Path $VrSrc "weapon_wheel"
$WheelDst = Join-Path $VrDst "weapon_wheel"
if (Test-Path $WheelSrc) {
  New-Item -ItemType Directory -Force -Path $WheelDst | Out-Null
  Copy-Item -Force (Join-Path $WheelSrc "*.vtf") $WheelDst
  Write-Host "Installed HL2VR radial-menu textures to $WheelDst"
}
$OffSrc = Join-Path $VrSrc "viewmodel_offsets.txt"
$OffDst = Join-Path $VrDst "viewmodel_offsets.txt"
if (Test-Path $OffSrc) {
  Copy-Item -Force $OffSrc $OffDst
  Write-Host "Installed $OffDst (baked pose table; numpad extras start empty)"
}
$CfgSrc = Join-Path $VrSrc "config.txt"
$CfgDst = Join-Path $VrDst "config.txt"
if (-not (Test-Path $CfgDst)) {
  Copy-Item -Force $CfgSrc $CfgDst
  Write-Host "Installed $CfgDst (edit RenderScale here; restart to apply)"
} else {
  Write-Host "Kept existing $CfgDst"
  $cfgText = Get-Content -LiteralPath $CfgDst -Raw
  $cfgText = [regex]::Replace($cfgText, '(?m)^HudDistance=.*$', 'HudDistance=1.05')
  $cfgText = [regex]::Replace($cfgText, '(?m)^HudSize=.*$', 'HudSize=0.70')
  $cfgText = [regex]::Replace($cfgText, '(?m)^ViewmodelScale=.*$', 'ViewmodelScale=0.68')
  $cfgText = [regex]::Replace($cfgText, '(?m)^CompositorPostPresentHandoff=.*$', 'CompositorPostPresentHandoff=true')
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsDebugBoxes=.*$', 'VrHandsDebugBoxes=false')
  if ($cfgText -notmatch '(?m)^HudDistance=') { $cfgText += "`r`nHudDistance=1.05`r`n" }
  if ($cfgText -notmatch '(?m)^HudSize=') { $cfgText += "`r`nHudSize=0.70`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^MenuPanelScale=.*$', 'MenuPanelScale=0.70')
  if ($cfgText -notmatch '(?m)^MenuPanelScale=') { $cfgText += "`r`nMenuPanelScale=0.70`r`n" }
  if ($cfgText -notmatch '(?m)^MenuCursorSmoothSec=') { $cfgText += "`r`nMenuCursorSmoothSec=0.18`r`n" }
  if ($cfgText -notmatch '(?m)^ViewmodelScale=') { $cfgText += "`r`nViewmodelScale=0.68`r`n" }
  if ($cfgText -notmatch '(?m)^CompositorPostPresentHandoff=') { $cfgText += "`r`nCompositorPostPresentHandoff=true`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsGlovesEnabled=') { $cfgText += "`r`nVrHandsGlovesEnabled=true`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsModelScale=') { $cfgText += "`r`nVrHandsModelScale=0.85`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsModelScale=.*$', 'VrHandsModelScale=0.85')
  if ($cfgText -notmatch '(?m)^VrHandsPoseRotationOffset=') { $cfgText += "`r`nVrHandsPoseRotationOffset=0,180,0`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsPoseRotationOffset=.*$', 'VrHandsPoseRotationOffset=0,180,0')
  if ($cfgText -notmatch '(?m)^VrHandsPoseOffsetMeters=') { $cfgText += "`r`nVrHandsPoseOffsetMeters=0,-0.008,-0.10`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsPoseOffsetMeters=.*$', 'VrHandsPoseOffsetMeters=0,-0.008,-0.10')
  if ($cfgText -notmatch '(?m)^AutoMatQueueMode=') { $cfgText += "`r`nAutoMatQueueMode=false`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^AutoMatQueueMode=.*$', 'AutoMatQueueMode=false')
  if ($cfgText -notmatch '(?m)^MulticoreMode=') { $cfgText += "`r`nMulticoreMode=true`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^MulticoreMode=.*$', 'MulticoreMode=true')
  Write-Host "MulticoreMode=true (mat_queue_mode 2 after stereo warmup; menu stays 0)."
  if ($cfgText -notmatch '(?m)^AntiAliasing=') { $cfgText += "`r`nAntiAliasing=0`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsUseHevGloves=') { $cfgText += "`r`nVrHandsUseHevGloves=true`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsDebugBoxes=') { $cfgText += "`r`nVrHandsDebugBoxes=false`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandHud=') { $cfgText += "`r`nVrHandHud=true`r`n" }
  if ($cfgText -notmatch '(?m)^VrCrosshair=') { $cfgText += "`r`nVrCrosshair=false`r`n" }
  if ($cfgText -notmatch '(?m)^ScopeZoomFovScale=') { $cfgText += "`r`nScopeZoomFovScale=0.28`r`n" }
  if ($cfgText -notmatch '(?m)^ScopeZoomSmoothSec=') { $cfgText += "`r`nScopeZoomSmoothSec=0.16`r`n" }
  if ($cfgText -notmatch '(?m)^DesktopLeftoverRender=') { $cfgText += "`r`nDesktopLeftoverRender=false`r`n" }
  if ($cfgText -notmatch '(?m)^ForceOpenVis=') { $cfgText += "`r`nForceOpenVis=false`r`n" }
  if ($cfgText -notmatch '(?m)^HiddenAreaMesh=') { $cfgText += "`r`nHiddenAreaMesh=false`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^HiddenAreaMesh=.*$', 'HiddenAreaMesh=false')
  if ($cfgText -notmatch '(?m)^MaterialPredraw=') { $cfgText += "`r`nMaterialPredraw=false`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^MaterialPredraw=.*$', 'MaterialPredraw=false')
  if ($cfgText -notmatch '(?m)^StereoBlitGpuFlush=') { $cfgText += "`r`nStereoBlitGpuFlush=false`r`n" }
  # 2026-09-06: deferred publish raised fps but broke WMR smoothing. Force false.
  if ($cfgText -notmatch '(?m)^OpenXrDeferredPublish=') { $cfgText += "`r`nOpenXrDeferredPublish=false`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^OpenXrDeferredPublish=.*$', 'OpenXrDeferredPublish=false')
  if ($cfgText -notmatch '(?m)^OpenXrSlotCoolingMs=') { $cfgText += "`r`nOpenXrSlotCoolingMs=4`r`n" }
  if ($cfgText -notmatch '(?m)^OpenXrMaxPending=') { $cfgText += "`r`nOpenXrMaxPending=1`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^VRRuntimeBackend=(hl2vr|openvrhl2vr|hl2vropenvr|steamvrhl2vr)\s*$', 'VRRuntimeBackend=openxr')
  if ($cfgText -notmatch '(?m)^VRRuntimeBackend=') { $cfgText += "`r`nVRRuntimeBackend=openxr`r`n" }
  if ($cfgText -notmatch '(?m)^OpenXRHelper=') { $cfgText += "`r`nOpenXRHelper=true`r`n" }
  if ($cfgText -notmatch '(?m)^OpenXRHelperSubmitTestFrames=') { $cfgText += "`r`nOpenXRHelperSubmitTestFrames=0`r`n" }
  if ($cfgText -notmatch '(?m)^OpenXRHelperWaitReadySeconds=') { $cfgText += "`r`nOpenXRHelperWaitReadySeconds=45`r`n" }
  if ($cfgText -notmatch '(?m)^OpenXRHelperUseGameRenderPoseForProjection=') { $cfgText += "`r`nOpenXRHelperUseGameRenderPoseForProjection=true`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^OpenXRHelperUseGameRenderPoseForProjection=.*$', 'OpenXRHelperUseGameRenderPoseForProjection=true')
  if ($cfgText -notmatch '(?m)^OpenXRHelperHandTracking=') { $cfgText += "`r`nOpenXRHelperHandTracking=false`r`n" }
  # GitHub default: world RTs at one eye. Do not overwrite a user false.
  if ($cfgText -notmatch '(?m)^WorldRenderAtEyeSize=') { $cfgText += "`r`nWorldRenderAtEyeSize=true`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^WorldRenderAtEyeSize=.*$', 'WorldRenderAtEyeSize=true')
  $cfgText = [regex]::Replace($cfgText, '(?m)^Roomscale1To1.*\r?\n', '')
  $cfgText = [regex]::Replace($cfgText, '(?m)^PhysicalCrouch=.*\r?\n', '')
  $cfgText = [regex]::Replace($cfgText, '(?m)^RoomscaleMovement=.*\r?\n', '')
  Set-Content -LiteralPath $CfgDst -Value $cfgText -Encoding ASCII -NoNewline
  Write-Host "Updated HudDistance/HudSize/ViewmodelScale/CompositorPostPresentHandoff/gloves/wrist HUD/OpenXR in $CfgDst"
  Write-Host "WorldRenderAtEyeSize=true (world at eye size, same as github.com/.../black-mesa-vr)."
}
Write-Host "Installed SteamVR bindings to $ManifestDst"
Write-Host "If SteamVR still has old face-button bindings, restore BMVR defaults (v4: left trigger=use, B=alt-fire, A=reload, Y=menu)."

$CfgGame = Join-Path $GameRoot "bms\cfg"
if (-not (Test-Path $CfgGame)) { throw "Missing $CfgGame" }
$BmvrCfgSrc = Join-Path $VrSrc "bmvr.cfg"
$BmvrCfgDst = Join-Path $CfgGame "bmvr.cfg"
Copy-Item -Force $BmvrCfgSrc $BmvrCfgDst
Write-Host "Installed $BmvrCfgDst (PVS on; mat_queue_mode 2 is set by the DLL after warmup)"
$Autoexec = Join-Path $CfgGame "autoexec.cfg"
if (Test-Path $Autoexec) {
  $autoText = Get-Content -LiteralPath $Autoexec -Raw
  if ($autoText -notmatch '(?m)^\s*exec\s+bmvr') {
    Add-Content -LiteralPath $Autoexec -Value "`r`nexec bmvr`r`n"
    Write-Host "Appended 'exec bmvr' to $Autoexec"
  }
} else {
  Set-Content -LiteralPath $Autoexec -Value "exec bmvr`r`n" -Encoding ASCII
  Write-Host "Created $Autoexec"
}

# Stock dxvk.conf sets deviceLossOnFocusLoss=True and overrides our built-in
# bms.exe profile (False). Steam verify restores the stock file.
$DxvkConf = Join-Path $DxvkDir "dxvk.conf"
$ConfText = @"
d3d9.deferSurfaceCreation = True
d3d9.deviceLossOnFocusLoss = False
"@
Set-Content -LiteralPath $DxvkConf -Value $ConfText -Encoding ASCII
Write-Host "Set $DxvkConf deviceLossOnFocusLoss=False"

Write-Host ""
Write-Host "Launch from Steam with the existing L4D2VR options, plus:"
Write-Host "  -oldgameui"
Write-Host "The new Black Mesa game UI is upside-down in the headset and stays black after load."
Write-Host "If the launcher video menu is Direct3D 9, also add:"
Write-Host "  -enabledxvk"
Write-Host "That is a verified load-path issue: default/native D3D9 never loads this DLL."
Write-Host "Log: $GameRoot\bmvr_log.txt  (also next to whichever d3d9.dll actually loaded)"
Write-Host "Close the game before the next install."
