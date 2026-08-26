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

$VrSrc = Join-Path $Root "VR"
$VrDst = Join-Path $GameRoot "VR"
if (-not (Test-Path $VrSrc)) { throw "Missing $VrSrc" }
New-Item -ItemType Directory -Force -Path $VrDst | Out-Null
$ManifestSrc = Join-Path $VrSrc "SteamVRActionManifest"
$ManifestDst = Join-Path $VrDst "SteamVRActionManifest"
if (Test-Path $ManifestDst) {
  Remove-Item -Recurse -Force $ManifestDst
}
Copy-Item -Recurse -Force $ManifestSrc $ManifestDst
$HandsSrc = Join-Path $VrSrc "hands"
$HandsDst = Join-Path $VrDst "hands"
if (Test-Path $HandsSrc) {
  New-Item -ItemType Directory -Force -Path $HandsDst | Out-Null
  Copy-Item -Force (Join-Path $HandsSrc "*.glb") $HandsDst
  Write-Host "Installed HEV glove GLBs to $HandsDst"
}
$CfgSrc = Join-Path $VrSrc "config.txt"
$CfgDst = Join-Path $VrDst "config.txt"
if (-not (Test-Path $CfgDst)) {
  Copy-Item -Force $CfgSrc $CfgDst
  Write-Host "Installed $CfgDst (edit RenderScale here; restart to apply)"
} else {
  Write-Host "Kept existing $CfgDst"
  $cfgText = Get-Content -LiteralPath $CfgDst -Raw
  $cfgText = [regex]::Replace($cfgText, '(?m)^HudDistance=.*$', 'HudDistance=1.05')
  $cfgText = [regex]::Replace($cfgText, '(?m)^HudSize=.*$', 'HudSize=0.70')
  $cfgText = [regex]::Replace($cfgText, '(?m)^ViewmodelScale=.*$', 'ViewmodelScale=0.68')
  $cfgText = [regex]::Replace($cfgText, '(?m)^CompositorPostPresentHandoff=.*$', 'CompositorPostPresentHandoff=true')
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsDebugBoxes=.*$', 'VrHandsDebugBoxes=false')
  if ($cfgText -notmatch '(?m)^HudDistance=') { $cfgText += "`r`nHudDistance=1.05`r`n" }
  if ($cfgText -notmatch '(?m)^HudSize=') { $cfgText += "`r`nHudSize=0.70`r`n" }
  if ($cfgText -notmatch '(?m)^ViewmodelScale=') { $cfgText += "`r`nViewmodelScale=0.68`r`n" }
  if ($cfgText -notmatch '(?m)^CompositorPostPresentHandoff=') { $cfgText += "`r`nCompositorPostPresentHandoff=true`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsGlovesEnabled=') { $cfgText += "`r`nVrHandsGlovesEnabled=true`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsModelScale=') { $cfgText += "`r`nVrHandsModelScale=0.85`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsModelScale=.*$', 'VrHandsModelScale=0.85')
  if ($cfgText -notmatch '(?m)^VrHandsPoseRotationOffset=') { $cfgText += "`r`nVrHandsPoseRotationOffset=0,180,0`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsPoseRotationOffset=.*$', 'VrHandsPoseRotationOffset=0,180,0')
  if ($cfgText -notmatch '(?m)^VrHandsPoseOffsetMeters=') { $cfgText += "`r`nVrHandsPoseOffsetMeters=0,-0.035,0`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^VrHandsPoseOffsetMeters=.*$', 'VrHandsPoseOffsetMeters=0,-0.035,0')
  if ($cfgText -notmatch '(?m)^AutoMatQueueMode=') { $cfgText += "`r`nAutoMatQueueMode=false`r`n" }
  $cfgText = [regex]::Replace($cfgText, '(?m)^AutoMatQueueMode=.*$', 'AutoMatQueueMode=false')
  if ($cfgText -notmatch '(?m)^AntiAliasing=') { $cfgText += "`r`nAntiAliasing=0`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsUseHevGloves=') { $cfgText += "`r`nVrHandsUseHevGloves=true`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandsDebugBoxes=') { $cfgText += "`r`nVrHandsDebugBoxes=false`r`n" }
  if ($cfgText -notmatch '(?m)^VrHandHud=') { $cfgText += "`r`nVrHandHud=true`r`n" }
  if ($cfgText -notmatch '(?m)^DesktopLeftoverRender=') { $cfgText += "`r`nDesktopLeftoverRender=false`r`n" }
  Set-Content -LiteralPath $CfgDst -Value $cfgText -Encoding ASCII -NoNewline
  Write-Host "Updated HudDistance/HudSize/ViewmodelScale/CompositorPostPresentHandoff/gloves/wrist HUD in $CfgDst"
}
Write-Host "Installed SteamVR bindings to $ManifestDst"
Write-Host "If SteamVR still has X=Flashlight or old jump/crouch, restore BMVR defaults (v3: Y=next, X=prev, right grip=flashlight)."

$CfgGame = Join-Path $GameRoot "bms\cfg"
if (-not (Test-Path $CfgGame)) { throw "Missing $CfgGame" }
$BmvrCfgSrc = Join-Path $VrSrc "bmvr.cfg"
$BmvrCfgDst = Join-Path $CfgGame "bmvr.cfg"
Copy-Item -Force $BmvrCfgSrc $BmvrCfgDst
Write-Host "Installed $BmvrCfgDst (crosshair off only; no flashlight cvars)"
$Autoexec = Join-Path $CfgGame "autoexec.cfg"
if (Test-Path $Autoexec) {
  $autoText = Get-Content -LiteralPath $Autoexec -Raw
  if ($autoText -notmatch '(?m)^\s*exec\s+bmvr') {
    Add-Content -LiteralPath $Autoexec -Value "`r`nexec bmvr`r`n"
    Write-Host "Appended 'exec bmvr' to $Autoexec"
  }
} else {
  Set-Content -LiteralPath $Autoexec -Value "exec bmvr`r`n" -Encoding ASCII
  Write-Host "Created $Autoexec"
}

# Stock dxvk.conf sets deviceLossOnFocusLoss=True and overrides our built-in
# bms.exe profile (False). Steam verify restores the stock file.
$DxvkConf = Join-Path $DxvkDir "dxvk.conf"
$ConfText = @"
d3d9.deferSurfaceCreation = True
d3d9.deviceLossOnFocusLoss = False
"@
Set-Content -LiteralPath $DxvkConf -Value $ConfText -Encoding ASCII
Write-Host "Set $DxvkConf deviceLossOnFocusLoss=False"

Write-Host ""
Write-Host "Launch from Steam with the existing L4D2VR options, plus:"
Write-Host "  -oldgameui"
Write-Host "The new Black Mesa game UI is upside-down in the headset and stays black after load."
Write-Host "If the launcher video menu is Direct3D 9, also add:"
Write-Host "  -enabledxvk"
Write-Host "That is a verified load-path issue: default/native D3D9 never loads this DLL."
Write-Host "Log: $GameRoot\bmvr_log.txt  (also next to whichever d3d9.dll actually loaded)"
Write-Host "Close the game before the next install."
