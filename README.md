# Black Mesa VR

L4D2VR’s VR architecture adapted to Steam **Black Mesa** (Win32, DXVK).

Primary reference: https://github.com/keyou91/l4d2vr  
Secondary (BM-specific evidence only): https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr

This is **not** a new VR design. The shipped artifact is a combined `d3d9.dll` (L4D2VR’s DXVK fork + OpenVR + Black Mesa hooks).

Sources live in `src/`. The `L4D2VR/` folder is only include shims so DXVK can `#include "L4D2VR/game.h"`.

## Status (honest)

| Milestone | State |
| --- | --- |
| Implemented | Yes (L4D2VR-style `d3d9.dll`, BM offsets, capture Submit fallback) |
| Compiled | Yes — `build\Release\d3d9.dll` (Win32) |
| Installed | Yes — next to `bms.exe`, `bin\`, and the DXVK folder |
| Launched with this DLL | Yes |
| Runtime-initialized | Yes |
| Head tracking | User-verified with fused stereo (L4D2VR/Portal2 HMD on view copies + `SetViewAngles` + CreateMove) |
| Rendering in headset | Yes — fused stereo Submit (`-oldgameui`) |
| Load / headset-off freeze | Yes — `WaitGetPoses` on a pose-waiter thread (user-verified 2026-08-16) |
| Stereo | User-verified fused window-fit (~1584×1440 on 1440p) and gbmatch squash-blit from the 2560 window into SteamVR-sized eyes. `steamvr_rt` blacked the world. `hmd_world` (LITERAL FullFrame/G-buffer at rec) warped the HMD — persist-skipped. |
| Motion controllers | User-verified: uncoupled viewmodel on the aim controller, FP arms hidden, independent hand markers + finger curl. |
| Weapon in HMD | User-verified 2026-08-19: world-space `v_` gun keeps desktop proportions (view-Y unstretch). Walk-bob/ghosting still open. |
| Multicore (`mat_queue_mode`) | Compiled: real `GetMatQueueMode` + `SetThreadMode` AutoMatQueueMode. **Not user-verified.** If it dies, next launch skips `mat_queue`. |
| Gameplay in headset | Yes with `-oldgameui`. New game UI is upside-down / black after load. |

Acceptance is **visible fused Black Mesa gameplay in the headset**.

## Steam launch options

Keep the existing L4D2VR options:

```
-heapsize 524288 -processheap -high -novid -windowed -oldgameui
```

**Verified UI issue:** Black Mesa's new game UI is upside-down in the headset and gameplay stays black after the load screen. `-oldgameui` is required. Do not drop the L4D2VR options above.

**Verified load issue:** the in-game/launcher video default can be native Direct3D 9. This DLL is DXVK. If the desktop session is not DXVK, nothing in the headset is expected.

If the launcher shows Direct3D 9, add:

```
-enabledxvk
```

That does not replace the options above; it forces the launcher to AddDllDirectory the DXVK folder. We also install `d3d9.dll` next to `bms.exe` and in `bin\` so `LoadLibrary("d3d9.dll")` can still find us.

## Build

Win32 only. From a Developer PowerShell or with VS 2022 Build Tools:

```powershell
.\scripts\build.ps1
```

Output: `build\Release\d3d9.dll`

Drag-and-drop zip (DLL in all three load paths, `VR\`, SteamVR bindings, resolution notes):

```powershell
.\scripts\pack_release.ps1
```

Output: `dist\Black-Mesa-VR-drop-in.zip` — copy the folder contents into `Steam\steamapps\common\Black Mesa` (next to `bms.exe`). Read `BMVR_README.txt` in the zip for launch options and `RenderScale`.

## Install

**Quit Black Mesa first.** `scripts\install.ps1` will stop `bms.exe` if it is still running, because Windows will not replace a mapped DLL.

```powershell
.\scripts\install.ps1
```

Copies `d3d9.dll` and SteamVR’s 32-bit `openvr_api.dll` into:

1. `Black Mesa\bin\thirdparty\dxvk-windows-x86\` (DXVK folder)
2. `Black Mesa\bin\` (`LoadLibrary("d3d9.dll")` search)
3. `Black Mesa\` (next to `bms.exe`, L4D2VR-style)

Proof the new DLL loaded: `Black Mesa\bmvr_log.txt` appears **this session** (and a DXVK log whose build line is MSVC `x86`, not stock gcc 2.6.2).

## Test

1. Quit the game if it is running.
2. Run `.\scripts\install.ps1`.
3. Start **SteamVR**.
4. Launch Black Mesa from Steam. Prefer Vulkan/DXVK in the launcher, or `-enabledxvk`.
5. Confirm `bmvr_log.txt` starts with `BMVR d3d9.dll loaded`.
6. Look in the headset. Do not treat compile/load/tracking as success.

## Resolution and controllers

`scripts\install.ps1` copies `VR\SteamVRActionManifest` next to `bms.exe` and creates `VR\config.txt` if it is missing (existing config is kept).

- **RenderScale** (restart required): `1.0` is SteamVR’s recommended per-eye size (can be taller than the game window). `GetScreenSize` and the swapchain stay at the HWND. World G-buffers stay at the window (`hmd_world` persist-skip); the window scene is scaled into the eyes. Crash-sticky `hmd_offscreen` for eye size. SteamVR overlay SS still feeds the recommended size.
- **HP Reverb G2**: left stick walk, right stick turn, right trigger attack, left trigger alt-fire, right B use, right A jump, right grip crouch, left grip reload, left-stick click recenter, right-stick click flashlight, **left menu/system = pause (ESC)**, **left X = sprint**. Left Y next weapon. Previous weapon stays on the right-stick dpad. Pause is a window `VK_ESCAPE`, not `gameui_activate`.
- **Uncoupled gun / motion aim** (sd805 L4D2VR + Portal 2 VR, not keyou91 hands): `CalcViewModelView` places the first-person weapon at the right controller (left if `LeftHanded=true`). CreateMove aims with that controller; the headset still drives the camera. Stick walk stays look-relative. Per-weapon viewmodel offsets apply from the `v_` model name; `ViewmodelPosOffset*` is the fallback. Recenter (left-stick click) also zeros snap/smooth yaw if `RecenterResetsYaw=true`. Haptics pulse on fire/use/reload/snap-turn. Bullets still spawn from the eyes until a `Weapon_ShootPosition` hook exists. Tune `ViewmodelPosOffset*` / `ControllerPitchTilt` / `IPDScale` / `HeightOffset` in `VR\config.txt` (restart after edits; config is read at DLL load). Existing `config.txt` is kept on install — missing keys use the DLL defaults. HEV/marine FP arms are hidden; independent SteamVR glove meshes (`VrHandsGlovesEnabled`) draw at each controller. Do not parent the gun to a glove wrist.
- **Multicore / QoL (compiled, not user-verified):** `GetMatQueueMode` is the real `IMaterialSystem` vfunc 11 so DXVK's queued Present lock can run. `AutoMatQueueMode` switches 0 in menu/load/pause and 2 in gameplay via `SetThreadMode` (never `ClientCmd`). Crash-sticky `mat_queue`. ICvar probe sets `crosshair 0`, motion blur/grain off, `engine_no_focus_sleep 0`, `fps_max` = HMD Hz, `cl_bob* 0`. `DrawModelExecute` notes viewmodels and can hide the local world-model (`HideLocalPlayerModel`).
- If sticks do nothing, SteamVR Bindings for **Black Mesa** / this app — confirm the G2 layout is the default we installed. Log lines: `SetActionManifestPath … err=0`, `UpdateActionState err=0`, `VR input walk=`.

If the game exits while trying an L4D2VR mechanism, relaunch: crash-sticky `bmvr_in_*.flag` and durable `bmvr_skip.txt` next to `bms.exe` disable only that attempt.

Immediate-crash loop:

```powershell
.\scripts\boot_probe.ps1
```

See `docs/` and `AGENTS.md`.
