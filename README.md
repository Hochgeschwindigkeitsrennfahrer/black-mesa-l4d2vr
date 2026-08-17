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
| Head tracking | Yes — relative look on a RenderView copy (`-oldgameui`) |
| Rendering in headset | Yes — 16:9 capture Submit, both eyes the same (`-oldgameui`) |
| Load / headset-off freeze | Yes — `WaitGetPoses` on a pose-waiter thread (user-verified 2026-08-16) |
| Stereo | User-verified fused 1584×1440 double RenderView (`-oldgameui`). Was upside-down from a Vulkan v-flip on that blit; flip removed. **Upright not verified this build.** |
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

If the game exits while trying an L4D2VR mechanism, relaunch: crash-sticky `bmvr_in_*.flag` and durable `bmvr_skip.txt` next to `bms.exe` disable only that attempt.

Immediate-crash loop:

```powershell
.\scripts\boot_probe.ps1
```

See `docs/` and `AGENTS.md`.
