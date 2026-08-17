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

## Stereo copy retry (`stereo_copy`, rejected 2026-08-17)

Two 16:9 `RenderView`s + `StretchRect` into private 1080 RTs. User: performs poorly and is **not fused**. Durable skip. True stereo needs named HMD-aspect eye RTs like L4D2VR / Portal 2 VR.

## Named-RT double RenderView retry (`stereo_rv`, not yet verified)

Portal 2 VR (`Gistix/portal2vr`, L4D2VR fork) does **not** call `PushRenderTargetAndViewport` itself for the world. It `CreateNamedRenderTargetTextureEx("leftEye0"/"rightEye0")`, `SetRenderTarget` on `IMatRenderContext`, then two `RenderView`s. Black Mesa HDR still `PushRT`s `_rt_FullFrameFB` inside `RenderView`.

**Verified fail 2026-08-17:** named RTs created `1920×1878`, stereo began, then the first frame redirected `_rt_gbshadowmaprt` / `static` / `flashlight` **8192×8192** into the eye (viewport forced to 1920×1878, original 8K depth kept). Process died (`in_stereo_rv`). User then played the capture path (`namedRT=0 direct=0`) — same 16:9 blit both eyes, “a bit offset, does not fuse.” That is not L4D2VR stereo.

**Misread, corrected 2026-08-17:** `materialsystem+0x68480` (`ret 24`) is an adjacent **function in the binary**, not the vtable slot before GetRT. `CBlackMesaViewRender_RenderView` (`client.dll+0x20EE40`) calls IMaterialSystem **`+0x19C`** GetRenderContext, context **`+0x1C`** GetRT, then later **`+0x18`** SetRT with that saved pointer (`1020f5e4`). 6-arg PushRT is context **`+0x23C`**; HUD Pop is **`+0x24C`**. Do not call `+0x68480` as SetRT. Do call **`+0x18`** as `SetRenderTarget(ITexture*)`.

**Verified fail 2026-08-17 (morning):** same death with the FullFrameFB-only fix. Log:

```
LevelInit map=bm_c0a0b eligible=1
Eye RT size 1580x1440 … Named eye RTs ready … created=1
Stereo namedRT RenderView begin setup=2560x1440 eye=1580x1440 fov=98.5 aspect=1.097
Stereo PushRT _rt_gbshadowmaprt 8192x8192 redirect=0
Stereo PushRT _rt_gbshadowmapstaticrt 8192x8192 redirect=0
Stereo PushRT _rt_gbshadowmapflashlightrt 8192x8192 redirect=0
```

Heartbeat stuck `in_stereo_rv`. No 4th PushRT (log flushes). Crash is inside the **first** stereo `RenderView` during/after flashlight CSM `PushRT`, **before** FullFrameFB. Common with the 8K-redirect death: `EnsureNamedEyeTextures` from that same `RenderView` (`BeginRTAlloc` + **Release** of in-use capture eye surfaces / Vulkan shared images) then an immediate modified-size `RenderView`. `stereo_copy` (two same-size `RenderView`s, no named RTs) previously survived.

Durable skip of that exact attempt: `stereo_rv` skip-file is **ignored** on this retry. Crash-sticky for the new attempt is `bmvr_in_named_push.flag`.

**stereo_fov (user rejected 2026-08-17):** same-size double `RenderView` + `StretchRect` into 16:9 capture eyes. Different eye poses confirmed; image **not fused**, **squeezed horizontally**, poor FPS. Cause: HMD FOV/aspect drawn into 16:9 pixels. Kept only as fallback if `named_push` crash-stickies.

## Named-RT PushRT stereo retry (`named_push`, verified fail 2026-08-17)

Present-time create **worked**: `leftEye0`/`rightEye0` 1580×1440, `created=1`, then `namedRT=1` on present 604. First gameplay `RenderView` (`setup=2560x1440` tram `bm_c0a0b`, eye 1580×1440, `ctx=12129D18`) died with heartbeat `in_named_push`.

Last flushed lines (not CSM this time):

```
Stereo namedRT PushRT begin setup=2560x1440 eye=1580x1440 fov=98.5 aspect=1.097 ctx=12129D18 ipd=2.36
Stereo PushRT null 0x0 redirect=0
Stereo PushRT null 0x0 redirect=0
Stereo PushRT null 0x0 redirect=0
```

Our L4D2VR `EyeRenderTargetScope` wrap called **original** `PushRT(eye, 1580×1440)` (bypasses the hook, so it does not log), then BM `RenderView` immediately `PushRT(NULL, 0, 0, 0, 0)` three times through the hook. Source `PushRT(NULL)` binds the **backbuffer**. That is a 2560×1440 LDR swapchain + **0×0 viewport** on top of a 1580×1440 HDR eye already on the RT stack. Crash is inside the 3rd original `PushRT(null 0x0)` (log flushes; no 4th line, no FullFrameFB). Viewport/GetViewport clamp never ran on those 0×0 sizes (clamp only shrinks when larger than the eye).

Ghidra MCP was still attached to **`bms.exe`** while both DLL CodeBrowsers were open. PushRT RVA remains `materialsystem+0x6A3D0`.

**named_bind (not play-tested):** rewrite-only, no outer wrap. Incomplete vs L4D2: RenderView's prologue GetRT / SetRT(`+0x18`) restore would put the **backbuffer** back if the eye was never pushed first.

**This retry (`named_l4d`, verified fail 2026-08-17):** wrap + null rewrite + SEPARATE depth. First gameplay `RenderView` logged three `PushRT null 0x0 redirect=1 -> lefteye0 1580x1440` then died (`in_named_l4d`). Next launch auto-persisted `named_l4d` and ran `stereo_fov` (16:9 IPD blit). User: fusion only at close range, world massive and squeezed — that blit, not named-RT stereo.

**named_eye (verified fail 2026-08-17):** SHARED depth + viewport clamp, still **1580×1440** `CViewSetup` / named RT. Same death: three `PushRT null 0x0 redirect=1 -> lefteye0 1580x1440` then crash. Headset image changed at end of load because capture 2560×1440 eyes were replaced with 1580×1440 named RTs, then stereo `RenderView` died.

**Framebuffer-sized named RT (verified fail 2026-08-17):** same wrap + rewrite with `leftEye0` at **2560×1440** (G-buffer size). Log: `Stereo namedRT eye begin setup=2560x1440 eye=2560x1440` then three `PushRT null 0x0 redirect=1 -> lefteye0 2560x1440`. Heartbeat `in_named_eye`. Size mismatch is **not** the crash. `CSimpleWorldView` draws after those three binds; redirecting BM's deferred `PushRT(NULL)` (= backbuffer) onto a separate HDR named RT is what dies.

**hmd_fb G-buffer lie only (verified fail 2026-08-17):** `GetBackBufferDimensions` / `CreateNamedRTEx` made `_rt_FullFrameFB` and `_rt_gb*` **1580×1440**. Swapchain and Present capture stayed **2560×1440**. First gameplay `RenderView` was already `setup=1580x1440`. Then `EnsureStereoEyeSurfaces` `CreateTexture` mid-frame and the process died (`in_hmd_fb`) before `callOriginal`. Deferred G-buffer 1580 + D3D depth/backbuffer 2560 is the mismatch.

**This retry (matching windowed backbuffer, verified fail 2026-08-17 evening):** CreateDevice/Reset rewrote the windowed swapchain to **1576×1440**. Menu survived 1000+ frames (`rt0-vp 1576x1440`, Submit `eL=0`). After `LevelInit bm_c1a0a` the process died before any `RenderView setup=` / `Stereo HMD-fb begin`. Last flushed lines: `HMD-fb stereo: queued cl_csm_enabled 0` then one present (`inGame=0 eligible=1`). WER: BEX `0xC0000005` / Exception Data `00000008` (execute), `engine.dll_unloaded` + `0x0021E110` — same second-chance bucket as earlier post-load deaths, not a useful first-chance site (Ghidra front was still `client.dll`).

Cause (static + this log, not another named-RT wrap):

1. `IDirect3DDevice9::Reset` uses a **local** copy so we do not write `Windowed` back. Load Reset(2560) is skipped against the existing 1576 swapchain. Engine **videomode / `GetScreenSize`** (IVEngineClient slot 5, `engine+0xA6BD0`) stays **2560**. `GetBackBufferDimensions` / `_rt_Hud` / `_rt_gb*` stay **1576**.
2. `client.dll` HUD downsample `FUN_10267420` (xrefs `_rt_Hud`) reads `CViewSetup` `width`/`height` at `param_1[4]`/`[6]` (offsets `0x10`/`0x18`) and PushRTs `_rt_Hud`. Menu skips that path (`iVar3==0`); first in-game HUD does not. 2560 view on a 1576 HUD RT.
3. `ClientCmd("cl_csm_enabled 0; …")` from Present during load (`inGame=0`) is a named-RT CSM leftover and fired at the same moment. `stereo_copy` reached 3D **without** that command. Removed; not required for G-buffer blit stereo.

**GetScreenSize + Reset write-back (2026-08-17, next launch):** hooks installed (`GetScreenSize`). Menu 1584×1440 Submit `eL=0`. After `LevelInit bm_c1a0a`: `GetScreenSize 2560x1440 -> 1584x1440`, `RenderView setup=1584x1440 fov=79`. Videomode split is **fixed**. Then `Stereo HMD-fb begin` `fov=79.0->98.7 zNear=6.0 ipd=2.36` and death inside `Stereo HMD-fb left RenderView 1584x1440` (heartbeat `in_hmd_fb`). First gameplay view was immediately replaced with a double RenderView + HMD FOV/IPD/zNear during spawn.

**WER `engine.dll+0x21E110`:** with `engine.dll` in Ghidra this is `FUN_1021e110` — `SetMiniDumpFunction` / `MinidumpSetUnhandledExceptionFunction` target. It calls `SteamAPI_WriteMiniDump` then `FUN_1006c0b0`. Installed from `FUN_1021d4f0`. Every BM abort lands here; it is not the first-chance fault.

**This retry:** one `RenderView` pass-through for 8 main views at the engine's 1584 setup (no HMD FOV/IPD/zNear), then stereo. If the log stops on `HMD-fb pass-through 1/8`, 1584 world render is the fault. If it reaches `Stereo HMD-fb left RenderView` again, spawn timing was the fault and double-RV/FOV is next. Fusion is **not** verified.

**Pass-through then stereo (2026-08-17):** 8/8 pass-through `setup=1584x1440 fov=79 zNear=7 inGame=1` succeeded. First stereo left RenderView died (`fov=79->98.7 zNear=6 ipd=2.36`). Cause: `NormalizeViewSetupForVREye` wrote eye height 1440 into `CViewSetup+0x1C`. On BM that dword is `m_eStereoEye` (RenderView `cmp [ebx+0x1c], 2` and `byte [eax+this+0x744]`). Pass-through left it 0 (mono). Stereo turned it into an OOB index. `m_nUnscaledHeight` is at `0x20`. Also stop forcing L4D2 `zNear=6` / viewmodel FOV.

**Headset fusion (2026-08-17, user-verified):** after the `m_eStereoEye` fix, stereo at 1584×1440 **fuses**. Image was upside-down. Cause: direct HMD-aspect Submit applied `ApplyVulkanYFlip` on projection UVs. The same `StretchRect`→`TransferSurface` capture path is upright with `{0,0,1,1}` and no v-flip (old UI). Do not Y-flip that blit for Vulkan. Per-frame `Stereo HMD-fb left/right RenderView` logs were flushing twice a frame (~12 FPS); throttled.

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
