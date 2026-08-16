# Build BMSVR d3d9.dll only — no install, no game launch.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$build = "$root\BMSVR\build_d3d9"

if (-not (Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }
Copy-Item "$root\BMSVR\CMakeLists.d3d9.txt" "$build\CMakeLists.txt" -Force
& $cmake -A Win32 -B $build -S $build
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }
& $cmake --build $build --config Release -- /m
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

$dll = Join-Path $build "Release\d3d9.dll"
if (-not (Test-Path $dll)) { throw "Build output missing: $dll" }
Write-Host "OK build-only: $dll"
Get-Item $dll | Format-List FullName, Length, LastWriteTime
