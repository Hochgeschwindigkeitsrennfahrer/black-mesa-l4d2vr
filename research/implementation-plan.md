# Implementation plan — independent VR hands + BM weapon animation

**Status:** research handoff. Do not treat this as a license to change stereo, 6DoF, or the d3d9 load path.  
**Architecture:** [`research/vr-hand-architecture.md`](vr-hand-architecture.md) (read first).  
**Sources of truth:** [`l4d2vr-hands.md`](l4d2vr-hands.md), [`l4d2vr-weapons.md`](l4d2vr-weapons.md), [`l4d2vr-melee.md`](l4d2vr-melee.md), [`source-viewmodel.md`](source-viewmodel.md), [`blackmesa-weapons.md`](blackmesa-weapons.md).

Evidence tags: **Confirmed from source** / **Confirmed from asset inspection** / **Confirmed from Ghidra** / **Strong inference** / **Unknown** (verification step).

---

## Read this first

L4D2VR keeps the original Source viewmodel as the gun: `CalcViewModelView` is given the weapon-hand controller pose (scale 43.2, −45° aim pitch, per-weapon offsets), sequences still run, and `DrawModelExecute` optionally hides a **separate** `/arms/` model or IK’s ValveBiped bones. Independent GLB gloves are a parallel D3D9 mesh on controller matrices; the **shipping default is native-IK arms, gloves off**, not GLB. Black Mesa viewmodels are one `v_*.mdl` per weapon on `C_BlackMesaViewModel`: bodypart `studio`/`body` is the gun, bodypart `arms` is 6–7 skins, **separate bone roots**, no blank arms choice, `v_hands.mdl` is the wrong skeleton. BMVR already uncouples that **whole** MDL to the aim controller (`dCalcViewModelView` + `SetAbsOrigin`/`SetAbsAngles`) — that is reusable. What must change is the **arm representation**: hide the `arms` meshes and draw independent hands at each controller; do not copy L4D2VR’s native-IK default (BM’s problem is two monitor-arms holding a gun glued to one controller). Implementation order: do not touch stereo/tracking; first hide arms on the current controller-parented gun; then retune scale/offsets; then an off-hand mesh; native IK only if hybrid fails; two-hand aim later; then crowbar motion melee with double-swing mitigation; never drop `SendWeaponAnim`.

---

## 1. Prerequisites

### Concrete steps

1. Read [`vr-hand-architecture.md`](vr-hand-architecture.md) end-to-end. Do not invent `SetWeaponModel` hands MDLs, slot-1 viewmodels as hands, or encoded L4D2 `CUserCmd` melee.
2. Confirm the live BMVR snapshot still matches the research:
   - `src/hooks.cpp` `dCalcViewModelView` installed; `dDrawModelExecute` **compiled, `createHook` skipped** (load hang 2026-08-18).
   - `src/vr.cpp` right-only `UpdateControllerTracking`; `ResolveWeaponViewmodelPose`; `UpdateCrowbarMelee`.
   - `src/vr.h` `m_VRScale = 39.37f` (not 43.2).
   - `ControllerPitchTilt` default **−35°** (not L4D2VR −45°).
3. **Verify `DrawModelExecute` ABI** before any hide-arms work. Offset: `engine.dll` `0xF6A20` (`src/offsets.h`). Detour signature in `src/hooks.h`: 3-arg after `this` (`state`, `ModelRenderInfo_t`, `pCustomBoneToWorld`). **Unknown:** whether this build is `IStudioRender` 3-arg or a newer `IMatRenderContext` first-arg. **Done only when:** a MinHook install reaches `LevelInit` / gameplay without the 2026-08-18 load-to-menu hang. Use a crash-sticky (same pattern as `bmvr_in_*.flag`) so a bad ABI cannot brick the next launch.
4. Confirm Steam install still has `models/weapons/v_*.mdl` with bodypart name `arms` and materials `$viewmodelhands` / `v_hand*` ([`blackmesa-weapons.md`](blackmesa-weapons.md) §B).
5. Headset + `-oldgameui` + combined `d3d9.dll` in all three install locations. Close `bms.exe` before overwrite.

### Files / functions (read, not redesign)

`src/hooks.cpp` `initSourceHooks`, `src/offsets.h` `DrawModelExecute`, `docs/RUNTIME.md` (DME hang), `docs/L4D2VR-MAP.md` (hands not ported).

### Done looks like

Written ABI conclusion (3-arg works / does not). DME either safely hooked with a sticky or explicitly blocked with a new Ghidra/x32dbg note. No stereo/load-path edits.

### Evidence

**Confirmed from source** (unhooked DME, hang comment). **Unknown** ABI until this step.

---

## 2. Existing systems that must remain untouched

Do not modify as part of hands/weapons unless a **new** verified failure on this DLL requires it.

| System | Files (do not “improve”) | Why |
|---|---|---|
| Combined d3d9 / DXVK / `IDirect3DVR9` / OpenVR submit | `src/dllmain.cpp`, DXVK, `CreateDevice` | Architecture. **Confirmed from source** (`docs/ARCHITECTURE.md`). |
| Stereo `RenderView`, named/private eye RTs, `m_eStereoEye` | `src/hooks.cpp` `dRenderView`, `src/vr.cpp` | Same world gun both eyes. **Confirmed from source.** |
| HMD 6DoF camera, LevelInit `background*` gate | `src/vr.cpp`, `src/game.cpp` | AGENTS.md. |
| Uncoupled VM **mechanism** | `dCalcViewModelView`, `GetRecommendedViewmodelAbsPos/Angle`, `SetAbsOrigin` `0xAF720` / `SetAbsAngles` `0xAF600` | Reuse; only feed better offsets / hide arms around it. |
| `GetViewModelFOV` → HMD FOV in gameplay | `dGetViewModelFOV` | Do not restore 54/75 to “fix” gun size. **Confirmed from Ghidra** (BM default 75; shotgun 50). |
| Analog walk, controller `CreateMove` viewangles, HUD overlay path | `src/vr.cpp` ProcessInput, `dVGui_Paint` | Out of scope. |
| Launch options, `-oldgameui`, no Y-flip 2D UI | — | Verified. |
| `ExitProcess` on `VR_Init` failure | — | Forbidden. |

**Done looks like:** diff does not touch stereo RT, swapchain, or compositor submit except if independent-hand **draw** needs an existing eye surface bind (L4D2VR `m_D9LeftEyeSurface` / `m_D9RightEyeSurface` — **Unknown** on BM; verify, do not redesign Present).

---

## 3. First implementation step

**Hide FP arms while keeping the gun on the current controller-parented viewmodel.** Proves bodypart separation. No new hand renderer yet.

### Concrete steps

1. Install `Hooks::dDrawModelExecute` **only after §1 ABI pass**. Keep current `HideLocalPlayerModel` skip for the local **world** model (`info.entity_index == local` and **not** `ModelNameIsViewmodel`). Do **not** skip the whole viewmodel draw.
2. Detect viewmodels with existing `ModelNameIsViewmodel` (`/v_` in path) (`src/hooks.cpp`).
3. **Do not** copy L4D2VR `"/arms/"` path hide — BM arms are not a separate model. **Confirmed from asset inspection.**
4. For the duration of the original DME call on a viewmodel:
   - Find materials used by the `arms` bodypart: HEV `v_hand` / `v_hand_m`; also `marine_d`, `bare_hands_d`, `gman_hands_d`, `sec_guard_d`, `zombie_hands_diffuse`, `zombie_guard_hands_d` ([`blackmesa-weapons.md`](blackmesa-weapons.md) §B). `$viewmodelhands` marks HEV gore FX, **not** a hide switch (**Confirmed from Ghidra**).
   - Set `MATERIAL_VAR_NO_DRAW` (`src/sdk/material.h`, bit `1<<2`) on those `IMaterial`s, call `hkDrawModelExecute.fOriginal`, restore flags. L4D2VR still calls original so attachments/events are not stripped. **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §9).
5. Gate on gameplay eligibility (same as `dCalcViewModelView`: VR on, not `background*`, in-game).
6. **Do not** `SetBodygroup` yet: stock MDL has **no blank** `arms` index. Writing `m_nBody` (`+0x6C4`) cannot mean “no arms.” **Confirmed from asset inspection.**
7. Optional parallel (not required for “done”): asset `blank` in `$bodygroup arms` after Crowbar decompile — then `FindBodygroupByName("arms")`. Exact engine setter for `vm_bodygroup_overrides` is **Unknown**.

### Files / functions

`src/hooks.cpp` `dDrawModelExecute`, `initSourceHooks` (add `createHook` for DME), `src/offsets.h` `DrawModelExecute`, `src/sdk/material.h` `MATERIAL_VAR_NO_DRAW`, `src/sdk/sdk.h` `ForcedMaterialOverride` (L4D2VR uses NO_DRAW **and** override on the cached arms **model** — BM must be **per-material**, not whole-model override, or the gun vanishes).

### Done looks like

In HMD on a gameplay map: **gun visible**, **no HEV/marine arms**, gun still glued to aim controller, fire/reload sequences still play on the gun. Desktop/HMD log: DME running, no load hang. Status: **headset presentation + visual verification**, not merely compiled.

### Evidence

**Confirmed from asset inspection** (two bodyparts). **Confirmed from source** (L4D2VR hide pattern; BM DME unhooked). **Unknown:** whether `marine_d` / `bare_hands_d` are also used on world models — if world NPCs lose hands, narrow the NO_DRAW window to viewmodel DME only (already the plan) and verify material pointers are not shared globally in a sticky way.

---

## 4. Hand system implementation

**Independent off-hand and matching-hand meshes, not parented to the gun.** After §3.

### Concrete steps

1. **Track the off-hand controller** in `VR::UpdateControllerTracking` (`src/vr.cpp`). Today only `AimControllerRole()` (right unless `LeftHanded`). Add physical left/right poses; gameplay swap like L4D2VR `physicalHandIndexForGameplay`. Convert with existing `HmdMatrixToSourcePos(..., m_VRScale)` and yaw recenter. **Confirmed from source.**
2. Build world matrix \(W = B C S\) from [`vr-hand-architecture.md`](vr-hand-architecture.md) §6.4 / [`l4d2vr-hands.md`](l4d2vr-hands.md) §5.2. Per-hand meter offsets converted **before** model scale. Use **BM** `m_VRScale` (39.37), not 43.2.
3. **v1 renderer (pick in this order):**
   1. Debug boxes / simple controller mesh at \(W\) into the **same eye RT the game already submitted**. Proves independence without ozz.
   2. Optional: port L4D2VR `VrHandSystem` **mechanism** (`vr_hand_system.cpp`, `BuildControllerWorld`, SteamVR `vr_glove_*.glb`, skeletal **summary** curls). Do **not** port ValveBiped VM-pose, magazine interaction, or world-model pose relay. Ozz is init-time skeleton builder, not a clip player. **Confirmed from source.**
4. Draw **after** the Source viewmodel (L4D2VR `FinishVrHandsEyeRender`). Immediate path binds left/right D3D9 eye surfaces. **Unknown:** BM DXVK queued insertion (`mat_queue_mode != 0`) — start with `mat_queue_mode` 0 / existing BM queue policy; do not add L4D2VR call-queue hooks in v1.
5. Do **not** construct hands as `C_BaseViewModel` / `CreateEntity`. **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §2).
6. Do **not** use `models/weapons/v_hands.mdl`. **Confirmed from asset inspection.**
7. Finger curls: optional OpenVR `GetSkeletalSummaryData`; **do not** retarget `R_Index1` sequences onto the GLB. **Confirmed from source.**
8. Config flags (new, names may match L4D2VR but defaults must be **BM hybrid**): gloves/debug hands **on** for the visual proof; `NativeViewmodelHandsOnly` **off**. Opposite of L4D2VR sample defaults. Document why in the flag comment (architecture §4).

### Files / functions

`src/vr.cpp` `UpdateControllerTracking`, `src/vr.h` pose members, optional new files under `src/` ported from `third_party/l4d2vr/L4D2VR/vr_hands/` (**mechanism only**). Draw insertion: wherever BM already has per-eye present/submit (`src/vr.cpp` Update / DXVK Present) — **Unknown** exact bind of `m_D9LeftEyeSurface` on this fork; grep before adding a second Present path.

### Done looks like

Left mesh follows left controller while the **gun stays on the right** (or swapped if `LeftHanded`). Right mesh follows right controller **without** dragging the gun off its offset pose. Moving only the left controller does **not** move the gun. HMD visual verification.

### Evidence

**Confirmed from source** (GLB not an entity; default vs gloves). **Unknown:** BM eye-surface draw and queue.

---

## 5. Weapon attachment

The gun is **already** attached to the aim controller. This section is **retune + do not re-parent**, not a new attach system.

### Concrete steps

1. Keep `Hooks::dCalcViewModelView` → `VR::GetRecommendedViewmodelAbsPos/Angle` → original → `CallSetAbsOriginAngles`. **Confirmed from source.**
2. Keep \( \mathbf{p} = \mathbf{p}_0 - o_x\mathbf{f} - o_y\mathbf{r} - o_z\mathbf{u} \) in `GetRecommendedViewmodelAbsPos` + `ApplyViewmodelBasisOffsets`. **Confirmed from source** ([`l4d2vr-weapons.md`](l4d2vr-weapons.md) §2.3).
3. **Do not** parent the VM to a GLB wrist / `R_Wrist`. **Do not** `EF_BONEMERGE` `w_*` onto hands (`w_*` have no FP anims). **Do not** enable L4D2VR `VrHandsRightUseViewmodelPose` in v1 (that glues the **hand** to the **gun**).
4. Identity: `Game::GetActiveWeaponModelName` reads the **weapon entity** `m_nModelIndex` (`+0x94`) = `w_crowbar.mdl`, not `v_crowbar.mdl`. Substring `crowbar` still matches. DME sees `/v_` via `ModelNameIsViewmodel`. Do not “fix” this by swapping to viewmodel index unless offsets mis-key. **Confirmed from source** + **Confirmed from Ghidra.**
5. Skip AutoGrip in v1 (no grip attachment on crowbar). **Confirmed from asset inspection.**
6. Two-hand aim blend: **not this section** (later).

### Files / functions

`src/hooks.cpp` `dCalcViewModelView`, `src/vr.cpp` `GetRecommendedViewmodelAbsPos`, `GetRecommendedViewmodelAbsAngle`, `ResolveWeaponViewmodelPose`, `ApplyViewmodelBasisOffsets`, `src/game.cpp` `GetActiveWeaponModelName`.

### Done looks like

With arms hidden: grip sits on the aim controller in the HMD for crowbar + one gun (glock or mp5). Same pose both eyes (no double gun). Sequences still fire.

### Evidence

**Confirmed from source** (L4D2VR gun is never a GLB child). **Unknown:** final numeric offsets per BM mesh until HMD tune (§7).

---

## 6. Viewmodel / arms separation

### Concrete steps

1. Treat §3 as the runtime split. Confirm in HLMV or a log: bodypart 0 draws, bodypart 1 materials are NO_DRAW.
2. Do **not** `SetModel` to a custom VM. Do **not** `w_*` in first person.
3. If material skip leaks (shared `IMaterial*` globally hiding world gloves): switch to **asset blank bodygroup** (`$bodygroup arms { blank }`) and set group `"arms"` on the VM. `m_nBody` networked 8-bit — prefer setting on the client VM in SP; **Unknown** MP. Manifest already keys `"arms"` 0–6. **Confirmed from asset inspection.**
4. `AE_CL_DISABLE_BODYGROUP` exists in client (`FUN_101354a0`) but crowbar sequences fire **no** events. Do not wait on animevents. **Confirmed from Ghidra** + **Confirmed from asset inspection.**
5. Extra bodyparts (`dart_live`, `grenade`, `rocket`, …) are **held-object toggles**, not arms — do not NO_DRAW those. **Confirmed from asset inspection.**

### Files / functions

`dDrawModelExecute`; optional `C_BaseViewModel::m_nBody` `+0x6C4`; do not add a `SetWeaponModel` hook unless hybrid failed.

### Done looks like

Same as §3, plus at least one two-handed gun (shotgun/glock): **both** native arms gone, gun mesh remains, pump/slide still animates.

### Evidence

**Confirmed from asset inspection** (split). **Strong inference** (hiding meshes leaves gun bones).

---

## 7. Scale / pivot correction

Do this **after** arms are hidden (giant arms invalidate scale judgment).

### Concrete steps

1. Keep HMD `GetViewModelFOV`. Do not reintroduce 54° “to make the gun smaller.” **Confirmed from Ghidra.**
2. Retune `VR::ResolveWeaponViewmodelPose` tables in the HMD. Current values are L4D2VR mappings (**Confirmed from source** `src/vr.cpp` ~1608–1630). Crowbar/wrench: pos `(19.5, 6, -13.5)`, ang `(-24.5, -6.5, -6)`. Live `ViewmodelPosOffset*` / `ViewmodelAngOffset*` add on top (`src/bmvr_flags.cpp`).
3. `ControllerPitchTilt` (−35°) vs L4D2VR −45°: try −45° **only** as a config experiment if the barrel does not follow the controller; do not hardcode a third tilt.
4. Prefer offsets over `m_flModelScale` (`+0x7C0`, about entity origin). If scale is used, keep the existing clamp in `dCalcViewModelView`. **Confirmed from source.**
5. If the gun snaps to the face, check the **80 hu** delta clamp in `GetRecommendedViewmodelAbsPos` before changing scale.
6. `FormatViewModelAttachment`: **skip** unless tracers/muzzle disagree with the visible barrel. BMVR FOVs already match in gameplay so \(f \approx 1\). **Unknown** if BM still has the function — Ghidra search is a verification step, not a v1 hook. **Confirmed from source** (SDK formula in [`source-viewmodel.md`](source-viewmodel.md) §5.4).
7. Pivot diagnosis: baked rest of `crowbar_new` ≈ `(-13.3, -4.75, 14.1)` is camera-space, not a wrist socket. **Confirmed from asset inspection.**

### Files / functions

`src/vr.cpp` `ResolveWeaponViewmodelPose`, `ApplyViewmodelBasisOffsets`, `src/bmvr_flags.cpp` offset/tilt/scale, `src/hooks.cpp` `dGetViewModelFOV`.

### Done looks like

Crowbar grip in the palm (not floating a meter forward); glock not huge; muzzle roughly along controller forward after tilt. Document final numbers in a short note under `research/` only if asked; otherwise comments next to the table.

### Evidence

**Confirmed from asset inspection** (baked roots). **Unknown:** 1:1 L4D2 offset reuse.

---

## 8. Animation preservation

### Concrete steps

1. Do **not** intercept `SendWeaponAnim` / `SetSequence` / `m_nSequence` (`+0x960`) for fire/reload/pump/draw.
2. Do **not** freeze the VM except as a **melee** mitigation (§9).
3. Confirm after hide-arms: `ACT_VM_PRIMARYATTACK`, shotgun pump / `AE_CLIENT_EJECT_CUSTOM`, reload mag bones still move. **Confirmed from asset inspection** (events on guns; crowbar events = 0).
4. Independent hands **must not** play `ACT_VM_*`. They are a parallel mesh. **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §10).
5. Accept: reload will not put the **VR** left hand on the mag. Do not build magazine interaction in this plan (L4D2-specific).
6. Optional: L4D2VR bob-kill (zero owner velocity around original `CalcViewModelView`). Only if lag still drifts the gun after `SetAbsOrigin`. **Confirmed from source** ([`l4d2vr-weapons.md`](l4d2vr-weapons.md) §2.4).

### Files / functions

None required if §3–§5 hold. Do not add `SetWeaponModel` replacements.

### Done looks like

Fire, reload, pump, draw visible on the **gun** in the HMD with arms hidden and hands independent.

### Evidence

**Confirmed from source** (L4D2VR never replaces the sequence player). **Strong inference** (independent roots).

---

## 9. Crowbar melee

Do **after** the gun+hands look right. Do not block §3–§8 on melee polish.

### Concrete steps

1. Keep the L4D2VR **constants**: `|TrackedDeviceVel| > 1.1` m/s (raw OpenVR, **not** `m_VRScale`), 50° about controller right, 10 Rodrigues samples, abort if swing angle ≤ 0.01°. **Confirmed from source** ([`l4d2vr-melee.md`](l4d2vr-melee.md) §2, §8).
2. BM already has this **client-side** in `VR::UpdateCrowbarMelee` (`src/vr.cpp`): hull ±16, range **70** hu, on hit haptic + `IN_ATTACK` 120 ms; `dTraceRay` rewrites origin while `m_PerformingMelee` (sticky `melee_trace`). **Confirmed from source** ([`blackmesa-weapons.md`](blackmesa-weapons.md) §A).
3. **Do not** port L4D2 `WriteUsercmd` packing / `TestMeleeSwingCollision` / `WeaponID==19`. **Confirmed from source** ([`l4d2vr-melee.md`](l4d2vr-melee.md) §9).
4. **Verify** script range **56** hu vs fan **70** (`weapon_crowbar.dmx` `primary_attack.melee.range`). Align to 56 unless HMD testing shows 70 is required. **Confirmed from asset inspection.**
5. **Double-swing mitigation:** `ACT_VM_HITCENTER` rotates `crowbar_new` while the VM root already follows the swing. **Strong inference.** While `speed > 1.1` (or while `m_PerformingMelee`), do **not** rely on the canned hit anim — prefer idle/draw hold. **Unknown:** exact client function that starts crowbar `SendWeaponAnim` on `IN_ATTACK` (server.dll `PrimaryAttack` not walked) — tracing that is a verification step before blocking the activity.
6. Do **not** copy `UpdateMotionGestures` (left flick = shove, not damage). Do **not** copy AutoFastMelee `slot1`/`slot2` (`ClientCmd` from CreateMove has crashed BM; weapon cycle is `weaponselect`). **Confirmed from source.**
7. Hide native melee sleeves is already done if §3 works; L4D2VR extra-hides arms on melee unless native-IK. Hybrid already hides all FP arms.

### Files / functions

`src/vr.cpp` `UpdateCrowbarMelee`, `TryGetMeleeTraceOrigin`, ProcessInput `m_MeleeAttackUntilMs` → `IN_ATTACK`; `src/hooks.cpp` `dTraceRay`; `src/bmvr_flags.cpp` `TryMeleeTrace`.

### Done looks like

A fast physical swing deals crowbar damage along the **controller arc**, not the eye ray; haptics on hit; the visible crowbar does **not** play a second huge studio swing on top of the hand motion (or the remaining double-swing is documented as accepted). Gameplay verified in HMD.

### Evidence

**Confirmed from source** (algorithm + BM client fan). **Discrepancy:** [`l4d2vr-melee.md`](l4d2vr-melee.md) §10 vs current `UpdateCrowbarMelee` — see architecture §10. **Unknown:** server hull vs `TraceRay` rewrite completeness.

---

## 10. Testing strategy

Use the project ladder. Hands/weapons are **not** done at compiled/launched.

| Gate | What to prove | Hands/weapons note |
|---|---|---|
| Compiled | DLL links | Insufficient |
| Launched | `bms.exe` + our `d3d9.dll` | DME sticky must not prevent launch |
| Runtime-init | OpenVR + interfaces | Hands system lazy-init must not `ExitProcess` |
| Head tracking | HMD moves camera | Unrelated; do not regress |
| Rendering | World in RT | — |
| Stereo | Two eyes, no double gun | Same VM pose both eyes **Confirmed from source** |
| Headset presentation | Compositor submit | Independent hands must appear **in the HMD**, not only desktop |
| Gameplay | Walk, shoot, crowbar | Required for §5–§9 |

**Visual checklist (HMD):**

1. Menu/old UI still upright (`-oldgameui`).
2. In-map: no FP arms; gun on aim controller.
3. Left controller moves left hand only.
4. Fire + reload on glock/mp5; pump on shotgun.
5. Scale: grip in palm after §7.
6. Crowbar motion melee after §9.

Log tags already used: `CalcViewModelView controller origin=`, `Crowbar melee fan hit`, `Viewmodel model`. Add a single log when arm materials are NO_DRAW (rate-limited).

Do not claim success from desktop window only.

---

## 11. Likely failure points

| Failure | Why | What to do (still in-plan) |
|---|---|---|
| DME `createHook` hang | 2026-08-18 coincidence / ABI | Crash-sticky; Ghidra/x32dbg `engine.dll` `0xF6A20`; do not skip original. **Confirmed from source.** |
| Whole gun vanishes | `ForcedMaterialOverride` / NO_DRAW on the entire VM | Per-arm materials only. |
| World characters lose gloves | Shared `IMaterial*` | Restore flags after DME; if still shared, blank bodygroup. **Unknown.** |
| Arms still visible | Wrong material names for marine/sci | Match MDL materials in [`blackmesa-weapons.md`](blackmesa-weapons.md). |
| Gun follows left hand | Parenting hands to VM or swapping roles | Keep VM on `AimControllerRole` only. |
| Hands glued to gun | Accidental VM-pose / `BuildViewmodelWorld` | v1 is `BuildControllerWorld` only. |
| Huge gun | Judging scale with arms on, or restoring VM FOV | Hide first; keep HMD FOV; offsets not scale. |
| Gun at feet / face | Recenter subtracted twice, or 80 hu clamp | Do not change delta-from-HMD math. **Confirmed from source.** |
| Reloads look empty | Expected limitation | Do not start mag-phys v1. |
| Double crowbar swing | `ACT_VM_HITCENTER` + controller root | §9 mitigation. |
| Melee traces from eyes | `dTraceRay` sticky off / `m_PerformingMelee` false | Check `TryMeleeTrace` flag. |
| Queued GLB invisible / crash | BM queue ≠ L4D2VR call queue | Immediate draw or boxes first. **Unknown.** |
| Native IK “as default” | Copying L4D2VR sample config | Forbidden for v1 (architecture §4). |

---

## 12. Fallback approaches

Use only if the listed **done** criterion failed in the HMD.

| If this failed | Fallback | Do not jump here first |
|---|---|---|
| Material NO_DRAW insufficient or leaks | Asset `blank` in `$bodygroup arms` + set `"arms"` | Weapon-only recompile |
| Blank + materials both fail | Decompile, omit arm `$model`, keep sequences (QC not shipped) | `w_*` in first person (no FP anims) |
| Independent GLB cannot draw on DXVK eyes | Debug boxes / OpenVR render models at \(W\) | New Source entities |
| Boxes OK but HEV gloves wanted on the **native gun** | Native-arm IK on `R_Arm`/`L_Arm` with a **new** bone-name table; crop melee sleeves | Copy-paste ValveBiped `Bip01_*` code |
| Native IK also looks like two arms on one controller | Stay hybrid; do not IK `L_Arm` onto the gun | Slot-1 VM as a “left hand” |
| Grip never matches | More offset table work; last resort `m_flModelScale` | AutoGrip without a named bone |
| `FormatViewModelAttachment` proven wrong in HMD | Bypass/identity remap | Changing world FOV |
| Client melee fan never damages | Verify `IN_ATTACK` window + `dTraceRay`; then Ghidra `CWeapon_Crowbar::PrimaryAttack` | L4D2 encoded usercmd protocol |
| Double-swing mitigation unknown hook | Leave idle sequence while `m_PerformingMelee` if a sequence write offset is verified | AutoFastMelee slot dance |

**Out of scope even as fallback:** stereo redesign, `v_hands.mdl` merge, TF `ShouldAttachToHands`, magazine phys, world-model pose relay, `ExitProcess` on VR init fail.

---

## Implementation order (locked)

1. Prerequisites / DME ABI (§1–§2).
2. Hide FP arms, gun still controller-parented (§3, §6).
3. Scale/pivot/offset tables; skip `FormatViewModelAttachment` until proven (§7).
4. Independent off-hand (and right-hand) mesh — not parented to the gun (§4–§5).
5. Optional native-arm IK **only if hybrid fails** (§12).
6. Two-handed aim blend — later ([`l4d2vr-weapons.md`](l4d2vr-weapons.md) §2.1).
7. Crowbar motion melee constants + double-swing mitigation (§9).
8. Animation: keep `SendWeaponAnim` throughout (§8).

---

## Quick index for the implementer

| Need | Location |
|---|---|
| Pose write | `src/hooks.cpp` `dCalcViewModelView`; `client.dll` `0x29D930` |
| Hard-lock | `SetAbsOrigin` `0xAF720`, `SetAbsAngles` `0xAF600` |
| Offsets | `src/vr.cpp` `ResolveWeaponViewmodelPose` |
| Tracking | `src/vr.cpp` `UpdateControllerTracking` (add left) |
| Hide arms | `src/hooks.cpp` `dDrawModelExecute` (install) |
| Weapon name | `src/game.cpp` `GetActiveWeaponModelName` |
| FOV | `dGetViewModelFOV` `0x216510` |
| Melee | `VR::UpdateCrowbarMelee`, `dTraceRay` |
| BM bones/bodyparts | [`blackmesa-weapons.md`](blackmesa-weapons.md) |
| L4D2VR hand math | [`l4d2vr-hands.md`](l4d2vr-hands.md) §5 |
| L4D2VR weapon math | [`l4d2vr-weapons.md`](l4d2vr-weapons.md) §2 |
| L4D2VR melee | [`l4d2vr-melee.md`](l4d2vr-melee.md) §2 |
