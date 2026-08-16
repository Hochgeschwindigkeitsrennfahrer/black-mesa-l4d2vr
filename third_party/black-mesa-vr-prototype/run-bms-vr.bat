@echo off
REM Black Mesa VR launcher — uses the system OpenXR runtime (WMR, SteamVR, etc.).
REM Does NOT set XR_RUNTIME_JSON. For SteamVR-only, use run-bms-vr-steamvr.bat.
REM NOTE: BM is Win32+DXVK Vulkan (needs XR_KHR_vulkan_enable). WMR 32-bit often
REM only has D3D11 → OpenXR CreateInstance fails. Prefer run-bms-vr-steamvr.bat for HMD.
set DXVK_HUD=0
set DXVK_ASYNC=1
REM Optional SteamVR 32-bit override (BM is Win32). Prefer run-bms-vr-steamvr.bat instead:
REM set "XR_RUNTIME_JSON=C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win32.json"
cd /d "%~dp0"
if exist "bms.exe" (
  start "" "bms.exe" -window -novid -w 1280 -h 720 +mat_queue_mode 0 +mat_vsync 0 +crosshair 0 +fps_max 0 -dxlevel 95 +map bm_c1a1b
) else (
  echo Copy this bat next to bms.exe or run from the Black Mesa install folder.
  pause
)
