# One-shot: grant the current user Modify on Black Mesa\bin (and game root for bmsvr.cfg).
# This WILL trigger one UAC prompt. Run once, then use rebuild_and_install.ps1 without elevation.
# Usage: powershell -ExecutionPolicy Bypass -File .\scripts\grant_bin_write.ps1
$ErrorActionPreference = "Stop"
$bmRoot = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
$bmBin = Join-Path $bmRoot "bin"
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
  Write-Host "Elevating once (UAC) to grant write access for: $user"
  $self = $MyInvocation.MyCommand.Path
  Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$self`"" -Verb RunAs -Wait
  exit $LASTEXITCODE
}

if (-not (Test-Path $bmBin)) {
  throw "Black Mesa bin not found: $bmBin"
}

Write-Host "Granting Modify to $user on:"
Write-Host "  $bmBin"
Write-Host "  $bmRoot (bmsvr.cfg)"
icacls $bmBin /grant "${user}:(OI)(CI)M" /T | Out-Host
if ($LASTEXITCODE -ne 0) { throw "icacls failed on bin (exit $LASTEXITCODE)" }
icacls $bmRoot /grant "${user}:M" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "icacls failed on game root (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "Done. Future .\scripts\rebuild_and_install.ps1 runs need no UAC."
pause
