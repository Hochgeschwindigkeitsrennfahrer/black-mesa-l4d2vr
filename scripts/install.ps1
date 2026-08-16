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
  Write-Host "Black Mesa is running (PID $($running.Id -join ', ')). Stopping it so d3d9.dll can be replaced."
  $running | Stop-Process -Force
  Start-Sleep -Seconds 2
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

# 1) Launcher DXVK folder (AddDllDirectory / optional full-path LoadLibraryW)
Install-One $DxvkDir
# 2) bin\ — shaderapidx9 LoadLibrary("d3d9.dll") with USER_DIRS; bin is added before the game root
Install-One $BinDir
# 3) Next to bms.exe — L4D2VR-style, and AddDllDirectory(game root)
Install-One $GameRoot

Write-Host ""
Write-Host "Launch from Steam with the existing L4D2VR options, plus:"
Write-Host "  -oldgameui"
Write-Host "The new Black Mesa game UI is upside-down in the headset and stays black after load."
Write-Host "If the launcher video menu is Direct3D 9, also add:"
Write-Host "  -enabledxvk"
Write-Host "That is a verified load-path issue: default/native D3D9 never loads this DLL."
Write-Host "Log: $GameRoot\bmvr_log.txt  (also next to whichever d3d9.dll actually loaded)"
Write-Host "Close the game before the next install."
