# Install BMSVR into the Black Mesa folder.
param(
  [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa",
  [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path $PSScriptRoot -Parent
if (-not $BuildDir) {
  $cand = @(
    "$Repo\BMSVR\build\Release\bmsvr.dll",
    "$Repo\BMSVR\build\Debug\bmsvr.dll"
  ) | Where-Object { Test-Path $_ } | Select-Object -First 1
  $BuildDir = $cand
}

if (-not (Test-Path "$GameDir\bin")) {
  Write-Error "Black Mesa bin folder missing. Finish installing the game first: $GameDir"
}

$client = Get-ChildItem $GameDir -Recurse -Filter client.dll -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $client) {
  Write-Warning "client.dll not found — game appears incomplete. DLL will still be copied, but VR will not work yet."
}

if (-not $BuildDir -or -not (Test-Path $BuildDir)) {
  Write-Error "bmsvr.dll not built. Run cmake build in BMSVR/ first."
}

$vrDir = Join-Path $GameDir "VR"
New-Item -ItemType Directory -Force -Path $vrDir | Out-Null
Copy-Item $BuildDir (Join-Path $GameDir "bin\bmsvr.dll") -Force
Copy-Item "$Repo\BMSVR\assets\bmsvr.cfg" (Join-Path $GameDir "bmsvr.cfg") -Force

# Prefer ASI-style load if winmm/proxy already present; also drop a note for d3d9 VR.
$note = @"
BMSVR installed.

1. Replace bin\d3d9.dll with a VR-capable DXVK build (see docs/ARCHITECTURE.md).
   Backup any RTX Remix d3d9.dll first.
2. Ensure an OpenXR runtime is active (SteamVR OpenXR is fine).
3. Launch Black Mesa windowed with:
   -window -novid -w 1280 -h 720 +mat_queue_mode 0 +mat_vsync 0 +crosshair 0
4. LoadLibrary path: if bmsvr.dll is not auto-loaded by your d3d9 VR build,
   inject via an ASI loader or call LoadLibrary("bmsvr.dll") from the DXVK VR init.

"@
Set-Content -Path (Join-Path $vrDir "INSTALL.txt") -Value $note
Write-Host $note
Write-Host "Copied $($BuildDir) -> $GameDir\bin\bmsvr.dll"
