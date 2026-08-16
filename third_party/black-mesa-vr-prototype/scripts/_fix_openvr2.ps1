$bm = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa\bin"
$log = "C:\Users\Henning\Documents\cursor\black mesa\scripts\openvr_fix_log.txt"
$lines = New-Object System.Collections.Generic.List[string]

# Restore stock Source openvr for ABI compatibility if anything still loads it
$stock = Join-Path $bm "openvr_api.dll.stock_sourcevr"
if (Test-Path $stock) {
  Copy-Item $stock (Join-Path $bm "openvr_api.dll") -Force
  $lines.Add("RESTORED stock openvr_api.dll")
}

# Disable Valve sourcevr — conflicts with inject VR / wrong OpenVR ABI
$svr = Join-Path $bm "sourcevr.dll"
$disabled = Join-Path $bm "sourcevr.dll.disabled"
if (Test-Path $svr) {
  if (Test-Path $disabled) { Remove-Item $disabled -Force }
  Rename-Item $svr $disabled
  $lines.Add("DISABLED sourcevr.dll")
} elseif (Test-Path $disabled) {
  $lines.Add("sourcevr already disabled")
}

$lines | Set-Content $log
