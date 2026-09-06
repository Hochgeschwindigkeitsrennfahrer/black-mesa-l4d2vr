# Performance pass (2026-09-06)

Line-by-line review of `src/`, the DXVK fork's `d3d9_vr.*`, and the hand /
weapon-menu renderers for per-Present, per-RenderView, per-CreateMove and
per-draw-call overhead. Everything below is **implemented, compiled and
installed** (`scripts/install.ps1`, `d3d9.dll` 2898944 bytes); none of it is
runtime-verified yet (no HMD session since the changes). Verification notes
are at the end.

Ground rules used: no new abstraction layers, no behaviour change on the
rendered image, every cache keyed on something that already proves staleness
(pointer identity, `m_PresentTick`, header pointer), and every removed
synchronisation point traced through the DXVK fork before it was removed.

## 1. GPU / compositor path

### OpenXR helper publish no longer waits for device idle

`PrepareOpenXrEyeSurfacesForRead` called `IDirect3DVR9::WaitDeviceIdle()`
(`Flush` + `SynchronizeCsThread(SynchronizeAll)` + `vkDeviceWaitIdle`) once per
published pair, ~143/s. That is a full pipeline drain on the render thread
every Present. Replaced with a per-slot `D3DQUERYTYPE_EVENT` and a new
`IDirect3DVR9::FlushCommands()` (fork: `D3D9DeviceEx::Flush()` only, no sync),
polled without `D3DGETDATA_FLUSH` at the start and end of `SubmitVRTextures`.
Full design, fallbacks and invariants: `docs/openxr-eye-publish.md` §7.
Config: `OpenXrDeferredPublish` (default `false` after HMD runs; `true`
is the throughput A/B), `OpenXrSlotCoolingMs` (4).

**First HMD run (2026-09-06) regressed feel while raising fps:** tearing while
turning and judder while walking in low-fps areas. Log: `maxPending=2`,
`dropped=1-3/s` — unbounded CPU run-ahead made every published pose two GPU
frames stale, and the timer-based slot reuse let the game overwrite a slot the
helper was still blitting from (its blit sits behind the game's GPU queue when
GPU-bound). Fixed in the same day, `docs/openxr-eye-publish.md` §7a:

- `OpenXrMaxPending=1` (new): Present blocks on the *previous* copy's event
  before copying new eyes. One frame of CPU/GPU overlap stays; pose age is
  bounded to one GPU frame; nothing rendered is dropped.
- Bridge v14: helper reports `helperConsumedFrameId` after its blits' queue
  idle; the game never rewrites a slot whose frame the helper has not finished.
  Slots 3 -> 4.
- Helper graphics queue asks for `VK_KHR/EXT_global_priority` HIGH.

### OpenVR IPC off the per-Present path

- `TickCompositorFocus`: `CanRenderScene` + `GetTrackedDeviceActivityLevel`
  (two vrclient round trips) now 10 Hz instead of every Present.
- `PollSteamVrRecommendedSize`: `GetRecommendedRenderTargetSize` now 4 Hz once
  a valid size is known (eye resize already has a 300 ms settle window).

### D3D9 queries that took DXVK's device lock every frame

- `D3dRt0IsEyeSized`: was `GetRenderTarget(0)` + `GetDesc` + `Release` per
  call. `HookedSetRenderTarget` now records the actual RT0 pointer/size
  (`g_Rt0Actual*`) and the check reads that.
- `SurfaceMatchesWindowOrBackbuffer`: `GetBackBuffer` result cached
  (`g_CachedBackBuffer`), invalidated on `Reset`/release.
- `HookedSetRenderTarget` colour capture on unbind: the
  `GetRenderTarget`/`GetViewport` pair is skipped unless
  `bmvr::OffscreenWorldMatchesEyes()` says the capture can do anything.
- `LogFullFrameSizeIfReady`: four `IMaterialSystem::FindTexture` per top-level
  `RenderView` -> 20 Hz.

### State blocks

- `DrawIndependentHandMarkers`: `CreateStateBlock(D3DSBT_ALL)` + `Release` per
  call -> one persistent `m_HandEngineStateBlock`.
- `vr_hand_renderer_d3d9.cpp`: `D3DSBT_ALL` per hand per eye (a full DXVK
  state walk each way) -> one recorded state block (`g_HandStateBlock`)
  covering only the render/sampler/texture-stage state the hand pass touches,
  via `BeginHandStateScope`/`EndHandStateScope`. Released in `ReleaseShared`.

### Shader constant hook

`HookedSetShaderConstantF` runs for every constant upload in the frame. It:
- no longer heap-allocates a copy of the constant range;
- early-outs vertex-shader bone palette uploads (never rewritten);
- is copy-on-write (`ConstantCow`): the range is only copied into the stack
  buffer if a screenspace-c0 / window-size / perspective-aspect rewrite
  actually matches. `RewriteScreenspaceVertexC0` was split into a read-only
  `ScreenspaceVertexC0Matches` and `ApplyScreenspaceVertexC0`.

### Draw batching

`DrawHandHud`: flat-colour glyph quads are gathered into one
`DrawPrimitiveUP(D3DPT_TRIANGLELIST)` instead of one call per glyph.

## 2. CPU hot paths in hooks

### Viewmodel classification cache

`GetActiveWeaponModelName()` (mutex + `std::string` copy) followed by a fan of
`strstr` calls appeared in `dCreateMove`, `dCalcViewModelView`,
`UpdateCrowbarMelee`, `IsCrowbarEquipped`, sprite hooks and the hand code.
`NoteViewmodelModel` now classifies the model name once when it changes
(`ClassifyViewmodel` -> `ViewmodelClass`) and mirrors the booleans into the
atomic `m_VmClassFlags` (`kVmFlagCrowbar`, `kVmFlagRpg`, `kVmFlagGluon`,
`kVmFlagMp5`, ...). Hot paths read `ViewmodelIsCrowbar()` etc. lock-free.
`HeldWeaponCache` in `hooks.cpp` (thread_local: main thread and render
thread both reach it) additionally caches the RPG/Gluon "held" answer per
`m_PresentTick`. `hooks.cpp` `IsCrowbarViewmodel` / `IsMp5Viewmodel` are gone;
`dCalcViewModelView` and `SuppressViewmodelMovementAnims` (twice per
CalcViewModelView, previously a model-info lookup + four `strstr` each) read
the flags.

### Per-call string work removed

- `dPushRenderTargetAndViewport`: RT name -> `{aux, world, hud}` memoised by
  texture pointer (`ClassifyRtName` / `RtNameClass`); `RtStackTopIsWorldScene`
  reads the flag stored in the stack entry. Flashlight diag log check
  short-circuits on the log gate first.
- `dDrawScreenSpaceRectangle`: `QueryContextColorRtSize` only when the DSSR
  diag log is on; the `s_seen` `strcmp` loop stops once the table is full.
- `dSpriteQuad`: skips both pose-mutex look-angle computations unless the RPG
  laser or gluon FX is live.
- `dSpriteRendererDraw`: `SafeModelName` / `GetEntityClientClassName` only
  when the RPG or gluon path can use them.
- `SuppressViewmodelMovementAnims`: sequence label -> `{action, loco, equip}`
  cached by label pointer (re-validated by content).
- `FindIdleSequence`: cached by studio header pointer; reset in
  `ApplyViewmodelStudioWork` when the viewmodel changes.
- `FindRpgLaserDot`: was a full entity-list walk per eye per frame. Now
  re-validates the cached entity index and only rescans when that fails, at
  most every 250 ms.
- `dTraceRay`: pure passthrough hook removed (trampoline cost on every trace).
- `EnsureServerFlashlightHook` / `EnsureWeaponShootOriginHooks` /
  `EnsureWeaponVfxHooks`: `s_done` early-out once every target is resolved
  instead of `GetModuleHandleA` + pattern checks per call.
- `CallSetAbsOriginAngles` (runs from `dCalcViewModelView` every frame) and
  `PatchGluonImpactParticleDefs` (every gluon beam update) called
  `GetModuleHandleA("client.dll")` per call — loader lock + module-list walk.
  Both use a cached `ClientModule()` now; client.dll lives for the whole
  process.
- `UpdateRpgLaserPoint` (per Present) resolved the active weapon entity, its
  model name and ran the lowercase+`strstr` RPG test every call; it now gates
  on `ViewmodelIsRpg()` (refreshed just before it in `ProcessInput`).

### Hands (`vr_hands.cpp`)

- `HandRig`: joint indices, bind-local matrices and topological order computed
  once per GLB asset instead of per hand per eye (string lookups + O(N^2)
  parent resolution each call before).
- `BuildSummaryCurlPalette`: skipped when the five curl values and
  `gripCurlMin` are unchanged since the last build for that hand.
- `SamplePalmSceneLight`: cached per hand per `m_PresentTick` + palm position
  so the second eye reuses the first eye's sample.

### Input

`UpdateViewmodelNumpadAdjust`: ~30 `GetAsyncKeyState` per Present -> polled at
8 ms and only while the game window has keyboard focus.

## 3. Windows API and I/O

- `GameWindow()` / `QueryWindowClientSize()` (`bmvr_flags.cpp`): `FindWindowA`
  and `GetClientRect` results cached centrally; callers that used to do their
  own lookups go through these.
- `BeginRisky` / `EndRisky`: the crash-sticky flag files were created/deleted
  on every call. Armed state is now an in-memory set under a mutex and the
  file is only touched when the armed state actually changes. The on-disk
  semantics (flag present while the risky section is armed) are unchanged.
- `Log()`: three `OutputDebugStringA` calls per line -> one. Each call raises
  `DBG_PRINTEXCEPTION_C` through the kernel even with no debugger attached.

## 4. Deliberately not changed

- `RenderView` double call, named RT usage, `CViewSetup` handling: L4D2VR
  mechanism, not a cost problem.
- The GPU wait after the left-eye blit in the SteamVR (OpenVR compositor)
  stereo-blit path: the same-image-both-eyes hazard in `docs/RUNTIME.md`
  (2026-08-18, user-verified fix) still applies there; only the OpenXR helper
  publish was deferred.
- DXVK fork internals beyond the added `FlushCommands()`: the flush heuristics
  (`ConsiderFlush`) and `GetData` semantics were traced, not modified.
- `OpenXRHelper64` `RenderEye`: one `vkQueueSubmit` + `vkQueueWaitIdle` per
  eye per compositor frame. Restored after the skip-blit / fence attempts
  (`docs/openxr-eye-publish.md` §7b–§7d): without the wait, WMR did not get
  a finished swapchain on a regular cadence. Default game publish is
  `WaitDeviceIdle` again so those helper waits are not sitting behind a
  full game GPU queue.
- `UpdateAimCrosshair`: one `TraceRay` per Present plus up to three
  `LookupAttachment` name scans in `TryGetVrMuzzleWorld`. The trace is what
  makes the reticle agree with the shot; the attachment scans are a handful
  of `strcmp`s on a viewmodel. Not worth a cache keyed on the studio header
  yet.

## 5. Verify after the next HMD run

```
Select-String -Path "...\bin\bmvr_log.txt" -Pattern 'OpenXR publish rate'
```
Expect `deferred=0` (default false). `published=` near `present=`,
`consumedLag` low. Helper compositor submits should stay near display
rate in-world. Compare headset feel to the pre-pass build, not desktop fps.

Hands: check gloves still animate (curl palette cache) and that lighting does
not differ between eyes (light cache is keyed on the same Present).

Weapons: crowbar swing detection, RPG laser dot follow, gluon beam and MP5
viewmodel offset all read from the new classification flags; a wrong flag
shows up as the L4D2VR default offset/scale being applied to that weapon.

Compare frame time with the previous build using the existing
`bmvr_log.txt` Present-rate lines; the expected win is on the render thread
(no per-Present `vkDeviceWaitIdle`, far fewer DXVK device-lock round trips),
so look at Present interval variance as much as the mean.
