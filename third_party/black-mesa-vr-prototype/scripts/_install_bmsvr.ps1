$root = "C:\Users\Henning\Documents\cursor\black mesa"
$bm = "C:\Program Files (x86)\Steam\steamapps\common\Black Mesa"
$src = "$root\BMSVR\build_d3d9\Release\d3d9.dll"
$log = "$root\scripts\install_log.txt"
$lines = New-Object System.Collections.Generic.List[string]

function Copy-Logged($from, $to) {
  Copy-Item -LiteralPath $from -Destination $to -Force
  $lines.Add("COPIED $from -> $to")
}

Copy-Logged $src "$bm\bin\d3d9.dll"
Copy-Logged "$root\BMSVR\thirdparty\openxr\pkg\native\Win32\release\bin\openxr_loader.dll" "$bm\bin\openxr_loader.dll"
Copy-Logged "$root\reference\portal2vr\thirdparty\openvr\bin\win32\openvr_api.dll" "$bm\bin\openvr_api_bmsvr.dll"
# Keep game openvr_api.dll if present; also ensure ours is available as openvr_api.dll for DXVK
if (-not (Test-Path "$bm\bin\openvr_api.dll")) {
  Copy-Logged "$root\reference\portal2vr\thirdparty\openvr\bin\win32\openvr_api.dll" "$bm\bin\openvr_api.dll"
} else {
  $lines.Add("KEEP existing openvr_api.dll")
}

New-Item -ItemType Directory -Force -Path "$bm\VR" | Out-Null
Copy-Logged "$root\BMSVR\assets\bmsvr.cfg" "$bm\bmsvr.cfg"
Copy-Logged "$root\BMSVR\assets\bmsvr.cfg" "$bm\VR\bmsvr.cfg"

Copy-Logged "$root\run-bms-vr.bat" "$bm\run-bms-vr.bat"
$lines.Add("WROTE run-bms-vr.bat")
if (Test-Path "$root\run-bms-vr-steamvr.bat") {
  Copy-Logged "$root\run-bms-vr-steamvr.bat" "$bm\run-bms-vr-steamvr.bat"
  $lines.Add("WROTE run-bms-vr-steamvr.bat")
}
$lines | Set-Content $log -Encoding UTF8
