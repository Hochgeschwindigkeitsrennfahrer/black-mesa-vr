# Rebuild d3d9.dll + OpenXR helper, then install every file the mod needs.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

Write-Host "Repo: $Root"
Write-Host ""

$build = Join-Path $PSScriptRoot "build.ps1"
$install = Join-Path $PSScriptRoot "install.ps1"
if (-not (Test-Path -LiteralPath $build)) { throw "Missing $build" }
if (-not (Test-Path -LiteralPath $install)) { throw "Missing $install" }

Write-Host "=== Build ==="
& $build
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "build.ps1 failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "=== Install ==="
& $install
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "install.ps1 failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "Rebuild + install finished."
