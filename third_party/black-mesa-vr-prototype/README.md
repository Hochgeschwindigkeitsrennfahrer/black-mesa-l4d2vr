# Black Mesa VR (BMSVR)

OpenXR VR mod for retail **Black Mesa**, using the L4D2VR/Portal2VR inject model (DXVK VR `d3d9.dll` + Source hooks).

## Status

- RTX Remix removed from the game directory
- Combined VR `d3d9.dll` built and installed to `Black Mesa\bin\`
- OpenXR loader installed; launcher: `run-bms-vr.bat`
- Offsets scanned for BM build 19042901 (some still candidates — check `bmsvr_log.txt` after launch)

## Play

1. **Recommended for HMD:** `run-bms-vr-steamvr.bat` (forces 32-bit SteamVR OpenXR). BM is Win32 + DXVK Vulkan and needs `XR_KHR_vulkan_enable`.
2. System default: `run-bms-vr.bat` (does **not** set `XR_RUNTIME_JSON`). If Windows OpenXR is **WMR** (`MixedRealityRuntime.json`), init often fails — WMR 32-bit typically exposes D3D11 only, not Vulkan. Flat/menu still works; HMD path will not.
3. Or Steam launch options: `-window -novid -w 1280 -h 720 +mat_queue_mode 0 +mat_vsync 0 +crosshair 0`  
   (still need an OpenXR runtime that supports Vulkan — prefer the SteamVR bat for headset)

## Rebuild / reinstall

```powershell
.\scripts\rebuild_and_install.ps1
```

Install copies without UAC. If you get Access Denied writing to `Black Mesa\bin`, run **once**:

```powershell
.\scripts\grant_bin_write.ps1
```

(that one script elevates once via UAC to grant your user Modify on `bin`; then AFK rebuilds stay elevation-free).

## Layout

| Path | Role |
| --- | --- |
| `BMSVR/` | Mod sources (hooks, OpenXR, shims) |
| `BMSVR/build_d3d9/` | Combined DXVK VR + BMSVR → `d3d9.dll` |
| `docs/ARCHITECTURE.md` | RE notes |
| `reference/portal2vr/dxvk` | VR DXVK fork used for the build |

## Config

Edit `bmsvr.cfg` next to `bms.exe` (`vr_scale`, `snap_turn`, `roomscale`, …).
