$bm = 'C:\Program Files (x86)\Steam\steamapps\common\Black Mesa\bin'
$src = 'C:\Users\Henning\Documents\cursor\black mesa\reference\portal2vr\thirdparty\openvr\bin\win32\openvr_api.dll'
$log = 'C:\Users\Henning\Documents\cursor\black mesa\scripts\openvr_fix_log.txt'
$backup = Join-Path $bm 'openvr_api.dll.stock_sourcevr'
try {
  if ((Test-Path (Join-Path $bm 'openvr_api.dll')) -and -not (Test-Path $backup)) {
    Copy-Item (Join-Path $bm 'openvr_api.dll') $backup -Force
  }
  Copy-Item $src (Join-Path $bm 'openvr_api.dll') -Force
  $len = (Get-Item (Join-Path $bm 'openvr_api.dll')).Length
  "OK replaced openvr_api.dll size=$len" | Set-Content $log
} catch {
  "FAIL $($_.Exception.Message)" | Set-Content $log
}
