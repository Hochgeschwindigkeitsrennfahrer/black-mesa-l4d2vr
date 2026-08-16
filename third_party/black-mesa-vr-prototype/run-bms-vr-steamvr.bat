@echo off
REM Black Mesa VR launcher — REQUIRED for HMD with current DXVK Vulkan OpenXR stack.
REM Forces 32-bit SteamVR OpenXR (steamxr_win32.json). BM is Win32; do not use steamxr_win64.json.
REM run-bms-vr.bat uses system OpenXR (often WMR) which lacks XR_KHR_vulkan_enable → CreateInstance fails.
set DXVK_HUD=0
set DXVK_ASYNC=1
set "XR_RUNTIME_JSON=C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win32.json"
cd /d "%~dp0"
if exist "bms.exe" (
  start "" "bms.exe" -window -novid -w 1280 -h 720 +mat_queue_mode 0 +mat_vsync 0 +crosshair 0 +fps_max 0 -dxlevel 95 +map bm_c1a1b
) else (
  echo Copy this bat next to bms.exe or run from the Black Mesa install folder.
  pause
)
