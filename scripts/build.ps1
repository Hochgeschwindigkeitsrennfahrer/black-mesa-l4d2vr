# Build Win32 d3d9.dll (L4D2VR DXVK + Black Mesa VR) and the x64 OpenXR helper.
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

$HelperSrc = Join-Path $Root "OpenXRHelper64"
$HelperBuild = Join-Path $Root "build_openxr_helper64"
New-Item -ItemType Directory -Force -Path $HelperBuild | Out-Null
& $CMake -S $HelperSrc -B $HelperBuild -A x64
if ($LASTEXITCODE -ne 0) { throw "OpenXR helper cmake configure failed" }
& $CMake --build $HelperBuild --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "OpenXR helper build failed" }

$HelperExe = Join-Path $HelperBuild "Release\OpenXRHelper64.exe"
if (-not (Test-Path $HelperExe)) {
  throw "OpenXRHelper64.exe not found at $HelperExe"
}

& (Join-Path $PSScriptRoot "fetch_openxr_loader.ps1")
$Loader = Join-Path $Root "third_party\openxr\loader\openxr_loader.dll"
$HelperOut = Join-Path $Root "build\Release\openxr_helper64"
New-Item -ItemType Directory -Force -Path $HelperOut | Out-Null
Copy-Item -Force $HelperExe (Join-Path $HelperOut "OpenXRHelper64.exe")
Copy-Item -Force $Loader (Join-Path $HelperOut "openxr_loader.dll")
Write-Host "Built $(Join-Path $HelperOut 'OpenXRHelper64.exe')"
