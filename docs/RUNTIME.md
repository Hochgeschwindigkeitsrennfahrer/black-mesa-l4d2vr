# Runtime observations

## Why nothing happened last session

- `bms.exe` PID 17044 started **19:40:51**. Our `d3d9.dll` was copied at **19:49:24**. The running process never mapped the new file.
- `bmvr_log.txt` did not exist — our `DllMain` never ran.
- `bms_d3d9.log` last write **19:01:17** (stock DXVK 2.6.2 gcc). The 19:40 session did **not** write a DXVK log → **native D3D9**, not DXVK.
- Install was only `bin\thirdparty\dxvk-windows-x86\d3d9.dll`. On modern Windows the launcher `SetDefaultDllDirectories(0xC00)` and **skips** full-path `LoadLibraryW` of that file (`FUN_00401470` / `FUN_00401620`). If DXVK is off, that folder is never `AddDllDirectory`'d, so `shaderapidx9` `LoadLibrary("d3d9.dll")` finds System32.

## Load path (what we do now)

Install to:

- `bin\thirdparty\dxvk-windows-x86\` (when DXVK is on)
- `bin\` (USER_DIRS search; added before game root)
- next to `bms.exe` (L4D2VR; game root is also AddDllDirectory'd)

Optional Steam extra: `-enabledxvk` if the launcher video menu is Direct3D 9.

## Verified fail: OpenVR Submit DoNotHaveFocus (101)

2026-08-16 after Reset skip: `OpenVR Submit ... eL=101 eR=101` then unbind capture then crash. 101 is `VRCompositorError_DoNotHaveFocus`. OpenVR throttles WaitGetPoses to 10 Hz in that state. Cause: `GetLastPoses` does not take scene focus. Fix: always `WaitGetPoses`, `CompositorBringToFront`, do not Submit until focus, skip unbind StretchRect, skip `wait_idle`.

2026-08-16 after `hmd_swap` skip: windowed start, capture+Submit of the loading bar to the headset (`eL=0 eR=0`, 1920×1080 from FullFrameFB 2048×2048). Then `IDirect3DDevice9::Reset` with **Windowed=false**, DXVK `Setting display mode: 1920x1080@0` in a tight loop, ~1 FPS, crash on `LevelInit background01`. Crash left the monitor at 1920×1080.

Cause: exclusive fullscreen + SteamVR focus. `d3d9.deviceLossOnFocusLoss = True` in BM's `dxvk.conf` then reports device lost, Source Resets again.

Fix: `DXVK_FORCE_WINDOWED=1` at `DllMain`, and Reset/CreateDevice keep `Windowed=TRUE` **on a local copy** (do not write it back into the game's `D3DPRESENT_PARAMETERS`). Identical 1920×1080 windowed Resets are skipped so Source's fullscreen retry cannot destroy the swapchain and VR eye textures every frame.

2026-08-16 follow-up: after the Windowed force, the game **stayed windowed** but still Reset-looped (`BMVR refusing exclusive fullscreen` then `Device reset` + recreate 1920×1080 eyes until crash). Cause: we mutated the caller's `Windowed` flag to TRUE; Source copied it back, saw mismatch with saved fullscreen settings, and Reset again. `WaitGetPoses` then blocked ~1s because Submit never kept up.

## Verified fail: HMD-sized swapchain (`hmd_swap`)

2026-08-16 live run (PID 39344): CreateDevice + `InitOpenVR` set the swapchain / `m_RenderWidth` to OpenVR recommended **3168×3100**. One capture+Submit succeeded (`eL=0 eR=0`), then `Released VR render targets for device reset`. After Reset, `GetRenderTarget` was null, capture NONE, `submitCalls=0`. Desktop was black; headset stayed in the SteamVR waiting room.

Durable skip: `hmd_swap` in `bmvr_skip.txt`. DXVK `Reset` no longer forces the backbuffer to eye size while that skip is set. Capture falls back to `GetBackBuffer` + `StretchRect` when RT0 is missing.

## Also verified fail: named eye RTs (`named_rt`)

`GetBackBufferFormat` returned a pointer, not an `ImageFormat`. Skipped.

## Verified fail: VR textures during menu (`background01`)

2026-08-16: `IsInGame()` is true on `background01`. Creating `IDirect3DVR9` eye textures then made DXVK `Present` `VrResolveEyeSurfacesToSubmit` / `VrMirrorEyeToDesktopBackBuffer` every frame. Desktop: 1 FPS black load bar; log stopped after the first PrePresent capture (latch) even when the process was still alive. MinHook on `LevelInit` crashed inside the original on `background01`.

Fix: poll `IEngineClient::GetLevelNameShort` (vtable slot 52), reject `background*`, do not create eye textures / capture / WaitGetPoses / Submit until a real map. Keep `LevelInit` unhooked until menu load is stable.

## Verified: 2D capture in headset (bm_c0a0a)

2026-08-16: repeating `OpenVR submit eL=0 eR=0` at ~90 FPS on `bm_c0a0a`. User confirmed headset image. Remaining: no true stereo (same 16:9 blit both eyes), no HMD camera, vertical stretch (16:9 submitted into HMD aspect ~1.097).

## CViewSetup / RenderView (2026-08-16)

- Hooked `client.dll+0x207730` is **not** `CViewRender::RenderView`. Callers push floats; `fld [ebp+8]`. Pass-through was harmless; stereo copies interpreted a float as `CViewSetup&`.
- Real function: `CBlackMesaViewRender` `IViewRender` vtable slot 6 at `client.dll+0x20EE40`, `ret 0xC` (3 stack args).
- BM `CViewSetup` is `0x148`, not L4D2 `0x6E4`. Origin is `0xBC`, fov `0xB4`, aspect `0xE8`. Writing HMD into L4D2 offsets `0x3C`/`0x48` overwrites `m_matCustomViewMatrix`.

## Verified fail: double RenderView (`stereo_rv`)

2026-08-16: after 90 capture Submits at ~69 FPS, named `leftEye0`/`rightEye0` 1920×1878 created and bound. First L4D2VR-style two-`RenderView` into those RTs died with heartbeat `in_stereo_rv`. `CViewSetup` fields were valid (`fov=106.3`, world origin). Crash-sticky disables double RenderView. Look uses HMD angles on the original setup (`angles` at `0xC8`); stretch uses UV crop of the 16:9 capture.

## Verified fail: absolute HMD on live CViewSetup (`abs_view`)

2026-08-16: load logo submitted (`eL=0`). After ~90 Submits we wrote HMD yaw/pitch onto the live `CViewSetup`. On `bm_c0a0b` the tram camera was `ang=(4.6, 96.5)` and became HMD `(-0.5, 14.7)` — looking off the train into unlit void. Headset went black; logo had been visible because look/crop only started after load. Durable skip: `abs_view`. L4D2VR applies HMD only on stereo view copies, not the caller’s setup.

## Verified fail: projection UV on 16:9 capture (WMR portal 2026-08-16)

Headset / Mixed Reality Portal showed a **left vertical strip** of the pause menu, **upside-down and mirrored**, rest black. Desktop pause menu was upright 16:9. Cause: capture Submit used `GetProjectionRaw` / L4D2VR hidden-area `m_TextureBounds` meant for an HMD-aspect eye RT. On a 16:9 D3D blit those UVs sample a flipped corner. Known-good in-headset session used full-frame capture Submit `{0,0,1,1}` (stretched, both eyes the same). Extra `vMin`/`vMax` swap on that blit submitted upside down.

Fix: capture path Submits `{0,0,1,1}`. Projection bounds stay for a future direct-eye / named-RT path only.

## Menu compositor retry (`menu_vr`)

Main menu never appeared in VR because capture/`WaitGetPoses`/Submit were gated on `m_GameplayEligible` (reject `background*`). That gate was from an earlier 1 FPS hang when DXVK `VrResolveEyeSurfacesToSubmit` ran with `m_StereoRenderViewActive`. The capture path sets that false. Retry private 1080 capture + Submit on any LevelInit map including `background*`. Crash-sticky `bmvr_in_menu_vr.flag`. Pre-LevelInit StretchRect still skipped (empty map name).

## WaitGetPoses on Present thread (verified hang 2026-08-16)

PID 19580 froze with `Responding=False` after `LevelInit map=bm_c0a0b`. No further present ticks. Next call on that thread is `WaitGetPoses`. Same hang when the headset is taken off (WMR stops vsync; WaitGetPoses never returns). Low FPS then freeze is WaitGetPoses blocking, then never returning.

Fix: dedicated pose-waiter thread + `SetExplicitTimingMode(Explicit_ApplicationPerformsPostPresentHandoff)` so WaitGetPoses does not touch the Vulkan queue. Present only consumes the last pose and Submits; if the waiter stalls >500ms, skip Submit **and** `PostPresentHandoff` so the desktop game keeps running (headset may go waiting-room). `rel_look` skip-file entries from this hang are ignored (look was not the cause).

Do not pause WaitGetPoses during map load — a previous attempt to skip it when the window looked hidden froze after the load screen. Keep the waiter running across LevelShutdown; just keep it off the Present thread.

## Tab-out (2026-08-16, verified in log)

Latest crash: `bm_c0a0b` at ~90 FPS, then 62→34→**15 FPS** while OpenVR Submit still returned `eL=0`. No `Released VR render targets`, no DXVK `Device reset`. DXVK's `WM_ACTIVATEAPP` handler **minimizes** the game (`ShowWindow(SW_MINIMIZE)`) unless `D3DCREATE_NOWINDOWCHANGES`, then `WaitGetPoses` keeps running against a minimized swapchain until a later tab-out dies.

Fix: skip that exclusive-fullscreen minimize/`SetWindowPos` while a VR session is active. Do **not** skip `IDirect3DDevice9::Reset` for tiny/iconic sizes and do **not** drop `WaitGetPoses` when the window looks hidden — those two guards froze the game after the load screen (log stuck on `background01` at 90 FPS, process alive, never reached map `LevelInit`). Map load Reset must run. Keep calling WaitGetPoses so the compositor does not hold the GPU queue.

Stock `bin\thirdparty\dxvk-windows-x86\dxvk.conf` still has `d3d9.deviceLossOnFocusLoss=True`. `NotifyWindowActivated` returns immediately while VR is enabled.

## `-oldgameui` (verified 2026-08-16)

Black Mesa's **new** game UI is upside-down in the HMD and the world stays black after the load screen. **`-oldgameui`** shows an upright menu and the actual game in the headset. Do not Y-flip the 2D capture path to chase the new UI — that would invert the working old UI. New UI is a different RT / Y origin; leave it.

## Relative look (`rel_look`)

Head tracking cannot move the desktop unless the **rendered** camera changes. Absolute HMD on the live `CViewSetup` (`abs_view`) is still skipped. This retry adds **relative** yaw/pitch (`HMD - latched`) onto a **copy** passed to the single `RenderView` on real maps only. Origin is unchanged (tram camera stays). Desktop and headset both show the offset because they share that one view. CreateMove stays unhooked. Crash-sticky `bmvr_in_rel_look.flag`. User confirmed head tracking works (2026-08-16) with `-oldgameui`.

After launch, `bmvr_log.txt` next to `bms.exe` and next to the loaded `d3d9.dll`.

## Live debugging

Use **x32dbg** (Black Mesa is 32-bit). Attach to `bms.exe` and check the path of loaded `d3d9.dll`. If it is `C:\Windows\System32\d3d9.dll`, our code is not running.
