# Completely remove Black Mesa VR files from the game folder.
# Safe to double-click via UNINSTALL-BMVR.bat next to bms.exe.
param(
  [string]$GameRoot = "",
  [switch]$Yes
)

$ErrorActionPreference = "Stop"

function Resolve-GameRoot {
  if ($GameRoot -and (Test-Path (Join-Path $GameRoot "bms.exe"))) { return $GameRoot }
  if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot "bms.exe"))) { return $PSScriptRoot }
  $here = (Get-Location).Path
  if (Test-Path (Join-Path $here "bms.exe")) { return $here }
  $steam = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
  if (Test-Path (Join-Path $steam "bms.exe")) { return $steam }
  throw "Could not find Black Mesa (bms.exe). Put this script in the game folder."
}

$GameRoot = Resolve-GameRoot
$BinDir = Join-Path $GameRoot "bin"
$DxvkDir = Join-Path $GameRoot "bin\thirdparty\dxvk-windows-x86"
$KeepNames = @(
  "UNINSTALL-BMVR.bat",
  "UNINSTALL-BMVR.ps1",
  "REBUILD-INSTALL-BMVR.bat",
  "REBUILD-INSTALL-BMVR.ps1"
)

Write-Host ""
Write-Host "Remove Black Mesa VR files from:"
Write-Host "  $GameRoot"
Write-Host ""
Write-Host "This deletes the VR folder, our d3d9.dll copies, logs, flags, crowbar assets,"
Write-Host "and bmvr.cfg. Stock DXVK in the thirdparty folder is restored from backup."
Write-Host "The uninstall/rebuild helper scripts in this folder are kept."
Write-Host ""

if (-not $Yes) {
  $answer = Read-Host "Type YES to continue"
  if ($answer -ne "YES") {
    Write-Host "Cancelled."
    exit 0
  }
}

function Stop-GameProcesses {
  foreach ($name in @("bms", "OpenXRHelper64")) {
    $procs = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
    if ($procs.Count -gt 0) {
      Write-Host ("Stopping {0} (PID {1})" -f $name, ($procs.Id -join ", "))
      $procs | Stop-Process -Force -ErrorAction SilentlyContinue
    }
  }
  cmd /c "taskkill /F /IM bms.exe /T >nul 2>&1"
  cmd /c "taskkill /F /IM OpenXRHelper64.exe /T >nul 2>&1"
  Start-Sleep -Seconds 2
}

Stop-GameProcesses

$script:Removed = 0
$script:Failed = New-Object System.Collections.Generic.List[string]

function Remove-Target([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return }
  $name = Split-Path $Path -Leaf
  if ($KeepNames -contains $name) { return }
  try {
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
    Write-Host "Removed $Path"
    $script:Removed++
  } catch {
    $script:Failed.Add($Path)
    Write-Host ("FAILED  {0}  ({1})" -f $Path, $_.Exception.Message)
  }
}

function Remove-Glob([string]$Dir, [string]$Filter) {
  if (-not (Test-Path -LiteralPath $Dir)) { return }
  Get-ChildItem -LiteralPath $Dir -Force -File -Filter $Filter -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Target $_.FullName
  }
}

# Restore stock DXVK before deleting the mod copy in that folder.
$stockBackup = Join-Path $DxvkDir "d3d9.dll.stock-dxvk"
$dxvkDll = Join-Path $DxvkDir "d3d9.dll"
$restoredDxvk = $false
if ((Test-Path -LiteralPath $stockBackup) -and (Test-Path -LiteralPath $DxvkDir)) {
  try {
    Copy-Item -LiteralPath $stockBackup -Destination $dxvkDll -Force
    $restoredDxvk = $true
    Write-Host "Restored stock DXVK d3d9.dll from backup ($((Get-Item -LiteralPath $dxvkDll).Length) bytes)"
  } catch {
    Write-Host ("FAILED restoring stock DXVK: {0}" -f $_.Exception.Message)
    $script:Failed.Add($stockBackup)
  }
}

$stockConf = @"
d3d9.deferSurfaceCreation = True
d3d9.deviceLossOnFocusLoss = True
"@
$dxvkConf = Join-Path $DxvkDir "dxvk.conf"
if (Test-Path -LiteralPath $DxvkDir) {
  Set-Content -LiteralPath $dxvkConf -Value $stockConf -Encoding ASCII
  Write-Host "Restored stock $dxvkConf (deviceLossOnFocusLoss=True)"
}

# Game-root extras (none of these exist in a stock install).
Remove-Target (Join-Path $GameRoot "d3d9.dll")
Remove-Target (Join-Path $GameRoot "openvr_api.dll")
Remove-Target (Join-Path $GameRoot "openxr_loader.dll")
Remove-Target (Join-Path $GameRoot "BMVR_README.txt")
Remove-Target (Join-Path $GameRoot "TEST_RELEASE.txt")
Remove-Target (Join-Path $GameRoot "STICK_CLICK_SPRINT.txt")
Remove-Target (Join-Path $GameRoot "DISABLE-SOURCEVR.bat")
Remove-Target (Join-Path $GameRoot "bms_d3d9.log")
Remove-Target (Join-Path $GameRoot "VR")

foreach ($dir in @($GameRoot, $BinDir, $DxvkDir)) {
  Remove-Glob $dir "d3d9.dll.hung*"
  Remove-Glob $dir "d3d9.dll.userbackup-*"
  Remove-Glob $dir "d3d9.dll.stock-dxvk"
  Remove-Glob $dir "bmvr_*"
  Remove-Glob $dir "bmvr_in_*.flag"
  Remove-Glob $dir "bms_d3d9.log"
}

# bin\d3d9.dll is our copy, not stock. Leave bin\openvr_api.dll (game file; we may have overwritten it).
Remove-Target (Join-Path $BinDir "d3d9.dll")
Remove-Target (Join-Path $BinDir "openxr_loader.dll")
$sourceVr = Join-Path $BinDir "sourcevr.dll"
$sourceVrDisabled = Join-Path $BinDir "sourcevr.dll.disabled"
if (Test-Path -LiteralPath $sourceVrDisabled) {
  if (Test-Path -LiteralPath $sourceVr) {
    Write-Host "Kept existing bin\sourcevr.dll (did not overwrite from sourcevr.dll.disabled)"
  } else {
    Move-Item -LiteralPath $sourceVrDisabled -Destination $sourceVr
    Write-Host "Restored bin\sourcevr.dll from sourcevr.dll.disabled"
  }
}
Remove-Target (Join-Path $DxvkDir "openvr_api.dll")
if (-not $restoredDxvk) {
  Remove-Target $dxvkDll
  Write-Host "WARN: no d3d9.dll.stock-dxvk backup. Deleted the mod DLL from the DXVK folder."
  Write-Host "      Steam -> Black Mesa -> Properties -> Installed Files -> Verify integrity of game files."
}

foreach ($gamedir in @("bms", "bshift")) {
  $cfgDir = Join-Path $GameRoot "$gamedir\cfg"
  Remove-Target (Join-Path $cfgDir "bmvr.cfg")
  $autoexec = Join-Path $cfgDir "autoexec.cfg"
  if (Test-Path -LiteralPath $autoexec) {
    $lines = Get-Content -LiteralPath $autoexec
    $kept = @($lines | Where-Object { $_ -notmatch '^\s*exec\s+bmvr\b' })
    if ($kept.Count -ne $lines.Count) {
      if ($kept.Count -eq 0) {
        Remove-Target $autoexec
      } else {
        Set-Content -LiteralPath $autoexec -Value ($kept -join "`r`n") -Encoding ASCII
        Write-Host "Removed 'exec bmvr' from $autoexec"
      }
    }
  }

  $modelDir = Join-Path $GameRoot "$gamedir\models\weapons"
  Remove-Glob $modelDir "vr_crowbar.*"
  Remove-Target (Join-Path $GameRoot "$gamedir\materials\models\weapons\vr_crowbar")
}

Write-Host ""
if ($script:Failed.Count -gt 0) {
  Write-Host ("Done with {0} removal(s), {1} failed." -f $script:Removed, $script:Failed.Count)
  Write-Host "Close Black Mesa / SteamVR helpers and run this again, or Steam-verify the game."
  exit 1
}

Write-Host ("Removed {0} item(s). Game folder is clear of this mod." -f $script:Removed)
Write-Host "bin\openvr_api.dll was left in place (stock game file; this mod may have overwritten it)."
Write-Host "Steam Verify integrity restores that copy if you want a 100% stock binary."
exit 0
