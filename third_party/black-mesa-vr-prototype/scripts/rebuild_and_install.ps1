# Full BMSVR rebuild + install (no UAC elevation).
# Copies d3d9.dll / openxr_loader.dll with a normal Copy-Item.
# If Access Denied: grant write once via scripts\grant_bin_write.ps1 (one UAC), then re-run.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$bmRoot = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
$bmBin = Join-Path $bmRoot "bin"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$build = "$root\BMSVR\build_d3d9"

Copy-Item "$root\BMSVR\CMakeLists.d3d9.txt" "$build\CMakeLists.txt" -Force
& $cmake -A Win32 -B $build -S $build
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }
& $cmake --build $build --config Release -- /m
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

$d3d9Src = Join-Path $build "Release\d3d9.dll"
$oxrSrc = Join-Path $root "BMSVR\thirdparty\openxr\pkg\native\Win32\release\bin\openxr_loader.dll"
$cfgSrc = Join-Path $root "BMSVR\assets\bmsvr.cfg"

if (-not (Test-Path $d3d9Src)) { throw "Build output missing: $d3d9Src" }
if (-not (Test-Path $bmBin)) { throw "Black Mesa bin missing: $bmBin" }

function Copy-NoElevate {
  param([string]$From, [string]$To)
  try {
    Copy-Item -LiteralPath $From -Destination $To -Force -ErrorAction Stop
    Write-Host "OK  $From -> $To"
  } catch {
    $msg = $_.Exception.Message
    $inUse = $msg -match 'being used by another process|von einem anderen Prozess'
    $accessDenied = ($msg -match 'Access.*(denied|verweigert)') -or ($_.Exception -is [System.UnauthorizedAccessException])
    if ($inUse) {
      Write-Host ""
      Write-Host "FILE IN USE / DATEI BELEGT:" -ForegroundColor Yellow
      Write-Host "  $To"
      Write-Host "Close Black Mesa (and any injector), then re-run. No UAC needed."
      exit 1
    }
    if ($accessDenied) {
      Write-Host ""
      Write-Host "ACCESS DENIED / ZUGRIFF VERWEIGERT writing:" -ForegroundColor Yellow
      Write-Host "  $To"
      Write-Host ""
      Write-Host "One-time fix (one UAC prompt), then re-run this script:"
      Write-Host "  powershell -ExecutionPolicy Bypass -File `"$root\scripts\grant_bin_write.ps1`""
      Write-Host ""
      Write-Host "Or manually grant your user Modify on:"
      Write-Host "  $bmBin"
      Write-Host "  (Properties -> Security, or run Cursor/terminal as admin once and use grant_bin_write.ps1)"
      Write-Host ""
      Write-Host "Normal rebuild/install does NOT elevate (no UAC each time)."
      exit 1
    }
    throw
  }
}

Copy-NoElevate $d3d9Src (Join-Path $bmBin "d3d9.dll")
if (Test-Path $oxrSrc) {
  Copy-NoElevate $oxrSrc (Join-Path $bmBin "openxr_loader.dll")
} else {
  Write-Warning "openxr_loader.dll not found at $oxrSrc (skipped)"
}

# Keep Black Mesa stock openvr_api.dll (old ABI). Do NOT replace with SteamVR openvr.
$stock = Join-Path $bmBin "openvr_api.dll.stock_sourcevr"
if (Test-Path $stock) {
  Copy-NoElevate $stock (Join-Path $bmBin "openvr_api.dll")
}

# Keep Valve sourcevr disabled - conflicts with inject VR
$svr = Join-Path $bmBin "sourcevr.dll"
$dis = Join-Path $bmBin "sourcevr.dll.disabled"
if (Test-Path $svr) {
  try {
    Move-Item -LiteralPath $svr -Destination $dis -Force -ErrorAction Stop
    Write-Host "OK  disabled sourcevr.dll -> sourcevr.dll.disabled"
  } catch {
    Write-Warning "Could not disable sourcevr.dll: $($_.Exception.Message)"
  }
}

Copy-NoElevate $cfgSrc (Join-Path $bmRoot "bmsvr.cfg")

$batSrc = Join-Path $root "run-bms-vr.bat"
if (Test-Path $batSrc) {
  Copy-NoElevate $batSrc (Join-Path $bmRoot "run-bms-vr.bat")
}
$batSv = Join-Path $root "run-bms-vr-steamvr.bat"
if (Test-Path $batSv) {
  Copy-NoElevate $batSv (Join-Path $bmRoot "run-bms-vr-steamvr.bat")
}

Write-Host ""
Write-Host "Installed (no UAC). Launch run-bms-vr.bat (system OpenXR / WMR) or run-bms-vr-steamvr.bat."
