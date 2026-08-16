# Fetch OpenXR loader + optional DXVK VR submodule hints.
param(
  [switch]$CloneDxvk
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path $PSScriptRoot -Parent
$OxrLib = Join-Path $Repo "BMSVR\thirdparty\openxr\lib"
New-Item -ItemType Directory -Force -Path $OxrLib | Out-Null

if (-not $env:VULKAN_SDK) {
  Write-Warning "VULKAN_SDK is not set. Install the LunarG Vulkan SDK and re-open the shell."
} else {
  Write-Host "Vulkan SDK: $env:VULKAN_SDK"
  $loaderCandidates = @(
    "$env:VULKAN_SDK\Lib32\openxr_loader.lib",
    "$env:VULKAN_SDK\Lib\openxr_loader.lib"
  ) | Where-Object { Test-Path $_ }
  foreach ($c in $loaderCandidates) {
    Copy-Item $c $OxrLib -Force
    Write-Host "Copied $c"
  }
  $dll = @(
    "$env:VULKAN_SDK\Bin32\openxr_loader.dll",
    "$env:VULKAN_SDK\Bin\openxr_loader.dll"
  ) | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($dll) {
    Copy-Item $dll $OxrLib -Force
    Write-Host "Copied $dll (ship 32-bit next to bms.exe)"
  }
}

if ($CloneDxvk) {
  $dxvk = Join-Path $Repo "BMSVR\thirdparty\dxvk"
  if (-not (Test-Path $dxvk)) {
    git clone --depth 1 https://github.com/fholger/dxvk_l4d2vr.git $dxvk
  } else {
    Write-Host "DXVK already present at $dxvk"
  }
}

Write-Host "Done."
