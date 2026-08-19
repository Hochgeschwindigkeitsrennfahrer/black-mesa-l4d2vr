# VR hand architecture — Black Mesa weapon + BM animation + independent VR hands

**Status:** research handoff only. No implementation in this file.  
**Audience:** a future implementation agent.  
**Do not contradict** `research/l4d2vr-hands.md`, `research/l4d2vr-weapons.md`, `research/l4d2vr-melee.md`, `research/source-viewmodel.md`, `research/blackmesa-weapons.md` without citing a discrepancy.

Evidence tags: **Confirmed from source** / **Confirmed from asset inspection** / **Confirmed from Ghidra** / **Strong inference** / **Unknown**.

Companion: [`research/implementation-plan.md`](implementation-plan.md).

---

## Direct answer

**Yes — with a hybrid, not a blind L4D2VR native-IK copy.**

Black Mesa’s existing animated weapon models can stay: keep `C_BlackMesaViewModel` and the stock `v_*.mdl` sequences (fire, reload, pump, draw, sprint). Traditional monitor-oriented first-person arms can be replaced by independently posed L4D2VR-style VR hands. The **weapon stays the native viewmodel**, physically attached to the **aim controller** (right unless `LeftHanded`) via the existing `CalcViewModelView` uncouple — **not** parented to a GLB hand bone. The off-hand is a **separate** mesh at the off-hand controller, not a second bone of the gun MDL.

That is possible because BM already splits arms and gun as **two bodyparts with separate root bone trees** inside one MDL. Sequences still drive both trees; hiding the arm *mesh* leaves gun bones animating. **Confirmed from asset inspection** ([`research/blackmesa-weapons.md`](blackmesa-weapons.md)).

What this does **not** give you for free:

- Two-handed hold poses will not auto-follow the left controller (the old `L_Arm` clip still runs on hidden bones).
- Reloads still animate mag / slide / pump on **gun** bones; the vanished left FP hand no longer “holds” the mag.
- Stock MDLs have **no blank `arms` bodygroup**. `v_hands.mdl` is a different skeleton (`Bip001` vs `R_Arm`) and is unused. **Confirmed from asset inspection.**
- Native L4D2VR `NativeViewmodelHandsOnly` IK is the **wrong default** for BM (see §4).

---

## 1. What must remain unchanged

Do **not** redesign these. They are out of scope for hands/weapons and already the L4D2VR d3d9 path.

| Keep | Why | Evidence |
|---|---|---|
| Combined `d3d9.dll` (DXVK fork + `IDirect3DVR9` + OpenVR Vulkan submit) | Project architecture; load next to `bms.exe`, `bin\`, and `bin\thirdparty\dxvk-windows-x86\` | **Confirmed from source** (`docs/ARCHITECTURE.md`, `docs/L4D2VR-MAP.md`) |
| Stereo: two `RenderView`s into eye RTs when that path is live; same **world** gun pose both eyes | IPD on the gun drew two weapons (2026-08-17) | **Confirmed from source** (`src/hooks.cpp` `dCalcViewModelView`, `docs/RUNTIME.md`) |
| 6DoF HMD camera, LevelInit map-name gate (reject `background*`), no `ExitProcess` on `VR_Init` failure | AGENTS.md constraints | **Confirmed from source** |
| Controller tracking → Source units via existing `HmdMatrixToSourcePos(..., m_VRScale)` and `GetRecommendedViewmodelAbsPos` delta-from-HMD | Already the sd805/Portal 2 uncouple | **Confirmed from source** (`src/vr.cpp`) |
| `C_BlackMesaViewModel` entity, `SendWeaponAnim` / `m_nSequence`, muzzle attachments, particles | Weapon animation preservation | **Confirmed from Ghidra** + **Confirmed from source** ([`source-viewmodel.md`](source-viewmodel.md), [`blackmesa-weapons.md`](blackmesa-weapons.md)) |
| Per-weapon offset tables in `VR::ResolveWeaponViewmodelPose` | L4D2VR grip calibration already mapped to BM name substrings | **Confirmed from source** |
| Hard-lock after original `CalcViewModelView` via `SetAbsOrigin` / `SetAbsAngles` (`client.dll` `0xAF720` / `0xAF600`) | Bob/lag otherwise returns the gun to the camera | **Confirmed from Ghidra** + **Confirmed from source** |
| Steam launch options; `-oldgameui`; do not Y-flip 2D capture | Verified BM UI constraint | **Confirmed from source** (AGENTS.md) |

**Do not** introduce a second weapon entity, a `w_*` first-person gun, or a TF-style `SetWeaponModel` → hands MDL unless hybrid has failed in the HMD. **Confirmed from asset inspection:** `w_*` have no FP sequences.

---

## 2. What should be replaced

| Replace | With | Why |
|---|---|---|
| Visible FP arm meshes (`arms` bodypart: HEV / marine / sci / guard / zombie / G-Man) | Independent VR hands at each controller | Arms are authored for a camera-locked two-handed hold. Parenting the **whole** VM to the right controller is exactly “two arms holding a gun glued to the right controller.” **Confirmed from asset inspection** + **Confirmed from source** (BMVR today parents the combined VM). |
| L4D2VR shipping default `NativeViewmodelHandsOnly=true` (IK native arms, gloves off) | BM **hybrid** (hide arms, keep gun sequences, independent hands) | See §4. L4D2’s default assumes a **separate** `/arms/` studio model that can be IK’d while the gun VM stays. BM packs both in one `v_*`. |
| Optional later: canned `ACT_VM_HITCENTER` as the *only* melee motion | L4D2VR 1.1 m/s gate + 10-trace 50° fan driving **damage**, with swing-anim mitigation | See §10. Sequences may still play; stacking them on a controller-parented crowbar **double-swings**. **Strong inference** ([`blackmesa-weapons.md`](blackmesa-weapons.md) D, [`l4d2vr-melee.md`](l4d2vr-melee.md)). |

Nothing else in the stereo / compositor / input stack should be “replaced” to get hands.

---

## 3. What should be hooked

Exact functions from the research. Do not invent APIs.

### 3.1 Already hooked — keep, do not redesign

| Function | Where | Role for this architecture |
|---|---|---|
| `C_BlackMesaViewModel::CalcViewModelView` | `client.dll` RVA `0x29D930` (`FUN_1029d930`); `src/hooks.cpp` `dCalcViewModelView` | **The pose write.** Feed aim-controller recommended origin/angles, then `SetAbsOrigin`/`SetAbsAngles`. **Confirmed from Ghidra** + **Confirmed from source.** |
| `IClientMode::GetViewModelFOV` | `client.dll` `0x216510`; `dGetViewModelFOV` | Gameplay returns HMD `m_Fov` instead of `fov_viewmodel` (~50–75). Makes `FormatViewModelAttachment` scale factor ≈ 1 if that function exists. **Confirmed from Ghidra** + **Confirmed from source.** |
| `IEngineTrace::TraceRay` | `src/hooks.cpp` `dTraceRay` | While `m_PerformingMelee`, rewrite ray start to viewmodel/hand origin (crash-sticky `melee_trace`). **Confirmed from source.** |
| `CreateMove` | already | Controller `viewangles`, `IN_ATTACK` from melee window, `weaponselect`. **Confirmed from source.** |

### 3.2 Must hook (or re-install) for hybrid hands

| Function | Where | Role |
|---|---|---|
| `DrawModelExecute` | `engine.dll` `0xF6A20`; `src/hooks.cpp` `dDrawModelExecute` **compiled but not installed** | Hide arm materials (or later native IK). L4D2VR’s hide path is `MATERIAL_VAR_NO_DRAW` + still call original. **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §9, `src/hooks.cpp` comment 2026-08-18 load hang). **Unknown:** IModelRender 3-arg vs `IMatRenderContext` first-arg ABI on this BM build — **verify before createHook**. |
| Left-controller pose sampling | `VR::UpdateControllerTracking` today tracks **only** `AimControllerRole()` | Off-hand world matrix needs a physical left (or swapped) pose. **Confirmed from source** (`src/vr.cpp`). |

### 3.3 Optional / later hooks

| Function | Role | Evidence |
|---|---|---|
| `FormatViewModelAttachment` | Bypass FOV remap if attachments still disagree with world muzzle after HMD FOV | SDK 2013 **Confirmed from source** ([`source-viewmodel.md`](source-viewmodel.md) §5.4). **Unknown** whether BM’s `C_BlackMesaViewModel` still has it. If `GetViewModelFOV` already equals world FOV, \(f \approx 1\) and the hook may be unnecessary. |
| `C_BaseAnimating::SetBodygroup` / `FindBodygroupByName("arms")` | Asset-side blank index, or fight `m_nBody` at `+0x6C4` | **Confirmed from Ghidra** (field). Exact setter for `vm_bodygroup_overrides` **Unknown**. |
| Studio bone rewrite inside DME | Native-arm IK **only if hybrid fails** | L4D2VR `hooks_misc.inl` `dDrawModelExecute`. BM bone names are `R_Arm` / `L_Arm`, **not** `ValveBiped.Bip01_*`. **Confirmed from asset inspection.** |
| `ProcessUsercmds` / `ReadUsercmd` / `WriteUsercmd` | L4D2 encoded VR melee protocol | **L4D2-specific.** BM has no `TestMeleeSwingCollision`. Do not port the packing unless a BM server decoder exists. **Confirmed from source** ([`l4d2vr-melee.md`](l4d2vr-melee.md) §9). |

Do **not** hook `SendWeaponAnim` to stop fire/reload. Do **not** `SetModel` the VM to `w_*` or `v_hands.mdl`.

---

## 4. Why BM hybrid, not L4D2VR’s native-IK default

**Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §1, sample config):

- Named independent hands = GLB `VrHandSystem`.
- **Shipping default** = `VrHandsEnabled=true`, `VrHandsGlovesEnabled=false` → **`NativeViewmodelHandsOnly=true`**, `HideArms=false`.
- Gloves on → `HideArms=true` and native-only off. GLB init failure falls back to native IK.

That default works in L4D2 because:

1. Terror FP arms are **separate** `models/weapons/arms/` studio models. `HideArms` matches `"/arms/"` in the **model path**. The gun is a different model. **Confirmed from source.**
2. Native IK rewrites **arm bones only** toward the controller/palm; the weapon root is still controller-placed via `CalcViewModelView`. **Confirmed from source.**
3. L4D2 viewmodels use ValveBiped names (`Bip01_L_Hand`, …) that the IK/curl code expects. **Confirmed from source.**

Black Mesa is different (**Confirmed from asset inspection**, [`blackmesa-weapons.md`](blackmesa-weapons.md)):

1. One MDL per weapon (`v_crowbar.mdl`, …). Bodypart 0 = gun (`studio`/`body`). Bodypart 1 = named group **`arms`** (6–7 skins). **Not** two entities, **not** `/arms/` path.
2. Arm bones (`R_Arm` root) and weapon bones (`crowbar_new`, `bone_gun`, …) are **separate roots** (`parent = -1`). The gun is **not** parented to `R_Wrist`.
3. Two-handed guns ship **both** arm meshes in the same `arms` group. Sequences pose **two arms holding the gun**.
4. BMVR today parents the **entire** VM (arms+gun) to the aim controller. That is the visual bug: a monitor-authored two-handed hold, glued to one controller.
5. `v_hands.mdl` is `Bip001` Biped, unused, not an includemodel. Native IK cannot retarget onto it. Finger names are `R_Index1`, not `Bip01_R_Finger1`.

If BM copied `NativeViewmodelHandsOnly` first:

- IK would have to run on `R_Arm`/`L_Arm` **inside the same entity** whose origin is the right controller.
- The left arm’s rest/anim is “on the gun,” not “at the left controller.” Full-arm IK *can* pull `L_Arm` toward the left controller in DME (**Strong inference**, L4D2VR does this on a *separate* arms model), but you still fight every two-handed sequence that plants the left palm on the weapon, and you inherit ValveBiped-specific finger curl code that **does not match BM names**.
- You would still see HEV sleeves reaching from a camera-space shoulder rest (`R_Arm` rest ≈ `(-7.92, 13.11, -10.36)` in VM space) unless crop/IK is tuned per weapon.

**Hybrid is the smallest BM-correct L4D2VR idea:** same as L4D2VR’s *glove* mode (`HideArms` + independent meshes), not its *sample default*. Keep L4D2VR’s **weapon** side (controller → `CalcViewModelView` → sequences). Replace only the **arm representation**.

Native IK remains the **fallback** if independent hands fail (no GLB, DME material skip insufficient, or users want HEV gloves that wrap the native gun). It is not step 1.

---

## 5. Asset-level vs runtime-level

| Concern | Runtime (preferred first) | Asset (optional, cleaner hide) |
|---|---|---|
| Hide arms | During viewmodel `DrawModelExecute`, `MATERIAL_VAR_NO_DRAW` on arm materials (`$viewmodelhands`, `v_hand*`, and per-skin `marine_d` / `bare_hands_d` / …), then restore. Still call original so attachments/events survive. **Confirmed from source** (L4D2VR hide pattern) + **Confirmed from Ghidra** (`$viewmodelhands` is a gore-FX flag, **not** a hide switch). | Decompile QC, add `blank` to `$bodygroup arms`, recompile. Engine already selects `arms` 0–6 via `scripts/playermodel_manifest.txt` `vm_bodygroup_overrides`. **Confirmed from asset inspection.** |
| Keep gun mesh + sequences | Do nothing to `m_nSequence` / `SendWeaponAnim`. | Do **not** drop weapon SMDs. Do **not** need two MDLs. |
| Independent hands | GLB (`VrHandSystem` mechanism) or a simple controller/debug mesh at each controller world matrix. Not a Source entity. **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §2). | SteamVR `vr_glove_*.glb` already on disk; ozz is hierarchy builder only. |
| Weapon attach | Controller → recommended pose → VM origin. Offset tables. | `$origin` is **not** in MDL keyvalues; camera placement is baked into root rest poses (crowbar `crowbar_new` ≈ `(-13.3, -4.75, 14.1)`). **Confirmed from asset inspection.** Retune offsets; do not wait on a recompile. |
| Off-hand | Left controller pose → independent mesh. | Do not bone-merge `v_hands.mdl` onto `v_crowbar` (skeleton mismatch). |
| Melee | Client 1.1 / 50° / 10-trace fan + `IN_ATTACK` (already sketched in `UpdateCrowbarMelee`). | N/A |
| Blank arms in stock VPK | **Impossible** with `m_nBody` alone. **Confirmed from asset inspection.** | Required for a networked bodygroup hide. |

`m_nBody` is **8-bit networked** on `DT_BaseViewModel` (`+0x6C4`). A client-only `SetBodygroup` can fight updates; SP listen server is the same process. **Confirmed from Ghidra.** Prefer material skip until a blank exists.

---

## 6. Transform hierarchy (equations)

Spaces:

- OpenVR tracking: meters.
- Source world: Hammer units. **BMVR `m_VRScale` default is `39.37`**, not L4D2VR’s `43.2`. **Confirmed from source** (`src/vr.h`). Use BM’s scale in every BM equation. L4D2VR `43.2` is cited only to copy *mechanism*.
- Source eye height: L4D2VR subtracts **64** from `CameraAnchor`. BMVR uses **body/setup origin + (controller − HMD)** with an **80 hu** reach clamp. **Confirmed from source** (`src/vr.cpp` `GetRightControllerAbsPos` / `GetRecommendedViewmodelAbsPos`). Do not silently switch BM to `CameraAnchor-64` as part of hands work.

L4D2VR weapon-aim extra pitch default **−45°** (`m_WeaponAimPitchOffsetDeg`). BMVR `ControllerPitchTilt` default **−35°**. **Confirmed from source.** Treat −45° as the L4D2VR calibration to *match against*, not a constant to paste.

### 6.1 Tracking → aim-controller (keep)

**Confirmed from source** (`VR::UpdateControllerTracking`).

\[
\mathbf{p}_{\mathrm{ctrl}} = \mathrm{HmdMatrixToSourcePos}(M_{\mathrm{right}},\, s),\quad s = m\_VRScale
\]

\[
\mathbf{p}_{\mathrm{hmd}} = \mathrm{HmdMatrixToSourcePos}(M_{\mathrm{hmd}},\, s)
\]

\[
\boldsymbol{\delta} = \mathrm{PivotYaw}(\mathbf{p}_{\mathrm{ctrl}} - \mathbf{p}_{\mathrm{hmd}},\, \psi_{\mathrm{recenter}})
\]

If \(|\boldsymbol{\delta}| > 80\), the gun is **head-locked** (existing fallback). Else:

\[
\mathbf{p}_{0} = \mathrm{GetViewOrigin}(\mathbf{p}_{\mathrm{body}}) + \boldsymbol{\delta}
\]

Let \((\mathbf{f},\mathbf{r},\mathbf{u}) = \mathrm{AngleVectors}(\boldsymbol{\theta}_{\mathrm{ctrl}})\). Weapon pitch about local right:

\[
\mathbf{f}' = \mathrm{Rodrigues}(\mathbf{f},\, \mathbf{r},\, \tau),\quad
\mathbf{u}' = \mathrm{Rodrigues}(\mathbf{u},\, \mathbf{r},\, \tau)
\]

\(\tau =\) `ControllerPitchTilt` (BM default −35°). L4D2VR equivalent \(\tau = -45°\).

### 6.2 Per-weapon basis then origin (keep, retune)

**Confirmed from source** ([`l4d2vr-weapons.md`](l4d2vr-weapons.md) §2.2–2.3, `VR::ApplyViewmodelBasisOffsets` / `ResolveWeaponViewmodelPose`).

Let \(\mathbf{A} = (A_p, A_y, A_r)\) and \(\mathbf{o} = (o_x, o_y, o_z)\) from the table + live config.

\[
\begin{aligned}
\mathbf{f} &\leftarrow \mathrm{Rodrigues}(\mathbf{f}', \mathbf{u}', A_y),\quad
\mathbf{r} \leftarrow \mathrm{Rodrigues}(\mathbf{r}, \mathbf{u}', A_y) \\
\mathbf{f} &\leftarrow \mathrm{Rodrigues}(\mathbf{f}, \mathbf{r}, A_p),\quad
\mathbf{u} \leftarrow \mathrm{Rodrigues}(\mathbf{u}', \mathbf{r}, A_p) \\
\mathbf{r} &\leftarrow \mathrm{Rodrigues}(\mathbf{r}, \mathbf{f}, A_r),\quad
\mathbf{u} \leftarrow \mathrm{Rodrigues}(\mathbf{u}, \mathbf{f}, A_r)
\end{aligned}
\]

\[
\mathbf{p}_{\mathrm{vm}} = \mathbf{p}_{0} - o_x\,\mathbf{f} - o_y\,\mathbf{r} - o_z\,\mathbf{u}
\]

**Sign:** positive \(o_x\) pulls the model **backward** along gun forward (place the grip on the controller).

\[
\boldsymbol{\theta}_{\mathrm{vm}} = \mathrm{VectorAngles}(\mathbf{f}, \mathbf{u})
\]

`dCalcViewModelView`: `original(vm, owner, p_vm, θ_vm)` then `SetAbsOrigin(p_vm)`, `SetAbsAngles(θ_vm)`.

Studio then runs sequences on `C_BlackMesaViewModel`. Gun roots (`crowbar_new`, `bone_gun`, …) and arm roots (`R_Arm`, `L_Arm`) both concatenate from **this** entity origin. Hiding arm meshes does not change that. **Confirmed from asset inspection** + **Strong inference** (studiomdl).

### 6.3 Optional two-hand aim (later — do not do first)

**Confirmed from source** ([`l4d2vr-weapons.md`](l4d2vr-weapons.md) §2.1). Replaces **weapon** \(\mathbf{f},\mathbf{r},\mathbf{u}\), not the left-hand mesh.

\[
\mathbf{f}_{\mathrm{two}} = \mathrm{normalize}(\mathbf{p}_{\mathrm{left}} - \mathbf{p}_{\mathrm{right}})
\]

\[
\mathbf{f}_{\mathrm{aim}} = \mathrm{lerp}(\mathbf{f}',\, \mathbf{f}_{\mathrm{two}},\, \alpha \cdot w_{\mathrm{dist}})
\]

Default \(\alpha = 1\); \(w_{\mathrm{dist}} = 1\) for pistols else \(\mathrm{clamp}(|\Delta| / (0.12\,s), 0, 1)\). Virtual stock and special-infected aim are **L4D2-specific** — skip unless re-verified.

Left GLB/controller mesh **still** uses the left controller matrix unless a later “hand glued to gun” mode is added (`VrHandsRightUseViewmodelPose` is the opposite parenting). **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §8).

### 6.4 Independent VR hand world matrix (new)

**Confirmed from source** (`VrHandMath::BuildControllerWorld`, [`l4d2vr-hands.md`](l4d2vr-hands.md) §5.2).

Let \((\mathbf{F},\mathbf{R},\mathbf{U})\) be `AngleVectors` of that hand’s controller QAngle, origin \(=\mathbf{p}_{\mathrm{hand}}\) in Source units.

\[
B = \begin{bmatrix} \mathbf{R} & \mathbf{U} & -\mathbf{F} & \mathbf{p}_{\mathrm{hand}} \\ 0 & 0 & 0 & 1 \end{bmatrix}
\]

Local correction \(C = R_z R_y R_x\) with translation \(\mathbf{t}_{\mathrm{m}} \cdot s\) (**meters → Source before model scale** so size tweaks do not walk the calibration).

\[
S = \mathrm{diag}(\sigma s,\, \sigma s,\, \sigma s,\, 1),\quad
\sigma = \mathrm{clamp}(m\_VrHandsModelScale,\, 0.25,\, 4)
\]

\[
W = B\, C\, S
\]

Apply once for **physical left** and once for **physical right**. Gameplay swap if `LeftHanded` (`physicalHandIndexForGameplay = leftHanded ? 1-i : i`). **Confirmed from source.**

This matrix is **not** in the weapon chain unless someone later sets VM-pose gloves (`W` parented to `Bip01_R_Hand`). BM has `R_Wrist`, not `Bip01_R_Hand`. **Do not enable VM-pose parenting in v1** — that is “hand glued to gun,” which recreates the two-hands-on-one-controller look for the gripping hand.

### 6.5 SDK attachment FOV remap (usually skip on BM)

**Confirmed from source** ([`source-viewmodel.md`](source-viewmodel.md) §5.4):

\[
f = \frac{\tan(\mathrm{fov}/2)}{\tan(\mathrm{fov}_{\mathrm{vm}}/2)}
\]

View-space XY of the attachment is multiplied by \(f\). BMVR already sets \(\mathrm{fov}_{\mathrm{vm}} = \mathrm{fov}_{\mathrm{HMD}}\) in gameplay, so \(f \approx 1\). **Strong inference:** do not hook `FormatViewModelAttachment` unless HMD verification shows muzzle/tracers still in camera-FOV space.

### 6.6 End-to-end v1 (hybrid)

```text
OpenVR right pose
  → p_ctrl, τ pitch, per-weapon F,R,U and o
  → C_BlackMesaViewModel abs origin/angles          // gun + hidden arm bones
  → studio sequences (fire/reload/pump on gun roots)
  → DrawModelExecute: NO_DRAW arm materials; draw gun
  → GPU (HMD FOV both world and VM)

OpenVR left pose  ──► W_left  = B C S   // independent off-hand mesh
OpenVR right pose ──► W_right = B C S   // independent right hand mesh
                      (not parented to crowbar_new / bone_gun)
```

The **weapon attaches to the controller** (native VM). Hands attach to **controllers**. Nothing attaches to a GLB bone in v1.

---

## 7. How the weapon should attach

**Controller → native viewmodel. Not GLB wrist. Not `v_hands` merge.**

L4D2VR does the same: “The gun is **not** parented to a GLB bone.” Hierarchy is controller → recommended pose → `CBaseViewModel` → sequences. **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §8, [`l4d2vr-weapons.md`](l4d2vr-weapons.md) §1).

BM specifics:

- No grip `$attachment`. Muzzle attachments sit on **gun** bones (often identity in bone space). **Confirmed from asset inspection.**
- Pivot is the VM entity origin plus baked root rest (e.g. `crowbar_new`). Offset tables exist to put that origin so the **grip** meets the controller. Retune empirically in the HMD; L4D2 numbers are a starting point, not 1:1 (**Unknown** until visual pass — [`l4d2vr-weapons.md`](l4d2vr-weapons.md) §8).
- AutoGrip (rigid delta so a named palm/grip bone sits on the controller) is L4D2VR DME work. BM has no named grip bone on crowbar. **Skip AutoGrip in v1.** **Strong inference.**
- `m_flModelScale` at VM `+0x7C0` scales about the **entity origin**, not the grip. L4D2VR does not use it. Prefer offsets. **Confirmed from Ghidra** + **Confirmed from source.**

---

## 8. How the off-hand should work

**v1:** independent controller pose → independent mesh. Not parented to the gun.

- Sample **physical** left controller every frame (today BMVR does not). Swap with right when `LeftHanded`.
- Draw GLB glove or a simple controller/box mesh with \(W\) from §6.4.
- Do **not** drive that mesh from `L_Arm` / `L_Wrist` sequences. No finger retarget. OpenVR skeletal curls are optional polish (`GetSkeletalSummaryData` → local-Z flexion). **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §4.1).
- Two-handed **aim blend** is a **later** flag (`m_VrHandsTwoHandedGripActive` in L4D2VR is **not** automatic for rifles). **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §7).
- `MAX_VIEWMODELS == 2` exists on BM (`m_hViewModel[0]` at `+0x13F0`, count 2). **Confirmed from Ghidra.** Slot 1 is still a Source viewmodel (camera pipeline). **Do not** use slot 1 as the off-hand in v1 — that repeats camera-locked VM architecture ([`source-viewmodel.md`](source-viewmodel.md) §6).

Empty hands: L4D2VR uses a dummy pistol placeholder so shove still exists. BM crowbar/use is different. **Unknown** whether BM needs an empty-hands VM; verify after hybrid gun+hands look correct.

---

## 9. How animation coexists with VR transforms

**Root transform is VR; local animation is still Source.** Same sentence as L4D2VR. **Confirmed from source** ([`l4d2vr-hands.md`](l4d2vr-hands.md) §10).

| Layer | Behavior |
|---|---|
| `SendWeaponAnim` / `m_nSequence` | Unchanged. Fire, reload, pump, draw, sprint still select clips on `C_BlackMesaViewModel`. |
| Arm bones | Keep sampling (harmless if mesh hidden). **Do not** retarget onto GLB fingers. |
| Gun bones | Keep sampling (`bone_clip`, `bone_pump`, `crowbar_new`, …). Reloads still move the mag. |
| Independent hands | Parallel D3D9 (or debug) mesh. Not the thing that plays `ACT_VM_PRIMARYATTACK`. |
| Bob / lag | Original `CalcViewModelView` still runs (`cl_viewmodel_lag`, weapon bob vfunc). Hard-lock overwrites abs pose. Optional: zero owner velocity around the call like L4D2VR `ViewmodelDisableMoveBob`. **Confirmed from Ghidra** + **Confirmed from source.** |

Do not export a weapon-only MDL unless material/bodygroup hide fails. **Strong inference** ([`blackmesa-weapons.md`](blackmesa-weapons.md) D).

---

## 10. Crowbar melee

Copy **L4D2VR’s real algorithm**, not motion-gesture shove.

Constants (**Confirmed from source**, [`l4d2vr-melee.md`](l4d2vr-melee.md) §2, §8):

```text
TRACKING_VEL_GATE   = 1.1     // |OpenVR TrackedDeviceVel| m/s, NOT Source units
BLADE_PITCH_DEG     = 50      // Rodrigues about controller right
NUM_TRACES          = 10
MIN_SWING_ANGLE_DEG = 0.01
```

Direction: previous vs current **controller** orientation, 50° blade tilt, Rodrigues fan about \(\mathbf{d}_0 \times \mathbf{d}_1\). Hit geometry stays **vanilla** (L4D2: `TestMeleeSwingCollision`; BM: crowbar DMX range **56 hu**, damage 10 / MP 20). **Confirmed from asset inspection.**

**Discrepancy (cite both):**

- [`l4d2vr-melee.md`](l4d2vr-melee.md) §10: BMVR melee is “a client TraceRay origin experiment, not a copy of the 1.1 / 50° / 10-trace **server** algorithm.”
- [`blackmesa-weapons.md`](blackmesa-weapons.md) §A and current `VR::UpdateCrowbarMelee`: BMVR **does** run a 1.1 / 50° / 10-trace **client** hull fan (±16 cube, range **70** hu) from the viewmodel origin, then haptic + `IN_ATTACK` for 120 ms, plus `dTraceRay` origin rewrite.

Both are true: the **geometry constants** are copied; the **damage path** is not L4D2 `TestMeleeSwingCollision` / encoded `CUserCmd`. Implementation should keep the client fan + synthesized attack (BM has no melee WeaponID 19 protocol), and **verify** range 56 vs 70 in the HMD.

**ACT_VM_HITCENTER double-swing:** crowbar hit sequences rotate `crowbar_new` **and** the VM root already follows the controller. Motion melee + canned swing = the mesh swings twice. **Strong inference.** Mitigation (pick one, verify):

- Do not send `ACT_VM_HITCENTER` while `speed > 1.1` (leave `ACT_VM_IDLE` / draw).
- Freeze cycle on the weapon root during the fan (L4D2VR magazine freeze is the nearest prior art — **do not** copy mag logic blindly).
- Accept the double-swing only as a known bug if idle-lock looks worse.

Do **not** copy `UpdateMotionGestures` left-outward → `+attack2` as melee (that is L4D2 shove). **Confirmed from source.**

Do **not** copy AutoFastMelee `slot1`/`slot2` (L4D2 recovery cancel; BM weapon cycle is `CUserCmd::weaponselect`, and `ClientCmd` from CreateMove has crashed BM). **Confirmed from source.**

---

## 11. Scale / pivot / FOV diagnosis

Symptoms if the gun looks huge, floats, or pivots from the muzzle:

| Cause | Mechanism | What to change | Evidence |
|---|---|---|---|
| Viewmodel FOV | Stock `fov_viewmodel` 75 default, shotgun 50, hivehand 65. BMVR replaces with HMD FOV. Narrow VM FOV makes close meshes fill the screen. | Keep HMD FOV (stereo). Do not restore 54/75 to “fix” size. | **Confirmed from Ghidra** + **Confirmed from source** |
| Baked camera placement | `eyeposition` (0,0,0); `view_bb` zeros; roots offset (crowbar `crowbar_new` ≈ −13.3, −4.75, 14.1) | Offset table \(o_x,o_y,o_z\), not `$origin` (absent from MDL KV) | **Confirmed from asset inspection** |
| No grip attachment | Grip is not a socket | Empirical offsets; optional later AutoGrip if a bone is named | **Confirmed from asset inspection** |
| `m_flModelScale` | Scale about VM origin, not wrist | Last resort; L4D2VR unused | **Confirmed from source** |
| Camera-space assumption | SDK `CalcViewModelView` is eyes + bob + lag | Already replaced by controller input + hard-lock | **Confirmed from source** ([`source-viewmodel.md`](source-viewmodel.md) §5) |
| `FormatViewModelAttachment` | XY squash if fov ≠ fov_vm | Skip unless tracers disagree | **Confirmed from source** (SDK); **Unknown** on BM |
| Reach clamp 80 hu | Delta controller−HMD | If the gun snaps to the face, this fired | **Confirmed from source** |
| Arms still visible | Combined MDL | Hide first — giant HEV arms dominate scale judgment | **Confirmed from asset inspection** |

Tune **after** arms are hidden. Offset tables copied from L4D2 (`crowbar` 19.5, 6, −13.5 / −24.5, −6.5, −6) are already in `ResolveWeaponViewmodelPose`. Retune per BM mesh in the HMD.

---

## 12. Default vs optional paths

| Path | When | Notes |
|---|---|---|
| **Default (BM hybrid)** | v1 | Hide `arms` (material, later blank bodygroup). Keep gun on controller-parented `C_BlackMesaViewModel`. Independent hands on both controllers (GLB or debug mesh). No finger retarget. No two-hand aim. |
| **L4D2VR sample default** | Do not use as BM v1 | `NativeViewmodelHandsOnly` on a combined arms+gun MDL. |
| **GLB gloves** | Preferred independent-hand renderer once DME/init is safe | Port *mechanism* from `VrHandSystem`, not L4D2 bone names. Lazy construct. **Unknown:** DXVK queued insertion vs L4D2VR call-queue on BM. |
| **Simple controller mesh** | If GLB/ozz is too large for the first visual proof | Boxes at \(W\) still prove independence. |
| **Native-arm IK** | Only if hybrid fails visually | New BM bone-name table (`R_Arm`, `R_Wrist`, `L_Arm`, …). Crop huge melee sleeves. |
| **VM-pose gloves** | Later, two-hand wrap | Hand parented to gun palm — opposite of “weapon on controller, hands independent.” |
| **Two-hand aim blend** | Later | `ResolvePavlovTwoHandedAimBasis`. |
| **Asset blank `arms`** | Parallel, not a blocker | Makes `m_nBody` hide MP-safe. |
| **Weapon-only recompile** | Last resort | Keep sequences; drop arm `$model`. QC not in VPK. |

---

## 13. Honest limitations

- Two-handed poses will not auto-follow the off-hand controller.
- Reloads still animate mag/slide/pump; the left FP hand that staged them is gone.
- No blank `arms` choice in stock MDLs; `v_hands.mdl` is the wrong skeleton and unused.
- Hitscan is still eye-sourced in SDK (`Weapon_ShootPosition`); BMVR already aims `CreateMove` viewangles from the controller. Tracer vs bullet mismatch may remain. **Confirmed from source** ([`source-viewmodel.md`](source-viewmodel.md) §5.6) + **Unknown** exact BM `PrimaryAttack` origin (server.dll not walked).
- `DrawModelExecute` install hung load-to-menu once (2026-08-18). Hands hide **depends** on a verified DME ABI. Crash-sticky recommended.
- Independent GLB draw into DXVK eye RTs is **not ported**. `docs/L4D2VR-MAP.md`: ozz hands not ported. **Unknown** queued vs immediate on BM.
- World-model pose relay / first-person body are L4D2-specific; out of scope.

---

## 14. Cross-links

| Doc | Use |
|---|---|
| [`research/l4d2vr-hands.md`](l4d2vr-hands.md) | GLB vs native IK, `BuildControllerWorld`, HideArms, why default ≠ gloves |
| [`research/l4d2vr-weapons.md`](l4d2vr-weapons.md) | Offset equations, `CalcViewModelView` hard-lock, no second gun entity |
| [`research/l4d2vr-melee.md`](l4d2vr-melee.md) | 1.1 / 50° / 10-trace; what is *not* melee |
| [`research/source-viewmodel.md`](source-viewmodel.md) | `CalcViewModelView` as pose write, bodygroups, bonemerge, `FormatViewModelAttachment` |
| [`research/blackmesa-weapons.md`](blackmesa-weapons.md) | BM bodyparts, bones, DMX, Ghidra RVAs, hybrid conclusion |
| [`research/implementation-plan.md`](implementation-plan.md) | Ordered steps, done criteria, test ladder |

BMVR snapshot cited: `src/hooks.cpp` (`dCalcViewModelView`, unhooked `dDrawModelExecute`), `src/vr.cpp` (`GetRecommendedViewmodelAbsPos`, `ResolveWeaponViewmodelPose`, `UpdateCrowbarMelee`, right-only `UpdateControllerTracking`), `src/game.cpp` `GetActiveWeaponModelName` (reads **weapon entity** `m_nModelIndex` = `w_*`, crowbar still matches), `src/offsets.h`, `src/vr.h` `m_VRScale = 39.37f`.
