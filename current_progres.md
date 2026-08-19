# Current progress (2026-08-19 late evening)

Read this file at the start of every session. Do not rediscover items below.

## Working (user-verified; do not rewrite)

- Fused 3D stereo, uncoupled 6DoF, save-load (nested `LockSubmissionQueue` deadlock fixed).
- Left-menu pause activation (slot 108 engine thread).
- Desktop HUD visible (do not steal `_rt_gui` destination).
- Crosshair off via `bmvr.cfg`.
- **FP arms hidden in HMD** — bodypart `arms` `nummeshes=0` sticky patch. User confirmed gone.
- Independent left/right controller tracking in-game: **cyan left**, **magenta right**, each follows its own controller.
- **Weapon proportions in HMD** — view-Y unstretch; upright vs flat controller matches desktop Glock shape (user-verified 2026-08-19).
- Do **not** re-enable `steamvr_rt`, `hmd_swap`, `hmd_native`, named-RT wrap, `abs_view`, EyePosition, Y-flip, CreateMove ClientCmd, flashlight cvars.
- Do **not** hook `engine.dll` `0xF6A20`. That is **not** `DrawModelExecute`.
- Do **not** `FindMaterial` from DrawModelExecute.
- Do **not** try another `MATERIAL_VAR_NO_DRAW` variation to hide FP arms.

## Plan order (research/implementation-plan.md)

1. DME ABI — **done** (`engine.dll` `0x113E80`, slot 19, thiscall `ret 0xC`).
2. Hide FP `arms` bodypart — **HMD confirmed gone**.
3. Scale/pivot §7 — **HMD proportions user-verified 2026-08-19** (view-Y unstretch). Walk jump/ghost still open.
4. Independent hand mesh §4 — **v1 proxy compiled** (palm + finger stubs at controller pose; not Source entities). GLB later.
5. Native IK only if hybrid fails. Two-hand aim later. Crowbar melee polish later.
6. Never intercept `SendWeaponAnim`.

## §7 Weapon transform (this pass)

**Root cause of invisible/off-screen weapon (2026-08-19 HMD report):** last build used raw OpenVR tracking coords as `P0` (`~−17,−17,48`) instead of Source world space (`~130,43,72`). Log showed `Viewmodel pose p0=(−16.7,−17.0,47.6)` while `eye=(129.8,42.9,72.0)` — weapon drawn near tracking origin, off-screen.

**Fix:** `P0 = engine eye/setup + PivotYaw(controllerTracking − hmdTracking)`. Bake offsets unchanged. Same conversion as hand-proxy `toWorld()`.

**Prior displacement bug (before tracking-space regression):** inverted bake sign (`ox=restX` vs `ox=−restX`) pushed gun forward/left when P0 was correct.

**Corrected architecture (L4D2VR §2.3 + live bake):**

| Step | Implementation |
|---|---|
| P0 | `eye/setup + (aimCtrl − hmd)` in world space — **not** raw `m_RightControllerPosAbs` |
| P0 | `ControllerTrackingToWorld()` = `GetViewOrigin(setup) + (ctrl−hmd)` — same frame as stereo RenderView |
| Position offsets | **L4D2 empirical tables** (glock `20.5, 5, −2`; crowbar `19.5, 6, −13.5`). DME bake logged for diagnosis only |
| Position | `p0 − ox·F − oy·R − oz·U` |
| Angles | L4D2 per-weapon table (crowbar `−24.5, −6.5, −6`) |
**Warp vs scale (2026-08-19 screenshots):** desktop (16:9 HUD pass) gun looks correct; HMD gun stretches along **view-up** as the controller turns (upright → long barrel, flat → long grip) and looks oversized vs the same crate. That is view-space aspect, not `m_flModelScale`. Pink squares on the slide are `VrHandsDebugBoxes`, not missing textures.

**Fix:** L4D2VR CalcViewModelView (controller pose as eye + hard-lock). DME unstretch along HMD up by `eyeAspect/windowAspect` on the **eye pass only** (do not squash the desktop HUD pass). `ViewmodelScale` still 1.0 until warp is gone; then retune.

| Scale | DME around controller when `ViewmodelScale≠1`. Default **1.0**. |

**HMD pass 2 fix (2026-08-19):** weapon was in wrong world frame (setup.origin vs GetViewOrigin); bake oy=18.54 on glock pulled gun far from controller; hand markers used wrong projection FOV.

**HMD verify:** markers on controller; glock position good enough; **warp/size in HMD still the open item.**

**Finger flex:** OpenVR skeletal summary on hand proxy — **user confirmed working**.

**Known:** bullet traces still spawn from engine eye + `cmd->viewangles` (no `Weapon_ShootPosition` hook yet) — may appear offset from visible gun until that hook exists.

## §4 Independent hand mesh (v1 — HMD good enough on weapon)

Parallel proxy at each controller (palm + skeletal finger stubs via `GetSkeletalSummaryData`), not Source entities. Left mirrored. Uses `ControllerTrackingToWorld()` same as gun. **Next:** port L4D2VR `VrHandSystem` + SteamVR `vr_glove_*.glb` (plan §4 step 3.2).

## Compositor FPS stalls (investigation)

See `docs/compositor-performance.md`. Rubber-band feel: stale background poses + runtime handoff experiment. **Fix:** L4D2VR app handoff default; synchronous `WaitGetPoses` on render thread when pose age > 16 ms.

## Unresolved (document only — do not derail hands/weapons)

| Issue | Status |
|---|---|
| Flashlight beam in HMD | **Still broken** (impulse hook remains) |
| HUD in HMD | Still wrong |
| Pause/menu in HMD | Still wrong |
| SteamVR resolution → VR quality | No visible improvement |
| Full-window 2560×1440 | Stable |

## User checklist

| Item | Status |
|---|---|
| Stereo / 6DoF / save-load / pause activation | Working. Do not rewrite. |
| FP arms hidden | **HMD confirmed** |
| Weapon grip on aim controller | Position **good enough**; scale default **0.55** — HMD verify proportions |
| Finger flex on hand proxy | **Confirmed** |
| Compositor stalls | Runtime handoff default; HMD verify smoothness |

Log: `C:\Program Files (x86)\Steam\steamapps\common\Black Mesa\bmvr_log.txt`

Look for: `HideViewmodelArms … arms='arms' … zeroed=7`, `Viewmodel bake bone=crowbar_new`, `Viewmodel pose p0=`, `Hand proxy eye=`, `First IN_ATTACK`, `First-shot present spike`.

### Do not retry

`steamvr_rt`, `hmd_swap`, `hmd_native`, named stereo wrap, stealing `_rt_gui`, `PushRT(NULL)` onto an eye, flashlight cvars, CreateMove ClientCmd, **`engine.dll` `0xF6A20` as DrawModelExecute**, **`FindMaterial` from DME**, **`MATERIAL_VAR_NO_DRAW` for FP arms**, debug-marker-only polish, `v_hands.mdl`, whole-VM `ForcedMaterialOverride`, `NativeViewmodelHandsOnly`, zeroing `mstudiobodyparts_t::nummodels`.

Build: `build\Release\d3d9.dll` installed to all three paths via `scripts/install.ps1`.
