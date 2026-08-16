# Launch Black Mesa via Steam, watch bmvr_log + process, print a diagnosis.
# Exit 0 = process still alive after StableSec with Game initialized.
# Exit 1 = crash or never started.
param(
  [int]$AppearTimeoutSec = 90,
  [int]$StableSec = 50,
  [string]$Map = ""
)

$ErrorActionPreference = "Continue"
$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
$Steam = "C:\Program Files (x86)\Steam\steam.exe"
$AppId = 362890
$Root = Split-Path -Parent $PSScriptRoot
$ProbeDir = Join-Path $Root "probe_logs"
New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null

function Get-BmsProcess { Get-Process -Name "bms" -ErrorAction SilentlyContinue }
function Stop-Bms {
  cmd /c "taskkill /F /IM bms.exe /T >nul 2>&1"
  Start-Sleep -Seconds 2
}

function Diagnose {
  $log = Join-Path $GameRoot "bmvr_log.txt"
  $dx = Join-Path $GameRoot "bms_d3d9.log"
  $hb = Join-Path $GameRoot "bmvr_heartbeat.txt"
  Write-Host "---- diagnosis ----"
  if (Test-Path $hb) { Write-Host ("heartbeat: " + (Get-Content $hb -Raw).Trim()) }
  if (Test-Path $log) {
    $text = Get-Content $log -Raw
    $tail = @(Get-Content $log -Tail 40)
    Write-Host "log tail:"
    $tail | ForEach-Object { Write-Host $_ }
    if ($text) {
      $ticks = [regex]::Matches($text, 'present tick n=(\d+) ~(\d+)fps')
      if ($ticks.Count -gt 0) {
        $last = $ticks[$ticks.Count - 1]
        Write-Host ("FINDING: last present tick n={0} fps={1}" -f $last.Groups[1].Value, $last.Groups[2].Value)
      }
    }
    if ($text -match 'eL=101') { Write-Host "FINDING: OpenVR Submit DoNotHaveFocus (101)" }
    if ($text -match 'direct-eyes') { Write-Host "FINDING: stereo/direct eye Submit path" }
    if ($text -match 'Stereo RenderView copies') { Write-Host "FINDING: stereo RenderView ran" }
    if ($text -match 'Named eye RTs ready') { Write-Host "FINDING: named eye RTs created" }
    if ($text -match 'Letterbox capture') { Write-Host "FINDING: letterboxed 16:9 capture into HMD-aspect eyes" }
    if ($text -match 'WaitGetPoses DoNotHaveFocus') { Write-Host "FINDING: compositor focus not acquired" }
    $releases = ([regex]::Matches($text, 'Released VR render targets')).Count
    Write-Host "FINDING: VR texture reset count=$releases"
    if ($text -match 'LevelInit map=') { Write-Host "FINDING: reached LevelInit" }
    if ($text -match 'hmdSwap=(\d+) waitIdle=(\d+)') { Write-Host "FINDING: hmdSwap=$($Matches[1]) waitIdle=$($Matches[2])" }
  }
  if (Test-Path $dx) {
    $dxText = Get-Content $dx -Raw
    $resets = ([regex]::Matches($dxText, 'Device reset')).Count
    $skips = ([regex]::Matches($dxText, 'skipping identical windowed Reset')).Count
    Write-Host "FINDING: d3d Device reset=$resets identical-skip=$skips"
  }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
foreach ($name in @("bmvr_log.txt", "bmvr_heartbeat.txt", "bms_d3d9.log")) {
  $src = Join-Path $GameRoot $name
  if (Test-Path $src) {
    Copy-Item $src (Join-Path $ProbeDir ("watch_{0}_{1}" -f $stamp, $name)) -Force
  }
}

Stop-Bms
Remove-Item (Join-Path $GameRoot "bmvr_log.txt") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $GameRoot "bmvr_heartbeat.txt") -ErrorAction SilentlyContinue

if (-not (Test-Path $Steam)) { throw "Steam not found: $Steam" }
$launchAt = Get-Date
$steamArgs = @("-applaunch", "$AppId")
if ($Map) { $steamArgs += @("+map", $Map) }
Start-Process -FilePath $Steam -ArgumentList $steamArgs
Write-Host "Launched steam $($steamArgs -join ' ') at $launchAt"

$appeared = $null
$deadline = (Get-Date).AddSeconds($AppearTimeoutSec)
while ((Get-Date) -lt $deadline) {
  $candidates = @(Get-BmsProcess | Where-Object { $_.StartTime -ge $launchAt.AddSeconds(-2) })
  if ($candidates.Count -gt 0) {
    $appeared = $candidates | Sort-Object StartTime -Descending | Select-Object -First 1
    break
  }
  Start-Sleep -Seconds 1
}
if (-not $appeared) {
  Write-Host "FAIL: bms.exe never started"
  Diagnose
  Write-Host 'AGENT_LOOP_WAKE_bmvr_watch {"prompt":"watch_launch: bms.exe never started. Read probe_logs and fix."}'
  exit 1
}
Write-Host "bms.exe PID $($appeared.Id) started $($appeared.StartTime)"

$stableUntil = (Get-Date).AddSeconds($StableSec)
$sawInit = $false
$ok = $false
$lowFpsHits = 0
while ((Get-Date) -lt $stableUntil.AddSeconds(30)) {
  $alive = Get-BmsProcess | Where-Object { $_.Id -eq $appeared.Id }
  if (-not $alive) {
    Write-Host "CRASH: bms.exe exited after $([int]((Get-Date) - $appeared.StartTime).TotalSeconds)s"
    Diagnose
    Write-Host 'AGENT_LOOP_WAKE_bmvr_watch {"prompt":"watch_launch: game crashed. Read bmvr_log.txt and bms_d3d9.log, fix, rebuild, install, and run scripts/watch_launch.ps1 again."}'
    exit 1
  }
  $log = Join-Path $GameRoot "bmvr_log.txt"
  $content = ""
  if (Test-Path $log) {
    $content = Get-Content $log -Raw -ErrorAction SilentlyContinue
    if ($content -match 'Game initialized') { $sawInit = $true }
  }
  $hb = Join-Path $GameRoot "bmvr_heartbeat.txt"
  $hbFresh = $false
  if (Test-Path $hb) {
    $hbFresh = ((Get-Date) - (Get-Item $hb).LastWriteTime).TotalSeconds -lt 3
  }
  $presentOk = $false
  $fps = -1
  if ($content) {
    $ticks = [regex]::Matches($content, 'present tick n=(\d+) ~(\d+)fps')
    if ($ticks.Count -gt 0) {
      $last = $ticks[$ticks.Count - 1]
      $tickN = [int]$last.Groups[1].Value
      $fps = [int]$last.Groups[2].Value
      $logAge = ((Get-Date) - (Get-Item $log).LastWriteTime).TotalSeconds
      if ($tickN -ge 30 -and $logAge -lt 4) { $presentOk = $true }
      if ($fps -ge 0 -and $fps -lt 10 -and $tickN -ge 30) { $lowFpsHits++ }
    }
  }
  if ($lowFpsHits -ge 8) {
    Write-Host "HANG: ~1 FPS present loop fps=$fps"
    Diagnose
    Stop-Bms
    Write-Host 'AGENT_LOOP_WAKE_bmvr_watch {"prompt":"watch_launch: ~1 FPS hang. Read bmvr_log.txt, stop creating/submitting VR work during menu/load, rebuild, install, rerun watch_launch.ps1."}'
    exit 1
  }
  if ($sawInit -and $hbFresh -and $presentOk -and ((Get-Date) -ge $stableUntil)) {
    $ok = $true
    break
  }
  Start-Sleep -Seconds 2
}

if ($ok) {
  Write-Host "WATCH_OK pid=$($appeared.Id) init=1 after ${StableSec}s"
  Diagnose
  Write-Host 'AGENT_LOOP_WAKE_bmvr_watch {"prompt":"watch_launch: process survived. Read bmvr_log.txt Submit codes and tell the user the status. Do not claim headset gameplay unless eL=0 repeating after LevelInit of a real map."}'
  exit 0
}

Write-Host "WATCH_FAIL still running but not stable init=$sawInit"
Diagnose
Stop-Bms
Write-Host 'AGENT_LOOP_WAKE_bmvr_watch {"prompt":"watch_launch: hang without Game initialized. Read logs, fix, rebuild, install, rerun watch_launch.ps1."}'
exit 1
