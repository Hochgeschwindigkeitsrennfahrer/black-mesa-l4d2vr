# Launch Black Mesa, detect an immediate crash, persist one skip, relaunch.
# The DLL also consumes leftover bmvr_in_*.flag files on the next process.
# Success = bms.exe still alive after Game initialized + heartbeat, for -StableSec.
param(
  [int]$MaxAttempts = 6,
  [int]$AppearTimeoutSec = 75,
  [int]$StableSec = 25
)

$ErrorActionPreference = "Continue"
$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
$Steam = "C:\Program Files (x86)\Steam\steam.exe"
$AppId = 362890
$Root = Split-Path -Parent $PSScriptRoot
$ProbeDir = Join-Path $Root "probe_logs"
New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null

$SkipOrder = @("named_rt", "stereo_rv", "wait_idle", "abs_view", "hmd_swap")
$SkipPath = Join-Path $GameRoot "bmvr_skip.txt"

function Get-BmsProcess {
  Get-Process -Name "bms" -ErrorAction SilentlyContinue
}

function Stop-Bms {
  $procs = @(Get-BmsProcess)
  if ($procs.Count -eq 0) { return }
  Write-Host "Stopping bms.exe PID $($procs.Id -join ', ')"
  cmd /c "taskkill /F /IM bms.exe /T >nul 2>&1"
  Start-Sleep -Seconds 2
  $left = @(Get-BmsProcess)
  if ($left.Count -gt 0) {
    Write-Host "WARN: bms.exe still present PID $($left.Id -join ', ') (debugger attach?). Ignoring stale processes."
  }
}

function Get-SkipList {
  if (-not (Test-Path $SkipPath)) { return @() }
  Get-Content $SkipPath | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -notmatch '^#' }
}

function Add-Skip([string]$Name, [string]$Why) {
  if (-not $Name) { return }
  $have = @(Get-SkipList)
  if ($have -contains $Name) { return }
  Add-Content -Path $SkipPath -Value $Name
  Write-Host "SKIP +$Name ($Why)"
  Add-Content -Path (Join-Path $ProbeDir "skips.log") -Value "$(Get-Date -Format o) +$Name $Why"
}

function Archive-Logs([int]$Attempt) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  foreach ($name in @("bmvr_log.txt", "bmvr_heartbeat.txt", "bms_d3d9.log")) {
    $src = Join-Path $GameRoot $name
    if (Test-Path $src) {
      Copy-Item $src (Join-Path $ProbeDir ("a{0}_{1}_{2}" -f $Attempt, $stamp, $name)) -Force
    }
  }
}

function Get-InProgressFlags {
  Get-ChildItem $GameRoot, (Join-Path $GameRoot "bin"), (Join-Path $GameRoot "bin\thirdparty\dxvk-windows-x86") -Filter "bmvr_in_*.flag" -ErrorAction SilentlyContinue
}

function Infer-Skip {
  $flags = @(Get-InProgressFlags)
  foreach ($f in $flags) {
    if ($f.Name -match '^bmvr_in_(.+)\.flag$') { return $Matches[1] }
  }
  $log = Join-Path $GameRoot "bmvr_log.txt"
  if (Test-Path $log) {
    $tail = @(Get-Content $log -Tail 30)
    $text = $tail -join "`n"
    if ($text -match 'named eye RTs') { return "named_rt" }
    if ($text -match 'stereo RenderView') { return "stereo_rv" }
    if ($text -match 'WaitDeviceIdle|wait_idle') { return "wait_idle" }
    if ($text -match 'absolute HMD|absHmd=1') { return "abs_view" }
    if ($text -match 'HMD recommended|hmdSwap=1') { return "hmd_swap" }
  }
  $have = @(Get-SkipList)
  foreach ($n in $SkipOrder) {
    if ($have -notcontains $n) { return $n }
  }
  return $null
}

function Test-GameWindow {
  $hwnd = [BmvrProbe.Native]::FindWindowA("Valve001", $null)
  return [int64]$hwnd -ne 0
}

Add-Type -Namespace BmvrProbe -Name Native -MemberDefinition @"
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Ansi)]
    public static extern System.IntPtr FindWindowA(string lpClassName, string lpWindowName);
"@ -ErrorAction SilentlyContinue

Write-Host "BMVR boot probe: max $MaxAttempts attempts, appear ${AppearTimeoutSec}s, stable ${StableSec}s"
Write-Host "Game: $GameRoot"

# Seed skip from the crash that just happened.
foreach ($f in @(Get-InProgressFlags)) {
  if ($f.Name -match '^bmvr_in_(.+)\.flag$') {
    Add-Skip $Matches[1] "leftover in-progress flag from previous crash"
  }
}

for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
  Write-Host ""
  Write-Host "======== attempt $attempt / $MaxAttempts skips=$(@(Get-SkipList) -join ',') ========"
  Stop-Bms
  Archive-Logs $attempt
  Remove-Item (Join-Path $GameRoot "bmvr_log.txt") -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $GameRoot "bmvr_heartbeat.txt") -ErrorAction SilentlyContinue

  if (-not (Test-Path $Steam)) { throw "Steam not found: $Steam" }
  $launchAt = Get-Date
  Start-Process -FilePath $Steam -ArgumentList @("-applaunch", "$AppId")
  Write-Host "Launched steam -applaunch $AppId"

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
    $skip = Infer-Skip
    if ($skip) { Add-Skip $skip "process never appeared" }
    continue
  }
  Write-Host "bms.exe PID $($appeared.Id) started $($appeared.StartTime)"

  $stableUntil = (Get-Date).AddSeconds($StableSec)
  $sawInit = $false
  $ok = $false
  while ((Get-Date) -lt $stableUntil.AddSeconds(90)) {
    $alive = Get-BmsProcess | Where-Object { $_.Id -eq $appeared.Id }
    if (-not $alive) {
      Write-Host "CRASH: bms.exe exited"
      $skip = Infer-Skip
      if ($skip) { Add-Skip $skip "process exited during boot" }
      $ok = $false
      break
    }
    $log = Join-Path $GameRoot "bmvr_log.txt"
    if (Test-Path $log) {
      $content = Get-Content $log -Raw -ErrorAction SilentlyContinue
      if ($content -match 'Game initialized') { $sawInit = $true }
    }
    $hb = Join-Path $GameRoot "bmvr_heartbeat.txt"
    $hbFresh = $false
    if (Test-Path $hb) {
      $hbFresh = ((Get-Date) - (Get-Item $hb).LastWriteTime).TotalSeconds -lt 3
    }
    $hasWindow = Test-GameWindow
    if ($sawInit -and $hbFresh -and ((Get-Date) -ge $stableUntil)) {
      $ok = $true
      Write-Host "STABLE: initialized heartbeat=fresh window=$hasWindow after ${StableSec}s"
      break
    }
    Start-Sleep -Seconds 2
  }

  if ($ok) {
    Write-Host "PROBE_OK attempt=$attempt skips=$(@(Get-SkipList) -join ',')"
    Write-Host "AGENT_LOOP_WAKE_bmvr_boot {`"prompt`":`"boot probe succeeded; game is running. Read probe_logs and bmvr_log.txt and summarize.`"}"
    exit 0
  }

  if (Get-BmsProcess) {
    Write-Host "HANG: still running but not stable (init=$sawInit)"
    Stop-Bms
  }
  $skip = Infer-Skip
  if ($skip) { Add-Skip $skip "boot did not become stable" }
}

Write-Host "PROBE_FAIL after $MaxAttempts attempts skips=$(@(Get-SkipList) -join ',')"
Write-Host "AGENT_LOOP_WAKE_bmvr_boot {`"prompt`":`"boot probe failed after max attempts. Read probe_logs and bmvr_skip.txt and decide the next code change.`"}"
exit 1
