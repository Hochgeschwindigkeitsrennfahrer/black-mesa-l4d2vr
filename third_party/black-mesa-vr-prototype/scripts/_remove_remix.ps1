$bm = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
$log = "C:\Users\Henning\Documents\cursor\black mesa\scripts\remix_removal_log.txt"
$items = @(
  "$bm\rtx_comp",
  "$bm\rtx-remix",
  "$bm\rtx.conf",
  "$bm\run-bms-rtx.bat",
  "$bm\nrc_session_log.txt",
  "$bm\metrics.txt",
  "$bm\imgui.ini",
  "$bm\bms.dxvk-cache",
  "$bm\bin\.trex",
  "$bm\bin\a_blackmesa-rtx.asi",
  "$bm\bin\blackmesa-rtx.pdb",
  "$bm\bin\artifacts_readme.txt",
  "$bm\bin\d3d9.dll",
  "$bm\bin\d3d9.pdb",
  "$bm\bin\NvRemixLauncher32.exe",
  "$bm\bin\NvRemixLauncher32.pdb",
  "$bm\bin\winmm.dll"
)
$out = New-Object System.Collections.Generic.List[string]
foreach ($p in $items) {
  if (Test-Path $p) {
    try {
      Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction Stop
      $out.Add("REMOVED $p")
    } catch {
      $out.Add("FAILED $p : $($_.Exception.Message)")
    }
  } else {
    $out.Add("SKIP missing $p")
  }
}
$out | Set-Content -Path $log -Encoding UTF8
