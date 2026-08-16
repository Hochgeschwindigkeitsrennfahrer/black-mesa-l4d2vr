# Scan Black Mesa client/engine/materialsystem for Source VR hook signatures.
param(
  [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
)

$ErrorActionPreference = "Stop"

function Find-Pattern([byte[]]$data, [string]$pattern) {
  $parts = $pattern -split '\s+' | Where-Object { $_ }
  $bytes = @()
  $mask = @()
  foreach ($p in $parts) {
    if ($p -eq '?' -or $p -eq '??') { $bytes += 0; $mask += $false }
    else { $bytes += [Convert]::ToByte($p, 16); $mask += $true }
  }
  $len = $bytes.Count
  for ($i = 0; $i -le $data.Length - $len; $i++) {
    $ok = $true
    for ($j = 0; $j -lt $len; $j++) {
      if ($mask[$j] -and $data[$i + $j] -ne $bytes[$j]) { $ok = $false; break }
    }
    if ($ok) { return $i }
  }
  return -1
}

$modules = @{
  "client.dll" = Get-ChildItem $GameDir -Recurse -Filter client.dll -ErrorAction SilentlyContinue | Select-Object -First 1
  "engine.dll" = Get-ChildItem $GameDir -Recurse -Filter engine.dll -ErrorAction SilentlyContinue | Select-Object -First 1
  "materialsystem.dll" = Get-ChildItem $GameDir -Recurse -Filter materialsystem.dll -ErrorAction SilentlyContinue | Select-Object -First 1
  "server.dll" = Get-ChildItem $GameDir -Recurse -Filter server.dll -ErrorAction SilentlyContinue | Select-Object -First 1
}

foreach ($k in $modules.Keys) {
  if (-not $modules[$k]) { Write-Host "MISSING: $k"; }
  else { Write-Host "FOUND: $($modules[$k].FullName)" }
}

$patterns = @(
  @{ Name = "RenderView"; Module = "client.dll"; Sig = "55 8B EC 81 EC ? ? ? ? 53 56 57 8B D9" },
  @{ Name = "CalcViewModelView"; Module = "client.dll"; Sig = "55 8B EC 83 EC 48 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 8B 10" },
  @{ Name = "g_pClientMode"; Module = "client.dll"; Sig = "89 04 B5 ? ? ? ? E8" },
  @{ Name = "AdjustEngineViewport"; Module = "client.dll"; Sig = "55 8B EC 8B 0D ? ? ? ? 85 C9 74 17" },
  @{ Name = "DrawModelExecute"; Module = "engine.dll"; Sig = "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B" },
  @{ Name = "VGui_Paint"; Module = "engine.dll"; Sig = "55 8B EC E8 ? ? ? ? 8B 10 8B C8 8B 52 38" },
  @{ Name = "GetRenderTarget"; Module = "materialsystem.dll"; Sig = "83 79 4C 00" },
  @{ Name = "Viewport"; Module = "materialsystem.dll"; Sig = "55 8B EC 83 EC 28 8B C1" },
  @{ Name = "PushRenderTargetAndViewport"; Module = "materialsystem.dll"; Sig = "55 8B EC 83 EC 24 8B 45 08 8B 55 10 89" },
  @{ Name = "PopRenderTargetAndViewport"; Module = "materialsystem.dll"; Sig = "56 8B F1 83 7E 4C 00" },
  @{ Name = "EyePosition"; Module = "server.dll"; Sig = "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3" }
)

Write-Host ""
Write-Host "=== Signature scan results (file offsets) ==="
$cache = @{}
foreach ($p in $patterns) {
  $mod = $modules[$p.Module]
  if (-not $mod) {
    Write-Host ("{0,-32} {1} MISSING MODULE" -f $p.Name, $p.Module)
    continue
  }
  if (-not $cache.ContainsKey($p.Module)) {
    $cache[$p.Module] = [System.IO.File]::ReadAllBytes($mod.FullName)
  }
  $off = Find-Pattern $cache[$p.Module] $p.Sig
  if ($off -ge 0) {
    Write-Host ("{0,-32} {1} +0x{2:X}" -f $p.Name, $p.Module, $off)
  } else {
    Write-Host ("{0,-32} {1} NOT FOUND" -f $p.Name, $p.Module)
  }
}

Write-Host ""
Write-Host "Update BMSVR/offsets.h with the found offsets, then rebuild."
