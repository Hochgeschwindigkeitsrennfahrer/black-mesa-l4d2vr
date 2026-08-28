# Fetch Khronos OpenXR.Loader 1.1.60 (same package L4D2VR's openxr branch uses).
# The x64 helper loads openxr_loader.dll from its own directory.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$LoaderDir = Join-Path $Root "third_party\openxr\loader"
$Nupkg = Join-Path $LoaderDir "OpenXR.Loader.1.1.60.nupkg"
$Extract = Join-Path $LoaderDir "OpenXR.Loader.1.1.60.extract"
$Dll = Join-Path $LoaderDir "openxr_loader.dll"

New-Item -ItemType Directory -Force -Path $LoaderDir | Out-Null

if (-not (Test-Path $Dll)) {
  if (-not (Test-Path $Nupkg)) {
    $Urls = @(
      "https://api.nuget.org/v3-flatcontainer/openxr.loader/1.1.60/openxr.loader.1.1.60.nupkg",
      "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.60/OpenXR.Loader.1.1.60.nupkg"
    )
    $downloaded = $false
    foreach ($Url in $Urls) {
      Write-Host "Downloading OpenXR.Loader 1.1.60 from $Url"
      try {
        Invoke-WebRequest -Uri $Url -OutFile $Nupkg -UseBasicParsing
        if ((Test-Path $Nupkg) -and ((Get-Item $Nupkg).Length -gt 10000)) {
          $downloaded = $true
          break
        }
      } catch {
        Write-Host ("Download failed: " + $_.Exception.Message)
      }
    }
    if (-not $downloaded) {
      throw "Could not download OpenXR.Loader 1.1.60"
    }
  }
  if (Test-Path $Extract) {
    Remove-Item -LiteralPath $Extract -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $Extract | Out-Null
  $Zip = Join-Path $Extract "OpenXR.Loader.1.1.60.zip"
  Copy-Item -LiteralPath $Nupkg -Destination $Zip -Force
  Expand-Archive -LiteralPath $Zip -DestinationPath $Extract -Force
  $Src = Join-Path $Extract "native\x64\release\bin\openxr_loader.dll"
  if (-not (Test-Path $Src)) {
    throw "x64 openxr_loader.dll missing inside OpenXR.Loader 1.1.60 package"
  }
  Copy-Item -Force $Src $Dll
}

Write-Host "OpenXR loader: $Dll"
