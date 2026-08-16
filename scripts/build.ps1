# Build Win32 d3d9.dll (L4D2VR DXVK + Black Mesa VR).
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$CMake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $CMake)) {
  $CMake = "cmake"
}

$BuildDir = Join-Path $Root "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

& $CMake -S $Root -B $BuildDir -A Win32
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $CMake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$Out = Join-Path $BuildDir "Release\d3d9.dll"
if (Test-Path $Out) {
  Write-Host "Built $Out"
} else {
  throw "d3d9.dll not found at $Out"
}
