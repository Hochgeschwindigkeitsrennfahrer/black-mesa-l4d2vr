# BMSVR Checkpoint (stable mono + stereoCopy)

Saved git commit baseline for “working HMD image without killing BM”.

## Working path

1. Gate VR on **LevelInit map name** (reject `background*`) + `SeenGameplay` + ~90 CreateMove frames.
2. OpenXR probe → deferred session → empty-frame pacing (~60 frames); keep Wait/Begin/End every frame through SYNCHRONIZED→VISIBLE→FOCUSED.
3. Private eye RTs via `IDirect3DDevice9::CreateRenderTarget` (NOT MaterialSystem named RTs).
4. **Pre-Present** GPU copy of RT0/backbuffer into a **private frame-copy RT** (post-Present RT0 is often cleared → black 3D + only pause-menu VGUI leftovers).
5. After Present: aspect-fit `StretchRect` into eye RTs → `TransferSurface` → `SubmitEyes` with **Vulkan blit V-flip** (unflipped = upside-down / “kopfüber” in SteamVR).
6. Optional **stereoCopy**: opposing horizontal crops from one mono frame (no 2× RenderView).
7. `CaptureFrameBeforePresent()` before Present; `Update()` / OpenXR submit **after** Present.
8. Relative yaw (optional soft pitch) in CreateMove after pose settle; then ProcessInput.

## Hard constraints (do not regress)

| Approach | Result |
| --- | --- |
| MaterialSystem named RTs / Begin–End allocation | Crashes BM |
| Double `RenderView` in one frame | Exits BM |
| `WaitDeviceIdle` every frame | Exits BM |
| Absolute HMD pitch/yaw into `CViewSetup` / `cmd->viewangles` | Crashes BM |
| Relying on `GetBackBuffer` | Often 1×1 stub |
| CreateMove frame gate alone (no LevelInit) | Falsely arms VR on menu `background*` |

## Config knobs (`bmsvr.cfg`)

- `stereo_copy`, `stereo_offset`, `stereo_converge`
- `soft_pitch` (0 default — relative pitch validated safe in short run; enable for HMD feel)
- `move_deadzone`, `turn_deadzone`
- `eye_use_hmd_res` (0 default — window-sized private RTs; 1 = HMD recommended size, CreateRenderTarget 1440×1407 validated; **mono path still upscales from window frame** — sharpness limited by window buffer)
- `viewmodel_vr` / `viewmodel_off_f|r|u` (0 default — when on, CalcViewModelView gets a tiny eyePos input nudge in eyeAngles basis before original; angles untouched)
- `snap_turn`, `snap_angle`, `turn_speed`, `roomscale`, …

## Build / install

```powershell
.\scripts\rebuild_and_install.ps1
```

No UAC on the normal loop. If Access Denied once: `.\scripts\grant_bin_write.ps1`.  
Close Black Mesa before overwrite if DLL is locked.

Launch:
- `run-bms-vr.bat` — **system** OpenXR (`XR_RUNTIME_JSON` unset). If Windows default is WMR → **will fail** `XR_KHR_vulkan_enable`.
- `run-bms-vr-steamvr.bat` — **required for HMD** today → forces 32-bit `steamxr_win32.json` (BM is Win32; do not use `steamxr_win64.json`).

### OpenXR runtime / Vulkan (WMR vs SteamVR)

- BM is **Win32** + **DXVK Vulkan**. OpenXR session uses `XR_KHR_vulkan_enable` (`GraphicsBindingVulkanKHR`) — **not** D3D11, **not** `vulkan_enable2`.
- The active runtime must advertise `XR_KHR_vulkan_enable`. Probe enumerates extensions and logs **REQUIRED extension missing** before `xrCreateInstance`; success logs `OpenXR CreateInstance OK (XR_KHR_vulkan_enable)`.
- **WMR / Mixed Reality** (`MixedRealityRuntime.json`) **32-bit does not** expose Vulkan — `D3D11_enable` only → CreateInstance skipped. Confirmed 2026-07-27 via `run-bms-vr.bat`.
- **SteamVR 32-bit** has `XR_KHR_vulkan_enable` — CreateInstance + session → VISIBLE/FOCUSED works. Confirmed 2026-07-27 via `run-bms-vr-steamvr.bat` on `bm_c1a1b` (1440×1409). Then BM **exited after private eye RTs ready**, before capture lock / `SubmitEyes` — next in-headset debug target (no dump; same as prior DXVK shared-queue races).
- **For HMD:** always `run-bms-vr-steamvr.bat` until a WMR Vulkan path exists.

Flat/no-HMD smoke: DXVK only auto-binds SteamVR if `BMSVR_FORCE_STEAMVR` is set. Prefer `scripts\openxr_disabled_runtime.json` for flat tests.

## LevelInit / LevelShutdown gate (wired + VR-validated 2026-07-25)

- MinHook Shared `LevelInit` `0x110A80` / `LevelShutdown` `0x110B30`.
- `background*` / empty → `vr_eligible=0`, never OpenXR; OpenXR shut down if returning to menu.
- Real maps (e.g. `bm_c0a0a`) → eligible; CreateMove + **90** frames may start OpenXR.
- Proven: menu `background04` stays flat; `bm_c0a0a` arms VR; background return tears down OpenXR; map re-entry re-inits cleanly.
- `soft_pitch 1` relative CreateMove look: logged `yaw+soft_pitch`, no crash in short in-map run (repo default still `0`).
- `eye_use_hmd_res 1`: private eye RTs at 1440×1407 succeed; capture logged `src=2048x1024 -> eyeRT=1440x1407` (height upsample). Keep repo default `0` until a true high-res render path exists.

## CalcViewModelView (pose nudge experiment 2026-07-25)

- **Gameplay** RVA `0x29D930` on `C_BlackMesaViewModel` (`retn 0x0C`: owner*, eyePos&, eyeAng&).
- Shared/base `0x7CF60` exists but BM vt slot [230] overrides it — hooking Shared alone never fires in-map.
- Retired false positive `0xF090` (unrelated `retn 0x14` helper).
- **Signature:** eyePos/eyeAng are **inputs** (Source const refs). Original reads them → `SetLocalOrigin`/`SetLocalAngles`. Post-return tweaks do nothing; do not mutate caller storage.
- **Cfg:** `viewmodel_vr 0` (repo default). When 1 + VR eligible: local-copy eyePos nudge by `viewmodel_off_f/r/u` in eyeAngles basis, then call original. Angles untouched. Disable via cfg if unstable.
- **Validated in-map (2026-07-25):** with installed `viewmodel_vr 1` / `off_f 2`, logged `VR-adj … (-2768)->(-2766)`; SubmitEyes + soft_pitch + LevelInit gate OK through `bm_c0a0a`→`bm_c0a0b`; no crash.

## Black HMD / upside-down fix (2026-07-25)

- **Symptom:** gameplay black in HMD; pause menu fragment visible but upside-down; not SteamVR dashboard.
- **Causes:** (1) post-Present capture of game RT0 after discard/clear; (2) missing V-flip on OpenXR Vulkan blit; (3) squash from `eye_use_hmd_res 1` (2048×1024→1440×1407) made UI look like a “corner”.
- **Fix:** pre-Present private copy + aspect-fit StretchRect + blit Y-flip; keep `eye_use_hmd_res 0` default.
- Session still advances 3→4→5 when HMD is worn — focus is necessary but was not the black-content root cause.

## Capture crop + unbind latch (2026-07-27)

- RT0 is `_rt_FullFrameFB` **2048×1024** with window crop **1280×720**; viewport often `(0,304)-(1280,1024)` (bottom-aligned POT).
- Wrong crops (`rt0-tl`) → half-menu / offset UI; unresolved `bb-win` StretchRect → pure black (`S_OK`).
- Pre-Present CPU sample always `lit=0` under `DXVK_ASYNC` even when StretchRect succeeds — do not trust LockRect for lock decisions.
- **New:** `CaptureGameColorOnUnbind` from DXVK `SetRenderTarget(0)` after `FlushImplicit`, using the *pre-change* viewport. Only **oversized** FullFrameFB-class RTs (e.g. 2048×1024) — window-sized ping-pong unbinds crashed BM. Present path reuses that latch (`PrePresent using unbind-latched copy`).
- `rt0-vp` requires a **window-sized** viewport; full-RT vp would collapse to `rt0-tl` via `min(win)` — skip and use `rt0-bl`.
- Auto never picks `bb` (stub = black). CPU `lit` ignored for lock (DXVK_ASYNC). Default lock: `rt0-vp` then `rt0-bl`.
- Cfg: `capture_src auto|rt0_vp|rt0_tl|rt0_bl|rt0_ctr|rt0|bb`.

## Next (needs in-headset testing)

- **SteamVR path:** CreateInstance/FOV/FOCUSED OK; debug silent exit after `VR D3D eye RTs ready` (before unbind capture / SubmitEyes) on `bm_c1a1b`
- Confirm upright full world (not black / not half UI) after that fix — look for `Unbind capture` + `PrePresent using unbind-latched` + `SubmitEyes` in `bmsvr_log.txt`
- Feel-test soft_pitch / deadzones / `viewmodel_vr` (parked — capture was higher priority)
- If viewmodel_vr offset feels right: hand-controller absolute pose (still no absolute HMD into engine view)
- True stereo without 2× RenderView (alternate-eye frames or better crop)
- Higher-res capture than window StretchRect (without MaterialSystem named RTs / double RenderView)
- HUD overlay
- OverrideView only if safe and carefully logged

## Offline RE note (2026-07-25)

ClientMode Shared/BM vtables mapped (see `docs/OFFSETS.md`). OverrideView real impl Shared `0x110BE0` — not hooked yet.
