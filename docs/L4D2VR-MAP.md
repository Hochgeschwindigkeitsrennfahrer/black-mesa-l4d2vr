# L4D2VR → Black Mesa map

Inspected `third_party/l4d2vr` for the mechanisms that actually produce VR, then mapped each onto this install.

The project work folder is `src/`, not `L4D2VR/`. `L4D2VR/` exists only because DXVK `#include "L4D2VR/..."`.

## Load

- L4D2VR: replace `d3d9.dll` next to the game exe.
- BMVR: that **and** `bin\` **and** `bin\thirdparty\dxvk-windows-x86\`, because BM’s launcher uses `SetDefaultDllDirectories` + `AddDllDirectory` instead of always `LoadLibraryW` the DXVK full path.

`DllMain` → `InitBMVR` thread → `g_Game = new Game()`. Combined DLL name remains `d3d9`.

## Source access

Same wait + `VClient018`, `VEngineClient015`, `VMaterialSystem081`, `VClientEntityList003`.

`GetMatQueueMode` = `IMaterialSystem` vfunc **11** `GetThreadMode`.

## VR runtime

OpenVR scene app, `WaitGetPoses`, `Submit` + `LockSubmissionQueue`. Vulkan Y-flip on texture bounds.

`CreateDevice` retries L4D2VR `VR_Init` + HMD swapchain size (no `ExitProcess` on failure).

## Stereo / camera

L4D2VR: two `RenderView` calls with per-eye `CViewSetup` into named eye RTs.

BMVR: **retry that** with BM’s 3-arg `RenderView` (`client.dll+0x207730`). Nested/offscreen calls pass through. If the process dies, `bmvr_in_stereo_rv.flag` falls back to one RenderView + horizontal crop of a captured frame.

Look: relative yaw in `CreateMove` after LevelInit gate (reject `background*`). Absolute HMD into `CViewSetup` is retried on the stereo path only (not `cmd->viewangles`).

## Getting the game image

L4D2VR: `CreateNamedRenderTargetTextureEx` (`leftEye0` / `rightEye0`) with `m_CreatingTextureID`.

BMVR: retry that **without** poking L4D2’s `isGameRunning` offset (wrong on BM and a likely prototype crash). If it returns null or AVs (SEH), private `CreateTexture` + unbind/pre-Present capture.

## Sync

Retry `WaitDeviceIdle` after `TransferSurface`. Crash-sticky disable if the process dies.

## L4D2-only (not ported)

Melee/terror weapons, workshop, ozz hands, pose relay, Neko HDR post, ReShade VR compat, desktop HUD overlays, queued compositor worker, index-buffer / datacache forcing in `dllmain.cpp`, `ExitProcess` on `VR_Init` failure.
